/*
 * 03onvif_discovery 默认配置。
 *
 * 这一节的目标是：不提前知道摄像头 IP，只要摄像头和板卡在同一局域网，
 * 程序就通过 WS-Discovery 自动找到 ONVIF Device Service 地址，
 * 然后继续通过 ONVIF Media Service 获取 RTSP URL。
 */

#ifndef ONVIF_CONFIG_H
#define ONVIF_CONFIG_H

/* WS-Discovery 标准组播地址，ONVIF 设备通常会监听这个地址。 */
#define ONVIF_DISCOVERY_MCAST_IP "239.255.255.250"

/* WS-Discovery 标准端口。 */
#define ONVIF_DISCOVERY_PORT 3702

/* 搜索摄像头的等待时间，单位 ms。 */
#define ONVIF_DISCOVERY_TIMEOUT_MS 3000

/* 最多记录几个发现到的设备。 */
#define ONVIF_MAX_DEVICES 8

/* ONVIF 默认用户名和密码。开源版本不内置真实密码，推荐运行时用命令行传入。 */
#define ONVIF_CAMERA_USER "admin"
#define ONVIF_CAMERA_PASS ""

/*
 * 如果你暂时不想自动发现，或者某些摄像头不响应 WS-Discovery，
 * 可以在这里写死 Device Service URL，例如：
 *   "http://192.168.1.64/onvif/device_service"
 *
 * 留空则优先走自动发现。
 */
#define ONVIF_MANUAL_DEVICE_URL ""

/* HTTP 连接、发送、接收超时，单位秒。 */
#define ONVIF_HTTP_TIMEOUT_SEC 5

/* HTTP 响应缓存上限，普通 ONVIF XML 很小，256 KB 足够。 */
#define ONVIF_HTTP_MAX_RESPONSE (256 * 1024)

/* 是否启用 WS-Security UsernameToken Digest。大多数 ONVIF 摄像头需要它。 */
#define ONVIF_USE_WSSE_DIGEST 1

#endif
