#!/usr/bin/env python3
"""Push a ThermoDeck build to the thermostat over Wi-Fi (see app/web/ota.c).

  tools/ota.py <thermostat address> [--file build/thermodeck] [--port 8080] [-f]

-f / --force sends the build even if the device is already running it (e.g. to test the
update path itself).

The thermostat shows a one-time PIN when the update starts; type it here. The PIN is
never sent: the upload carries an HMAC made with it, covering this exact binary. After the
upload the device restarts into the new build; this script waits until /version reports the
uploaded binary's SHA-256. If the new build dies within 30 s, the device rolls back and
this script says so.

The address can also come from $THERMODECK_HOST. Standard library only.
"""
import argparse
import hashlib
import hmac
import http.client
import os
import sys
import time

LABEL = 'thermodeck-ota-v2'
HERE = os.path.dirname(os.path.abspath(__file__))


def request(host, port, method, path, body=None, headers=None, timeout=10):
    """Returns (status, text), or (None, error) if the device can't be reached."""
    try:
        c = http.client.HTTPConnection(host, port, timeout=timeout)
        c.request(method, path, body=body, headers=headers or {})
        r = c.getresponse()
        return r.status, r.read().decode(errors='replace').strip()
    except OSError as e:
        return None, str(e)


def running(host, port):
    """(sha256, instance) of the running ThermoDeck; instance changes on every restart.
    (None, None) if it can't be reached."""
    st, body = request(host, port, 'GET', '/version', timeout=5)
    if st != 200:
        return None, None
    info = dict(line.split(' ', 1) for line in body.splitlines() if ' ' in line)
    return info.get('sha256'), info.get('instance')


def ask_pin():
    """The 6 digits shown on the thermostat (spaces allowed)."""
    while True:
        try:
            pin = input('PIN shown on the thermostat: ').replace(' ', '')
        except EOFError:
            sys.exit('\nno PIN entered')
        if len(pin) == 6 and pin.isdigit():
            return pin
        print('the PIN is 6 digits')


def upload(a, data, sha, nonce, pin):
    msg = f'{LABEL}\n{nonce}\n{len(data)}\n{sha}\n'.encode()
    auth = hmac.new(pin.encode(), msg, hashlib.sha256).hexdigest()
    return request(a.host, a.port, 'POST', '/ota', body=data, timeout=60, headers={
        'Content-Type': 'application/octet-stream',
        'X-OTA-Nonce': nonce,
        'X-OTA-SHA256': sha,
        'X-OTA-Auth': auth,
    })


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('host', nargs='?', default=os.environ.get('THERMODECK_HOST'))
    ap.add_argument('--file', default=os.path.join(HERE, '..', 'build', 'thermodeck'))
    ap.add_argument('--port', type=int, default=8080)
    ap.add_argument('-f', '--force', action='store_true', help='send even if the device runs this build already')
    a = ap.parse_args()
    if not a.host:
        ap.error('give the thermostat address (or set THERMODECK_HOST)')

    data = open(a.file, 'rb').read()
    sha = hashlib.sha256(data).hexdigest()
    old_sha, old_instance = running(a.host, a.port)
    if old_sha == sha and not a.force:
        print('the device is already running this build (use -f to send it anyway)')
        return 0

    # ask for a PIN on the thermostat screen
    st, text = request(a.host, a.port, 'POST', '/ota/begin')
    if st != 200:
        sys.exit(f'device: {st or "unreachable"} {text}')
    nonce = dict(line.split(' ', 1) for line in text.splitlines()).get('nonce', '')
    print(f'{a.file}: {len(data)} bytes, sha256 {sha[:12]}')
    print('The thermostat is now showing a PIN.')

    # upload with the PIN; a wrong PIN can be retried while the device allows it
    while True:
        st, text = upload(a, data, sha, nonce, ask_pin())
        print(f'device: {st} {text}')
        if st == 200:
            break
        if st == 403 and text.startswith('Wrong PIN (') and 'left' in text:
            continue
        return 1

    # wait for the restart (a new instance), then for the trial period to pass
    print('waiting for the new build to start...', end='', flush=True)
    deadline = time.time() + 60
    while time.time() < deadline:
        time.sleep(2)
        now_sha, instance = running(a.host, a.port)
        if now_sha == sha and instance != old_instance:
            print(' running.')
            break
        print('.', end='', flush=True)
    else:
        print('\nthe device did not come back with the new build within 60 s')
        return 1
    print('watching it for 35 s (an early crash rolls it back)...', end='', flush=True)
    time.sleep(35)
    now, _ = running(a.host, a.port)
    if now == sha:
        print(' OK, update kept.')
        return 0
    print(f'\nthe device is now running {now[:12] if now else "nothing reachable"}: the update was rolled back')
    return 1


if __name__ == '__main__':
    sys.exit(main())
