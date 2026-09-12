/* C 版 YOLOv5 后处理接口。 */
#include "yolo_postprocess_c.h"

/* expf/logf/fmaxf/fminf。 */
#include <math.h>
/* printf/fprintf/file。 */
#include <stdio.h>
/* qsort。 */
#include <stdlib.h>
/* memset/strcspn。 */
#include <string.h>

/* 候选框最大数量，避免动态内存分配。 */
#define CANDIDATE_MAX 4096

/* labels 表，最多 80 个类别。 */
static char g_labels[YOLO_CLASS_NUM][YOLO_OBJ_NAME_MAX];
/* labels 是否已经加载。 */
static int g_labels_loaded = 0;

/* 拷贝类别名，保证以 '\0' 结尾，超长名称会被安全截断。 */
static void copy_obj_name(char dst[YOLO_OBJ_NAME_MAX], const char *src)
{
    size_t i = 0;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (i + 1 < YOLO_OBJ_NAME_MAX && src[i] != '\0') {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

/* stride 8 输出层对应的 anchor。 */
static const int g_anchor0[6] = {10, 13, 16, 30, 33, 23};
/* stride 16 输出层对应的 anchor。 */
static const int g_anchor1[6] = {30, 61, 62, 45, 59, 119};
/* stride 32 输出层对应的 anchor。 */
static const int g_anchor2[6] = {116, 90, 156, 198, 373, 326};

/* 后处理内部候选框。 */
typedef struct {
    /* 框左上角 x，模型输入坐标系。 */
    float x;
    /* 框左上角 y，模型输入坐标系。 */
    float y;
    /* 框宽度，模型输入坐标系。 */
    float w;
    /* 框高度，模型输入坐标系。 */
    float h;
    /* 置信度。 */
    float score;
    /* 类别 id。 */
    int class_id;
    /* NMS 后是否被抑制。 */
    int suppressed;
} candidate_t;

/* 候选框静态数组，避免 malloc/free 干扰实时性。 */
static candidate_t g_candidates[CANDIDATE_MAX];
/* 当前候选框数量。 */
static int g_candidate_count = 0;

/* sigmoid 激活函数。 */
static float sigmoid_f(float x)
{
    return 1.0f / (1.0f + expf(-x));
}

/* sigmoid 的反函数，用于把阈值转换到量化前空间。 */
static float unsigmoid_f(float y)
{
    return -1.0f * logf((1.0f / y) - 1.0f);
}

/* INT8 反量化：真实值 = (q - zp) * scale。 */
static float deqnt_i8_to_f32(int8_t q, int32_t zp, float scale)
{
    return ((float)q - (float)zp) * scale;
}

/* float 量化到 int8，用于快速过滤阈值。 */
static int8_t qnt_f32_to_i8(float f, int32_t zp, float scale)
{
    /* 量化公式：q = f / scale + zp。 */
    int v = (int)(f / scale + zp);
    /* int8 上限。 */
    if (v > 127) {
        v = 127;
    }
    /* int8 下限。 */
    if (v < -128) {
        v = -128;
    }
    /* 返回 int8。 */
    return (int8_t)v;
}

/* 把整数限制在 [lo, hi] 范围内。 */
static int clamp_i(int v, int lo, int hi)
{
    /* 小于下限则取下限。 */
    if (v < lo) {
        return lo;
    }
    /* 大于上限则取上限。 */
    if (v > hi) {
        return hi;
    }
    /* 否则返回原值。 */
    return v;
}

/* 加载 labels 文件。 */
int yolo_postprocess_init(const char *label_path)
{
    /* 打开 labels 文件。 */
    FILE *fp = fopen(label_path, "r");
    /* 打开失败则返回。 */
    if (!fp) {
        fprintf(stderr, "open label file failed: %s\n", label_path);
        return -1;
    }

    /* 清空 labels 表。 */
    memset(g_labels, 0, sizeof(g_labels));
    /* 单行临时缓冲。 */
    char line[256];
    /* 当前类别索引。 */
    int idx = 0;
    /* 逐行读取，最多读 80 行。 */
    while (idx < YOLO_CLASS_NUM && fgets(line, sizeof(line), fp)) {
        /* 去掉行尾 \r 或 \n。 */
        line[strcspn(line, "\r\n")] = '\0';
        /* 拷贝类别名称，超长标签会被安全截断。 */
        copy_obj_name(g_labels[idx], line);
        /* 下一个类别。 */
        idx++;
    }
    /* 关闭文件。 */
    fclose(fp);

    /* 标记 labels 已加载。 */
    g_labels_loaded = 1;
    /* 打印加载数量。 */
    printf("loaded %d labels from %s\n", idx, label_path);
    /* 成功。 */
    return 0;
}

/* 复位后处理状态。 */
void yolo_postprocess_deinit(void)
{
    /* 当前 labels 是静态数组，无需 free，只需要标记无效。 */
    g_labels_loaded = 0;
}

/* 添加一个候选框。 */
static int add_candidate(float x, float y, float w, float h, float score, int class_id)
{
    /* 超出最大候选数量则丢弃。 */
    if (g_candidate_count >= CANDIDATE_MAX) {
        return -1;
    }

    /* 取当前空槽位。 */
    candidate_t *c = &g_candidates[g_candidate_count++];
    /* 保存左上角 x。 */
    c->x = x;
    /* 保存左上角 y。 */
    c->y = y;
    /* 保存宽度。 */
    c->w = w;
    /* 保存高度。 */
    c->h = h;
    /* 保存分数。 */
    c->score = score;
    /* 保存类别。 */
    c->class_id = class_id;
    /* 初始不抑制。 */
    c->suppressed = 0;
    /* 成功。 */
    return 0;
}

/* 解析 YOLOv5 的一路输出。 */
static void parse_output(int8_t *input,
                         const int *anchor,
                         int grid_h,
                         int grid_w,
                         int stride,
                         int class_num,
                         float threshold,
                         int32_t zp,
                         float scale)
{
    /* YOLO 每个预测点包含：x,y,w,h,obj_conf + class_num 类分数。 */
    int prop_box_size = 5 + class_num;
    /* 当前输出层网格总数。 */
    int grid_len = grid_h * grid_w;
    /* 把置信度阈值转换成 int8 阈值，先过滤 obj_conf。 */
    int8_t threshold_i8 = qnt_f32_to_i8(unsigmoid_f(threshold), zp, scale);

    /* 每个输出层有 3 个 anchor。 */
    for (int a = 0; a < 3; ++a) {
        /* 遍历网格行。 */
        for (int i = 0; i < grid_h; ++i) {
            /* 遍历网格列。 */
            for (int j = 0; j < grid_w; ++j) {
                /* 当前 anchor 在当前 grid 上的起始偏移。 */
                int offset = (prop_box_size * a) * grid_len + i * grid_w + j;
                /* 指向当前位置预测数据。 */
                int8_t *p = input + offset;
                /* obj_conf 在第 4 个通道。 */
                int8_t obj = p[4 * grid_len];
                /* obj_conf 低于阈值则跳过。 */
                if (obj < threshold_i8) {
                    continue;
                }

                /* 解码中心点 x。 */
                float box_x = sigmoid_f(deqnt_i8_to_f32(p[0 * grid_len], zp, scale)) * 2.0f - 0.5f;
                /* 解码中心点 y。 */
                float box_y = sigmoid_f(deqnt_i8_to_f32(p[1 * grid_len], zp, scale)) * 2.0f - 0.5f;
                /* 解码宽度系数。 */
                float box_w = sigmoid_f(deqnt_i8_to_f32(p[2 * grid_len], zp, scale)) * 2.0f;
                /* 解码高度系数。 */
                float box_h = sigmoid_f(deqnt_i8_to_f32(p[3 * grid_len], zp, scale)) * 2.0f;

                /* 中心点 x 还原到模型输入坐标。 */
                box_x = (box_x + (float)j) * (float)stride;
                /* 中心点 y 还原到模型输入坐标。 */
                box_y = (box_y + (float)i) * (float)stride;
                /* 宽度还原到模型输入坐标。 */
                box_w = box_w * box_w * (float)anchor[a * 2];
                /* 高度还原到模型输入坐标。 */
                box_h = box_h * box_h * (float)anchor[a * 2 + 1];
                /* 从中心点转换成左上角 x。 */
                box_x -= box_w / 2.0f;
                /* 从中心点转换成左上角 y。 */
                box_y -= box_h / 2.0f;

                /* 当前最佳类别 id。 */
                int best_id = 0;
                /* 当前最佳类别量化分数。 */
                int8_t best_q = p[5 * grid_len];
                /* 遍历类别，找最大类别分数。 */
                for (int k = 1; k < class_num; ++k) {
                    /* 第 k 类的量化分数。 */
                    int8_t q = p[(5 + k) * grid_len];
                    /* 更新最大分数。 */
                    if (q > best_q) {
                        best_q = q;
                        best_id = k;
                    }
                }

                /* 把最佳类别分数反量化并过 sigmoid。 */
                float score = sigmoid_f(deqnt_i8_to_f32(best_q, zp, scale));
                /* 类别分数也超过阈值才加入候选框。 */
                if (score >= threshold) {
                    add_candidate(box_x, box_y, box_w, box_h, score, best_id);
                }
            }
        }
    }
}

/* 计算两个候选框的 IoU。 */
static float overlap(candidate_t *a, candidate_t *b)
{
    /* a 框左上角 x。 */
    float ax1 = a->x;
    /* a 框左上角 y。 */
    float ay1 = a->y;
    /* a 框右下角 x。 */
    float ax2 = a->x + a->w;
    /* a 框右下角 y。 */
    float ay2 = a->y + a->h;
    /* b 框左上角 x。 */
    float bx1 = b->x;
    /* b 框左上角 y。 */
    float by1 = b->y;
    /* b 框右下角 x。 */
    float bx2 = b->x + b->w;
    /* b 框右下角 y。 */
    float by2 = b->y + b->h;

    /* 交集左上角 x。 */
    float x1 = fmaxf(ax1, bx1);
    /* 交集左上角 y。 */
    float y1 = fmaxf(ay1, by1);
    /* 交集右下角 x。 */
    float x2 = fminf(ax2, bx2);
    /* 交集右下角 y。 */
    float y2 = fminf(ay2, by2);
    /* 交集宽度。 */
    float w = fmaxf(0.0f, x2 - x1 + 1.0f);
    /* 交集高度。 */
    float h = fmaxf(0.0f, y2 - y1 + 1.0f);
    /* 交集面积。 */
    float inter = w * h;
    /* a 框面积。 */
    float area_a = (ax2 - ax1 + 1.0f) * (ay2 - ay1 + 1.0f);
    /* b 框面积。 */
    float area_b = (bx2 - bx1 + 1.0f) * (by2 - by1 + 1.0f);
    /* 并集面积。 */
    float uni = area_a + area_b - inter;
    /* 返回 IoU；并集无效时返回 0。 */
    return uni <= 0.0f ? 0.0f : inter / uni;
}

/* qsort 比较函数：按 score 从高到低排序。 */
static int cmp_candidate_desc(const void *pa, const void *pb)
{
    /* 转成候选框指针。 */
    const candidate_t *a = (const candidate_t *)pa;
    /* 转成候选框指针。 */
    const candidate_t *b = (const candidate_t *)pb;
    /* a 分数小，排序时放后面。 */
    if (a->score < b->score) {
        return 1;
    }
    /* a 分数大，排序时放前面。 */
    if (a->score > b->score) {
        return -1;
    }
    /* 分数相同。 */
    return 0;
}

/* 对候选框做 NMS，并转换到最终输出结构。 */
static int finish_candidates(int model_h,
                             int model_w,
                             float scale_w,
                             float scale_h,
                             float nms_threshold,
                             yolo_result_group_t *group)
{
    /* 没有候选框就直接返回 0 个结果。 */
    if (g_candidate_count <= 0) {
        return 0;
    }

    /* 按置信度从高到低排序。 */
    qsort(g_candidates, g_candidate_count, sizeof(g_candidates[0]), cmp_candidate_desc);

    /* NMS：同类别高重叠框只保留分数最高的。 */
    for (int i = 0; i < g_candidate_count; ++i) {
        /* 已被抑制的候选框跳过。 */
        if (g_candidates[i].suppressed) {
            continue;
        }
        /* 用当前高分框去压制后面的低分框。 */
        for (int j = i + 1; j < g_candidate_count; ++j) {
            /* 已被抑制则跳过。 */
            if (g_candidates[j].suppressed) {
                continue;
            }
            /* 不同类别之间不互相抑制。 */
            if (g_candidates[i].class_id != g_candidates[j].class_id) {
                continue;
            }
            /* IoU 大于阈值，则认为是重复框，抑制掉 j。 */
            if (overlap(&g_candidates[i], &g_candidates[j]) > nms_threshold) {
                g_candidates[j].suppressed = 1;
            }
        }
    }

    /* 把未被抑制的候选框转换成最终检测结果。 */
    for (int i = 0; i < g_candidate_count && group->count < YOLO_OBJ_MAX; ++i) {
        /* 当前候选框。 */
        candidate_t *c = &g_candidates[i];
        /* 被 NMS 抑制的框跳过。 */
        if (c->suppressed) {
            continue;
        }

        /* 避免标签文件行数少于类别 id 时越界。 */
        int class_id = c->class_id;
        if (class_id < 0 || class_id >= YOLO_CLASS_NUM) {
            class_id = 0;
        }

        /* 取一个输出槽位。 */
        yolo_detect_t *r = &group->results[group->count++];
        /* 保存类别 id。 */
        r->class_id = class_id;
        /* 保存置信度。 */
        r->score = c->score;
        /* 保存类别名称，超长标签会被安全截断。 */
        copy_obj_name(r->name, g_labels[class_id]);
        /* 模型坐标还原到原始视频坐标并限制范围。 */
        r->box.left = clamp_i((int)(c->x / scale_w), 0, model_w);
        /* 模型坐标还原到原始视频坐标并限制范围。 */
        r->box.top = clamp_i((int)(c->y / scale_h), 0, model_h);
        /* 模型坐标还原到原始视频坐标并限制范围。 */
        r->box.right = clamp_i((int)((c->x + c->w) / scale_w), 0, model_w);
        /* 模型坐标还原到原始视频坐标并限制范围。 */
        r->box.bottom = clamp_i((int)((c->y + c->h) / scale_h), 0, model_h);
    }

    /* 成功。 */
    return 0;
}

/* YOLOv5 三输出后处理入口。 */
int yolo_postprocess_run(int8_t *out0,
                         int8_t *out1,
                         int8_t *out2,
                         int model_h,
                         int model_w,
                         float conf_threshold,
                         float nms_threshold,
                         float scale_w,
                         float scale_h,
                         const int32_t zps[3],
                         const float scales[3],
                         int class_num,
                         yolo_result_group_t *group)
{
    /* labels 必须先加载。 */
    if (!g_labels_loaded) {
        fprintf(stderr, "labels not loaded\n");
        return -1;
    }

    /* 清空输出结果。 */
    memset(group, 0, sizeof(*group));
    /* 清空候选框数量。 */
    g_candidate_count = 0;
    /* 类别数必须在支持范围内。 */
    if (class_num <= 0 || class_num > YOLO_CLASS_NUM) {
        fprintf(stderr, "unexpected class_num=%d\n", class_num);
        return -1;
    }

    /* 解析 stride=8 的输出层。 */
    parse_output(out0, g_anchor0, model_h / 8, model_w / 8, 8, class_num, conf_threshold, zps[0], scales[0]);
    /* 解析 stride=16 的输出层。 */
    parse_output(out1, g_anchor1, model_h / 16, model_w / 16, 16, class_num, conf_threshold, zps[1], scales[1]);
    /* 解析 stride=32 的输出层。 */
    parse_output(out2, g_anchor2, model_h / 32, model_w / 32, 32, class_num, conf_threshold, zps[2], scales[2]);

    /* 做 NMS 并生成最终检测结果。 */
    return finish_candidates(model_h, model_w, scale_w, scale_h, nms_threshold, group);
}

/* YOLOv5 单输出后处理入口，适配自训练导出的 [1,25200,6] 模型。 */
int yolo_postprocess_run_single(int8_t *out0,
                                int elem_count,
                                int model_h,
                                int model_w,
                                float conf_threshold,
                                float nms_threshold,
                                float scale_w,
                                float scale_h,
                                int32_t zp,
                                float scale,
                                yolo_result_group_t *group)
{
    /* labels 必须先加载。 */
    if (!g_labels_loaded) {
        fprintf(stderr, "labels not loaded\n");
        return -1;
    }

    /* 清空输出结果。 */
    memset(group, 0, sizeof(*group));
    /* 清空候选框数量。 */
    g_candidate_count = 0;

    /* 单输出每个预测点 6 个值：x,y,w,h,obj_conf,class_conf。 */
    const int item_size = 6;
    /* 输出总元素数必须能被 6 整除。 */
    if (elem_count <= 0 || elem_count % item_size != 0) {
        fprintf(stderr, "unexpected single YOLO output elems=%d\n", elem_count);
        return -1;
    }

    /* 预测点数量，常见是 25200。 */
    int box_count = elem_count / item_size;

    /* 遍历每个预测点。 */
    for (int i = 0; i < box_count; ++i) {
        /* 每个预测点的起始地址。 */
        int8_t *p = out0 + i * item_size;

        /* YOLOv5 导出后的单输出通常已经是解码后的模型坐标。 */
        float cx = deqnt_i8_to_f32(p[0], zp, scale);
        /* 中心点 y。 */
        float cy = deqnt_i8_to_f32(p[1], zp, scale);
        /* 框宽。 */
        float w = deqnt_i8_to_f32(p[2], zp, scale);
        /* 框高。 */
        float h = deqnt_i8_to_f32(p[3], zp, scale);
        /* 目标置信度。 */
        float obj = deqnt_i8_to_f32(p[4], zp, scale);
        /* 单类别 person 的类别置信度。 */
        float cls = deqnt_i8_to_f32(p[5], zp, scale);
        /* 最终分数。 */
        float score = obj * cls;

        /* 分数低于阈值就跳过。 */
        if (score < conf_threshold) {
            continue;
        }

        /* 过滤异常框。 */
        if (w <= 1.0f || h <= 1.0f) {
            continue;
        }

        /* 如果导出模型输出的是 0~1 归一化坐标，则放大到模型输入尺寸。 */
        if (cx <= 2.0f && cy <= 2.0f && w <= 2.0f && h <= 2.0f) {
            cx *= (float)model_w;
            w *= (float)model_w;
            cy *= (float)model_h;
            h *= (float)model_h;
        }

        /* 从中心点格式转换成左上角格式。 */
        float x = cx - w / 2.0f;
        /* 从中心点格式转换成左上角格式。 */
        float y = cy - h / 2.0f;

        /* 当前模型只有 person 一个类别，所以 class_id 固定为 0。 */
        add_candidate(x, y, w, h, score, 0);
    }

    /* 做 NMS 并生成最终检测结果。 */
    return finish_candidates(model_h, model_w, scale_w, scale_h, nms_threshold, group);
}
