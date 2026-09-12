/*
 * onvif_http.c
 *
 * 最小 HTTP/1.1 客户端。
 * 只实现本项目需要的 POST SOAP：
 *   - 解析 http://host:port/path
 *   - TCP connect
 *   - 发送 HTTP POST
 *   - 读取响应 body
 */

#include "onvif_http.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

static int starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

int http_parse_url(const char *url, HttpUrl *out)
{
    const char *p;
    const char *host_begin;
    const char *host_end;
    const char *path_begin;
    const char *port_begin = NULL;
    size_t host_len;
    size_t path_len;

    if (!url || !out) {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    if (starts_with(url, "http://")) {
        out->https = 0;
        out->port = 80;
        host_begin = url + strlen("http://");
    } else if (starts_with(url, "https://")) {
        out->https = 1;
        out->port = 443;
        host_begin = url + strlen("https://");
    } else {
        return -1;
    }

    path_begin = strchr(host_begin, '/');
    if (!path_begin) {
        path_begin = "/";
        host_end = url + strlen(url);
    } else {
        host_end = path_begin;
    }

    p = host_begin;
    while (p < host_end) {
        if (*p == ':') {
            port_begin = p + 1;
            host_end = p;
            break;
        }
        p++;
    }

    host_len = (size_t)(host_end - host_begin);
    if (host_len == 0 || host_len >= sizeof(out->host)) {
        return -1;
    }

    memcpy(out->host, host_begin, host_len);
    out->host[host_len] = '\0';

    if (port_begin) {
        out->port = atoi(port_begin);
        if (out->port <= 0 || out->port > 65535) {
            return -1;
        }
    }

    path_len = strlen(path_begin);
    if (path_len == 0 || path_len >= sizeof(out->path)) {
        return -1;
    }

    memcpy(out->path, path_begin, path_len + 1);
    return 0;
}

static int set_timeouts(int fd, int timeout_sec)
{
    struct timeval tv;

    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        return -1;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
        return -1;
    }

    return 0;
}

static int connect_with_timeout(const char *host, int port, int timeout_sec)
{
    char port_text[16];
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *rp;
    int fd = -1;

    snprintf(port_text, sizeof(port_text), "%d", port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_text, &hints, &res) != 0) {
        return -1;
    }

    for (rp = res; rp; rp = rp->ai_next) {
        int flags;
        int err;
        socklen_t err_len;
        fd_set wfds;
        struct timeval tv;

        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) {
            continue;
        }

        flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(fd);
            fd = -1;
            continue;
        }

        err = connect(fd, rp->ai_addr, rp->ai_addrlen);
        if (err < 0 && errno != EINPROGRESS) {
            close(fd);
            fd = -1;
            continue;
        }

        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        tv.tv_sec = timeout_sec;
        tv.tv_usec = 0;

        err = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (err <= 0) {
            close(fd);
            fd = -1;
            continue;
        }

        err = 0;
        err_len = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &err_len) < 0 || err != 0) {
            close(fd);
            fd = -1;
            continue;
        }

        if (fcntl(fd, F_SETFL, flags) < 0 ||
            set_timeouts(fd, timeout_sec) < 0) {
            close(fd);
            fd = -1;
            continue;
        }
        break;
    }

    freeaddrinfo(res);
    return fd;
}

static int send_all(int fd, const char *buf, size_t len)
{
    size_t done = 0;

    while (done < len) {
        ssize_t n = send(fd, buf + done, len - done, 0);
        if (n <= 0) {
            return -1;
        }
        done += (size_t)n;
    }

    return 0;
}

static char *find_header_body_split(char *resp)
{
    char *p = strstr(resp, "\r\n\r\n");

    if (p) {
        *p = '\0';
        return p + 4;
    }

    p = strstr(resp, "\n\n");
    if (p) {
        *p = '\0';
        return p + 2;
    }

    return NULL;
}

static int str_contains_case(const char *haystack, const char *needle)
{
    size_t needle_len = strlen(needle);
    const char *p;

    if (needle_len == 0) {
        return 1;
    }

    for (p = haystack; *p; p++) {
        size_t i;
        for (i = 0; i < needle_len; i++) {
            if (!p[i]) {
                return 0;
            }
            if (tolower((unsigned char)p[i]) != tolower((unsigned char)needle[i])) {
                break;
            }
        }
        if (i == needle_len) {
            return 1;
        }
    }

    return 0;
}

