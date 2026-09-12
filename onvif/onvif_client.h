/*
 * onvif_client.h
 *
 * 03 自动发现版对外接口。
 */

#ifndef ONVIF_CLIENT_H
#define ONVIF_CLIENT_H

#include "onvif_config.h"

typedef struct OnvifClientConfig {
    const char *manual_device_url;
    const char *user;
    const char *password;
    int use_wsse_digest;
    int discovery_timeout_ms;
    int http_timeout_sec;
} OnvifClientConfig;

typedef struct OnvifClientResult {
    char device_service_url[256];
    char media_service_url[256];
    char profile_token[128];
    char rtsp_url[512];
} OnvifClientResult;

void onvif_client_default_config(OnvifClientConfig *cfg);

int onvif_client_get_rtsp_url(const OnvifClientConfig *cfg,
                              OnvifClientResult *result);

#endif

