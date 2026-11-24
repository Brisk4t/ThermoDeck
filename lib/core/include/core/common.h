#ifndef CORE_COMMON_H
#define CORE_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TD_VERSION "0.1.0"

#define ARRAY_LEN(a)     (sizeof(a) / sizeof((a)[0]))
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

/* Directory holding config + state (default /config/thermodeck, or argv[1]). */
extern char g_dir[256];

uint64_t now_ms(void); /* CLOCK_MONOTONIC in ms */
void tdlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Copies at most dstlen-1 bytes, always terminates. */
void str_copy(char *dst, size_t dstlen, const char *src);
void path_join(char *out, size_t outlen, const char *name); /* g_dir/name */

char *file_read(const char *path, size_t *len); /* malloc'd, NUL-terminated; NULL on error */
int file_write_atomic(const char *path, const void *data, size_t len, int mode);

size_t base64_encode(const unsigned char *in, size_t len, char *out, size_t outlen);
void url_encode(const char *in, char *out, size_t outlen);
void url_decode(const char *in, size_t inlen, char *out, size_t outlen);
/* Find `name=` in a query/form string and url-decode its value. 0 if found. */
int query_param(const char *qs, const char *name, char *out, size_t outlen);
void hex_encode(const unsigned char *in, size_t len, char *out);
int hex_decode(const char *in, unsigned char *out, size_t outlen); /* returns bytes or -1 */
void random_bytes(unsigned char *buf, size_t len);

/* Earliest plausible wall-clock time (build time); used if NTP has not run. */
extern const long g_build_epoch;

#endif
