#!/usr/bin/env python3
"""Histogram of BT.656-style timing codes in a raw video-endpoint dump (EVC_DUMP_STREAM=file)."""
import collections, re, sys

d = open(sys.argv[1], "rb").read()
codes = collections.Counter()
gaps = collections.Counter()
prev = None
seq = []
for m in re.finditer(rb"\xff\x00\x00", d):
    xy = d[m.start() + 3] if m.start() + 3 < len(d) else None
    codes[xy] += 1
    if prev is not None:
        gaps[(prev[1], xy, m.start() - prev[0])] += 1
    prev = (m.start(), xy)
    if len(seq) < 40:
        seq.append("%02x@%d" % (xy, m.start()))
print("bytes", len(d))
print("codes", {("%02x" % k): v for k, v in sorted(codes.items(), key=lambda kv: -kv[1])[:16]})
print("first", " ".join(seq))
print("commonest (code -> next code, distance):")
for (a, b, g), n in gaps.most_common(12):
    print("  %02x -> %02x  %5d bytes  x%d" % (a, b, g, n))
