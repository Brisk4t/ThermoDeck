#!/usr/bin/env python3
"""Tiny serial console helper. Usage:
  ser.py raw "<text>" [timeout]      send text (with \\n escapes) and dump output
  ser.py cmd "<shell cmd>" [timeout]  run command, wait for end marker
"""
import os, sys, termios, time, select, tty
DEV = '/dev/ttyUSB0'
fd = os.open(DEV, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
a = termios.tcgetattr(fd)
tty.setraw(fd)
a = termios.tcgetattr(fd)
a[4] = a[5] = termios.B115200
a[2] |= termios.CLOCAL | termios.CREAD
termios.tcsetattr(fd, termios.TCSANOW, a)

def read_for(t, until=None):
    buf = b''
    end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                d = os.read(fd, 4096)
            except BlockingIOError:
                continue
            buf += d
            if until and until in buf:
                break
    return buf

def write(s):
    b = s.encode() if isinstance(s, str) else s
    for i in range(0, len(b), 64):
        os.write(fd, b[i:i+64]); time.sleep(0.01)

mode = sys.argv[1]
if mode == 'raw':
    read_for(0.2)
    write(sys.argv[2].encode().decode('unicode_escape'))
    t = float(sys.argv[3]) if len(sys.argv) > 3 else 2
    sys.stdout.write(read_for(t).decode(errors='replace'))
elif mode == 'cmd':
    read_for(0.2)
    t = float(sys.argv[3]) if len(sys.argv) > 3 else 10
    mark = '__E%d__' % int(time.time()*1000 % 100000)
    write(sys.argv[2] + '; echo ' + mark[:3] + '""' + mark[3:] + '\n')
    out = read_for(t, until=mark.encode()).decode(errors='replace')
    # strip echoed command line
    lines = out.split('\n')
    if lines and mark[3:] in lines[0]:
        lines = lines[1:]
    sys.stdout.write('\n'.join(l for l in lines if mark not in l))
