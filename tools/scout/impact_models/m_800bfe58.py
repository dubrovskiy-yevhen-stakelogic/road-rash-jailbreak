# 0x800BFE58(e, col, idx) RASHCDG, 172 B, leaf: one per-player byte counter ++
from .h_dcommon import GS, MUT, u32
FRAME = 0
CNT = 0x800D81D8        # 36-byte records; the byte at +24 + col + 4*idx


def model(m, a0, a1, a2, a3):
    gs = m.lw(GS)
    h = m.lhu(a0 + 0xAC)
    if h < m.lw(gs + 48):                                   # sltu: a human player's bike
        a = u32(a1 + (a2 << 2) + 36 * h + CNT)
    else:
        v = m.lw(a0 + 0x440)
        if v != 0:
            return v
        h2 = m.lhu(m.lw(a0 + 0x358) + 0xAC) + 2
        a = u32(a1 + (a2 << 2) + 36 * h2 + CNT)
    v = m.lbu(a + 24) + (2 if MUT['on'] else 1)
    m.sb(a + 24, v)
    return v
