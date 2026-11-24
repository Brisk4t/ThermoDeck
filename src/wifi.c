/*
 * Wi-Fi via wpa_supplicant's control socket (the same protocol wpa_cli uses). All calls
 * happen on the UI thread: wifi_poll() is called from the main loop and turns
 * wpa_supplicant's STATUS into a simple connection state for the UI. Networks are only
 * saved to wifi.conf once a connection has actually worked, and only the derived key is
 * stored, never the passphrase.
 */

#include "wifi.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "bearssl.h"
#include "common.h"
#include "config.h"

#define WPA_CTRL_DIR "/var/run/wpa_supplicant"
#define MAX_SAVED    8

/* control socket, current status as shown by the UI */
static int ctrl_fd = -1;
static char local_path[64];
static wifi_status st;

/* saved networks, most recently used first */
static struct {
    char ssid[33];
    char psk[65];
    int secured;
} saved[MAX_SAVED];
static int n_saved;

/* connection state machine timers / bookkeeping */
static uint64_t next_status_ms, next_scan_results_ms, connect_started_ms, dhcp_started_ms, next_auto_scan_ms;
static int dhcp_started;
static struct {
    int active;
    char ssid[33];
    char psk[65];
    int secured;
} pending; /* saved once connected */
static int scan_results_tries;
static int last_was_completed;
/* Wrong-key detection: the 4-way handshake starting and then falling back to scanning. */
static char last_wpa_state[32];
static int handshake_fails;

/* ---------- wpa_supplicant control interface ---------- */

