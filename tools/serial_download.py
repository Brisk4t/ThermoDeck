#!/usr/bin/env python3
"""Download a file from the thermostat over the serial console.
Usage: serial_download.py <remote path> <local file>"""
import base64, gzip, hashlib, os, re, select, sys, termios, time, tty

remote, local = sys.argv[1], sys.argv[2]
fd = os.open('/dev/ttyUSB0', os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
tty.setraw(fd)
a = termios.tcgetattr(fd)
a[4] = a[5] = termios.B115200
a[2] |= termios.CLOCAL | termios.CREAD
termios.tcsetattr(fd, termios.TCSANOW, a)

def read_until(marker, t):
    buf = b''
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try:
                buf += os.read(fd, 65536)
            except BlockingIOError:
                pass
            if marker in buf:
                break
    return buf

read_until(b'\x00', 0.5)
cmd = f'md5sum {remote}; gzip -c {remote} > /tmp/.dl.gz; echo "@@BEG""IN@@"; uuencode -m /tmp/.dl.gz x; echo "@@EN""D@@"; rm -f /tmp/.dl.gz\n'
os.write(fd, cmd.encode())
out = read_until(b'@@END@@', 900)
m = re.search(rb'([0-9a-f]{32})\s+' + re.escape(remote.encode()), out)
body = out.split(b'@@BEGIN@@', 1)[1].split(b'@@END@@', 1)[0]
lines = [l.strip() for l in body.replace(b'\r', b'').split(b'\n')]
i = next(k for k, l in enumerate(lines) if l.startswith(b'begin-base64'))
data = b''
for l in lines[i + 1:]:
    if l == b'====':
        break
    data += l
raw = gzip.decompress(base64.b64decode(data))
open(local, 'wb').write(raw)
ok = m and hashlib.md5(raw).hexdigest() == m.group(1).decode()
print(f'{local}: {len(raw)} bytes, md5 {"OK" if ok else "MISMATCH"}')
