/*
 * RC：ONVIF 自动发现 + RTSP 拉流硬解 + RGA 预处理 + YOLO 推理 + LCD 显示。
 *
 * 这个文件只做“业务编排”，具体能力都在子模块里：
 *   onvif/   ：发现摄像头、获取 RTSP URL。
 *   media/   ：FFmpeg 拉流，调用 h264_rkmpp/hevc_rkmpp 硬解码。
 *   infer/   ：RKNN YOLO 推理和后处理。
 *   display/ ：RGA 转 LCD 格式，并在 framebuffer 上显示和画框。
 *
 * 常用运行方式：
 *   ./bin/onvif_yolo_lcd
 *   ./bin/onvif_yolo_lcd admin 123456 auto ./model/person_yolov5n_640_raw_heads.rknn 0 clockwise 5 ./model/person_labels.txt
 *   ./bin/onvif_yolo_lcd unused unused rtsp://192.168.1.64:554/stream1 ./model/person_yolov5n_640_raw_heads.rknn
 */

#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "capture_decode.h"
#include "display_overlay.h"
#include "onvif_client.h"
#include "yolo_infer.h"

/* RC 默认模型：使用 raw-heads 版本，避免 RKNN 1.3.0 对部分 Split 算子支持不足。 */
#define RC_DEFAULT_MODEL_PATH "./model/person_yolov5n_640_raw_heads.rknn"
/* RC 默认标签：自训练 person 单类别模型。 */
#define RC_DEFAULT_LABEL_PATH "./model/person_labels.txt"
/* 默认推理间隔：每 5 帧跑一次 YOLO，降低 NPU/CPU/LCD 总压力。 */
#define RC_DEFAULT_INFER_INTERVAL 5
/* 默认 RTSP 传输：TCP 更稳，UDP 更低延迟但可能丢包。 */
#define RC_DEFAULT_RTSP_TRANSPORT "tcp"
/* 默认 LCD 提交方式：pwrite 是当前已经验证能稳定显示的路径。 */
#define RC_DEFAULT_DISPLAY_BACKEND "pwrite"

/* 信号处理函数需要访问管线状态，所以保存一个全局指针。 */
static pipeline_state_t *g_state = NULL;

