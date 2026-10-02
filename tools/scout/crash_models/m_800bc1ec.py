# CanEngage 0x800BC1EC(bike me, bike t, s32 along) - RASHCDG cfe43a77, 784 B, frame 40; one callee AiProject.
# Returns 1 when t may be fought (live, mounted, within the lateral / longitudinal window, fast enough for an AI
# attacker, not beyond the road edge for an AI attacker), else 0. It returns ONLY 0 or 1.
import pairs as P, impact as I
u32, s32, iabs = P.u32, P.s32, P.iabs
FRAME = 40
NAME = "CanEngage"
GS, AIPROJ = 0x8005B2F8, 0x800B6AAC
LATW, ALONGW, CLS, EDGE = 0x80052F84, 0x80052F80, 0x80052EE4, 0x80052F50
AIBIT = 0x08000000


def model(m, a0, a1, a2, a3):
    me, t = a0, a1
    if m.lhs(t + 320) == 0:                                   # (s16) t+0x140: not live
        return 0
    mnt = m.lw(m.lw(t + 852) + 604)                           # t->rider+0x25C
    if not mnt < 2:                                           # sltiu: off the bike
        return 0
    if not m.lhu(t + 172) < m.lw(m.lw(GS) + 48):              # t is not a player ...
        if mnt != 1:                                          # ... and not fully mounted
            return 0
    gs = m.lw(GS)
    if m.lbu(gs + 57) == 3 and m.lhu(t + 172) < m.lw(gs + 48):   # Jailbreak phase 3, t a player
        return 1
    lim = m.lw(LATW)                                          # 8.0
    if m.lhu(t + 172) < m.lw(m.lw(GS) + 48):
        lim = u32(lim << 2)                                   # x4 for a player target
    v1 = m.lw(t + 360)
    if v1 == m.lw(me + 360) and ((v1 >> 16) == 0 or m.lw(t + 336) == m.lw(me + 336)):
        d = s32(m.lw(t + 344) - m.lw(me + 344))              # the same road piece: road laterals
        if m.lws(me + 364) < 0:
            d = s32(-d)
    else:
        d = s32(m.call(AIPROJ, u32(t + 184), u32(me + 432), u32(me + 184)))
    p = m.lw(t + 856)
    if p != 0 and d < 0:                                      # a passenger: widen by both half-widths
        d = s32(d + u32(m.lw(t + 304) + m.lw(p + 304)))
    if s32(lim) < iabs(d):
        return 0
    a = iabs(a2)
    lim2 = m.lw(ALONGW)                                       # 16.0
    if m.lhu(t + 172) < m.lw(m.lw(GS) + 48):
        lim2 = u32(lim2 << 2)
    if s32(lim2) < a:
        return 0
    if m.lw(me + 560) & AIBIT:                                # an AI attacker: the target's speed
        cls = m.lbu(m.lw(me + 1084) + 1) & 15
        k = m.lbu(CLS + 36 * cls + 18)
        v = s32(u32(k * m.lws(m.lw(me + 556) + 224)))         # mult low word; me+0x22C -> +0xE0
        if v < 0:
            v += 127
        v >>= 7
        if m.lws(t + 480) < v:                                # t+0x1E0 (speed)
            return 0
    x = m.lws(t + 344)
    if m.lw(t + 372) == 0 or m.lhu(t + 362) != 0:
        s2 = 0x39999
    else:
        sel = m.lhs(t + 408) if x < 0 else m.lhs(t + 420)     # the road half-width on t's side
        s2 = u32(m.lws(t + 424) * sel)
    e = s32(u32(iabs(x) - s2))
    if m.lw(me + 560) & AIBIT:
        if m.lhu(t + 362) != 1:
            if s32(m.lw(EDGE)) < e:                           # 2.0 beyond the road edge
                return 0
    return 0 if P.MUTATE["on"] else 1                         # MUTATION: the accepting return
