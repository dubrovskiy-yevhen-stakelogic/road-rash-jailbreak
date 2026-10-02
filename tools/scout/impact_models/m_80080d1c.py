"""RASHCDG 0x80080D1C (sha1 cfe43a77...), 4192 bytes, frame 80, eleven arguments:
    s32 HitSpeed(Bike *e, s16 dir[3], s32 *pSpeed, Shape *shape,
                 [entry+16] const s16 nrm[3], [+20] s32 a5, [+24] s32 a6, [+28] s32 a7,
                 [+32] s32 a8, [+36] s32 a9, [+40] s32 a10)          -> 0 or 1
Transcription grade; every guest store is reproduced in order."""
import sys
pass
import impact as I, pairs as P

u32, s32, iabs = P.u32, P.s32, P.iabs
FRAME = 80
FIXMUL, FIXDIV, DOT, MULADD16 = 0x8001FC90, 0x80010028, 0x8002E698, 0x8002EA20
SCALE, ATAN2, NORM = 0x8002EE50, 0x80020018, 0x8002E468
BIKES = 0x8005B3A0
from ._util import MUTD as MUT


def gte(m, r, ir, dst):
    """ctc2 R11/R22/R33 <- lh r[0..2]; mtc2 IR1..3 <- lh ir[0..2]; OP sf=1; sh IR1..3 -> dst[0..2]"""
    a = [m.lhs(r + 2 * k) for k in range(3)]
    b = [m.lhs(ir + 2 * k) for k in range(3)]
    o = I.gte_op(a[0], a[1], a[2], b[0], b[1], b[2])
    for k in range(3):
        m.sh(dst + 2 * k, o[k])


def rad(ang):
    """(ang * 25736) >> 8: 4096-per-turn -> 16.16 radians (x 2*pi*16), 32-bit wrap then sra"""
    return s32(u32(s32(ang) * 25736)) >> 8


def neg_div(m, a, b):
    """the 4-arm signed FixDiv idiom that yields -(a / b) (0x80081A70, 0x80081B80, 0x80081BEC)"""
    a, b = s32(a), s32(b)
    if a > 0:
        if b > 0:
            return s32(u32(-s32(m.call(FIXDIV, a, b))))
        return s32(m.call(FIXDIV, a, u32(-b)))
    if b > 0:
        return s32(m.call(FIXDIV, u32(-a), b))
    return s32(u32(-s32(m.call(FIXDIV, u32(-a), u32(-b)))))


