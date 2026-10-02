r"""The DMD3 clip payload (DATA\ANIMTBL*.PSX, docs\formats\rmd3.md 5), decoded the way the rider animation machine
decodes it - RASHCDG 0x8005E2BC (the channel table), 0x8005E558 (the bit reader) and 0x8005E5A4 (the forward decoder).
Pure file-side code: it reads the player's own files under work\disc_us and never copies
their bytes anywhere.

A clip (one DMD3 block) is:
    +0x00 "DMD3"  +0x04 u32 size  +0x08 u32 102  +0x0C u8 2  +0x0D u8 FLAGS  +0x0E u8 index  +0x0F u8 PARTS
    +0x10 u16 KEYS  +0x12 u16 RATE (0 = 10)  +0x14 u32 0x18 (the loader turns it into a pointer)  +0x18 payload
channels = 3 root translation channels, then 4 per part (a 1.14 quaternion x, y, z, w).
FLAGS bit 0: the 3 root channels are raw s16[KEYS] arrays (else all three are constant 0).
FLAGS bit 1: every further channel is a record {s16 n; u16 base15|anim<<15; u16 scale|..; u16 width<<8|..;
             halfword bit stream} (n = the record's length in halfwords after the first); else raw s16[KEYS] arrays.
An animated channel: acc = base (15-bit signed); key 0 = (acc * scale) >> 9; each next key: acc += delta << 4 with a
`width`-bit two's-complement delta read MSB-first from the stream, key = (acc * scale) >> 9 (as s16). A delta equal to
the escape code (first byte of the width's table at RASHCDG 0x800CC654, minus 1, masked) starts a run: counts of
(2^w - 1) accumulate while the code 2^w - 1 repeats, the next code c adds c + 4, and the value after that repeats for
the whole count."""
import os
import struct

MUT = {"on": False}      # anim.py verify --mutate: a run one key short
DISC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "work", "disc_us", "DATA")
FILES = ["ANIMTBLW", "ANIMTBSW", "ANIMTBL1", "ANIMTBS1", "ANIMTBL2", "ANIMTBL3", "ANIMTBJ3", "ANIMTBLB", "ANIMTBSB",
         "ANIMTBLP", "ANIMTBLS", "ANIMTBLJ"]


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


def s8(v):
    v &= 0xFF
    return v - 0x100 if v & 0x80 else v


def load(name):
    with open(os.path.join(DISC, name + ".PSX"), "rb") as f:
        return f.read()


def blocks(data):
    """[(offset, size)] of the flat DMD3 chain"""
    out, off = [], 0
    while off < len(data):
        if data[off:off + 4] != b"DMD3":
            raise ValueError("not a DMD3 chain at %#x" % off)
        size = struct.unpack_from("<I", data, off + 4)[0]
        out.append((off, size))
        off += size
    return out


class Channel:
    __slots__ = ("kind", "ptr", "pos", "width", "esc", "run", "rval", "inrun", "acc", "scale", "value")


