#!/bin/sh
# Authorize Spotify from the serial console.
#
#   /config/thermodeck/auth.sh
#       prints the Spotify login URL: open it in a browser on any device and approve.
#       The browser then lands on http://127.0.0.1:8080/callback?code=... (fails to load).
#
#   /config/thermodeck/auth.sh 'http://127.0.0.1:8080/callback?code=...&state=...'
#       hands that URL to ThermoDeck (keep the single quotes: the URL contains '&').
#
#   /config/thermodeck/auth.sh logout
#       forget the Spotify login (the LAN web page can't do this).
#
# Note: the login URL changes each time ThermoDeck restarts, so use a fresh one.
# The same flow works from a browser at http://<thermostat ip>:8080 with the on-screen PIN.

PORT=$(sed -n 's/^[[:space:]]*port[[:space:]]*=[[:space:]]*\([0-9]*\).*/\1/p' /config/thermodeck/thermodeck.ini 2>/dev/null | head -n 1)
[ -n "$PORT" ] || PORT=8080
BASE="http://127.0.0.1:$PORT"

if [ -z "$1" ]; then
    url=$(wget -q -O - "$BASE/" | grep -o 'https://accounts.spotify.com/authorize[^"]*' | head -n 1 | sed 's/&amp;/\&/g')
    if [ -z "$url" ]; then
        echo "No login URL: Spotify is already connected, not configured (client_id/secret), or ThermoDeck isn't running."
        exit 1
    fi
    echo "Open this in a browser and approve, then run: $0 '<the address you end up on>'"
    echo
    echo "$url"
    exit 0
fi

if [ "$1" = "logout" ]; then
    wget -q -O - "$BASE/logout"
    exit $?
fi

case "$1" in
*code=*) ;;
*) echo "That doesn't contain code=... -- paste the whole address the browser ended up on."; exit 1 ;;
esac
query="${1#*\?}"
wget -q -O - "$BASE/callback?$query" | sed -n 's/.*<b class=[a-z]*>\([^<]*\)<.*/\1/p'
echo
