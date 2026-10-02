"""RASHCDG 0x80083928 (sha1 cfe43a77...), 1544 bytes, frame 152:
    s32 HitOutcome(Bike *e, Entity *o, const s16 n[3])
called once, from 0x800AF3B0 at 0x800B0128. Classifies the hit (the code or-ed into e+0x238 bits 0..5,
or -1 = "exchange", recorded as e+0x234 |= 0x4000), latches the pre-contact heading/speed, computes the
1-D momentum exchange with the partner (0x80083864) and hands the result to 0x80080D1C (codes < 16 and
-1) or 0x80080B10 (codes 16, 32)."""
import sys
pass
import pairs as P
u32, s32, iabs = P.u32, P.s32, P.iabs
FRAME = 152
FIXMUL, DOT, SCALE, ATAN2 = 0x8001FC90, 0x8002E698, 0x8002EE50, 0x80020018
F4564, F49D8, F3864, FD1C, FB10 = 0x80084564, 0x800849D8, 0x80083864, 0x80080D1C, 0x80080B10
from ._util import MUTD as MUT


def mid(a, b):
    """mult a,b; (lo >> 16) | (hi << 16): the middle word of the 64-bit signed product"""
    return s32(u32((s32(a) * s32(b)) >> 16))


def model(m, e, o, n, a3):
    sp = m.sp
    m.call(SCALE, m.lw(o + 480), o + 450, sp + 48)
    m.sw(e + 564, m.lw(e + 564) & 0xFFFFBFFF)                           # 0x8008397C
    if not (m.lw(e + 568) & 0x2000000):
        h = [m.lhu(e + 450 + 2 * k) for k in range(3)]
        f = m.lw(e + 568)
        spd = m.lw(e + 480)
        for k in range(3):
            m.sh(e + 864 + 2 * k, h[k])       # 0x800839AC/B0/B4
        m.sw(e + 860, spd)                    # 0x800839B8
        m.sw(e + 568, f | 0x2000000)          # 0x800839BC
    pdir, pspd = e + 864, e + 860
    m.sw(sp + 92, pdir)
    m.sw(sp + 96, pspd)
    f = m.lw(e + 568)
    a0 = f & 0x200
    soft = False
    if not a0:
        if not (f & 0x400):
            soft = True
        elif iabs(m.lhs(n + 2)) < 2048:
            soft = True
    if not soft:
        # ---- 0x80083A0C ----
        k5 = 5 if a0 else 0
        m.sw(sp + 16, k5)
        m.sw(sp + 20, 0x30000)
        m.sw(sp + 24, 0)
        s2 = s32(m.call(F4564, n, pdir, pspd, e + 456, stack=(k5, 0x30000, 0)))
        if s2 < 0:
            return s2
        m.call(F49D8, e)
        return s2
    # ---- 0x80083A5C ----
    m.call(SCALE, m.lw(e + 860), pdir, e + 456)
    rel = [s32(u32(m.lw(e + 456 + 4 * k) - m.lw(sp + 48 + 4 * k))) for k in range(3)]
    for k in range(3):
        m.sw(sp + 64 + 4 * k, rel[k])
    s5 = s32(u32(mid(rel[2], m.lhs(n + 4) << 4) + s32(u32(mid(rel[1], m.lhs(n + 2) << 4) +
                                                            mid(rel[0], m.lhs(n) << 4)))))
    d1 = s32(m.call(DOT, pdir, n))
    d2 = s32(m.call(DOT, e + 814, n))
    ang = s32(m.call(ATAN2, d2, u32(-d1)))
    s4 = s32(u32(s32(ang) * 25736)) >> 8
    s4 = s32(u32((0x1921F if s4 > 0 else -0x1921F) - s4))
    lim = 0x1226C if (s4 ^ m.lws(e + 488)) < 0 else 0xF5BE
    m.sw(sp + 88, lim)
    s6 = s32(m.call(DOT, pdir, o + 450))
    if not (s5 < 1):
        return 0
    if lim < iabs(s4):
        a2 = s5 < -0x11E1B0
        v1 = s5 < -0x23C361
        s2 = 0 if 0xC417 < s6 else 2
        a0 = 4 if (a2 and not v1) else s2
        s2 = 8 if v1 else a0
        if s2 == 0:
            s2 = -1
        elif m.lw(e + 560) & 0x20000000:
            s2 = 2
    else:
        v = s32(m.call(DOT, o + 450, n))
        a2 = 0xB333 < v
        v1 = s5 < -0x6B4A2
        a3 = s5 < -0xB2D0E
        s2 = (15 if (a2 and v1 and not a3) else 0) + (32 if (a2 and a3) else 1)
        if s2 == 32 and (m.lw(e + 560) & 0x20000000):
            s2 = 16
        elif s2 == 1 and m.lw(e + 828) == u32(o + 172):
            s2 = 0
    # ---- 0x80083D20 ----
    if s2 == 0:
        return 0
    f = m.lw(e + 568)
    if f & 0xF:
        f &= 0xFFF007F0
        m.sw(e + 568, f)                      # 0x80083D48
        m.sw(e + 828, 0)                      # 0x80083D50
        m.sw(e + 568, f | 0x08000000)         # 0x80083D54
    if s2 >= 16 or s2 == -1:
        # ---- 0x80083D6C: the momentum exchange ----
        tot = s32(u32(m.lw(e + 316) + m.lw(o + 316)))
        dv = u32((tot >> 1) + (s32(u32(tot - 2)) >> 31))
        inv = (0x80000000 // dv) if dv else 0xFFFFFFFF
        s1 = m.call(FIXMUL, m.lw(e + 316), inv)
        s0 = m.call(FIXMUL, m.lw(o + 316), inv)
        v2 = m.call(FIXMUL, m.lw(o + 480), iabs(s6))
        m.sw(sp + 16, sp + 80)
        m.sw(sp + 20, sp + 84)
        m.call(F3864, s1, s0, m.lw(e + 480), v2, stack=(sp + 80, sp + 84))
    else:
        m.sw(sp + 80, m.lw(m.lw(sp + 96)))
    # ---- 0x80083E08 ----
    if s2 == -1:
        m.sw(e + 564, m.lw(e + 564) | 0x4000)                           # 0x80083E20
    else:
        m.sw(e + 568, m.lw(e + 568) | u32(s2))                          # 0x80083E30
    if s2 < 16:
        v = m.call(FIXMUL, s5, 3664)
        st = (n, v, s4, m.lw(sp + 88), m.lw(sp + 80), 1143, 0)
        for k, x in enumerate(st):
            m.sw(sp + 16 + 4 * k, x)
        s2 = m.call(FD1C, e, pdir, pspd, o + 172, stack=st)
    else:
        m.sw(sp + 16, m.lw(sp + 80))
        s2 = m.call(FB10, e, pdir, pspd, o + 450, stack=(m.lw(sp + 80),))
    f = m.lw(e + 568)
    if f & 0x1F:
        b = 4096
    else:
        b = (1 if (f & 0x220) else 0) << 11
    if MUT['on']:
        b = 0
    f = m.lw(e + 568) | b
    m.sw(e + 568, f)                          # 0x80083EDC
    if f & 0x130:
        m.sb(o + 509, m.lbu(o + 509) | 0x10)  # 0x80083EF8
    return s2
