"""Model of RASHCDG 0x80090D84 (sha1 cfe43a77...), 1764 B, frame 48 - the rider knock-off applier
(name ours, [guess]): picks the fall stance from the bike's crash flags, commits it through StanceEvent,
recomputes the rider's place from the bike, resets the bike's AI command stack, and runs the police
proximity-arrest test."""
import os, sys
pass
import pairs as P

FRAME = 48
from ._util import MUTX as MUT
COVER = set()
GS = 0x8005B2F8
STANCE, FIXMUL, COPYH, DOTLCM, RAND = 0x800C4550, 0x8001FC90, 0x8003FA18, 0x8002E698, 0x8001FC58
TAKEDOWN, MULADD, AICLR, AIPUSH, SND = 0x800BF51C, 0x8002EAD8, 0x800BCD10, 0x800BCA68, 0x80018440
FALL, ARREST = 0x80091468, 0x80096F30
VIEW, VIEWSZ = 0x800CD898, 1132
u32, s32 = P.u32, P.s32


def iabs(v):                       # sra/addu/xor: iabs(INT_MIN) stays INT_MIN
    v = s32(v)
    return s32(u32((v ^ (v >> 31)) - (v >> 31)))


def model(m, a0, a1, a2, a3):
    R = a0
    s2 = 1
    if m.lw(R + 0x25C) < 2:                                   # sltiu: unsigned
        body(m, R, s2)
    else:
        COVER.add('25C>=2')
    v = m.lw(R + 0x228) & 0xFFFF7FFF                           # 0x80091444
    m.sw(R + 0x228, v)
    return v


def body(m, R, s2):
    B = m.lw(R + 0x254)
    s4 = (m.lbu(R + 0x23C) >> 5) & 1
    COVER.add('s4=%d' % s4); COVER.add('ra=%x' % m.regs['ra'])
    rd = m.lw(B + 0x43C)
    m.sb(rd + 0x0F, 0)                                         # 0x80090DC8
    fc = m.lw(B + 0x238)
    neg = u32(-s4)
    if (fc & 0x7FF) == 0:
        r = m.lw(R + 0x228)
        if (r & 0x60000) == 0:
            COVER.add('E18'); m.call(STANCE, (neg & 0x31) + 41, R, 0)             # 0x80090E18: 41 / 90
        else:
            COVER.add('E6C-a'); m.call(STANCE, (neg & 0x31) + 42, R, (1 if r & 0x40000 else 0) << 8)   # 0x80090E6C: 42 / 91
    elif (fc & 0x18000) or ((fc & 0x400) and (m.lw(R + 0x228) & 0x60000)):
        COVER.add('E6C-b'); m.call(STANCE, 42, R, (1 if m.lw(R + 0x228) & 0x40000 else 0) << 8)       # 0x80090E6C: 42
    else:
        f2 = m.lw(B + 0x238)
        if f2 & 0x20:
            ev = (neg & 0x32) + 39                             # 39 / 89
            if m.lws(B + 0x2A4) < 0:
                p = 0x100
            elif (f2 & 0x40) and m.lws(B + 0x28C) > 0:
                p = 0x100
            else:
                p = 0
            COVER.add('ECC'); COVER.add('ECC-p%x' % p); m.call(STANCE, ev, R, p)                           # 0x80090ECC
            s2 = 0
        elif f2 & 0x140:
            s2 = 0; COVER.add('F00')
            m.call(STANCE, (neg & 0x33) + 38, R, (m.lws(B + 0x2A4) >> 31) & 0x100)  # 0x80090F00: 38 / 89
        elif (f2 & 0x680) and not (f2 & 0x40000):
            COVER.add('F34'); m.call(STANCE, (neg & 0x31) + 41, R, 0)             # 0x80090F34: 41 / 90
            s2 = 1 if (m.lw(B + 0x238) & 0x480) else 0
            COVER.add('F34-s2=%d' % s2)
        else:
            COVER.add('F60'); m.call(STANCE, (neg & 0x30) + 40, R, 0)             # 0x80090F60: 40 / 88
            if not (m.lw(B + 0x238) & 0x20000):
                m.sw(R + 0x1E0, m.call(FIXMUL, m.lw(R + 0x1E0), 0xC000))   # 0x80090F88
            v = m.lws(R + 0x1E0)
            if v < 0x50000:
                v = 0x50000
            m.sw(R + 0x1E0, v)                                 # 0x80090FB0 (delay slot)
            m.call(COPYH, 9, B + 0x1B0, R + 0x1B0)
            m.sw(B + 0x238, m.lw(B + 0x238) & 0xFFF9FFFF)      # 0x80090FCC
            h = m.lhu(R + 0xAC)
            gs = m.lw(GS)
            s2 = 0
            COVER.add('F60-gate')
            if (h >> 5) == 1 and (h & 0x1F) < m.lws(gs + 0x30):
                COVER.add('view'); X = m.lw(m.lw(R + 0x254) + 0x358) if s4 else m.lw(R + 0x254)
                rec = u32(VIEW + VIEWSZ * m.lhu(X + 0xAC))
                v1 = m.lw(rec + 0x224) & 0xFFFD7FFF
                z = m.lw(rec + 0x304)
                m.sw(rec + 0x224, v1)                          # 0x80091068
                if z != 0:
                    COVER.add('view-busy')
                if z == 0:
                    k = None
                    pp = m.lw(B + 0x33C)
                    if pp and (m.lhu(pp) >> 5) == 8 and (m.lhu(pp + 2) & 0x200):
                        k = 1; COVER.add('k1-pool8')
                    elif m.lw(R + 0x228) & 0x10000:
                        k = 0; COVER.add('k0-by-10000')
                    else:
                        d = m.call(DOTLCM, B + 0x334, u32(m.lw(B + 0x154) + 14))
                        if iabs(d) > 0xDDB1:
                            r = m.call(RAND); COVER.add('rand')
                            k = 0 if (r % 100) < 50 else 2
                            COVER.add('rand<50' if k == 0 else 'rand>=50')
                        else:
                            k = 0; COVER.add('k0-by-dot')
                    COVER.add('k=%s' % k)
                    if k == 1:
                        m.sw(rec + 0x224, m.lw(rec + 0x224) | 0x8000)          # 0x80091184
                    elif k == 2:
                        m.sw(rec + 0x318, 0)                                    # 0x80091164
                        f = 0x80020000 if (m.lw(B + 0x238) & 4) else 0x20000
                        m.sw(rec + 0x224, m.lw(rec + 0x224) | f)                # 0x80091184
    # 0x80091188
    if s2 == 0:
        COVER.add('takedown'); m.call(TAKEDOWN, B)
    m.call(MULADD, B + 0xB8, B + 0x1B6, 0xFFFF0000, R + 0xB8)
    x, y, z = m.lw(R + 0xB8), m.lw(R + 0xBC), m.lw(R + 0xC0)
    m.sw(R + 0x1D4, x + (1 if MUT else 0))                      # 0x800911BC (MUT: +1)
    m.sw(R + 0x1D8, y)                                         # 0x800911C0
    m.sw(R + 0x1DC, z)                                         # 0x800911C8 (delay slot)
    m.call(AICLR, B)
    rd = m.lw(B + 0x43C)
    a3 = 1 if (m.lw(rd + 0x28) != 0 or m.lbu(rd + 0x27) >= 248) else 0
    m.sh(m.sp + 16, (u32(-a3) & 0xFFFFFFFE) + 4)               # 4, or 2 when arrested/flagged
    m.sh(m.sp + 18, 224)
    m.call(AIPUSH, m.sp + 16, 1, B)
    v = m.lw(B + 0x234) & 0xFFF7FFFF
    m.sw(B + 0x39C, 0)                                         # 0x80091240
    m.sw(B + 0x394, 0)                                         # 0x80091244
    m.sw(B + 0x398, 0)                                         # 0x80091248
    m.sw(B + 0x234, v)                                         # 0x80091250
    m.sh(m.sp + 16, 0)
    m.sh(m.sp + 18, 224)
    m.call(AIPUSH, m.sp + 16, 0, B)
    if not s4:
        h = m.lhu(B + 0xAC)
        if h < m.lw(m.lw(GS) + 0x30):                          # sltu
            COVER.add('snd'); m.call(SND, h, 1)
    if m.lw(R + 0x228) & 0x10000:
        COVER.add('fall'); m.call(FALL, R)
    police(m, R, B)


