/*
 * Over-the-air updates (see web/ota.h for the overview).
 *
 *   POST /ota/begin   -> 200 "nonce <32 hex>\nexpires <seconds>\n"; the screen shows a PIN
 *   POST /ota         the binary, with these headers:
 *     Content-Length: <size>
 *     X-OTA-Nonce:    <nonce from /ota/begin>
 *     X-OTA-SHA256:   <hex SHA-256 of the binary>
 *     X-OTA-Auth:     <hex HMAC-SHA256(key = the 6 PIN digits,
 *                                      "thermodeck-ota-v2\n<nonce>\n<size>\n<sha256>\n")>
 *
 * The PIN only travels from the screen to the person sending the update. Each session's
 * PIN and nonce work once, expire after OTA_PIN_SECONDS, and OTA_MAX_TRIES wrong attempts
 * end the session; a new session can then only start after OTA_COOLDOWN_MS. The
 * authorization covers the binary's hash, so it can't be reused for a different binary.
 */

#include "web/ota.h"

#include <ctype.h>
#include <pthread.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "bearssl.h"
#include "core/common.h"
#include "ui/ui.h"

#define OTA_MAX_SIZE    (8 * 1024 * 1024)
#define OTA_TRIAL_MS    30000 /* an update that runs this long is kept */
#define OTA_PIN_SECONDS 120   /* how long a PIN stays valid */
#define OTA_MAX_TRIES   3     /* wrong PINs before the session ends */
#define OTA_COOLDOWN_MS 60000 /* after a session ended by wrong PINs */
#define OTA_AUTH_LABEL  "thermodeck-ota-v2"

/* The current session, shared by the web thread and the UI (which shows the PIN). */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static ota_phase phase;
static char pin[7], nonce[33], from[16];
static uint64_t expires_ms, cooldown_until_ms;
static int tries;

/* Ends the session (call with mu held). */
static void end_session(void)
{
    phase = OTA_IDLE;
    memset(pin, 0, sizeof pin);
    nonce[0] = 0;
}

/* Expires a waiting session whose time ran out (call with mu held). */
static void expire(void)
{
    if (phase == OTA_WAITING_PIN && now_ms() >= expires_ms) end_session();
}

