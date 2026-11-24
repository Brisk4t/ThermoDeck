#ifndef HASS_HASS_H
#define HASS_HASS_H

/*
 * Home Assistant client. A worker thread keeps a WebSocket open to HA, loads a Lovelace
 * dashboard (layout defined and edited in HA), subscribes to the states of the entities
 * it shows, and calls services when the UI taps something. Everything the UI shows is
 * in g_ha, guarded by g_ha.mu.
 */

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    HA_OFF,          /* no url / token in thermodeck.ini */
    HA_WAIT_NETWORK, /* no Wi-Fi yet */
    HA_CONNECTING,
    HA_AUTH_FAILED, /* token rejected */
    HA_NO_DASHBOARD,
    HA_READY,
    HA_ERROR, /* can't reach HA; retrying (see msg) */
} ha_status;

#define HA_MAX_ENTS  96
#define HA_MAX_ITEMS 160

typedef struct {
    char id[64]; /* entity_id */
    char name[64];
    char state[32];
    char unit[16];
    char icon[48];
    char device_class[24];
    char extra[48];      /* secondary info, e.g. current temperature or media title */
    int brightness;      /* 0..255 for lights that are on, -1 if not reported */
    int has_state;       /* a state has arrived */
    char real_state[32]; /* last state from HA; `state` may be an optimistic guess after a tap */
    uint64_t hold_until; /* when to give up on the guess and show real_state again (ms) */
} ha_entity;

/* Dashboard items in display order; containers (stacks, grids, sections) are flattened. */
typedef enum {
    HI_VIEW,       /* a dashboard view starts (new page); text = title */
    HI_BREAK,      /* start a new row (between sections) */
    HI_HEADING,    /* text */
    HI_TILE,       /* entity: icon, name, state */
    HI_BUTTON,     /* entity (optional): big icon + name */
    HI_SENSOR,     /* entity: name + big value */
    HI_ROW,        /* entity row of an entities card */
    HI_TEXT,       /* markdown card (as plain text) */
    HI_UNSUPPORTED /* text = card type */
} ha_kind;

typedef enum {
    HA_ACT_DEFAULT, /* what the entity's domain usually does on tap (toggle, run, ...) */
    HA_ACT_NONE,
    HA_ACT_TOGGLE,
    HA_ACT_CALL, /* service + data */
} ha_action;

typedef struct {
    uint8_t kind, span, rows, act; /* span: width in 12ths; rows: height in grid rows */
    uint8_t small;                 /* heading: subtitle style */
    uint8_t vertical;              /* tile: icon above the text */
    int16_t ent;                   /* index into ents, or -1 */
    char name[48];                 /* override, "" = the entity's name */
    char icon[48];                 /* override, "" = the entity's icon */
    char text[192];
    char service[48]; /* HA_ACT_CALL: "domain.service" */
    char data[256];   /* HA_ACT_CALL: extra JSON members, e.g. "\"target\":{...}" */
} ha_item;

typedef struct {
    ha_item items[HA_MAX_ITEMS];
    int n_items;
    ha_entity ents[HA_MAX_ENTS];
    int n_ents;
} ha_dash;

typedef struct {
    pthread_mutex_t mu;
    ha_status status;
    char msg[160];       /* status detail / setup hint */
    uint32_t version;    /* bumped on any change visible to the UI */
    uint32_t layout_ver; /* bumped when the items change (dashboard reloaded) */
    ha_dash d;
    char toast[96];
    uint64_t toast_until;
} ha_shared;

extern ha_shared g_ha;

void hass_start(void);
/* Wi-Fi up / down (from the main loop). */
void hass_set_net(int up);
/* The UI tapped item i: run its action (optimistic for toggles). */
void hass_press(int item);

/* ---- dashboard.c ---- */
/* Parses a lovelace/config result into items + the entity list (states empty).
 * Returns 0, or -1 with a reason in err. */
int ha_dashboard_parse(const char *js, size_t len, ha_dash *out, char *err, size_t errlen);
/* Dashboard of tiles for a comma-separated entity list (fallback when HA has none). */
void ha_dashboard_from_list(const char *list, ha_dash *out);

/* ---- entity.c: how entities look and behave (shared by the UI and the worker) ---- */
int ha_entity_is_on(const ha_entity *e);   /* "active" state: light on, door open, playing ... */
int ha_entity_toggles(const ha_entity *e); /* tapping toggles it (shows a switch in rows) */
void ha_entity_state_text(const ha_entity *e, char *out, size_t outlen);
const char *ha_entity_icon(const ha_entity *e); /* attribute icon or a default by domain / class */
uint32_t ha_entity_color(const ha_entity *e);   /* accent color while on */

#endif
