/* 防止重复包含。 */
#ifndef YOLO_POSTPROCESS_C_H
#define YOLO_POSTPROCESS_C_H

/* int8_t/int32_t 等固定宽度整数。 */
#include <stdint.h>

/* 单个类别名称最大长度。 */
#define YOLO_OBJ_NAME_MAX 32
/* 单帧最多保留多少个检测结果。 */
#define YOLO_OBJ_MAX      64
/* 最多支持的类别数，COCO 是 80，自训练 person 模型是 1。 */
#define YOLO_CLASS_NUM    80

/* 检测框坐标，坐标系是原始视频帧坐标。 */
typedef struct {
    /* 左边界 x。 */
    int left;
    /* 上边界 y。 */
    int top;
    /* 右边界 x。 */
    int right;
    /* 下边界 y。 */
    int bottom;
} yolo_box_t;

/* 单个检测目标。 */
typedef struct {
    /* 类别名称，例如 person/chair/bed。 */
    char name[YOLO_OBJ_NAME_MAX];
    /* 检测框。 */
    yolo_box_t box;
    /* 置信度。 */
    float score;
    /* 类别 id。 */
    int class_id;
} yolo_detect_t;

/* 一帧图像里的检测结果集合。 */
typedef struct {
    /* 有效检测目标数量。 */
    int count;
    /* 检测目标数组。 */
    yolo_detect_t results[YOLO_OBJ_MAX];
} yolo_result_group_t;

/* 初始化后处理模块，主要是读取 labels 文件。 */
int yolo_postprocess_init(const char *label_path);
/* 释放/复位后处理模块状态。 */
void yolo_postprocess_deinit(void);

/* 对 YOLOv5 三路 INT8 输出做解码、过滤和 NMS。 */
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
                         yolo_result_group_t *group);

/* 对 YOLOv5 导出后的单路输出 [1,25200,6] 做解码、过滤和 NMS。 */
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
                                yolo_result_group_t *group);

/* 头文件结束。 */
#endif
