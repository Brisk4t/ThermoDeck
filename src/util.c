#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef BUILD_EPOCH
#define BUILD_EPOCH 1764000000L
#endif
const long g_build_epoch = BUILD_EPOCH;

char g_dir[256] = "/config/thermodeck";

uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

void tdlog(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    uint64_t t = now_ms();
    int n = snprintf(buf, sizeof buf, "[%5llu.%03llu] ", (unsigned long long)(t / 1000), (unsigned long long)(t % 1000));
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

void str_copy(char *dst, size_t dstlen, const char *src)
{
    if (!dstlen) return;
    size_t n = src ? strlen(src) : 0;
    if (n >= dstlen) n = dstlen - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = 0;
}

void path_join(char *out, size_t outlen, const char *name)
{
    snprintf(out, outlen, "%s/%s", g_dir, name);
}

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
            if (!nb) { free(buf); close(fd); return NULL; }
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
        if (w <= 0) { close(fd); unlink(tmp); return -1; }
        p += w;
        len -= (size_t)w;
    }
    fsync(fd);
    close(fd);
    if (rename(tmp, path) < 0) { unlink(tmp); return -1; }
    return 0;
}

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

void url_encode(const char *in, char *out, size_t outlen)
{
    static const char hx[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < outlen; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hx[c >> 4];
            out[o++] = hx[c & 15];
        }
    }
    out[o] = 0;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void url_decode(const char *in, size_t inlen, char *out, size_t outlen)
{
    size_t o = 0;
    for (size_t i = 0; i < inlen && o + 1 < outlen; i++) {
        if (in[i] == '+') {
            out[o++] = ' ';
        } else if (in[i] == '%' && i + 2 < inlen && hexval(in[i + 1]) >= 0 && hexval(in[i + 2]) >= 0) {
            out[o++] = (char)(hexval(in[i + 1]) * 16 + hexval(in[i + 2]));
            i += 2;
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = 0;
}

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

void hex_encode(const unsigned char *in, size_t len, char *out)
{
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = hx[in[i] >> 4];
        out[i * 2 + 1] = hx[in[i] & 15];
    }
    out[len * 2] = 0;
}

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

int sntp_sync(const char *server)
{
    struct addrinfo hints = {0}, *ai = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(server, "123", &hints, &ai) != 0 || !ai) {
        tdlog("ntp: cannot resolve %s", server);
        return -1;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int rc = -1;
    if (fd >= 0) {
        unsigned char pkt[48] = {0};
        pkt[0] = 0x1b; /* LI=0, VN=3, Mode=3 (client) */
        for (int attempt = 0; attempt < 3 && rc; attempt++) {
            if (sendto(fd, pkt, sizeof pkt, 0, ai->ai_addr, ai->ai_addrlen) != sizeof pkt) break;
            struct pollfd pfd = {fd, POLLIN, 0};
            if (poll(&pfd, 1, 2500) <= 0) continue;
            unsigned char resp[48];
            if (recv(fd, resp, sizeof resp, 0) < 48) continue;
            uint32_t secs = ((uint32_t)resp[40] << 24) | ((uint32_t)resp[41] << 16) | ((uint32_t)resp[42] << 8) | resp[43];
            uint32_t frac = ((uint32_t)resp[44] << 24) | ((uint32_t)resp[45] << 16) | ((uint32_t)resp[46] << 8) | resp[47];
            if (secs == 0) continue;
            struct timeval tv;
            tv.tv_sec = (time_t)(secs - 2208988800u);
            tv.tv_usec = (suseconds_t)(((uint64_t)frac * 1000000u) >> 32);
            if (tv.tv_sec < g_build_epoch) continue;
            if (settimeofday(&tv, NULL) == 0) {
                rc = 0;
                tdlog("ntp: clock set from %s", server);
            } else {
                tdlog("ntp: settimeofday failed: %s", strerror(errno));
                rc = -2;
            }
        }
        close(fd);
    }
    freeaddrinfo(ai);
    return rc;
}
