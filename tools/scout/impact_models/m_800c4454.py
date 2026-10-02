# StanceEnter(ev, r, p) RASHCDG 0x800C4454, 172 B, frame 40
from .h_dcommon import TAB, MUT
FRAME = 40


def model(m, a0, a1, a2, a3):
    r, p, ev = a1, a2, a0 & 0xFFFF
    c = m.lhu(TAB + 8 * ev + 2)
    cur = m.lhu(r + 0x220)
    t1 = p >> 8
    if c == (4 if MUT['on'] else 3):                               # the NEW stance is category 3 (MUTATION: 4)
        return m.call(0x800BFC5C, r, cur, ev, p)
    if ev != 0:
        return p & 0x8000                                          # v0 = the delay-slot andi
    anim = m.lw(m.lw(r + 0x254) + 0x21C)                           # r->f254 (its bike) ->f21C
    if p & 0x8000:
        m.sw(m.sp + 16, 11)
        m.sw(m.sp + 20, 0)
        m.sw(m.sp + 24, 0)
        return m.call(0x8005C018, anim, 0, t1 & 0xFF, 6, stack=(11, 0, 0))
    m.sw(m.sp + 16, 0)
    return m.call(0x8005BF6C, anim, 0, t1 & 0xFF, 0, stack=(0,))
