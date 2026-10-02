"""WeaponSteal RASHCDG 0x800BFF04(me, t, side, *flags) - cfe43a77, 1112 B, frame 56; one caller 0x800C0938
(FightUpdate, when me's riderDef+0x3C == 32, bare fists). Takes t's weapon inside a frame window of t's clip."""
import pairs as P, impact as I
u32, s32, iabs = P.u32, P.s32, P.iabs
FRAME = 56
NAME = "WeaponSteal"
GS, TAB, FIGHT = 0x8005B2F8, 0x800541D4, 0x8005AD4C
GATE, COUNT, PS3D, STANCE = 0x800C2E9C, 0x800BFE58, 0x80017BA0, 0x800C4550
GRAB, FX, SWAPCLIP, QCLOSE = 0x800958F0, 0x800273EC, 0x800C2F84, 0x800C2030
FXTAB = 0x800CF018


def model(m, a0, a1, a2, a3):
    me, t, side, fl = a0, a1, a2, a3
    np_ = m.lw(m.lw(GS) + 0x30)
    s3 = m.lw(me + 0x354)                                        # my rider
    ok = (m.lhu(me + 0xAC) < np_ or m.lhu(t + 0xAC) < np_ or (m.lbu(s3 + 0x23C) & 0x20)
          or (m.lbu(m.lw(t + 0x354) + 0x23C) & 0x20))            # a player or a passenger on either side
    if not ok:
        return None
    tr = m.lw(t + 0x354)
    if m.lhu(u32(8 * m.lhu(tr + 0x220) + TAB) + 2) != 3:         # t's rider in a combat stance
        return None
    trd = m.lw(t + 0x43C)
    if not (m.lbu(trd + 0x3C) & 0x80):                           # t holds an armed command
        return None
    if not m.call(GATE, s3, m.lbu(trd + 0x2E)):                  # 0x800BFFEC  my record 0 node 1, frame window
        return None
    an = m.lw(m.lw(t + 0x354) + 0x21C)                           # t's rider's animation object
    f0 = m.lw(fl)
    ch = u32(m.lw(an + 4) + 12 * m.lw(an + 0x0C))
    clip = m.lw(u32(4 * m.lbu(ch) + m.lw(m.lw(an + 0x28) + 4)))
    fr = m.lws(an + 0x10)                                        # the frame counter
    n = s32(u32(m.lhu(clip + 0x10) - 1) << 16) >> 16             # (s16)(clip length - 1)
    if 3 * n < s32(u32(fr << 2)):
        m.sw(fl, f0 | 0x40)                                      # 0x800C0068  too late: blocked
    v = m.lw(fl)
    if v & 0x40:
        m.call(PS3D, m.lw(me + 0xB8), m.lw(me + 0xC0), 88, 0)    # 0x800C0330  the block sound
        return None
    if not (n < s32(u32(fr << 1))):                              # before half the clip: nothing yet
        return None
    m.sw(fl, v | 0x80)                                           # 0x800C008C  the steal
    m.sb(m.lw(me + 0x43C) + 0x2E, m.lbu(m.lw(t + 0x43C) + 0x2E))  # 0x800C00A0  my weapon := t's
    w = m.lbu(m.lw(t + 0x43C) + 0x2E)
    d = m.lw(me + 0x43C)
    m.sh(d + 0x2C, m.lhu(d + 0x2C) | ((1 << (w & 31)) & 0xFFFF))   # 0x800C00BC  my possession mask
    d = m.lw(t + 0x43C)
    w = m.lbu(d + 0x2E)
    m.sh(d + 0x2C, m.lhu(d + 0x2C) & ~(1 << (w & 31)) & 0xFFFF)   # 0x800C00E0  t loses it
    m.sb(m.lw(t + 0x43C) + 0x2E, 9)                              # 0x800C00EC  t: fists
    m.sb(m.lw(t + 0x43C) + 0x46, m.lbu(me + 0xAC))               # 0x800C00FC  NEW: who took my weapon
    m.sb(m.lw(me + 0x43C) + 0x47, m.lbu(t + 0xAC))               # 0x800C0110  NEW: whose weapon I took
    m.call(COUNT, me, 0, 0)                                      # 0x800C010C
    m.call(COUNT, t, 0, 1)                                       # 0x800C011C
    tr = m.lw(t + 0x354)
    s0 = m.lbu(tr + 0x23C) >> 7                                  # t's rider +0x23C bit 7
    nd = m.lbs(u32(tr + m.lbu(tr + 0x23A)) + 0x23E)
    rec = u32(12 * m.lbu(tr + 0x239) + m.lw(FIGHT))
    s5 = m.lhu(u32(12 * nd + m.lw(rec + 8)))                     # t's current node animStart
    snd = 96 if (m.lbu(m.lw(me + 0x43C) + 1) & 0xF) == 0 else 97  # class nibble 0 -> 96
    m.call(PS3D, m.lw(me + 0xB8), m.lw(me + 0xC0), snd, 0)       # 0x800C0194
    m.sb(m.lw(me + 0x43C) + 0x3C, 142)                           # 0x800C01B4  my command: armed
    m.sb(m.lw(t + 0x43C) + 0x3C, 32 if not P.MUTATE["on"] else 33)   # 0x800C01C0  t's: bare fists
    m.call(STANCE, 16, m.lw(t + 0x354), ((side & 0xFFFFFFF0) ^ 0x100) | 16)   # 0x800C01C8  t's reaction
    m.call(GRAB, s3, side & 0x100)                               # 0x800C01D4
    if s0:
        tr = m.lw(t + 0x354)
        m.sb(tr + 0x23C, m.lbu(tr + 0x23C) & 0x7F)               # 0x800C01F8
        a1_ = m.lw(me + 0x43C)
        k = m.lbu(m.lw(t + 0x43C) + 0x2F)                        # t's swings left
        if m.lbu(a1_ + 0x2E) < 8:
            k = ((m.lw(a1_ + 0x30) >> ((4 * m.lbu(a1_ + 0x2E)) & 31)) & 0xF) + k
        d = m.lw(me + 0x43C)
        sh = (4 * m.lbu(d + 0x2E)) & 31
        m.sw(d + 0x30, m.lw(d + 0x30) & u32(~(15 << sh)))        # 0x800C0250
        d = m.lw(me + 0x43C)
        sh = (4 * m.lbu(d + 0x2E)) & 31
        m.sw(d + 0x30, m.lw(d + 0x30) | u32((k & 15) << sh))     # 0x800C0270
        m.sb(m.lw(me + 0x43C) + 0x2F, k)                         # 0x800C027C  my swings
        sh = (4 * m.lbu(m.lw(me + 0x43C) + 0x2E)) & 31
        d = m.lw(t + 0x43C)
        m.sw(d + 0x30, m.lw(d + 0x30) & u32(~(15 << sh)))        # 0x800C02A4  t's counter for it
        m.sw(m.sp + 16, 0)
        m.call(FX, u32(FXTAB + 172 * m.lbs(s3 + 0x23B)), 0, 1500, 6, stack=(0,))   # 0x800C02D8
        m.sb(s3 + 0x23C, m.lbu(s3 + 0x23C) | 0x80)               # 0x800C02EC
    m.sb(m.lw(t + 0x43C) + 0x2F, 0)                              # 0x800C0310  t: no swings
    m.call(SWAPCLIP, s5, s3, (side & 0xFF00) >> 8, (side >> 16) & 0xFF)   # 0x800C030C
    m.call(QCLOSE, s3)                                           # 0x800C0314
    return None
