/* 防止重复包含。 */
#ifndef RGA_PREPROCESS_H
#define RGA_PREPROCESS_H

/* uint8_t/int64_t 等固定宽度整数。 */
#include <stdint.h>

/* RGA 输入来自 FFmpeg 解码后的 AVFrame。 */
#include <libavutil/frame.h>

/* fb_ctx_t 和 rotate_mode_t 定义在公共类型头文件里。 */
#include "pipeline_types.h"

/* 把 DRM_PRIME/NV12 帧通过 RGA 转成 YOLO 需要的 RGB888。 */
int rga_preprocess_yolo(const AVFrame *frame,
                        int dst_w,
                        int dst_h,
                        uint8_t *rgb888,
                        int *src_w_out,
                        int *src_h_out,
                        int64_t *cost_us);

/* 把 DRM_PRIME/NV12 帧通过 RGA 转成 LCD framebuffer 需要的 BGRA8888。 */
int rga_render_lcd(const AVFrame *frame,
                   const fb_ctx_t *fb,
                   rotate_mode_t rotate,
                   uint8_t *bgra,
                   int64_t *cost_us);

/* 头文件结束。 */
#endif