def model(m, e, dirp, pspeed, shape):
    E = m.entry_sp
    m.sw(E + 12, shape)                       # 0x80080D20 sw a3,92(sp)
    m.sw(E + 8, pspeed)                       # 0x80080D58 sw a2,88(sp)
    nrm = m.lw(E + 16)
    a5, a6, s7, s0 = m.lws(E + 20), m.lws(E + 24), m.lws(E + 28), m.lws(E + 32)
    a9, a10 = m.lws(E + 36), m.lw(E + 40)
    sp = m.sp

    f = m.lw(e + 568)
    f = (f | 0x400000) if m.lws(e + 364) < 0 else (f & 0xFFBFFFFF)
    m.sw(e + 568, f)                          # 0x80080D8C
    s6 = m.lhu(shape) >> 5                    # partner pool
    a0 = m.lw(e + 568)

    if a0 & 0x100:
        # ---- 0x80080DB0: the airborne / reflect block ----
        if not (0x4786B < m.lws(e + 576)):
            m.sw(e + 576, 0)                  # 0x80080DC8
        else:
            v = s32(m.call(DOT, dirp, nrm))
            m.call(MULADD16, dirp, nrm, u32(-(v << 1)), e + 456)
            for k in range(3):
                m.sh(dirp + 2 * k, m.lws(e + 456 + 4 * k) >> 4)          # 0x80080E00/10/20
            gte(m, e + 522, dirp, e + 814)                              # 0x80080E70..78
            if m.call(NORM, e + 814) == 0:
                for k in range(3):
                    m.sh(e + 814 + 2 * k, m.lhu(e + 432 + 2 * k))       # 0x80080E98..A0
            gte(m, e + 814, e + 522, dirp)                              # 0x80080EEC..F4
            v = m.call(FIXMUL, 0x8000, m.lw(e + 576))
            m.sw(pspeed, v)                   # 0x80080F0C
            m.sw(e + 576, v)                  # 0x80080F10
            m.call(SCALE, m.lw(pspeed), dirp, e + 456)
            d1 = m.call(DOT, e + 528, e + 814)
            d2 = m.call(DOT, e + 528, e + 450)
            ang = m.call(ATAN2, d1, d2)
            m.sw(e + 676, rad(ang))           # 0x80080F74
        # 0x80080F78
        m.sw(e + 568, m.lw(e + 568) & 0xFFFFFFF0)                       # 0x80080F84
        b = 1 if s6 in (3, 5, 6) else 0
        m.sw(e + 568, m.lw(e + 568) | (b << 13))                        # 0x80080FC0
        ret = 1
        for k in range(3):
            m.sh(e + 820 + 2 * k, m.lhu(nrm + 2 * k))                   # 0x80081D30/3C/48
        return ret

    # ---- 0x80080FC4 ----
    s8 = 1 if (a0 & 0x600) else 0
    v1 = (1 - s8) & ((a0 >> 4) & 1)
    a1 = u32(a0 | (v1 << 27))
    a2 = a1 & 0xFFFFEFEF
    old = m.lws(e + 576)
    pv = m.lws(pspeed)
    m.sw(e + 568, a2)                         # 0x80081000
    s3 = pv if s8 else old
    lab = '1104'
    if not s8 and not (0x4786B < s3):
        if a1 & 2:
            lab = '1058'
        elif not (m.lw(e + 564) & 0x4000):
            lab = '11F4'
        elif s0 < m.lws(pspeed):
            lab = '1058'
        else:
            lab = '1104'

    if lab == '1058':
        v1 = m.lw(e + 560)
        m.sw(e + 568, a2 & 0xFFFFFFFC)        # 0x80081060
        m.sw(e + 728, 0)                      # 0x8008106C
        if not (v1 & 0x300):
            s3 = 0
        a1 = 0
        m.sw(e + 564, (m.lw(e + 564) & 0xFFFFBFFF) | 0x1000)            # 0x8008108C
        go10d0 = True
        if s6 == 0:
            other = u32(m.lw(BIKES) + u32(m.lhu(shape) * 1096))
            if m.lw(other + 560) & 0x2000:
                go10d0 = False
        if go10d0 and (m.lw(e + 560) & 0x08002000) == 0x08000000:
            a1 = 1
        s0 = 0
        m.sw(e + 560, m.lw(e + 560) | (u32(-a1) & 0x6000))              # 0x80081100
        return tail(m, e, s8, s3, pspeed, dirp, nrm, s0)

    if lab == '1104':
        if m.lw(e + 564) & 0x4000:
            s3 = s0
            if s3 < 0x10000:
                s3 = 0x10000
            if not s8 and m.lw(e + 616) != 0:
                return tail(m, e, s8, s3, pspeed, dirp, nrm, 1)
            if 0xA0000 < s3:
                a1 = 0x38000
            else:
                if s3 > 0:
                    q = m.call(FIXDIV, s3, 0xA0000)
                else:
                    q = u32(-s32(m.call(FIXDIV, u32(-s3), 0xA0000)))
                a1 = u32(m.call(FIXMUL, 0xFFFD8000, q) + 0x60000)
            v = m.call(FIXMUL, 0x40000, a1)
            v1 = m.lw(e + 616)
            m.sw(e + 624, v)                  # 0x800811A8
            m.sw(e + 632, 0)                  # 0x800811B0
            m.sw(e + 620, 0xFFFE0000)         # 0x800811B4
            m.sw(e + 628, v1)                 # 0x800811B8
            if (m.lws(pspeed) < s0) != MUT['on']:          # mutation control: the sign flip dropped
                x, y = m.lw(e + 624), m.lw(e + 620)
                m.sw(e + 624, u32(-s32(x)))   # 0x800811E8
                m.sw(e + 620, u32(-s32(y)))   # 0x800811F0
            return tail(m, e, s8, s3, pspeed, dirp, nrm, 1)
        lab = '11F4'

    # ---- 0x800811F4 ----
    s4 = iabs(a6)
    rider = m.lw(e + 852)
    if m.lw(rider + 604) < 2:
        rider_hit(m, rider, s3, dirp)                                   # 0x80081218..44
    if m.lbu(m.lw(e + 852) + 572) & 0x10:
        pr = m.lw(m.lw(e + 856) + 852)
        if m.lw(pr + 604) < 2:
            rider_hit(m, pr, s3, dirp)                                  # 0x80081288..B4
    fl = m.lw(e + 568)
    for k in range(3):
        m.sh(e + 808 + 2 * k, m.lhu(dirp + 2 * k))                      # 0x800812C0/CC/D8
    t1 = fl & 1
    if s8:
        lab = '13DC'
    elif a10 != 0 or (fl & 0xC) != 4:
        lab = '13D4'
    else:
        if m.lws(e + 616) >= 17158:
            lab = '1360'
        else:
            lab = '1334'
            if 57189 < a6 and m.lws(e + 676) < -34314:
                lab = '1360'
            if lab == '1334':
                if -57190 < a6:
                    lab = '13D4'
                elif 34314 < m.lws(e + 676):
                    lab = '1360'
                else:
                    lab = '13D4'
    if lab == '1360':
        m.sw(e + 488, 0xFFF80000)             # 0x80081378
        h = m.lhu(e + 450)
        s3 = 0x50000
        m.sh(e + 798, 0)                      # 0x80081384
        m.sw(e + 728, 0)                      # 0x80081388
        m.sw(e + 568, (m.lw(e + 568) & 0xFFF00600) | 0x200)             # 0x8008139C
        m.sh(e + 800, u32(-h))                # 0x800813A0
        m.sh(e + 796, m.lhu(e + 454))         # 0x800813A8
        v = s32(m.call(DOT, dirp, nrm))
        m.call(MULADD16, dirp, nrm, u32(-(v << 1)), sp + 16)
        m.sw(pspeed, 0x50000)                 # 0x800813D0
        lab = '14F4'
    if lab == '13D4':
        lab = '13DC' if s8 else '1434'
    if lab == '13DC':
        if m.lw(e + 568) & 4:
            for k in range(3):
                m.sw(sp + 16 + 4 * k, u32(-(m.lhs(dirp + 2 * k) << 4)))
            lab = '14C4'
        else:
            lab = '1434'
    if lab == '1434':
        n0, n2 = m.lhs(nrm), m.lhs(nrm + 4)
        d0, d2 = m.lhs(dirp), m.lhs(dirp + 4)
        if s32(u32(n2 * d0 - n0 * d2)) > 0:
            x, z = n2 << 4, -(n0 << 4)
        else:
            x, z = -(n2 << 4), n0 << 4
        m.sw(sp + 16, u32(x))
        m.sw(sp + 24, u32(z))
        v1 = (1 if 0 < m.lhs(dirp + 2) else 0) if s8 else 0
        m.sw(sp + 20, u32(-v1) & u32(m.lhs(dirp + 2) << 4))
        lab = '14C4'
    if lab == '14C4':
        if a6 < 0:
            m.sw(e + 568, m.lw(e + 568) | 0x100000)                      # 0x800814F0
        else:
            m.sw(e + 568, m.lw(e + 568) & 0xFFEFFFFF)                    # 0x800814F0
    # ---- 0x800814F4 ----
    for k in range(3):
        m.sh(dirp + 2 * k, m.lws(sp + 16 + 4 * k) >> 4)                  # 0x80081500/10/24
    if not s8:
        gte(m, e + 522, dirp, e + 814)                                   # 0x80081578..80
        if m.call(NORM, e + 814) == 0:
            for k in range(3):
                m.sh(e + 814 + 2 * k, m.lhu(e + 432 + 2 * k))            # 0x800815A0..A8
        gte(m, e + 814, e + 522, dirp)                                   # 0x800815F4..FC
    fl = m.lw(e + 568)
    m.sw(e + 828, shape)                      # 0x80081610
    if fl & 0x200:
        return tail(m, e, s8, s3, pspeed, dirp, nrm, 1)

    if t1:
        # ---- 0x80081624 ----
        if s4 > 0:
            q = m.call(FIXDIV, s4, s7) if s7 > 0 else u32(-s32(m.call(FIXDIV, s4, u32(-s7))))
        else:
            q = u32(-s32(m.call(FIXDIV, u32(-s4), s7))) if s7 > 0 else m.call(FIXDIV, u32(-s4), u32(-s7))
        v = m.call(FIXMUL, u32(-1311), q)
        s3 = s32(m.call(FIXMUL, u32(v + 0xFEB8), s3))
        a0 = s32(u32(s3 + ((s32(u32(s3 - 0x20000)) >> 31) & u32(0x20000 - s3))))
        d = s32(u32(0xA0000 - s3))
        sc = s32(u32(a0 + ((d >> 31) & u32(d))))                         # clamp(s3, 2.0, 10.0)
        if sc >= 0:
            dv = s32(u32((sc >> 1) + (s32(u32(sc - 2)) >> 31)))
            r = (0x80000000 // u32(dv)) if u32(dv) else 0xFFFFFFFF
        else:
            n = s32(u32(-sc))
            dv = s32(u32((n >> 1) + (s32(u32(n - 2)) >> 31)))
            r = u32(-((0x80000000 // u32(dv)) if u32(dv) else 0xFFFFFFFF))
        m.sw(e + 728, r)                      # 0x80081708
        a0 = s32(u32(a6 + a9)) if a6 > 0 else s32(u32(a6 - a9))
        v = s32(m.call(FIXMUL, a0, sc))
        m.sw(e + 488, v)                      # 0x80081748
        big = s6 == 8 or (s6 == 6 and all(0x10000 < m.lws(shape + o) for o in (132, 136, 140)))
        if big:
            # ---- 0x8008179C ----
            if (v ^ m.lws(e + 636)) >= 0:
                m.sw(e + 640, 0)              # 0x800817B4
                return tail(m, e, s8, s3, pspeed, dirp, nrm, 1)
            m.sw(e + 644, 0)                  # 0x800817C0
            r = s32(m.call(FIXMUL, iabs(m.lws(e + 636)), sc))
            m.sw(e + 640, r)                  # 0x800817E4
            if r < 0x10000:
                r = 0x10000
            m.sw(e + 640, r)                  # 0x800817F8
            m.sw(e + 640, u32(-r) if m.lws(e + 636) >= 0 else r)         # 0x80081804
            return tail(m, e, s8, s3, pspeed, dirp, nrm, 1)
        # ---- 0x80081808 ----
        a3 = 0
        if s6 == 0:
            other = u32(m.lw(BIKES) + u32(m.lhu(shape) * 1096))
            if m.lw(other + 568) & 1:
                if (m.lws(e + 636) ^ m.lws(e + 488)) < 0:
                    a3 = 1 if (m.lws(e + 636) ^ m.lws(other + 636)) < 0 else 0
        a1 = 5719 if s4 < 17157 else 11438
        if 34314 < s4:
            a1 += 5719
        f27c = m.lws(e + 636)
        a1 = s32(u32(f27c - a1)) if m.lws(e + 488) < 0 else s32(u32(f27c + a1))
        m.sw(e + 644, a1)                     # 0x800818B8
        keep = 1 if (a3 == 0 or (a1 ^ m.lws(e + 636)) < 0) else 0
        a0 = s32(u32(-keep) & m.lw(e + 644))
        m.sw(e + 644, a0)                     # 0x800818EC
        a2 = m.lws(m.lw(e + 556) + 228)
        v0 = s32(u32(-a2))
        v1 = s32(u32(a0 + ((s32(u32(a0 - v0)) >> 31) & u32(v0 - a0))))
        t = s32(u32(a2 - a0))
        v1 = s32(u32(v1 + (t if t < 0 else 0)))                          # clamp(a0, -lim, lim)
        m.sw(e + 644, v1)                     # 0x80081930
        r = s32(m.call(FIXMUL, u32(iabs(v1) - iabs(m.lws(e + 636))), sc))
        a1 = iabs(r)
        m.sw(e + 640, r)                      # 0x80081954
        if a1 < 0x10000:
            a1 = 0x10000
        m.sw(e + 640, a1)                     # 0x80081980
        m.sw(e + 640, a1 if m.lws(e + 636) < m.lws(e + 644) else u32(-a1))  # 0x8008198C
        return tail(m, e, s8, s3, pspeed, dirp, nrm, 1)

    # ---- 0x80081990 ----
    a0 = s32(u32(0x1921F - s4))
    a1 = s32(u32(0x1921F - s7))
    if a0 > 0:
        if a1 > 0:
            q = m.call(FIXDIV, a0, a1)
        else:
            q = u32(-s32(m.call(FIXDIV, a0, u32(s7 - 0x1921F))))
    else:
        a0 = u32(s4 - 0x1921F)
        if a1 > 0:
            q = u32(-s32(m.call(FIXDIV, a0, a1)))
        else:
            q = m.call(FIXDIV, a0, u32(s7 - 0x1921F))
    v = m.call(FIXMUL, 6553, q)
    s3 = s32(m.call(FIXMUL, v, s3))
    if s8 and s3 < 0x30000:
        s3 = 0x30000
    x = m.lws(e + 616)
    m.sw(e + 616, 0 if x > 0 else x)          # 0x80081A20
    v = s32(u32(-m.lws(pspeed)))
    a1 = s32(u32(v + (u32(v) >> 31))) >> 1
    fl = m.lw(e + 568)
    m.sw(e + 620, a1)                         # 0x80081A50
    if fl & 8:
        a0 = s32(u32(iabs(m.lws(e + 616)) + 0xDF66))
        m.sw(e + 728, neg_div(m, a0, a1))     # 0x80081A88 / 0x80081AA4 / 0x80081ABC
        m.sw(e + 624, 0)                      # 0x80081AC4
    else:
        v1 = s32(u32(a1 + ((s32(u32(a1 + 0x140000)) >> 31) & u32(0xFFEC0000 - a1))))
        t = s32(u32(0xFFFC0000 - a1))
        v1 = s32(u32(v1 + (t if t < 0 else 0)))                          # clamp(a1, -20.0, -4.0)
        m.sw(e + 620, v1)                     # 0x80081AFC
        a1 = s32(u32(iabs(m.lws(e + 616)) + 0xB2B8))
        fl = m.lw(e + 568)
        m.sw(e + 624, a1)                     # 0x80081B20
        if not (fl & 4):
            t1 = a5
            v1 = s32(u32(t1 + ((s32(u32(t1 + 0x10000)) >> 31) & u32(0xFFFF0000 - t1))))
            nt = s32(u32(-t1))
            r = m.call(FIXMUL, u32(v1 + (nt if nt < 0 else 0)), a1)      # clamp(a5, -1.0, 0)
            m.sw(e + 624, u32(-s32(r)))       # 0x80081B5C
        a0 = m.lws(e + 624)
        if a0 < 11438:
            a0 = 11438
        m.sw(e + 624, a0)                     # 0x80081B78
        a0 = s32(u32(a0 << 1))
        v = neg_div(m, a0, m.lws(e + 620))
        a0 = m.lws(e + 620)
        m.sw(e + 728, v)                      # 0x80081BE8
        v1 = neg_div(m, a0, v)
        x = m.lw(e + 616)
        m.sw(e + 624, v1)                     # 0x80081C30
        fl = m.lw(e + 568)
        m.sw(e + 632, 0)                      # 0x80081C38
        m.sw(e + 628, x)                      # 0x80081C3C
        m.sw(e + 728, u32(m.lw(e + 728) << 1))                          # 0x80081C50
        if fl & 2:
            if s6 == 3:
                if m.lw(shape + 480) == 0:
                    s3 = 0
            elif 5 <= s6 < 7:
                s3 = 0
    # ---- 0x80081C94 ----
    a1 = m.lws(e + 728)
    if a1 > 0:
        v = s32(m.call(FIXDIV, 1143, a1))
    else:
        v = s32(u32(-s32(m.call(FIXDIV, 1143, u32(-a1)))))
    m.sw(e + 488, v)                          # 0x80081CB0 / 0x80081CC0
    v1 = v if a6 >= 0 else s32(u32(-v))
    fl = m.lw(e + 568)
    m.sw(e + 488, v1)                         # 0x80081CE8
    if fl & 0xC:
        v1 = s32(u32(v1 << 2))
    m.sw(e + 488, v1)                         # 0x80081CF0
    return tail(m, e, s8, s3, pspeed, dirp, nrm, 1)


def rider_hit(m, r, s3, dirp):
    m.sw(r + 480, s3)
    m.sh(r + 456, m.lhu(dirp))
    m.sh(r + 458, m.lhu(dirp + 2))
    m.sw(r + 552, m.lw(r + 552) | 0x200000)
    m.sh(r + 460, m.lhu(dirp + 4))


def tail(m, e, s8, s3, pspeed, dirp, nrm, s0):
    """0x80081CF8: the speed hand-over, then the latched normal"""
    if s8:
        m.sw(pspeed, s3)                      # 0x80081D10
        m.call(SCALE, s3, dirp, e + 456)
    else:
        m.sw(e + 576, s3)                     # 0x80081D1C  <- entity +0x240
    for k in range(3):
        m.sh(e + 820 + 2 * k, m.lhu(nrm + 2 * k))                        # 0x80081D30/3C/48
    return s0
