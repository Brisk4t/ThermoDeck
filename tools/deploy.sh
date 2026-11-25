#!/bin/sh
# Upload ThermoDeck to the thermostat over the serial console (first install, or to change
# boot.sh / the boot hook). Binary-only updates can go over Wi-Fi with tools/ota.py.
#   tools/deploy.sh            upload binary + launcher (+ example config if none on device)
#   tools/deploy.sh --ini F    also upload F as /config/thermodeck/thermodeck.ini
set -e
cd "$(dirname "$0")/.."
SER="python3 tools/ser.py cmd"
make -s
$SER "mkdir -p /config/thermodeck" 5 >/dev/null
python3 tools/serial_upload.py build/thermodeck /config/thermodeck/thermodeck.new
python3 tools/serial_upload.py scripts/boot.sh /config/thermodeck/boot.sh
python3 tools/serial_upload.py scripts/auth.sh /config/thermodeck/auth.sh
$SER "mkdir -p /config/developer" 5 >/dev/null
python3 tools/serial_upload.py scripts/cpss-child-wrapper.sh /config/developer/cpss-child-wrapper.sh
$SER "chmod +x /config/thermodeck/thermodeck.new /config/thermodeck/boot.sh /config/thermodeck/auth.sh /config/developer/cpss-child-wrapper.sh && mv /config/thermodeck/thermodeck.new /config/thermodeck/thermodeck && sync && ls -l /config/thermodeck /config/developer" 10
if [ "$1" = "--ini" ] && [ -n "$2" ]; then
    python3 tools/serial_upload.py "$2" /config/thermodeck/thermodeck.ini
    $SER "chmod 600 /config/thermodeck/thermodeck.ini" 5 >/dev/null
else
    python3 tools/serial_upload.py scripts/thermodeck.ini.example /config/thermodeck/thermodeck.ini.example
    $SER "[ -f /config/thermodeck/thermodeck.ini ] || cp /config/thermodeck/thermodeck.ini.example /config/thermodeck/thermodeck.ini" 5 >/dev/null
fi
echo "Deployed (boot hook: /config/developer/cpss-child-wrapper.sh)."
echo "Restart now without rebooting:  python3 tools/ser.py cmd '/config/thermodeck/boot.sh takeover'"
echo "Later updates over Wi-Fi:        tools/ota.py <thermostat address>  (PIN on the screen)"
