"""RASHCDG 0x8008E50C (sha1 cfe43a77...), 780 B (0x8008E50C..0x8008E818), frame 56 - PoseC (name ours), the
pose initialiser of stances 40 / 88 (RiderLaunch case C):
    void PoseC(Rider *R)          // a1 is never read (RiderLaunch leaves it unset)
B = the bike (R+0x254; passenger: -> +0x358). Coefficients (a, b, c) along three rows from a base point:
  * R+0x228 bit 16 (the launch): rows = B's +0x204/+0x20A/+0x210, base = B+0xB8 when B's flagsC & 0x600, else
    B+0x1F8; a = -0.001 (passenger 0.5635), b = -2.6 constant, c = 0; the rider's frame := B's +0x1B0..+0x1C0
    (CopyHalfwords 9) first;
  * else: rows = the rider's OWN +0x1B0/+0x1B6/+0x1BC, base = R+0xB8 := B+0x1F8 + R+0x244 (written first);
    a as above, b(t) = -0.65 + FixMul(b0 + 0.65, t) / (frames - 1), b0 = -1.86 (passenger -2.06),
    c = 0.0669 (passenger -0.3315).
Then R.1C2 = -R.row1B6 (written twice: the copy, then the negation) and R+0x1E0 = max(B+0x1E0, 5.0).
Callers: 0x80091B78 (RiderLaunch), 0x8008F1C4 (0x8008F138)."""
import pairs as P
u32, s32 = P.u32, P.s32
FRAME = 56
NAME = "PoseC"
FIXMUL, COPYH = 0x8001FC90, 0x8003FA18
COVER = set()


def s16(v):
    return ((v & 0xFFFF) ^ 0x8000) - 0x8000


def divs(a, b):
    a, b = s32(a), s32(b)
    if b == 0:
        return u32(-1 if a >= 0 else 1)
    if a == -0x80000000 and b == -1:
        return 0x80000000
    q = abs(a) // abs(b)
    return u32(q if (a < 0) == (b < 0) else -q)


def clip_phase(m, R):
    A = m.lw(R + 0x21C)
    frac = divs(u32(m.lw(A + 32) << 16), m.lw(A + 24))
    idx = m.lbu(u32(m.lw(A + 4) + 12 * m.lw(A + 12)))
    clip = m.lw(u32(m.lw(m.lw(A + 40) + 4) + 4 * idx))
    nf = s16(m.lhu(clip + 16) - 1)
    return u32((m.lw(A + 16) << 16) + frac), nf


def model(m, R, a1, a2, a3):
    COVER.add('ra=%x' % m.regs['ra'])
    t, nf = clip_phase(m, R)
    if m.lbu(R + 0x23C) & 0x20:
        COVER.add("passenger")
        s8, s5, s7, s0 = 0x9041, 0xFFFDF08A, u32(-21725), 0xFFFF599A
        B = m.lw(m.lw(R + 0x254) + 0x358)
    else:
        B = m.lw(R + 0x254)
        s8, s5, s7, s0 = u32(-65), 0xFFFE23CA, 4384, 0xFFFF599A
    if m.lw(R + 0x228) & 0x10000:
        COVER.add("bit16")
        s7, s5 = 0, 0xFFFD6667
        s2 = B + 0x204
        s6 = (B + 0xB8) if (m.lw(B + 0x238) & 0x600) else (B + 0x1F8)
        COVER.add("base-b8" if (m.lw(B + 0x238) & 0x600) else "base-1f8")
        m.call(COPYH, 9, B + 0x1B0, R + 0x1B0)
    else:
        COVER.add("no16")
        m.sw(R + 0xB8, u32(m.lw(B + 0x1F8) + m.lw(R + 0x244)))     # 0x8008E660
        m.sw(R + 0xBC, u32(m.lw(B + 0x1FC) + m.lw(R + 0x248)))     # 0x8008E674
        m.sw(R + 0xC0, u32(m.lw(B + 0x200) + m.lw(R + 0x24C)))     # 0x8008E68C (the jal's delay slot)
        s2, s6 = R + 0x1B0, R + 0xB8
        v = m.call(FIXMUL, u32(s5 - s0), t)
        s5 = u32(s0 + divs(v, nf))
    zs = []
    for i, dst in enumerate((0xB8, 0xBC, 0xC0)):
        x = m.call(FIXMUL, s8, u32(m.lhs(s2 + 2 * i) << 4))
        y = m.call(FIXMUL, s5, u32(m.lhs(s2 + 6 + 2 * i) << 4))
        z = m.call(FIXMUL, s7, u32(m.lhs(s2 + 12 + 2 * i) << 4))
        v = u32(m.lw(s6 + 4 * i) + x + y + z)
        if i < 2:
            m.sw(R + dst, v)                                       # 0x8008E6EC / 0x8008E73C
        else:
            zs = v
    h = [m.lhu(R + 0x1B6 + 2 * i) for i in range(3)]
    m.sh(R + 0x1C2, h[0])                                          # 0x8008E79C
    m.sh(R + 0x1C4, h[1])                                          # 0x8008E7A4
    m.sw(R + 0xC0, zs)                                             # 0x8008E7AC
    m.sh(R + 0x1C6, h[2])                                          # 0x8008E7B0
    m.sh(R + 0x1C2, u32(-h[0]))                                    # 0x8008E7C0
    m.sh(R + 0x1C4, u32(-h[1]))                                    # 0x8008E7C4
    m.sh(R + 0x1C6, u32(-h[2] + (1 if P.MUTATE["on"] else 0)))     # 0x8008E7C8  MUTATION: off by one unit
    sp = m.lws(B + 0x1E0)
    m.sw(R + 0x1E0, u32(sp) if 0x4FFFF < sp else 0x50000)         # 0x8008E7E4
    return None
