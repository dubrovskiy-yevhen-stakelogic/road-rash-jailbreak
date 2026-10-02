"""Model of RASHCDG 0x800AF3B0 (sha1 cfe43a77...), the impact outcome solver.
0x800AF3B0..0x800B0510, 4448 bytes, 1112 instructions, frame 256.
    s32 ImpactSolve(E *e, Shape *shape, s16 *normal, u32 face, [entry+16] u32 flags, [entry+20] s32 *imp)
Every frame slot the guest keeps in memory is kept at its real address (m.sp + off); the caller's home
slots entry_sp+8/+12/+20 (a2, a3, imp) are written like the guest does (outside the own frame: compared).
MUT (set by the mutation control) flips one claim."""
import sys
pass
import pairs as P

FRAME = 256
from ._util import MUTD as MUT
u32, s32, iabs = P.u32, P.s32, P.iabs

FIXMUL, FIXDIV, DOTLCM, SCALE = 0x8001FC90, 0x80010028, 0x8002E698, 0x8002EE50
POOLS = 0x800CE4D0
G_3968, G_396C, G_3970 = 0x800D3968, 0x800D396C, 0x800D3970


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def mh(x, y):
    """mult x,y; (lo >> 16) | (hi << 16): bits 16..47 of the signed 64-bit product"""
    return u32((s32(x) * s32(y)) >> 16)


def neg_row(m, dst, src):
    """sp+dst[0..2] = -(u16)src[0..2] (sh of negu)"""
    for k in range(3):
        m.sh(dst + 2 * k, u32(-m.lhu(src + 2 * k)))


