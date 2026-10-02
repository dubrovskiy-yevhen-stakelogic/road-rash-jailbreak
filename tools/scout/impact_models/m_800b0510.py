"""0x800B0510 PropHitFace(e, prop, s16 out[3]) - RASHCDG, 1204 B, frame 72"""
from .h_cx import *
FRAME = 72


def _copy(m, out, src):
    for i in range(3):
        m.sh(out + 2 * i, m.lhu(src + 2 * i))


def _neg(m, out, src):
    """the negated-copy store pattern 0x800B07EC.. / 0x800B08A0.. / 0x800B0970..: 6 halfword stores"""
    m.sh(out, m.lhu(src))
    m.sh(out + 2, m.lhu(src + 2))
    v0, v1 = m.lhu(out), m.lhu(src + 4)
    m.sh(out, u32(-v0))
    v0 = m.lhu(out + 2)
    m.sh(out + 4, v1)
    m.sh(out + 4, u32(-v1))
    m.sh(out + 2, v0 if mut('b0510_neg') else u32(-v0))


def model(m, a0, a1, a2, a3):
    e, prop, out = a0, a1, a2
    sp = m.sp
    blk(m, 0x800B0510)
    d = [u32(m.lw(prop + 184 + 4 * i) - m.lw(e + 184 + 4 * i)) for i in range(3)]
    for i in range(3):
        m.sw(sp + 16 + 4 * i, d[i])
    m.call(SCALE, m.lw(prop + 480), prop + 450, prop + 456)
    for i in range(3):
        m.sw(sp + 32 + 4 * i, u32(m.lw(prop + 456 + 4 * i) - m.lw(e + 456 + 4 * i)))
    r = m.call(0x8002E604, sp + 16, sp + 32)
    if s32(r) > 0:
        blk(m, 0x800B09AC)
        return 0
    blk(m, 0x800B05BC)
    if s32(dot3(m, d, e + 444)) >= 0:
        blk(m, 0x800B0650)
        _copy(m, out, e + 444)
        return 1
    blk(m, 0x800B066C)
    if s32(dot3(m, d, e + 432)) >= 0:
        blk(m, 0x800B0700)
        _copy(m, out, e + 432)
        return 1
    blk(m, 0x800B071C)
    d = [u32(m.lw(prop + 184 + 4 * i) - m.lw(e + 196 + 4 * i)) for i in range(3)]   # from corner 0
    for i in range(3):
        m.sw(sp + 16 + 4 * i, d[i])
    if s32(dot3(m, d, e + 432)) <= 0:
        blk(m, 0x800B07EC)
        _neg(m, out, e + 432)
        return 1
    blk(m, 0x800B080C)
    if s32(dot3(m, d, e + 444)) <= 0:
        blk(m, 0x800B08A0)
        _neg(m, out, e + 444)
        return 1
    blk(m, 0x800B08C0)
    if s32(dot3(m, d, e + 438)) >= 0:
        blk(m, 0x800B0954)
        _copy(m, out, e + 438)
        return 1
    blk(m, 0x800B0970)
    _neg(m, out, e + 438)
    return 1
BLOCKS = [0x800B0510, 0x800B09AC, 0x800B05BC, 0x800B0650, 0x800B066C, 0x800B0700, 0x800B071C, 0x800B07EC,
          0x800B080C, 0x800B08A0, 0x800B08C0, 0x800B0954, 0x800B0970]
