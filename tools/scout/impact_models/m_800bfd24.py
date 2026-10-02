# 0x800BFD24(r, cur, ev, p) RASHCDG, 80 B, frame 24: leaving a category-3 stance for a non-3 one
from .h_dcommon import TAB, MUT
FRAME = 24


def model(m, a0, a1, a2, a3):
    rec = TAB + 8 * (a2 & 0xFFFF)
    if m.lhu(rec + 2) == 3:
        return 3
    m.call(0x80095AEC, a0, a1, rec if not MUT['on'] else rec + 8, a3)
    return m.call(0x800C1014, a0)