/* 判断字符串是否以 prefix 开头。 */
static int starts_with(const char *s, const char *prefix)
{
    if (!s || !prefix) {
        return 0;
    }
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* 判断 RTSP URL 的 host 前面是否已经带有 user:password@。 */
static int rtsp_url_has_userinfo(const char *url)
{
    const char *host_begin;
    const char *path_begin;
    const char *at;

    if (!url) {
        return 0;
    }

    host_begin = strstr(url, "://");
    if (!host_begin) {
        return 0;
    }
    host_begin += 3;

    path_begin = strchr(host_begin, '/');
    at = strchr(host_begin, '@');

    return at && (!path_begin || at < path_begin);
}

/* ONVIF 有些设备返回的 RTSP URL 不带认证信息，这里补成 rtsp://user:pass@host/path。 */
static int build_authenticated_rtsp_url(const char *url,
                                        const char *user,
                                        const char *password,
                                        char *out,
                                        size_t out_size)
{
    const char *host_begin;
    int prefix_len;
    int n;

    if (!url || !out || out_size == 0) {
        return -1;
    }

    if (!starts_with(url, "rtsp://") ||
        rtsp_url_has_userinfo(url) ||
        !user || !user[0] ||
        !password) {
        n = snprintf(out, out_size, "%s", url);
        return (n > 0 && (size_t)n < out_size) ? 0 : -1;
    }

    host_begin = strstr(url, "://") + 3;
    prefix_len = (int)(host_begin - url);

    n = snprintf(out,
                 out_size,
                 "%.*s%s:%s@%s",
                 prefix_len,
                 url,
                 user,
                 password,
                 host_begin);
    return (n > 0 && (size_t)n < out_size) ? 0 : -1;
}

/* 命令行旋转参数转换。 */
static rotate_mode_t parse_rotate(const char *s)
{
    if (!s || strcmp(s, "clockwise") == 0 || strcmp(s, "cw") == 0) {
        return ROTATE_CLOCKWISE;
    }
    if (strcmp(s, "counterclockwise") == 0 || strcmp(s, "ccw") == 0) {
        return ROTATE_COUNTERCLOCKWISE;
    }
    if (strcmp(s, "none") == 0) {
        return ROTATE_NONE;
    }
    return ROTATE_CLOCKWISE;
}

/* 准备 LCD framebuffer 调试显示环境。 */
static void prepare_framebuffer_display(void)
{
    /*
     * 注意：这里不再自动 killall weston。
     * 实测当前板卡上 killall/pidof weston 偶发阻塞，会导致业务程序卡在启动前。
     * 如果 weston 确实占用显示层，建议在外部调试脚本里手动处理。
     */

    /*
     * 某些系统会把 fb0 blank 掉；这里尝试解除 blank。
     * 没有这个 sysfs 节点也没关系，所以 stderr 丢弃。
     */
    system("echo 0 > /sys/class/graphics/fb0/blank 2>/dev/null");
}

/* Ctrl+C 或 kill 时通知三个业务线程优雅退出。 */
static void handle_signal(int signo)
{
    (void)signo;
    if (g_state) {
        pipeline_request_stop(g_state);
    }
}

/* 打印启动参数说明。 */
static void usage(const char *program)
{
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s [user] [password] [auto|device_service_url|rtsp_url] [model] [max_frames] [none|clockwise|counterclockwise] [infer_interval] [labels] [discovery_timeout_ms] [tcp|udp] [pwrite|mmap]\n", program);
    fprintf(stderr, "\n");
    fprintf(stderr, "Defaults:\n");
    fprintf(stderr, "  user/password        : from onvif/onvif_config.h\n");
    fprintf(stderr, "  source               : auto\n");
    fprintf(stderr, "  model                : %s\n", RC_DEFAULT_MODEL_PATH);
    fprintf(stderr, "  max_frames           : 0, run forever\n");
    fprintf(stderr, "  rotate               : clockwise\n");
    fprintf(stderr, "  infer_interval       : %d\n", RC_DEFAULT_INFER_INTERVAL);
    fprintf(stderr, "  labels               : %s\n", RC_DEFAULT_LABEL_PATH);
    fprintf(stderr, "  rtsp_transport       : %s\n", RC_DEFAULT_RTSP_TRANSPORT);
    fprintf(stderr, "  display_backend      : %s\n", RC_DEFAULT_DISPLAY_BACKEND);
    fprintf(stderr, "\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "  %s\n", program);
    fprintf(stderr, "  %s admin 123456 auto %s 0 clockwise 5 %s\n", program, RC_DEFAULT_MODEL_PATH, RC_DEFAULT_LABEL_PATH);
    fprintf(stderr, "  %s admin 123456 http://192.168.1.64/onvif/device_service %s\n", program, RC_DEFAULT_MODEL_PATH);
    fprintf(stderr, "  %s unused unused rtsp://192.168.1.64:554/stream1 %s\n", program, RC_DEFAULT_MODEL_PATH);
}

/* 从命令行和 ONVIF 解析最终 RTSP URL。 */
static int resolve_rtsp_url(int argc,
                            char **argv,
                            char *rtsp_url,
                            size_t rtsp_url_size)
{
    OnvifClientConfig onvif_cfg;
    OnvifClientResult onvif_result;
    const char *source = "auto";

    if (!rtsp_url || rtsp_url_size == 0) {
        return -1;
    }
    rtsp_url[0] = '\0';

    onvif_client_default_config(&onvif_cfg);

    if (argc > 1) {
        onvif_cfg.user = argv[1];
    }
    if (argc > 2) {
        onvif_cfg.password = argv[2];
    }
    if (argc > 3) {
        source = argv[3];
    }
    if (argc > 12) {
        usage(argv[0]);
        return -1;
    }
    if (argc > 9) {
        onvif_cfg.discovery_timeout_ms = atoi(argv[9]);
        if (onvif_cfg.discovery_timeout_ms <= 0) {
            onvif_cfg.discovery_timeout_ms = ONVIF_DISCOVERY_TIMEOUT_MS;
        }
    }

    if (starts_with(source, "rtsp://") || starts_with(source, "rtsps://")) {
        snprintf(rtsp_url, rtsp_url_size, "%s", source);
        printf("source mode : direct RTSP, skip ONVIF\n");
        return 0;
    }

    if (strcmp(source, "auto") == 0) {
        onvif_cfg.manual_device_url = "";
        printf("source mode : ONVIF auto discovery\n");
    } else {
        onvif_cfg.manual_device_url = source;
        printf("source mode : manual ONVIF Device Service URL\n");
    }

    printf("onvif user  : %s\n", onvif_cfg.user ? onvif_cfg.user : "");
    printf("onvif auth  : %s\n", onvif_cfg.use_wsse_digest ? "WSSE digest" : "WSSE text");
    printf("onvif wait  : %d ms\n", onvif_cfg.discovery_timeout_ms);

    if (onvif_client_get_rtsp_url(&onvif_cfg, &onvif_result) < 0) {
        fprintf(stderr, "ONVIF failed: cannot resolve RTSP URL\n");
        return -1;
    }

    if (build_authenticated_rtsp_url(onvif_result.rtsp_url,
                                     onvif_cfg.user,
                                     onvif_cfg.password,
                                     rtsp_url,
                                     rtsp_url_size) < 0) {
        fprintf(stderr, "build authenticated RTSP URL failed\n");
        return -1;
    }

    printf("device url  : %s\n", onvif_result.device_service_url);
    printf("media url   : %s\n", onvif_result.media_service_url);
    printf("profile     : %s\n", onvif_result.profile_token);

    return 0;
}

/* 根据命令行生成整条管线的只读配置。 */
static void fill_app_config(int argc,
                            char **argv,
                            const char *rtsp_url,
                            app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->rtsp_url = rtsp_url;
    cfg->rtsp_transport = (argc > 10) ? argv[10] : RC_DEFAULT_RTSP_TRANSPORT;
    cfg->display_backend = (argc > 11) ? argv[11] : RC_DEFAULT_DISPLAY_BACKEND;
    cfg->model_path = (argc > 4) ? argv[4] : RC_DEFAULT_MODEL_PATH;
    cfg->max_frames = (argc > 5) ? atoi(argv[5]) : 0;
    cfg->rotate = (argc > 6) ? parse_rotate(argv[6]) : ROTATE_CLOCKWISE;
    cfg->infer_interval = (argc > 7) ? atoi(argv[7]) : RC_DEFAULT_INFER_INTERVAL;
    cfg->label_path = (argc > 8) ? argv[8] : RC_DEFAULT_LABEL_PATH;

    if (cfg->max_frames < 0) {
        cfg->max_frames = 0;
    }
    if (cfg->infer_interval <= 0) {
        cfg->infer_interval = RC_DEFAULT_INFER_INTERVAL;
    }
    if (!cfg->rtsp_transport ||
        (strcmp(cfg->rtsp_transport, "tcp") != 0 &&
         strcmp(cfg->rtsp_transport, "udp") != 0)) {
        cfg->rtsp_transport = RC_DEFAULT_RTSP_TRANSPORT;
    }
    if (!cfg->display_backend ||
        (strcmp(cfg->display_backend, "pwrite") != 0 &&
         strcmp(cfg->display_backend, "mmap") != 0)) {
        cfg->display_backend = RC_DEFAULT_DISPLAY_BACKEND;
    }
}

/* 启动解码、推理、显示三个线程。 */
static int run_pipeline(const app_config_t *cfg)
{
    pipeline_state_t state;
    pthread_t decode_thread;
    pthread_t infer_thread;
    pthread_t display_thread;
    decode_thread_arg_t decode_arg;
    infer_thread_arg_t infer_arg;
    display_thread_arg_t display_arg;
    int decode_started = 0;
    int infer_started = 0;
    int display_started = 0;

    if (pipeline_state_init(&state) < 0) {
        fprintf(stderr, "pipeline_state_init failed\n");
        return -1;
    }

    g_state = &state;
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    decode_arg.cfg = cfg;
    decode_arg.state = &state;
    infer_arg.cfg = cfg;
    infer_arg.state = &state;
    display_arg.cfg = cfg;
    display_arg.state = &state;

    if (pthread_create(&decode_thread, NULL, decode_thread_main, &decode_arg) != 0) {
        fprintf(stderr, "pthread_create decode failed\n");
        goto fail;
    }
    decode_started = 1;

    if (pthread_create(&infer_thread, NULL, infer_thread_main, &infer_arg) != 0) {
        fprintf(stderr, "pthread_create infer failed\n");
        goto fail;
    }
    infer_started = 1;

    if (pthread_create(&display_thread, NULL, display_thread_main, &display_arg) != 0) {
        fprintf(stderr, "pthread_create display failed\n");
        goto fail;
    }
    display_started = 1;

    pthread_join(decode_thread, NULL);
    pthread_join(infer_thread, NULL);
    pthread_join(display_thread, NULL);

    printf("final      : decoded=%" PRIu64 " displayed=%" PRIu64 " infer=%" PRIu64 "\n",
           state.decoded_frames,
           state.displayed_frames,
           state.infer_frames);

    pipeline_state_deinit(&state);
    g_state = NULL;
    return 0;

fail:
    pipeline_request_stop(&state);
    if (decode_started) {
        pthread_join(decode_thread, NULL);
    }
    if (infer_started) {
        pthread_join(infer_thread, NULL);
    }
    if (display_started) {
        pthread_join(display_thread, NULL);
    }
    pipeline_state_deinit(&state);
    g_state = NULL;
    return -1;
}

/* 程序入口。 */
int main(int argc, char **argv)
{
    char rtsp_url[512];
    app_config_t cfg;

    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        usage(argv[0]);
        return 0;
    }

    printf("RC ONVIF + FFmpeg/MPP + RGA + RKNN/YOLO + LCD\n");

    if (resolve_rtsp_url(argc, argv, rtsp_url, sizeof(rtsp_url)) < 0) {
        return 1;
    }

    fill_app_config(argc, argv, rtsp_url, &cfg);

    printf("rtsp url   : %s\n", cfg.rtsp_url);
    printf("model      : %s\n", cfg.model_path);
    printf("labels     : %s\n", cfg.label_path);
    printf("max frames : %d\n", cfg.max_frames);
    printf("rotate     : %d\n", cfg.rotate);
    printf("infer gap  : every %d frame(s)\n", cfg.infer_interval);
    printf("transport  : %s\n", cfg.rtsp_transport);
    printf("display    : %s\n", cfg.display_backend);

    prepare_framebuffer_display();

    if (run_pipeline(&cfg) < 0) {
        return 1;
    }

    return 0;
}