class Clip:
    """one DMD3 block, decoded channel by channel into keys[channel][key] (s16)"""

    def __init__(self, data, off, widths_first):
        self.data, self.off = data, off
        h = struct.unpack_from("<4sIIBBBBHHI", data, off)
        self.size, self.flags, self.index, self.parts, self.keys, self.rate = h[1], h[4], h[5], h[6], h[7], h[8]
        self.widths_first = widths_first              # {w: first byte of the table at 0x800CC654[w]} from RASHCDG
        self.overrun = False
        self._table()

    def hw(self, a):
        if a + 2 > len(self.data):
            self.overrun = True
            return 0
        return struct.unpack_from("<H", self.data, a)[0]

    def _table(self):
        """0x8005E2BC"""
        d, A = self.data, self.keys
        p = self.off + 24
        left = s32(self.size - 24)
        chans = []
        if self.flags & 1:
            for _ in range(3):
                left -= 2 * A
                c = Channel()
                c.kind, c.ptr = "raw", p
                chans.append(c)
                p += 2 * A
        else:
            for _ in range(3):
                c = Channel()
                c.kind, c.value = "const", 0
                chans.append(c)
        if self.flags & 2:
            while left >= 3:
                c = Channel()
                w1 = self.hw(p + 2)
                base = w1 & 0x7FFF
                if base & 0x4000:
                    base |= ~0x7FFF
                if not (w1 & 0x8000):
                    c.kind, c.value = "const", s16(base << 6)
                else:
                    c.kind = "anim"
                    c.scale = self.hw(p + 4) & 0x7FFF
                    c.width = (self.hw(p + 6) & 0x7FFF) >> 8
                    c.acc = base
                    c.value = s16(s32((base * c.scale) & 0xFFFFFFFF) >> 9)
                    c.ptr, c.pos, c.run, c.rval, c.inrun = p + 6, 8, 0, 0, False
                    c.esc = (self.widths_first[c.width] - 1) & ((1 << c.width) - 1)
                chans.append(c)
                n = s16(self.hw(p))
                left -= 2 * (n + 1)
                p += 2 * n + 2
            self.left = left
        else:
            self.left = left - 2 * A * 4 * self.parts
            for _ in range(4 * self.parts):
                c = Channel()
                c.kind, c.ptr = "raw", p
                chans.append(c)
                p += 2 * A
        self.chans = chans
        self.end = p

    def _bits(self, c):
        """0x8005E558"""
        t1, w = c.pos, c.width
        t0 = t1 + w
        win = ((self.hw(c.ptr) << 16) | self.hw(c.ptr + 2)) & 0xFFFFFFFF
        c.ptr += (t0 >> 3) & 0x1E
        c.pos = t0 & 0xF
        return (((win << (t1 & 31)) & 0xFFFFFFFF) >> ((32 - w) & 31)) & 0xFF

    def _step(self, c):
        """one key of an animated channel (the body of 0x8005E5A4's inner loop)"""
        if c.inrun:
            d = c.rval
            c.run = (c.run - 1) & 0xFFFFFFFF
            c.inrun = s32(c.run) > 0
        else:
            d = self._bits(c)
            if s8(d) == s8(c.esc):
                c.run = 0
                while True:
                    d = self._bits(c)
                    lim = 1 << c.width
                    if s8(d) != lim - 1:
                        break
                    c.run = (c.run - 1 + lim) & 0xFFFFFFFF
                c.run = (c.run + (3 if MUT["on"] else 4) + s8(d)) & 0xFFFFFFFF
                d = self._bits(c)
                c.rval = d
                c.run = (c.run - 1) & 0xFFFFFFFF
                c.inrun = True
        w, sd = c.width, s8(d)
        if (sd >> ((w - 1) & 31)) & 1:
            c.acc = s32(c.acc + ((sd - (1 << (w & 31))) << 4))
        else:
            c.acc = s32(c.acc + (sd << 4))
        c.value = s16(s32((c.acc * c.scale) & 0xFFFFFFFF) >> 9)
        return c.value

    def track(self):
        """keys[channel] = [s16 value at key 0 .. KEYS-1]"""
        out = []
        for c in self.chans:
            if c.kind == "raw":
                out.append([s16(self.hw(c.ptr + 2 * k)) for k in range(self.keys)])
            elif c.kind == "const":
                out.append([c.value] * self.keys)
            else:
                v = [c.value]
                for _ in range(1, self.keys):
                    v.append(self._step(c))
                out.append(v)
        return out


def widths_first(word_at, byte_at):
    """{w: first byte of the delta table of width w} from RASHCDG 0x800CC654 (8 pointers)"""
    return {w: byte_at(word_at(0x800CC654 + 4 * w)) for w in range(8)}


def decode_file(name, wf):
    data = load(name)
    return [Clip(data, off, wf) for off, _ in blocks(data)]
