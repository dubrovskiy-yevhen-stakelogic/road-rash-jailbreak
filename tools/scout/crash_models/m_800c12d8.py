"""FightBegin RASHCDG 0x800C12D8(me, side) - cfe43a77, 152 B, frame 32; callers 0x800C120C (FightStart),
0x800C12BC (FightRestart). Picks the FIGHT.BIN record (0x800BF978), clears the rider's fight scratch and plays the
current node's animStart through StanceEvent."""
import pairs as P, impact as I
u32, s32, iabs = P.u32, P.s32, P.iabs
FRAME = 32
NAME = "FightBegin"
FIGHT, PICK, STANCE = 0x8005AD4C, 0x800BF978, 0x800C4550


def model(m, a0, a1, a2, a3):
    me, side = a0, a1
    r = m.lw(me + 0x354)
    m.sb(r + 0x23C, m.lbu(r + 0x23C) & 0xFD)                     # 0x800C1300 (the jal's delay slot)
    m.call(PICK, me)                                             # 0x800C12FC
    m.sw(r + 0x230, 0)                                           # 0x800C1314
    m.sw(r + 0x234, 0)                                           # 0x800C1318
    m.sb(r + 0x238, 0)                                           # 0x800C131C
    rec = u32(12 * m.lbu(r + 0x239) + m.lw(FIGHT))
    n = m.lbs(u32(r + m.lbu(r + 0x23A)) + 0x23E)                 # lb: the node id, signed
    ev = m.lhu(u32(12 * n + m.lw(rec + 8)))                      # FightNode.animStart
    return m.call(STANCE, ev, r, side | (0x18 if P.MUTATE["on"] else 0x10))   # 0x800C1354
