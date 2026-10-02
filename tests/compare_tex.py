"""Compares our C++ texture decoder against the independent Python probe, pixel for pixel.

The two implementations were written from the same byte-level spec but by different hands, so an
exact match is evidence the spec is right - the same discipline the audio cross-check uses.

  python tests/compare_tex.py <cpp-png-dir> <probe-png-dir>

Both directories hold PNGs; files are paired by the texture id encoded in their names
(`chunkNN.idXXXX.kN.png` from rrtool, `NAME.lectNN.idxxxx.kN.png` from tex.py).
Exit code 0 when every pair matches.
"""
import os
import re
import struct
import sys
import zlib


def read_png(path):
    data = open(path, "rb").read()
    pos, idat, width, height, colour = 8, b"", 0, 0, 0
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, _, colour = struct.unpack(">IIBB", payload[:10])
        elif kind == b"IDAT":
            idat += payload
        elif kind == b"IEND":
            break
    raw = zlib.decompress(idat)
    channels = {0: 1, 2: 3, 4: 2, 6: 4}[colour]
    stride = width * channels + 1
    rows, previous = [], bytearray(width * channels)
    for y in range(height):
        filt = raw[y * stride]
        line = bytearray(raw[y * stride + 1:(y + 1) * stride])
        if filt == 1:
            for i in range(channels, len(line)):
                line[i] = (line[i] + line[i - channels]) & 255
        elif filt == 2:
            for i in range(len(line)):
                line[i] = (line[i] + previous[i]) & 255
        elif filt == 3:
            for i in range(len(line)):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + previous[i]) >> 1)) & 255
        elif filt == 4:
            for i in range(len(line)):
                a = line[i - channels] if i >= channels else 0
                b = previous[i]
                c = previous[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if (pa <= pb and pa <= pc) else (b if pb <= pc else c))) & 255
        rows.append(bytes(line))
        previous = line
    return width, height, channels, b"".join(rows)


def index_by_id(directory):
    # The probe writes several files per texture id: the image itself, a `.pal.` strip of its CLUT,
    # and for 4bpp atlases one `.clutNNNN.` variant per candidate palette. Only the plain image is
    # comparable, so everything with an extra qualifier is skipped.
    found = {}
    for name in os.listdir(directory):
        lowered = name.lower()
        if not lowered.endswith(".png"):
            continue
        if ".pal." in lowered or ".clut" in lowered or ".grey." in lowered or ".gtp" in lowered:
            continue
        match = re.search(r"id([0-9a-fA-F]{4})", name)
        if not match:
            continue
        found.setdefault(int(match.group(1), 16), []).append(os.path.join(directory, name))
    return found


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    ours, probe = index_by_id(sys.argv[1]), index_by_id(sys.argv[2])
    shared = sorted(set(ours) & set(probe))
    if not shared:
        print("no texture ids in common - nothing compared")
        return 1

    compared = differing = pixels = bad_pixels = 0
    for texture_id in shared:
        a_path, b_path = sorted(ours[texture_id])[0], sorted(probe[texture_id])[0]
        wa, ha, ca, pa = read_png(a_path)
        wb, hb, cb, pb = read_png(b_path)
        compared += 1
        if (wa, ha) != (wb, hb):
            differing += 1
            print("SIZE  id %04X: %dx%d vs %dx%d" % (texture_id, wa, ha, wb, hb))
            continue
        local_bad = 0
        for i in range(wa * ha):
            if pa[i * ca:i * ca + 3] != pb[i * cb:i * cb + 3]:
                local_bad += 1
        pixels += wa * ha
        bad_pixels += local_bad
        if local_bad:
            differing += 1
            print("DIFF  id %04X: %d of %d pixels" % (texture_id, local_bad, wa * ha))

    print("images compared   : %d" % compared)
    print("images differing  : %d" % differing)
    print("pixels compared   : %d" % pixels)
    print("pixels differing  : %d" % bad_pixels)
    return 0 if differing == 0 else 1


sys.exit(main())
