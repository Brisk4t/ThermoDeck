#!/usr/bin/env python3
"""Mock Home Assistant WebSocket API, for testing ThermoDeck without a real HA server.

Implements what ThermoDeck uses: auth, lovelace/config, subscribe_events (lovelace_updated),
subscribe_entities (with "a" / "c" state events), weather/subscribe_forecast (from
tests/data/ha_forecasts.json), call_service (toggle / turn_on / turn_off change states and
notify subscribers) and ping. Edit the dashboard file while it runs and
connected clients get lovelace_updated, like editing a dashboard in HA.

  tools/mock_hass.py [--port 8123] [--token test-token] [--url-path dashboard-thermodeck]
                     [--dashboard tests/data/ha_dashboard.json] [--states tests/data/ha_states.json]

Then point thermodeck.ini at it: [homeassistant] url = http://<this machine>:8123, token = test-token
Standard library only.
"""
import argparse
import base64
import hashlib
import json
import os
import random
import socket
import struct
import threading
import time

ap = argparse.ArgumentParser()
ap.add_argument('--port', type=int, default=8123)
ap.add_argument('--token', default='test-token')
ap.add_argument('--url-path', default='dashboard-thermodeck')
here = os.path.dirname(os.path.abspath(__file__))
ap.add_argument('--dashboard', default=os.path.join(here, '..', 'tests', 'data', 'ha_dashboard.json'))
ap.add_argument('--states', default=os.path.join(here, '..', 'tests', 'data', 'ha_states.json'))
ap.add_argument('--forecasts', default=os.path.join(here, '..', 'tests', 'data', 'ha_forecasts.json'))
args = ap.parse_args()

lock = threading.Lock()
states = json.load(open(args.states))
clients = []  # Client objects with live subscriptions


def log(*a):
    print(time.strftime('%H:%M:%S'), *a, flush=True)


class Client:
    def __init__(self, sock, addr):
        self.sock, self.addr = sock, addr
        self.wlock = threading.Lock()
        self.ent_subs = {}  # subscription id -> set of entity ids
        self.lovelace_subs = set()

    # ---- framing ----
    def send(self, obj):
        data = json.dumps(obj).encode()
        hdr = bytearray([0x81])
        if len(data) < 126:
            hdr.append(len(data))
        elif len(data) < 65536:
            hdr.append(126)
            hdr += struct.pack('>H', len(data))
        else:
            hdr.append(127)
            hdr += struct.pack('>Q', len(data))
        with self.wlock:
            self.sock.sendall(bytes(hdr) + data)

    def recv_exact(self, n):
        buf = b''
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError('closed')
            buf += chunk
        return buf

    def recv(self):
        """Next text message (handles ping / close)."""
        msg = b''
        while True:
            b0, b1 = self.recv_exact(2)
            op, ln = b0 & 0x0F, b1 & 0x7F
            if ln == 126:
                ln = struct.unpack('>H', self.recv_exact(2))[0]
            elif ln == 127:
                ln = struct.unpack('>Q', self.recv_exact(8))[0]
            mask = self.recv_exact(4) if b1 & 0x80 else b'\0\0\0\0'
            payload = bytes(c ^ mask[i % 4] for i, c in enumerate(self.recv_exact(ln)))
            if op == 8:
                raise ConnectionError('close frame')
            if op == 9:
                with self.wlock:
                    self.sock.sendall(bytes([0x8A, len(payload)]) + payload)
                continue
            msg += payload
            if b0 & 0x80:
                return json.loads(msg)

    # ---- protocol ----
    def run(self):
        self.send({'type': 'auth_required', 'ha_version': '2026.11.0'})
        m = self.recv()
        if m.get('type') != 'auth' or m.get('access_token') != args.token:
            log(self.addr, 'auth rejected')
            self.send({'type': 'auth_invalid', 'message': 'Invalid access token or password'})
            return
        self.send({'type': 'auth_ok', 'ha_version': '2026.11.0'})
        log(self.addr, 'authenticated')
        with lock:
            clients.append(self)
        try:
            while True:
                self.handle(self.recv())
        finally:
            with lock:
                clients.remove(self)

    def result(self, mid, ok=True, result=None, code=None, message=None):
        r = {'id': mid, 'type': 'result', 'success': ok}
        if ok:
            r['result'] = result
        else:
            r['error'] = {'code': code, 'message': message}
        self.send(r)

    def handle(self, m):
        mid, t = m.get('id'), m.get('type')
        log(self.addr, '<-', json.dumps(m)[:160])
        if t == 'ping':
            self.send({'id': mid, 'type': 'pong'})
        elif t == 'lovelace/config':
            if m.get('url_path') != args.url_path:
                self.result(mid, False, code='config_not_found', message='Unknown config specified')
            else:
                self.result(mid, True, json.load(open(args.dashboard)))
        elif t == 'subscribe_events' and m.get('event_type') == 'lovelace_updated':
            self.lovelace_subs.add(mid)
            self.result(mid)
        elif t == 'subscribe_entities':
            ids = set(m.get('entity_ids') or states.keys())
            self.ent_subs[mid] = ids
            self.result(mid)
            with lock:
                full = {e: states[e] for e in ids if e in states}
            self.send({'id': mid, 'type': 'event', 'event': {'a': full}})
        elif t == 'weather/subscribe_forecast':
            fc = json.load(open(args.forecasts)).get(m.get('entity_id'), {}).get(m.get('forecast_type'))
            if fc is None:
                self.result(mid, False, code='invalid_entity_id', message='Weather entity not found')
            else:
                self.result(mid, True, None)
                self.send({'id': mid, 'type': 'event', 'event': {'type': m['forecast_type'], 'forecast': fc}})
        elif t == 'unsubscribe_events':
            self.ent_subs.pop(m.get('subscription'), None)
            self.lovelace_subs.discard(m.get('subscription'))
            self.result(mid)
        elif t == 'call_service':
            self.call_service(mid, m)
        else:
            self.result(mid, False, code='unknown_command', message='Unknown command.')

    def call_service(self, mid, m):
        domain, service = m['domain'], m['service']
        target = (m.get('target') or {}).get('entity_id') or (m.get('service_data') or {}).get('entity_id') or []
        if isinstance(target, str):
            target = [target]
        changed = {}
        with lock:
            for eid in target:
                if eid not in states:
                    continue
                st = states[eid]
                cur = st['s']
                new = cur
                if service == 'toggle':
                    new = {'on': 'off', 'off': 'on', 'open': 'closed', 'closed': 'open',
                           'locked': 'unlocked', 'unlocked': 'locked'}.get(cur, cur)
                elif service in ('turn_on', 'open_cover', 'unlock'):
                    new = 'on' if eid.split('.')[0] not in ('scene', 'script') else time.strftime('%Y-%m-%dT%H:%M:%S')
                elif service in ('turn_off', 'close_cover', 'lock'):
                    new = 'off'
                elif service == 'media_play_pause':
                    new = 'paused' if cur == 'playing' else 'playing'
                elif service == 'press':
                    new = time.strftime('%Y-%m-%dT%H:%M:%S')
                if new != cur:
                    st['s'] = new
                    diff = {'s': new}
                    if eid.startswith('light.'):
                        st['a']['brightness'] = 200 if new == 'on' else None
                        diff['a'] = {'brightness': st['a']['brightness']}
                    changed[eid] = {'+': diff}
        if domain == 'light' and service == 'explode':
            self.result(mid, False, code='service_not_found', message='Service light.explode not found.')
            return
        self.result(mid, True, {'context': {'id': 'x'}})
        broadcast_changes(changed)


