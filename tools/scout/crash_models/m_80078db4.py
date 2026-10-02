"""Model of RASHCDG 0x80078DB4 (sha1 cfe43a77...), 3436 B (0x80078DB4..0x80079B20), 859 instructions, frame 64:

    void ImpactStatePass(BikeNode *head)          (name ours)

called twice per frame from the rider pass 0x8007B840: at 0x8007DCD8 with head 0x8005B350 (the impact-class
list, flagsC & 0x1FF) and at 0x8007DCE4 with head 0x8005B2D8 (the flagsC & 0x400 list).  For every bike on the
list (node at +0x440, next at node+4, entity = node - 1088) it runs the bike's impact state machine:
  * flagsC bit 4 (0x10): wait for +0x2A4 == 0, then clear it and set 0x08000000 (re-list) or 0x880;
  * flagsC bits 0..3 (the HitOutcome class): stop the controls; class 1 re-takes the heading once (0x2000);
    class 8 (class 4 with a mounted rider) posts the rider's knock-off request +0x228 |= 0x8000; class 8 (class
    4 on a wipeout bike) with a pitch below -50 deg or an expired ramp LAUNCHES the bike (random yaw kick, random
    spin axis, flagsC 0x40A00); otherwise the fall-over ramp is set up (flagsC 0x4000) and, when the ramp is
    over, the impact ends (class bits cleared, 0x08000000 re-list, a GetRCnt-picked crash sound, or
    BikeSteerLean for a bump).
Every store below carries its pc.  All addresses RASHCDG unless SLUS."""
import pairs as P
import impact as I  # noqa: F401  (house style)

u32, s32, iabs = P.u32, P.s32, P.iabs
FRAME = 64
NAME = "ImpactStatePass"

FIXMUL, FIXDIV, SQRT, SCALE = 0x8001FC90, 0x80010028, 0x8004CF74, 0x8002EE50
DOTLCM, MULADD, RAND, ASIN = 0x8002E698, 0x8002EAD8, 0x8001FC58, 0x8001FF3C
NORMALIZE, ATAN2, STEERLEAN = 0x8002E468, 0x80020018, 0x80073D74
GETRCNT, PS3D = 0x80043F00, 0x80017BA0
TRIG = 0x8005624C            # SLUS {s16 sin, s16 cos} x 4096
ARMS = {}                    # arm tag -> count (bookkeeping for the notes; not part of the comparison)


def arm(tag):
    ARMS[tag] = ARMS.get(tag, 0) + 1


def s16(v):
    return ((v & 0xFFFF) ^ 0x8000) - 0x8000


def mul_lo(a, b):
    return s32(u32(s32(a) * s32(b)))


def hi_u(a, b):
    """multu a, b; mfhi"""
    return (u32(a) * u32(b)) >> 32


def horiz_rescale(m, s1, sx, sz):
    """0x80078F54..0x8007903C / 0x80079414..0x800794FC: the horizontal part (sx, sz, both << 4) rescaled so that
    the vector with vertical s1 (<< 4) is unit: sqrt((1 - y^2) / (x^2 + z^2)), a sign-magnitude FixDiv.
    Returns (k, sx, sz) where k = SqrtGte(..) << 2 and sx/sz are the (possibly replaced) inputs."""
    a = m.call(FIXMUL, sx, sx)
    b = m.call(FIXMUL, sz, sz)
    s3 = s32(u32(a + b))
    if s3 < 6:                                                   # slti 6
        sx, sz, s3 = 0, 655, 6
    y2 = m.call(FIXMUL, s1, s1)
    if s32(u32(0x10000 - y2)) > 0:
        if s3 > 0:
            r = m.call(FIXDIV, u32(0x10000 - m.call(FIXMUL, s1, s1)), s3)
        else:
            r = u32(-m.call(FIXDIV, u32(0x10000 - m.call(FIXMUL, s1, s1)), u32(-s3)))
    else:
        if s3 > 0:
            r = u32(-m.call(FIXDIV, u32(m.call(FIXMUL, s1, s1) - 0x10000), s3))
        else:
            r = m.call(FIXDIV, u32(m.call(FIXMUL, s1, s1) - 0x10000), u32(-s3))
    k = u32(m.call(SQRT, r) << 2)
    return k, sx, sz


