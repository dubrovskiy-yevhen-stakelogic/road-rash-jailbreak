# 0x800BFC5C(r, cur, ev, p) RASHCDG, 200 B, frame 40: entering a category-3 stance
from .h_dcommon import TAB, MUT
FRAME = 40


def model(m, a0, a1, a2, a3):
    s3, s2, s1 = a3 >> 8, (a3 >> 16) & 0xFF, a2

    def armed():
        return m.lbu(m.lw(m.lw(a0 + 0x254) + 0x43C) + 0x3C) & 0x80     # r->bike->riderDef[+0x3C] bit 7
    if m.lhu(TAB + 8 * (a1 & 0xFFFF) + 2) != 3:                         # coming from a non-3 stance
        if not armed():
            return 1 if MUT['on'] else 0        # MUTATION: v0 1
        m.call(0x800958F0, a0, a3 & 0x100)
    if not armed():
        return 0
    return m.call(0x800C2F84, s1 & 0xFFFF, a0, s3 & 0xFF, s2)
