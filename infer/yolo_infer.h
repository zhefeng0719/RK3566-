/* 防止重复包含。 */
#ifndef YOLO_INFER_H
#define YOLO_INFER_H

/* uint8_t/int64_t 等固定宽度整数。 */
#include <stdint.h>

/* 公共配置、共享状态和检测结果类型。 */
#include "pipeline_types.h"

/* RKNN YOLO 运行时上下文。 */
typedef struct {
    /* RKNN context；这里用 void* 存，避免头文件暴露 rknn_api.h。 */
    void *ctx;
    /* 模型输入宽度，例如 640。 */
    int input_w;
    /* 模型输入高度，例如 640。 */
    int input_h;
    /* 模型输入通道数，例如 3。 */
    int input_c;
    /* 模型输入总字节数，例如 640*640*3。 */
    int input_size;
    /* YOLO 类别数，官方 COCO 是 80，自训练 person 是 1。 */
    int class_num;
    /* 输出 tensor 数量：官方 COCO 模型是 3，自训练导出模型可能是 1。 */
    int output_num;
    /* 输出 tensor 的元素数量。 */
    uint32_t output_elems[3];
    /* 输出 tensor 的量化零点。 */
    int32_t output_zps[3];
    /* 输出 tensor 的量化 scale。 */
    float output_scales[3];
} yolo_runtime_t;

/* 初始化 RKNN、读取 tensor 属性、加载 labels。 */
int yolo_runtime_init(yolo_runtime_t *rt, const char *model_path, const char *label_path);
/* 释放 RKNN 上下文和后处理 labels。 */
void yolo_runtime_deinit(yolo_runtime_t *rt);

/* 对一张 RGB888 图像运行 YOLO 推理和后处理。 */
int yolo_runtime_run(yolo_runtime_t *rt,
                     uint8_t *rgb888,
                     int src_w,
                     int src_h,
                     yolo_result_group_t *result,
                     int64_t *cost_us);

/* 推理线程启动参数。 */
typedef struct {
    /* 全局只读配置。 */
    const app_config_t *cfg;
    /* 三线程共享状态。 */
    pipeline_state_t *state;
} infer_thread_arg_t;

/* 推理线程入口函数。 */
void *infer_thread_main(void *arg);

/* 头文件结束。 */
#endif
