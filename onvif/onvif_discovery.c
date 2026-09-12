/*
 * onvif_discovery.c
 *
 * WS-Discovery 自动发现。
 * 原理：
 *   1. 往 239.255.255.250:3702 发送 Probe 组播包。
 *   2. ONVIF 摄像头收到后，用 UDP 单播返回 ProbeMatches。
 *   3. ProbeMatches 里的 XAddrs 字段包含 Device Service 地址。
 */

#include "onvif_discovery.h"
#include "onvif_xml.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void make_uuid(char *out, size_t out_size)
{
    unsigned int a;
    unsigned int b;
    unsigned int c;
    unsigned int d;

    srand((unsigned int)(time(NULL) ^ getpid()));
    a = (unsigned int)rand();
    b = (unsigned int)rand();
    c = (unsigned int)rand();
    d = (unsigned int)rand();

    snprintf(out,
             out_size,
             "uuid:%08x-%04x-%04x-%04x-%08x%04x",
             a,
             b & 0xffffU,
             c & 0xffffU,
             d & 0xffffU,
             a ^ c,
             b & 0xffffU);
}

static int build_probe_message(char *out, size_t out_size)
{
    char uuid[80];

    make_uuid(uuid, sizeof(uuid));

    snprintf(out,
             out_size,
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
             "<e:Envelope "
             "xmlns:e=\"http://www.w3.org/2003/05/soap-envelope\" "
             "xmlns:w=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
             "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
             "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
             "<e:Header>"
             "<w:MessageID>%s</w:MessageID>"
             "<w:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</w:To>"
             "<w:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</w:Action>"
             "</e:Header>"
             "<e:Body>"
             "<d:Probe>"
             "<d:Types>dn:NetworkVideoTransmitter</d:Types>"
             "</d:Probe>"
             "</e:Body>"
             "</e:Envelope>",
             uuid);

    return 0;
}

static int already_exists(const OnvifDiscoveredDevice *devices,
                          int count,
                          const char *xaddr)
{
    int i;

    for (i = 0; i < count; i++) {
        if (strcmp(devices[i].xaddr, xaddr) == 0) {
            return 1;
        }
    }

    return 0;
}

static int parse_probe_match_xaddr(const char *xml, char *out, size_t out_size)
{
    char xaddrs[1024];

    if (xml_get_first_tag_text(xml, "XAddrs", xaddrs, sizeof(xaddrs)) < 0) {
        return -1;
    }

    return xml_get_first_http_url_from_text(xaddrs, out, out_size);
}

int onvif_discover_devices(OnvifDiscoveredDevice *devices,
                           int max_devices,
                           int timeout_ms)
{
    int fd;
    int count = 0;
    char probe[2048];
    struct sockaddr_in dst;
    long long deadline;
    int ttl = 2;

    if (!devices || max_devices <= 0) {
        return -1;
    }

    memset(devices, 0, sizeof(devices[0]) * (size_t)max_devices);
    build_probe_message(probe, sizeof(probe));

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(ONVIF_DISCOVERY_PORT);
    inet_pton(AF_INET, ONVIF_DISCOVERY_MCAST_IP, &dst.sin_addr);

    printf("Discovery: send Probe -> %s:%d\n",
           ONVIF_DISCOVERY_MCAST_IP,
           ONVIF_DISCOVERY_PORT);

    if (sendto(fd,
               probe,
               strlen(probe),
               0,
               (struct sockaddr *)&dst,
               sizeof(dst)) < 0) {
        perror("sendto");
        close(fd);
        return -1;
    }

    deadline = now_ms() + timeout_ms;

    while (now_ms() < deadline && count < max_devices) {
        long long remain_ms = deadline - now_ms();
        struct timeval tv;
        fd_set rfds;
        char buf[8192];
        ssize_t n;

        if (remain_ms < 0) {
            remain_ms = 0;
        }

        tv.tv_sec = (time_t)(remain_ms / 1000);
        tv.tv_usec = (suseconds_t)((remain_ms % 1000) * 1000);

        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);

        if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
            break;
        }

        n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            continue;
        }

        buf[n] = '\0';

        if (parse_probe_match_xaddr(buf,
                                    devices[count].xaddr,
                                    sizeof(devices[count].xaddr)) == 0) {
            if (!already_exists(devices, count, devices[count].xaddr)) {
                printf("Discovery: found device[%d] XAddr=%s\n",
                       count,
                       devices[count].xaddr);
                count++;
            }
        }
    }

    close(fd);
    return count;
}

