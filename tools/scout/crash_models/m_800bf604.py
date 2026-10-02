# HitShove 0x800BF604(bike victim, u8 node, u8 record, s32 side) - RASHCDG cfe43a77, 112 B, leaf.
# When `node` is the record's designated shove node (SLUS u8 0x80053210[record]), the victim gets a sideways kick:
# +0x2D0 := tab[0] << 8, flagsB +0x234 |= 0x01000000, +0x290 := +-(tab[1] << 10) (negated for side 1), with
# tab = SLUS u8[2] 0x800531E8 + 2 * record.
import pairs as P, impact as I
u32, s32, iabs = P.u32, P.s32, P.iabs
FRAME = 0
NAME = "HitShove"
KEY, TAB = 0x80053210, 0x800531E8


def model(m, a0, a1, a2, a3):
    if (a1 != m.lbu(u32(a2 + KEY))) != P.MUTATE["on"]:        # MUTATION: the key test inverted
        return None
    p = u32(TAB + 2 * a2)
    m.sw(a0 + 720, m.lbu(p) << 8)                             # 0x800BF63C  +0x2D0
    m.sw(a0 + 564, m.lw(a0 + 564) | 0x01000000)               # 0x800BF654  +0x234
    v1 = u32(m.lbu(p + 1) << 10)
    mask = u32(-v1 - v1)
    m.sw(a0 + 656, u32(v1 + (u32(-a3) & mask)))               # 0x800BF668  +0x290 = side ? -v1 : v1
    return None