/* Plain-text response, then the connection is closed by the caller. */
static void reply(int fd, int status, const char *text)
{
    char head[256];
    const char *st = status == 200 ? "OK" : status == 403 ? "Forbidden" : "Bad Request";
    int n = snprintf(head, sizeof head,
                     "HTTP/1.0 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                     status, st, strlen(text));
    send(fd, head, (size_t)n, MSG_NOSIGNAL);
    send(fd, text, strlen(text), MSG_NOSIGNAL);
    if (status != 200) tdlog("ota: rejected: %s", text);
}

/* Rejects a request before its body was read: reads and discards the rest of the upload
 * first (bounded by OTA_MAX_SIZE and the socket timeout), otherwise closing the socket
 * resets the connection and the sender never sees the reason. */
static void reject_early(int fd, int status, const char *text, long long left)
{
    static char buf[16384];
    if (left > OTA_MAX_SIZE) left = OTA_MAX_SIZE;
    while (left > 0) {
        ssize_t r = recv(fd, buf, left < (long long)sizeof buf ? (size_t)left : sizeof buf, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        left -= r;
    }
    reply(fd, status, text);
}

/* Copies header `name`'s value (trimmed) into out; "" if it's not among the headers. */
static void get_header(const char *req, const char *hdr_end, const char *name, char *out, size_t outlen)
{
    out[0] = 0;
    size_t nl = strlen(name);
    for (const char *p = strstr(req, "\r\n"); p && p < hdr_end; p = strstr(p + 2, "\r\n")) {
        if (strncasecmp(p + 2, name, nl) || p[2 + nl] != ':') continue;
        const char *v = p + 3 + nl;
        while (*v == ' ') v++;
        size_t l = strcspn(v, "\r\n");
        if (l >= outlen) l = outlen - 1;
        memcpy(out, v, l);
        out[l] = 0;
        while (l && out[l - 1] == ' ') out[--l] = 0;
        return;
    }
}

/* Constant-time comparison of two equal-length strings. */
static int same(const char *a, const char *b, size_t n)
{
    unsigned char d = 0;
    for (size_t i = 0; i < n; i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

/* The update must be an executable for this machine: same ELF class, byte order and CPU
 * as the binary that is running now. */
static int elf_matches_self(const char *path)
{
    unsigned char a[20], b[20];
    int fa = open(path, O_RDONLY | O_CLOEXEC), fb = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    /* e_ident: magic, class, data, version; then e_machine at offset 18 */
    int ok = fa >= 0 && fb >= 0 && read(fa, a, 20) == 20 && read(fb, b, 20) == 20 && !memcmp(a, ELFMAG, SELFMAG) &&
             !memcmp(a + EI_CLASS, b + EI_CLASS, 3) && a[18] == b[18] && a[19] == b[19];
    if (fa >= 0) close(fa);
    if (fb >= 0) close(fb);
    return ok;
}

/* Receives `size` body bytes (the first `have` already in `start`) into path, hashing them. */
static int receive_body(int fd, const char *start, size_t have, size_t size, const char *path, char hash_hex[65])
{
    int out = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0755);
    if (out < 0) return -1;
    br_sha256_context sha;
    br_sha256_init(&sha);
    static char buf[16384];
    size_t got = 0;
    int ok = 1;
    if (have > size) have = size;
    if (have) {
        br_sha256_update(&sha, start, have);
        ok = write(out, start, have) == (ssize_t)have;
        got = have;
    }
    while (ok && got < size) {
        size_t want = size - got < sizeof buf ? size - got : sizeof buf;
        ssize_t r = recv(fd, buf, want, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            ok = 0;
            break;
        }
        br_sha256_update(&sha, buf, (size_t)r);
        ok = write(out, buf, (size_t)r) == r;
        got += (size_t)r;
    }
    ok = ok && fsync(out) == 0;
    close(out);
    unsigned char h[32];
    br_sha256_out(&sha, h);
    hex_encode(h, 32, hash_hex);
    return ok ? 0 : -1;
}

/* thermodeck.new -> thermodeck, keeping the current one as thermodeck.old. The rename is
 * atomic, so a power cut leaves either the old or the new binary in place. */
static int install(const char *newp)
{
    char cur[300], old[300], mark[300];
    path_join(cur, sizeof cur, "thermodeck");
    path_join(old, sizeof old, "thermodeck.old");
    path_join(mark, sizeof mark, "ota-pending");
    unlink(old);
    if (link(cur, old) < 0 && errno != ENOENT) tdlog("ota: couldn't keep the old binary: %s", strerror(errno));
    if (rename(newp, cur) < 0) return -1;
    file_write_atomic(mark, "1\n", 2, 0644);
    sync();
    return 0;
}

void ota_begin(int fd, const char *peer_ip)
{
    char text[120];
    pthread_mutex_lock(&mu);
    expire();
    uint64_t now = now_ms();
    if (phase != OTA_IDLE) {
        pthread_mutex_unlock(&mu);
        reply(fd, 403, "Another update is already in progress on the thermostat.\n");
        return;
    }
    if (now < cooldown_until_ms) {
        int wait = (int)((cooldown_until_ms - now) / 1000) + 1;
        pthread_mutex_unlock(&mu);
        snprintf(text, sizeof text, "Too many wrong PINs; try again in %d s.\n", wait);
        reply(fd, 403, text);
        return;
    }
    unsigned char r[20];
    random_bytes(r, sizeof r);
    uint32_t v = ((uint32_t)r[0] << 24 | (uint32_t)r[1] << 16 | (uint32_t)r[2] << 8 | r[3]) % 1000000u;
    snprintf(pin, sizeof pin, "%06u", (unsigned)v);
    hex_encode(r + 4, 16, nonce);
    str_copy(from, sizeof from, peer_ip);
    expires_ms = now + OTA_PIN_SECONDS * 1000;
    tries = 0;
    phase = OTA_WAITING_PIN;
    snprintf(text, sizeof text, "nonce %s\nexpires %d\n", nonce, OTA_PIN_SECONDS);
    pthread_mutex_unlock(&mu);
    tdlog("ota: update requested from %s, PIN on screen", peer_ip);
    ui_wake();
    reply(fd, 200, text);
}

/* Checks X-OTA-Auth against the session's PIN. Returns 0 if it matches (the session then
 * moves to installing), otherwise fills `why` and returns -1. */
static int authorize(const char *nonce_hdr, long long size, const char *sha_hdr, const char *auth_hdr, char *why,
                     size_t whylen)
{
    pthread_mutex_lock(&mu);
    expire();
    if (phase != OTA_WAITING_PIN || strlen(nonce_hdr) != 32 || !same(nonce_hdr, nonce, 32)) {
        pthread_mutex_unlock(&mu);
        str_copy(why, whylen, "No update is waiting for this request (it may have expired); start again.\n");
        return -1;
    }
    char msg[200], expect[65];
    int ml = snprintf(msg, sizeof msg, "%s\n%s\n%lld\n%s\n", OTA_AUTH_LABEL, nonce, size, sha_hdr);
    br_hmac_key_context kc;
    br_hmac_context hc;
    unsigned char mac[32];
    br_hmac_key_init(&kc, &br_sha256_vtable, pin, 6);
    br_hmac_init(&hc, &kc, 0);
    br_hmac_update(&hc, msg, (size_t)ml);
    br_hmac_out(&hc, mac);
    hex_encode(mac, 32, expect);
    if (!same(expect, auth_hdr, 64)) {
        int left = OTA_MAX_TRIES - ++tries;
        if (left <= 0) {
            end_session();
            cooldown_until_ms = now_ms() + OTA_COOLDOWN_MS;
            snprintf(why, whylen, "Wrong PIN. Too many tries: the request was cancelled.\n");
        }
        else
            snprintf(why, whylen, "Wrong PIN (%d %s left).\n", left, left == 1 ? "try" : "tries");
        pthread_mutex_unlock(&mu);
        ui_wake();
        sleep(1); /* slow down guessing */
        return -1;
    }
    phase = OTA_INSTALLING; /* the PIN is used up */
    memset(pin, 0, sizeof pin);
    pthread_mutex_unlock(&mu);
    ui_wake();
    return 0;
}

/* The upload failed after the PIN was accepted: back to normal. */
static void install_failed(void)
{
    pthread_mutex_lock(&mu);
    end_session();
    pthread_mutex_unlock(&mu);
    ui_wake();
}

int ota_receive(int fd, const char *req, const char *hdr_end, size_t have)
{
    struct timeval tv = {15, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    char cl[24], nonce_hdr[48], sha_hdr[80], auth_hdr[80];
    get_header(req, hdr_end, "Content-Length", cl, sizeof cl);
    get_header(req, hdr_end, "X-OTA-Nonce", nonce_hdr, sizeof nonce_hdr);
    get_header(req, hdr_end, "X-OTA-SHA256", sha_hdr, sizeof sha_hdr);
    get_header(req, hdr_end, "X-OTA-Auth", auth_hdr, sizeof auth_hdr);
    long long size = atoll(cl);
    long long left = size - (long long)(have - (size_t)(hdr_end + 4 - req)); /* body bytes not read yet */
    for (char *c = sha_hdr; *c; c++) *c = (char)tolower((unsigned char)*c);
    for (char *c = nonce_hdr; *c; c++) *c = (char)tolower((unsigned char)*c);
    for (char *c = auth_hdr; *c; c++) *c = (char)tolower((unsigned char)*c);

    if (size <= 0 || size > OTA_MAX_SIZE || strlen(sha_hdr) != 64 || strlen(auth_hdr) != 64) {
        reject_early(fd, 400, "Missing or invalid OTA headers.\n", left);
        return 0;
    }
    /* the PIN check comes before anything touches the flash */
    char why[120];
    if (authorize(nonce_hdr, size, sha_hdr, auth_hdr, why, sizeof why) < 0) {
        reject_early(fd, 403, why, left);
        return 0;
    }

    /* the binary: receive, hash, check it's an executable for this CPU */
    char newp[300], got_hash[65];
    path_join(newp, sizeof newp, "thermodeck.new");
    tdlog("ota: receiving %lld bytes", size);
    if (receive_body(fd, hdr_end + 4, have - (size_t)(hdr_end + 4 - req), (size_t)size, newp, got_hash) < 0) {
        unlink(newp);
        install_failed();
        reply(fd, 400, "Upload failed (connection dropped or flash write error).\n");
        return 0;
    }
    if (!same(got_hash, sha_hdr, 64)) {
        unlink(newp);
        install_failed();
        reply(fd, 400, "SHA-256 mismatch: the upload was corrupted.\n");
        return 0;
    }
    if (!elf_matches_self(newp)) {
        unlink(newp);
        install_failed();
        reply(fd, 400, "Not an executable for this device.\n");
        return 0;
    }
    if (install(newp) < 0) {
        unlink(newp);
        install_failed();
        reply(fd, 400, "Couldn't install the new binary.\n");
        return 0;
    }
    tdlog("ota: installed %.12s, restarting", got_hash);
    reply(fd, 200, "Installed. ThermoDeck is restarting.\n");
    return 1;
}

void ota_send_version(int fd)
{
    /* hash the running binary once; it can't change under us (an update replaces the file) */
    static char hash[65];
    if (!hash[0]) {
        int f = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
        if (f >= 0) {
            br_sha256_context sha;
            br_sha256_init(&sha);
            static char buf[16384];
            ssize_t r;
            while ((r = read(f, buf, sizeof buf)) > 0) br_sha256_update(&sha, buf, (size_t)r);
            close(f);
            unsigned char h[32];
            br_sha256_out(&sha, h);
            hex_encode(h, 32, hash);
        }
    }
    /* random per run, so a sender can tell a restart happened even when the build is the same */
    static char instance[17];
    if (!instance[0]) {
        unsigned char r[8];
        random_bytes(r, sizeof r);
        hex_encode(r, sizeof r, instance);
    }
    char text[240];
    snprintf(text, sizeof text, "ThermoDeck %s\nbuild %ld\nsha256 %s\ninstance %s\n", TD_VERSION, g_build_epoch,
             hash[0] ? hash : "unknown", instance);
    reply(fd, 200, text);
}

void ota_get_view(ota_view *v)
{
    pthread_mutex_lock(&mu);
    expire();
    v->phase = phase;
    memcpy(v->pin, pin, sizeof v->pin);
    memcpy(v->from, from, sizeof v->from);
    uint64_t now = now_ms();
    v->seconds_left = phase == OTA_WAITING_PIN && expires_ms > now ? (int)((expires_ms - now + 999) / 1000) : 0;
    pthread_mutex_unlock(&mu);
}

void ota_cancel(void)
{
    pthread_mutex_lock(&mu);
    if (phase == OTA_WAITING_PIN) {
        end_session();
        tdlog("ota: cancelled on the thermostat");
    }
    pthread_mutex_unlock(&mu);
}

void ota_tick(void)
{
    static uint64_t start;
    static int done;
    if (done) return;
    uint64_t now = now_ms();
    if (!start) start = now;
    if (now - start < OTA_TRIAL_MS) return;
    done = 1;
    char mark[300];
    path_join(mark, sizeof mark, "ota-pending");
    if (unlink(mark) == 0) tdlog("ota: update confirmed after %d s", OTA_TRIAL_MS / 1000);
}
