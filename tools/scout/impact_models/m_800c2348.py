# CombatDecode 0x800C2348(pad, bike) - RASHCDG cfe43a77, 580 B, frame 40
import sys
from pairs import u32, s32
FRAME = 40
from ._util import MUTX as MUT
EDGE, SND, PUSH = 0x800C2178, 0x80017BA0, 0x800C1DD4

def model(m, a0, a1, a2, a3):
    pad, e = a0, a1
    rd = m.lw(e + 1084)                                      # +0x43C riderDef
    edge = lambda n: m.call(EDGE, pad, n)
    cmd = None
    taunt = False
    if edge(8):
        cmd = 77
    elif edge(4):
        cmd = 75
    elif edge(3):
        cmd = 71
    elif edge(7):
        w = m.lbu(rd + 46)
        if w == 9:
            cmd = 38
        elif w == 5 or w == 1:
            cmd = 146 if (MUT and w == 5) else 148
        elif u32(w - 6) & 0xFF < 3:
            taunt = True
    elif edge(5):
        w = m.lbu(rd + 46)
        if u32(w - 2) < 2 or w == 0 or w == 8:
            cmd = 148
        elif u32(w - 6) & 0xFF < 2:
            taunt = True
    elif edge(6):
        w = m.lbu(rd + 46)
        if w in (4, 6, 7):
            cmd = 148
        elif w == 8:
            taunt = True
    elif edge(1):
        w = m.lbu(rd + 46)
        if w == 9:
            cmd = 32
        elif u32(w - 6) & 0xFF < 3:
            taunt = True
        else:
            cmd = 142
    elif edge(2):
        w = m.lbu(rd + 46)
        if w == 9:
            cmd = 36
        elif u32(w - 6) & 0xFF < 3:
            taunt = True
        else:
            cmd = 146
    if taunt:                                                 # 0x800C2540
        m.call(SND, m.lw(e + 184), m.lw(e + 192), 78, 0)
        return 0
    if cmd is None:
        return 0
    m.sb(rd + 60, cmd)                                        # 0x800C2534 riderDef+0x3C
    m.call(PUSH, e)                                           # 0x800C2558
    m.sb(m.lw(e + 852) + 573, 0)                              # 0x800C2568 rider+0x23D
    return 1
