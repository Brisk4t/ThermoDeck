#!/usr/bin/env python3
"""ppm2png.py in.ppm out.png [scale] [x y w h] -- tiny dependency-free converter (debug screenshots)."""
import sys, zlib, struct

def read_ppm(p):
    d = open(p, 'rb').read()
    parts, i = [], 0
    while len(parts) < 4:
        while d[i:i+1].isspace(): i += 1
        j = i
        while not d[j:j+1].isspace(): j += 1
        parts.append(d[i:j]); i = j
    i += 1
    w, h = int(parts[1]), int(parts[2])
    return w, h, d[i:i + w * h * 3]

def main():
    w, h, px = read_ppm(sys.argv[1])
    scale = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    x0, y0, cw, ch = (map(int, sys.argv[4:8]) if len(sys.argv) > 7 else (0, 0, w, h))
    rows = []
    for y in range(y0, y0 + ch):
        row = b''.join(px[(y * w + x) * 3:(y * w + x) * 3 + 3] * scale for x in range(x0, x0 + cw))
        rows += [b'\0' + row] * scale
    raw = b''.join(rows)
    def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', cw * scale, ch * scale, 8, 2, 0, 0, 0)) \
        + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
    open(sys.argv[2], 'wb').write(png)

main()
