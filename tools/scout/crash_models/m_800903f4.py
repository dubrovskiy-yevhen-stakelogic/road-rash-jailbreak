"""Model of RASHCDG 0x800903F4 (sha1 cfe43a77...), 1056 B, frame 48:
    void Remount(Bike *B, s32 fromRoad)
(name ours). Seats the bike's rider (and, on a two-rider bike, the passenger
rider of the partner bike +0x358) back on the bike: with fromRoad and a live bike the bike is first put back on
its road slice (MulAdd of the slice origin and forward row by +0x15C into +0x1F8, copied to the box centre, the
heading from the slice, reversed with +0x16C = -2 when flagsC bit 22, else +0x16C = 2, 0x8007EC30), its lateral
+0x158 zeroed; then flagsC |= 0x08000000 (re-file), the rider's +0x228 &= 0xBFE67FC7, its live word := the
bike's, 0x80012838(B, R, 2, 0) + RiderDismount(R, 1) (rider +0x25C := 0 inside), the AI stack cleared and
{4 = race (2 when finished / placed >= 248), 224} pushed (mode 1); a live bike gets 0x80096564 and its rows
+0x1C2 / +0x32E from +0x210 / +0x204; a player's view record +0x224/+0x228 is reset and the "rider on" sound
mode set (SLUS 0x80018440(h, 0)) and the camera target (SLUS 0x800235B0)."""
import pairs as P
u32, s32, iabs = P.u32, P.s32, P.iabs
FRAME = 48
NAME = "Remount"

GS, VIEW, VIEWSZ = 0x8005B2F8, 0x800CD898, 1132
MULADD, EC30, F090C, F5AEC, ATTACH = 0x8002EAD8, 0x8007EC30, 0x8002090C, 0x80095AEC, 0x80012838
DISMOUNT, AICLR, AIPUSH, F6564, F6AF8, SNDMODE, CAMTGT = (0x800C3104, 0x800BCD10, 0x800BCA68, 0x80096564,
                                                          0x80086AF8, 0x80018440, 0x800235B0)
MASK228 = 0xBFE67FC7
COVER = set()
CALLS = []                 # per call: [a0, a1, arm labels...]; dev.py prints them per run with --arms
_cur = [None]


def arm(x):
    COVER.add(x)
    _cur[0].append(x)


