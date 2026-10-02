"""RASHCDG 0x80084564 (sha1 cfe43a77...), 1140 B (0x80084564..0x800849D8), frame 56 - Bounce (name ours):
    s32 Bounce(const s16 n[3], s16 dir[3], s32 *pSpeed, s32 vel[3], [e+16] s32 k, [e+20] s32 floor, [e+24] s32 flat)
Reflects the unit heading `dir` (4.12) off the contact normal `n` (4.12), scales the speed by 0.8 (0.1 when the
normal is near-horizontal, |n.y| < 0.5) with a floor, re-normalises `dir` and rebuilds the velocity
vel = Scale(*pSpeed, dir). Returns -1 (untouched) when dir . n >= 0 (moving away), else 1 for a wall-like normal,
0 for a floor-like one.
Callers (9): 0x80081F70, 0x80081FE0, 0x80082088, 0x800824E4 (0x80081D7C), 0x80083A38 (HitOutcome),
0x80084030 (ImpactTurn), 0x800A9718 (0x800A9868), 0x800B1FD4 (the wipeout arm 0x800B1978), 0x800B3600
(PropTopple 0x800B3344). Store pcs in the comments."""
import pairs as P
u32, s32, iabs = P.u32, P.s32, P.iabs
FRAME = 56
NAME = "Bounce"

FIXMUL, DOT, SQRT, ASIN, RATTAN = 0x8001FC90, 0x8002E698, 0x8004CF74, 0x8001FF3C, 0x8001FEB4
MULADD16, NORMALIZE, SCALE = 0x8002EA20, 0x8002E468, 0x8002EE50
TRIG = 0x8005624C                     # SLUS {s16 sin, s16 cos} x 4096 entries
COVER = set()


def sra(v, n):
    return s32(v) >> n


def mulhi(a, b):                      # mult a,b; mfhi  (signed 64-bit product, high word)
    return s32(u32((s32(a) * s32(b)) >> 32))


