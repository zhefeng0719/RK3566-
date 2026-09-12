/* RGA 预处理模块头文件。 */
#include "rga_preprocess.h"

/* PRIu64/PRIx64 等格式宏。 */
#include <inttypes.h>
/* printf/fprintf。 */
#include <stdio.h>

/* DRM_PRIME 帧描述结构。 */
#include <libavutil/hwcontext_drm.h>
/* 像素格式名称工具。 */
#include <libavutil/pixdesc.h>
/* FFmpeg 高精度相对时间。 */
#include <libavutil/time.h>

/* RGA 工具函数声明。 */
#include <RgaUtils.h>
/* RGA im2d API。 */
#include <im2d.h>
/* RGA 基础 API。 */
#include <rga.h>

/* 如果 sysroot 没有 drm_fourcc.h，就手动定义 NV12 fourcc。 */
#ifndef DRM_FORMAT_NV12
#define DRM_FORMAT_NV12 0x3231564e
#endif

/* 返回当前相对时间，单位微秒。 */
static int64_t now_us(void)
{
    return av_gettime_relative();
}

/* 把 fourcc 数值转成四字符字符串，例如 NV12。 */
static void fourcc_to_string(uint32_t fourcc, char out[5])
{
    /* 取最低 8 位作为第 1 个字符。 */
    out[0] = (char)(fourcc & 0xff);
    /* 取第 8~15 位作为第 2 个字符。 */
    out[1] = (char)((fourcc >> 8) & 0xff);
    /* 取第 16~23 位作为第 3 个字符。 */
    out[2] = (char)((fourcc >> 16) & 0xff);
    /* 取第 24~31 位作为第 4 个字符。 */
    out[3] = (char)((fourcc >> 24) & 0xff);
    /* C 字符串结尾。 */
    out[4] = '\0';
}

/* 第一次拿到 DRM_PRIME 帧时打印 dma-buf 布局。 */
static void print_drm_desc_once(const AVDRMFrameDescriptor *desc)
{
    /* 静态变量用于保证只打印一次。 */
    static int printed = 0;
    /* 已打印过就直接返回。 */
    if (printed) {
        return;
    }
    /* 标记已经打印。 */
    printed = 1;

    /* 打印 object 和 layer 数量。 */
    printf("drm_prime descriptor:\n");
    printf("  nb_objects=%d nb_layers=%d\n", desc->nb_objects, desc->nb_layers);
    /* 遍历 dma-buf object。 */
    for (int i = 0; i < desc->nb_objects; ++i) {
        printf("  object[%d]: fd=%d size=%zu modifier=0x%" PRIx64 "\n",
               i, desc->objects[i].fd, (size_t)desc->objects[i].size,
               desc->objects[i].format_modifier);
    }
    /* 遍历图像 layer。 */
    for (int i = 0; i < desc->nb_layers; ++i) {
        /* fourcc 字符串。 */
        char fmt[5];
        /* 把 layer format 转成字符串。 */
        fourcc_to_string(desc->layers[i].format, fmt);
        /* 打印 layer 的格式和 plane 数量。 */
        printf("  layer[%d]: format=%s(0x%08x) nb_planes=%d\n",
               i, fmt, desc->layers[i].format, desc->layers[i].nb_planes);
        /* 遍历 plane，打印 offset 和 pitch。 */
        for (int p = 0; p < desc->layers[i].nb_planes; ++p) {
            printf("    plane[%d]: object=%d offset=%zu pitch=%td\n",
                   p,
                   desc->layers[i].planes[p].object_index,
                   (size_t)desc->layers[i].planes[p].offset,
                   desc->layers[i].planes[p].pitch);
        }
    }
}