def model(m, B, fromRoad, a2, a3):
    _cur[0] = [hex(B), fromRoad, "ra=%x" % m.regs["ra"]]
    CALLS.append(_cur[0])
    if fromRoad != 0:
        if m.lhs(B + 0x140) != 0:
            arm("road")
            sl = m.lw(B + 0x154)
            m.call(MULADD, sl + 20, sl + 14, m.lw(B + 0x15C), B + 0x1F8)
            x, y, z = m.lw(B + 0x1F8), m.lw(B + 0x1FC), m.lw(B + 0x200)
            sl = m.lw(B + 0x154)
            m.sw(B + 0xB8, x)                                  # 0x80090448
            m.sw(B + 0xBC, y)                                  # 0x8009044C
            m.sw(B + 0xC0, z)                                  # 0x80090450
            m.sh(B + 0x1C2, m.lhu(sl + 14))                    # 0x8009045C
            m.sh(B + 0x1C4, m.lhu(sl + 16))                    # 0x80090468
            m.sh(B + 0x1C6, m.lhu(sl + 18))                    # 0x80090474
            if m.lw(B + 0x238) & 0x400000:
                arm("reversed")
                h0 = m.lhu(B + 0x1C2)
                m.sw(B + 0x16C, 0xFFFFFFFE)                    # 0x80090490
                h2 = m.lhu(B + 0x1C6)
                m.sh(B + 0x1C2, -h0)                           # 0x8009049C
                h1 = m.lhu(B + 0x1C4)
                m.sh(B + 0x1C6, -h2)                           # 0x800904A8
                m.sh(B + 0x1C4, -h1)                           # 0x800904B4 (delay slot)
            else:
                m.sw(B + 0x16C, 2)                             # 0x800904BC
            m.call(EC30, B)
        m.sw(B + 0x158, 0)                                     # 0x800904C8
    m.call(F090C, B)
    m.sw(B + 0x238, m.lw(B + 0x238) | 0x08000000)              # 0x800904E8
    R = m.lw(B + 0x354)
    m.sw(R + 0x228, m.lw(R + 0x228) & MASK228)                 # 0x800904F8
    m.sh(m.lw(B + 0x354) + 0x140, m.lhu(B + 0x140))            # 0x80090508
    R = m.lw(B + 0x354)
    if m.lw(R + 0x22C) != 0:
        arm("22c")
        m.call(F5AEC, R)
        m.sw(m.lw(B + 0x354) + 0x22C, 0)                       # 0x80090534
    m.call(ATTACH, B, m.lw(B + 0x354), 2, 0)
    a0 = m.lw(B + 0x354)
    m.sb(B + 0x48, 0)                                          # 0x80090558 (delay slot)
    m.call(DISMOUNT, a0, 1)
    gs = m.lw(GS)
    if (m.lbu(m.lw(B + 0x354) + 0x23C) & 0x10) and m.lbu(gs + 0x39) != 1:
        arm("passenger")
        Pr = m.lw(m.lw(B + 0x358) + 0x354)
        v1 = m.lhu(B + 0x140)
        v0 = m.lw(Pr + 0x228)
        m.sh(Pr + 0x140, v1)                                   # 0x800905AC
        m.sw(Pr + 0x228, v0 & MASK228)                         # 0x800905B8 (delay slot)
        m.call(ATTACH, B, Pr, 2, 1)
        m.call(DISMOUNT, Pr, 1)
        m.sw(Pr + 0x25C, 1)                                    # 0x800905C8
        m.sw(m.lw(B + 0x358) + 0x2D0, 0)                       # 0x800905D4
    m.call(AICLR, B)
    rd = m.lw(B + 0x43C)
    a3 = 1 if (m.lw(rd + 0x28) != 0 or not (m.lbu(rd + 0x27) < 248)) else 0
    op = u32((u32(-a3) & 0xFFFFFFFE) + 4)                      # 4 = race, 2 when finished / a result code
    m.sh(m.sp + 16, op)
    m.sh(m.sp + 18, 224)
    arm("op%d" % (op & 0xFFFF))
    m.call(AIPUSH, m.sp + 16, 1, B)
    if m.lhs(B + 0x140) != 0:
        arm("live")
        m.call(F6564, B)
        r = [m.lhu(B + o) for o in (0x210, 0x212, 0x214, 0x204, 0x206, 0x208)]
        R = m.lw(B + 0x354)
        for o, v in zip((0x1C2, 0x1C4, 0x1C6, 0x32E, 0x330, 0x332), r):
            m.sh(B + o, v)                                     # 0x80090670 .. 0x80090684
        if (m.lbu(R + 0x23C) & 0x10) and m.lbu(m.lw(GS) + 0x39) != 1:
            arm("passenger-lateral")
            m.sw(m.lw(B + 0x358) + 0x158, m.lw(B + 0x158))     # 0x800906C0
            Q = m.lw(B + 0x358)
            s = u32(m.lw(B + 0x130) + m.lw(Q + 0x130))
            if m.lws(B + 0x16C) < 0:
                v = u32(m.lw(Q + 0x158) - s)
            else:
                v = u32(m.lw(Q + 0x158) + s)
            m.sw(Q + 0x158, v)                                 # 0x80090708
            m.sw(m.lw(B + 0x358) + 0x16C, m.lw(B + 0x16C))     # 0x80090718
    gs = m.lw(GS)
    h = m.lhu(B + 0xAC)
    if h < m.lw(gs + 0x30):                                    # sltu: a player
        arm("player")
        V = u32(VIEW + VIEWSZ * h)
        m.sw(V + 0x224, m.lw(V + 0x224) & 0xF5FFFF7F)          # 0x80090770
        if m.lbu(gs + 4) == 44 and m.lbu(gs + 0x39) == 2:
            arm("race44")
            v = m.lw(V + 0x228) | 0x10
            f = m.lws(V + 0x308)
            m.sw(V + 0x228, v)                                 # 0x800907A8 (delay slot)
            if f < 5:
                m.sw(V + 0x308, 5)                             # 0x800907B0
                m.sw(V + 0x318, 0)                             # 0x800907B4
            m.sb(m.lw(GS) + 0x0A, 2)                           # 0x800907C0: gs+0x0A := gs+0x39 (== 2)
        else:
            m.sw(V + 0x224, (m.lw(V + 0x224) | 6) & 0xFF807FFF)   # 0x800907D4
        m.call(F6AF8, V)
        m.call(SNDMODE, m.lhu(B + 0xAC), 0 + (1 if P.MUTATE["on"] else 0))   # MUTATE: mode 1 ("rider off")
        m.call(CAMTGT, m.lhu(B + 0xAC), B)
    return None
