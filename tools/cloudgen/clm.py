#!/usr/bin/env python3
"""CLM1 container writer: header + 3 raw LZ4 blocks (light/shadow/core)."""
import struct
import lz4.block

def write_clm(path, w, h, light, shadow, core):
    blocks = []
    for m in (light, shadow, core):
        raw = m.tobytes() if m is not None and m.size else b""
        blocks.append(lz4.block.compress(raw, mode="high_compression",
                                         store_size=False) if raw else b"")
    with open(path, "wb") as f:
        f.write(b"CLM1")
        f.write(struct.pack("<HH", w, h))
        f.write(struct.pack("<III", *(len(b) for b in blocks)))
        for b in blocks:
            f.write(b)
    return sum(len(b) for b in blocks) + 20
