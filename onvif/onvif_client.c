/*
 * onvif_client.c
 *
 * 自动发现版 ONVIF 客户端。
 *
 * 总流程：
 *   WS-Discovery Probe
 *     -> Device Service XAddr
 *     -> GetCapabilities
 *     -> Media Service XAddr
 *     -> GetProfiles
 *     -> Profile Token
 *     -> GetStreamUri
 *     -> RTSP URL
 */

#include "onvif_client.h"
#include "onvif_auth.h"
#include "onvif_discovery.h"
#include "onvif_http.h"
#include "onvif_xml.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void safe_copy(char *dst, size_t dst_size, const char *src)
{
    size_t len;

    if (!dst || dst_size == 0) {
        return;
    }

    if (!src) {
        dst[0] = '\0';
        return;
    }

    len = strlen(src);
    if (len >= dst_size) {
        len = dst_size - 1;
    }

    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void xml_escape(const char *in, char *out, size_t out_size)
{
    size_t used = 0;

    if (!out || out_size == 0) {
        return;
    }

    out[0] = '\0';

    if (!in) {
        return;
    }

    while (*in && used + 1 < out_size) {
        const char *rep = NULL;

        if (*in == '&') {
            rep = "&amp;";
        } else if (*in == '<') {
            rep = "&lt;";
        } else if (*in == '>') {
            rep = "&gt;";
        } else if (*in == '"') {
            rep = "&quot;";
        } else if (*in == '\'') {
            rep = "&apos;";
        }

        if (rep) {
            size_t n = strlen(rep);
            if (used + n >= out_size) {
                break;
            }
            memcpy(out + used, rep, n);
            used += n;
        } else {
            out[used++] = *in;
        }

        in++;
    }

    out[used] = '\0';
}

static int is_xml_tag_boundary(char c)
{
    return c == '>' || c == '/' || c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static const char *find_media_capability_tag(const char *resp)
{
    const char *p;

    if (!resp) {
        return NULL;
    }

    p = resp;
    while ((p = strstr(p, "Media")) != NULL) {
        const char *lt = p;

        while (lt > resp && *lt != '<' && *lt != '>' && *lt != '\r' && *lt != '\n') {
            lt--;
        }

        if (*lt == '<' &&
            (p == lt + 1 || *(p - 1) == ':') &&
            is_xml_tag_boundary(p[5])) {
            return lt;
        }

        p += 5;
    }

    return NULL;
}

static int extract_media_xaddr(const char *resp, char *out, size_t out_size)
{
    const char *media_tag = find_media_capability_tag(resp);

    if (media_tag &&
        xml_get_first_tag_text(media_tag, "XAddr", out, out_size) == 0) {
        return 0;
    }

    return xml_get_first_tag_text(resp, "XAddr", out, out_size);
}

static int build_envelope(const char *security_header,
                          const char *body_content,
                          char *out,
                          size_t out_size)
{
    const char *header_begin;
    const char *header_end;

    if (security_header && security_header[0]) {
        header_begin = "<s:Header>";
        header_end = "</s:Header>";
    } else {
        header_begin = "<s:Header/>";
        header_end = "";
        security_header = "";
    }

    snprintf(out,
             out_size,
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
             "<s:Envelope "
             "xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
             "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
             "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" "
             "xmlns:tt=\"http://www.onvif.org/ver10/schema\" "
             "xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/"
             "oasis-200401-wss-wssecurity-secext-1.0.xsd\" "
             "xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/"
             "oasis-200401-wss-wssecurity-utility-1.0.xsd\">"
             "%s%s%s"
             "<s:Body>%s</s:Body>"
             "</s:Envelope>",
             header_begin,
             security_header,
             header_end,
             body_content);

    return 0;
}

static int build_authenticated_envelope(const OnvifClientConfig *cfg,
                                        const char *body_content,
                                        char *out,
                                        size_t out_size)
{
    char security[2048];

    if (wsse_build_security_header(cfg->user,
                                   cfg->password,
                                   cfg->use_wsse_digest,
                                   security,
                                   sizeof(security)) < 0) {
        fprintf(stderr, "build WSSE header failed\n");
        return -1;
    }

    return build_envelope(security, body_content, out, out_size);
}

static int soap_post_and_get_body(const char *url,
                                  const char *action,
                                  const char *envelope,
                                  int timeout_sec,
                                  char **out_body)
{
    int status = 0;

    if (http_post_soap(url,
                       action,
                       envelope,
                       timeout_sec,
                       ONVIF_HTTP_MAX_RESPONSE,
                       out_body,
                       &status) < 0) {
        if (*out_body) {
            fprintf(stderr, "SOAP fault or HTTP error body:\n%.1000s\n", *out_body);
        }
        return -1;
    }

    return 0;
}

static int call_get_capabilities(const OnvifClientConfig *cfg,
                                 OnvifClientResult *result)
{
    char envelope[4096];
    char *resp = NULL;
    char xaddr[512];
    const char *body =
        "<tds:GetCapabilities>"
        "<tds:Category>Media</tds:Category>"
        "</tds:GetCapabilities>";

    if (build_authenticated_envelope(cfg, body, envelope, sizeof(envelope)) < 0) {
        return -1;
    }

    printf("ONVIF: GetCapabilities -> %s\n", result->device_service_url);

    if (soap_post_and_get_body(result->device_service_url,
                               "http://www.onvif.org/ver10/device/wsdl/GetCapabilities",
                               envelope,
                               cfg->http_timeout_sec,
                               &resp) < 0) {
        http_free_body(resp);
        return -1;
    }

    if (extract_media_xaddr(resp, xaddr, sizeof(xaddr)) < 0) {
        fprintf(stderr, "GetCapabilities OK, but Media XAddr not found\n");
        http_free_body(resp);
        return -1;
    }

    safe_copy(result->media_service_url, sizeof(result->media_service_url), xaddr);
    printf("ONVIF: Media XAddr = %s\n", result->media_service_url);

    http_free_body(resp);
    return 0;
}

static int call_get_profiles(const OnvifClientConfig *cfg,
                             OnvifClientResult *result)
{
    char envelope[4096];
    char *resp = NULL;
    const char *body = "<trt:GetProfiles/>";
    const char *preferred_profile = "profile_2";

    if (build_authenticated_envelope(cfg, body, envelope, sizeof(envelope)) < 0) {
        return -1;
    }

    printf("ONVIF: GetProfiles -> %s\n", result->media_service_url);

    if (soap_post_and_get_body(result->media_service_url,
                               "http://www.onvif.org/ver10/media/wsdl/GetProfiles",
                               envelope,
                               cfg->http_timeout_sec,
                               &resp) < 0) {
        http_free_body(resp);
        return -1;
    }

    if (strstr(resp, "token=\"profile_2\"") ||
        strstr(resp, "token='profile_2'")) {
        safe_copy(result->profile_token,
                  sizeof(result->profile_token),
                  preferred_profile);
        printf("ONVIF: prefer sub-stream profile token = %s\n",
               result->profile_token);
    } else if (xml_get_first_profile_token(resp,
                                           result->profile_token,
                                           sizeof(result->profile_token)) < 0) {
        fprintf(stderr, "GetProfiles OK, but profile token not found\n");
        http_free_body(resp);
        return -1;
    }

    printf("ONVIF: selected profile token = %s\n", result->profile_token);

    http_free_body(resp);
    return 0;
}

static int call_get_stream_uri(const OnvifClientConfig *cfg,
                               OnvifClientResult *result)
{
    char token_escaped[256];
    char body[1024];
    char envelope[4096];
    char *resp = NULL;

    xml_escape(result->profile_token, token_escaped, sizeof(token_escaped));

    snprintf(body,
             sizeof(body),
             "<trt:GetStreamUri>"
             "<trt:StreamSetup>"
             "<tt:Stream>RTP-Unicast</tt:Stream>"
             "<tt:Transport>"
             "<tt:Protocol>RTSP</tt:Protocol>"
             "</tt:Transport>"
             "</trt:StreamSetup>"
             "<trt:ProfileToken>%s</trt:ProfileToken>"
             "</trt:GetStreamUri>",
             token_escaped);

    if (build_authenticated_envelope(cfg, body, envelope, sizeof(envelope)) < 0) {
        return -1;
    }

    printf("ONVIF: GetStreamUri -> %s\n", result->media_service_url);

    if (soap_post_and_get_body(result->media_service_url,
                               "http://www.onvif.org/ver10/media/wsdl/GetStreamUri",
                               envelope,
                               cfg->http_timeout_sec,
                               &resp) < 0) {
        http_free_body(resp);
        return -1;
    }

    if (xml_get_first_tag_text(resp, "Uri", result->rtsp_url, sizeof(result->rtsp_url)) < 0) {
        fprintf(stderr, "GetStreamUri OK, but Uri not found\n");
        http_free_body(resp);
        return -1;
    }

    printf("ONVIF: RTSP URL = %s\n", result->rtsp_url);

    http_free_body(resp);
    return 0;
}

static int resolve_device_service_url(const OnvifClientConfig *cfg,
                                      OnvifClientResult *result)
{
    OnvifDiscoveredDevice devices[ONVIF_MAX_DEVICES];
    int count;

    if (cfg->manual_device_url && cfg->manual_device_url[0]) {
        safe_copy(result->device_service_url,
                  sizeof(result->device_service_url),
                  cfg->manual_device_url);
        printf("ONVIF: use manual Device Service URL = %s\n",
               result->device_service_url);
        return 0;
    }

    count = onvif_discover_devices(devices,
                                   ONVIF_MAX_DEVICES,
                                   cfg->discovery_timeout_ms);
    if (count <= 0) {
        fprintf(stderr, "Discovery failed: no ONVIF device found\n");
        return -1;
    }

    safe_copy(result->device_service_url,
              sizeof(result->device_service_url),
              devices[0].xaddr);
    printf("ONVIF: select discovered Device Service URL = %s\n",
           result->device_service_url);
    return 0;
}

void onvif_client_default_config(OnvifClientConfig *cfg)
{
    if (!cfg) {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->manual_device_url = ONVIF_MANUAL_DEVICE_URL;
    cfg->user = ONVIF_CAMERA_USER;
    cfg->password = ONVIF_CAMERA_PASS;
    cfg->use_wsse_digest = ONVIF_USE_WSSE_DIGEST;
    cfg->discovery_timeout_ms = ONVIF_DISCOVERY_TIMEOUT_MS;
    cfg->http_timeout_sec = ONVIF_HTTP_TIMEOUT_SEC;
}

int onvif_client_get_rtsp_url(const OnvifClientConfig *cfg,
                              OnvifClientResult *result)
{
    if (!cfg || !result) {
        return -1;
    }

    memset(result, 0, sizeof(*result));

    if (resolve_device_service_url(cfg, result) < 0) {
        return -1;
    }

    if (call_get_capabilities(cfg, result) < 0) {
        return -1;
    }

    if (call_get_profiles(cfg, result) < 0) {
        return -1;
    }

    if (call_get_stream_uri(cfg, result) < 0) {
        return -1;
    }

    return 0;
}
