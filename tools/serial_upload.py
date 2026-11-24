#!/usr/bin/env python3
"""Upload a file to the thermostat over the serial console (no network needed).

Usage: serial_upload.py <local file> <remote path> [--dev /dev/ttyUSB0]

The file is gzipped + base64 (uuencode -m) encoded, streamed into `cat` on the
device with echo disabled, then decoded with busybox uudecode/gunzip and its
md5sum is compared with the local one.
"""
import base64, gzip, hashlib, os, select, sys, termios, time, tty

args = [a for a in sys.argv[1:] if not a.startswith('--dev')]
dev = '/dev/ttyUSB0'
for i, a in enumerate(sys.argv):
    if a == '--dev':
        dev = sys.argv[i + 1]
        args.remove(dev)
if len(args) != 2:
    print(__doc__); sys.exit(1)
local, remote = args

fd = os.open(dev, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
tty.setraw(fd)
a = termios.tcgetattr(fd)
a[4] = a[5] = termios.B115200
a[2] |= termios.CLOCAL | termios.CREAD
termios.tcsetattr(fd, termios.TCSANOW, a)

def drain(t=0.2, until=None):
    buf = b''
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                buf += os.read(fd, 65536)
            except BlockingIOError:
                pass
            if until and until in buf:
                break
    return buf

def send(b):
    for i in range(0, len(b), 256):
        os.write(fd, b[i:i + 256])
        time.sleep(256 * 10 / 115200 * 1.15)  # stay just under line rate
        drain(0)

def run(cmd, t=30):
    mark = b'__UPL%d__' % (int(time.time() * 1000) % 100000)
    drain(0.3)
    send(cmd.encode() + b'; echo "' + mark[:4] + b'""' + mark[4:] + b'"\n')
    out = drain(t, until=mark + b'\r\n')
    if mark not in out.replace(mark[:4] + b'""', b''):
        pass
    return out.decode(errors='replace')

data = open(local, 'rb').read()
md5 = hashlib.md5(data).hexdigest()
gz = gzip.compress(data, 9)
b64 = base64.encodebytes(gz)  # 76-char lines
payload = b'begin-base64 644 x\n' + b64 + b'====\n'
print(f'{local}: {len(data)} bytes, {len(gz)} gz, {len(payload)} to send (~{len(payload)*10/115200:.0f}s)')

run('stty -echo', 3)
drain(0.3)
send(b'cat > /tmp/.upl.uu\n')
time.sleep(0.3)
t0 = time.time()
lines = payload.split(b'\n')
sent = 0
for i in range(0, len(lines), 40):
    chunk = b'\n'.join(lines[i:i + 40]) + b'\n'
    send(chunk)
    sent += len(chunk)
    sys.stdout.write(f'\r  {sent * 100 // len(payload)}%  '); sys.stdout.flush()
time.sleep(0.5)
os.write(fd, b'\x04')
time.sleep(0.5)
print(f'\r  sent in {time.time() - t0:.0f}s')
out = run(f'uudecode -o /tmp/.upl.gz /tmp/.upl.uu && gunzip -c /tmp/.upl.gz > {remote} && rm -f /tmp/.upl.gz /tmp/.upl.uu; md5sum {remote}; stty echo', 60)
ok = md5 in out
print('remote md5 OK' if ok else 'MD5 MISMATCH / error:\n' + out)
sys.exit(0 if ok else 2)
