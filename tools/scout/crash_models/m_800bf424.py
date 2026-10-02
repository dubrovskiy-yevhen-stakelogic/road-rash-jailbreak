# NoteHit 0x800BF424(bike attacker, bike victim) - RASHCDG cfe43a77, 248 B, leaf.
# The "last blow" table at 0x800CD548, 4 records of 12 B {bike attacker, bike victim, s32 time}: record h for a
# player attacker (handle h < numPlayers), record passenger->handle + 2 for a passenger attacker (+0x440 == 0);
# an AI attacker writes nothing. Read back by 0x800BF51C (the knock-off credit).
import pairs as P, impact as I
u32, s32, iabs = P.u32, P.s32, P.iabs
FRAME = 0
NAME = "NoteHit"
GS, TAB = 0x8005B2F8, 0x800CD548


def model(m, a0, a1, a2, a3):
    gs = m.lw(GS)
    if m.lhu(a0 + 172) < m.lw(gs + 48):
        m.sw(TAB + 12 * m.lhu(a0 + 172), a0)                  # 0x800BF45C
        m.sw(TAB + 12 * m.lhu(a0 + 172) + 4, a1)              # 0x800BF478
        i = m.lhu(a0 + 172)
    else:
        if m.lw(a0 + 1088) != 0:
            return None
        m.sw(TAB + 12 * (m.lhu(m.lw(a0 + 856) + 172) + 2), a0)        # 0x800BF4BC
        m.sw(TAB + 12 * (m.lhu(m.lw(a0 + 856) + 172) + 2) + 4, a1)    # 0x800BF4E4
        i = m.lhu(m.lw(a0 + 856) + 172) + 2
    m.sw(TAB + 12 * i + 8, m.lw(gs + 16) + (1 if P.MUTATE["on"] else 0))   # 0x800BF510 (MUTATION: +1)
    return None
