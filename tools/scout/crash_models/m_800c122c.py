"""FightRestart RASHCDG 0x800C122C(me, t, side) - cfe43a77, 172 B, frame 32; callers 0x800C0824 (FightUpdate, the
AI's half, not yet in a combat stance), 0x800C14C0 (0x800C1370, the op-9 drive-by strike)."""
import pairs as P, impact as I
u32, s32, iabs = P.u32, P.s32, P.iabs
FRAME = 32
NAME = "FightRestart"
REDRAW, BEGIN, ANIMSTATE = 0x800B92C0, 0x800C12D8, 0x800CE170


def model(m, a0, a1, a2, a3):
    me, side = a0, a2
    rd = m.lw(me + 0x43C)
    if (m.lbu(rd + 0x3D) & 3) == (0 if P.MUTATE["on"] else 2):                              # combo-hit nibble & 3 == 2: a new move
        m.call(REDRAW, rd)                                       # 0x800C125C  Rand + the weapon selector
        rd = m.lw(me + 0x43C)
    if m.lbu(rd + 0x3C) & 0x80:                                  # an armed command
        w = m.lbu(rd + 0x2E)
        if w in (0, 4):                                          # nunchaku / chain
            if m.lw(ANIMSTATE + 8) == m.lw(ANIMSTATE + 12):
                m.sb(rd + 0x3C, 32)                              # 0x800C12B4  back to bare fists
    return m.call(BEGIN, me, side)                               # 0x800C12BC
