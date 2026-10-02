# StanceGate(ev, r, &p) RASHCDG 0x800C3E9C, 1464 B, frame 88
from .h_dcommon import TAB, PERSTATE, BANKS, MUT, cat
FRAME = 88
NOREPEAT = (11, 77, 13, 78, 4)


def model(m, a0, a1, a2, a3):
    r, ev, pp = a1, a0, a2                      # s6, s1, [sp+96]
    m.sw(m.sp + 96, a2)                         # the caller's home slot of a2 (reloaded at 0x800C43F8)
    red = 224                                   # [sp+32] u16: the stance 0x800C37B0 redirects to
    p = m.lw(pp)                                # s4
    s8 = p & 6                                  # bits 1..2: the play mode
    s7 = p >> 8                                 # bits 8..31 (a3 of the play call is & 0xFF)
    b0 = p & 1                                  # [sp+36]
    b16 = (p >> 16) & 0xFF                      # [sp+40]
    m.sh(m.sp + 32, red)                        # the guest keeps these three locals in its frame
    m.sw(m.sp + 36, b0)
    m.sw(m.sp + 40, b16)
    rider = r if (m.lhu(r + 0xAC) >> 5) == 1 else 0      # s0
    cur = m.lhu(r + 0x220)                      # s2
    anim = m.lw(r + 0x21C)                      # s5
    mode = p & 0x18                             # bits 3..4: the acceptance rule
    if m.lhs(r + 0x140) == 0:
        return 0
    e16 = ev & 0xFFFF
    if e16 in NOREPEAT:
        p &= 0xFFFFFFFE
        s7 |= 0x40
    if cat(m, cur) == 2 and s8 == 4:
        s8 = 2
    if cur == e16 and cur in NOREPEAT:
        return 0
    # acceptance (0x800C3FD4 .. 0x800C408C)
    ok = 0
    if rider == 0 or mode == 16 or cur == 224:
        ok = 1
    elif mode == 8:
        ok = 1 if m.lw(TAB + 8 * cur + 4) & (1 << (cat(m, e16) & 31)) else 0
    elif mode == 0:
        if m.lw(TAB + 8 * cur + 4) & (1 << (cat(m, e16) & 31)):
            ok = 1
        elif not (p & 0x20):
            red = m.call(0x800C37B0, rider, e16) & 0xFFFF
            m.sh(m.sp + 32, red)
            ok = 1 if red != 224 else 0
    if not ok:
        return 0
    if red != 224:
        ev = red                                # s1 = the redirected stance
    elif rider and not (p & 0x20) and m.lhu(rider + 0x260) != 0:
        m.sh(rider + 0x260, 0)                  # drop the queued path
    if cur == 0 and (ev & 0xFFFF) != 4:
        return 0
    m.sw(anim + 36, (m.lw(anim + 36) & 0xFFFFFF7F) | ((p & 0x40) << 7))
    e16 = ev & 0xFFFF
    rc, rn = TAB + 8 * cur, TAB + 8 * e16
    if (m.lw(rc) & 0xF) != (m.lw(rn) & 0xF):   # a different bank
        m.call(0x80012858, anim, m.lw(BANKS + 4 * (m.lw(rn) & 0xF)))
        if m.lhu(rc + 2) == 5 and m.lhu(rn + 2) == 6:     # category 5 (leaving) -> 6 (tumbling)
            if m.lbu(rider + 0x23C) & 0x20:
                m.call(0x80068D20, m.lw(m.lw(rider + 0x254) + 0x358), rider, 1)
            else:
                m.call(0x80068D20, m.lw(rider + 0x254), rider, 0)
            m.sb(rider + 0x48, 3)
    f228 = m.lw(r + 0x228) & 0xEFFFFFFF
    m.sw(r + 0x228, f228)

    def per():
        t = m.lw(PERSTATE)
        return m.lw(t + 4 * e16) if t else 0
    clip = (m.lw(rn) >> 4) & 0xFFF
    if s8 != 0:
        if s8 == 2:
            m.sw(r + 0x228, f228 | 0x10000000)
            s2 = 0
        else:
            s2 = 1
            if m.call(0x8005BEF4, anim) == 0:
                if red == 224:
                    return 0
                n = m.lhu(rider + 0x260)
                m.sh(rider + 0x260, n + 1)
                m.sh(rider + (n << 1) + 610, ev)          # push onto the queue, reject
                return 0
        st = (1 if b0 == 0 else 0, (p >> 7) & 1, b16, per())
        for k, v in enumerate(st):
            m.sw(m.sp + 16 + 4 * k, v)
        m.call(0x8005C140, anim, clip, s2, s7 & 0xFF, stack=st)
    elif b0 == 1:
        st = (per(),)
        m.sw(m.sp + 16, st[0])
        m.call(0x8005C0B0, anim, clip, s7 & 0xFF, b16, stack=st)
    elif p & 0x8000:
        st = (11, b16, per())
        for k, v in enumerate(st):
            m.sw(m.sp + 16 + 4 * k, v)
        m.call(0x8005C018, anim, clip, s7 & 0xFF, 6, stack=st)
    else:
        st = (per(),)
        m.sw(m.sp + 16, st[0])
        m.call(0x8005BF6C, anim, clip, s7 & 0xFF, b16, stack=st)
    pp = m.lw(m.sp + 96)
    m.sw(pp, p ^ (1 if MUT['on'] else 0))       # MUTATION: flips bit 0 of the written-back p
    if red != 224:
        m.sw(pp, p | 0x20)
    return 1
