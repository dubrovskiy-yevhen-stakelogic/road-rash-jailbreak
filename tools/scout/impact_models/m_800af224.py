"""Model of RASHCDG 0x800AF224 ImpactGate, 396 bytes, 99 instructions, frame 56.
    s32 ImpactGate(E *e, Shape *shape, u32 code, u32 flags, [entry+16] s32 *imp)"""
import sys
pass
import pairs as P

FRAME = 56
from ._util import MUTD as MUT
u32, s32 = P.u32, P.s32


def model(m, e, shape, code, flags):
    sp = m.sp
    imp = m.arg(4)                                     # lw v0,72(sp)
    s1, t0 = 6, 0
    if code != 0:
        if code & 0x100:                               # the partner's face `flags & 0xFF`, side from e's heading
            m.sw(sp + 16, 0)
            m.call(0x800B675C, shape + 24, shape + 260, flags & 0xFF, sp + 24, stack=(0,))
            d = s32(m.call(0x8002E698, e + 0x1C2, sp + 24))
            if d < -0xDDB2:
                s1 = 3
            elif 0xDDB2 < d:
                s1 = 1
            else:
                s1 = ((code & 1) << 1) ^ (code & 2)
        elif flags & 0x200:                            # the partner's face, side = code & 0xFF
            s1 = code & 0xFF
            m.sw(sp + 16, 0)
            m.call(0x800B675C, shape + 24, shape + 260, flags & 0xFF, sp + 24, stack=(0,))
        else:                                          # e's own face `code & 0xFF`, negated
            s1 = code & 0xFF
            m.sw(sp + 16, 0)
            m.call(0x800B675C, e + 0xC4, e + 0x204, s1, sp + 24, stack=(0,))
            for k in (0, 4, 2):
                m.sh(sp + 24 + k, u32(-m.lhu(sp + 24 + k)))
        t0 = 1
    else:                                              # code 0: only when riding on this very shape
        if not (m.lw(e + 0x234) & 0x8000) or m.lw(e + 0x340) != shape:
            return 0
    if s32(u32(m.lw(e + 0x138) << 1)) < s32(m.lw(shape + 140)):
        t0 |= 4 if not MUT["on"] else 2
    m.sw(sp + 16, t0)
    m.sw(sp + 20, imp)
    return m.call(0x800AF3B0, e, shape, sp + 24, s1, stack=(t0, imp))
