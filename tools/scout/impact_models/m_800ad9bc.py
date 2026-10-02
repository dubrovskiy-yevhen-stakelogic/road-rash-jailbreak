"""0x800AD9BC LandingPitch(e, h, a2, a3, [k]) - RASHCDG, 696 B, frame 64"""
from .h_cx import *
FRAME = 64


def model(m, a0, a1, a2, a3):
    e, h, s5, s6 = a0, a1, a2, a3
    blk(m, 0x800AD9BC)
    m.sw(e + 768, u32(-h))                                              # 0x800ADA10  +0x300 = -h
    m.sw(e + 564, m.lw(e + 564) | 0x218000 | (u32(-s6) & 0x20000))      # 0x800ADA24  +0x234
    k = m.arg(4)
    flag = 0 if 0x7FFF < s32(k) else 1
    m.sw(m.sp + 16, flag)
    two = u32(m.lw(e + 308) << 1)                                       # 2 * e->f134
    if s32(h) > 0:
        if s32(two) > 0:
            blk(m, 0x800ADA88)
            s1 = m.call(FIXDIV, h, two)
        else:
            blk(m, 0x800ADA54)
            s1 = u32(-m.call(FIXDIV, h, u32(-two)))
    else:
        if s32(two) <= 0:
            blk(m, 0x800ADA84)
            s1 = m.call(FIXDIV, u32(-h), u32(-two))
        else:
            blk(m, 0x800ADA74)
            s1 = u32(-m.call(FIXDIV, u32(-h), two))
    s4 = sra(h, 1)
    if 0xFD70 < s32(s1):
        blk(m, 0x800ADAA4)
        s1 = 0xFD70
        s4 = m.call(FIXMUL, 0xFD70, m.lw(e + 308))
    blk(m, 0x800ADAB8)
    v = m.call(ASIN, s1)
    s1 = sra(u32(v * 25736), 8)                                         # 4096/turn -> radians 16.16
    if flag:
        blk(m, 0x800ADAF4)
        t = m.call(FIXMUL, k, 0x20000)
        s0 = m.call(FIXMUL, s1, t)
        idx = (u32(s0 * 163) >> 12) & 0x3FFC                            # radians 16.16 -> TRIG byte offset
        sn = m.lhs(TRIG + idx)
        v = m.call(FIXMUL, m.lw(e + 308), u32(sn << 4))
        m.sw(e + 772, u32(-v))                                          # 0x800ADB54  +0x304
    else:
        tail = False
        if s32(k) < 0x10000:
            blk(m, 0x800ADB64)
            if s6 != 0 and s5 != 0:
                blk(m, 0x800ADB6C)
                m.sw(e + 772, u32(-h))                                  # 0x800ADBC0
                tail = True
            else:
                blk(m, 0x800ADB78)
                s0 = m.call(FIXMUL, u32(0x10000 - k), 0x20000)
                v = m.call(FIXMUL, u32(h - s4), s0)
                m.sw(e + 772, u32(v - h))                               # 0x800ADB98
                if s6 == 0:
                    blk(m, 0x800ADBA4)
                    s0 = m.call(FIXMUL, s1, s0)
                else:
                    s0 = s1
        else:
            blk(m, 0x800ADBB8)
            m.sw(e + 772, u32(-h))                                      # 0x800ADBC0
            if s6 == 0:
                blk(m, 0x800ADBF0)
                s0 = 0
            else:
                tail = True
        if tail:
            blk(m, 0x800ADBC4)
            m.call(0x80084BE8, e, 0)
            s0 = s1
            s5 = 1
            m.sw(e + 564, m.lw(e + 564) & 0xFFFEFFFF)                   # 0x800ADBEC
    blk(m, 0x800ADBF4)
    n0 = u32(-s0)
    s0 = u32(n0 + (u32(-s5) & u32(s0 - n0)))                            # s5 ? s0 : -s0 (for s5 = 0 / 1)
    v0 = 0
    if flag and s32(s0) > 0:
        blk(m, 0x800ADC20)
        v0 = 1 if s32(s0) < m.lws(e + 616) else 0
    f = m.lw(e + 616)
    if mut('ad9bc_max'):
        v0 ^= 1
    new = u32(s0 + (u32(-v0) & u32(f - s0)))
    m.sw(e + 616, new)                                                  # 0x800ADC40  +0x268
    return new
BLOCKS = [0x800AD9BC, 0x800ADA88, 0x800ADA54, 0x800ADA84, 0x800ADA74, 0x800ADAA4, 0x800ADAB8, 0x800ADAF4,
          0x800ADB64, 0x800ADB6C, 0x800ADB78, 0x800ADBA4, 0x800ADBB8, 0x800ADBF0, 0x800ADBC4, 0x800ADBF4,
          0x800ADC20]
