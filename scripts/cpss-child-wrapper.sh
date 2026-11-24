#!/bin/sh
# ThermoDeck boot hook -- installed as /config/developer/cpss-child-wrapper.sh
#
# The stock app (/config/cpss) starts its UI process (idtm) through this script
# whenever it exists and is executable (CpssWatchdogHandler::startIdtm). /config/cpss
# itself can't be replaced: /usr/sbin/liblinks re-points it at /idt/cpss on every boot.
#
# So: cpss boots, asks us to start idtm, and we hand the device to ThermoDeck instead
# (boot.sh takeover stops cpss, feeds the watchdog itself and starts ThermoDeck).
#
# Disable (stock UI on next boot):  touch /config/thermodeck/disable
# Uninstall:                        rm /config/developer/cpss-child-wrapper.sh

if [ -f /config/thermodeck/disable ] || [ ! -x /config/thermodeck/boot.sh ]; then
    exec /config/idtm "$@"
fi

# Only once per boot, even if cpss retries starting its UI.
mkdir /tmp/thermodeck-takeover.lock 2>/dev/null || exec sleep 600

# Detach (in its own subshell) so the takeover survives cpss being killed.
( /config/thermodeck/boot.sh takeover > /tmp/thermodeck-takeover.log 2>&1 ) </dev/null &
# Stay alive so cpss doesn't consider its UI dead and respawn it in the meantime.
exec sleep 600
