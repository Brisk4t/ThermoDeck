/* Host-side checks for the non-UI modules: make -C . test */
#include <assert.h>
#include <stdlib.h>
#include "common.h"
#include "https.h"
#include "json.h"

static const char *PLAYER =
  "{\"device\":{\"id\":\"x\",\"is_active\":true,\"name\":\"Kitchen \\u00e9 Speaker\",\"volume_percent\":42},"
  "\"shuffle_state\":false,\"progress_ms\":61234,\"item\":{\"album\":{\"name\":\"Alb\\\"um\",\"images\":[{\"url\":\"u\",\"width\":64}]},"
  "\"artists\":[{\"name\":\"A1\",\"id\":\"1\"},{\"name\":\"B\\u00f6\",\"id\":\"2\"}],\"duration_ms\":200000,\"name\":\"Song \\ud83d\\ude00 x\",\"empty\":{},\"arr\":[]},"
  "\"currently_playing_type\":\"track\",\"is_playing\":true,\"context\":null}";

int main(void)
{
    static jtok t[512];
    json_doc d;
    int n = json_parse(&d, PLAYER, strlen(PLAYER), t, 512);
    assert(n > 0);
    char s[256];
    int item = json_get(&d, 0, "item");
    assert(item > 0);
    json_str(&d, json_get(&d, item, "name"), s, sizeof s); printf("title=%s\n", s);
    assert(!strcmp(s, "Song \xF0\x9F\x98\x80 x"));
    json_str(&d, json_path(&d, item, "album.name"), s, sizeof s); assert(!strcmp(s, "Alb\"um"));
    json_str(&d, json_get(&d, json_at(&d, json_get(&d, item, "artists"), 1), "name"), s, sizeof s); assert(!strcmp(s, "B\xC3\xB6"));
    assert(json_at(&d, json_get(&d, item, "artists"), 2) == -1);
    assert(json_int(&d, json_get(&d, item, "duration_ms"), 0) == 200000);
    assert(json_int(&d, json_get(&d, 0, "progress_ms"), 0) == 61234);
    assert(json_bool(&d, json_get(&d, 0, "is_playing"), 0) == 1);
    assert(json_int(&d, json_path(&d, 0, "device.volume_percent"), -1) == 42);
    assert(json_is_null(&d, json_get(&d, 0, "context")));
    assert(json_get(&d, 0, "nope") == -1);
    assert(json_get(&d, json_get(&d, 0, "device"), "is_playing") == -1); /* no leak into siblings */
    char code[128], st[64];
    assert(query_param("http://127.0.0.1:8080/callback?code=AQ%2Bx_y&state=abc" + 31, "code", code, sizeof code) == 0);
    assert(!strcmp(code, "AQ+x_y"));
    assert(query_param("code=1&state=abc", "state", st, sizeof st) == 0 && !strcmp(st, "abc"));
    assert(query_param("xcode=1", "code", st, sizeof st) == -1);
    char b64[32]; base64_encode((const unsigned char *)"ab:cd", 5, b64, sizeof b64); assert(!strcmp(b64, "YWI6Y2Q="));
    puts("json/util OK");

    static https_conn c;
    http_resp r;
    https_init(&c, "api.spotify.com");
    int rc = https_request(&c, "GET", "/v1/me/player", "Authorization: Bearer invalid\r\n", NULL, 0, &r);
    printf("api: rc=%d status=%d body=%.120s\n", rc, r.status, r.body ? r.body : r.error);
    http_resp_free(&r);
    rc = https_request(&c, "GET", "/v1/me/player", "Authorization: Bearer invalid\r\n", NULL, 0, &r);
    printf("api (keep-alive): rc=%d status=%d\n", rc, r.status);
    http_resp_free(&r);
    static https_conn a;
    https_init(&a, "accounts.spotify.com");
    rc = https_request(&a, "POST", "/api/token", "Content-Type: application/x-www-form-urlencoded\r\n", "grant_type=refresh_token&refresh_token=x", 40, &r);
    printf("accounts: rc=%d status=%d body=%.120s\n", rc, r.status, r.body ? r.body : r.error);
    http_resp_free(&r);
    return 0;
}
