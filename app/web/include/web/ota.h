#ifndef WEB_OTA_H
#define WEB_OTA_H

#include <stddef.h>

/*
 * Over-the-air updates through the web server (tools/ota.py is the sender).
 *
 * Someone has to be at the thermostat: POST /ota/begin makes the screen show a one-time
 * 6-digit PIN, and the sender proves it knows that PIN when it uploads the binary (POST
 * /ota, signed with the PIN; the PIN itself never crosses the network). The binary goes to
 * thermodeck.new, its SHA-256 and ELF header are verified, the running binary is kept as
 * thermodeck.old, and the new one is renamed into place. ThermoDeck then exits and boot.sh
 * starts the new build. If the new build exits within 30 s, boot.sh puts the old one back
 * (the "ota-pending" marker tells it an update is on trial).
 */

/* Exit status after installing an update: boot.sh restarts without treating it as a crash. */
#define OTA_EXIT_RESTART 75

typedef enum { OTA_IDLE, OTA_WAITING_PIN, OTA_INSTALLING } ota_phase;

/* What the screen shows during an update. */
typedef struct {
    ota_phase phase;
    char pin[7];      /* while OTA_WAITING_PIN */
    char from[16];    /* sender's IP address */
    int seconds_left; /* until the PIN expires */
} ota_view;

/* POST /ota/begin from peer_ip: starts a session and shows the PIN. */
void ota_begin(int fd, const char *peer_ip);

/* Handles POST /ota on connection fd. req..hdr_end are the request headers; `have` bytes
 * of req (headers + the start of the body) are already read. Returns 1 if an update was
 * installed and ThermoDeck should exit(OTA_EXIT_RESTART) so boot.sh restarts it. */
int ota_receive(int fd, const char *req, const char *hdr_end, size_t have);

/* GET /version: version, build time, SHA-256 of the running binary and a random id for this
 * run of ThermoDeck (plain text). */
void ota_send_version(int fd);

/* For the UI (any thread): the current session, and the Cancel button. */
void ota_get_view(ota_view *v);
void ota_cancel(void);

/* Call from the main loop: after 30 s of running, an update on trial counts as good. */
void ota_tick(void);

#endif