/* 从 FFmpeg DRM_PRIME AVFrame 中取出 NV12 dma-buf 参数。 */
static int get_nv12_dma_buf(const AVFrame *frame,
                            int *dma_fd,
                            int *visible_w,
                            int *visible_h,
                            int *stride_w,
                            int *stride_h)
{
    /* 当前项目只接受 DRM_PRIME 帧。 */
    if (frame->format != AV_PIX_FMT_DRM_PRIME || !frame->data[0]) {
        fprintf(stderr, "expected DRM_PRIME frame, got %s\n",
                av_get_pix_fmt_name((enum AVPixelFormat)frame->format));
        return -1;
    }

    /* data[0] 指向 AVDRMFrameDescriptor。 */
    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)frame->data[0];
    /* 第一次打印 dma-buf 布局。 */
    print_drm_desc_once(desc);

    /* 必须至少有一个 object 和一个 layer。 */
    if (desc->nb_objects < 1 || desc->nb_layers < 1) {
        fprintf(stderr, "bad DRM_PRIME descriptor\n");
        return -1;
    }

    /* 取第 0 个 layer，正常视频帧通常只有一层。 */
    const AVDRMLayerDescriptor *layer = &desc->layers[0];
    /* 当前只支持 NV12，两 plane：Y 和 UV。 */
    if (layer->format != DRM_FORMAT_NV12 || layer->nb_planes < 2) {
        /* 如果格式不对，打印实际 fourcc。 */
        char fmt[5];
        fourcc_to_string(layer->format, fmt);
        fprintf(stderr, "only NV12 DRM_PRIME is supported, got %s\n", fmt);
        return -1;
    }

    /* plane 0 是 Y 平面。 */
    const AVDRMPlaneDescriptor *y_plane = &layer->planes[0];
    /* plane 1 是 UV 平面。 */
    const AVDRMPlaneDescriptor *uv_plane = &layer->planes[1];
    /* 从 object 中取得真实 dma-buf fd。 */
    int fd = desc->objects[y_plane->object_index].fd;
    /* 检查 fd、pitch、UV offset 是否有效。 */
    if (fd < 0 || y_plane->pitch <= 0 || uv_plane->offset == 0) {
        fprintf(stderr, "bad NV12 dma-buf layout\n");
        return -1;
    }

    /* 输出 dma-buf fd。 */
    *dma_fd = fd;
    /* 输出可见宽度。 */
    *visible_w = frame->width;
    /* 输出可见高度。 */
    *visible_h = frame->height;
    /* 输出真实行跨度。 */
    *stride_w = (int)y_plane->pitch;
    /* UV offset / pitch 可得到 Y 平面的真实 stride 高度。 */
    *stride_h = (int)(uv_plane->offset / y_plane->pitch);
    /* 成功。 */
    return 0;
}

/* RGA：把 NV12 dma-buf 转成 YOLO 输入 RGB888。 */
int rga_preprocess_yolo(const AVFrame *frame,
                        int dst_w,
                        int dst_h,
                        uint8_t *rgb888,
                        int *src_w_out,
                        int *src_h_out,
                        int64_t *cost_us)
{
    /* dma-buf fd。 */
    int dma_fd, src_w, src_h, stride_w, stride_h;
    /* 从 AVFrame 中解析 NV12 dma-buf 布局。 */
    if (get_nv12_dma_buf(frame, &dma_fd, &src_w, &src_h, &stride_w, &stride_h) < 0) {
        return -1;
    }

    /* 把 dma-buf fd 包装成 RGA 输入 buffer。 */
    rga_buffer_t src = wrapbuffer_fd_t(dma_fd, src_w, src_h, stride_w, stride_h, RK_FORMAT_YCbCr_420_SP);
    /* 把普通内存 rgb888 包装成 RGA 输出 buffer。 */
    rga_buffer_t dst = wrapbuffer_virtualaddr(rgb888, dst_w, dst_h, RK_FORMAT_RGB_888);
    /* 源矩形：完整输入画面。 */
    im_rect src_rect = {0, 0, src_w, src_h};
    /* 目标矩形：完整 YOLO 输入尺寸。 */
    im_rect dst_rect = {0, 0, dst_w, dst_h};
    /* 第三路 pattern buffer 当前不用，置零。 */
    rga_buffer_t pat = {0};
    /* pattern 矩形当前不用，置零。 */
    im_rect pat_rect = {0};

    /* 记录 RGA 开始时间。 */
    int64_t t0 = now_us();
    /* 执行 RGA：NV12 -> RGB888，并缩放到 YOLO 输入尺寸。 */
    IM_STATUS ret = improcess(src, dst, pat, src_rect, dst_rect, pat_rect, IM_SYNC);
    /* 记录 RGA 结束时间。 */
    int64_t t1 = now_us();
    /* 检查 RGA 返回值。 */
    if (ret != IM_STATUS_SUCCESS) {
        fprintf(stderr, "RGA YOLO preprocess failed: %s\n", imStrError(ret));
        return -1;
    }

    /* 如果调用者需要耗时，就返回微秒耗时。 */
    if (cost_us) {
        *cost_us = t1 - t0;
    }
    /* 返回原始视频宽度。 */
    if (src_w_out) {
        *src_w_out = src_w;
    }
    /* 返回原始视频高度。 */
    if (src_h_out) {
        *src_h_out = src_h;
    }
    /* 成功。 */
    return 0;
}

