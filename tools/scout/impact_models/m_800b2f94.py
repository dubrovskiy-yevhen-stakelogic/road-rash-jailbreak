"""0x800B2F94 PropKick(prop, bike, s16 dir[3]) - RASHCDG, 944 B, frame 32"""
from .h_cx import *
FRAME = 32


def _r714(r):
    """Rand() % 714 - 357 by the multu 0x16F26017 / srl 6 idiom, as u32"""
    q = ((r * 0x16F26017) >> 32) >> 6
    return u32(u32(r - 714 * q) - 357)


def _cls(m, prop):
    return (m.lhu(m.lw(prop) + 14) & 0xF80) >> 7


def model(m, a0, a1, a2, a3):
    prop, bike = a0, a1
    blk(m, 0x800B2F94)
    if m.lw(prop + 592) & 2:
        blk(m, 0x800B2FC0)
        return m.call(0x800B3344, prop, 0, a2, 0)
    blk(m, 0x800B2FD4)
    m.sw(prop + 480, m.call(FIXMUL, 0x11999, m.lw(bike + 480)))       # 0x800B2FE4  prop +0x1E0
    if 0x50000 < m.lws(bike + 480):
        blk(m, 0x800B2FFC)
        t = s32(u32(_r714(m.call(RAND)) << 16)) >> 16
        m.sh(prop + 450, u32(m.lhu(bike + 450) + (s32(u32(t * m.lhs(bike + 454))) >> 12)))   # 0x800B305C
        m.sh(prop + 452, m.lhu(bike + 452))                                                 # 0x800B3068
        a0v = u32(m.lhs(prop + 452) << 4)
        m.sh(prop + 454, u32(m.lhu(bike + 454) - (s32(u32(t * m.lhs(bike + 450))) >> 12)))  # 0x800B3098
        asn = m.call(ASIN, a0v)
        c = _cls(m, prop)
        a1v = u32(-(1 if c in (2, 5) else 0)) & 0x39
        c = _cls(m, prop)
        a0b = 1 if c in (1, 4) else 0
        idx = u32((u32(-a0b) & 0xFFFFFFC7) + a1v + 1479 - asn) & 0xFFF
        cosv = m.lhs(TRIG + idx * 4 + 2)
        c = _cls(m, prop)
        t0 = u32(-(1 if c in (2, 5) else 0)) & 0x15439
        c = _cls(m, prop)
        a3b = 1 if c in (1, 4) else 0
        a3v = u32((u32(-a3b) & 0xFFFE3A5E) + 0x633B6 + t0)
        m.call(0x8007E868, prop + 450, m.lw(prop + 480), u32(cosv), a3v)
        s = s32(m.lw(bike + 480))
        v1 = u32(((s * 0x55555556) >> 32) - (s >> 31))                  # s / 3, toward zero
        m.sw(prop + 488, v1)                                             # 0x800B31FC  +0x1E8
        if 0xA0000 < s32(v1):
            blk(m, 0x800B3200)
            v1 = 0xA0000
        blk(m, 0x800B3204)
        m.sw(prop + 488, v1)                                             # 0x800B3208 (delay slot)
        s0 = s32(u32(_r714(m.call(RAND)) << 16)) >> 16
        m.sh(prop + 560, u32(m.lhu(prop + 454) + (s32(u32(s0 * m.lhs(prop + 450))) >> 12)))  # 0x800B326C
        r = m.call(RAND)
        m.sh(prop + 578, 0)                                              # 0x800B3280
        m.sh(prop + 562, _r714(r))                                       # 0x800B32B4
        m.sh(prop + 564, u32((s32(u32(s0 * m.lhs(prop + 454))) >> 12) - m.lhu(prop + 450)))  # 0x800B32C8
        m.call(NORMALIZE, prop + 560)
        m.sw(prop + 592, m.lw(prop + 592) | (2 if mut('b2f94_flag') else 3))   # 0x800B32DC
    else:
        blk(m, 0x800B32E0)
        spd = m.lw(prop + 480)
        m.sh(prop + 450, m.lhu(bike + 450))                              # 0x800B32E8
        m.sh(prop + 452, m.lhu(bike + 452))                              # 0x800B32F4
        m.sh(prop + 454, m.lhu(bike + 454))                              # 0x800B3304 (delay slot)
        m.call(SCALE, spd, prop + 450, prop + 456)
    blk(m, 0x800B3308)
    snd = m.call(SURFSND, m.lw(prop + 180))
    m.call(PS3D, m.lw(prop + 184), m.lw(prop + 192), snd, 0)
    return 1
BLOCKS = [0x800B2F94, 0x800B2FC0, 0x800B2FD4, 0x800B2FFC, 0x800B3200, 0x800B3204, 0x800B32E0, 0x800B3308]
