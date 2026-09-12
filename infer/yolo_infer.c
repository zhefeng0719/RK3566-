/* RKNN 推理模块头文件。 */
#include "yolo_infer.h"

/* PRIu64 打印宏。 */
#include <inttypes.h>
/* printf/fprintf。 */
#include <stdio.h>
/* malloc/free。 */
#include <stdlib.h>
/* memset。 */
#include <string.h>

/* FFmpeg 时间函数，用于统计耗时。 */
#include <libavutil/time.h>
/* RKNN Runtime C API。 */
#include <rknn_api.h>

/* 推理线程需要调用 RGA 做 YOLO 输入预处理。 */
#include "rga_preprocess.h"

/* YOLO 置信度阈值。 */
#define YOLO_BOX_THRESH 0.50f
/* YOLO NMS 阈值。 */
#define YOLO_NMS_THRESH 0.60f

/* 当前相对时间，单位微秒。 */
static int64_t now_us(void)
{
    return av_gettime_relative();
}

/* 初始化 YOLO RKNN 运行时。 */
int yolo_runtime_init(yolo_runtime_t *rt, const char *model_path, const char *label_path)
{
    /* 清空运行时结构。 */
    memset(rt, 0, sizeof(*rt));

    /* RKNN 上下文。 */
    rknn_context ctx = 0;
    /* 从模型文件初始化 RKNN。 */
    int ret = rknn_init(&ctx, (void *)model_path, 0, 0, NULL);
    /* 初始化失败则返回。 */
    if (ret < 0) {
        fprintf(stderr, "rknn_init failed: %d\n", ret);
        return -1;
    }
    /* 保存 RKNN 上下文。 */
    rt->ctx = (void *)(uintptr_t)ctx;

    /* 查询 RKNN API 和驱动版本。 */
    rknn_sdk_version ver;
    /* 版本查询成功就打印。 */
    if (rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver)) == RKNN_SUCC) {
        printf("rknn api: %s, driver: %s\n", ver.api_version, ver.drv_version);
    }

    /* 查询模型输入输出数量。 */
    rknn_input_output_num io_num;
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    /* 当前支持 YOLOv5：1 输入；输出可以是官方三输出，也可以是自训练单输出。 */
    if (ret != RKNN_SUCC || io_num.n_input != 1 ||
        (io_num.n_output != 1 && io_num.n_output != 3)) {
        fprintf(stderr, "unexpected YOLO io num, ret=%d input=%d output=%d\n",
                ret, io_num.n_input, io_num.n_output);
        return -1;
    }
    /* 保存输出数量。 */
    rt->output_num = io_num.n_output;

    /* 输入 tensor 属性。 */
    rknn_tensor_attr in_attr;
    /* 清空属性结构。 */
    memset(&in_attr, 0, sizeof(in_attr));
    /* 查询第 0 个输入。 */
    in_attr.index = 0;
    /* 读取输入 tensor 属性。 */
    ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof(in_attr));
    /* 查询失败则返回。 */
    if (ret != RKNN_SUCC) {
        fprintf(stderr, "query input attr failed: %d\n", ret);
        return -1;
    }

    /* 如果模型输入布局是 NHWC，dims=[1,H,W,C]。 */
    if (in_attr.fmt == RKNN_TENSOR_NHWC) {
        rt->input_h = in_attr.dims[1];
        rt->input_w = in_attr.dims[2];
        rt->input_c = in_attr.dims[3];
    /* 如果模型输入布局是 NCHW，dims=[1,C,H,W]。 */
    } else {
        rt->input_c = in_attr.dims[1];
        rt->input_h = in_attr.dims[2];
        rt->input_w = in_attr.dims[3];
    }
    /* 计算输入 buffer 字节数。 */
    rt->input_size = rt->input_w * rt->input_h * rt->input_c;
    /* 打印解析后的模型输入尺寸。 */
    printf("YOLO input: %dx%dx%d\n", rt->input_w, rt->input_h, rt->input_c);

    /* 查询输出 tensor 的量化参数。 */
    for (int i = 0; i < rt->output_num; ++i) {
        /* 输出 tensor 属性。 */
        rknn_tensor_attr out_attr;
        /* 清空属性结构。 */
        memset(&out_attr, 0, sizeof(out_attr));
        /* 设置要查询的输出索引。 */
        out_attr.index = i;
        /* 查询输出属性。 */
        ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &out_attr, sizeof(out_attr));
        /* 查询失败则返回。 */
        if (ret != RKNN_SUCC) {
            fprintf(stderr, "query output attr failed: %d\n", ret);
            return -1;
        }
        /* 保存量化零点。 */
        rt->output_zps[i] = out_attr.zp;
        /* 保存量化 scale。 */
        rt->output_scales[i] = out_attr.scale;
        /* 保存输出元素数量，单输出模型后处理需要用到。 */
        rt->output_elems[i] = out_attr.n_elems;
        /* 三输出 raw-head 模型可以从输出通道数推导类别数：channels = 3 * (5 + class_num)。 */
        if (io_num.n_output == 3 && i == 0) {
            int channels = out_attr.dims[1];
            int class_num = channels / 3 - 5;
            if (class_num > 0 && class_num <= 80) {
                rt->class_num = class_num;
            }
        }
        /* 打印输出属性摘要。 */
        printf("YOLO output[%d]: name=%s elems=%u zp=%d scale=%f\n",
               i, out_attr.name, out_attr.n_elems, out_attr.zp, out_attr.scale);
    }

    /* 单输出导出模型目前是自训练 person，类别数固定 1。 */
    if (rt->output_num == 1) {
        rt->class_num = 1;
    }
    /* 如果三输出模型没有推导出类别数，则按 COCO 80 类兜底。 */
    if (rt->class_num <= 0) {
        rt->class_num = 80;
    }
    /* 打印最终使用的类别数。 */
    printf("YOLO class_num: %d\n", rt->class_num);

    /* 初始化 YOLO C 后处理，主要是读取 labels。 */
    if (yolo_postprocess_init(label_path) < 0) {
        return -1;
    }

    /* 成功。 */
    return 0;
}

