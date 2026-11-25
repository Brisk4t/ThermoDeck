#!/bin/sh
# ThermoDeck launcher.
#
# At boot, the stock app (/config/cpss) starts its UI through
# /config/developer/cpss-child-wrapper.sh, which runs "boot.sh takeover".
# (/config/cpss itself can't be replaced: liblinks resets it every boot.)
#
#   boot.sh takeover   stop the ecobee app (cpss/idtm) and start ThermoDeck now
#   boot.sh stop       stop ThermoDeck (watchdog keeps being fed)
#   boot.sh            start ThermoDeck in the foreground (no takeover)
#
# Escape hatches:
#   touch /config/thermodeck/disable              -> next boot runs the stock UI
#   rm /config/developer/cpss-child-wrapper.sh    -> uninstall

TD=/config/thermodeck
LOG=/tmp/thermodeck.log


start_watchdog() {
    # The stock app (cpss) feeds /dev/watchdog. Without it the SoC resets, so feed it here.
    if ! pidof watchdog >/dev/null 2>&1; then
        watchdog -T 60 -t 5 /dev/watchdog || echo "thermodeck: failed to start watchdog feeder"
    fi
}

stop_thermodeck() {
    for p in $(pidof boot.sh); do [ "$p" != "$$" ] && kill $p 2>/dev/null; done
    killall thermodeck 2>/dev/null
}

case "$1" in
takeover)
    stop_thermodeck
    # SIGKILL so the stock app can't run any cleanup (it holds the watchdog open).
    killall -9 cpss idtm ecobeeADK 2>/dev/null
    i=0
    while pidof cpss >/dev/null 2>&1 && [ $i -lt 20 ]; do usleep 100000; i=$((i+1)); done
    start_watchdog
    # The stock app leaves wpa_supplicant in AP/setup mode: restart it clean.
    killall dhcpcd wpa_supplicant 2>/dev/null
    sleep 1
    wpa_supplicant -iwlan0 -B -t -C/var/run/wpa_supplicant
    sleep 1
    iwconfig wlan0 power off 2>/dev/null
    # continue below in the background, detached from this shell
    nohup_run=1
    ;;
stop)
    stop_thermodeck
    exit 0
    ;;
*)
    start_watchdog
    ;;
esac

uptime_s() { cut -d. -f1 /proc/uptime; }

run_loop() {
    while true; do
        if [ -f $LOG ] && [ $(wc -c < $LOG) -gt 262144 ]; then mv $LOG $LOG.1; fi
        echo "=== thermodeck start $(date) ===" >> $LOG
        t0=$(uptime_s)
        $TD/thermodeck $TD >> $LOG 2>&1
        rc=$?
        echo "=== thermodeck exited with $rc ===" >> $LOG
        # An OTA update on trial (ota-pending, removed by ThermoDeck after 30 s) that dies
        # early is rolled back to the previous build. Exit 75 = restarting into a new update.
        if [ $rc -ne 75 ] && [ -f $TD/ota-pending ] && [ $(($(uptime_s) - t0)) -lt 30 ] && [ -f $TD/thermodeck.old ]; then
            mv $TD/thermodeck.old $TD/thermodeck && rm -f $TD/ota-pending && sync
            echo "=== update failed to start, rolled back to the previous build ===" >> $LOG
        fi
        sleep 3
    done
}

if [ -n "$nohup_run" ]; then
    run_loop </dev/null >/dev/null 2>&1 &
    echo "thermodeck: started (log: $LOG)"
else
    run_loop
fi
