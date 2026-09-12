/* 解码线程模块头文件。 */
#include "capture_decode.h"

/* PRIu64 打印 uint64_t。 */
#include <inttypes.h>
/* printf/fprintf。 */
#include <stdio.h>

/* FFmpeg 解码 API。 */
#include <libavcodec/avcodec.h>
/* FFmpeg 解封装/RTSP API。 */
#include <libavformat/avformat.h>
/* FFmpeg 基础工具。 */
#include <libavutil/avutil.h>
/* FFmpeg 日志级别控制。 */
#include <libavutil/log.h>
/* 像素格式名称打印。 */
#include <libavutil/pixdesc.h>

/* 把 FFmpeg 错误码打印成人类可读字符串。 */
static void print_av_error(const char *prefix, int err)
{
    /* FFmpeg 错误字符串缓冲区。 */
    char buf[AV_ERROR_MAX_STRING_SIZE];
    /* av_strerror 把错误码转成字符串。 */
    av_strerror(err, buf, sizeof(buf));
    /* 打印错误前缀、错误文本和错误码。 */
    fprintf(stderr, "%s: %s (%d)\n", prefix, buf, err);
}

/* FFmpeg 硬解码器询问输出格式时会调用这个函数。 */
static enum AVPixelFormat choose_decoder_format(AVCodecContext *ctx,
                                                const enum AVPixelFormat *fmts)
{
    /* 当前不需要使用 ctx。 */
    (void)ctx;
    /* 遍历解码器提供的候选格式。 */
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        /* 优先选择 DRM_PRIME，这样能拿到 dma-buf。 */
        if (*p == AV_PIX_FMT_DRM_PRIME) {
            return AV_PIX_FMT_DRM_PRIME;
        }
    }
    /* 如果没有 DRM_PRIME，说明不能走当前硬件零拷贝链路。 */
    fprintf(stderr, "decoder did not offer DRM_PRIME\n");
    /* 返回第一个格式，后面 RGA 模块会拒绝非 DRM_PRIME。 */
    return fmts[0];
}

/* 把最新解码帧发布给显示线程和推理线程。 */
static int publish_latest_frame(pipeline_state_t *state, const AVFrame *frame)
{
    /* clone 只是增加引用，不会复制整帧图像数据；dma-buf 仍然共享。 */
    AVFrame *clone = av_frame_clone(frame);
    /* clone 失败通常是内存不足。 */
    if (!clone) {
        fprintf(stderr, "av_frame_clone failed\n");
        return -1;
    }

    /* 锁住最新帧。 */
    pthread_mutex_lock(&state->frame_mutex);
    /* 如果已有旧帧，释放旧帧引用；我们只保留最新帧。 */
    if (state->latest_frame.frame) {
        av_frame_free(&state->latest_frame.frame);
    }
    /* 保存新帧引用。 */
    state->latest_frame.frame = clone;
    /* 帧序号加一。 */
    state->latest_frame.seq++;
    /* 标记最新帧有效。 */
    state->latest_frame.valid = 1;
    /* 记录解码帧计数。 */
    state->decoded_frames = state->latest_frame.seq;
    /* 唤醒等待新帧的显示线程和推理线程。 */
    pthread_cond_broadcast(&state->frame_cond);
    /* 解锁。 */
    pthread_mutex_unlock(&state->frame_mutex);
    /* 发布成功。 */
    return 0;
}

/* 从解码器中尽可能取出所有当前可用帧。 */
static int receive_ready_frames(AVCodecContext *dec,
                                AVFrame *frame,
                                const app_config_t *cfg,
                                pipeline_state_t *state)
{
    /* 通用返回值。 */
    int ret = 0;

    /* receive_frame 每次取一帧；一个 packet 可能产出多帧。 */
    while (!state->stop && (ret = avcodec_receive_frame(dec, frame)) >= 0) {
        /* 发布最新帧给显示线程和推理线程。 */
        publish_latest_frame(state, frame);
        /* 第 1 帧和每 60 帧打印一次解码状态。 */
        if (state->decoded_frames == 1 || state->decoded_frames % 60 == 0) {
            printf("decode: frame=%" PRIu64 " %dx%d fmt=%s\n",
                   state->decoded_frames,
                   frame->width,
                   frame->height,
                   av_get_pix_fmt_name((enum AVPixelFormat)frame->format));
        }
        /* 归还当前 frame 引用，方便 FFmpeg/MPP 复用底层缓冲。 */
        av_frame_unref(frame);

        /* 如果指定了最大帧数，达到后请求全局停止。 */
        if (cfg->max_frames > 0 && state->decoded_frames >= (uint64_t)cfg->max_frames) {
            pipeline_request_stop(state);
            break;
        }
    }

    /* EAGAIN 表示当前暂时没有更多输出帧，是正常状态。 */
    if (ret == AVERROR(EAGAIN)) {
        return 0;
    }
    /* EOF 表示解码器已经冲刷结束，也按正常结束处理。 */
    if (ret == AVERROR_EOF) {
        return 0;
    }
    /* 其他负数才是真错误。 */
    if (ret < 0) {
        print_av_error("avcodec_receive_frame failed", ret);
        return ret;
    }

    /* 正常返回。 */
    return 0;
}

