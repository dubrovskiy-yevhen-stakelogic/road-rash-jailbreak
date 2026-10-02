# KnockOff 0x800BF674(bike victim, u8 cmd, s32 side, bike attacker) - RASHCDG cfe43a77, 492 B, frame 32;
# one callee Arrest 0x80096F30 (opaque).
import pairs as P, impact as I
u32, s32, iabs = P.u32, P.s32, P.iabs
FRAME = 32
NAME = "KnockOff"
GS, FLASH, ARREST = 0x8005B2F8, 0x800D6198, 0x80096F30


def model(m, a0, a1, a2, a3):
    v, c, side, att = a0, a1, a2, a3
    if m.lws(v + 364) < 0:                                    # +0x16C < 0
        m.sw(v + 568, m.lw(v + 568) | 0x00400000)             # 0x800BF6AC flagsC bit 22
    else:
        m.sw(v + 568, m.lw(v + 568) & 0xFFBFFFFF)             # 0x800BF6AC
    r = m.lw(v + 852)
    m.sw(r + 552, m.lw(r + 552) & 0xFFF9FFFF)                 # 0x800BF6C4 rider+0x228 &= ~0x60000
    cc = c & 0xFF
    special = (u32(c - 36) & 0xFF) < 2 or cc in (75, 76, 146, 147)
    ns = u32(-side)
    if m.lw(v + 856) != 0 or (m.lw(v + 568) & 0x400):         # a passenger aboard, or flagsC 0x400
        a2_ = 0x8000 if special else (u32((ns & 0xFFFE0000) + 0x40000) | 0x8000)
        r = m.lw(v + 852)
        m.sw(r + 552, m.lw(r + 552) | a2_)                    # 0x800BF750 the knock-off request
        m.sw(v + 720, 0xFFFF0000)                             # 0x800BF75C +0x2D0 = -1.0
    else:
        if special:
            a2_ = 0x880
        else:
            a2_ = u32((ns & 0x8000) + 0x8000) | 0x840
            r = m.lw(v + 852)
            m.sw(r + 552, m.lw(r + 552) | 0x8000 | u32((ns & 0xFFFE0000) + 0x40000))   # 0x800BF7CC
        m.sw(v + 568, (m.lw(v + 568) & 0xFFFF9FC0) | a2_)     # 0x800BF7E0 flagsC class / kind bits
    gs = m.lw(GS)
    h = m.lhu(att + 172)
    if h < m.lw(gs + 48):                                     # a player attacker
        i = h + min(1 - h, 0)                                 # min(h, 1)
        m.sw(FLASH + 224 * i + 140, 0 if P.MUTATE["on"] else 1)   # 0x800BF82C the dash flash (MUTATION: 0)
        if m.lbu(gs + 4) & 1:                                 # a police race
            m.call(ARREST, att, v, 9)
    return None
