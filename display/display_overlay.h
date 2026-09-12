/* 防止重复包含。 */
#ifndef DISPLAY_OVERLAY_H
#define DISPLAY_OVERLAY_H

/* 显示模块需要公共配置、framebuffer 类型和检测结果类型。 */
#include "pipeline_types.h"

/* 打开 /dev/fb0，并读取 LCD 宽高、bpp、行跨度；need_mmap 非 0 时额外尝试 mmap。 */
int fb_open_device(fb_ctx_t *fb, const char *path, int need_mmap);
/* 关闭 framebuffer 文件描述符。 */
void fb_close_device(fb_ctx_t *fb);

/* 把 YOLO 检测框画到 BGRA8888 LCD buffer 上。 */
void draw_result_boxes(uint8_t *bgra,
                       const fb_ctx_t *fb,
                       rotate_mode_t rotate,
                       int src_w,
                       int src_h,
                       const yolo_result_group_t *result);

/* 显示线程启动参数。 */
typedef struct {
    /* 全局只读配置。 */
    const app_config_t *cfg;
    /* 三线程共享状态。 */
    pipeline_state_t *state;
} display_thread_arg_t;

/* 显示线程入口函数。 */
void *display_thread_main(void *arg);

/* 头文件结束。 */
#endif