/* 解码线程主函数。 */
void *decode_thread_main(void *arg)
{
    /* pthread 传入的是 void*，先转回解码线程参数。 */
    decode_thread_arg_t *a = (decode_thread_arg_t *)arg;
    /* 读取全局配置。 */
    const app_config_t *cfg = a->cfg;
    /* 读取共享状态。 */
    pipeline_state_t *state = a->state;

    /* FFmpeg 输入上下文，负责 RTSP 和解封装。 */
    AVFormatContext *fmt = NULL;
    /* FFmpeg 解码器上下文，绑定 h264_rkmpp/hevc_rkmpp。 */
    AVCodecContext *dec = NULL;
    /* 压缩数据包，里面是 H.264/H.265 码流片段。 */
    AVPacket *pkt = NULL;
    /* 解码后的图像帧，目标是 DRM_PRIME/dma-buf。 */
    AVFrame *frame = NULL;
    /* 视频流索引。 */
    int video_index = -1;
    /* 通用返回值。 */
    int ret = 0;

    /* 关闭 FFmpeg 内部噪声日志，避免 h264_rkmpp 在 EAGAIN 场景下刷屏。 */
    av_log_set_level(AV_LOG_FATAL);

    /* 初始化 FFmpeg 网络模块。 */
    avformat_network_init();

    /* RTSP 打开参数。 */
    AVDictionary *opts = NULL;
    /* RTSP/RTP 传输方式：tcp 稳定，udp 低延迟但可能丢包。 */
    av_dict_set(&opts, "rtsp_transport",
                cfg->rtsp_transport ? cfg->rtsp_transport : "tcp",
                0);
    /* 打开连接超时 5 秒。 */
    av_dict_set(&opts, "stimeout", "5000000", 0);
    /* 减少 FFmpeg 内部缓冲，降低延迟。 */
    av_dict_set(&opts, "fflags", "nobuffer", 0);
    /* 请求低延迟模式。 */
    av_dict_set(&opts, "flags", "low_delay", 0);

    /* 打开 RTSP 输入。 */
    ret = avformat_open_input(&fmt, cfg->rtsp_url, NULL, &opts);
    /* 参数字典用完释放。 */
    av_dict_free(&opts);
    /* 打开失败则退出线程。 */
    if (ret < 0) {
        print_av_error("avformat_open_input failed", ret);
        goto out;
    }

    /* 读取流信息，例如编码格式、分辨率等。 */
    ret = avformat_find_stream_info(fmt, NULL);
    /* 流信息读取失败则退出。 */
    if (ret < 0) {
        print_av_error("avformat_find_stream_info failed", ret);
        goto out;
    }

    /* 遍历所有 stream，找到第一个视频流。 */
    for (unsigned int i = 0; i < fmt->nb_streams; ++i) {
        /* codec_type 为 VIDEO 表示视频流。 */
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_index = (int)i;
            break;
        }
    }
    /* 没有视频流则无法继续。 */
    if (video_index < 0) {
        fprintf(stderr, "no video stream\n");
        goto out;
    }

    /* 取得视频流编码参数。 */
    AVCodecParameters *par = fmt->streams[video_index]->codecpar;
    /* 准备选择硬解码器。 */
    AVCodec *decoder = NULL;
    /* H.264 使用 Rockchip MPP H.264 解码器。 */
    if (par->codec_id == AV_CODEC_ID_H264) {
        decoder = avcodec_find_decoder_by_name("h264_rkmpp");
    /* H.265 使用 Rockchip MPP H.265 解码器。 */
    } else if (par->codec_id == AV_CODEC_ID_HEVC) {
        decoder = avcodec_find_decoder_by_name("hevc_rkmpp");
    }
    /* 找不到硬解码器就退出，不走软件解码 fallback。 */
    if (!decoder) {
        fprintf(stderr, "no Rockchip MPP decoder for codec_id=%d\n", par->codec_id);
        goto out;
    }
    /* 打印实际选择的解码器。 */
    printf("selected decoder: %s\n", decoder->name);

    /* 为解码器分配上下文。 */
    dec = avcodec_alloc_context3(decoder);
    /* 分配失败则退出。 */
    if (!dec) {
        goto out;
    }
    /* 把视频流参数复制进解码器上下文。 */
    ret = avcodec_parameters_to_context(dec, par);
    /* 参数复制失败则退出。 */
    if (ret < 0) {
        print_av_error("avcodec_parameters_to_context failed", ret);
        goto out;
    }
    /* 设置像素格式选择回调，强制优先 DRM_PRIME。 */
    dec->get_format = choose_decoder_format;
    /* 直接声明期望输出 DRM_PRIME。 */
    dec->pix_fmt = AV_PIX_FMT_DRM_PRIME;
    /* 设置低延迟标志。 */
    dec->flags |= AV_CODEC_FLAG_LOW_DELAY;

    /* 打开解码器，此时底层会初始化 MPP 解码链路。 */
    ret = avcodec_open2(dec, decoder, NULL);
    /* 解码器打开失败则退出。 */
    if (ret < 0) {
        print_av_error("avcodec_open2 failed", ret);
        goto out;
    }

    /* 分配 packet。 */
    pkt = av_packet_alloc();
    /* 分配 frame。 */
    frame = av_frame_alloc();
    /* 两个对象都必须分配成功。 */
    if (!pkt || !frame) {
        goto out;
    }

    /* 主循环：只要没有停止请求，就持续从 RTSP 读 packet。 */
    while (!state->stop && (ret = av_read_frame(fmt, pkt)) >= 0) {
        /* 非视频 packet 直接丢弃。 */
        if (pkt->stream_index != video_index) {
            av_packet_unref(pkt);
            continue;
        }

        /* 把当前 packet 送进解码器；EAGAIN 时先取帧再重试同一个 packet。 */
        while (!state->stop) {
            /* 送入压缩码流 packet。 */
            ret = avcodec_send_packet(dec, pkt);
            /* 成功送入，跳出重试循环。 */
            if (ret == 0) {
                break;
            }
            /* EAGAIN 表示解码器输出队列未取空，需要先 receive。 */
            if (ret == AVERROR(EAGAIN)) {
                /* 先取出已有解码帧，给 MPP 腾出内部缓冲。 */
                ret = receive_ready_frames(dec, frame, cfg, state);
                /* 取帧发生真错误则退出。 */
                if (ret < 0) {
                    break;
                }
                /* 继续重试同一个 packet，不丢包。 */
                continue;
            }
            /* RTSP 偶尔会有坏包；非 EAGAIN 错误丢包继续跑。 */
            print_av_error("avcodec_send_packet failed, drop packet", ret);
            /* 清掉错误码，让外层继续读取下一个 packet。 */
            ret = 0;
            break;
        }

        /* 当前 packet 不再需要，释放引用。 */
        av_packet_unref(pkt);

        /* 如果 send/receive 发生真错误，退出解码循环。 */
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            break;
        }

        /* 成功送入 packet 后继续尽可能取出所有可用帧。 */
        ret = receive_ready_frames(dec, frame, cfg, state);
        /* 取帧发生真错误则退出。 */
        if (ret < 0) {
            break;
        }
    }

/* 统一清理出口。 */
out:
    /* 解码线程退出时请求其他线程也停止。 */
    pipeline_request_stop(state);
    /* 释放 frame。 */
    av_frame_free(&frame);
    /* 释放 packet。 */
    av_packet_free(&pkt);
    /* 释放解码器上下文。 */
    avcodec_free_context(&dec);
    /* 关闭 RTSP 输入。 */
    avformat_close_input(&fmt);
    /* 反初始化 FFmpeg 网络模块。 */
    avformat_network_deinit();
    /* pthread 线程函数返回 NULL。 */
    return NULL;
}
