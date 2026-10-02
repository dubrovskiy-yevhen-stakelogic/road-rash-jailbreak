# SetRiderState(ev, e, p) RASHCDG 0x800C2FF4, 272 B, leaf, no frame
from .h_dcommon import TAB, GS, MUT, u32
FRAME = 0


def model(m, a0, a1, a2, a3):
    e = a1
    if (m.lhu(e + 0xAC) >> 5) != 1:                                # not a pool-1 rider
        v1 = m.lw(e + 0x228)
        m.sh(e + 0x220, a0)
        m.sw(e + 0x228, (((v1 | 0x20000000) & 0xFFFF00FF) | 0x300))
        return 0xFFFF00FF
    c = m.lhu(TAB + 8 * (a0 & 0xFFFF) + 2)
    if c == 0:
        mount = 0
    elif c < 5:
        mount = 1
    elif c < 6:
        mount = 2
    elif c >= 7:
        mount = 4
    else:
        mount = 3
    gs = m.lw(GS)
    v1 = m.lw(e + 0x228)
    m.sw(e + 0x25C, mount)
    m.sh(e + 0x220, a0)
    clk = m.lw(gs + 16)
    v1 = v1 | 0x02000000
    m.sw(e + 0x228, v1)
    m.sw(e + 0x224, clk)
    v = (v1 | 0x20000000) if u32(mount - 2) < 2 else (v1 & 0xDFFFFFFF)
    m.sw(e + 0x228, v)
    v = m.lw(e + 0x228)
    v = (v | 0x08000000) if (a2 & (0x200 if MUT['on'] else 0x100)) else (v & 0xF7FFFFFF)
    m.sw(e + 0x228, v)
    m.sb(e + 0x222, 0)
    return v
