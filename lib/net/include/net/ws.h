#ifndef NET_WS_H
#define NET_WS_H

/*
 * Minimal WebSocket client (RFC 6455) over plain TCP (ws://, http://) or TLS (wss://,
 * https://). Text messages only; pings are answered automatically. Not thread-safe:
 * one thread owns a connection.
 */

#include <stddef.h>

typedef struct ws_conn ws_conn;

/* Connects to url (scheme://host[:port]) and upgrades path, e.g. "/api/websocket".
 * Returns NULL with a description in err on failure. */
ws_conn *ws_connect(const char *url, const char *path, char *err, size_t errlen);
void ws_close(ws_conn *c);

int ws_send(ws_conn *c, const char *text, size_t len);

/* Waits up to timeout_ms for a message or for extra_fd (may be -1) to become readable.
 * Returns a bitmask: WS_READY (data to receive), WS_EXTRA (extra_fd readable); 0 on timeout. */
enum { WS_READY = 1, WS_EXTRA = 2 };
int ws_wait(ws_conn *c, int extra_fd, int timeout_ms);

/* Receives the next frame. Returns 1 with a malloc'd, NUL-terminated text message in *msg,
 * 0 if it was a control frame (handled), or -1 if the connection is closed / broken. */
int ws_recv(ws_conn *c, char **msg, size_t *len);

#endif
