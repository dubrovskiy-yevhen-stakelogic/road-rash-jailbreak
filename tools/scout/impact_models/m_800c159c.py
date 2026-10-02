# ReachTest 0x800C159C(me, tgt, lat, along, [sp+16] u32 *flags) - RASHCDG cfe43a77, 532 B, frame 56
import sys
from pairs import u32, s32, iabs
FRAME = 56
from ._util import MUTX as MUT
FIGHT, SINE, FIXMUL = 0x8005AD4C, 0x8005624C, 0x8001FC90


def model(m, a0, a1, a2, a3):
    me, tg, lat, along = a0, a1, s32(a2), s32(a3)
    dy = s32(m.lw(tg + 188) - m.lw(me + 188))                 # +0xBC
    t0 = iabs(dy)
    rider = m.lw(me + 852)
    nid = m.lbu(u32(rider + m.lbu(rider + 570)) + 574)
    node = lambda: u32(m.lw(u32(m.lw(FIGHT) + 12 * m.lbu(rider + 569)) + 8) + 12 * nid)
    fp = m.arg(4)
    s1 = 0
    if m.lhu(node() + 8) != 0:                                # node.damage
        s1 = 1 if t0 < m.lws(me + 312) else 0                 # me+0x138
    s1 |= (m.lw(fp) >> 8) & 1
    if s1 and not (0x8000 if MUT else 0xFFFE) < iabs(along):
        d = s32(m.lw(me + 636) - m.lw(tg + 636))              # +0x27C
        if lat > 0:
            d = s32(-d)
        sn = m.lhs(SINE + ((u32(d * 163) >> 12) & 0x3FFC))
        a1v = m.lws(tg + 312)
        half = s32(u32(a1v + (u32(a1v) >> 31))) >> 1
        if t0 < half:
            q = s32(u32(a1v * 3))
            if q < 0:
                q = s32(u32(q + 3))
            q >>= 2
            r = m.call(FIXMUL, u32(sn << 4), u32(dy + q))
            r = u32(r + (m.lbu(node() + 10) << 12))           # node.reach << 12
            r = s32(u32(r + m.lw(m.lw(me + 1084) + 8) + m.lw(me + 304) + m.lw(tg + 304)))
            m.sw(fp, m.lw(fp) | (1 if iabs(lat) < r else 0))  # 0x800C1760
    f = m.lw(fp) | 2
    m.sw(fp, f)                                               # 0x800C1774
    return (1 if (f & 0x80) == 0 else 0) if s1 else 0
