/*
 * onvif_auth.h
 *
 * WS-Security UsernameToken 生成工具。
 * Digest 计算规则：
 *   PasswordDigest = Base64(SHA1(NonceBinary + Created + Password))
 */

#ifndef ONVIF_AUTH_H
#define ONVIF_AUTH_H

#include <stddef.h>

int wsse_build_security_header(const char *user,
                               const char *password,
                               int use_digest,
                               char *out,
                               size_t out_size);

#endif