def sm_div(m, a, b, neg_pos):
    """a sign-magnitude FixDiv(|a|, |b|); neg_pos = the sign the guest gives the (a > 0, b > 0) quotient"""
    a, b = s32(a), s32(b)
    if a > 0:
        if b > 0:
            q = m.call(FIXDIV, a, b)
            return u32(-q) if neg_pos else q
        q = m.call(FIXDIV, a, u32(-b))
        return q if neg_pos else u32(-q)
    if b > 0:
        q = m.call(FIXDIV, u32(-a), b)
        return q if neg_pos else u32(-q)
    q = m.call(FIXDIV, u32(-a), u32(-b))
    return u32(-q) if neg_pos else q


def model(m, a0, a1, a2, a3):
    head = a0
    m.sw(m.entry_sp, head)                                       # 0x80078DE0 (the caller's home slot)
    p = m.lw(head + 4)
    m.sw(m.sp + 16, p)
    if p == head:
        arm('empty-%x' % head)
        return None
    arm('walk-%x' % head)
    while True:
        bike(m, p)
        p = m.lw(m.lw(m.sp + 16) + 4)
        m.sw(m.sp + 16, p)
        if p == m.lw(m.entry_sp):
            return None


def bike(m, node):
    e = u32(node - 1088)
    fc = m.lw(e + 0x238)
    R = m.lw(e + 0x354)
    arm('bike')
    if iabs(m.lws(e + 0x16C)) >= 3:                              # slti 3
        fc ^= 0x400000
        arm('toggle22')
    m.sw(e + 0x238, fc)                                          # 0x80078E34 (always)
    if fc & 0x10:
        if m.lw(e + 0x2A4) != 0:
            arm('b4-wait')
            return
        m.sw(e + 0x238, fc & 0xFFFFFFEF)                         # 0x80078E4C
        if m.lw(R + 0x25C) < 2:                                  # sltiu
            v1 = 1
        elif m.lw(e + 0x358) != 0 and m.lw(e + 0x440) != 0:
            v1 = 1
        else:
            v1 = 0
        arm('b4-end-%d' % v1)
        m.sw(e + 0x238, m.lw(e + 0x238) | u32((u32(-v1) & 0x07FFF780) + 0x880))   # 0x80078EA8
        return
    if (fc & 0xF) == 0:
        arm('idle')
        return
    arm('class%x' % (fc & 0xF))
    fc = m.lw(e + 0x238)
    fa = m.lw(e + 0x230)
    m.sw(e + 0x254, 0)                                           # 0x80078EC4
    m.sw(e + 0x24C, 0)                                           # 0x80078EC8
    s6 = 1 if (fc & 0x600) else 0
    m.sw(e + 0x230, fa & 0xF8000000)                             # 0x80078ED8
    v = m.lw(e + 0x1E8) if m.lws(e + 0x2D4) < m.lws(e + 0x2D8) else 0
    m.sw(e + 0x1E8, v)                                           # 0x80078EFC
    s8 = fc & 1
    fwd = u32(e + ((u32(-s6) & u32(-84)) + 528))                 # s6 ? +0x1BC (matrix row 2) : +0x210
    if s8:
        take = True
        if not (m.lw(e + 0x238) & 0x2000):
            if s32(m.call(DOTLCM, u32(node - 268), fwd)) < 0:     # e+0x334, the latched impact direction
                take = False
                arm('c1-behind')
        if take:
            if s6:
                arm('c1-take-row')
                s5 = u32(m.lhs(fwd) << 4)
                s4 = u32(m.lhs(fwd + 4) << 4)
                s1 = u32(m.lhs(e + 0x1C4) << 4)
                k, s5, s4 = horiz_rescale(m, s1, s5, s4)
                x = m.call(FIXMUL, k, s5)
                m.sh(e + 0x1C2, s32(x) >> 4)                     # 0x80079064
                z = m.call(FIXMUL, k, s4)
                m.sh(e + 0x1C6, s32(z) >> 4)                     # 0x8007907C
                m.call(SCALE, m.lw(e + 0x1E0), e + 0x1C2, e + 0x1C8)
            else:
                arm('c1-take-fwd')
                h0 = m.lhu(fwd)
                a0 = m.lhu(e + 0x204)
                v1 = m.lhu(e + 0x208)
                m.sh(e + 0x1C2, h0)                              # 0x80079094
                m.sh(e + 0x1C4, m.lhu(fwd + 2))                  # 0x800790A0
                h2 = m.lhu(fwd + 4)
                w = m.lhu(e + 0x206)
                m.sh(e + 0x32E, a0)                              # 0x800790AC
                m.sh(e + 0x332, v1)                              # 0x800790B0
                m.sh(e + 0x330, w)                               # 0x800790B4
                m.sh(e + 0x1C6, h2)                              # 0x800790B8
            m.sw(e + 0x238, m.lw(e + 0x238) | 0x2000)            # 0x800790C8
    # ---- 0x800790CC
    fc = m.lw(e + 0x238)
    if (fc & 8) or ((fc & 4) and m.lw(R + 0x25C) < 2):
        if m.lw(R + 0x25C) < 2:
            v1 = m.lws(e + 0x268)
            if v1 < s32(0xFFFF4D48):                             # -0.698 (-40 deg)
                v1 = s32(0xFFFF4D48)
                arm('ko-floor')
            m.sw(e + 0x268, v1)                                  # 0x80079130
            if (not s6 and v1 < 0) or m.lws(e + 0x2D8) < m.lws(e + 0x2D4):
                arm('ko-request')
                w = m.lw(R + 0x228)
                if m.lw(R + 0x25C) < 2:
                    w |= 0x8000
                m.sw(R + 0x228, w)                               # 0x8007916C
                if m.lbu(m.lw(e + 0x354) + 0x23C) & 0x10:
                    arm('ko-second')
                    R2 = m.lw(m.lw(e + 0x358) + 0x354)           # no null test of +0x358
                    if m.lw(R2 + 0x25C) < 2:
                        m.sw(R2 + 0x228, m.lw(R2 + 0x228) | 0x8000)    # 0x800791BC
            else:
                arm('ko-no')
        # ---- 0x800791C0
        fc = m.lw(e + 0x238)
        if not (fc & 8) and not (s6 and (fc & 4)):
            arm('c4-no-launch')
            return
        if not ((not s6 and m.lws(e + 0x268) < s32(0xFFFF209A)) or m.lws(e + 0x2D8) < m.lws(e + 0x2D4)):
            arm('launch-no')
            return
        launch(m, e, R, s6)
        return
    # ---- 0x80079680
    fall(m, e, R, s6, s8)


