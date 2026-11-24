#ifndef NET_HTTPS_H
#define NET_HTTPS_H
#include <stddef.h>

#include "bearssl.h"

typedef struct {
    char host[64];
    int fd;
    int connected;
    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    br_sslio_context ioc;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
} https_conn;

typedef struct {
    int status; /* HTTP status, or -1 on transport error */
    char *body; /* malloc'd, NUL-terminated (may be empty) */
    size_t body_len;
    int retry_after; /* seconds, from Retry-After */
    char error[96];  /* transport error description */
} http_resp;

void https_init(https_conn *c, const char *host);
void https_close(https_conn *c);
/* extra_headers: zero or more "Name: value\r\n" lines, may be NULL. */
int https_request(https_conn *c, const char *method, const char *path, const char *extra_headers, const char *body,
                  size_t body_len, http_resp *r);
void http_resp_free(http_resp *r);

#endif
