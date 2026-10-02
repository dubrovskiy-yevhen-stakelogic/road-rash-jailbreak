"""FightSteer RASHCDG 0x800C0CE8(me, t, along) - cfe43a77, 432 B, frame 48; one caller 0x800C064C (FightUpdate, the
AI's half, flagsC & 0x600 clear and the AI-driven bit set). Keeps the AI beside its target: a lateral bias through
the lateral setter 0x800BC4FC and a commanded speed through FightPace 0x800C0E98."""
import pairs as P, impact as I
u32, s32, iabs = P.u32, P.s32, P.iabs
FRAME = 48
NAME = "FightSteer"
K, GS = 0x80052F50, 0x8005B2F8
EVENT, SIDETEST, LATSET, PACE = 0x8001A760, 0x800BC618, 0x800BC4FC, 0x800C0E98


def model(m, a0, a1, a2, a3):
    me, t, along = a0, a1, s32(a2)
    dv = s32(m.lw(t + 0x1E0) - m.lw(me + 0x1E0))                 # s4: target speed - own speed
    s1 = s32(u32(iabs(dv) << 2))                                 # 4 |dv|, sll
    k4c = m.lws(K + 0x4C)
    if not (k4c < s1):
        s1 = k4c                                                 # max(4|dv|, K[0x4C])
    if iabs(along) < s1:
        a1_, a0_ = m.lws(t + 0x158), m.lws(me + 0x158)
        if s32(m.lw(t + 0x16C) ^ m.lw(me + 0x16C)) < 0:
            lat = s32(a1_ + a0_)                                 # opposite road directions
        else:
            lat = s32(a1_ - a0_)
        if m.lws(K + 0x20) < iabs(along):
            s0 = 10
        elif iabs(lat) < m.lws(K + 0x24):
            s0 = 9
        else:
            s0 = 11
        if s0 == 11:
            gs = m.lw(GS)
            if m.lhu(t + 0xAC) < m.lw(gs + 0x30):                # sltu: the target is a player
                m.call(EVENT, m.lhu(me + 0xAC), 0)               # 0x800C0DF0  SLUS 0x8001A760
        b = m.lws(u32(K + 4 * s0))                               # K[9] 0x80052F74, K[10], K[11]
        if lat > 0:
            b = s32(-b)
        if m.call(SIDETEST, t, m.lhu(me + 0xAC), u32(b)):        # 0x800C0E24
            b = s32(-b)
        m.call(LATSET, me, t, u32(b if not P.MUTATE["on"] else b + 1))   # 0x800C0E3C
    m.call(PACE, me, t, u32(along), u32(dv))                     # 0x800C0E50
    v = m.lw(me + 0x234)
    m.sw(me + 0x394, 0)                                          # 0x800C0E60
    m.sw(me + 0x398, 0)                                          # 0x800C0E64
    v &= 0xFFF7FFFF
    m.sw(me + 0x234, v)                                          # 0x800C0E6C  flagsB bit 19 cleared
    return v
