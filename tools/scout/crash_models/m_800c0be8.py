"""FightContinue RASHCDG 0x800C0BE8(me, t, along, s) - cfe43a77, 256 B, frame 40; one caller 0x800C060C (FightUpdate,
the AI's half, when CanEngage's word has no bit 2). `s` = CanEngage's word | the passenger bit 1.
Returns 1 = keep fighting, 0 = the fight is over (backed off with op 6, or popped)."""
import pairs as P, impact as I
u32, s32, iabs = P.u32, P.s32, P.iabs
FRAME = 40
NAME = "FightContinue"
BACKOFF, AIPUSH, STANCE, AIPOP = 0x800BB8FC, 0x800BCA68, 0x800C4550, 0x800BC8DC


def model(m, a0, a1, a2, a3):
    me, t, along, s = a0, a1, a2, a3
    sp = m.sp
    if not (s & 1):                                              # out of reach
        if m.lw(me + 0x440) != 0 and m.call(BACKOFF, me, t, u32(-s32(along))):   # 0x800C0C18
            m.sh(sp + 16, 6)                                     # {op 6 "back off", target t}
            m.sh(sp + 18, m.lhu(t + 0xAC))
            m.call(AIPUSH, u32(sp + 16), 2, me)                  # 0x800C0C3C
            return 0
    else:
        d = m.lw(m.lw(t + 0x358) + 0x43C) if (s & 2) else m.lw(t + 0x43C)   # the passenger's riderDef, or t's
        hp = m.lbu(d + 0x0F)
        k = m.lbu(m.lw(me + 0x43C) + 0x3D)
        lt = ((k >> 4) < (k & 0xF)) if P.MUTATE["on"] else ((k & 0xF) < (k >> 4))
        if lt:                                                   # combo hits < limit (sltu)
            if hp != 0:                                          # the victim still has health
                return 1
    R = m.lw(me + 0x354)
    if m.lbu(R + 0x23C) & 0x40:
        m.call(STANCE, 77, R, 1)                                 # 0x800C0CC0
    m.call(AIPOP, me)                                            # 0x800C0CC8
    return 0
