/*
 * onvif_http.h
 *
 * 最小 HTTP POST 客户端。
 * 用于向 ONVIF Device Service / Media Service 发送 SOAP XML。
 */

#ifndef ONVIF_HTTP_H
#define ONVIF_HTTP_H

#include <stddef.h>

typedef struct HttpUrl {
    char host[128];
    int port;
    char path[256];
    int https;
} HttpUrl;

int http_parse_url(const char *url, HttpUrl *out);

int http_post_soap(const char *url,
                   const char *soap_action,
                   const char *soap_body,
                   int timeout_sec,
                   size_t max_response,
                   char **out_body,
                   int *out_status);

void http_free_body(char *body);

#endif

