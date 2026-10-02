# ComboEdge 0x800C2178(pad, action) - RASHCDG cfe43a77, 160 B, leaf, no frame
import sys
from pairs import u32, s32
FRAME = 0
from ._util import MUTX as MUT
MAP, GS = 0x800CCB78, 0x8005B2F8

def model(m, a0, a1, a2, a3):
    p = u32(MAP + u32(a1 << 2))                     # 0x800C2178..84
    tbl = m.lw(a0 + 184)                            # pad+0xB8: control -> slot table
    slot = m.lw(u32(u32(m.lhu(p) << 2) + tbl))
    stamp = m.lws(u32(a0 + u32(slot << 3)) + 20)    # slot record +0x00 (record+0x14+8*slot)
    if stamp <= 0:                                  # blez 0x800C21B0
        return 0
    lim = m.lws(m.lw(GS) + 32)                      # game_state+0x20
    if lim < s32(m.lw(a0) - stamp):                 # slt, signed; pad+0 is the record clock
        return 0
    k = m.lhu(p + 2)
    if k == 15 or MUT:                              # 15 = no second button
        return 1
    slot2 = m.lw(u32(u32(k << 2) + tbl))
    return 1 if m.lws(u32(a0 + u32(slot2 << 3)) + 20) > 0 else 0   # slt v0,zero,v0: held (level)
