# HitRumble 0x800BFD74(bike e, s32 damage) - RASHCDG cfe43a77, 228 B, frame 24; one callee SLUS 0x8001DD74.
# The pad port: e+0xAC for a rider bike (+0x440 != 0); -1 (nothing) when rider+0x23C bit 6; else the passenger
# bike's handle + 1 (+1 more with two players). Calls 0x8001DD74(port, damage*100/128, 100, clamp(damage*150/128,0,255)).
import pairs as P, impact as I
u32, s32, iabs = P.u32, P.s32, P.iabs
FRAME = 24
NAME = "HitRumble"
GS, RUMBLE = 0x8005B2F8, 0x8001DD74


def model(m, a0, a1, a2, a3):
    e, dmg = a0, a1
    v = s32(u32(dmg * 150))
    if v < 0:
        v = s32(u32(v + 127))
    q = v >> 7
    a0_ = q if v >= 0 else 0                                  # nor(sra 31) & q
    t = 255 - q
    a3_ = s32(u32(a0_ + (t if t < 0 else 0)))                 # clamp to 0..255
    if m.lw(e + 1088) != 0:
        port = m.lhu(e + 172)
    elif m.lbu(m.lw(e + 852) + 572) & 0x40:
        port = -1
    else:
        port = m.lhu(m.lw(e + 856) + 172) + 1 + (0 if m.lw(m.lw(GS) + 48) < 2 else 1)
    if port < 0:
        return None
    w = s32(u32(dmg * 100))
    if w < 0:
        w = s32(u32(w + 127))
    m.call(RUMBLE, u32(port), u32(w >> 7), 100 + (1 if P.MUTATE["on"] else 0), u32(a3_))   # MUTATION: 101
    return None
