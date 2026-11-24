# Adding a dashboard card type

ThermoDeck draws Home Assistant (Lovelace) dashboards natively. HA's frontend is not
reused: each supported card type is reimplemented as one C file in `app/cards/`. This is
the procedure for adding one. It is written so that an agent (or a person) can follow it
step by step and verify the result without guessing.

**Goal of a card:** show the same information and actions as HA's card, laid out for a
320×480 screen in ThermoDeck's style. Not a pixel copy of HA.

## How cards fit in

```
HA lovelace/config ──> app/cards/dashboard.c ──> card_def.options / .parse ──> ha_item(s) in g_ha.d
   (JSON)               views, sections,          (one file per card type)        + entities / attributes /
                        stacks, grids                                               forecasts to track
                                                                                        │
app/hass/hass.c  <── subscribes to exactly what the items asked for ◄──────────────────┘
   (worker thread: states, attributes, forecasts arrive here, under g_ha.mu)
                                                                                        │
app/ui/screens/hass.c ──> lays items out on the 12-column grid, pages ──> card_def.draw(card_ctx)
```

| File | Role |
|---|---|
| `app/cards/include/cards/card.h` | The `card_def` template, `card_ctx`, parse and drawing helpers. **Read this first.** |
| `app/cards/registry.c` | The list of card types. |
| `app/cards/dashboard.c` | Structure only: views, sections, stacks, grid and conditional cards; sizing from `grid_options`; entity, name, icon and `tap_action` for every card. |
| `app/cards/kit.c` | Shared drawing helpers (card background, name and icon resolution, switch, number formatting, …). |
| `app/cards/<name>.c` | One card type each. `tile.c` is the simplest full example; `weather.c` shows tracked attributes and a subscription. |
| `app/hass/entity.c` | Per-domain behaviour: on/off, state wording, default icons and colours. |
| `app/hass/hass.c` | WebSocket client: entity states, tracked attributes, forecasts, service calls. |
| `tests/data/ha_*.json` | Test dashboard, entity states and forecasts (used by the snapshot test and the mock server). |
| `tools/mock_hass.py` | Mock HA server speaking the real WebSocket protocol. |

## Procedure

### 1. Research the card (don't skip)

Collect these facts before writing code, and keep them as notes:

1. **The config schema.** Read HA's docs page `https://www.home-assistant.io/dashboards/<type>/`
   and the frontend source on GitHub: `home-assistant/frontend`,
   `src/panels/lovelace/cards/hui-<type>-card.ts` and the card's config interface in
   `src/panels/lovelace/cards/types.ts`.
2. **The default size in a sections view.** Find `getGridOptions()` (or `getLayoutOptions()`)
   in `hui-<type>-card.ts`. Columns there are 12ths of the section width, rows are grid rows
   (56 px in HA). These become the `span` and `rows` of the `card_def` as they are. ThermoDeck
   draws a row 48 px tall (`CARD_ROW_H`, so that a page holds 12 columns × 8 rows), so a card
   is about 15% shorter than in HA: check that its content fits `CARD_ROWS_H(rows)`.
3. **The data the card shows.** For each value on screen, note where it comes from:
   - the entity's state: always available (`e->state`, plus `ha_entity_state_text()`);
   - an entity attribute: needs `card_want_attr()`. `friendly_name`, `unit_of_measurement`,
     `icon`, `device_class` and `brightness` are always tracked;
   - a separate subscription or command (for example the forecast, which comes from
     `weather/subscribe_forecast` and not from attributes since HA 2023.9): the HA client
     needs support for it. See step 5.
4. **What a tap does.** HA's default `tap_action` for the card. ThermoDeck has no
   "more-info" dialogs, so either the card is tappable (it runs the entity's default action:
   toggle, run a scene or script, play/pause …) or it isn't.