def launch(m, e, R, s6):
    """0x80079218..0x8007967C: the bike is thrown"""
    if s6 and (m.lw(e + 0x238) & 4):
        arm('launch-c4-wipeout')
        a1 = m.lws(e + 0x1E0)
        if a1 < 0x20000:
            a1 = 0x20000
        h = [m.lhu(e + 0x204), m.lhu(e + 0x206), m.lhu(e + 0x208)]
        m.sw(e + 0x1E8, a1)                                      # 0x80079258
        m.sh(e + 0x31C, h[0])                                    # 0x8007925C
        m.sh(e + 0x31E, h[1])                                    # 0x80079260
        m.sh(e + 0x320, h[2])                                    # 0x80079268
    else:
        if not s6:
            arm('launch-lift')
            m.call(MULADD, e + 0x1F8, e + 0x20A, u32(-(m.lw(e + 0x134) << 1)), e + 0xB8)
            m.call(MULADD, e + 0xB8, e + 0x210, m.lw(e + 0x134), e + 0xB8)
        arm('launch-throw')
        v = m.call(FIXMUL, 0xE666, m.lw(R + 0x1E0))             # 0.9 x the rider's +0x1E0
        m.sw(e + 0x1E0, v)                                       # 0x800792B8
        if s32(v) < 0xA0000:
            v = 0xA0000                                          # floor 10.0
        m.sw(e + 0x1E0, v)                                       # 0x800792C4
        r = m.call(RAND)                                         # Rand #1: the yaw kick
        q = hi_u(r, 0xE6C2B449) >> 8
        d = s16(u32(r - 284 * q - 142))
        t = s16(d + 142 if d > 0 else d - 142)                  # +-[142..284] (12.5..25 deg)
        arm('yaw+' if t > 0 else 'yaw-')
        t0 = mul_lo(t, m.lhs(e + 0x32C))
        lo = mul_lo(t, m.lhs(e + 0x328))
        m.sh(e + 0x1C4, m.lhu(e + 0x32A))                       # 0x80079334
        a0 = u32(m.lhs(e + 0x1C4) << 4)
        m.sh(e + 0x1C2, m.lhu(e + 0x328) + (t0 >> 12))           # 0x80079348
        m.sh(e + 0x1C6, m.lhu(e + 0x32C) - (lo >> 12))           # 0x80079360
        asn = m.call(ASIN, a0)
        idx = u32(1251 - asn) & 0xFFF
        s0 = m.lhu(TRIG + ((idx << 2) | 2))                     # the cosine half
        sp_ = m.lws(e + 0x1E0)
        if 0x8DC28 < sp_:                                        # speed > 8.86
            if sp_ > 0:
                q = u32(-m.call(FIXDIV, 0x8DC28, sp_))
            else:
                q = m.call(FIXDIV, 0x8DC28, u32(-sp_))
            a0 = q >> 4                                          # srl
            arm('pitch-by-speed')
            if s16(a0) < s16(s0):
                a0 = s0
            s0 = a0
        y = s16(s0)
        if m.lhs(e + 0x1C4) != y:
            arm('pitch-rescale')
            s3 = u32(m.lhs(e + 0x1C2) << 4)
            s4 = u32(m.lhs(e + 0x1C6) << 4)
            m.sh(e + 0x1C4, s0)                                  # 0x80079418
            k, s3, s4 = horiz_rescale(m, u32(y << 4), s3, s4)
            x = m.call(FIXMUL, k, s3)
            m.sh(e + 0x1C2, s32(x) >> 4)                         # 0x80079524
            z = m.call(FIXMUL, k, s4)
            m.sh(e + 0x1C6, s32(z) >> 4)                         # 0x8007952C
        v1 = m.lws(e + 0x1E0)
        if 0xA0000 < v1:
            v1 = 0xA0000
        m.sw(e + 0x1E8, v1)                                      # 0x8007954C
        r = m.call(RAND)                                         # Rand #2: spin axis x (and z)
        s0 = s16(u32(r - 714 * (hi_u(r, 0x16F26017) >> 6) - 357))
        m.sh(e + 0x31C, m.lhu(e + 0x1C6) + (mul_lo(s0, m.lhs(e + 0x1C2)) >> 12))   # 0x800795B0
        r = m.call(RAND)                                         # Rand #3: spin axis y
        m.sw(e + 0x2D8, 0)                                       # 0x800795C4
        m.sh(e + 0x31E, u32(r - 714 * (hi_u(r, 0x16F26017) >> 6) - 357))            # 0x800795F8
        m.sh(e + 0x320, (mul_lo(s0, m.lhs(e + 0x1C6)) >> 12) - m.lhu(e + 0x1C2))  # 0x8007960C
        m.call(NORMALIZE, e + 0x31C)
    # ---- 0x80079610
    fl = 0x40A00 if not P.MUTATE["on"] else 0x40200             # MUTATION: bit 11 (0x800, "launch") dropped
    m.sw(e + 0x238, (m.lw(e + 0x238) & 0xFFF00200) | fl)         # 0x80079634
    b2 = m.lw(e + 0x358)
    if b2 != 0 and m.lw(e + 0x440) != 0:
        arm('launch-second')
        m.sw(b2 + 0x238, m.lw(b2 + 0x238) | 0x40000)             # 0x80079654
    v1 = m.lws(e + 0x268)
    if v1 < s32(0xFFFF209A):
        v1 = s32(0xFFFF209A)                                     # floor -0.873 (-50 deg)
    m.sw(e + 0x268, v1)                                          # 0x8007967C