def model(m, n, d, psp, vel):
    COVER.add('ra=%x' % m.regs['ra'])
    es = m.entry_sp
    k = m.lws(es + 16)                                             # lw s0,72(sp)
    lim = m.lws(es + 20)                                           # lw s7,76(sp)
    m.sw(es + 12, vel)                                             # 0x800845B0 home slot of a3
    s1 = s32(m.call(DOT, d, n))
    if s1 >= 0:
        COVER.add("away")
        return u32(-1)                                             # 0x800849A4
    s6 = 1 if abs(m.lhs(n + 2)) < 2048 else 0                      # slti on iabs((s16)n[1])
    if s6 and m.lhs(d + 2) < 0:
        # ---- 0x800845F8: a wall-like normal, the heading pointing down: the horizontal mirror ----
        COVER.add("wall-down")
        s2 = s32(u32(-(m.lhs(n + 4) << 4)))
        s3 = s32(u32(m.lhs(n + 0) << 4))
        a = m.call(FIXMUL, s2, s2)
        b = m.call(FIXMUL, s3, s3)
        l0 = m.call(SQRT, u32(a + b))
        neg = s32(u32(l0 << 2)) < 0
        a = m.call(FIXMUL, s2, s2)
        b = m.call(FIXMUL, s3, s3)
        l1 = m.call(SQRT, u32(a + b))
        a = m.call(FIXMUL, s2, s2)
        b = m.call(FIXMUL, s3, s3)
        l2 = m.call(SQRT, u32(a + b))
        if neg:                                                    # 0x80084684
            COVER.add("inv-neg")
            x = s32(u32(-(l1 << 2))) >> 1
            y = s32(u32(-(l2 << 2) - 2)) >> 31
            dv = u32(x + y)
            inv = u32(-((0x80000000 // dv) if dv else 0xFFFFFFFF))
        else:                                                      # 0x80084700
            x = s32(u32(l1 << 2)) >> 1
            y = s32(u32((l2 << 2) - 2)) >> 31
            dv = u32(x + y)
            inv = (0x80000000 // dv) if dv else 0xFFFFFFFF
        s2 = s32(m.call(FIXMUL, s2, inv))                          # the unit horizontal tangent (-n.z, 0, n.x)
        s3 = s32(m.call(FIXMUL, s3, inv))
        d0 = m.lhs(d + 0) << 4
        d2 = m.lhs(d + 4) << 4
        a = m.call(FIXMUL, s2, d0)
        b = m.call(FIXMUL, s3, d2)
        s0 = s32(u32(u32(a + b) << 1))                             # 2 (t . d)
        v = m.call(FIXMUL, s0, s2)
        m.sh(d + 0, u32(sra(u32(v - d0), 4)))                      # 0x8008478C
        if m.lw(es + 24):                                          # the 7th argument
            COVER.add("flat")
            m.sh(d + 2, 0)                                         # 0x800847A4
        else:
            m.sh(d + 2, u32(sra(u32(0xB4FD - (m.lhs(d + 2) << 4)), 5)))   # 0x800847BC: (0.7071 - dy) / 2
        v = m.call(FIXMUL, s0, s3)
        m.sh(d + 4, u32(sra(u32(v - d2), 4)))                      # 0x800847D4
        s7 = s32(u32(lim + (u32(lim) >> 31))) >> 1                 # the floor halved (signed / 2)
    else:
        # ---- 0x800847E8: the restitution about the normal ----
        s0 = k
        if lim == 0:
            v = m.lws(psp)
            if not (v > 0x1FFFF):
                COVER.add("k0-slow")
                s0 = 0                                             # below 2.0: no extra kick
            elif not (v > 0x9FFFF):
                COVER.add("k-scaled")
                t = m.call(FIXMUL, u32(v - 0x20000), 8192)         # (speed - 2.0) / 8
                s0 = s32(m.call(FIXMUL, t, u32(s0 << 16))) >> 16
            else:
                COVER.add("k-fast")
        else:
            COVER.add("k-floor")
        s1 = s32(u32(-s1))                                         # |dir . n|
        s2 = s32(m.call(ASIN, u32(s1)))
        if s0 != 0:
            COVER.add("k!=0")
            p = s32(u32(s0 * s2))                                  # mult; mflo
            q = s32(u32((mulhi(p, 0x66666667) >> 2) - (p >> 31)))  # p / 10, truncated
            a1 = s32(u32(q + (sra(u32(q - 56), 31) & s32(u32(56 - q)))))           # min(q, 56)
            a0 = s32(u32(a1 + (sra(u32(512 - q), 31) & s32(u32(512 - q)))))        # + min(512 - q, 0)
            t = m.call(RATTAN, u32(a0))
            c = m.lhs(TRIG + (((s2 & 0xFFF) << 2) | 2))           # cos(asin)
            v = m.call(FIXMUL, u32(c << 4), t)
            s0 = s32(u32(s1 + v))
        else:
            s0 = s1
        if 0xFC28 < s1 and s0 < 0x11999:                            # nearly head-on: at least 1.1
            COVER.add("min1.1")
            s0 = 0x11999
        m.call(MULADD16, d, n, u32(s0), m.lw(es + 12))              # vel = (dir << 4) + MH(n << 4, s0)
        for i in range(3):
            m.sh(d + 2 * i, u32(sra(m.lw(m.lw(es + 12) + 4 * i), 4)))   # 0x80084924 / 34 / 44
        s7 = lim
    # ---- 0x80084948: the tail ----
    a0 = u32((u32(-s6) & 0xFFFF4CCD) + 0xCCCC)                      # s6 ? 0.1 : 0.8
    v = s32(m.call(FIXMUL, a0, m.lw(psp)))
    m.sw(psp, u32(v + (1 if P.MUTATE["on"] else 0)))                # 0x80084978  MUTATION: off by one unit
    a1 = v if s7 < v else s7
    m.sw(psp, u32(a1))                                              # 0x80084988 (the jal's delay slot)
    m.call(NORMALIZE, d)
    m.call(SCALE, m.lw(psp), d, m.lw(es + 12))
    return s6
