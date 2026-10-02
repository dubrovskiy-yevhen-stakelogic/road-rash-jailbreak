# HitStance 0x800BF860(rider me, rider victim, s32 hit, u32 *damage) - RASHCDG cfe43a77, 280 B, leaf, one jump table.
# Returns the VICTIM's reaction stance (FIGHT.BIN node animHit / animMiss) and, on a hit, stores the node's base
# damage through a3. A victim rider with +0x23C bit 5 gets a remapped stance through the table at 0x8005BA04.
import pairs as P, impact as I
u32, s32, iabs = P.u32, P.s32, P.iabs
FRAME = 0
NAME = "HitStance"
FIGHT, JT, JTN = 0x8005AD4C, 0x8005BA04, 6


def _case(i):
    """the stance a jump-table case loads: every target is `j 0x800BF970; li v1,N` -> N"""
    t = P.word_at(JT + 4 * i)
    assert P.word_at(t) == 0x0802FE5C, hex(t)                # j 0x800BF970
    w = P.word_at(t + 4)
    assert w >> 16 == 0x2403, hex(w)                          # li v1,imm
    return w & 0xFFFF


def model(m, a0, a1, a2, a3):
    n = m.lbu(u32(a0 + m.lbu(a0 + 570)) + 574)                # rider+0x23E[rider+0x23A]
    rec = m.lbu(a0 + 569)                                     # rider+0x239
    node = u32(m.lw(u32(m.lw(FIGHT) + 12 * rec) + 8) + 12 * n)
    if a2 != 0:
        m.sw(a3, m.lhu(node + 8))                             # 0x800BF8AC: *damage = node.damage
        s = m.lhu(node + (6 if P.MUTATE["on"] else 4))        # animHit (MUTATION: animMiss read instead)
    else:
        s = m.lhu(node + 6)                                   # animMiss
    if m.lbu(a1 + 572) & 0x20:
        i = u32(s - 30)
        s = _case(i) if i < JTN else 87
    return s
