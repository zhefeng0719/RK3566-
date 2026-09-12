/* 显示模块头文件。 */
#include "display_overlay.h"

/* errno 错误码。 */
#include <errno.h>
/* open 函数和 O_RDWR。 */
#include <fcntl.h>
/* PRIu64 打印宏。 */
#include <inttypes.h>
/* printf/fprintf。 */
#include <stdio.h>
/* malloc/free/abs。 */
#include <stdlib.h>
/* memset。 */
#include <string.h>
/* ioctl/pwrite/close。 */
#include <sys/ioctl.h>
/* mmap/munmap/msync。 */
#include <sys/mman.h>
#include <unistd.h>

/* Linux framebuffer ioctl 结构。 */
#include <linux/fb.h>

/* 显示线程需要调用 RGA 渲染函数。 */
#include "rga_preprocess.h"

/* 打开 framebuffer 设备。 */
int fb_open_device(fb_ctx_t *fb, const char *path, int need_mmap)
{
    /* 清空 framebuffer 上下文。 */
    memset(fb, 0, sizeof(*fb));
    /* 打开 /dev/fb0。 */
    fb->fd = open(path, O_RDWR);
    /* 打开失败直接返回。 */
    if (fb->fd < 0) {
        fprintf(stderr, "open %s failed: errno=%d\n", path, errno);
        return -1;
    }

    /* 可变屏幕信息，例如分辨率和 bpp。 */
    struct fb_var_screeninfo vinfo;
    /* 固定屏幕信息，例如每行字节数。 */
    struct fb_fix_screeninfo finfo;
    /* 读取 framebuffer 参数。 */
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
        ioctl(fb->fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        fprintf(stderr, "ioctl framebuffer failed: errno=%d\n", errno);
        close(fb->fd);
        fb->fd = -1;
        return -1;
    }

    /* LCD 宽度。 */
    fb->width = (int)vinfo.xres;
    /* LCD 高度。 */
    fb->height = (int)vinfo.yres;
    /* 每像素位数。 */
    fb->bpp = (int)vinfo.bits_per_pixel;
    /* 每行真实字节数。 */
    fb->line_length = (int)finfo.line_length;
    /* 一整屏字节数。 */
    fb->screen_size = (size_t)fb->line_length * fb->height;

    /* 打印 LCD 信息。 */
    printf("fb: %dx%d bpp=%d line=%d size=%zu\n",
           fb->width, fb->height, fb->bpp, fb->line_length, fb->screen_size);

    /* 尝试解除 framebuffer blank，失败不退出，因为有些驱动不支持这个 ioctl。 */
    if (ioctl(fb->fd, FBIOBLANK, FB_BLANK_UNBLANK) < 0) {
        fprintf(stderr, "FBIOBLANK unblank failed: errno=%d, continue\n", errno);
    }

    /* 当前程序只支持 32bpp，方便直接写 BGRA。 */
    if (fb->bpp != 32) {
        fprintf(stderr, "only 32bpp framebuffer is supported\n");
        close(fb->fd);
        fb->fd = -1;
        return -1;
    }

    /* 只有显式选择 mmap 后端时才映射，避免影响已验证的 pwrite 路径。 */
    if (need_mmap) {
#ifndef ENABLE_FB_MMAP_EXPERIMENT
        /*
         * 实测当前 RK3566 镜像上 /dev/fb0 的 mmap() 会阻塞不返回。
         * 为了避免 RC 程序被一个实验选项卡死，默认构建直接回退到 pwrite。
         * 若后续要继续研究 fbdev mmap，可在 Makefile 里额外加 -DENABLE_FB_MMAP_EXPERIMENT。
         */
        fprintf(stderr, "fb mmap disabled in safe build, fallback to pwrite\n");
#else
        printf("fb mmap: try map size=%zu\n", fb->screen_size);
        fb->map_size = fb->screen_size;
        fb->map = (uint8_t *)mmap(NULL,
                                  fb->map_size,
                                  PROT_READ | PROT_WRITE,
                                  MAP_SHARED,
                                  fb->fd,
                                  0);
        if (fb->map == MAP_FAILED) {
            fprintf(stderr, "fb mmap failed: errno=%d, continue with pwrite\n", errno);
            fb->map = NULL;
            fb->map_size = 0;
        } else {
            printf("fb mmap: addr=%p size=%zu\n", (void *)fb->map, fb->map_size);
        }
#endif
    }
    return 0;
}

