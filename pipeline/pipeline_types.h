/* 防止头文件被重复包含。 */
#ifndef PIPELINE_TYPES_H
#define PIPELINE_TYPES_H

/* pthread 用于互斥锁、条件变量和线程类型。 */
#include <pthread.h>
/* size_t 类型来自 stddef.h。 */
#include <stddef.h>
/* uint64_t 等固定宽度整数来自 stdint.h。 */
#include <stdint.h>

/* AVFrame 是 FFmpeg 表示一帧解码后图像的核心结构。 */
#include <libavutil/frame.h>

/* YOLO 检测框结果结构来自 C 版后处理模块。 */
#include "yolo_postprocess_c.h"

/* LCD 显示旋转方向。 */
typedef enum {
    /* 不旋转，源画面按原方向缩放到 LCD。 */
    ROTATE_NONE = 0,
    /* 顺时针旋转 90 度，适配当前竖屏 LCD。 */
    ROTATE_CLOCKWISE = 1,
    /* 逆时针旋转 90 度，作为备用方向。 */
    ROTATE_COUNTERCLOCKWISE = 2,
} rotate_mode_t;

/* Linux framebuffer 设备上下文。 */
typedef struct {
    /* /dev/fb0 的文件描述符。 */
    int fd;
    /* framebuffer 的 mmap 映射地址；为空表示 mmap 不可用。 */
    uint8_t *map;
    /* mmap 映射长度，一般等于 screen_size。 */
    size_t map_size;
    /* LCD 可见宽度，例如 480。 */
    int width;
    /* LCD 可见高度，例如 800。 */
    int height;
    /* 每个像素位数，当前项目是 32bpp。 */
    int bpp;
    /* framebuffer 每行真实字节数，可能大于 width * bytes_per_pixel。 */
    int line_length;
    /* 一整屏需要写入的字节数。 */
    size_t screen_size;
} fb_ctx_t;

/* 程序启动参数，所有线程只读这份配置。 */
typedef struct {
    /* RTSP 地址，例如 rtsp://192.168.1.3:8554/cam。 */
    const char *rtsp_url;
    /* RTSP 传输方式：tcp 或 udp。 */
    const char *rtsp_transport;
    /* LCD 提交方式：pwrite 或 mmap。 */
    const char *display_backend;
    /* RKNN 模型路径，例如 model/RK356X/yolov5s-640-640.rknn。 */
    const char *model_path;
    /* labels 路径，例如 ./model/coco_80_labels_list.txt。 */
    const char *label_path;
    /* 最大解码帧数，0 表示一直运行。 */
    int max_frames;
    /* 推理间隔，例如 5 表示每 5 帧推理一次。 */
    int infer_interval;
    /* LCD 显示旋转方向。 */
    rotate_mode_t rotate;
} app_config_t;

/* 线程之间共享的最新视频帧。 */
typedef struct {
    /* 最新 AVFrame；这里通常是 DRM_PRIME / dma-buf 帧。 */
    AVFrame *frame;
    /* 帧序号，每发布一帧加 1。 */
    uint64_t seq;
    /* 是否已经有有效帧。 */
    int valid;
} shared_frame_t;

/* 线程之间共享的最新 YOLO 检测结果。 */
typedef struct {
    /* YOLO 后处理得到的检测框集合。 */
    yolo_result_group_t result;
    /* 检测框对应的原始视频宽度，例如 640。 */
    int src_w;
    /* 检测框对应的原始视频高度，例如 360。 */
    int src_h;
    /* 这份检测结果对应哪一帧。 */
    uint64_t seq;
    /* 是否已经有有效检测结果。 */
    int valid;
} shared_result_t;

/* 整条管线的共享状态。 */
typedef struct {
    /* 保护 latest_frame 的互斥锁。 */
    pthread_mutex_t frame_mutex;
    /* 新帧到达时唤醒 display/infer 线程。 */
    pthread_cond_t frame_cond;
    /* 最新解码帧，只保留一帧，不排队老帧。 */
    shared_frame_t latest_frame;

    /* 保护 latest_result 的互斥锁。 */
    pthread_mutex_t result_mutex;
    /* 最新检测框，显示线程会复用它画框。 */
    shared_result_t latest_result;

    /* 全局停止标志；Ctrl+C 或解码结束都会置 1。 */
    volatile int stop;
    /* 解码线程发布过多少帧。 */
    uint64_t decoded_frames;
    /* 显示线程显示过多少帧。 */
    uint64_t displayed_frames;
    /* 推理线程完成过多少次推理。 */
    uint64_t infer_frames;
} pipeline_state_t;

/* 初始化共享状态里的锁、条件变量和字段。 */
int pipeline_state_init(pipeline_state_t *state);
/* 释放共享状态里持有的 AVFrame 和同步对象。 */
void pipeline_state_deinit(pipeline_state_t *state);
/* 请求所有线程停止，并唤醒可能正在等待新帧的线程。 */
void pipeline_request_stop(pipeline_state_t *state);

/* 头文件结束。 */
#endif
