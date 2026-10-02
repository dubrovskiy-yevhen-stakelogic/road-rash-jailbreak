# StanceLeave(ev, r, p) RASHCDG 0x800C4500, 80 B, frame 24
from .h_dcommon import TAB, MUT
FRAME = 24


def model(m, a0, a1, a2, a3):
    cur = m.lhu(a1 + 0x220)
    if m.lhu(TAB + 8 * cur + 2) == (4 if MUT['on'] else 3):          # the CURRENT stance is category 3
        return m.call(0x800BFD24, a1, cur, a0 & 0xFFFF, a2)        # a3 = p
    return 3                                                       # v0 = the li 3 of the compare
