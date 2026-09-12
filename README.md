# RK3566 ONVIF RTSP YOLO LCD Pipeline

这是一个面向 RK3566 Buildroot 环境的网络摄像头视频处理示例工程。程序通过 ONVIF 获取摄像头 RTSP 地址，使用 FFmpeg 调用 Rockchip MPP 硬解码 H.264/H.265 视频流，再使用 RGA 做图像格式转换和缩放，最后通过 RKNN/NPU 执行 YOLO 推理，并把叠加检测框后的画面显示到 Linux framebuffer。

项目目标不是实现完整 NVR，也不是完整 ONVIF SDK，而是提供一条清晰的嵌入式音视频 AI 链路：

```text
ONVIF 发现摄像头
  -> 获取 RTSP URL
  -> FFmpeg 拉流 / 解封装
  -> MPP 硬解码
  -> DRM_PRIME / NV12 视频帧
  -> RGA 预处理
  -> RKNN YOLO 推理
  -> RGA LCD 转换
  -> framebuffer 显示
```

## 功能概览

- 支持 ONVIF WS-Discovery 自动发现局域网摄像头。
- 支持手动指定 ONVIF Device Service URL。
- 支持跳过 ONVIF，直接传入 RTSP URL 调试后半段链路。
- 支持 RTSP over TCP / UDP 切换。
- 使用 FFmpeg 的 `h264_rkmpp` / `hevc_rkmpp` 解码器调用 Rockchip MPP 硬件解码。
- 优先请求 `AV_PIX_FMT_DRM_PRIME`，让解码结果以 dma-buf 形式暴露。
- 使用 RGA 将 NV12 视频帧转换为 YOLO 输入需要的 RGB888。
- 使用 RKNN Runtime 调用 NPU 执行 YOLO 推理。
- 使用 RGA 将视频帧转换为 LCD framebuffer 需要的 BGRA8888。
- 在 LCD 显示画面上叠加检测框、类别名和置信度。
- 使用单进程三线程结构，解码、推理、显示互相解耦。

## 总体架构

```text
main thread
  |
  |-- ONVIF 阶段，只在启动时执行
  |     |
  |     |-- WS-Discovery
  |     |-- GetCapabilities
  |     |-- GetProfiles
  |     |-- GetStreamUri
  |     `-- 得到最终 RTSP URL
  |
  `-- pipeline 阶段，启动三个工作线程
        |
        |-- decode_thread
        |     输入 : RTSP URL
        |     输出 : 最新一帧 AVFrame(DRM_PRIME/NV12)
        |
        |-- infer_thread
        |     输入 : 最新 AVFrame
        |     输出 : 最新 YOLO 检测结果
        |
        `-- display_thread
              输入 : 最新 AVFrame + 最新 YOLO 检测结果
              输出 : /dev/fb0 LCD 画面
```

线程之间不使用帧队列，而是只保留“最新帧”和“最新检测结果”。这样可以避免推理或显示变慢时旧帧堆积，更适合低延迟实时预览。

## 目录结构

```text
RC/
├── main.c                  # 程序入口、参数解析、ONVIF 调用、线程创建和回收
├── Makefile                # RK3566 Buildroot 交叉编译配置
├── build_rk3566.sh         # 一键编译脚本
├── onvif/                  # ONVIF 发现、认证、HTTP、XML 解析和 RTSP URL 获取
├── media/                  # FFmpeg 拉流、MPP 硬解码、RGA 图像预处理
├── infer/                  # RKNN YOLO 推理和 YOLO 后处理
├── display/                # LCD framebuffer 显示和检测框绘制
├── pipeline/               # 公共数据结构、线程同步和共享状态
└── model/                  # 示例 RKNN 模型和标签文件
```

## 数据流和格式

### 1. ONVIF 输入输出

输入：

- 摄像头账号和密码；
- `auto`、ONVIF Device Service URL，或者直接 RTSP URL。

输出：

- `rtsp://...` 视频流地址。

处理过程：

```text
WS-Discovery
  -> Device Service URL
  -> GetCapabilities
  -> Media Service URL
  -> GetProfiles
  -> profile token
  -> GetStreamUri
  -> RTSP URL
```

当前代码会优先选择 `profile_2`。很多摄像头会把 `profile_1` 作为主码流，把 `profile_2` 作为子码流。子码流分辨率低、码率低，更适合边缘端实时 AI 推理。

### 2. 解码输入输出

输入：

- RTSP URL；
- RTSP 传输方式：`tcp` 或 `udp`。

输出：

- FFmpeg `AVFrame`；
- 期望格式为 `AV_PIX_FMT_DRM_PRIME`；
- 底层图像通常是 NV12；
- 图像内存来自 MPP 解码器导出的 dma-buf。

