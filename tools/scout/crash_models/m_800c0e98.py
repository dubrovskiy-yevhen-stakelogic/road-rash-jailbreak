"""FightPace RASHCDG 0x800C0E98(me, t, along, dv) - cfe43a77, 380 B, leaf; one caller 0x800C0E50 (FightSteer).
Sets the AI's commanded speed me+0x39C from the target's speed and the longitudinal offset `along`.
FightSteer's only callee that is not a seam; modelled because a port of FightSteer needs it."""
import pairs as P, impact as I
u32, s32, iabs = P.u32, P.s32, P.iabs
FRAME = 0
NAME = "FightPace"
K, GS, SPEEDTAB = 0x80052F50, 0x8005B2F8, 0x80053024


def model(m, a0, a1, a2, a3):
    me, t, along = a0, a1, s32(a2)
    k4c = m.lws(K + 0x4C)
    if k4c < along:                                              # far behind the target (along > K4C)
        gs = m.lw(GS)
        v = u32(m.lw(t + 0x1E0) - m.lw(u32(4 * m.lw(gs + 0x3C) + SPEEDTAB)))
        m.sw(me + 0x39C, v)                                      # 0x800C0EE8
        return v
    if along < s32(-k4c):                                        # far ahead: the bike's own top speed
        m.sw(me + 0x39C, m.lw(m.lw(me + 0x22C) + 0xE0))          # 0x800C0F04
        gs = m.lw(GS)
        pl = 1 if m.lhu(t + 0xAC) < m.lw(gs + 0x30) else 0
        f = m.lw(me + 0x234) | (0x200 if pl else 0)
        m.sw(me + 0x234, f)                                      # 0x800C0F34  flagsB bit 9 when the target is a player
        return pl
    a3_ = 1 if iabs(along) < m.lws(K + 0x20) else 0
    if a3_ and m.lhu(t + 0xAC) < m.lhu(me + 0xAC) and (m.lw(t + 0x230) & 0x08000000):
        top = u32(t + 8 * m.lbs(t + 0x3B2) + 0x3BC)
        if m.lhu(top - 6) == m.lhu(me + 0xAC):                   # the target's top command is aimed at me
            op = m.lhu(top - 8)
            if 14 <= op < 17:                                    # ops 14..16: leave the pace alone
                return 1
    h = iabs(along)
    h = s32(h + (u32(h) >> 31)) >> 1                             # |along| / 2, truncating
    a3_ |= 1 if 0x10000 < h else 0
    v = h if a3_ else 0x10000
    d = s32(-v) if along >= 0 else v
    if P.MUTATE["on"]:
        d = s32(-d)
    r = u32(m.lw(t + 0x1E0) + d)
    m.sw(me + 0x39C, r)                                          # 0x800C1008
    return r