/* 关闭 framebuffer。 */
void fb_close_device(fb_ctx_t *fb)
{
    /* 如果创建过 mmap 映射，先解除映射。 */
    if (fb->map) {
        munmap(fb->map, fb->map_size);
        fb->map = NULL;
        fb->map_size = 0;
    }
    /* fd 有效才关闭。 */
    if (fb->fd >= 0) {
        close(fb->fd);
        fb->fd = -1;
    }
}

/* 把已经渲染好的 BGRA 整屏提交到 framebuffer。 */
static int fb_present_frame(const fb_ctx_t *fb, const app_config_t *cfg, const uint8_t *bgra)
{
    /* 显式选择 mmap 且 mmap 可用时，直接 memcpy 到 framebuffer 映射区。 */
    if (cfg->display_backend &&
        strcmp(cfg->display_backend, "mmap") == 0 &&
        fb->map &&
        fb->map_size >= fb->screen_size) {
        memcpy(fb->map, bgra, fb->screen_size);
        /* 对 fbdev 映射区通常不需要 msync；这里尝试异步刷新，失败不当作致命错误。 */
        if (msync(fb->map, fb->screen_size, MS_ASYNC) < 0 && errno != EINVAL) {
            fprintf(stderr, "fb msync warning: errno=%d\n", errno);
        }
        return 0;
    }

    /* 默认沿用已经验证稳定的 pwrite 路径。 */
    ssize_t written = pwrite(fb->fd, bgra, fb->screen_size, 0);
    if (written != (ssize_t)fb->screen_size) {
        fprintf(stderr, "pwrite fb failed: %zd/%zu errno=%d\n",
                written, fb->screen_size, errno);
        return -1;
    }
    return 0;
}

/* 在 BGRA8888 buffer 上写一个指定颜色的像素。 */
static void put_pixel_color(uint8_t *buf, const fb_ctx_t *fb, int x, int y,
                            uint8_t r, uint8_t g, uint8_t b)
{
    /* 越界坐标直接忽略。 */
    if (x < 0 || y < 0 || x >= fb->width || y >= fb->height) {
        return;
    }
    /* 根据 y、line_length、x 找到像素地址。 */
    uint8_t *p = buf + y * fb->line_length + x * 4;
    /* BGRA：第 0 字节是 B。 */
    p[0] = b;
    /* BGRA：第 1 字节是 G。 */
    p[1] = g;
    /* BGRA：第 2 字节是 R。 */
    p[2] = r;
    /* Alpha 当前 framebuffer 不关心，置 0。 */
    p[3] = 0;
}

/* 在 BGRA8888 buffer 上写一个绿色像素。 */
static void put_pixel(uint8_t *buf, const fb_ctx_t *fb, int x, int y)
{
    /* 绿色用于检测框和文字，和前面实验保持一致。 */
    put_pixel_color(buf, fb, x, y, 0, 255, 0);
}

/* 取两个整数里的较小值。 */
static int min_int(int a, int b)
{
    /* 三目运算符比写 if 更紧凑。 */
    return a < b ? a : b;
}

/* 取两个整数里的较大值。 */
static int max_int(int a, int b)
{
    /* 三目运算符比写 if 更紧凑。 */
    return a > b ? a : b;
}

