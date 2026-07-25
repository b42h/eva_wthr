#!/usr/bin/env python3
"""Decode one entry from assets/clouds.bin to a PNG for eyeballing.
Usage: clm_read.py <pack> <type> <subtype> <variant> <out.png> [plane]
For a bolt: type=1, subtype=<direction>, variant=<v>. plane 0=core,1=glow."""
import struct, sys
import lz4.block
import numpy as np
from PIL import Image

def toc(data):
    assert data[:4] in (b'CLP2', b'CLP3'), data[:4]
    count = struct.unpack_from('<H', data, 4)[0]
    for i in range(count):
        layer, pool, variant, etype, off, size = struct.unpack_from('<BBBBII', data, 6+i*12)
        # sprite entries: 'layer' field carries subtype
        yield dict(subtype=layer, pool=pool, variant=variant, type=etype, off=off, size=size)

def decode_clm(blob):
    assert blob[:4] == b'CLM1', blob[:4]
    w, h = struct.unpack_from('<HH', blob, 4)
    sizes = struct.unpack_from('<III', blob, 8)
    off, planes = 20, []
    for cs in sizes:
        if cs == 0:
            planes.append(None); continue
        raw = lz4.block.decompress(bytes(blob[off:off+cs]), uncompressed_size=w*h)
        planes.append(np.frombuffer(raw, dtype=np.uint8).reshape(h, w))
        off += cs
    return w, h, planes

if __name__ == '__main__':
    pack, t, sub, var, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
    plane_idx = int(sys.argv[6]) if len(sys.argv) > 6 else 0
    data = open(pack, 'rb').read()
    for e in toc(data):
        if e['type'] == t and e['subtype'] == sub and e['variant'] == var:
            w, h, planes = decode_clm(data[e['off']:e['off']+e['size']])
            p = planes[plane_idx]
            if p is None:
                sys.exit(f"plane {plane_idx} empty")
            Image.fromarray(p, 'L').save(out)
            print(f"saved {out} {w}x{h} plane{plane_idx}")
            break
    else:
        sys.exit("entry not found")
