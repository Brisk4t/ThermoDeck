# ThermoDeck


<p align="center">
  <img src="./images/ThermoDeck%20Spotify.png" alt="ThermoDeck Spotify Page" width="300">
  <img src="./images/ThermoDeck%20HA.png" alt="ThermoDeck Home Assistant Page" width="300">
</p>

## Overview

ThermoDeck replaces the stock interface of an ecobee thermostat (i.MX28, 320×480 touchscreen) with a small dashboard. It's a single static C binary with no browser and no GUI toolkit. It draws straight to the framebuffer.

- **Spotify page:** now playing, cover art, play/pause, next/previous and volume. Log in from your phone; the login page is protected by a PIN shown on the thermostat.
- **Home Assistant pages:** swipe up from Spotify. The layout comes from a dashboard you build in HA's own editor, and edits show up live. Each section of the dashboard is one screen of 12 columns × 8 rows. Cards are drawn natively; [ADDING_CARDS.md](ADDING_CARDS.md) lists how to add more.
- **Settings:** Wi-Fi setup with an on-screen keyboard, time zone and brightness. The screen dims after 30 s unless touched or someone is near it.
- **Web server (port 8080):** a status page, the Spotify login, and a live preview at `/preview` of the HA pages as the thermostat draws them.

Code layout: `lib/` holds reusable modules (net, gfx, json, input, core). `app/` holds the features (backplate, wifi, spotify, hass, cards, web, ui). Each module keeps its public headers in `include/<module>/`.

## Getting started

You need root on the thermostat and its serial console on `/dev/ttyUSB0`.

1. Build, then upload everything over serial: `tools/deploy.sh`. This installs the binary, its launcher and a boot hook in `/config/developer/`.
2. Edit `/config/thermodeck/thermodeck.ini` on the device; `scripts/thermodeck.ini.example` explains every option.
   - `[spotify]`: your Spotify app's client ID and secret.
   - `[homeassistant]`: your HA address, a long-lived access token, and the dashboard's URL path (default `dashboard-thermodeck`).
3. Start it without rebooting: `python3 tools/ser.py cmd '/config/thermodeck/boot.sh takeover'`.
4. Join Wi-Fi on the screen, then open `http://<thermostat>:8080` to log in to Spotify.
5. Later builds go over Wi-Fi: `tools/ota.py <thermostat>`, then type the PIN the thermostat shows. A build that fails to start is rolled back.

ThermoDeck never runs the HVAC: it turns every relay off at startup ([BACKPLATE.md](BACKPLATE.md)).

**Going back:**
- `touch /config/thermodeck/disable` makes the next boot run the stock UI.
- Deleting `/config/developer/cpss-child-wrapper.sh` uninstalls ThermoDeck.

## Building

You need a host C compiler, `curl`, `git` and Python 3. For the device build you also need the Bootlin ARMv5 musl toolchain in `~/opt/toolchains/` (override with `TOOLCHAIN=`).

```sh
make              # build/thermodeck: static ARMv5TE binary
make host         # the same, for this machine (compile check)
make tests        # host test programs in build-host/tests/
make format       # clang-format (pip install clang-format)
make format-check
```

The first build downloads pinned, checksummed dependencies into `third_party/`: BearSSL, qrcodegen, stb, Mozilla's CA bundle, DejaVu fonts and Material Design Icons. From those it generates the fonts, icons and trust anchors into `gen/`. Neither folder is kept in git.

**Testing without the device:**
- `tests/ui_host` renders every screen to PPM files.
- `tools/mock_hass.py` is a stand-in Home Assistant server; `tests/hass_host` runs the real HA client against it.

## Adding cards

Each Home Assistant card type is one file in `app/cards/` that fills in a `card_def` template (`app/cards/include/cards/card.h`):

- which Lovelace types it handles;
- its default grid size;
- optionally, how to read its options and which extra data it needs (attributes or forecasts);
- how to draw it into a box.

The framework handles the rest: sizing, entity, name, icon, tap action, page layout and taps.

To add one:
1. Research the card in HA's docs and frontend source.
2. Copy the template from the guide into `app/cards/<name>.c` and list it in `app/cards/registry.c`.
3. Add test data under `tests/data/`.
4. Check the result with the snapshot test, the mock server and `/preview` on the device.

The full step-by-step procedure, written so an agent can follow it, is in [ADDING_CARDS.md](ADDING_CARDS.md).