/* 把 0-9/a-z/A-Z/. 等字符转换成 5x7 点阵。 */
static const char *glyph_5x7(char c)
{
    /* 大写字母统一转小写，减少字库数量。 */
    if (c >= 'A' && c <= 'Z') {
        c = (char)(c - 'A' + 'a');
    }

    /* 每个字符 7 行，每行 5 列；'1' 表示点亮。 */
    switch (c) {
    case '0': return "01110" "10001" "10011" "10101" "11001" "10001" "01110";
    case '1': return "00100" "01100" "00100" "00100" "00100" "00100" "01110";
    case '2': return "01110" "10001" "00001" "00010" "00100" "01000" "11111";
    case '3': return "11110" "00001" "00001" "01110" "00001" "00001" "11110";
    case '4': return "00010" "00110" "01010" "10010" "11111" "00010" "00010";
    case '5': return "11111" "10000" "10000" "11110" "00001" "00001" "11110";
    case '6': return "01110" "10000" "10000" "11110" "10001" "10001" "01110";
    case '7': return "11111" "00001" "00010" "00100" "01000" "01000" "01000";
    case '8': return "01110" "10001" "10001" "01110" "10001" "10001" "01110";
    case '9': return "01110" "10001" "10001" "01111" "00001" "00001" "01110";
    case 'a': return "01110" "10001" "10001" "11111" "10001" "10001" "10001";
    case 'b': return "11110" "10001" "10001" "11110" "10001" "10001" "11110";
    case 'c': return "01110" "10001" "10000" "10000" "10000" "10001" "01110";
    case 'd': return "11110" "10001" "10001" "10001" "10001" "10001" "11110";
    case 'e': return "11111" "10000" "10000" "11110" "10000" "10000" "11111";
    case 'f': return "11111" "10000" "10000" "11110" "10000" "10000" "10000";
    case 'g': return "01110" "10001" "10000" "10111" "10001" "10001" "01110";
    case 'h': return "10001" "10001" "10001" "11111" "10001" "10001" "10001";
    case 'i': return "01110" "00100" "00100" "00100" "00100" "00100" "01110";
    case 'j': return "00111" "00010" "00010" "00010" "00010" "10010" "01100";
    case 'k': return "10001" "10010" "10100" "11000" "10100" "10010" "10001";
    case 'l': return "10000" "10000" "10000" "10000" "10000" "10000" "11111";
    case 'm': return "10001" "11011" "10101" "10101" "10001" "10001" "10001";
    case 'n': return "10001" "11001" "10101" "10011" "10001" "10001" "10001";
    case 'o': return "01110" "10001" "10001" "10001" "10001" "10001" "01110";
    case 'p': return "11110" "10001" "10001" "11110" "10000" "10000" "10000";
    case 'q': return "01110" "10001" "10001" "10001" "10101" "10010" "01101";
    case 'r': return "11110" "10001" "10001" "11110" "10100" "10010" "10001";
    case 's': return "01111" "10000" "10000" "01110" "00001" "00001" "11110";
    case 't': return "11111" "00100" "00100" "00100" "00100" "00100" "00100";
    case 'u': return "10001" "10001" "10001" "10001" "10001" "10001" "01110";
    case 'v': return "10001" "10001" "10001" "10001" "10001" "01010" "00100";
    case 'w': return "10001" "10001" "10001" "10101" "10101" "10101" "01010";
    case 'x': return "10001" "10001" "01010" "00100" "01010" "10001" "10001";
    case 'y': return "10001" "10001" "01010" "00100" "00100" "00100" "00100";
    case 'z': return "11111" "00001" "00010" "00100" "01000" "10000" "11111";
    case '.': return "00000" "00000" "00000" "00000" "00000" "01100" "01100";
    case '-': return "00000" "00000" "00000" "11111" "00000" "00000" "00000";
    case '_': return "00000" "00000" "00000" "00000" "00000" "00000" "11111";
    case ' ': return "00000" "00000" "00000" "00000" "00000" "00000" "00000";
    default:  return "11111" "00001" "00010" "00100" "00100" "00000" "00100";
    }
}

/* 画一个 5x7 字符，可按 scale 放大。 */
static void draw_char_5x7(uint8_t *buf, const fb_ctx_t *fb, int x, int y, char c, int scale)
{
    /* 取字符点阵。 */
    const char *g = glyph_5x7(c);

    /* 遍历 7 行。 */
    for (int row = 0; row < 7; ++row) {
        /* 遍历 5 列。 */
        for (int col = 0; col < 5; ++col) {
            /* 只有点阵为 '1' 的位置才画。 */
            if (g[row * 5 + col] != '1') {
                continue;
            }
            /* 根据 scale 放大成小方块。 */
            for (int yy = 0; yy < scale; ++yy) {
                for (int xx = 0; xx < scale; ++xx) {
                    put_pixel(buf, fb, x + col * scale + xx, y + row * scale + yy);
                }
            }
        }
    }
}