static int parse_status_code(const char *headers)
{
    const char *space;

    if (!headers || strncmp(headers, "HTTP/", 5) != 0) {
        return 0;
    }

    space = strchr(headers, ' ');
    if (!space) {
        return 0;
    }

    return atoi(space + 1);
}

static char *decode_chunked_body(const char *body, size_t *out_len)
{
    const char *p = body;
    char *out;
    size_t cap = strlen(body) + 1;
    size_t used = 0;

    out = malloc(cap);
    if (!out) {
        return NULL;
    }

    while (*p) {
        unsigned long chunk_len;
        char *endptr;
        const char *data;

        chunk_len = strtoul(p, &endptr, 16);
        if (endptr == p) {
            break;
        }

        data = strstr(endptr, "\r\n");
        if (!data) {
            break;
        }
        data += 2;

        if (chunk_len == 0) {
            break;
        }

        if (used + chunk_len + 1 > cap) {
            free(out);
            return NULL;
        }

        memcpy(out + used, data, chunk_len);
        used += chunk_len;
        p = data + chunk_len;

        if (p[0] == '\r' && p[1] == '\n') {
            p += 2;
        }
    }

    out[used] = '\0';
    if (out_len) {
        *out_len = used;
    }
    return out;
}

static char *read_response(int fd, size_t max_response)
{
    char *buf;
    size_t used = 0;

    buf = malloc(max_response + 1);
    if (!buf) {
        return NULL;
    }

    while (used < max_response) {
        ssize_t n = recv(fd, buf + used, max_response - used, 0);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (used > 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                break;
            }
            free(buf);
            return NULL;
        }
        used += (size_t)n;
    }

    buf[used] = '\0';
    return buf;
}

int http_post_soap(const char *url,
                   const char *soap_action,
                   const char *soap_body,
                   int timeout_sec,
                   size_t max_response,
                   char **out_body,
                   int *out_status)
{
    HttpUrl parsed;
    int fd = -1;
    char *request = NULL;
    char *response = NULL;
    char *body;
    size_t req_len;
    int status;
    int ret = -1;

    if (out_body) {
        *out_body = NULL;
    }
    if (out_status) {
        *out_status = 0;
    }

    if (!url || !soap_body || !out_body) {
        return -1;
    }

    if (http_parse_url(url, &parsed) < 0) {
        fprintf(stderr, "http_parse_url failed: %s\n", url);
        return -1;
    }

    if (parsed.https) {
        fprintf(stderr, "HTTPS is not supported by this minimal client: %s\n", url);
        return -1;
    }

    fd = connect_with_timeout(parsed.host, parsed.port, timeout_sec);
    if (fd < 0) {
        perror("connect");
        return -1;
    }

    req_len = strlen(soap_body) + strlen(parsed.path) + strlen(parsed.host) +
              strlen(soap_action ? soap_action : "") + 512;
    request = malloc(req_len);
    if (!request) {
        goto out;
    }

    snprintf(request,
             req_len,
             "POST %s HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "User-Agent: rk3566-onvif-minimal/1.0\r\n"
             "Content-Type: application/soap+xml; charset=utf-8; action=\"%s\"\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "\r\n"
             "%s",
             parsed.path,
             parsed.host,
             parsed.port,
             soap_action ? soap_action : "",
             strlen(soap_body),
             soap_body);

    if (send_all(fd, request, strlen(request)) < 0) {
        perror("send");
        goto out;
    }

    response = read_response(fd, max_response);
    if (!response) {
        perror("recv");
        goto out;
    }

    body = find_header_body_split(response);
    if (!body) {
        fprintf(stderr, "invalid HTTP response\n");
        goto out;
    }

    status = parse_status_code(response);
    if (out_status) {
        *out_status = status;
    }

    if (str_contains_case(response, "transfer-encoding: chunked")) {
        size_t body_len = 0;
        *out_body = decode_chunked_body(body, &body_len);
        (void)body_len;
    } else {
        *out_body = strdup(body);
    }

    if (!*out_body) {
        goto out;
    }

    if (status < 200 || status >= 300) {
        fprintf(stderr, "HTTP status %d from %s\n", status, url);
        goto out;
    }

    ret = 0;

out:
    if (fd >= 0) {
        close(fd);
    }
    free(request);
    free(response);
    return ret;
}

void http_free_body(char *body)
{
    free(body);
}
