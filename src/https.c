/*
 * Minimal HTTPS/1.1 client on BearSSL. One https_conn per host, kept alive between
 * requests; responses are read fully into memory (Content-Length, chunked, or until close).
 * Certificates are checked against the embedded Mozilla roots (gen/trust_anchors.c).
 */

#include "https.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

extern const br_x509_trust_anchor *trust_anchors;
extern const size_t trust_anchors_num;

#define MAX_BODY (512 * 1024)

/* Prepares a (not yet connected) connection to host:443. */
void https_init(https_conn *c, const char *host)
{
    memset(c, 0, sizeof *c);
    str_copy(c->host, sizeof c->host, host);
    c->fd = -1;
}

/* Drops the socket; the next request reconnects. */
void https_close(https_conn *c)
{
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
    c->connected = 0;
}

void http_resp_free(http_resp *r)
{
    free(r->body);
    r->body = NULL;
    r->body_len = 0;
}

/* BearSSL I/O callbacks: plain blocking socket reads/writes. */
static int sock_read(void *ctx, unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    for (;;) {
        ssize_t r = read(fd, buf, len);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        return (int)r;
    }
}

static int sock_write(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    for (;;) {
        ssize_t w = send(fd, buf, len, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        return (int)w;
    }
}

/*
 * Resolves host and connects with a 10 s timeout; the socket then gets 15 s read/write
 * timeouts so a dead server can't hang the calling thread forever.
 */
static int tcp_connect(const char *host, int port, char *err, size_t errlen)
{
    struct addrinfo hints = {0}, *ai = NULL, *p;
    char ps[8];
    snprintf(ps, sizeof ps, "%d", port);
    hints.ai_family = AF_INET; /* the network here is IPv4 only */
    hints.ai_socktype = SOCK_STREAM;
    int g = getaddrinfo(host, ps, &hints, &ai);
    if (g != 0 || !ai) {
        snprintf(err, errlen, "DNS lookup failed for %s", host);
        return -1;
    }
    int fd = -1;
    /* try each address until one connects */
    for (p = ai; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        int fl = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int rc = connect(fd, p->ai_addr, p->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) {
            struct pollfd pfd = {fd, POLLOUT, 0};
            int soerr = 0;
            socklen_t sl = sizeof soerr;
            if (poll(&pfd, 1, 10000) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 && soerr == 0)
                rc = 0;
            else
                rc = -1;
        }
        if (rc == 0) {
            fcntl(fd, F_SETFL, fl);
            struct timeval tv = {15, 0};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            fcntl(fd, F_SETFD, FD_CLOEXEC);
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(ai);
    if (fd < 0) snprintf(err, errlen, "connect to %s failed", host);
    return fd;
}

/*
 * Connects and sets up the TLS client if there's no live connection.
 * (The handshake itself happens lazily on the first write.)
 */
static int ensure_connected(https_conn *c, char *err, size_t errlen)
{
    if (c->connected) return 0;
    c->fd = tcp_connect(c->host, 443, err, errlen);
    if (c->fd < 0) return -1;
    br_ssl_client_init_full(&c->sc, &c->xc, trust_anchors, trust_anchors_num);
    /* Validate certificates against the real time; if the clock was never set
     * (it boots in 2014), use the build time so that current certs pass. */
    time_t now = time(NULL);
    if (now < g_build_epoch) now = g_build_epoch;
    br_x509_minimal_set_time(&c->xc, (uint32_t)(now / 86400) + 719528u, (uint32_t)(now % 86400));
    br_ssl_engine_set_buffer(&c->sc.eng, c->iobuf, sizeof c->iobuf, 1);
    if (!br_ssl_client_reset(&c->sc, c->host, 0)) {
        snprintf(err, errlen, "TLS reset failed");
        https_close(c);
        return -1;
    }
    br_sslio_init(&c->ioc, &c->sc.eng, sock_read, &c->fd, sock_write, &c->fd);
    c->connected = 1;
    return 0;
}

/* Describes the last failure: a BearSSL error code, or the peer closing. */
static void tls_error(https_conn *c, const char *what, char *err, size_t errlen)
{
    int e = br_ssl_engine_last_error(&c->sc.eng);
    if (e)
        snprintf(err, errlen, "%s: TLS error %d", what, e);
    else
        snprintf(err, errlen, "%s: connection closed", what);
}

/* Buffered reader on top of br_sslio */
typedef struct {
    https_conn *c;
    unsigned char buf[2048];
    size_t pos, len;
    int got_any;
} reader;

/* Refills the buffer with the next decrypted chunk. */
static int rd_fill(reader *r)
{
    int n = br_sslio_read(&r->c->ioc, r->buf, sizeof r->buf);
    if (n <= 0) return -1;
    r->pos = 0;
    r->len = (size_t)n;
    r->got_any = 1;
    return n;
}

static int rd_byte(reader *r)
{
    if (r->pos >= r->len && rd_fill(r) < 0) return -1;
    return r->buf[r->pos++];
}

/* Reads a line (without CRLF). Returns length or -1. */
static int rd_line(reader *r, char *out, size_t outlen)
{
    size_t o = 0;
    for (;;) {
        int ch = rd_byte(r);
        if (ch < 0) return -1;
        if (ch == '\n') break;
        if (ch != '\r' && o + 1 < outlen) out[o++] = (char)ch;
    }
    out[o] = 0;
    return (int)o;
}

/* Reads exactly n bytes. */
static int rd_exact(reader *r, char *out, size_t n)
{
    while (n) {
        if (r->pos >= r->len && rd_fill(r) < 0) return -1;
        size_t k = r->len - r->pos;
        if (k > n) k = n;
        memcpy(out, r->buf + r->pos, k);
        r->pos += k;
        out += k;
        n -= k;
    }
    return 0;
}

/* Reads n more body bytes into r->body, growing it (capped at MAX_BODY). */
static int body_append_exact(reader *rd, http_resp *r, size_t *cap, size_t n)
{
    if (r->body_len + n > MAX_BODY) return -1;
    if (r->body_len + n + 1 > *cap) {
        size_t nc = (r->body_len + n + 1) * 2;
        char *nb = realloc(r->body, nc);
        if (!nb) return -1;
        r->body = nb;
        *cap = nc;
    }
    if (rd_exact(rd, r->body + r->body_len, n) < 0) return -1;
    r->body_len += n;
    r->body[r->body_len] = 0;
    return 0;
}

/*
 * Sends one request and reads the full response into r. got_any reports whether any
 * response bytes arrived (used to decide if a retry is safe).
 */
static int do_request(https_conn *c, const char *method, const char *path, const char *extra, const char *body,
                      size_t body_len, http_resp *r, int *got_any)
{
    char head[2048];
    /* request line + headers (Accept defaults to JSON unless the caller sets one) */
    int hl = snprintf(head, sizeof head,
                      "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: ThermoDeck/" TD_VERSION "\r\n"
                      "%sConnection: keep-alive\r\n%sContent-Length: %zu\r\n\r\n",
                      method, path, c->host, extra && strstr(extra, "Accept:") ? "" : "Accept: application/json\r\n",
                      extra ? extra : "", body_len);
    if (hl <= 0 || (size_t)hl >= sizeof head) {
        snprintf(r->error, sizeof r->error, "request too long");
        return -1;
    }
    *got_any = 0;
    if (br_sslio_write_all(&c->ioc, head, (size_t)hl) < 0 ||
        (body_len && br_sslio_write_all(&c->ioc, body, body_len) < 0) || br_sslio_flush(&c->ioc) < 0) {
        tls_error(c, "send", r->error, sizeof r->error);
        return -1;
    }

    reader rd; /* per call: the Spotify and album-art threads both make requests */
    memset(&rd, 0, sizeof rd);
    rd.c = c;
    char line[1024];
    int status;
    /* skip 1xx responses */
    do {
        if (rd_line(&rd, line, sizeof line) < 0) {
            *got_any = rd.got_any;
            tls_error(c, "recv", r->error, sizeof r->error);
            return -1;
        }
        *got_any = 1;
        if (sscanf(line, "HTTP/%*d.%*d %d", &status) != 1) {
            snprintf(r->error, sizeof r->error, "bad status line");
            return -1;
        }
        /* headers we care about */
        long clen = -1;
        int chunked = 0, close_after = 0;
        for (;;) {
            if (rd_line(&rd, line, sizeof line) < 0) {
                tls_error(c, "headers", r->error, sizeof r->error);
                return -1;
            }
            if (!line[0]) break;
            char *colon = strchr(line, ':');
            if (!colon) continue;
            *colon = 0;
            char *v = colon + 1;
            while (*v == ' ') v++;
            if (!strcasecmp(line, "Content-Length"))
                clen = atol(v);
            else if (!strcasecmp(line, "Transfer-Encoding") && strstr(v, "chunked"))
                chunked = 1;
            else if (!strcasecmp(line, "Connection") && !strncasecmp(v, "close", 5))
                close_after = 1;
            else if (!strcasecmp(line, "Retry-After"))
                r->retry_after = atoi(v);
        }
        if (status >= 100 && status < 200) continue;

        /* body: none, chunked, fixed length, or read-until-close */
        r->status = status;
        size_t cap = 0;
        r->body = malloc(1);
        r->body[0] = 0;
        r->body_len = 0;
        cap = 1;
        int no_body = status == 204 || status == 304 || !strcmp(method, "HEAD");
        if (no_body) { /* nothing */
        }
        else if (chunked) {
            for (;;) {
                if (rd_line(&rd, line, sizeof line) < 0) goto body_err;
                long n = strtol(line, NULL, 16);
                if (n < 0) goto body_err;
                if (n == 0) {
                    /* trailers until empty line */
                    while (rd_line(&rd, line, sizeof line) > 0) {}
                    break;
                }
                if (body_append_exact(&rd, r, &cap, (size_t)n) < 0) goto body_err;
                if (rd_line(&rd, line, sizeof line) < 0) goto body_err;
            }
        }
        else if (clen >= 0) {
            if (body_append_exact(&rd, r, &cap, (size_t)clen) < 0) goto body_err;
        }
        else {
            /* read until close */
            for (;;) {
                if (rd.pos >= rd.len && rd_fill(&rd) < 0) break;
                size_t k = rd.len - rd.pos;
                if (r->body_len + k > MAX_BODY) goto body_err;
                if (r->body_len + k + 1 > cap) {
                    cap = (r->body_len + k + 1) * 2;
                    r->body = realloc(r->body, cap);
                }
                memcpy(r->body + r->body_len, rd.buf + rd.pos, k);
                r->body_len += k;
                r->body[r->body_len] = 0;
                rd.pos = rd.len;
            }
            close_after = 1;
        }
        if (close_after) https_close(c);
        return 0;
    body_err:
        tls_error(c, "body", r->error, sizeof r->error);
        http_resp_free(r);
        return -1;
    } while (1);
}

/* Public entry point. Reuses the kept-alive connection when possible. */
int https_request(https_conn *c, const char *method, const char *path, const char *extra_headers, const char *body,
                  size_t body_len, http_resp *r)
{
    memset(r, 0, sizeof *r);
    r->status = -1;
    for (int attempt = 0; attempt < 2; attempt++) {
        int reused = c->connected;
        if (ensure_connected(c, r->error, sizeof r->error) < 0) return -1;
        int got_any = 0;
        uint64_t t0 = now_ms();
        if (do_request(c, method, path, extra_headers, body, body_len, r, &got_any) == 0) {
            if (!reused) tdlog("https: %s connected+request in %llums", c->host, (unsigned long long)(now_ms() - t0));
            return 0;
        }
        https_close(c);
        /* A kept-alive connection may have been closed by the server; retry once on a fresh one. */
        if (!(reused && !got_any)) break;
        r->status = -1;
        r->error[0] = 0;
    }
    tdlog("https: %s %s%s failed: %s", method, c->host, path, r->error);
    r->status = -1;
    return -1;
}