/* 释放 YOLO RKNN 运行时。 */
void yolo_runtime_deinit(yolo_runtime_t *rt)
{
    /* 如果 RKNN context 有效，就销毁。 */
    if (rt->ctx) {
        rknn_destroy((rknn_context)(uintptr_t)rt->ctx);
        rt->ctx = NULL;
    }
    /* 释放 YOLO 后处理内部状态。 */
    yolo_postprocess_deinit();
}

/* 对一张 RGB888 输入图运行 RKNN 和 YOLO 后处理。 */
int yolo_runtime_run(yolo_runtime_t *rt,
                     uint8_t *rgb888,
                     int src_w,
                     int src_h,
                     yolo_result_group_t *result,
                     int64_t *cost_us)
{
    /* 取回 RKNN context。 */
    rknn_context ctx = (rknn_context)(uintptr_t)rt->ctx;
    /* RKNN 输入结构。 */
    rknn_input input;
    /* 清空输入结构。 */
    memset(&input, 0, sizeof(input));
    /* 第 0 个输入。 */
    input.index = 0;
    /* 输入数据类型：UINT8。 */
    input.type = RKNN_TENSOR_UINT8;
    /* 输入布局：NHWC。 */
    input.fmt = RKNN_TENSOR_NHWC;
    /* 输入大小。 */
    input.size = rt->input_size;
    /* 输入 buffer 指针。 */
    input.buf = rgb888;

    /* 把输入 buffer 设置给 RKNN。 */
    int ret = rknn_inputs_set(ctx, 1, &input);
    /* 设置失败则返回。 */
    if (ret < 0) {
        fprintf(stderr, "rknn_inputs_set failed: %d\n", ret);
        return -1;
    }

    /* 记录 NPU 推理开始时间。 */
    int64_t t0 = now_us();
    /* 运行 RKNN 推理。 */
    ret = rknn_run(ctx, NULL);
    /* 记录 NPU 推理结束时间。 */
    int64_t t1 = now_us();
    /* 推理失败则返回。 */
    if (ret < 0) {
        fprintf(stderr, "rknn_run failed: %d\n", ret);
        return -1;
    }

    /* YOLOv5 输出 tensor，最多使用三个。 */
    rknn_output outputs[3];
    /* 清空输出结构。 */
    memset(outputs, 0, sizeof(outputs));
    /* 输出都要原始 INT8，不要 runtime 转 float。 */
    for (int i = 0; i < rt->output_num; ++i) {
        outputs[i].want_float = 0;
    }

    /* 取出 RKNN 输出。 */
    ret = rknn_outputs_get(ctx, rt->output_num, outputs, NULL);
    /* 获取失败则返回。 */
    if (ret < 0) {
        fprintf(stderr, "rknn_outputs_get failed: %d\n", ret);
        return -1;
    }

    /* 模型坐标到源图坐标的宽度缩放比例。 */
    float scale_w = (float)rt->input_w / (float)src_w;
    /* 模型坐标到源图坐标的高度缩放比例。 */
    float scale_h = (float)rt->input_h / (float)src_h;
    /* 根据输出数量选择后处理：官方模型是三输出，自训练导出模型是单输出。 */
    if (rt->output_num == 3) {
        ret = yolo_postprocess_run((int8_t *)outputs[0].buf,
                                   (int8_t *)outputs[1].buf,
                                   (int8_t *)outputs[2].buf,
                                   rt->input_h,
                                   rt->input_w,
                                   YOLO_BOX_THRESH,
                                   YOLO_NMS_THRESH,
                                   scale_w,
                                   scale_h,
                                   rt->output_zps,
                                   rt->output_scales,
                                   rt->class_num,
                                   result);
    } else {
        ret = yolo_postprocess_run_single((int8_t *)outputs[0].buf,
                                          (int)rt->output_elems[0],
                                          rt->input_h,
                                          rt->input_w,
                                          YOLO_BOX_THRESH,
                                          YOLO_NMS_THRESH,
                                          scale_w,
                                          scale_h,
                                          rt->output_zps[0],
                                          rt->output_scales[0],
                                          result);
    }

    /* 释放 RKNN 输出。 */
    rknn_outputs_release(ctx, rt->output_num, outputs);
    /* 后处理失败则返回。 */
    if (ret < 0) {
        return -1;
    }

    /* 返回 NPU 推理耗时。 */
    if (cost_us) {
        *cost_us = t1 - t0;
    }
    /* 成功。 */
    return 0;
}