5. **Real sample data.** Get a real config and real states from the user's HA. The token is
   in `/config/thermodeck/thermodeck.ini` on the device; ask the user before using it, and
   never write it into the repo. A minimal WebSocket client in Python:
   - authenticate with `{"type":"auth","access_token":...}`;
   - read the dashboard with `{"id":1,"type":"lovelace/config","url_path":"<dashboard>"}`;
   - read entities with `{"id":2,"type":"get_states"}`.

   Copy the card's config and the relevant entity's state and attributes into
   `tests/data/`. **Real formats beat remembered ones.**

### 2. Decide the shape

| Question | If yes |
|---|---|
| Does the card become one box on the grid? | Use `.options` (or nothing) and let the framework call `card_emit()`. |
| Does it become several boxes (rows, a title + content, a grid of buttons)? | Write `.parse`, emitting each box with `card_emit()` / `card_emit_sized()`, and titles with `card_add_heading()`. See `entities.c` and `markdown.c`. |
| Is its height not a whole number of grid rows? | Provide `.height(item, width)`. |
| Should a tap run the entity's action? | Set `CARD_TAPPABLE`. |
| Must it be full width, and kept on the same page as what follows (heading-like)? | Set `CARD_OWN_ROW`. |
| Does it need card-specific options at draw time? | Store them in the item: bits in `it->flags` (define an `enum` in your file), a small number in `it->opt`, text in `it->text`. |
| Does a piece of it need its own entity? | Use `card_entity(p, "domain.id")` and an extra item, or reuse one. |

### 3. Write `app/cards/<name>.c`

Start from this template. Keep the file self-contained: the template, its options and its
drawing, nothing else.

```c
/* <Type> card ("<lovelace-type>"): <one line on what it shows>. */

#include "cards/card.h"
#include "gfx/icons.h"
#include "ui/theme.h"

enum { MYCARD_SOME_OPTION = 1 }; /* bits in it->flags */

static const char *const TYPES[] = {"<lovelace-type>", NULL};

/* Card options -> item (runs once per dashboard load, on the HA worker thread). */
static void options(card_parser *p, int card, ha_item *it)
{
    if (card_bool(p, card, "some_option", 1)) it->flags |= MYCARD_SOME_OPTION;
    card_want_attr(p, it->ent, "some_attribute"); /* read later with ha_entity_attr() */
}

/* Draws into c->x, c->y, c->w, c->h (UI thread, g_ha.mu held: no hass_* calls here). */
static void draw(const card_ctx *c)
{
    card_bg(c);
    if (!c->e) return;
    char st[64];
    ha_entity_state_text(c->e, st, sizeof st);
    icon_draw(&icons_24, card_icon(c), c->x + 28, c->y + c->h / 2, C_SUB);
    gfx_text_fit(&font_sm, c->x + 54, c->y + 24, c->w - 62, card_name(c), C_TEXT);
    gfx_text_fit(&font_sm, c->x + 54, c->y + 42, c->w - 62, st, C_SUB);
}

const card_def card_mycard = {
    .name = "mycard",
    .types = TYPES,
    .span = 6, /* from HA's getGridOptions(): columns */
    .rows = 2, /* ... and rows */
    .flags = CARD_TAPPABLE,
    .options = options,
    .draw = draw,
};
```

Rules for `draw`:
- **Stay inside the box.** Use `gfx_text_fit()`, `card_text_center()` or `card_text_shrink()`
  for any text that can be long (names come from users).
