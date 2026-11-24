#ifndef SPOTIFY_SPOTIFY_H
#define SPOTIFY_SPOTIFY_H
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SP_WAIT_NETWORK, /* no Wi-Fi yet */
    SP_NO_CONFIG,    /* client_id / client_secret missing */
    SP_NEED_AUTH,    /* waiting for the user to authorize via the web page */
    SP_AUTHORIZING,  /* exchanging the code / refreshing */
    SP_READY,
} sp_auth_state;

enum { CMD_NEXT = 1, CMD_PREV = 2, CMD_VOLUME = 4, CMD_LOGOUT = 8, CMD_PLAY = 16, CMD_PAUSE = 32 };

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    uint32_t version; /* bumped on any change visible to the UI */

    /* inputs from the UI / web threads */
    int net_up; /* Wi-Fi connected with an IP */
    char ip[16];
    char wifi_ssid[33]; /* for the web status page */
    int wifi_signal;
    int cmds;             /* CMD_* bitmask */
    int volume_target;    /* for CMD_VOLUME */
    char auth_code[512];  /* set by the web server after the user authorizes */
    char oauth_state[40]; /* anti-CSRF state for the authorize URL */

    /* outputs */
    sp_auth_state auth;
    char auth_msg[128]; /* last auth error / hint */
    int time_synced;

    int has_item;
    char title[256], artist[256], album[256], device[64];
    int is_playing;
    int duration_ms, progress_ms;
    uint64_t sync_ms;           /* now_ms() at which progress_ms was valid */
    int volume;                 /* -1 unknown */
    int supports_volume;        /* device allows remote volume (Web API supports_volume) */
    uint64_t play_hold_until;   /* don't let polls override an optimistic play/pause */
    uint64_t volume_hold_until; /* don't let polls override an optimistic volume */
    int last_poll_ok;
    char toast[96]; /* transient message (e.g. "Premium required") */
    uint64_t toast_until;
} sp_shared;

extern sp_shared g_sp;

void spotify_start(void);
/* UI helpers (lock internally) */
void spotify_command(int cmd);
void spotify_volume_step(int delta);
/* Toggle play/pause (optimistic). */
void spotify_toggle_play(void);
void spotify_set_net(int up, const char *ip, const char *ssid, int signal);
/* From the web server: an authorization code (state is checked if non-empty). 0 if accepted. */
int spotify_submit_code(const char *code, const char *state);
/* Builds the authorize URL (for the web page). Returns 0 if configured. */
int spotify_authorize_url(char *out, size_t outlen);
const char *spotify_redirect_uri(void);
int spotify_configured(void);
/* Current interpolated progress in ms (caller holds no lock). */
int spotify_progress_now(void);

#endif
