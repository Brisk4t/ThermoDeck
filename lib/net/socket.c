/* TCP connect with timeouts, BearSSL client setup, and blocking socket I/O callbacks. */

#include "net/socket.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "core/common.h"

extern const br_x509_trust_anchor *trust_anchors;
extern const size_t trust_anchors_num;

/* BearSSL I/O callbacks: plain blocking socket reads/writes. */
int sock_read(void *ctx, unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    for (;;) {
        ssize_t r = read(fd, buf, len);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        return (int)r;
    }
}

int sock_write(void *ctx, const unsigned char *buf, size_t len)
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
int tcp_connect(const char *host, int port, char *err, size_t errlen)
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

int tls_client_setup(br_ssl_client_context *sc, br_x509_minimal_context *xc, unsigned char *iobuf, size_t iobuf_len,
                     const char *host, char *err, size_t errlen)
{
    br_ssl_client_init_full(sc, xc, trust_anchors, trust_anchors_num);
    /* Validate certificates against the real time; if the clock was never set
     * (it boots in 2014), use the build time so that current certs pass. */
    time_t now = time(NULL);
    if (now < g_build_epoch) now = g_build_epoch;
    br_x509_minimal_set_time(xc, (uint32_t)(now / 86400) + 719528u, (uint32_t)(now % 86400));
    br_ssl_engine_set_buffer(&sc->eng, iobuf, iobuf_len, 1);
    if (!br_ssl_client_reset(sc, host, 0)) {
        snprintf(err, errlen, "TLS reset failed");
        return -1;
    }
    return 0;
}