/* Binds a local datagram socket and connects it to wpa_supplicant's socket for wlan0. */
static int ctrl_open(void)
{
    if (ctrl_fd >= 0) return 0;
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un local = {0}, dest = {0};
    local.sun_family = AF_UNIX;
    snprintf(local_path, sizeof local_path, "/tmp/td_wpa_%d", (int)getpid());
    unlink(local_path);
    str_copy(local.sun_path, sizeof local.sun_path, local_path);
    if (bind(fd, (struct sockaddr *)&local, sizeof local) < 0) {
        close(fd);
        return -1;
    }
    dest.sun_family = AF_UNIX;
    snprintf(dest.sun_path, sizeof dest.sun_path, "%s/%s", WPA_CTRL_DIR, WIFI_IFACE);
    if (connect(fd, (struct sockaddr *)&dest, sizeof dest) < 0) {
        close(fd);
        unlink(local_path);
        return -1;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    ctrl_fd = fd;
    tdlog("wifi: connected to wpa_supplicant control socket");
    return 0;
}

static void ctrl_close(void)
{
    if (ctrl_fd >= 0) close(ctrl_fd);
    ctrl_fd = -1;
    unlink(local_path);
}

/* Sends a command, returns reply length (NUL-terminated) or -1. */
static int wpa_req(const char *cmd, char *reply, size_t len)
{
    if (ctrl_open() < 0) return -1;
    /* drop stale datagrams */
    char junk[256];
    while (recv(ctrl_fd, junk, sizeof junk, MSG_DONTWAIT) > 0) {}
    if (send(ctrl_fd, cmd, strlen(cmd), 0) < 0) {
        tdlog("wifi: send '%s' failed: %s", cmd, strerror(errno));
        ctrl_close();
        return -1;
    }
    uint64_t deadline = now_ms() + (strncmp(cmd, "SET_NETWORK", 11) ? 4000 : 10000);
    for (;;) {
        int left = (int)(deadline - now_ms());
        if (left <= 0) {
            tdlog("wifi: timeout on '%.20s'", cmd);
            return -1;
        }
        struct pollfd p = {ctrl_fd, POLLIN, 0};
        if (poll(&p, 1, left) <= 0) continue;
        ssize_t r = recv(ctrl_fd, reply, len - 1, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            ctrl_close();
            return -1;
        }
        reply[r] = 0;
        if (r > 0 && reply[0] == '<') continue; /* unsolicited event */
        return (int)r;
    }
}

/* Sends a formatted command; 0 if wpa_supplicant answered "OK". */
static int wpa_ok(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int wpa_ok(const char *fmt, ...)
{
    char cmd[256], reply[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    int r = wpa_req(cmd, reply, sizeof reply);
    if (r < 0) return -1;
    if (strncmp(reply, "OK", 2) != 0) {
        /* don't log the psk */
        tdlog("wifi: '%.*s' -> %s", (int)(strstr(cmd, " psk ") ? strstr(cmd, " psk ") - cmd : (long)strlen(cmd)), cmd,
              reply);
        return -1;
    }
    return 0;
}

/* ---------- saved networks: g_dir/wifi.conf, one per line: <ssid hex> <secured> <psk hex|-> ---------- */

/* Reads wifi.conf into saved[]. */
static void saved_load(void)
{
    char path[300];
    path_join(path, sizeof path, "wifi.conf");
    char *data = file_read(path, NULL);
    n_saved = 0;
    if (!data) return;
    for (char *line = strtok(data, "\n"); line && n_saved < MAX_SAVED; line = strtok(NULL, "\n")) {
        char ssid_hex[80], psk_hex[140];
        int sec;
        if (sscanf(line, "%79s %d %139s", ssid_hex, &sec, psk_hex) != 3) continue;
        unsigned char buf[70];
        int l = hex_decode(ssid_hex, buf, 32);
        if (l <= 0) continue;
        memcpy(saved[n_saved].ssid, buf, (size_t)l);
        saved[n_saved].ssid[l] = 0;
        saved[n_saved].secured = sec;
        saved[n_saved].psk[0] = 0;
        if (strcmp(psk_hex, "-")) {
            l = hex_decode(psk_hex, buf, 64);
            if (l < 0) continue;
            memcpy(saved[n_saved].psk, buf, (size_t)l);
            saved[n_saved].psk[l] = 0;
        }
        n_saved++;
    }
    free(data);
    tdlog("wifi: %d saved network(s)", n_saved);
}

/* Writes saved[] back to wifi.conf (mode 0600: it holds keys). */
static void saved_store(void)
{
    char path[300], buf[MAX_SAVED * 240];
    size_t o = 0;
    for (int i = 0; i < n_saved; i++) {
        char sh[70], ph[140];
        hex_encode((const unsigned char *)saved[i].ssid, strlen(saved[i].ssid), sh);
        if (saved[i].psk[0])
            hex_encode((const unsigned char *)saved[i].psk, strlen(saved[i].psk), ph);
        else
            strcpy(ph, "-");
        o += (size_t)snprintf(buf + o, sizeof buf - o, "%s %d %s\n", sh, saved[i].secured, ph);
    }
    path_join(path, sizeof path, "wifi.conf");
    if (file_write_atomic(path, buf, o, 0600) < 0) tdlog("wifi: failed to save %s", path);
}

/* Adds or updates a network and moves it to the front of the list. */
static void saved_put_first(const char *ssid, const char *psk, int secured)
{
    int i;
    for (i = 0; i < n_saved; i++)
        if (!strcmp(saved[i].ssid, ssid)) break;
    if (i == n_saved) {
        if (n_saved < MAX_SAVED) n_saved++;
        i = n_saved - 1;
    }
    for (; i > 0; i--) saved[i] = saved[i - 1];
    str_copy(saved[0].ssid, sizeof saved[0].ssid, ssid);
    str_copy(saved[0].psk, sizeof saved[0].psk, psk ? psk : "");
    saved[0].secured = secured;
    saved_store();
}

/* Whether credentials for ssid are saved. */
int wifi_is_saved(const char *ssid)
{
    for (int i = 0; i < n_saved; i++)
        if (!strcmp(saved[i].ssid, ssid)) return 1;
    return 0;
}

/* ---------- helpers ---------- */

/* Marks the status as changed so the UI repaints. */
static void bump(void) { st.version++; }

/* IPv4 address currently assigned to wlan0 ("" if none). */
static void get_ip(char *out, size_t len)
{
    out[0] = 0;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return;
    struct ifreq ifr = {0};
    str_copy(ifr.ifr_name, IFNAMSIZ, WIFI_IFACE);
    if (ioctl(s, SIOCGIFADDR, &ifr) == 0) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
        if (sin->sin_addr.s_addr) inet_ntop(AF_INET, &sin->sin_addr, out, (socklen_t)len);
    }
    close(s);
}

/* wpa_supplicant prints SSIDs with printf_encode(): \\, \", \e, \n, \r, \t, \xHH */
static void ssid_decode(const char *in, char *out, size_t outlen)
{
    size_t o = 0;
    while (*in && o + 1 < outlen) {
        if (*in == '\\' && in[1]) {
            in++;
            switch (*in) {
            case 'n':
                out[o++] = '\n';
                in++;
                break;
            case 'r':
                out[o++] = '\r';
                in++;
                break;
            case 't':
                out[o++] = '\t';
                in++;
                break;
            case 'e':
                out[o++] = 27;
                in++;
                break;
            case 'x': {
                unsigned v = 0;
                if (sscanf(in + 1, "%2x", &v) == 1) {
                    out[o++] = (char)v;
                    in += 3;
                }
                else
                    in++;
                break;
            }
            default: out[o++] = *in++; break;
            }
        }
        else {
            out[o++] = *in++;
        }
    }
    out[o] = 0;
}

/* WPA PSK = PBKDF2-HMAC-SHA1(passphrase, ssid, 4096, 32). Done here because asking
 * wpa_supplicant to do it (SET_NETWORK psk "...") blocks its control socket for seconds. */
static void wpa_psk_hex(const char *pass, const char *ssid, char out[65])
{
    br_hmac_key_context kc;
    br_hmac_key_init(&kc, &br_sha1_vtable, pass, strlen(pass));
    unsigned char key[40];
    size_t sl = strlen(ssid);
    /* two 20-byte PBKDF2 blocks -> the first 32 bytes are the key */
    for (int blk = 1; blk <= 2; blk++) {
        unsigned char u[20], t[20];
        br_hmac_context hc;
        unsigned char be[4] = {0, 0, 0, (unsigned char)blk};
        br_hmac_init(&hc, &kc, 0);
        br_hmac_update(&hc, ssid, sl);
        br_hmac_update(&hc, be, 4);
        br_hmac_out(&hc, u);
        memcpy(t, u, 20);
        for (int i = 1; i < 4096; i++) {
            br_hmac_init(&hc, &kc, 0);
            br_hmac_update(&hc, u, 20);
            br_hmac_out(&hc, u);
            for (int k = 0; k < 20; k++) t[k] ^= u[k];
        }
        memcpy(key + (blk - 1) * 20, t, 20);
    }
    hex_encode(key, 32, out);
}

/* True for an already-derived 64-hex-digit PSK. */
static int is_hex64(const char *s)
{
    if (strlen(s) != 64) return 0;
    for (; *s; s++)
        if (!strchr("0123456789abcdefABCDEF", *s)) return 0;
    return 1;
}

/* Fills in network id's settings: SSID, then WPA-PSK, WEP or open. */
static int write_network(int id, const char *ssid, const char *psk, int secured)
{
    char hex[70];
    hex_encode((const unsigned char *)ssid, strlen(ssid), hex);
    if (wpa_ok("SET_NETWORK %d ssid %s", id, hex) < 0) return -1;
    wpa_ok("SET_NETWORK %d scan_ssid 1", id);
    if (secured == 1 && psk && *psk) {
        char hex[65];
        if (is_hex64(psk))
            str_copy(hex, sizeof hex, psk);
        else
            wpa_psk_hex(psk, ssid, hex); /* old saved entries */
        if (wpa_ok("SET_NETWORK %d psk %s", id, hex)) return -1;
        wpa_ok("SET_NETWORK %d key_mgmt WPA-PSK", id);
    }
    else if (secured == 2 && psk && *psk) {
        wpa_ok("SET_NETWORK %d key_mgmt NONE", id);
        size_t l = strlen(psk);
        if (l == 10 || l == 26)
            wpa_ok("SET_NETWORK %d wep_key0 %s", id, psk);
        else
            wpa_ok("SET_NETWORK %d wep_key0 \"%s\"", id, psk);
        wpa_ok("SET_NETWORK %d wep_tx_keyidx 0", id);
        wpa_ok("SET_NETWORK %d auth_alg OPEN SHARED", id);
    }
    else {
        wpa_ok("SET_NETWORK %d key_mgmt NONE", id);
    }
    return 0;
}

/* Creates a network entry in wpa_supplicant; returns its id or -1. */
static int add_network(const char *ssid, const char *psk, int secured, int priority)
{
    char reply[32];
    if (wpa_req("ADD_NETWORK", reply, sizeof reply) < 0) return -1;
    int id = atoi(reply);
    if (!strncmp(reply, "FAIL", 4)) return -1;
    if (write_network(id, ssid, psk, secured) < 0) {
        wpa_ok("REMOVE_NETWORK %d", id);
        return -1;
    }
    wpa_ok("SET_NETWORK %d priority %d", id, priority);
    return id;
}

/* Asks dhcpcd for an address on wlan0. */
static void start_dhcp(void)
{
    /* dhcpcd 8: -b backgrounds immediately, -n rebinds if already running. */
    tdlog("wifi: starting dhcpcd");
    if (system("dhcpcd -b -n " WIFI_IFACE " >/dev/null 2>&1 &") != 0) tdlog("wifi: dhcpcd launch returned error");
    dhcp_started = 1;
    dhcp_started_ms = now_ms();
}

/* Writes fallback DNS servers if DHCP didn't provide any. */
static void ensure_resolv_conf(void)
{
    char *d = file_read("/etc/resolv.conf", NULL);
    int ok = d && strstr(d, "nameserver");
    free(d);
    if (ok) return;
    const char *dns = config_get("network", "fallback_dns", "1.1.1.1 8.8.8.8");
    char buf[256], tmp[128];
    size_t o = 0;
    str_copy(tmp, sizeof tmp, dns);
    for (char *t = strtok(tmp, " ,"); t; t = strtok(NULL, " ,"))
        o += (size_t)snprintf(buf + o, sizeof buf - o, "nameserver %s\n", t);
    FILE *f = fopen("/etc/resolv.conf", "w");
    if (f) {
        fwrite(buf, 1, o, f);
        fclose(f);
        tdlog("wifi: wrote fallback /etc/resolv.conf");
    }
}

/* ---------- public API ---------- */

/*
 * Connects to wpa_supplicant (waiting up to 20 s for it), resets its configuration and
 * starts connecting to the saved networks, best first.
 */
int wifi_init(void)
{
    memset(&st, 0, sizeof st);
    saved_load();
    for (int tries = 0; tries < 20 && ctrl_open() < 0; tries++) {
        tdlog("wifi: waiting for wpa_supplicant...");
        sleep(1);
    }
    if (ctrl_fd < 0) {
        str_copy(st.message, sizeof st.message, "wpa_supplicant not running");
        st.state = WS_FAILED;
        return -1;
    }
    /* Start from a clean station-mode configuration (the stock app may have left an AP up). */
    wpa_ok("DISCONNECT");
    wpa_ok("REMOVE_NETWORK all");
    wpa_ok("AP_SCAN 1");
    if (n_saved) {
        for (int i = 0; i < n_saved; i++) {
            int id = add_network(saved[i].ssid, saved[i].psk, saved[i].secured, MAX_SAVED - i);
            if (id >= 0) wpa_ok("ENABLE_NETWORK %d", id);
        }
        wpa_ok("REASSOCIATE");
        st.state = WS_CONNECTING;
        str_copy(st.ssid, sizeof st.ssid, saved[0].ssid);
        connect_started_ms = now_ms();
        handshake_fails = 0;
    }
    wifi_scan();
    bump();
    return 0;
}

const wifi_status *wifi_get(void) { return &st; }

/* Starts a scan; results are collected a few seconds later by wifi_poll(). */
void wifi_scan(void)
{
    char reply[32];
    if (wpa_req("SCAN", reply, sizeof reply) < 0) return;
    st.scanning = 1;
    scan_results_tries = 0;
    next_scan_results_ms = now_ms() + 2500;
    bump();
}

/* Parses SCAN_RESULTS into st.aps: one entry per SSID (strongest BSS), sorted by signal. */
static void fetch_scan_results(void)
{
    static char buf[8192];
    if (wpa_req("SCAN_RESULTS", buf, sizeof buf) < 0) return;
    wifi_ap aps[WIFI_MAX_AP];
    int n = 0;
    char *save = NULL;
    char *line = strtok_r(buf, "\n", &save); /* header */
    while ((line = strtok_r(NULL, "\n", &save)) && n < WIFI_MAX_AP) {
        /* bssid \t freq \t signal \t flags \t ssid */
        char *f[5] = {0};
        int k = 0;
        char *p = line;
        while (k < 5) {
            f[k++] = p;
            if (k == 5) break;
            p = strchr(p, '\t');
            if (!p) break;
            *p++ = 0;
        }
        if (k < 5 || !f[4][0]) continue;
        char ssid[33];
        ssid_decode(f[4], ssid, sizeof ssid);
        if (!ssid[0]) continue;
        int sig = atoi(f[2]);
        if (sig > 0) sig = sig / 2 - 100; /* some drivers report quality */
        /* security: 0 open, 1 WPA-PSK, 2 WEP, 3 unsupported (enterprise / WPA3-only) */
        int sec = 0;
        if (strstr(f[3], "EAP"))
            sec = 3;
        else if (strstr(f[3], "PSK"))
            sec = 1;
        else if (strstr(f[3], "WEP"))
            sec = 2;
        else if (strstr(f[3], "SAE"))
            sec = 3;
        /* several access points with the same name: keep the strongest */
        int dup = -1;
        for (int i = 0; i < n; i++)
            if (!strcmp(aps[i].ssid, ssid)) dup = i;
        if (dup >= 0) {
            if (sig > aps[dup].signal) aps[dup].signal = sig;
            continue;
        }
        str_copy(aps[n].ssid, sizeof aps[n].ssid, ssid);
        aps[n].signal = sig;
        aps[n].secured = sec;
        n++;
    }
    /* sort by signal, strongest first */
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && aps[j].signal > aps[j - 1].signal; j--) {
            wifi_ap t = aps[j];
            aps[j] = aps[j - 1];
            aps[j - 1] = t;
        }
    /* an empty result usually means the scan hasn't finished; retry a couple of times */
    if (n > 0 || scan_results_tries >= 3) {
        memcpy(st.aps, aps, sizeof(wifi_ap) * (size_t)n);
        st.n_aps = n;
        st.scanning = 0;
        bump();
    }
    else {
        next_scan_results_ms = now_ms() + 2000;
    }
}

/* Connects to ssid. For WPA the passphrase is turned into the hex PSK here (~150 ms). */
void wifi_connect(const char *ssid, const char *psk, int secured)
{
    tdlog("wifi: connecting to '%s' (sec=%d)", ssid, secured);
    char hexpsk[65];
    if (secured == 1 && psk && *psk && !is_hex64(psk)) {
        uint64_t t0 = now_ms();
        wpa_psk_hex(psk, ssid, hexpsk);
        tdlog("wifi: derived PSK in %llums", (unsigned long long)(now_ms() - t0));
        psk = hexpsk;
    }
    wpa_ok("DISCONNECT");
    wpa_ok("REMOVE_NETWORK all");
    int id = add_network(ssid, psk, secured, 10);
    if (id < 0) {
        st.state = WS_FAILED;
        str_copy(st.message, sizeof st.message, "Could not configure network");
        bump();
        return;
    }
    /* keep other saved networks as fallbacks, at lower priority */
    for (int i = 0; i < n_saved; i++) {
        if (!strcmp(saved[i].ssid, ssid)) continue;
        int oid = add_network(saved[i].ssid, saved[i].psk, saved[i].secured, 1);
        if (oid >= 0) wpa_ok("DISABLE_NETWORK %d", oid);
    }
    wpa_ok("SELECT_NETWORK %d", id);
    wpa_ok("REASSOCIATE");
    st.state = WS_CONNECTING;
    str_copy(st.ssid, sizeof st.ssid, ssid);
    st.ip[0] = 0;
    st.message[0] = 0;
    st.auth_failed = 0;
    connect_started_ms = now_ms();
    handshake_fails = 0;
    dhcp_started = 0;
    last_was_completed = 0;
    /* remember the credentials once the connection actually works (see wifi_poll) */
    pending.active = 1;
    str_copy(pending.ssid, sizeof pending.ssid, ssid);
    str_copy(pending.psk, sizeof pending.psk, psk ? psk : ""); /* WPA: the derived PSK, never the passphrase */
    pending.secured = secured;
    bump();
}

/* Removes a saved network, disconnecting if it's the current one. */
void wifi_forget(const char *ssid)
{
    int j = 0;
    for (int i = 0; i < n_saved; i++)
        if (strcmp(saved[i].ssid, ssid)) saved[j++] = saved[i];
    n_saved = j;
    saved_store();
    if (!strcmp(st.ssid, ssid)) {
        wpa_ok("DISCONNECT");
        wpa_ok("REMOVE_NETWORK all");
        st.state = WS_IDLE;
        st.ssid[0] = 0;
        st.ip[0] = 0;
        bump();
    }
}

/*
 * Called from the UI loop. Handles scan results, refreshes the list now and then while
 * disconnected, and once a second maps wpa_supplicant's state to ours:
 *   associated + IP -> CONNECTED, associated -> DHCP, otherwise CONNECTING / FAILED.
 */
void wifi_poll(void)
{
    uint64_t now = now_ms();
    if (st.scanning && now >= next_scan_results_ms) {
        scan_results_tries++;
        fetch_scan_results();
    }
    if (!st.scanning && now >= next_auto_scan_ms && st.state != WS_CONNECTED && st.state != WS_DHCP) {
        /* keep the list fresh while the user is choosing */
        next_auto_scan_ms = now + 20000;
        if (st.n_aps == 0 || st.state != WS_CONNECTING) wifi_scan();
    }
    if (now < next_status_ms) return;
    next_status_ms = now + 1000;

    char buf[1024];
    if (wpa_req("STATUS", buf, sizeof buf) < 0) {
        if (st.state != WS_FAILED || strcmp(st.message, "wpa_supplicant not responding")) {
            st.state = WS_FAILED;
            str_copy(st.message, sizeof st.message, "wpa_supplicant not responding");
            bump();
        }
        return;
    }
    /* pull wpa_state and ssid out of the STATUS reply */
    char wpa_state[32] = "", ssid[33] = "";
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (!strncmp(line, "wpa_state=", 10))
            str_copy(wpa_state, sizeof wpa_state, line + 10);
        else if (!strncmp(line, "ssid=", 5))
            ssid_decode(line + 5, ssid, sizeof ssid);
    }
    int completed = !strcmp(wpa_state, "COMPLETED");
    /* count handshakes that started and then fell back: that's how a wrong key shows up */
    int in_handshake = !strcmp(wpa_state, "4WAY_HANDSHAKE") || !strcmp(wpa_state, "GROUP_HANDSHAKE");
    int was_handshake = !strcmp(last_wpa_state, "4WAY_HANDSHAKE") || !strcmp(last_wpa_state, "GROUP_HANDSHAKE");
    if (was_handshake && !in_handshake && !completed) {
        handshake_fails++;
        tdlog("wifi: handshake failed (%d), now %s", handshake_fails, wpa_state);
    }
    if (completed) handshake_fails = 0;
    str_copy(last_wpa_state, sizeof last_wpa_state, wpa_state);
    char ip[16];
    get_ip(ip, sizeof ip);

    /* work out the new state */
    wifi_state ns = st.state;
    if (completed) {
        if (ssid[0] && strcmp(ssid, st.ssid)) str_copy(st.ssid, sizeof st.ssid, ssid);
        if (!last_was_completed) {
            tdlog("wifi: associated with '%s'", st.ssid);
            start_dhcp();
        }
        if (ip[0]) {
            ns = WS_CONNECTED;
            if (st.state != WS_CONNECTED) {
                tdlog("wifi: connected, ip %s", ip);
                ensure_resolv_conf();
                if (pending.active && !strcmp(pending.ssid, st.ssid)) {
                    saved_put_first(pending.ssid, pending.psk, pending.secured);
                    pending.active = 0;
                }
            }
        }
        else {
            ns = WS_DHCP;
            if (dhcp_started && now - dhcp_started_ms > 30000) {
                ns = WS_FAILED;
                str_copy(st.message, sizeof st.message, "No IP address (DHCP failed)");
                start_dhcp(); /* keep trying in the background */
            }
        }
    }
    else {
        if (last_was_completed) tdlog("wifi: link lost (state %s)", wpa_state);
        /* not associated: lost link, gave up, or wpa_supplicant reconnecting on its own */
        if (st.state == WS_CONNECTED || st.state == WS_DHCP) {
            ns = WS_CONNECTING;
            connect_started_ms = now;
        }
        else if (st.state == WS_CONNECTING && (handshake_fails >= 2 || now - connect_started_ms > 30000)) {
            ns = WS_FAILED;
            st.auth_failed = handshake_fails >= 1 || in_handshake || !strcmp(wpa_state, "ASSOCIATED");
            str_copy(st.message, sizeof st.message, st.auth_failed ? "Wrong password" : "Network not reachable");
        }
        else if (st.state == WS_IDLE && strcmp(wpa_state, "DISCONNECTED") && strcmp(wpa_state, "INACTIVE") &&
                 strcmp(wpa_state, "SCANNING") && wpa_state[0]) {
            ns = WS_CONNECTING;
            connect_started_ms = now;
        }
    }
    last_was_completed = completed;
    if (ns == WS_FAILED && completed && ip[0]) ns = WS_CONNECTED;
    /* publish changes */
    if (ns != st.state || strcmp(ip, st.ip)) {
        st.state = ns;
        str_copy(st.ip, sizeof st.ip, ip);
        if (ns == WS_CONNECTED) {
            st.message[0] = 0;
            st.auth_failed = 0;
        }
        bump();
    }
    /* signal of the current AP from the scan list */
    int sig = 0;
    for (int i = 0; i < st.n_aps; i++)
        if (!strcmp(st.aps[i].ssid, st.ssid)) sig = st.aps[i].signal;
    if (sig != st.signal) {
        st.signal = sig;
        bump();
    }
}

/* Reconnects to a saved network with its stored key. */
int wifi_connect_saved(const char *ssid)
{
    for (int i = 0; i < n_saved; i++) {
        if (!strcmp(saved[i].ssid, ssid)) {
            char psk[65];
            str_copy(psk, sizeof psk, saved[i].psk);
            wifi_connect(ssid, psk, saved[i].secured);
            return 0;
        }
    }
    return -1;
}
