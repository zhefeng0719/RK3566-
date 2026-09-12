/*
 * onvif_xml.c
 *
 * 轻量 XML 提取工具。
 * 注意：它不是通用 XML 解析器，只面向本节 ONVIF 最小链路。
 */

#include "onvif_xml.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int local_name_equal(const char *name_begin,
                            const char *name_end,
                            const char *local_name)
{
    const char *colon;
    size_t n;

    colon = memchr(name_begin, ':', (size_t)(name_end - name_begin));
    if (colon) {
        name_begin = colon + 1;
    }

    n = (size_t)(name_end - name_begin);
    return strlen(local_name) == n && strncmp(name_begin, local_name, n) == 0;
}

static int parse_tag_name(const char *lt,
                          const char **name_begin,
                          const char **name_end)
{
    const char *p;

    if (!lt || lt[0] != '<') {
        return -1;
    }

    p = lt + 1;
    if (*p == '/' || *p == '?' || *p == '!') {
        return -1;
    }

    *name_begin = p;
    while (*p && !isspace((unsigned char)*p) && *p != '>' && *p != '/') {
        p++;
    }
    *name_end = p;

    return *name_end > *name_begin ? 0 : -1;
}

static const char *find_open_tag(const char *xml, const char *local_name)
{
    const char *p = xml;

    while ((p = strchr(p, '<')) != NULL) {
        const char *name_begin;
        const char *name_end;

        if (parse_tag_name(p, &name_begin, &name_end) == 0 &&
            local_name_equal(name_begin, name_end, local_name)) {
            return p;
        }

        p++;
    }

    return NULL;
}

static const char *find_matching_close_tag(const char *content_begin,
                                           const char *local_name)
{
    const char *p = content_begin;

    while ((p = strstr(p, "</")) != NULL) {
        const char *name_begin = p + 2;
        const char *name_end = name_begin;

        while (*name_end && !isspace((unsigned char)*name_end) && *name_end != '>') {
            name_end++;
        }

        if (local_name_equal(name_begin, name_end, local_name)) {
            return p;
        }

        p += 2;
    }

    return NULL;
}

static void copy_trimmed(char *out,
                         size_t out_size,
                         const char *begin,
                         const char *end)
{
    size_t len;

    if (!out || out_size == 0) {
        return;
    }

    while (begin < end && isspace((unsigned char)*begin)) {
        begin++;
    }

    while (end > begin && isspace((unsigned char)*(end - 1))) {
        end--;
    }

    len = (size_t)(end - begin);
    if (len >= out_size) {
        len = out_size - 1;
    }

    memcpy(out, begin, len);
    out[len] = '\0';
}

int xml_get_first_tag_text(const char *xml,
                           const char *local_name,
                           char *out,
                           size_t out_size)
{
    const char *open_tag;
    const char *open_end;
    const char *text_begin;
    const char *close_tag;

    if (!xml || !local_name || !out || out_size == 0) {
        return -1;
    }

    out[0] = '\0';

    open_tag = find_open_tag(xml, local_name);
    if (!open_tag) {
        return -1;
    }

    open_end = strchr(open_tag, '>');
    if (!open_end) {
        return -1;
    }

    if (open_end > open_tag && *(open_end - 1) == '/') {
        return -1;
    }

    text_begin = open_end + 1;
    close_tag = find_matching_close_tag(text_begin, local_name);
    if (!close_tag) {
        return -1;
    }

    copy_trimmed(out, out_size, text_begin, close_tag);
    xml_unescape_inplace(out);
    return out[0] ? 0 : -1;
}

int xml_get_first_http_url_from_text(const char *text,
                                     char *out,
                                     size_t out_size)
{
    const char *p;
    const char *end;

    if (!text || !out || out_size == 0) {
        return -1;
    }

    out[0] = '\0';

    p = strstr(text, "http://");
    if (!p) {
        p = strstr(text, "https://");
    }
    if (!p) {
        return -1;
    }

    end = p;
    while (*end && !isspace((unsigned char)*end) && *end != '<' && *end != '"') {
        end++;
    }

    copy_trimmed(out, out_size, p, end);
    xml_unescape_inplace(out);
    return out[0] ? 0 : -1;
}

static int extract_attr_from_tag(const char *tag_begin,
                                 const char *tag_end,
                                 const char *attr_name,
                                 char *out,
                                 size_t out_size)
{
    const char *p = tag_begin;
    size_t attr_len = strlen(attr_name);

    while (p && p < tag_end) {
        const char *hit = strstr(p, attr_name);
        const char *value_begin;
        const char *value_end;
        char quote;

        if (!hit || hit >= tag_end) {
            break;
        }

        if (hit > tag_begin) {
            char before = *(hit - 1);
            if (!isspace((unsigned char)before) && before != '<') {
                p = hit + attr_len;
                continue;
            }
        }

        value_begin = hit + attr_len;
        while (value_begin < tag_end && isspace((unsigned char)*value_begin)) {
            value_begin++;
        }

        if (value_begin >= tag_end || *value_begin != '=') {
            p = hit + attr_len;
            continue;
        }

        value_begin++;
        while (value_begin < tag_end && isspace((unsigned char)*value_begin)) {
            value_begin++;
        }

        if (value_begin >= tag_end || (*value_begin != '"' && *value_begin != '\'')) {
            p = hit + attr_len;
            continue;
        }

        quote = *value_begin++;
        value_end = value_begin;
        while (value_end < tag_end && *value_end != quote) {
            value_end++;
        }

        if (value_end >= tag_end) {
            return -1;
        }

        copy_trimmed(out, out_size, value_begin, value_end);
        xml_unescape_inplace(out);
        return out[0] ? 0 : -1;
    }

    return -1;
}

int xml_get_first_profile_token(const char *xml,
                                char *out,
                                size_t out_size)
{
    const char *tag;
    const char *tag_end;

    if (!xml || !out || out_size == 0) {
        return -1;
    }

    out[0] = '\0';
    tag = find_open_tag(xml, "Profiles");
    if (!tag) {
        return -1;
    }

    tag_end = strchr(tag, '>');
    if (!tag_end) {
        return -1;
    }

    return extract_attr_from_tag(tag, tag_end, "token", out, out_size);
}

void xml_unescape_inplace(char *s)
{
    char *r;
    char *w;

    if (!s) {
        return;
    }

    r = s;
    w = s;

    while (*r) {
        if (strncmp(r, "&amp;", 5) == 0) {
            *w++ = '&';
            r += 5;
        } else if (strncmp(r, "&lt;", 4) == 0) {
            *w++ = '<';
            r += 4;
        } else if (strncmp(r, "&gt;", 4) == 0) {
            *w++ = '>';
            r += 4;
        } else if (strncmp(r, "&quot;", 6) == 0) {
            *w++ = '"';
            r += 6;
        } else if (strncmp(r, "&apos;", 6) == 0) {
            *w++ = '\'';
            r += 6;
        } else {
            *w++ = *r++;
        }
    }

    *w = '\0';
}