核心路径：

```text
avformat_open_input()
  -> avformat_find_stream_info()
  -> avcodec_find_decoder_by_name("h264_rkmpp" / "hevc_rkmpp")
  -> avcodec_send_packet()
  -> avcodec_receive_frame()
  -> publish latest_frame
```

### 3. RGA 预处理输入输出

RGA 有两条输出路径。

YOLO 路径：

```text
输入 : AVFrame(DRM_PRIME/NV12)
输出 : RGB888, width = model input width, height = model input height
用途 : RKNN input tensor
```

LCD 路径：

```text
输入 : AVFrame(DRM_PRIME/NV12)
输出 : BGRA8888, width = framebuffer width, height = framebuffer height
用途 : /dev/fb0 显示
```

当前 LCD 显示采用：

```text
RGA -> 用户态 BGRA8888 buffer -> 画框/文字 -> pwrite(/dev/fb0)
```

源码中保留了 framebuffer mmap 实验入口，但默认安全构建不会启用。部分 fbdev 驱动的 `mmap(/dev/fb0)` 可能阻塞或刷新行为不稳定；如果需要真正解决撕裂，推荐后续改成 DRM/KMS 双缓冲和 page flip。

### 4. YOLO/RKNN 输入输出

输入：

- RKNN 模型文件；
- label 文本文件；
- RGA 输出的 RGB888 图像。

输出：

- 检测框；
- 类别 ID；
- 类别名称；
- 置信度；
- 检测框坐标。

坐标说明：

- YOLO 后处理输出的框坐标基于原始视频尺寸，例如 640x360；
- LCD 显示模块会根据旋转方向和 LCD 分辨率把检测框映射到屏幕坐标。

模型要求：

- 模型必须是 RKNN 格式；
- 模型输入建议为 NHWC RGB888；
- 当前代码主要面向 YOLOv5 类 raw-head 输出；
- 自训练模型需要同步对应的 label 文件；
- 如果模型输出结构变化，需要同步修改 `infer/yolo_postprocess_c.c`。

## 线程模型

当前版本是单进程三线程。

```text
decode_thread
  负责 RTSP 拉流和 MPP 解码。
  只发布最新一帧，不缓存历史帧。

infer_thread
  按 infer_interval 取最新帧。
  通过 RGA 转 RGB888。
  调用 RKNN/NPU 推理。
  发布最新检测结果。

display_thread
  取最新帧。
  通过 RGA 转 BGRA8888。
  叠加最新检测结果。
  写入 framebuffer。
```

共享状态位于 `pipeline/pipeline_types.h`：

- `pipeline_state_t`：全局线程共享状态；
- `shared_frame_t`：最新视频帧；
- `shared_result_t`：最新检测结果；
- `app_config_t`：启动参数和只读配置；
- `fb_ctx_t`：framebuffer 参数。

## 主要模块和 API

### main.c

职责：

- 解析命令行参数；
- 调用 ONVIF 模块获取 RTSP URL；
- 自动给 ONVIF 返回的 RTSP URL 补充 `user:password@`；
- 初始化 `pipeline_state_t`；
- 创建并等待三个工作线程。

主要内部函数：

- `resolve_rtsp_url()`：根据命令行选择 ONVIF 自动发现、手动 ONVIF URL 或直接 RTSP。
- `fill_app_config()`：生成全局配置。
- `run_pipeline()`：启动 decode / infer / display 三个线程。
- `prepare_framebuffer_display()`：尝试解除 fb blank，不主动 kill 显示服务。

### onvif/

职责：

- 构造 WS-Discovery Probe；
- 接收摄像头返回的 XAddr；
- 构造 SOAP 请求；
- 生成 WS-Security UsernameToken；
- 解析 XML；
- 获取 RTSP URL。

主要 API：

```c
void onvif_client_default_config(OnvifClientConfig *cfg);

int onvif_client_get_rtsp_url(const OnvifClientConfig *cfg,
                              OnvifClientResult *result);
```

输入：

- `manual_device_url`：为空则自动发现，不为空则跳过发现；
- `user/password`：摄像头账号密码；
- `use_wsse_digest`：是否使用 WSSE PasswordDigest；
- `discovery_timeout_ms`：自动发现等待时间。

输出：

- `device_service_url`；
- `media_service_url`；
- `profile_token`；
- `rtsp_url`。

### media/capture_decode.c

职责：

- 初始化 FFmpeg 网络模块；
- 打开 RTSP；
- 选择视频流；
- 选择 Rockchip MPP 硬解码器；
- 读取 packet；
- 解码成 AVFrame；
- 发布最新帧。

主要 API：

```c
void *decode_thread_main(void *arg);
```