/* 画一段 ASCII 字符串。 */
static void draw_text_5x7(uint8_t *buf, const fb_ctx_t *fb, int x, int y,
                          const char *text, int scale)
{
    /* 当前字符的绘制 x 坐标。 */
    int cursor_x = x;

    /* 逐字符绘制，遇到字符串结束符停止。 */
    for (const char *p = text; *p; ++p) {
        /* 画当前字符。 */
        draw_char_5x7(buf, fb, cursor_x, y, *p, scale);
        /* 字符宽度 5，加 1 列间距。 */
        cursor_x += 6 * scale;
        /* 超出屏幕右侧就停止，防止无意义循环。 */
        if (cursor_x >= fb->width) {
            break;
        }
    }
}

/* 画文字背后的黑色底，增强在复杂画面上的可读性。 */
static void draw_text_background(uint8_t *buf, const fb_ctx_t *fb,
                                 int x, int y, int w, int h)
{
    /* 遍历背景矩形每一行。 */
    for (int yy = 0; yy < h; ++yy) {
        /* 遍历背景矩形每一列。 */
        for (int xx = 0; xx < w; ++xx) {
            /* 黑色底：RGB 都为 0。 */
            put_pixel_color(buf, fb, x + xx, y + yy, 0, 0, 0);
        }
    }
}

/* 在检测框旁边画类别名和置信度。 */
static void draw_box_label(uint8_t *buf, const fb_ctx_t *fb,
                           int box_min_x, int box_min_y,
                           const char *name, float score)
{
    /* 标签文本缓冲。 */
    char text[64];
    /* 格式示例：person 0.82。 */
    snprintf(text, sizeof(text), "%s %.2f", name, score);

    /* 字体放大倍数；480x800 小屏上 2 倍比较容易看清。 */
    const int scale = 2;
    /* 每个字符实际占用 6*scale 宽，最后一个字符多算一点没关系。 */
    int text_w = (int)strlen(text) * 6 * scale;
    /* 字符高度 7*scale。 */
    int text_h = 7 * scale;

    /* 默认把文字放在框上方。 */
    int tx = box_min_x;
    int ty = box_min_y - text_h - 4;

    /* 如果上方放不下，就放到框内部偏下位置。 */
    if (ty < 0) {
        ty = box_min_y + 4;
    }
    /* 防止文字超出屏幕右边。 */
    if (tx + text_w >= fb->width) {
        tx = fb->width - text_w - 1;
    }
    /* 防止文字超出屏幕左边。 */
    if (tx < 0) {
        tx = 0;
    }
    /* 防止文字超出屏幕下边。 */
    if (ty + text_h >= fb->height) {
        ty = fb->height - text_h - 1;
    }
    /* 防止文字超出屏幕上边。 */
    if (ty < 0) {
        ty = 0;
    }

    /* 先画黑底，比只画绿字更容易看清楚。 */
    draw_text_background(buf, fb, tx, ty, text_w + 2, text_h + 2);
    /* 再画绿色文字。 */
    draw_text_5x7(buf, fb, tx + 1, ty + 1, text, scale);
}

/* 把原始视频坐标映射到 LCD 坐标。 */
static void map_source_to_lcd(int sx, int sy, int src_w, int src_h,
                              const fb_ctx_t *fb, rotate_mode_t rotate,
                              int *dx, int *dy)
{
    /* 顺时针旋转：源 y 影响 LCD x，源 x 影响 LCD y。 */
    if (rotate == ROTATE_CLOCKWISE) {
        *dx = (src_h - 1 - sy) * fb->width / src_h;
        *dy = sx * fb->height / src_w;
    /* 逆时针旋转：方向和顺时针相反。 */
    } else if (rotate == ROTATE_COUNTERCLOCKWISE) {
        *dx = sy * fb->width / src_h;
        *dy = (src_w - 1 - sx) * fb->height / src_w;
    /* 不旋转：按宽高比例直接映射。 */
    } else {
        *dx = sx * fb->width / src_w;
        *dy = sy * fb->height / src_h;
    }
}