/* RGA：把 NV12 dma-buf 转成 LCD 需要的 BGRA8888。 */
int rga_render_lcd(const AVFrame *frame,
                   const fb_ctx_t *fb,
                   rotate_mode_t rotate,
                   uint8_t *bgra,
                   int64_t *cost_us)
{
    /* dma-buf fd 和图像尺寸/stride。 */
    int dma_fd, src_w, src_h, stride_w, stride_h;
    /* 从 AVFrame 中解析 NV12 dma-buf 布局。 */
    if (get_nv12_dma_buf(frame, &dma_fd, &src_w, &src_h, &stride_w, &stride_h) < 0) {
        return -1;
    }

    /* 把 dma-buf fd 包装成 RGA 输入。 */
    rga_buffer_t src = wrapbuffer_fd_t(dma_fd, src_w, src_h, stride_w, stride_h, RK_FORMAT_YCbCr_420_SP);
    /* 把 LCD BGRA 内存包装成 RGA 输出。 */
    rga_buffer_t dst = wrapbuffer_virtualaddr(bgra, fb->width, fb->height, RK_FORMAT_BGRA_8888);
    /* 源矩形：完整视频帧。 */
    im_rect src_rect = {0, 0, src_w, src_h};
    /* 目标矩形：完整 LCD 屏幕。 */
    im_rect dst_rect = {0, 0, fb->width, fb->height};
    /* pattern buffer 当前不用。 */
    rga_buffer_t pat = {0};
    /* pattern rect 当前不用。 */
    im_rect pat_rect = {0};
    /* IM_SYNC 表示同步执行，函数返回时 RGA 已完成。 */
    int usage = IM_SYNC;

    /* 顺时针旋转 90 度。 */
    if (rotate == ROTATE_CLOCKWISE) {
        usage |= IM_HAL_TRANSFORM_ROT_90;
    /* 逆时针旋转 90 度。 */
    } else if (rotate == ROTATE_COUNTERCLOCKWISE) {
        usage |= IM_HAL_TRANSFORM_ROT_270;
    }

    /* 记录 RGA 开始时间。 */
    int64_t t0 = now_us();
    /* 执行 RGA：NV12 -> BGRA8888，并完成旋转和缩放。 */
    IM_STATUS ret = improcess(src, dst, pat, src_rect, dst_rect, pat_rect, usage);
    /* 记录 RGA 结束时间。 */
    int64_t t1 = now_us();
    /* 检查 RGA 返回值。 */
    if (ret != IM_STATUS_SUCCESS) {
        fprintf(stderr, "RGA LCD render failed: %s\n", imStrError(ret));
        return -1;
    }

    /* 返回耗时。 */
    if (cost_us) {
        *cost_us = t1 - t0;
    }
    /* 成功。 */
    return 0;
}
