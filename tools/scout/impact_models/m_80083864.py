"""RASHCDG 0x80083864 (sha1 cfe43a77...), 196 bytes, frame 48:
    void Exchange1D(s32 m1, s32 m2, s32 v1, s32 v2, [entry+16] s32 *out1, [entry+20] s32 *out2)
m1, m2 are the two masses already divided by their sum (16.16); the 1-D elastic collision:
    *out1 = max(0, FixMul(m1 - m2, v1) + FixMul(2*m2, v2))
    *out2 = max(0, FixMul(2*m1, v1) + FixMul(m2 - m1, v2))
each stored twice (before and after the clamp)."""
import sys
pass
import pairs as P
u32, s32 = P.u32, P.s32
FRAME = 48
FIXMUL = 0x8001FC90
from ._util import MUTD as MUT


def model(m, m1, m2, v1, v2):
    o1, o2 = m.lw(m.entry_sp + 16), m.lw(m.entry_sp + 20)
    a = s32(m.call(FIXMUL, u32(m1 - m2), v1))
    b = s32(m.call(FIXMUL, u32(m1 << 1) if MUT['on'] else u32(m2 << 1), v2))
    r = s32(u32(a + b))
    m.sw(o1, r)                               # 0x800838C4
    m.sw(o1, r if r >= 0 else 0)              # 0x800838D8
    a = s32(m.call(FIXMUL, u32(m1 << 1), v1))
    b = s32(m.call(FIXMUL, u32(m2 - m1), v2))
    r = s32(u32(a + b))
    m.sw(o2, r)                               # 0x800838F4
    m.sw(o2, r if r >= 0 else 0)              # 0x800838FC
    return None
