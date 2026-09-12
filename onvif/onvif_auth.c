/*
 * onvif_auth.c
 *
 * 生成 ONVIF 常用的 WS-Security UsernameToken。
 * 为了方便交叉编译到 RK3566，这里自带 SHA1 和 Base64，不依赖 OpenSSL。
 */

#include "onvif_auth.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

typedef struct Sha1Ctx {
    uint32_t h[5];
    uint64_t bytes;
    uint8_t block[64];
    size_t used;
} Sha1Ctx;

static uint32_t rol32(uint32_t v, unsigned int n)
{
    return (v << n) | (v >> (32U - n));
}

static uint32_t load_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static void store_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void store_be64(uint8_t *p, uint64_t v)
{
    int i;

    for (i = 7; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

static void sha1_transform(Sha1Ctx *ctx, const uint8_t block[64])
{
    uint32_t w[80];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = load_be32(block + i * 4);
    }

    for (i = 16; i < 80; i++) {
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    a = ctx->h[0];
    b = ctx->h[1];
    c = ctx->h[2];
    d = ctx->h[3];
    e = ctx->h[4];

    for (i = 0; i < 80; i++) {
        uint32_t f;
        uint32_t k;
        uint32_t temp;

        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5a827999U;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1U;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdcU;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6U;
        }

        temp = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol32(b, 30);
        b = a;
        a = temp;
    }

    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
    ctx->h[4] += e;
}

static void sha1_init(Sha1Ctx *ctx)
{
    ctx->h[0] = 0x67452301U;
    ctx->h[1] = 0xefcdab89U;
    ctx->h[2] = 0x98badcfeU;
    ctx->h[3] = 0x10325476U;
    ctx->h[4] = 0xc3d2e1f0U;
    ctx->bytes = 0;
    ctx->used = 0;
}

static void sha1_update(Sha1Ctx *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    ctx->bytes += len;

    while (len > 0) {
        size_t n = 64 - ctx->used;

        if (n > len) {
            n = len;
        }

        memcpy(ctx->block + ctx->used, p, n);
        ctx->used += n;
        p += n;
        len -= n;

        if (ctx->used == 64) {
            sha1_transform(ctx, ctx->block);
            ctx->used = 0;
        }
    }
}

static void sha1_final(Sha1Ctx *ctx, uint8_t digest[20])
{
    uint64_t bit_len = ctx->bytes * 8U;
    size_t i;

    ctx->block[ctx->used++] = 0x80;

    if (ctx->used > 56) {
        while (ctx->used < 64) {
            ctx->block[ctx->used++] = 0;
        }
        sha1_transform(ctx, ctx->block);
        ctx->used = 0;
    }

    while (ctx->used < 56) {
        ctx->block[ctx->used++] = 0;
    }

    store_be64(ctx->block + 56, bit_len);
    sha1_transform(ctx, ctx->block);

    for (i = 0; i < 5; i++) {
        store_be32(digest + i * 4, ctx->h[i]);
    }
}

static int base64_encode(const uint8_t *in, size_t in_len, char *out, size_t out_size)
{
    static const char tab[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = ((in_len + 2) / 3) * 4 + 1;
    size_t i;
    size_t o = 0;

    if (!out || out_size < need) {
        return -1;
    }

    for (i = 0; i < in_len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        int remain = (int)(in_len - i);

        if (remain > 1) {
            v |= (uint32_t)in[i + 1] << 8;
        }
        if (remain > 2) {
            v |= (uint32_t)in[i + 2];
        }

        out[o++] = tab[(v >> 18) & 0x3f];
        out[o++] = tab[(v >> 12) & 0x3f];
        out[o++] = remain > 1 ? tab[(v >> 6) & 0x3f] : '=';
        out[o++] = remain > 2 ? tab[v & 0x3f] : '=';
    }

    out[o] = '\0';
    return 0;
}

static void fill_random(uint8_t *buf, size_t len)
{
    int fd;
    ssize_t n;
    size_t done = 0;

    fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        while (done < len) {
            n = read(fd, buf + done, len - done);
            if (n <= 0) {
                break;
            }
            done += (size_t)n;
        }
        close(fd);
    }

    if (done < len) {
        size_t i;
        srand((unsigned int)(time(NULL) ^ getpid()));
        for (i = done; i < len; i++) {
            buf[i] = (uint8_t)(rand() & 0xff);
        }
    }
}

static void make_created(char *out, size_t out_size)
{
    time_t now;
    struct tm tm_utc;

    now = time(NULL);
    gmtime_r(&now, &tm_utc);
    strftime(out, out_size, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

static int xml_escape_text(const char *in, char *out, size_t out_size)
{
    size_t used = 0;

    if (!out || out_size == 0) {
        return -1;
    }

    out[0] = '\0';

    if (!in) {
        return 0;
    }

    while (*in) {
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
                return -1;
            }
            memcpy(out + used, rep, n);
            used += n;
        } else {
            if (used + 1 >= out_size) {
                return -1;
            }
            out[used++] = *in;
        }

        in++;
    }

    out[used] = '\0';
    return 0;
}

static int make_password_digest(const uint8_t *nonce,
                                size_t nonce_len,
                                const char *created,
                                const char *password,
                                char *digest_b64,
                                size_t digest_b64_size)
{
    Sha1Ctx sha1;
    uint8_t digest[20];

    sha1_init(&sha1);
    sha1_update(&sha1, nonce, nonce_len);
    sha1_update(&sha1, created, strlen(created));
    sha1_update(&sha1, password ? password : "", strlen(password ? password : ""));
    sha1_final(&sha1, digest);

    return base64_encode(digest, sizeof(digest), digest_b64, digest_b64_size);
}

int wsse_build_security_header(const char *user,
                               const char *password,
                               int use_digest,
                               char *out,
                               size_t out_size)
{
    char created[32];
    char nonce_b64[64];
    char digest_b64[64];
    char user_xml[512];
    char password_xml[512];
    uint8_t nonce[16];
    int n;

    if (!out || out_size == 0) {
        return -1;
    }

    out[0] = '\0';

    if (!user || user[0] == '\0') {
        return 0;
    }

    make_created(created, sizeof(created));

    if (xml_escape_text(user, user_xml, sizeof(user_xml)) < 0 ||
        xml_escape_text(password ? password : "", password_xml, sizeof(password_xml)) < 0) {
        return -1;
    }

    if (use_digest) {
        fill_random(nonce, sizeof(nonce));
        if (base64_encode(nonce, sizeof(nonce), nonce_b64, sizeof(nonce_b64)) < 0) {
            return -1;
        }
        if (make_password_digest(nonce, sizeof(nonce), created, password,
                                 digest_b64, sizeof(digest_b64)) < 0) {
            return -1;
        }

        n = snprintf(out,
                     out_size,
                     "<wsse:Security s:mustUnderstand=\"1\">"
                     "<wsse:UsernameToken>"
                     "<wsse:Username>%s</wsse:Username>"
                     "<wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/"
                     "oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">%s</wsse:Password>"
                     "<wsse:Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/"
                     "oasis-200401-wss-soap-message-security-1.0#Base64Binary\">%s</wsse:Nonce>"
                     "<wsu:Created>%s</wsu:Created>"
                     "</wsse:UsernameToken>"
                     "</wsse:Security>",
                     user_xml,
                     digest_b64,
                     nonce_b64,
                     created);
    } else {
        n = snprintf(out,
                     out_size,
                     "<wsse:Security s:mustUnderstand=\"1\">"
                     "<wsse:UsernameToken>"
                     "<wsse:Username>%s</wsse:Username>"
                     "<wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/"
                     "oasis-200401-wss-username-token-profile-1.0#PasswordText\">%s</wsse:Password>"
                     "<wsu:Created>%s</wsu:Created>"
                     "</wsse:UsernameToken>"
                     "</wsse:Security>",
                     user_xml,
                     password_xml,
                     created);
    }

    if (n < 0 || (size_t)n >= out_size) {
        out[0] = '\0';
        return -1;
    }

    return 0;
}