def fall(m, e, R, s6, s8):
    """0x80079680..0x80079AD4: the fall-over ramp, and the end of the impact"""
    if not s8:
        if not s6 and m.lws(e + 0x26C) < 0 and m.lws(e + 0x268) <= 0:
            arm('fall-skip-pitch')
            return
        if not s6 and m.lw(R + 0x25C) >= 2 and iabs(m.lws(e + 0x28C)) != 0x1921F:
            ramp(m, e)
            return
    # ---- 0x8007991C
    if m.lws(e + 0x2D4) < m.lws(e + 0x2D8):
        arm('end-wait-ramp')
        return
    if not s8 and not s6 and m.lws(e + 0x268) < 0:
        arm('end-wait-pitch')
        return
    msk = u32(-s8)
    a0 = m.lw(e + 0x268)
    m.sw(e + 0x26C, msk & m.lw(e + 0x26C))                       # 0x80079960
    v = m.lw(e + 0x270)
    m.sw(e + 0x2B0, 0)                                           # 0x80079968
    m.sw(e + 0x2A4, 0)                                           # 0x8007996C
    m.sw(e + 0x270, msk & v)                                     # 0x80079978
    m.sw(e + 0x268, msk & a0)                                    # 0x80079980
    if s8:
        arm('end-bump')
        m.call(STEERLEAN, e)
        v1 = 0
        if m.lw(R + 0x25C) >= 2:
            v1 = 1 if m.lw(e + 0x358) == 0 else 0
        arm('end-bump-launch%d' % v1)
        m.sw(e + 0x238, m.lw(e + 0x238) | (u32(-v1) & 0x880))    # 0x800799C0
    else:
        t0 = 0x1C9C4 if (m.lw(e + 0x230) & 0x300) else 0         # 1.78
        m.sw(e + 0x1E8, 0)                                       # 0x800799D4
        f = [m.lhu(e + 0x210 + 2 * k) for k in range(3)]
        g = [m.lhu(e + 0x204 + 2 * k) for k in range(3)]
        m.sw(e + 0x240, t0)                                      # 0x800799F8
        m.sh(e + 0x1C2, f[0])                                    # 0x800799FC
        m.sh(e + 0x1C4, f[1])                                    # 0x80079A00
        m.sh(e + 0x1C6, f[2])                                    # 0x80079A04
        m.sh(e + 0x32E, g[0])                                    # 0x80079A08
        m.sh(e + 0x330, g[1])                                    # 0x80079A0C
        m.sh(e + 0x332, g[2])                                    # 0x80079A10
        if m.lw(R + 0x25C) < 2:
            arm('end-mounted')
            a0 = m.lw(e + 0x230)
            if (a0 & 0x08002000) == 0x08000000:
                a0 |= 0x6000
                arm('end-mounted-6000')
            m.sw(e + 0x230, a0)                                  # 0x80079A48
        else:
            arm('end-down')
            rc = m.call(GETRCNT, 0xF2000002)                     # root counter 2: HARDWARE
            m.call(PS3D, m.lw(e + 0xB8), m.lw(e + 0xC0), (((rc & 0xFF) * 5) >> 8) + 50, 0)
            m.sw(e + 0x238, m.lw(e + 0x238) | 0x900)             # 0x80079A88
    # ---- 0x80079A8C
    v1 = m.lw(e + 0x230)
    if m.lw(R + 0x25C) < 2:
        v1 |= 0x10080
    m.sw(e + 0x230, v1)                                          # 0x80079AA8
    v1 = m.lw(e + 0x238)
    a0 = v1 & 0xFFFF9FF0
    m.sw(e + 0x238, a0)                                          # 0x80079AC4
    if not (v1 & 0x600):
        a0 |= 0x08000000
    m.sw(e + 0x238, a0)                                          # 0x80079AD0
    m.sw(e + 0x33C, 0)                                           # 0x80079AD4