def broadcast_changes(changed):
    if not changed:
        return
    with lock:
        cs = list(clients)
    for c in cs:
        for sid, ids in list(c.ent_subs.items()):
            part = {e: d for e, d in changed.items() if e in ids}
            if part:
                try:
                    c.send({'id': sid, 'type': 'event', 'event': {'c': part}})
                except OSError:
                    pass


def serve(sock, addr):
    """HTTP upgrade, then the WebSocket session."""
    try:
        req = b''
        while b'\r\n\r\n' not in req:
            chunk = sock.recv(4096)
            if not chunk:
                return
            req += chunk
        head = req.split(b'\r\n\r\n')[0].decode(errors='replace').split('\r\n')
        hdrs = {l.split(':', 1)[0].strip().lower(): l.split(':', 1)[1].strip() for l in head[1:] if ':' in l}
        if not head[0].startswith('GET /api/websocket') or 'sec-websocket-key' not in hdrs:
            sock.sendall(b'HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n')
            return
        acc = base64.b64encode(hashlib.sha1((hdrs['sec-websocket-key'] +
                                             '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').encode()).digest()).decode()
        sock.sendall(('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                      'Sec-WebSocket-Accept: %s\r\n\r\n' % acc).encode())
        log(addr, 'connected')
        Client(sock, addr).run()
    except (ConnectionError, OSError, ValueError) as e:
        log(addr, 'disconnected:', e)
    finally:
        sock.close()


def background():
    """Sensor drift every 10 s, and lovelace_updated when the dashboard file changes."""
    mtime = os.path.getmtime(args.dashboard)
    while True:
        time.sleep(1)
        m = os.path.getmtime(args.dashboard)
        if m != mtime:
            mtime = m
            log('dashboard file changed: sending lovelace_updated')
            with lock:
                cs = list(clients)
            for c in cs:
                for sid in list(c.lovelace_subs):
                    try:
                        c.send({'id': sid, 'type': 'event',
                                'event': {'event_type': 'lovelace_updated', 'data': {'url_path': args.url_path}}})
                    except OSError:
                        pass
        if int(time.time()) % 10 == 0:
            changed = {}
            with lock:
                for eid, st in states.items():
                    if st['a'].get('device_class') == 'temperature':
                        st['s'] = '%.1f' % (float(st['s']) + random.choice((-0.1, 0.1)))
                        changed[eid] = {'+': {'s': st['s']}}
            broadcast_changes(changed)


srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('0.0.0.0', args.port))
srv.listen()
log('mock Home Assistant on port %d, token %r, dashboard %r' % (args.port, args.token, args.url_path))
threading.Thread(target=background, daemon=True).start()
while True:
    s, a = srv.accept()
    threading.Thread(target=serve, args=(s, a[0]), daemon=True).start()