/* 推理线程主函数。 */
void *infer_thread_main(void *arg)
{
    /* 取线程参数。 */
    infer_thread_arg_t *a = (infer_thread_arg_t *)arg;
    /* 全局只读配置。 */
    const app_config_t *cfg = a->cfg;
    /* 共享状态。 */
    pipeline_state_t *state = a->state;
    /* YOLO RKNN 运行时。 */
    yolo_runtime_t yolo;

    /* 初始化 RKNN 和 labels。 */
    if (yolo_runtime_init(&yolo, cfg->model_path, cfg->label_path) < 0) {
        pipeline_request_stop(state);
        return NULL;
    }

    /* 分配 YOLO RGB 输入 buffer。 */
    uint8_t *rgb = (uint8_t *)malloc(yolo.input_size);
    /* 分配失败则停止。 */
    if (!rgb) {
        fprintf(stderr, "malloc yolo input failed\n");
        yolo_runtime_deinit(&yolo);
        pipeline_request_stop(state);
        return NULL;
    }

    /* 推理线程处理到的最新帧序号。 */
    uint64_t last_seq = 0;
    /* RGA 预处理总耗时。 */
    int64_t rga_total = 0;
    /* RKNN 推理总耗时。 */
    int64_t rknn_total = 0;

    /* 主循环：等待新帧，按间隔推理。 */
    while (!state->stop) {
        /* 锁住最新帧。 */
        pthread_mutex_lock(&state->frame_mutex);
        /* 没有新帧就等待。 */
        while (!state->stop && (!state->latest_frame.valid || state->latest_frame.seq == last_seq)) {
            pthread_cond_wait(&state->frame_cond, &state->frame_mutex);
        }
        /* 收到停止请求则退出。 */
        if (state->stop) {
            pthread_mutex_unlock(&state->frame_mutex);
            break;
        }
        /* 如果当前帧不在推理间隔上，就跳过，只更新 last_seq。 */
        if (state->latest_frame.seq % (uint64_t)cfg->infer_interval != 0 && state->latest_frame.seq != 1) {
            last_seq = state->latest_frame.seq;
            pthread_mutex_unlock(&state->frame_mutex);
            continue;
        }
        /* clone 当前最新帧，避免解锁后被解码线程替换。 */
        AVFrame *frame = av_frame_clone(state->latest_frame.frame);
        /* 记录当前帧序号。 */
        last_seq = state->latest_frame.seq;
        /* 解锁。 */
        pthread_mutex_unlock(&state->frame_mutex);

        /* clone 失败则跳过。 */
        if (!frame) {
            continue;
        }

        /* 源图宽度。 */
        int src_w = 0;
        /* 源图高度。 */
        int src_h = 0;
        /* 本次 RGA 预处理耗时。 */
        int64_t rga_us = 0;
        /* 本次 RKNN 推理耗时。 */
        int64_t rknn_us = 0;
        /* 本次检测结果。 */
        yolo_result_group_t result;
        /* 清空检测结果。 */
        memset(&result, 0, sizeof(result));

        /* RGA 把最新帧转成 YOLO RGB，然后运行 RKNN。 */
        if (rga_preprocess_yolo(frame, yolo.input_w, yolo.input_h, rgb, &src_w, &src_h, &rga_us) == 0 &&
            yolo_runtime_run(&yolo, rgb, src_w, src_h, &result, &rknn_us) == 0) {
            /* 锁住最新检测结果。 */
            pthread_mutex_lock(&state->result_mutex);
            /* 更新最新检测结果。 */
            state->latest_result.result = result;
            /* 保存结果对应的源图宽度。 */
            state->latest_result.src_w = src_w;
            /* 保存结果对应的源图高度。 */
            state->latest_result.src_h = src_h;
            /* 保存结果对应的帧序号。 */
            state->latest_result.seq = last_seq;
            /* 标记检测结果有效。 */
            state->latest_result.valid = 1;
            /* 解锁。 */
            pthread_mutex_unlock(&state->result_mutex);

            /* 推理次数加一。 */
            state->infer_frames++;
            /* 累加 RGA 耗时。 */
            rga_total += rga_us;
            /* 累加 RKNN 耗时。 */
            rknn_total += rknn_us;
            /* 每 10 次推理或有检测结果时打印日志。 */
            if (state->infer_frames % 10 == 0 || result.count > 0) {
                printf("infer: count=%" PRIu64 " frame=%" PRIu64 " detect=%d avg_yolo_rga=%.3fms avg_rknn=%.3fms\n",
                       state->infer_frames, last_seq, result.count,
                       (double)rga_total / (double)state->infer_frames / 1000.0,
                       (double)rknn_total / (double)state->infer_frames / 1000.0);
                /* 打印每个检测框的类别、分数和源图坐标。 */
                for (int i = 0; i < result.count; ++i) {
                    printf("  %s %.3f box=(%d,%d,%d,%d)\n",
                           result.results[i].name,
                           result.results[i].score,
                           result.results[i].box.left,
                           result.results[i].box.top,
                           result.results[i].box.right,
                           result.results[i].box.bottom);
                }
            }
        }

        /* 释放 clone 出来的 AVFrame。 */
        av_frame_free(&frame);
    }

    /* 释放 YOLO 输入 buffer。 */
    free(rgb);
    /* 释放 RKNN 和后处理资源。 */
    yolo_runtime_deinit(&yolo);
    /* 线程退出。 */
    return NULL;
}
