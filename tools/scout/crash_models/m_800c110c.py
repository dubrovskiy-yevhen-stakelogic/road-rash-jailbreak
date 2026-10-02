"""FightStart RASHCDG 0x800C110C(me, t) - cfe43a77, 288 B, frame 32; one caller 0x800C0460 (FightUpdate, the human's
half, his rider not in a category-3 stance). Chooses the swing side (0 / 0x100) and calls FightBegin 0x800C12D8."""
import pairs as P, impact as I
u32, s32, iabs = P.u32, P.s32, P.iabs
FRAME = 32
NAME = "FightStart"
RAND, AIPROJ, BEGIN = 0x8001FC58, 0x800B6AAC, 0x800C12D8


def model(m, a0, a1, a2, a3):
    me, t = a0, a1
    r = m.lw(me + 0x354)
    m.sb(r + 0x23C, m.lbu(r + 0x23C) & 0xFE)                     # 0x800C1134  rider+0x23C bit 0 cleared
    s0 = 0
    if t == 0 and m.lw(me + 0x358) == 0:                         # no target, no passenger: a random side
        s0 = (m.call(RAND) & 1) << 8                             # 0x800C1150  the only Rand
    elif m.lw(me + 0x440) == 0:
        if t == m.lw(me + 0x358):
            s0 = 0x100
    elif m.lw(me + 0x358) != 0:
        if t != m.lw(me + 0x358):
            s0 = 0x100
    else:
        v1, v0 = m.lw(t + 0x168), m.lw(me + 0x168)
        if v1 != v0 or ((v1 >> 16) != 0 and m.lw(t + 0x150) != m.lw(me + 0x150)):
            m.call(AIPROJ, u32(t + 0xB8), u32(me + 0x1B0), u32(me + 0xB8))   # 0x800C11E4  result discarded
        d = s32(m.call(AIPROJ, u32(t + 0xB8), u32(me + 0x1B0), u32(me + 0xB8)))   # 0x800C11F4
        if (d > 0) if P.MUTATE["on"] else (d < 0):               # bgez: 0 counts as side 0
            s0 = 0x100
    return m.call(BEGIN, me, s0)                                 # 0x800C120C
