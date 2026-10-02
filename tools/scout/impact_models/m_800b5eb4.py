"""0x800B5EB4 FaceCrossing(A, B, dirV, n, [halfW, P0, P1, P2, P3, ax1, ax2, lim1, lim2, &cross, &t]) -
RASHCDG, 1752 B, frame 104; 15 arguments (a0..a3 + 11 on the stack)"""
from .h_cx import *
FRAME = 104


def _code(v, lim):
    """(v >>> 31) + (lim <s v ? 2 : 0)"""
    return (u32(v) >> 31) + (2 if s32(lim) < s32(v) else 0)


def _inv(n):
    """0x80000000 /u ((|n| >> 1) + ((|n| - 2) >> 31)), negated for n < 0 (divu: /0 -> LO = 0xFFFFFFFF)"""
    a = u32(-n) if s32(n) < 0 else u32(n)
    d = u32(sra(a, 1) + sra(u32(a - 2), 31))
    q = 0xFFFFFFFF if d == 0 else 0x80000000 // d
    return u32(-q) if s32(n) < 0 else u32(q)


def model(m, a0, a1, a2, a3):
    es = m.entry_sp
    blk(m, 0x800B5EB4)
    m.sw(es + 0, a0)                     # 0x800B5EC0  home spills into the caller's frame
    m.sw(es + 8, a2)                     # 0x800B5EEC
    m.sw(es + 12, a3)                    # 0x800B5EF0
    arg = [None] * 4 + [m.arg(k) for k in range(4, 15)]
    P0, ax1, ax2, lim1, lim2, pc, pt = arg[5], arg[9], arg[10], arg[11], arg[12], arg[13], arg[14]
    dA = [u32(m.lw(P0 + 4 * i) - m.lw(a0 + 4 * i)) for i in range(3)]
    dB = [u32(m.lw(P0 + 4 * i) - m.lw(a1 + 4 * i)) for i in range(3)]
    A1, A2 = dot3(m, dA, ax1), dot3(m, dA, ax2)          # s7, s3
    B2, B1 = dot3(m, dB, ax2), dot3(m, dB, ax1)          # s6, s1
    t1, v1 = _code(A1, lim1), _code(A2, lim2)
    c1, c0 = _code(B1, lim1), _code(B2, lim2)            # a1, a0
    m.sw(pc, 0)                                          # 0x800B6240
    m.sw(pt, 0)                                          # 0x800B624C (delay slot)
    go = False                                           # True = L6328, else fall to the projections
    if t1 == 0 and v1 == 0:
        go = True
    elif c1 == 0 and c0 == 0:
        go = True
    else:
        blk(m, 0x800B6268)
        # 0x800B6268..0x800B62B4
        if t1 != 0:
            if v1 != 0:
                if t1 == c1:
                    blk(m, 0x800B6278)
                    return u32(-1)
                if v1 == c0:
                    blk(m, 0x800B6280)
                    return u32(-1)
            else:
                if t1 == c1:
                    blk(m, 0x800B6290)
                    return u32(-1)
        else:
            if v1 == c0:
                blk(m, 0x800B6280)
                return u32(-1)
            if v1 == 0 and t1 == c1:
                blk(m, 0x800B6290)
                return u32(-1)
        if t1 == 0 and c1 == 0:
            go = True
        elif v1 == 0 and c0 == 0:
            go = True
    if not go:
        blk(m, 0x800B62B8)
        s0 = m.call(AIPROJ, P0, a3, a0)
        same = True
        for k in (6, 7, 8):
            v = m.call(AIPROJ, arg[k], a3, a0)
            if s32(s0 ^ v) < 0:
                same = False
                break
        if same:
            blk(m, 0x800B6320)
            return u32(-1)
    blk(m, 0x800B6328)
    s0 = m.call(DOTLCM, a2, ax1)
    s2 = m.call(DOTLCM, a2, ax2)
    s3, s6 = A2, B2
    if iabs(s0) < 655:
        blk(m, 0x800B6364)
        s3 = s3 if s32(s2) > 0 else u32(lim2 - s3)
        s6 = u32(lim2 - s6) if s32(s2) > 0 else s6
    elif iabs(s2) < 655:
        blk(m, 0x800B63AC)
        s3 = A1 if s32(s0) > 0 else u32(lim1 - A1)
        s6 = u32(lim1 - B1) if s32(s0) > 0 else B1
    else:
        blk(m, 0x800B63D0)
        i0, i2 = _inv(s0), _inv(s2)
        if s32(i0) > 0:
            s4 = m.call(FIXMUL, A1, i0)
            s0 = m.call(FIXMUL, u32(lim1 - B1), i0)
        else:
            s4 = m.call(FIXMUL, u32(A1 - lim1), i0)
            s0 = u32(-m.call(FIXMUL, B1, i0))
        if s32(i2) > 0:
            s3 = m.call(FIXMUL, s3, i2)
            s6 = m.call(FIXMUL, u32(lim2 - B2), i2)
        else:
            s3 = m.call(FIXMUL, u32(s3 - lim2), i2)
            s6 = u32(-m.call(FIXMUL, B2, i2))
        if s32(s4) < s32(s3):
            s3 = s4
        if s32(s0) < s32(s6):
            s6 = s0
    blk(m, 0x800B6528)
    cross = 1 if s32(s6) < s32(s3) else 0
    if mut('b5eb4_cross'):
        cross ^= 1
    m.sw(pc, cross)                                      # 0x800B6534
    mn = s3 if s32(s3) < s32(s6) else s6
    m.sw(pt, u32(arg[4] + mn))                           # 0x800B6554
    return 1
BLOCKS = [0x800B5EB4, 0x800B6268, 0x800B6278, 0x800B6280, 0x800B6290, 0x800B62B8, 0x800B6320, 0x800B6328, 0x800B6364, 0x800B63AC, 0x800B63D0,
          0x800B6528]