def model(m, e, shape, nrm, face):
    sp, esp = m.sp, m.entry_sp
    A2, A3, IMP = esp + 8, esp + 12, esp + 20          # caller's home slots 264/268/276(sp)
    flags = m.arg(4)                                   # 272(sp)
    m.sw(A2, nrm)                                      # 0x800AF3E4
    m.sw(A3, face)                                     # 0x800AF3E8
    f238 = m.lw(e + 0x238)
    s8 = m.lw(e + 0x240)
    s4 = 0
    if f238 & 0x600:
        s8 = m.lw(e + 0x1E0)
    pool = m.lhu(shape) >> 5
    m.sw(sp + 148, pool)
    if pool == 8:                                      # 0x800AF424: a pool-8 contact object
        m.sw(sp + 176, 1)
        m.sw(sp + 140, 0)
        t8 = u32(m.lhs(shape + 2))
        s5 = 0
        m.sw(sp + 144, 0)
        s7 = 1 if (t8 & 0xF) == 1 else 0
        m.sw(sp + 180, t8)
        m.sw(sp + 136, m.lw(shape + 4))
    else:                                              # 0x800AF458
        h = m.lhu(shape)
        tb = u32(POOLS + ((h >> 5) << 4))
        ent = u32(m.lw(tb) + u32(s32(m.lw(tb + 4)) * (h & 0x1F)))
        s7 = 0
        m.sw(sp + 180, 0)
        m.sw(sp + 176, 0)
        m.sw(sp + 136, m.lw(shape + 140))
        s5 = 0
        m.sw(sp + 144, ent)
        if pool == 4:
            s5 = ent                                   # a prop
        m.sw(sp + 140, 0)
        if pool == 3:
            m.sw(sp + 140, ent)                        # a traffic car
    p8 = m.lw(sp + 176)
    car = m.lw(sp + 140)
    riderped = 1 if u32(pool - 1) < 2 else 0
    m.sw(sp + 188, riderped)
    m.sw(sp + 164, 0)
    bit15 = (m.lw(e + 0x234) >> 15) & 1
    m.sw(sp + 156, bit15)
    is33c = 1 if m.lw(e + 0x33C) == shape else 0
    is340 = 1 if m.lw(e + 0x340) == shape else 0
    m.sw(sp + 152, is33c)
    m.sw(sp + 160, is340)
    sticky = 0
    if bit15:
        sticky = is340
        m.sw(sp + 164, sticky)
    m.sw(sp + 196, 0)                                  # "slow"
    crash = 1 if m.lw(e + 0x238) & 0xE else 0
    m.sw(sp + 168, crash)
    if not (s32(s8) > 0x1017D) and not sticky:
        if car == 0 or m.lw(car + 0x1E0) == 0:
            m.sw(sp + 196, 1)
    slow = m.lw(sp + 196)
    m.sw(sp + 184, 0)
    bit10 = (m.lw(e + 0x238) >> 10) & 1
    m.sw(sp + 172, bit10)

    go_tail = False
    s0 = 0
    s1 = 0
    s6 = 0
    if slow:                                           # 0x800AF5B0
        s0 = 0
        if not riderped and not s5 and not (m.lw(e + 0x230) & 0x300):
            m.sw(e + 0x240, 0)                         # 0x800AF5DC  speed target := 0
        m.sw(sp + 192, 1 if s5 else 0)
        go_tail = True
    else:
        s1 = sticky                                    # 0x800AF5EC
        m.sw(sp + 192, 0)
        if s5:
            cls = ((m.lhu(m.lw(s5) + 14) & 0xF80) >> 7)
            if u32(cls - 3) < 3:
                v1 = m.lw(s5 + 0x250)
                if not (v1 & 4) and not (s32(s8) > 0xB2D0D):
                    if v1 & 0x800:
                        s1 = 1                         # 0x800AF650
                    else:
                        m.sw(sp + 184, u32(-1))        # 0x800AF65C
                        if not (v1 & 0x200):
                            m.sw(sp + 16, s8)
                            m.call(0x800B3838, s5, e + 0x1C2, e + 0x32E, e + 0x20A, stack=(s8,))
                            m.sw(sp + 184, 1)
                            v = m.call(0x80017B30, m.lw(shape + 8))
                            m.call(0x80017BA0, m.lw(shape + 12), m.lw(shape + 20), v, 0)
                        m.sw(s5 + 0x250, m.lw(s5 + 0x250) | 0x400)   # 0x800AF6A8
        t9 = m.lw(sp + 184)                            # 0x800AF6AC
        if t9 != 0:
            s0 = 1 if s32(t9) > 0 else 0
            m.sw(IMP, 0)                               # 0x800AFF8C
            go_tail = True
    if not go_tail:
        if s5:                                         # 0x800AF6C0
            v = s32(m.call(DOTLCM, s5 + 0x1B6, s5 + 0x20A))
            if s1:
                s1 = 1
            elif m.lw(sp + 156):
                s1 = 0
            elif m.lw(e + 0x238) & 0x7FF:
                s1 = 0
            else:
                a1 = m.lw(s5 + 0x250)
                if a1 & 2:
                    s1 = 0
                elif m.lw(s5 + 0xB4) == 30 and iabs(v) < 6553:
                    s1 = 1
                else:                                  # 0x800AF734
                    cls = ((m.lhu(m.lw(s5) + 14) & 0xF80) >> 7)
                    s1 = 1 if (u32(cls - 3) < 3 and (a1 & 4)) else 0
        else:                                          # 0x800AF770
            if s1:
                pass
            elif m.lw(sp + 156):
                pass
            elif m.lw(sp + 172):
                s1 = 0
                if u32(m.lw(sp + 148) - 5) < 2 and not m.lw(sp + 152):
                    s1 = 1 if s32(m.lw(sp + 136)) < s32(m.lw(G_396C)) else 0
            else:                                      # 0x800AF7D4
                s1 = 0
                if m.lw(e + 0x238) & 0x7FF:
                    pass
                elif m.lw(sp + 148) == 0:
                    s1 = 1
                else:
                    go = True
                    if m.lw(sp + 176):
                        if flags & 8:
                            s1, go = 1, False
                        elif not s7:
                            s1, go = 0, False
                    if go:                             # 0x800AF824
                        a0 = m.lw(e + 0x268)
                        if s32(a0) < 0:
                            a0 = 0
                        v = m.call(FIXMUL, a0, u32(m.lw(G_396C) - m.lw(G_3968)))
                        s1 = 1 if s32(m.lw(sp + 136)) < s32(u32(m.lw(G_3968) + v)) else 0
        # 0x800AF86C: s6 = "fast enough to be thrown"
        s6 = 0
        if (s32(m.lw(G_3970)) < s32(s8) and not (m.lw(e + 0x238) & 0x7FF)
                and not (m.lw(e + 0x234) & 0x4000)):
            s6 = 1 if not m.lw(sp + 160) else 0
        if m.lw(sp + 164):
            s6 |= (m.lw(e + 0x234) >> 17) & 1
        elif car:                                      # 0x800AF8E8
            d = m.call(DOTLCM, car + 0x1C2, e + 0x1C2)
            v = m.call(FIXMUL, m.lw(car + 0x1E0), d)
            s4 = u32(s8 - v)
            s6 = 1 if s32(u32(m.lw(G_3970) << 1)) < s32(s4) else 0

        land = 0                                       # 0x800AF91C
        if s1 and s7:
            m.sw(sp + 156, 1)                          # 0x800AF934 (dead: read by no later path)
            s1 = 0
        elif s1:
            if m.lw(sp + 172):                         # 0x800AF948
                m.call(0x800B16F4, e, e + 0x240)
                m.sw(e + 0x234, m.lw(e + 0x234) & 0xFFFDFFFF)   # 0x800AF964
                s6 = 0
            m.call(SCALE, m.lw(e + 0x134), e + 0x210, sp + 88)
            sc = [m.lw(sp + 88 + 4 * k) for k in range(3)]
            top = [u32(m.lw(e + 0x1F8 + 4 * k) + sc[k]) for k in range(3)]
            for k in range(3):
                m.sw(sp + 72 + 4 * k, top[k])
            bot = [u32(m.lw(e + 0x1F8 + 4 * k) - sc[k]) for k in range(3)]
            for k in range(3):
                m.sw(sp + 88 + 4 * k, bot[k])
            m.sw(sp + 200, sp + 128)
            if (m.lhu(shape) >> 5) == 8:              # 0x800AF9F8: the plane of a pool-8 contact
                n = m.lw(A2)
                dt = [u32(top[k] - m.lw(shape + 8 + 4 * k)) for k in range(3)]
                for k in range(3):
                    m.sw(sp + 104 + 4 * k, dt[k])
                a = [mh(dt[k], u32(m.lhs(n + 2 * k) << 4)) for k in range(3)]
                db = [u32(bot[k] - m.lw(shape + 8 + 4 * k)) for k in range(3)]
                for k in range(3):
                    m.sw(sp + 104 + 4 * k, db[k])
                b = [mh(db[k], u32(m.lhs(n + 2 * k) << 4)) for k in range(3)]
                s1 = u32(a[2] + u32(a[1] + a[0]))
                s0 = u32(b[2] + u32(b[1] + b[0]))
                cross = False
                if s32(s1) > 0:
                    if s32(s0) < 0:
                        cross = True
                    else:
                        s1 = u32(-1)
                        m.sw(sp + 132, 0)
                        m.sw(sp + 128, 1)
                elif s32(s0) > 0:
                    if s32(s1) < 0:
                        cross = True
                    else:
                        s1 = u32(-1)
                        m.sw(sp + 132, 0)
                        m.sw(sp + 128, 1)
                else:
                    s1 = u32(-1)
                    m.sw(sp + 132, 0x10000)
                    m.sw(sp + 128, 0)
                if cross:                              # 0x800AFB98
                    v = m.call(DOTLCM, e + 0x210, m.lw(A2))
                    if s32(s1) > 0:
                        r = m.call(FIXMUL, v, s0)
                        m.sw(sp + 132, r)
                        m.sw(sp + 128, 0)
                    else:
                        r = m.call(FIXMUL, v, s1)
                        m.sw(sp + 132, r)
                        m.sw(sp + 128, 1)
                    s1 = 1
                    m.sw(sp + 132, u32(iabs(m.lw(sp + 132))))
            else:                                      # 0x800AFBFC: box against the partner's box
                pl = m.lhu(shape) >> 5
                rp = 1 if u32(pl - 1) < 2 else 0
                if pl == 4 or rp:                      # 0x800AFC14
                    t4, t3 = shape + 266, shape + 260
                    f88, f8c = m.lw(shape + 136), m.lw(shape + 140)
                    M = u32(-rp)
                    t5 = u32(f8c + (M & u32(f88 - f8c)))
                    m.sw(sp + 136, u32((f88 << 1) + (M & u32(f8c - (f88 << 1)))))
                    if m.lhs(shape + 274) >= 0:
                        t2, t1, t0, a3 = shape + 36, shape + 24, shape + 72, shape + 84
                    else:
                        t2, t1, t0, a3 = shape + 96, shape + 108, shape + 60, shape + 48
                        neg_row(m, sp + 120, shape + 266)
                        t4 = sp + 120
                else:                                  # 0x800AFC98
                    t4 = shape + 272
                    t5 = u32(m.lw(shape + 136) << 1)
                    if pl != 0:                        # 0x800AFD14
                        t3, t2, t1, t0 = shape + 260, shape + 48, shape + 60, shape + 24
                        m.sw(sp + 136, m.lw(shape + 140))
                        a3 = shape + 36
                    else:                              # a bike partner
                        m.sw(sp + 136, m.lw(shape + 132))
                        t3 = shape + 266
                        if m.lhs(shape + 262) > 0:
                            t2, t1, t0, a3 = shape + 96, shape + 48, shape + 36, shape + 84
                            neg_row(m, sp + 120, shape + 266)
                            t3 = sp + 120
                        else:
                            t2, t1, t0, a3 = shape + 60, shape + 108, shape + 72, shape + 24
                st = (u32(m.lw(e + 0x134) << 1), t2, t1, t0, a3, t3, t4, u32(m.lw(shape + 132) << 1), t5,
                      sp + 128, sp + 132)
                for k, v in enumerate(st):
                    m.sw(sp + 16 + 4 * k, v)
                s1 = m.call(0x800B5EB4, sp + 72, sp + 88, e + 0x210, e + 0x204, stack=st)
            # 0x800AFD88
            if s32(s1) < 0 and (s6 or (flags & 1)):
                if flags & 1:
                    m.sw(sp + 128, 1)
                    m.sw(sp + 132, 0)
                else:
                    m.sw(sp + 128, 0)
                    m.sw(sp + 132, m.lw(shape + 136))
                s1 = 1
            if s32(s1) > 0:
                land = 1
            elif s32(s1) < 0:
                land = -1
        if land > 0:                                   # 0x800AFDE4: on top of / latched to the partner
            m.sw(e + 0x340, shape)
            if m.lw(sp + 148) == 0:
                p = m.lw(sp + 144)
                m.sw(p + 0x230, m.lw(p + 0x230) | 0x02000000)   # 0x800AFE0C
            if not m.lw(sp + 156) and car:             # 0x800AFE30: landing on a car: the speed
                if s32(s4) < 0:
                    s4 = 0
                if s6:
                    a1 = m.lw(G_3970)
                    s4 = u32(s4 - u32(a1 << 1))
                    d = u32(a1 << 2)
                    if MUT["on"]:
                        d = u32(a1 << 1)
                    if s32(s4) > 0:
                        if s32(d) > 0:
                            v = m.call(FIXDIV, s4, d)
                        else:
                            v = u32(-s32(m.call(FIXDIV, s4, u32(-s32(d)))))
                    else:
                        if s32(d) > 0:
                            v = u32(-s32(m.call(FIXDIV, u32(-s32(s4)), d)))
                        else:
                            v = m.call(FIXDIV, u32(-s32(s4)), u32(-s32(d)))
                    s4 = v
                    lo = s4 & ~u32(s32(s4) >> 31)
                    hi = u32(0x10000 - s4)
                    t = u32(lo + (u32(s32(hi) >> 31) & hi))
                    v = m.call(FIXMUL, t, u32(-9830))
                    s8 = m.call(FIXMUL, u32(v + 0xE666), m.lw(e + 0x240))
                else:
                    s8 = u32(s8 - (s32(s4) >> 1))
                m.sw(e + 0x1E0, s8)                    # 0x800AFEF0
                m.sw(e + 0x240, s8)                    # 0x800AFEF4
            m.sw(sp + 16, m.lw(sp + 132))
            m.call(0x800AD9BC, e, m.lw(sp + 136), m.lw(sp + 128), s6, stack=(m.lw(sp + 132),))
            if m.lw(e + 0x238) & 0x400:
                m.sw(e + 0x33C, m.lw(e + 0x340))       # 0x800AFF30
            m.sw(IMP, 0)                               # 0x800AFF34
            s0 = 0
        elif land < 0:
            m.sw(IMP, 0)                               # 0x800AFF8C
            s0 = 0
        else:                                          # 0x800AFF48
            s0 = 0 if (m.lw(sp + 148) == 0 and m.lw(sp + 172) and m.lw(sp + 152)) else 1
            if s5:
                m.sw(sp + 192, 1)

    # 0x800AFF90 the tail
    if m.lw(sp + 192):                                 # a prop partner: push the prop instead
        imp = m.lw(IMP)
        x, y, z = (u32(s16(-m.lhu(imp + k))) for k in (0, 4, 8))
        m.sw(imp + 0, x)                               # 0x800AFFC4
        m.sw(imp + 4, y)                               # 0x800AFFD0
        m.sw(imp + 8, z)                               # 0x800AFFE4
        m.call(0x800A8DF0, s5, imp, 1)
        m.sw(IMP, 0)                                   # 0x800AFFF4
        s0 = 0
        if m.lw(s5 + 0x1E0) == 0:
            s0 = 1
        elif m.call(0x800B0510, e, s5, sp + 64):
            s0 = 1
        if s0:
            s0 = 0
            if m.call(0x800B2F94, s5, e, sp + 64):
                s0 = 1 if m.lw(sp + 196) == 0 else 0
    replaced = False
    if m.lw(sp + 172):                                 # 0x800B0050
        n = m.lw(A2)
        if iabs(s16(m.lhu(n + 2))) < 2048:
            if m.lw(sp + 148) == 0 or not (s32(m.lw(sp + 136)) > 32767):
                m.sw(A2, sp + 64)                      # 0x800B00B8
                neg_row(m, sp + 64, e + 0x20A)
                replaced = True
    if not replaced:
        imp = m.lw(IMP)
        if imp:
            m.call(0x800A8DF0, e, imp, 1)              # 0x800B00F4 ApplyImpulse(e, imp, 1)
    if s0 and not m.lw(sp + 168):                      # 0x800B0104
        if car:
            v1 = m.call(0x80083928, e, car, m.lw(A2))
        else:
            fl, fc = flags, m.lw(A3)
            B = 1 if (m.lw(sp + 176) and (m.lw(sp + 180) & 0x200)) else 0
            if m.lw(sp + 188) or s5:
                kind = 2
            elif (fl & 2) and (fc & 5) == 0:
                kind = 1 if fc != 2 else 0
            elif fl & 4:
                kind = 3
            else:
                kind = 5 - B
            v1 = m.call(0x80083F30, e, shape, m.lw(A2), kind)
    else:                                              # 0x800B0398
        v1 = 0
        if not m.lw(sp + 164) and (m.lw(e + 0x234) & 0x8000):
            v1 = 1 if m.lw(e + 0x340) == shape else 0
    if m.lw(sp + 176):
        return v1
    if m.lw(sp + 188) or s32(m.lw(sp + 184)) < 0:
        return 1
    if v1 == 0:
        return 0
    if car or m.lw(sp + 196):
        return 1
    if not m.lw(sp + 192):
        m.call(0x80017BA0, m.lw(shape + 12), m.lw(shape + 20), u32(m.lhs(shape + 150)), 0)
    gs = m.lw(0x8005B2F8)
    if (m.lhu(e + 0xAC) < m.lw(gs + 48) and m.lw(m.lw(e + 0x354) + 0x25C) < 2
            and m.lw(0x8005B220) == 0):
        a3 = u32((u32(-s5) & 0xD6945) + 0x165A1C)
        st = 2 if s5 else 1
        m.sw(sp + 16, st)
        m.call(0x800B658C, e, 0, s8, a3, stack=(st,))
    return 1
