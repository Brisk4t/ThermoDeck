#ifndef NET_SOCKET_H
#define NET_SOCKET_H

/* TCP and TLS plumbing shared by the HTTPS and WebSocket clients. */

#include <stddef.h>

#include "bearssl.h"

/* Resolves host and connects (10 s timeout). The socket gets 15 s read/write timeouts.
 * Returns the fd, or -1 with a description in err. */
int tcp_connect(const char *host, int port, char *err, size_t errlen);

/* Sets up a BearSSL client for host on iobuf (BR_SSL_BUFSIZE_BIDI bytes), validating the
 * server against the embedded Mozilla roots. The handshake happens on the first write.
 * Returns 0, or -1 with a description in err. */
int tls_client_setup(br_ssl_client_context *sc, br_x509_minimal_context *xc, unsigned char *iobuf, size_t iobuf_len,
                     const char *host, char *err, size_t errlen);

/* br_sslio callbacks for a blocking socket; ctx points to the int fd. */
int sock_read(void *ctx, unsigned char *buf, size_t len);
int sock_write(void *ctx, const unsigned char *buf, size_t len);

#endif
