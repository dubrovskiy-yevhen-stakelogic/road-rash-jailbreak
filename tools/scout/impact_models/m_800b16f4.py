"""0x800B16F4 LandingSpeedCut(e, pSpeed) - RASHCDG, 644 B, frame 56"""
from .h_cx import *
FRAME = 56


def _q(m, e, neg_pos, neg_neg):
    """one of the five `x = e->f1CC; x > 0 ? FixDiv(x, 8.0) : FixDiv(-x, 8.0)` arms; returns (x > 0, v0)"""
    x = m.lw(e + 460)
    if s32(x) > 0:
        return True, m.call(FIXDIV, x, 0x80000)
    return False, m.call(FIXDIV, u32(-x), 0x80000)


def model(m, a0, a1, a2, a3):
    e, psp = a0, a1
    blk(m, 0x800B16F4)
    h = m.lhu(e + 172)
    if h < m.lw(m.lw(GS) + 48):
        blk(m, 0x800B1738)
        view = u32(0x800CD898 + h * 1132)
        pos, v = _q(m, e, 0, 0)                          # 1: s3 = sign(3.0 - q)
        v1 = u32(0x30000 - v) if pos else u32(0x30000 + v)
        s3 = sra(v1, 31)
        pos, v = _q(m, e, 0, 0)                          # 2: s1 = sign(q)
        s1 = sra(v, 31) if pos else sra(u32(-v), 31)
        pos, v = _q(m, e, 0, 0)                          # 3: s0 = q
        s0 = v if pos else u32(-v)
        pos, v = _q(m, e, 0, 0)                          # 4: -q
        v = u32(-v) if pos else v
        s0 = u32(s0 + (s1 & v))                          # max(q, 0)
        pos, v = _q(m, e, 0, 0)                          # 5: 3.0 - q
        v1 = u32(0x30000 - v) if pos else u32(0x30000 + v)
        val = u32(s0 + (s3 & v1))                        # max(q,0) + min(3.0 - q, 0) = clamp(q, 0, 3.0)
        if mut('b16f4_view'):
            val = u32(val + 1)
        m.sw(view + 740, val)                            # 0x800B1868  view +0x2E4
    blk(m, 0x800B186C)
    d = m.call(DOTLCM, e + 450, e + 444)
    k = 0xE666 if s32(d) >= 0 else 6553
    v = m.call(FIXMUL, k, m.lw(psp))
    m.sw(e + 576, v)                                     # 0x800B1894  +0x240
    m.sw(e + 568, m.lw(e + 568) & 0xFFFFFBFF)            # 0x800B18A4  +0x238 &= ~0x400
    r = m.lw(e + 852)
    m.sw(r + 552, m.lw(r + 552) & 0xFFFFDFFF)            # 0x800B18B4  rider +0x228 &= ~0x2000
    if 0x30000 < m.lws(e + 768):
        blk(m, 0x800B18CC)
        m.call(PS3D, m.lw(e + 184), m.lw(e + 192), 54, 0)
        if (m.lhu(e + 172) < m.lw(m.lw(GS) + 48) and m.lw(m.lw(e + 852) + 604) < 2
                and m.lw(0x8005B220) == 0):
            blk(m, 0x800B1924)
            m.sw(m.sp + 16, 1)
            m.call(RUMBLE, e, 0, m.lw(psp), 0x165A1C, stack=(1,))
    blk(m, 0x800B1944)
    v0 = m.lw(e + 568) | 0x0C000000
    m.sw(e + 568, v0)                                    # 0x800B1950
    return v0
BLOCKS = [0x800B16F4, 0x800B1738, 0x800B186C, 0x800B18CC, 0x800B1924, 0x800B1944]