- **Handle missing data.** No entity, `!e->has_state` (the state hasn't arrived yet),
  `"unavailable"` (`card_unavailable()`), and attributes that are `NULL`.
- **No blocking, no allocation, no locks.** Draw runs on the UI thread with `g_ha.mu`
  already held.
- **Go easy on floating point.** The CPU has no FPU. Numbers arrive as JSON literals, so
  format them with `card_number()` or by copying the text, not with float math per pixel.
- **Use what exists:**
  - fonts `font_sm` (14), `font_md` (18), `font_mdb` (18 bold), `font_lg` (24 bold), `font_xl` (32 bold);
  - icons `icons_24` / `icons_36`;
  - colours from `ui/theme.h`;
  - entity colour and wording from `entity.c`.
- **Height is tight.** A card gets `CARD_ROWS_H(rows)`: 48 px for 1 row, 100 for 2, 152 for 3.
- **Width varies.** A card can be laid out anywhere from 3 to 12 columns
  (69–294 px; `width = span × 25 − 6`). Check the widths HA allows for it, and switch to a
  compact layout below a threshold if needed (see `COMPACT_W` in `weather.c`).

### 4. Register it

In `app/cards/registry.c`, add `card_mycard` to the `extern` line and to `CARDS[]`.
`card_unsupported` must stay first.

### 5. Data the HA client doesn't fetch yet (only if step 1 found any)

- **Attributes:** nothing to do. `card_want_attr()` makes `hass.c` keep the value
  (`HA_MAX_ATTRS` per entity).
- **A new subscription or command** (like the forecast): follow the forecast pattern
  end to end:
  1. In `hass.h`: a struct for the data and an array of them in `ha_dash` (like `fcs`).
  2. In `cards/dashboard.c`: a `card_want_<thing>()` parse helper (like `card_want_forecast()`),
     declared in `card.h`.
  3. In `hass.c`: subscribe after the entities (`subscribe_forecasts()`), store the
     subscription ids in `session`, and unsubscribe on reload (`subscribe_entities()`).
     Parse the events (`on_forecast()`, called from `on_message()`), and keep the data across
     dashboard reloads (`install_dashboard()`).
  4. In `tools/mock_hass.py` and `tests/data/`: answer the new message type with the real
     format captured in step 1.

### 6. Icons

If the card needs icons that aren't in `tools/mdi_icons.txt`, add the MDI names there
(validated at build time; about 1.5 kB each). Unknown names fall back to the domain
default, which hides mistakes, so check the snapshot.

### 7. Test data

- `tests/data/ha_dashboard.json`: add the card in a section. Cover the default config, the
  main option variants (e.g. `show_forecast: false`), and a narrow width
  (`"grid_options": {"columns": 6}`).
- `tests/data/ha_states.json`: the entity's state and attributes, copied from the real sample.
- `tests/ui_host.c`: if the card uses a new kind of data (step 5), load it in
  `load_hass_fixture()`.

### 8. Verify

```sh
make && make tests && make format-check          # build (ARM + host tests) and style
./build-host/tests/ui_host /tmp/snap              # renders every screen and HA page to PPM
python3 tools/ppm2png.py /tmp/snap/ha_page2.ppm /tmp/snap/ha_page2.png
```

- Look at every page your card appears on. Check that text fits at every width, missing
  data looks intentional, and the card stays on one page.
- If the HA client changed (step 5):
  ```sh
  tools/mock_hass.py &
  ./build-host/tests/hass_host http://127.0.0.1:8123 test-token
  ```
  Extend `tests/hass_host.c` to assert the new data arrives.
- On the device: deploy, then open `http://<thermostat>:8080/preview` and step to the page
  (or fetch `/screen.bmp?page=N`). This uses the user's real HA data.

### 9. Done checklist

- [ ] One file `app/cards/<name>.c`, starting with a header comment that names the Lovelace
      type(s) and the options supported.
- [ ] Registered in `registry.c`.
- [ ] Default `span`/`rows` taken from HA's `getGridOptions()`.
- [ ] Options read with the `card_*` helpers; no JSON parsing in `draw`.
- [ ] Missing / unavailable data handled; text never overflows the box.
- [ ] Tests updated, `make tests` and `make format-check` pass, snapshots checked.
- [ ] Code style: `else` on its own line after `}`, and a short comment per function and
      non-obvious block (enforced by `.clang-format` / the repo's conventions).
- [ ] Any new data path (step 5) works against `tools/mock_hass.py` and on the device.
