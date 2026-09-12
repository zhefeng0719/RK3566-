/*
 * onvif_discovery.h
 *
 * WS-Discovery 自动发现 ONVIF 设备。
 */

#ifndef ONVIF_DISCOVERY_H
#define ONVIF_DISCOVERY_H

#include "onvif_config.h"

typedef struct OnvifDiscoveredDevice {
    char xaddr[256];
} OnvifDiscoveredDevice;

int onvif_discover_devices(OnvifDiscoveredDevice *devices,
                           int max_devices,
                           int timeout_ms);

#endif

