#ifndef WIFI_H
#define WIFI_H
#include <stdint.h>

#define WIFI_MAX_AP 32

typedef struct {
    char ssid[33];
    int signal;  /* dBm */
    int secured; /* 0 open, 1 WPA/WPA2-PSK, 2 WEP, 3 unsupported (EAP) */
} wifi_ap;

typedef enum {
    WS_IDLE,       /* no network configured */
    WS_CONNECTING, /* associating */
    WS_DHCP,       /* associated, waiting for an IP */
    WS_CONNECTED,
    WS_FAILED,
} wifi_state;

typedef struct {
    wifi_state state;
    char ssid[33];     /* network being connected / connected */
    char ip[16];
    char message[96];  /* failure reason */
    int signal;        /* dBm of current AP, 0 unknown */
    wifi_ap aps[WIFI_MAX_AP];
    int n_aps;
    int scanning;
    uint32_t version;  /* bumped whenever anything above changes */
} wifi_status;

int wifi_init(void);
/* Drives the state machine; call frequently (it rate-limits itself). */
void wifi_poll(void);
const wifi_status *wifi_get(void);
void wifi_scan(void);
/* psk may be NULL/empty for open networks. */
void wifi_connect(const char *ssid, const char *psk, int secured);
/* Forget the given network (remove from saved list) and disconnect if current. */
void wifi_forget(const char *ssid);
int wifi_is_saved(const char *ssid);
/* Reconnect using stored credentials. 0 if the network was saved. */
int wifi_connect_saved(const char *ssid);

#endif
