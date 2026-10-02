"""FightEnd RASHCDG 0x800C1014(rider) - cfe43a77, 248 B, frame 24; callers 0x800BFD5C (0x800BFD24: a combat stance
left for a non-combat one), 0x800C0430 (FightUpdate's lock arm: the human's stance in 30..37)."""
import pairs as P, impact as I
u32, s32, iabs = P.u32, P.s32, P.iabs
FRAME = 24
NAME = "FightEnd"
GS, LASTSLOT, LASTTIME = 0x8005B2F8, 0x800CCB6C, 0x800CCB70
AIPOP = 0x800BC8DC


def model(m, a0, a1, a2, a3):
    r = a0
    B = m.lw(r + 0x254)                                          # the rider's bike
    m.sb(r + 0x239, 40 if P.MUTATE["on"] else 41)              # 0x800C102C  41: no FIGHT.BIN record
    m.sb(r + 0x23A, 0)                                           # 0x800C1030  node index
    if not (m.lw(B + 0x230) & 0x08000000) and not (m.lbu(r + 0x23C) & 0x40):   # the human (not GATED)
        top = u32(B + 8 * (m.lbs(B + 0x3B2) - 1))                # lb: signed depth
        if m.lhu(top + 0x3BC) == 16:                             # the top command is the fight
            gs = m.lw(GS)
            h = m.lhu(B + 0xAC)
            if h < m.lw(gs + 0x30):                              # sltu: a player's bike
                m.sh(LASTSLOT + 2 * h, m.lhu(top + 0x3BE))       # 0x800C10B4  u16 last opponent
                h = m.lhu(B + 0xAC)
                m.sw(u32(LASTTIME + 4 * h), m.lw(gs + 0x10))     # 0x800C10C8  s32 race clock
            m.call(AIPOP, B)                                     # 0x800C10CC
    an = m.lw(r + 0x21C)
    m.sw(an + 0x24, m.lw(an + 0x24) & 0xFFFFFF7F)   # 0x800C10E8  anim bit 7
    v = m.lbu(r + 0x23C) & 0xFD
    m.sb(r + 0x23C, v)                                           # 0x800C10F8  rider+0x23C bit 1 cleared
    return v
