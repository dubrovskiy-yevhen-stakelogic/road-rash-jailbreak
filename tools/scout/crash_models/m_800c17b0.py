# ApplyHit 0x800C17B0(bike me, bike t, u32 side, u32 flags) - RASHCDG cfe43a77, 1572 B, frame 72.
# Returns 1 when the victim's reaction stance was sent (victim still mounted), else 0.
import pairs as P, impact as I
u32, s32, iabs = P.u32, P.s32, P.iabs
FRAME = 72
NAME = "ApplyHit"
GS, DIV, NOFX = 0x8005B2F8, 0x80053188, 0x8005B220
HITSTANCE, REMEMBER, PS3D, QSND = 0x800BF860, 0x800A8BE0, 0x80017BA0, 0x80017B6C
BFE58, KNOCKOFF, NOTEHIT, SHOVE, RUMBLE, STANCE = 0x800BFE58, 0x800BF674, 0x800BF424, 0x800BF604, 0x800BFD74, 0x800C4550


def sdiv(a, b):
    a, b = s32(a), s32(b)
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def crossed(old, new):
    """0x800C1A1C..0x800C1A48: health crossing 64 or 32 downwards (old unsigned byte, new signed)"""
    if not old < 64 and new < 64:
        return True
    return not old < 32 and new < 32


