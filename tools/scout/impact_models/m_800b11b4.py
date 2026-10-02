"""Model of RASHCDG 0x800B11B4 (the box / other-volume reaction), 236 bytes, 59 instructions, frame 40.
    s32 BoxReact(E *e, Shape *shp, u32 code, u32 flags, [entry+16] s32 *imp, [entry+20] s16 *dir)"""
import sys
pass
import pairs as P

FRAME = 40
from ._util import MUTD as MUT
u32, s32, iabs = P.u32, P.s32, P.iabs


def model(m, e, shp, code, flags):
    sp = m.sp
    imp, dirp = m.arg(4), m.arg(5)
    m.sw(sp + 16, imp)
    v = m.call(0x800AF224, e, shp, code, flags, stack=(imp,))
    if v == 0:
        return v
    d = s32(m.call(0x8002E698, e + 0x1C2, dirp))
    if 0xDDB2 < d:
        s0 = 1
    elif d < -0xDDB2:
        s0 = 3
    else:
        x = s32(u32(m.lhs(e + 0x1C2) * m.lhs(dirp + 4)))   # mult / mflo
        y = s32(u32(m.lhs(e + 0x1C6) * m.lhs(dirp + 0)))
        s0 = (0 if x < y else 1) << 1                  # slt; xori 1; sll 1
        if MUT["on"]:
            s0 = (1 if x < y else 0) << 1
    r = m.call(0x8001FC90, u32(-d), m.lw(e + 0x1E0))
    sg = u32(s32(r) >> 31)
    a1 = u32(r + sg) ^ sg     # addu a1,v0,t0 before the jal and `xor a1,a1,v0` in its delay slot: iabs(r). impact.py
    #                           compares it at the callee's entry probe (pairs.callee_args does not re-execute xor)
    m.sw(sp + 16, s0)
    return m.call(0x800A9408, e, a1, m.lw(shp + 0x90), m.lw(e + 0x13C), stack=(s0,))
