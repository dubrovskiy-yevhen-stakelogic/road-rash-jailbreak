"""0x80083F30 ImpactTurn(e, shape, s16 n[3], mode) - RASHCDG, 1588 B, frame 120"""
from .h_cx import *
FRAME = 120
PI2 = 0x1921F                     # pi/2 in 16.16


def model(m, a0, a1, a2, a3):
    e, shape, n, mode = a0, a1, a2, a3
    sp = m.sp
    blk(m, 0x80083F30)
    m.sw(m.entry_sp + 4, shape)                                    # 0x80083F64 home spill of a1
    m.sw(e + 564, m.lw(e + 564) & 0xFFFFBFFF)                      # 0x80083F74  +0x234 &= ~0x4000
    if not (m.lw(e + 568) & 0x02000000):
        blk(m, 0x80083F8C)
        h = [m.lhu(e + 450 + 2 * i) for i in range(3)]
        f = m.lw(e + 568) | 0x02000000
        spd = m.lw(e + 480)
        for i in range(3):
            m.sh(e + 864 + 2 * i, h[i])                            # 0x80083FA4/A8/AC  +0x360..+0x364
        m.sw(e + 860, spd)                                         # 0x80083FB0  +0x35C
        m.sw(e + 568, f)                                           # 0x80083FB4
    blk(m, 0x80083FB8)
    f = m.lw(e + 568)
    b10, b9 = (f >> 10) & 1, f & 0x200
    s7, s8 = e + 864, e + 860
    air = bool(b9)
    if not air and b10:
        ny = ((m.lhu(n + 2) ^ 0x8000) - 0x8000)
        air = not (iabs(u32(ny)) < 2048)
    if air:
        blk(m, 0x80084004)
        a16 = 5 if b9 else 0
        m.sw(sp + 16, a16)
        m.sw(sp + 20, 0x30000)
        m.sw(sp + 24, 0)
        s1 = m.call(0x80084564, n, s7, s8, e + 456, stack=(a16, 0x30000, 0))
        if s32(s1) < 0:
            blk(m, 0x80084534)
            return s1
        m.call(0x800849D8, e)
        return s1
    blk(m, 0x80084054)
    s4 = 0
    if mode == 2:
        s0 = s1 = 0
    else:
        blk(m, 0x80084068)
        s4 = dot3(m, [m.lw(e + 456 + 4 * i) for i in range(3)], n)    # MH(e.v[i], n[i]) summed
        s1 = 0
        if s32(mode) < 2:
            blk(m, 0x800840FC)
            s0 = 11438 if mode == 1 else u32(-11438)
        else:
            if m.lw(e + 568) & 0x600:
                blk(m, 0x80084124)
                m.sh(sp + 50, 0)
                m.sh(sp + 48, m.lhu(e + 450))
                m.sh(sp + 52, m.lhu(e + 454))
                m.call(NORMALIZE, sp + 48)
                v1, v0 = m.lhu(sp + 52), m.lhu(sp + 48)
                m.sh(sp + 58, 0)
                m.sh(sp + 56, v1)
                m.sh(sp + 60, u32(-v0))
                s1 = m.call(DOTLCM, sp + 48, n)
                v = m.call(DOTLCM, sp + 56, n)
            else:
                blk(m, 0x800841B4)
                s1 = m.call(DOTLCM, s7, n)
                v = m.call(DOTLCM, e + 814, n)
            a = m.call(RATATAN2, v, u32(-s1))
            s0 = sra(u32(a * 25736), 8)
            s0 = u32((PI2 if s32(s0) > 0 else u32(-PI2)) - s0)
            v1 = u32(s0 + (sra(u32(s0 + PI2), 31) & u32(u32(-PI2) - s0)))     # max(s0, -pi/2)
            a0_ = u32(PI2 - s0)
            s0 = u32(v1 + (sra(a0_, 31) & a0_))                                 # + min(pi/2 - s0, 0)
    blk(m, 0x80084258)
    s1 = 1 if s32(s1) < 1 else 0
    if s1 == 0:
        blk(m, 0x80084530)
        return 0
    sp64 = 0
    if mode == 2:
        blk(m, 0x8008426C)
        s3 = 0
        s1 = u32(-1)
        sp64 = m.call(FIXMUL, 655, m.lw(s8))
        m.sw(sp + 64, sp64)
    else:
        blk(m, 0x80084288)
        m.sw(sp + 64, 0)
        s3 = 0x1226C if s32(s0 ^ m.lw(e + 488)) < 0 else 0xF5BE
        if s32(mode) < 2:
            blk(m, 0x800842B0)
            a1_ = sra(u32(s0 * 652), 16)
            a2_ = a1_ & 0xFFF
            c16 = u32(m.lhs(TRIG + a2_ * 4 + 2) << 4)
            v1 = u32(-c16) if s32(a1_) < 0 else c16
            s16 = u32(m.lhs(TRIG + a2_ * 4) << 4)
            a3_ = u32(-s16) if s32(a1_) > 0 else s16
            m.sw(sp + 16, v1)
            m.call(BLEND16, s7, e + 814, n, a3_, stack=(v1,))
        blk(m, 0x80084330)
        if s32(s4) > 0:
            s4 = 0
        if s32(s3) < iabs(s0):
            blk(m, 0x8008434C)
            a2_ = 0
            if s32(s4) < s32(0xFFEE1E50):
                a2_ = 1 if (m.lw(e + 560) & 0x20000000) == 0 else 0
            v1 = 0
            if mode == 5:
                v1 = 1 if s32(s4) < s32(0xFFDC3C9F) else 0
            v0 = 1 if (a2_ and v1 != 0) else 0
            a0_ = 1 if (a2_ and v1 == 0) else 0
            s1 = u32((u32(-v0) & 6) + (a0_ << 1) + 2)
        else:
            blk(m, 0x800843B8)
            s1 = 0 if m.lw(e + 828) == m.lw(m.entry_sp + 4) else 1
    blk(m, 0x800843DC)
    if s1 == 0:
        return 0
    v1 = m.lw(e + 568)
    if v1 & 0xF:
        blk(m, 0x800843F4)
        v0 = v1 & 0xFFF007F0
        m.sw(e + 568, v0)                                          # 0x80084404
        m.sw(e + 828, 0)                                           # 0x8008440C  +0x33C = 0
        m.sw(e + 568, v0 | 0x08000000)                             # 0x80084410
    if s1 == 0xFFFFFFFF:
        blk(m, 0x80084420)
        m.sw(e + 564, m.lw(e + 564) | 0x4000)                      # 0x8008442C
    else:
        blk(m, 0x80084434)
        m.sw(e + 568, m.lw(e + 568) | (s1 if not mut('83f30_code') else s1 ^ 1))   # 0x80084440
    blk(m, 0x80084444)
    k = m.call(FIXMUL, s4, 3664)
    st = (n, k, s0, s3, u32(m.lw(s8) - sp64), 1143, 1)
    for i, v in enumerate(st):
        m.sw(sp + 16 + 4 * i, v)
    s1 = m.call(0x80080D1C, e, s7, s8, m.lw(m.entry_sp + 4), stack=st)
    v1 = m.lw(e + 568)
    if v1 & 0x1F:
        blk(m, 0x800844A4)
        a1_ = 4096
        if mode == 3 and (v1 & 0xC):
            blk(m, 0x800844BC)
            r = m.lw(e + 852)
            m.sw(r + 552, m.lw(r + 552) | 0x10000)                 # 0x800844D0  rider +0x228
            if m.lbu(m.lw(e + 852) + 572) & 0x10:
                blk(m, 0x800844F0)
                r2 = m.lw(m.lw(e + 856) + 852)
                m.sw(r2 + 552, m.lw(r2 + 552) | 0x10000)           # 0x80084510
    else:
        blk(m, 0x80084514)
        a1_ = (1 if (v1 & 0x220) else 0) << 11
    blk(m, 0x80084520)
    m.sw(e + 568, m.lw(e + 568) | a1_)                             # 0x8008452C
    return s1
BLOCKS = [0x80083F30, 0x80083F8C, 0x80083FB8, 0x80084004, 0x80084534, 0x80084054, 0x80084068, 0x800840FC,
          0x80084124, 0x800841B4, 0x80084258, 0x80084530, 0x8008426C, 0x80084288, 0x800842B0, 0x80084330,
          0x8008434C, 0x800843B8, 0x800843DC, 0x800843F4, 0x80084420, 0x80084434, 0x80084444, 0x800844A4,
          0x800844BC, 0x800844F0, 0x80084514, 0x80084520]
