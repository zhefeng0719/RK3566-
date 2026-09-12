/*
 * onvif_xml.h
 *
 * 极轻量 XML 字符串工具。
 * 这里不实现完整 XML 解析器，只处理 ONVIF 最小链路需要的字段：
 *   XAddrs / XAddr / Profiles token / Uri
 */

#ifndef ONVIF_XML_H
#define ONVIF_XML_H

#include <stddef.h>

int xml_get_first_tag_text(const char *xml,
                           const char *local_name,
                           char *out,
                           size_t out_size);

int xml_get_first_http_url_from_text(const char *text,
                                     char *out,
                                     size_t out_size);

int xml_get_first_profile_token(const char *xml,
                                char *out,
                                size_t out_size);

void xml_unescape_inplace(char *s);

#endif

