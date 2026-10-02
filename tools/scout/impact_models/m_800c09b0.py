# FightStep 0x800C09B0(bike) - RASHCDG cfe43a77, 568 B, frame 40
import sys
from pairs import u32, s32
FRAME = 40
from ._util import MUTX as MUT
FIGHT = 0x8005AD4C
ANIMDONE, SETSTATE = 0x8005BE58, 0x800C4550
S7 = 0x08000000


def model(m, a0, a1, a2, a3):
    rider = m.lw(a0 + 852)
    s1 = m.lbu(u32(rider + m.lbu(rider + 570)) + 574)          # rider+0x23E[rider+0x23A]
    s2 = 0
    if s1 == 63:
        if m.call(ANIMDONE, m.lw(rider + 540)):
            s2 = 8
    elif m.call(ANIMDONE, m.lw(rider + 540)):
        i = m.lbu(rider + 570) + 1
        nxt = m.lbu(u32(rider + i) + 574)
        m.sb(rider + 570, i)                                  # 0x800C0A1C
        if nxt == 63:
            s2 = 4
        else:
            s1, s2 = nxt, 2
    else:
        if m.lbu(rider + 546) != 0:                           # rider+0x222 strike already fired
            return 0
        rec = u32(m.lw(FIGHT) + 12 * m.lbu(rider + 569))
        node = u32(m.lw(rec + 8) + 12 * s1)
        frame = m.lws(m.lw(rider + 540) + 16)
        hf = m.lbu(node + 11)
        v = 1 if (frame > hf if MUT else not frame < hf) else 0
        m.sb(rider + 546, v)                                  # 0x800C0A8C
        s2 = v
    fl = lambda: 272 if m.lw(rider + 552) & S7 else 16
    rec = lambda: u32(m.lw(FIGHT) + 12 * m.lbu(rider + 569))
    if s2 & 2:
        a2 = fl(); r = rec()
        m.call(SETSTATE, m.lhu(u32(m.lw(r + 8) + 12 * s1)), rider, a2)
    elif s2 & 4:
        a2 = fl(); r = rec()
        st = m.lhu(u32(m.lw(r + 8) + 12 * s1) + 2)            # animEnd of the node just finished
        if st == 224:
            st = m.lhu(r)                                     # the record's idle state
            a2 |= 2
        m.call(SETSTATE, st, rider, a2)
    elif s2 & 8:
        a2 = fl(); r = rec()
        m.call(SETSTATE, m.lhu(r), rider, a2)
    return s2 & 1