def ramp(m, e):
    """0x800796F0..0x80079918: the fall-over (roll) ramp from +0x28C to +-pi/2"""
    if m.lw(e + 0x294) != 0:
        arm('ramp-busy')
        return
    if m.lw(e + 0x238) & 0x4000:
        arm('ramp-done')
        return
    arm('ramp')
    a1 = m.lws(e + 0x26C)
    s3 = 0x8000
    if a1 >= 66:
        s3 = s32(sm_div(m, m.lw(e + 0x270), a1, True))
        arm('ramp-rate')
    if s3 < 0x8000:
        s3 = 0x8000
    v1 = 0x1921F if m.lws(e + 0x28C) > 0 else 0xFFFE6DE1
    m.sw(e + 0x294, v1)                                          # 0x800797B4
    if m.lw(e + 0x358) != 0 and m.lw(e + 0x440) != 0:
        arm('ramp-second')
        m.sw(e + 0x294, 0xFFFE6DE1)                              # 0x800797CC
    a0 = u32((m.lw(e + 0x294) - m.lw(e + 0x28C)) << 1)
    a0 = sm_div(m, a0, s3, False)
    v0 = u32(m.lw(e + 0x2D8) - m.lw(e + 0x2D4))
    v1 = m.lw(e + 0x28C)
    m.sw(e + 0x294, a0)                                          # 0x80079840
    m.sw(e + 0x290, 0)                                           # 0x80079844
    m.sw(e + 0x240, 0)                                           # 0x80079848
    m.sw(e + 0x1E8, 0)                                           # 0x8007984C
    m.sw(e + 0x2D8, v0)                                          # 0x80079850
    m.sw(e + 0x298, v1)                                          # 0x80079858
    if s32(v0) < 0:
        v0 = 0
    m.sw(e + 0x2D8, v0)                                          # 0x8007986C
    m.sw(e + 0x2D4, 0)                                           # 0x80079870
    m.sw(e + 0x2B0, 0)                                           # 0x80079878
    d1 = m.call(DOTLCM, e + 0x210, e + 0x32E)
    d2 = m.call(DOTLCM, e + 0x210, e + 0x1C2)
    a = m.call(ATAN2, d1, d2)
    a0 = s32(u32(s32(a) * 25736)) >> 8                           # 4096-unit angle -> 16.16 radians
    m.sw(e + 0x2A4, a0)                                          # 0x800798C4
    m.sw(e + 0x2A8, sm_div(m, a0, s3, True))                     # 0x8007990C
    m.sw(e + 0x238, m.lw(e + 0x238) | 0x4000)                    # 0x80079918
