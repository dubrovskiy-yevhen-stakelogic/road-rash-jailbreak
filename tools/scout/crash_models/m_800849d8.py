"""RASHCDG 0x800849D8 (sha1 cfe43a77...), 528 B (0x800849D8..0x80084BE8), frame 40 - Spin (name ours):
    void Spin(Bike *e)
The random spin-out after a Bounce: unless flagsC bit 19 (0x80000) is set, zero +0x2D4, set +0x2D8 = 0.5, draw
three Rand()s (SLUS 0x8001FC58):
    #1 -> +0x290 = 2 * (r1 %u 45752 - 22876)      (multu 0x2DD65ECF, mfhi >> 13)
    #2 -> +0x280 = 2 * (r2 %u 45752 - 22876)
    #3 -> +0x1E8 = FixMul(+0x1E8, 0.8 + r3 %u 26215)   (multu 0x4FFF8801, mfhi >> 13; 26215 = 0.4)
then clamps the yaw rate +0x1E8 by the speed (|.| <= +0x1E0 / 3 below 10.0, else 10.0; at least +-1.5 by an
exact min/max mask idiom) and gives it the sign that turns the heading +0x1C2 away from the +0x31C/+0x320 row.
Callers (6): 0x80081F84, 0x80081FF4, 0x8008209C (0x80081D7C), 0x80083A4C (HitOutcome), 0x80084044
(ImpactTurn), 0x800B1FF8 (the wipeout arm 0x800B1978)."""
import pairs as P
u32, s32 = P.u32, P.s32
FRAME = 40
NAME = "Spin"
RAND, FIXMUL = 0x8001FC58, 0x8001FC90
COVER = set()


def mulhu(a, b):
    return ((u32(a) * u32(b)) >> 32) & 0xFFFFFFFF


def mulhi(a, b):
    return s32(u32((s32(a) * s32(b)) >> 32))


def model(m, e, a1, a2, a3):
    COVER.add('ra=%x' % m.regs['ra'])
    if m.lw(e + 0x238) & 0x80000:                                  # flagsC bit 19: no spin
        COVER.add("bit19")
        return None
    m.sw(e + 0x2D4, 0)                                             # 0x80084A08
    m.sw(e + 0x2D8, 0x8000)                                        # 0x80084A10 (the jal's delay slot)
    r = m.call(RAND)
    s1 = u32(u32(r - 45752 * (mulhu(r, 0x2DD65ECF) >> 13)) - 22876)   # r %u 45752 - 22876
    r = m.call(RAND)
    s0 = u32(u32(r - 45752 * (mulhu(r, 0x2DD65ECF) >> 13)) - 22876)
    r = m.call(RAND)
    f = u32(r - 26215 * (mulhu(r, 0x4FFF8801) >> 13))                 # r %u 26215
    m.sw(e + 0x290, u32(s1 << 1))                                  # 0x80084AB0
    m.sw(e + 0x280, u32(s0 << 1))                                  # 0x80084AB4
    v = m.call(FIXMUL, m.lw(e + 0x1E8), u32(f + 0xCCCC))
    m.sw(e + 0x1E8, v)                                             # 0x80084B08
    a0 = m.lws(e + 0x1E0)
    if not (a0 > 0x9FFFF):
        COVER.add("slow")
        a0 = s32(u32(mulhi(a0, 0x55555556) - (a0 >> 31)))            # speed / 3
    else:
        COVER.add("fast")
        a0 = 0xA0000
    a1 = m.lws(e + 0x1E8)
    a2 = 0x18000
    if a1 < 0:
        COVER.add("neg")
        a2 = -0x18000
        a0 = s32(u32(-a0))
    a3 = s32(u32(m.lhs(e + 0x31C) * m.lhs(e + 0x1C6)))              # mult; mflo
    t1 = s32(u32(m.lhs(e + 0x320) * m.lhs(e + 0x1C2)))
    a0 = s32(u32(a0 - a1))
    v1 = s32(u32(a1 + ((s32(u32(a1 - a2)) >> 31) & s32(u32(a2 - a1)))))    # max(a1, a2)
    v1 = s32(u32(v1 + ((a0 >> 31) & a0)))                                # + min(lim - a1, 0)
    if P.MUTATE["on"]:
        v1 = s32(u32(v1 + 1))                                      # MUTATION: the clamp off by one unit
    m.sw(e + 0x1E8, u32(v1))                                       # 0x80084BA0 (delay slot: always)
    c = s32(u32(a3 - t1))                                          # e.31C x e.1C2 (y component)
    if (c < 0 and v1 > 0) or (c > 0 and v1 < 0):
        COVER.add("flip")
        m.sw(e + 0x1E8, u32(-m.lws(e + 0x1E8)))                      # 0x80084BC8
    return None