输入：

- `app_config_t.rtsp_url`；
- `app_config_t.rtsp_transport`。

输出：

- `pipeline_state_t.latest_frame`。

### media/rga_preprocess.c

职责：

- 从 DRM_PRIME AVFrame 中读取 dma-buf 描述；
- 使用 RGA 导入 dma-buf；
- 对 NV12 图像进行缩放、旋转、颜色空间转换；
- 为 YOLO 和 LCD 分别生成目标格式。

主要 API：

```c
int rga_preprocess_yolo(const AVFrame *frame,
                        int dst_w,
                        int dst_h,
                        uint8_t *rgb888,
                        int *src_w_out,
                        int *src_h_out,
                        int64_t *cost_us);

int rga_render_lcd(const AVFrame *frame,
                   const fb_ctx_t *fb,
                   rotate_mode_t rotate,
                   uint8_t *bgra,
                   int64_t *cost_us);
```

输入：

- `AVFrame(DRM_PRIME/NV12)`。

输出：

- YOLO：RGB888；
- LCD：BGRA8888。

### infer/

职责：

- 加载 RKNN 模型；
- 查询输入输出 tensor 属性；
- 载入 label；
- 设置 RKNN input；
- 执行 `rknn_run()`；
- 获取输出 tensor；
- 做 YOLO decode、阈值过滤和 NMS。

主要 API：

```c
int yolo_runtime_init(yolo_runtime_t *rt,
                      const char *model_path,
                      const char *label_path);

int yolo_runtime_run(yolo_runtime_t *rt,
                     uint8_t *rgb888,
                     int src_w,
                     int src_h,
                     yolo_result_group_t *result,
                     int64_t *cost_us);

void yolo_runtime_deinit(yolo_runtime_t *rt);

void *infer_thread_main(void *arg);
```

输入：

- RKNN 模型；
- labels；
- RGB888 图像；
- 原始视频宽高。

输出：

- `yolo_result_group_t` 检测结果。

### display/

职责：

- 打开 `/dev/fb0`；
- 读取 LCD 宽高、bpp、line_length；
- 把 RGA 输出的 BGRA8888 写入 framebuffer；
- 把 YOLO 检测框和文本叠加到 LCD 画面上。

主要 API：

```c
int fb_open_device(fb_ctx_t *fb, const char *path, int need_mmap);
void fb_close_device(fb_ctx_t *fb);

void draw_result_boxes(uint8_t *bgra,
                       const fb_ctx_t *fb,
                       rotate_mode_t rotate,
                       int src_w,
                       int src_h,
                       const yolo_result_group_t *result);

void *display_thread_main(void *arg);
```

输入：

- 最新视频帧；
- 最新检测结果；
- LCD framebuffer 参数。

输出：

- `/dev/fb0` 上的可见画面。

### pipeline/

职责：

- 定义公共数据结构；
- 管理线程同步；
- 管理停止标志；
- 释放共享 AVFrame。

主要 API：

```c
int pipeline_state_init(pipeline_state_t *state);
void pipeline_state_deinit(pipeline_state_t *state);
void pipeline_request_stop(pipeline_state_t *state);
```

## 编译环境

依赖 RK3566 Buildroot SDK 里的交叉编译 sysroot。默认路径：

```text
/home/x/Desktop/rk3566/LinuxSDK
```

如果你的 SDK 路径不同，可以覆盖 `RK_SDK`：

```sh
cd RC
make RK_SDK=/path/to/LinuxSDK
```

或者使用脚本：

```sh
sh build_rk3566.sh
```

生成：

```text
bin/onvif_yolo_lcd
```

链接依赖：

- FFmpeg：`libavformat`、`libavcodec`、`libavutil`
- RGA：`librga`
- RKNN Runtime：`librknnrt`
- pthread
- math

## 部署到板卡

示例路径：

```sh
/userdata/Project/RC
```

ADB 上传示例：

```powershell
adb shell "mkdir -p /userdata/Project/RC/model"
adb push bin/onvif_yolo_lcd /userdata/Project/RC/
adb push model/person_yolov5n_640_raw_heads.rknn /userdata/Project/RC/model/
adb push model/person_labels.txt /userdata/Project/RC/model/
adb shell "chmod +x /userdata/Project/RC/onvif_yolo_lcd"
```

运行前设置库路径：

```sh
cd /userdata/Project/RC
export LD_LIBRARY_PATH=/rockchip_test/npu2/lib:/usr/lib:$LD_LIBRARY_PATH
```

如果你的系统已经把 `librknnrt.so` 放进 `/usr/lib`，可以不加 `/rockchip_test/npu2/lib`。

## 运行方式

参数格式：

