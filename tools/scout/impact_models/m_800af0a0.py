"""Model of RASHCDG 0x800AF0A0 (the pole-record reaction), 388 bytes, 97 instructions, frame 80.
    s32 PoleReact(E *e, Shape *shp, s16 *dir, u32 flags, [entry+16] u32 bit4, [entry+20] s32 *imp)"""
import sys
pass
import pairs as P

FRAME = 80
from ._util import MUTD as MUT
u32, s32 = P.u32, P.s32


def model(m, e, shp, dirp, flags):
    sp = m.sp
    s2 = 1 if (flags & 5) == 1 else 0
    M = u32(-s2)
    f134, f130 = m.lw(e + 0x134), m.lw(e + 0x130)
    h = u32(f130 + (M & u32(f134 - f130)))            # s2 ? +0x134 : +0x130
    t = u32(-s32(h)) if flags < 2 else h               # sltiu: unsigned
    m.call(0x8002EAD8, e + 0xB8, e + ((M & 0xC) + 432), t, sp + 24)   # MulAdd -> sp+24 (never read)
    s1 = 0
    if m.lw(e + 0x340) == shp:
        s1 = m.lbu(e + 0x235) >> 7                     # sticky before the solver
    s5 = m.lw(e + 0x1E0)                               # speed before the solver
    s6 = m.arg(4)
    st = 1 if s6 else 3
    imp = m.arg(5)
    m.sw(sp + 16, st)
    m.sw(sp + 20, imp)
    v = m.call(0x800AF3B0, e, shp, dirp, flags, stack=(st, imp))
    if v == 0:
        return v
    if s1:
        s1 = 0
    else:
        s1 = 1 if (m.lw(e + 0x234) & 0x8000) and m.lw(e + 0x340) == shp else 0   # just got on it
    a2 = (0x30000 if not MUT["on"] else 0x20000) if (m.lhu(shp) >> 5) == 6 else 0x8000
    m.sw(sp + 16, flags)
    if s2 or s1:
        a1 = s5
    else:
        a1 = u32(s32(s5) >> 2)
    return m.call(0x800A9408, e, a1, a2, 0x10000, stack=(flags,))