def model(m, a0, a1, a2, a3):
    me, t, side, flags = a0, a1, a2, a3
    sp = m.sp
    s5 = 0
    s8 = 0
    hit = flags & 1
    rme = m.lw(me + 852)                                                  # s4
    st = m.call(HITSTANCE, rme, m.lw(t + 852), hit, u32(sp + 16)) & 0xFFFF
    m.sh(sp + 24, st)
    if hit:
        rd = m.lw(me + 1084)
        c = m.lbu(rd + 61)
        m.sb(rd + 61, (((c & 15) + 1) & 15) | (c & 0xF0))                 # 0x800C183C combo count += 1
        if not (m.lbu(rme + 572) & 0x20):
            m.sw(sp + 20, m.lhu(m.lw(me + 1084) + 66))
            h = m.lhu(t + 172) if m.lw(t + 1088) != 0 else m.lhu(m.lw(t + 856) + 172)
            m.call(REMEMBER, h, u32(sp + 20))
            m.sh(m.lw(me + 1084) + 66, m.lhu(sp + 20))                    # 0x800C1894 rd+0x42 "whom I hit"
        if not (m.lbu(m.lw(t + 852) + 572) & 0x20):
            m.sw(sp + 20, m.lhu(m.lw(t + 1084) + 64))
            m.call(REMEMBER, m.lhu(me + 172), u32(sp + 20))
            m.sh(m.lw(t + 1084) + 64, m.lhu(sp + 20))                     # 0x800C18D4 rd+0x40 "who hit me"
        base = m.lw(sp + 16)
        if m.lbu(rme + 572) & 0x80:
            base = u32(base + (base << 1))                                # x 3
        m.sw(sp + 16, base)
        if m.lbu(rme + 572) & 0x80:
            m.call(PS3D, m.lw(me + 184), m.lw(me + 192), 105, 0)
        rd = m.lw(me + 1084)
        k = 96 if m.lbu(rd + 15) < 97 else m.lbu(rd + 15)
        v = s32(u32(s32(u32(m.lw(sp + 16) * k)) * m.lbu(rd + 12)))       # two mult low words; +0x0C is lbu
        if v < 0:
            v += 16383
        s5 = v >> 14
        gs = m.lw(GS)
        tr = m.lw(t + 1084)
        hp = m.lbu(tr + 15)
        nh = s32(hp - s5)
        q = sdiv(s5 + 1, m.lw(DIV + 4 * m.lw(gs + 60)))
        if nh < 0:
            s3 = 97 if (m.lbu(rd + 1) & 15) else 96
            m.sb(tr + 15, 0)                                              # 0x800C19C0 health 0
            if m.lhu(me + 172) < m.lw(m.lw(GS) + 48) or m.lw(me + 1088) == 0:
                m.call(QSND, me, 109, 4, m.lhu(me + 172))
                m.call(QSND, me, s3, 10, m.lhu(me + 172))
        else:
            f = m.lbu(tr + 68)
            if crossed(hp, nh):
                f |= 0x20
            m.sb(tr + 68, f)                                              # 0x800C1A4C rd+0x44
            m.sb(m.lw(t + 1084) + 15, nh)                                 # 0x800C1A58 health
        sb = m.lbu(m.lw(t + 1084) + 14)
        s0 = s32(sb - q)
        if m.lbu(m.lw(t + 852) + 572) & 0x20:
            pv = m.lbu(m.lw(m.lw(t + 856) + 1084) + 14)
            if not pv < s0:
                s0 = pv
        if s0 <= 0:
            m.sb(m.lw(t + 1084) + 14, 0)                                  # 0x800C1AB8
        else:
            tr = m.lw(t + 1084)
            old = m.lbu(tr + 14)
            f = m.lbu(tr + 68)
            if crossed(old, s0):
                f |= 0x20
            m.sb(tr + 68, f)                                              # 0x800C1AF8
            m.sb(m.lw(t + 1084) + 14, s0)                                 # 0x800C1B04 rd+0x0E
        m.call(BFE58, me, 2, 0)
        m.call(BFE58, t, 2, 1)
    if m.lbu(m.lw(t + 1084) + 15) == 0:
        if not (m.lw(t + 568) & 0x3EC) and not (m.lw(t + 560) & 0x20000000):
            m.call(KNOCKOFF, t, m.lbu(m.lw(me + 1084) + 60), (side >> 8) & 1, me)
            m.call(BFE58, me, 1, 0)
            m.call(BFE58, t, 1, 1)
        p = m.lw(t + 856)
        if p != me:
            rd = m.lw(p + 1084) if m.lw(t + 1088) == 0 else m.lw(t + 1084)
            d = s32(m.lbu(rd + 37) - (m.lbu(rd + 36) >> 4))
            m.sb(rd + 37, d if d >= 0 else 0)                             # 0x800C1BE8 / 0x800C1C10 regen value
    m.call(NOTEHIT, me, t)
    if m.lw(m.lw(t + 852) + 604) < 2:                                     # the victim still on his bike
        side = side & u32(-16)
        a2_ = side ^ 0x100
        if flags & 1:
            if not (m.lw(t + 568) & 0x7FF):
                if m.lw(t + 1088) != 0:
                    rec = m.lbu(rme + 569)
                    if m.lbu(rme + 572) & 0x20:
                        rec = u32(rec - 20)
                    m.call(SHOVE, t, m.lbu(rme + 570), rec, (side >> 8) & 1)
                gs = m.lw(GS)
                go = True
                if not m.lhu(t + 172) < m.lw(gs + 48):
                    if m.lw(t + 1088) != 0 or (m.lbu(m.lw(t + 852) + 572) & 0x40):
                        go = False
                if go and m.lw(NOFX) == 0:
                    m.call(RUMBLE, t, u32(s5))
            an = m.lw(rme + 540)
            m.sw(an + 36, m.lw(an + 36) | 0x80)                           # 0x800C1D10 my rider's anim +0x24
            a2_ = side ^ 0x100
        m.call(STANCE, m.lhu(sp + 24), m.lw(t + 852), a2_ | (0 if P.MUTATE["on"] else 0x10))   # MUTATION: no 0x10
        s8 = 1
    if flags & 1:
        gs = m.lw(GS)
        go = True
        if not m.lhu(me + 172) < m.lw(gs + 48):
            if m.lw(me + 1088) != 0 or (m.lbu(m.lw(me + 852) + 572) & 0x40):
                go = False
        if go and m.lw(NOFX) == 0:
            m.call(RUMBLE, me, u32(s5))
    return s8