```text
./onvif_yolo_lcd \
  [user] \
  [password] \
  [auto|device_service_url|rtsp_url] \
  [model] \
  [max_frames] \
  [none|clockwise|counterclockwise] \
  [infer_interval] \
  [labels] \
  [discovery_timeout_ms] \
  [tcp|udp] \
  [pwrite|mmap]
```

参数说明：

- `user/password`：摄像头账号密码。
- `auto`：自动发现摄像头。
- `device_service_url`：手动指定 ONVIF Device Service URL。
- `rtsp_url`：跳过 ONVIF，直接拉流。
- `model`：RKNN 模型路径。
- `max_frames`：最大处理帧数，`0` 表示一直运行。
- `rotate`：LCD 旋转方向。
- `infer_interval`：每隔多少帧推理一次。
- `labels`：标签文件路径。
- `discovery_timeout_ms`：WS-Discovery 等待时间。
- `tcp|udp`：RTSP/RTP 传输方式。
- `pwrite|mmap`：LCD 提交方式。默认推荐 `pwrite`。

自动发现并运行：

```sh
./onvif_yolo_lcd \
  <camera-user> \
  <camera-password> \
  auto \
  ./model/person_yolov5n_640_raw_heads.rknn \
  0 \
  clockwise \
  5 \
  ./model/person_labels.txt \
  5000 \
  udp \
  pwrite
```

手动指定 ONVIF Device Service URL：

```sh
./onvif_yolo_lcd \
  <camera-user> \
  <camera-password> \
  http://<camera-ip>:<onvif-port>/onvif/device_service \
  ./model/person_yolov5n_640_raw_heads.rknn \
  0 \
  clockwise \
  5 \
  ./model/person_labels.txt \
  5000 \
  udp \
  pwrite
```

直接指定 RTSP URL：

```sh
./onvif_yolo_lcd \
  unused \
  unused \
  rtsp://<camera-user>:<camera-password>@<camera-ip>:554/<stream-path> \
  ./model/person_yolov5n_640_raw_heads.rknn \
  0 \
  clockwise \
  5 \
  ./model/person_labels.txt \
  5000 \
  udp \
  pwrite
```

## 主码流和子码流

多数网络摄像头会提供多个 profile：

- 主码流：分辨率高、码率高，适合录像或高清预览；
- 子码流：分辨率低、码率低，适合 AI 检测和低延迟预览。

本项目默认优先选择 `profile_2`，因为它通常对应子码流。对于 RK3566 上的实时 YOLO + LCD 显示链路，子码流更容易获得稳定帧率和低延迟。

如果你的摄像头 profile 映射不同，需要修改 `onvif/onvif_client.c` 中 profile 选择逻辑，或者直接使用 RTSP URL 参数跳过 ONVIF。

## TCP 和 UDP 选择

- `tcp`：更可靠，包按序到达，弱网络下更稳，但可能增加延迟。
- `udp`：延迟更低，不保证可靠传输，网络抖动时可能丢包或花屏。

实时显示和 AI 预览场景通常优先尝试 `udp`；录像或稳定性优先场景可以使用 `tcp`。

## LCD 显示说明

当前显示路径基于 Linux framebuffer：

```text
RGA -> BGRA8888 user buffer -> draw boxes/text -> pwrite(/dev/fb0)
```

要求：

- `/dev/fb0` 可写；
- framebuffer 为 32bpp；
- LCD 显示方向可通过 `clockwise` / `counterclockwise` / `none` 调整。

如果系统正在运行 weston/wayland/drm 显示服务，可能会覆盖 framebuffer 输出。程序内部不会自动 kill 显示服务，避免某些系统状态下命令阻塞。需要时请在自己的启动脚本中处理。

## 开源注意事项

- 不要在源码或 README 中提交真实摄像头密码。
- `rc_test_log.dat` 是本地调试记录，不建议公开。
- `bin/` 和 `obj/` 是构建产物，不建议提交。
- 如果 RKNN 模型来自第三方或训练数据受限，请确认模型和标签文件的发布许可。
- 如果要发布完整工程，建议补充 LICENSE，并说明依赖的 Rockchip SDK、FFmpeg、RGA、RKNN Runtime 的授权边界。

## 后续优化方向

- 使用 DRM/KMS dumb buffer + page flip 代替 framebuffer pwrite，降低撕裂。
- 把采集、推理、推流、看门狗拆成多进程，通过共享内存或 dma-buf 传递帧。
- 增加 RTSP/RTMP/GB28181 等网络推流模块。
- 增加模型热更新、配置文件、运行状态上报。
- 增加硬件看门狗和 systemd 管理。
- 支持更多 ONVIF 能力，例如云台控制、事件订阅、码流参数配置。
