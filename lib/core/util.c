/*
 * Small shared helpers: logging, time, strings, files, URL/base64/hex encoding and randomness.
 */

#include "core/common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef BUILD_EPOCH
#define BUILD_EPOCH 1764000000L
#endif
/* Build time (seconds since 1970): a lower bound for "the clock has been set". */
const long g_build_epoch = BUILD_EPOCH;

char g_dir[256] = "/config/thermodeck";

/* Monotonic milliseconds; unaffected by NTP changing the wall clock. */
uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* Timestamped log line to stderr (boot.sh sends it to /tmp/thermodeck.log). */
void tdlog(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    uint64_t t = now_ms();
    int n =
        snprintf(buf, sizeof buf, "[%5llu.%03llu] ", (unsigned long long)(t / 1000), (unsigned long long)(t % 1000));
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof buf - (size_t)n - 2, fmt, ap);
    va_end(ap);
    size_t l = strlen(buf);
    if (l == 0 || buf[l - 1] != '\n') {
        buf[l++] = '\n';
        buf[l] = 0;
    }
    fputs(buf, stderr);
    fflush(stderr);
}

/* strlcpy: always NUL-terminates, truncates if needed, NULL src gives "". */
void str_copy(char *dst, size_t dstlen, const char *src)
{
    if (!dstlen) return;
    size_t n = src ? strlen(src) : 0;
    if (n >= dstlen) n = dstlen - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

/* Path of a file in the data directory (g_dir). */
void path_join(char *out, size_t outlen, const char *name) { snprintf(out, outlen, "%s/%s", g_dir, name); }

/* Reads a whole file into a malloc'd, NUL-terminated buffer. */
char *file_read(const char *path, size_t *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap + 1);
    for (;;) {
        if (n == cap) {
            cap *= 2;
            char *nb = realloc(buf, cap + 1);
            if (!nb) {
                free(buf);
                close(fd);
                return NULL;
            }
            buf = nb;
        }
        ssize_t r = read(fd, buf + n, cap - n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fd);
    buf[n] = 0;
    if (len) *len = n;
    return buf;
}

/*
 * Writes to path.tmp, fsyncs, then renames over path, so a power cut never leaves a
 * half-written file.
 */
int file_write_atomic(const char *path, const void *data, size_t len, int mode)
{
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return -1;
    const char *p = data;
    while (len) {
        ssize_t w = write(fd, p, len);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) {
            close(fd);
            unlink(tmp);
            return -1;
        }
        p += w;
        len -= (size_t)w;
    }
    fsync(fd);
    close(fd);
    if (rename(tmp, path) < 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* Standard base64 with padding (for HTTP Basic auth). */
size_t base64_encode(const unsigned char *in, size_t len, char *out, size_t outlen)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        if (o + 4 >= outlen) break;
        out[o++] = tbl[(v >> 18) & 63];
        out[o++] = tbl[(v >> 12) & 63];
        out[o++] = i + 1 < len ? tbl[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? tbl[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

/* Percent-encodes everything except RFC 3986 unreserved characters. */
void url_encode(const char *in, char *out, size_t outlen)
{
    static const char hx[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < outlen; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out[o++] = (char)c;
        }
        else {
            out[o++] = '%';
            out[o++] = hx[c >> 4];
            out[o++] = hx[c & 15];
        }
    }
    out[o] = 0;
}

/* Hex digit value, or -1. */
static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decodes %XX and '+' (form encoding). */
void url_decode(const char *in, size_t inlen, char *out, size_t outlen)
{
    size_t o = 0;
    for (size_t i = 0; i < inlen && o + 1 < outlen; i++) {
        if (in[i] == '+') {
            out[o++] = ' ';
        }
        else if (in[i] == '%' && i + 2 < inlen && hexval(in[i + 1]) >= 0 && hexval(in[i + 2]) >= 0) {
            out[o++] = (char)(hexval(in[i + 1]) * 16 + hexval(in[i + 2]));
            i += 2;
        }
        else {
            out[o++] = in[i];
        }
    }
    out[o] = 0;
}

/*
 * Finds name=value in a query string (or a full URL) and URL-decodes the value.
 * Returns -1 if the parameter isn't there.
 */
int query_param(const char *qs, const char *name, char *out, size_t outlen)
{
    size_t nl = strlen(name);
    const char *p = qs;
    while (p && *p) {
        if (!strncmp(p, name, nl) && p[nl] == '=' && (p == qs || p[-1] == '&' || p[-1] == '?' || p[-1] == '#')) {
            const char *v = p + nl + 1;
            size_t vl = strcspn(v, "&# \r\n");
            url_decode(v, vl, out, outlen);
            return 0;
        }
        p = strpbrk(p + 1, "&?#");
        if (p) p++;
    }
    return -1;
}

/* Lowercase hex; out needs 2*len+1 bytes. */
void hex_encode(const unsigned char *in, size_t len, char *out)
{
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = hx[in[i] >> 4];
        out[i * 2 + 1] = hx[in[i] & 15];
    }
    out[len * 2] = 0;
}

/* Hex string -> bytes. Returns the byte count, or -1 on a bad digit / overflow. */
int hex_decode(const char *in, unsigned char *out, size_t outlen)
{
    size_t n = 0;
    while (in[0] && in[1]) {
        int a = hexval(in[0]), b = hexval(in[1]);
        if (a < 0 || b < 0 || n >= outlen) return -1;
        out[n++] = (unsigned char)(a * 16 + b);
        in += 2;
    }
    return (int)n;
}

/* Fills buf from /dev/urandom (weak fallback only if that fails). */
void random_bytes(unsigned char *buf, size_t len)
{
    int fd = open("/dev/urandom", O_RDONLY);
    size_t got = 0;
    if (fd >= 0) {
        while (got < len) {
            ssize_t r = read(fd, buf + got, len - got);
            if (r <= 0) break;
            got += (size_t)r;
        }
        close(fd);
    }
    for (; got < len; got++) buf[got] = (unsigned char)(rand() ^ (int)now_ms());
}