/* 用 Bresenham 算法画一条绿色粗线。 */
static void draw_line(uint8_t *buf, const fb_ctx_t *fb, int x0, int y0, int x1, int y1)
{
    /* x 方向距离。 */
    int dx = abs(x1 - x0);
    /* x 方向步进。 */
    int sx = x0 < x1 ? 1 : -1;
    /* y 方向距离，取负数是 Bresenham 经典写法。 */
    int dy = -abs(y1 - y0);
    /* y 方向步进。 */
    int sy = y0 < y1 ? 1 : -1;
    /* 误差项。 */
    int err = dx + dy;

    /* 循环直到画到终点。 */
    while (1) {
        /* 画 3x3 小方块，让框线更粗一点。 */
        for (int oy = -1; oy <= 1; ++oy) {
            for (int ox = -1; ox <= 1; ++ox) {
                put_pixel(buf, fb, x0 + ox, y0 + oy);
            }
        }
        /* 到达终点则结束。 */
        if (x0 == x1 && y0 == y1) {
            break;
        }
        /* Bresenham 误差翻倍。 */
        int e2 = 2 * err;
        /* 判断是否移动 x。 */
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        /* 判断是否移动 y。 */
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* 把一组 YOLO 检测框画到 LCD BGRA buffer 上。 */
void draw_result_boxes(uint8_t *bgra,
                       const fb_ctx_t *fb,
                       rotate_mode_t rotate,
                       int src_w,
                       int src_h,
                       const yolo_result_group_t *result)
{
    /* 没有检测结果或源尺寸非法时直接返回。 */
    if (!result || src_w <= 0 || src_h <= 0) {
        return;
    }

    /* 遍历每个检测框。 */
    for (int i = 0; i < result->count; ++i) {
        /* 取出第 i 个框。 */
        const yolo_box_t *b = &result->results[i].box;
        /* 框四个角映射后的 LCD 坐标。 */
        int x0, y0, x1, y1, x2, y2, x3, y3;
        /* 左上角。 */
        map_source_to_lcd(b->left, b->top, src_w, src_h, fb, rotate, &x0, &y0);
        /* 右上角。 */
        map_source_to_lcd(b->right, b->top, src_w, src_h, fb, rotate, &x1, &y1);
        /* 右下角。 */
        map_source_to_lcd(b->right, b->bottom, src_w, src_h, fb, rotate, &x2, &y2);
        /* 左下角。 */
        map_source_to_lcd(b->left, b->bottom, src_w, src_h, fb, rotate, &x3, &y3);
        /* 上边。 */
        draw_line(bgra, fb, x0, y0, x1, y1);
        /* 右边。 */
        draw_line(bgra, fb, x1, y1, x2, y2);
        /* 下边。 */
        draw_line(bgra, fb, x2, y2, x3, y3);
        /* 左边。 */
        draw_line(bgra, fb, x3, y3, x0, y0);

        /* 计算旋转后四个角的最小 x，用来放标签。 */
        int box_min_x = min_int(min_int(x0, x1), min_int(x2, x3));
        /* 计算旋转后四个角的最小 y，用来放标签。 */
        int box_min_y = min_int(min_int(y0, y1), min_int(y2, y3));
        /* 计算旋转后四个角的最大 x，后续如果要做更复杂布局可用。 */
        int box_max_x = max_int(max_int(x0, x1), max_int(x2, x3));
        /* 当前只需要避免未使用警告，同时保留调试意义。 */
        (void)box_max_x;

        /* 在框旁边写类别英文和置信度。 */
        draw_box_label(bgra, fb, box_min_x, box_min_y,
                       result->results[i].name,
                       result->results[i].score);
    }
}

/* 显示线程主函数。 */
void *display_thread_main(void *arg)
{
    /* 取线程参数。 */
    display_thread_arg_t *a = (display_thread_arg_t *)arg;
    /* 共享状态。 */
    pipeline_state_t *state = a->state;
    /* 只读配置。 */
    const app_config_t *cfg = a->cfg;
    /* framebuffer 上下文。 */
    fb_ctx_t fb;

    /* mmap 只在命令行选择 mmap 后端时启用。 */
    int need_mmap = cfg->display_backend &&
                    strcmp(cfg->display_backend, "mmap") == 0;
    /* 打开 /dev/fb0。 */
    if (fb_open_device(&fb, "/dev/fb0", need_mmap) < 0) {
        pipeline_request_stop(state);
        return NULL;
    }
    /* 打印本次显示提交方式，便于对比 pwrite 和 mmap。 */
    printf("display backend: %s%s\n",
           cfg->display_backend ? cfg->display_backend : "pwrite",
           (!fb.map && cfg->display_backend &&
            strcmp(cfg->display_backend, "mmap") == 0) ? " (fallback pwrite)" : "");

    /* 分配一整屏 BGRA buffer，RGA 先输出到这里。 */
    uint8_t *bgra = (uint8_t *)malloc(fb.screen_size);
    /* 分配失败则停止整条管线。 */
    if (!bgra) {
        fprintf(stderr, "malloc lcd bgra failed\n");
        fb_close_device(&fb);
        pipeline_request_stop(state);
        return NULL;
    }

    /* 显示线程已经处理到的最新帧序号。 */
    uint64_t last_seq = 0;
    /* RGA 显示总耗时。 */
    int64_t rga_total = 0;

    /* 主循环：直到收到 stop。 */
    while (!state->stop) {
        /* 锁住最新帧。 */
        pthread_mutex_lock(&state->frame_mutex);
        /* 如果没有新帧，就等待解码线程广播。 */
        while (!state->stop && (!state->latest_frame.valid || state->latest_frame.seq == last_seq)) {
            pthread_cond_wait(&state->frame_cond, &state->frame_mutex);
        }
        /* stop 后退出。 */
        if (state->stop) {
            pthread_mutex_unlock(&state->frame_mutex);
            break;
        }
        /* clone 最新帧，避免解锁后被解码线程替换释放。 */
        AVFrame *frame = av_frame_clone(state->latest_frame.frame);
        /* 记录这次处理的帧序号。 */
        last_seq = state->latest_frame.seq;
        /* 解锁，减少持锁时间。 */
        pthread_mutex_unlock(&state->frame_mutex);

        /* clone 失败就跳过。 */
        if (!frame) {
            continue;
        }

        /* 单次 LCD RGA 耗时。 */
        int64_t rga_us = 0;
        /* 把当前帧 RGA 到 LCD BGRA buffer。 */
        if (rga_render_lcd(frame, &fb, cfg->rotate, bgra, &rga_us) == 0) {
            /* 复制一份最新检测结果。 */
            shared_result_t result_copy;
            /* 锁住检测结果。 */
            pthread_mutex_lock(&state->result_mutex);
            /* 结构体拷贝，拿到当前最新框。 */
            result_copy = state->latest_result;
            /* 解锁。 */
            pthread_mutex_unlock(&state->result_mutex);

            /* 如果已有有效检测结果，就叠加绿色框。 */
            if (result_copy.valid) {
                draw_result_boxes(bgra, &fb, cfg->rotate,
                                  result_copy.src_w, result_copy.src_h,
                                  &result_copy.result);
            }

            /* 把整屏 BGRA buffer 提交到 framebuffer。 */
            fb_present_frame(&fb, cfg, bgra);

            /* 显示帧计数加一。 */
            state->displayed_frames++;
            /* 累加 RGA 耗时。 */
            rga_total += rga_us;
            /* 每 30 帧打印一次显示性能。 */
            if (state->displayed_frames % 30 == 0) {
                printf("display: frames=%" PRIu64 " avg_lcd_rga=%.3fms\n",
                       state->displayed_frames,
                       (double)rga_total / (double)state->displayed_frames / 1000.0);
            }
        }

        /* 释放 clone 出来的 AVFrame 引用。 */
        av_frame_free(&frame);
    }

    /* 释放 LCD BGRA buffer。 */
    free(bgra);
    /* 关闭 framebuffer。 */
    fb_close_device(&fb);
    /* 线程正常退出。 */
    return NULL;
}