def police(m, R, B):
    """0x800912AC..0x80091430: the proximity arrest (rules.md 9.3 trigger (A))"""
    gs = m.lw(GS)
    mode = m.lbu(gs + 4)
    if not (mode & 1):
        return
    if (m.lbu(m.lw(B + 0x43C) + 1) & 0xF) == 2:
        return
    if mode == 33:
        return
    s0 = 0; COVER.add('police-loop')
    if m.lws(gs + 0x30) <= 0:
        return
    while True:
        t0 = m.lw(0x8005B268 + 4 * s0)
        if (m.lbu(m.lw(t0 + 0x43C) + 1) & 0xF) == 2 and not (m.lws(0x8005B1F8) < m.lbu(m.lw(B + 0x43C) + 0x27)):
            dx = iabs(m.lhs(t0 + 0xBA) - m.lhs(B + 0xBA))
            dz = iabs(m.lhs(t0 + 0xC2) - m.lhs(B + 0xC2))
            hi_, lo_ = (dz, dx) if dx < dz else (dx, dz)
            a3 = lo_ + (lo_ >> 1)
            d = hi_ - (hi_ >> 5) - (hi_ >> 7) + (a3 >> 2) + (a3 >> 6)
            COVER.add('police-cop')
            if 0x8F0D8 < m.lws(t0 + 0x1E0):
                lim = m.lhs(u32(0x8005309C + 4 * m.lw(m.lw(GS) + 0x3C) + 2))
                if d < lim:
                    COVER.add('arrest'); m.call(ARREST, t0, B, 9)
        s0 += 1
        if not (s0 < m.lws(m.lw(GS) + 0x30)):
            return
