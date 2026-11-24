/*
 * WebSocket client: HTTP/1.1 upgrade handshake, then RFC 6455 frames. The transport is a
 * plain socket or a BearSSL stream; a small read buffer sits on top of either so that
 * ws_wait() can tell whether a frame is already waiting before it polls the socket.
 */

#include "net/ws.h"

#include <poll.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>

#include "core/common.h"
#include "net/socket.h"

#define WS_MAX_MSG (1024 * 1024)

struct ws_conn {
    int fd;
    int tls;
    char host[128];
    unsigned char rbuf[4096];
    size_t rpos, rlen;
    /* TLS state (unused for plain connections) */
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    br_sslio_context ioc;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
};

/* ---------- transport ---------- */

static int io_write(ws_conn *c, const void *buf, size_t len)
{
    if (c->tls) return br_sslio_write_all(&c->ioc, buf, len) < 0 || br_sslio_flush(&c->ioc) < 0 ? -1 : 0;
    const unsigned char *p = buf;
    while (len) {
        int w = sock_write(&c->fd, p, len);
        if (w <= 0) return -1;
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

/* Refills the read buffer (blocking, subject to the socket timeout). */
static int io_fill(ws_conn *c)
{
    int n = c->tls ? br_sslio_read(&c->ioc, c->rbuf, sizeof c->rbuf) : sock_read(&c->fd, c->rbuf, sizeof c->rbuf);
    if (n <= 0) return -1;
    c->rpos = 0;
    c->rlen = (size_t)n;
    return 0;
}

static int io_read(ws_conn *c, void *out, size_t n)
{
    unsigned char *o = out;
    while (n) {
        if (c->rpos >= c->rlen && io_fill(c) < 0) return -1;
        size_t k = c->rlen - c->rpos;
        if (k > n) k = n;
        memcpy(o, c->rbuf + c->rpos, k);
        c->rpos += k;
        o += k;
        n -= k;
    }
    return 0;
}

/* Data already decrypted / buffered that poll() on the socket wouldn't see. */
static int io_pending(ws_conn *c)
{
    if (c->rpos < c->rlen) return 1;
    return c->tls && (br_ssl_engine_current_state(&c->sc.eng) & BR_SSL_RECVAPP);
}

static int io_line(ws_conn *c, char *out, size_t outlen)
{
    size_t o = 0;
    for (;;) {
        char ch;
        if (io_read(c, &ch, 1) < 0) return -1;
        if (ch == '\n') break;
        if (ch != '\r' && o + 1 < outlen) out[o++] = ch;
    }
    out[o] = 0;
    return (int)o;
}

/* ---------- connect / handshake ---------- */

/* Splits "scheme://host[:port][/...]" into its parts; the path part is ignored. */
static int parse_url(const char *url, int *tls, char *host, size_t hostlen, int *port)
{
    const char *p = strstr(url, "://");
    if (!p) return -1;
    size_t sl = (size_t)(p - url);
    if ((sl == 2 && !strncasecmp(url, "ws", 2)) || (sl == 4 && !strncasecmp(url, "http", 4)))
        *tls = 0;
    else if ((sl == 3 && !strncasecmp(url, "wss", 3)) || (sl == 5 && !strncasecmp(url, "https", 5)))
        *tls = 1;
    else
        return -1;
    p += 3;
    size_t hl = strcspn(p, ":/");
    if (!hl || hl >= hostlen) return -1;
    memcpy(host, p, hl);
    host[hl] = 0;
    *port = *tls ? 443 : 80;
    if (p[hl] == ':') *port = atoi(p + hl + 1);
    return *port > 0 && *port < 65536 ? 0 : -1;
}

ws_conn *ws_connect(const char *url, const char *path, char *err, size_t errlen)
{
    ws_conn *c = calloc(1, sizeof *c);
    if (!c) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    int port;
    if (parse_url(url, &c->tls, c->host, sizeof c->host, &port) < 0) {
        snprintf(err, errlen, "bad URL %s", url);
        free(c);
        return NULL;
    }
    c->fd = tcp_connect(c->host, port, err, errlen);
    if (c->fd < 0) {
        free(c);
        return NULL;
    }
    if (c->tls) {
        if (tls_client_setup(&c->sc, &c->xc, c->iobuf, sizeof c->iobuf, c->host, err, errlen) < 0) goto fail;
        br_sslio_init(&c->ioc, &c->sc.eng, sock_read, &c->fd, sock_write, &c->fd);
    }

    /* upgrade request with a random key */
    unsigned char key[16];
    char key64[32], req[512];
    random_bytes(key, sizeof key);
    base64_encode(key, sizeof key, key64, sizeof key64);
    int n =
        snprintf(req, sizeof req,
                 "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                 "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nUser-Agent: ThermoDeck/" TD_VERSION "\r\n\r\n",
                 path, c->host, port, key64);
    if (io_write(c, req, (size_t)n) < 0) {
        snprintf(err, errlen, "%s: handshake send failed%s", c->host, c->tls ? " (TLS)" : "");
        goto fail;
    }
    /* expect "101 Switching Protocols", then skip the headers */
    char line[512];
    int status = 0;
    if (io_line(c, line, sizeof line) < 0 || sscanf(line, "HTTP/%*d.%*d %d", &status) != 1) {
        snprintf(err, errlen, "%s: no HTTP response%s", c->host, c->tls ? " (TLS error?)" : "");
        goto fail;
    }
    while (io_line(c, line, sizeof line) > 0) {}
    if (status != 101) {
        snprintf(err, errlen, "%s: upgrade refused (HTTP %d)", c->host, status);
        goto fail;
    }
    return c;
fail:
    ws_close(c);
    return NULL;
}

void ws_close(ws_conn *c)
{
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    free(c);
}

/* ---------- frames ---------- */

enum { OP_CONT = 0, OP_TEXT = 1, OP_BIN = 2, OP_CLOSE = 8, OP_PING = 9, OP_PONG = 10 };

/* Sends one complete frame; client frames must be masked. */
static int send_frame(ws_conn *c, int op, const char *data, size_t len)
{
    unsigned char hdr[14];
    size_t h = 0;
    hdr[h++] = (unsigned char)(0x80 | op);
    if (len < 126) {
        hdr[h++] = (unsigned char)(0x80 | len);
    }
    else if (len < 65536) {
        hdr[h++] = 0x80 | 126;
        hdr[h++] = (unsigned char)(len >> 8);
        hdr[h++] = (unsigned char)len;
    }
    else {
        hdr[h++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) hdr[h++] = (unsigned char)((uint64_t)len >> (i * 8));
    }
    unsigned char mask[4];
    random_bytes(mask, 4);
    memcpy(hdr + h, mask, 4);
    h += 4;
    if (io_write(c, hdr, h) < 0) return -1;
    /* mask the payload in chunks */
    unsigned char chunk[1024];
    for (size_t off = 0; off < len;) {
        size_t k = len - off < sizeof chunk ? len - off : sizeof chunk;
        for (size_t i = 0; i < k; i++) chunk[i] = (unsigned char)data[off + i] ^ mask[(off + i) & 3];
        if (io_write(c, chunk, k) < 0) return -1;
        off += k;
    }
    return 0;
}

int ws_send(ws_conn *c, const char *text, size_t len) { return send_frame(c, OP_TEXT, text, len); }

int ws_wait(ws_conn *c, int extra_fd, int timeout_ms)
{
    int r = 0;
    if (io_pending(c)) {
        r |= WS_READY;
        timeout_ms = 0; /* still report extra_fd if it's ready too */
    }
    struct pollfd pfd[2] = {{c->fd, POLLIN, 0}, {extra_fd, POLLIN, 0}};
    int n = poll(pfd, extra_fd >= 0 ? 2 : 1, timeout_ms);
    if (n > 0) {
        if (pfd[0].revents) r |= WS_READY; /* data, hangup or error: ws_recv() finds out */
        if (extra_fd >= 0 && pfd[1].revents) r |= WS_EXTRA;
    }
    return r;
}

int ws_recv(ws_conn *c, char **msg, size_t *msg_len)
{
    char *buf = NULL;
    size_t len = 0;
    *msg = NULL;
    /* read frames until a complete message (continuation frames are appended) */
    for (;;) {
        unsigned char h[2];
        if (io_read(c, h, 2) < 0) goto broken;
        int fin = h[0] & 0x80, op = h[0] & 0x0F, masked = h[1] & 0x80;
        uint64_t plen = h[1] & 0x7F;
        if (plen == 126 || plen == 127) {
            unsigned char ext[8];
            int nb = plen == 126 ? 2 : 8;
            if (io_read(c, ext, (size_t)nb) < 0) goto broken;
            plen = 0;
            for (int i = 0; i < nb; i++) plen = (plen << 8) | ext[i];
        }
        unsigned char mask[4] = {0};
        if (masked && io_read(c, mask, 4) < 0) goto broken;
        if (len + plen > WS_MAX_MSG) {
            tdlog("ws: message too large (%llu bytes)", (unsigned long long)(len + plen));
            goto broken;
        }

        if (op >= OP_CLOSE) {
            /* control frame: small, may arrive between fragments */
            char ctl[126];
            if (plen > sizeof ctl || io_read(c, ctl, (size_t)plen) < 0) goto broken;
            if (masked)
                for (size_t i = 0; i < plen; i++) ctl[i] ^= (char)mask[i & 3];
            if (op == OP_CLOSE) {
                send_frame(c, OP_CLOSE, ctl, plen < 2 ? (size_t)plen : 2);
                goto broken;
            }
            if (op == OP_PING && send_frame(c, OP_PONG, ctl, (size_t)plen) < 0) goto broken;
            if (!buf) return 0;
            continue;
        }

        char *nb = realloc(buf, len + plen + 1);
        if (!nb) goto broken;
        buf = nb;
        if (io_read(c, buf + len, (size_t)plen) < 0) goto broken;
        if (masked)
            for (size_t i = 0; i < plen; i++) buf[len + i] ^= (char)mask[i & 3];
        len += plen;
        buf[len] = 0;
        if (fin) {
            if (op == OP_BIN) {
                free(buf);
                return 0; /* not used by Home Assistant */
            }
            *msg = buf;
            *msg_len = len;
            return 1;
        }
    }
broken:
    free(buf);
    return -1;
}
