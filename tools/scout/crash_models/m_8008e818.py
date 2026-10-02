"""RASHCDG 0x8008E818 (sha1 cfe43a77...), 880 B (0x8008E818..0x8008EB88), frame 56 - PoseB (name ours), the
pose initialiser of stances 39 / 89 (RiderLaunch case B):
    void PoseB(Rider *R, s32 *out)
B = the bike (R+0x254; passenger: -> +0x358). With the clip phase t and n = frames - 1:
  a(t) = a1 + FixMul(a0 - a1, t) / n   (rider: 0 -> -0.1739; passenger 0.5575 -> 0.3575), negated by R+0x228 bit 27
  b(t) = b1 + FixMul(b0 - b1, t) / n   (rider: -0.75 -> -1.0850; passenger -1.0 -> -1.3868)
  c    = -0.2813 (passenger -0.7842)
R+0xB8.. = B+0xB8 + a * B.row1B0 + b * B.row1B6 + c * B.row1BC; the rider's frame = B's with rows 1B6 and 1BC
swapped (R.1BC = B.1B6, R.1B0 = B.1B0, R.1B6 = B.1BC); the heading R.1C2 = -R.row1B6 (each component stored
twice, then negated); with `out`: *out = R+0x138, R+0x138 /= 2; R+0x1E0 = max(B+0x1E0, 5.0).
Callers: 0x800918F0 (RiderLaunch, a1 = 0), 0x8008F1B4 (0x8008F138, a1 = sp+16)."""
import pairs as P
u32, s32 = P.u32, P.s32
FRAME = 56
NAME = "PoseB"
FIXMUL = 0x8001FC90
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


def model(m, R, out, a2, a3):
    COVER.add('ra=%x' % m.regs['ra'])
    m.sw(m.entry_sp + 4, out)                                      # 0x8008E848 home slot of a1
    t, nf = clip_phase(m, R)
    if m.lbu(R + 0x23C) & 0x20:
        COVER.add("passenger")
        s4, s6, s7, s0, s1 = 23429, 0xFFFE9CFB, 0xFFFF373F, 0x8EB8, 0xFFFF0000
        B = m.lw(m.lw(R + 0x254) + 0x358)
    else:
        B = m.lw(R + 0x254)
        s4, s6, s7, s0, s1 = u32(-11396), 0xFFFEEA3E, u32(-18435), 0, 0xFFFF4000
    v = m.call(FIXMUL, u32(s4 - s0), t)
    s4 = u32(s0 + divs(v, nf))
    v = m.call(FIXMUL, u32(s6 - s1), t)
    s6 = u32(s1 + divs(v, nf))
    if m.lw(R + 0x228) & 0x08000000:
        COVER.add("bit27")
        s4 = u32(-s4)
    for i, dst in enumerate((0xB8, 0xBC, 0xC0)):                   # 0x8008E9A8 / 9F8 / EA48
        x = m.call(FIXMUL, s4, u32(m.lhs(B + 0x1B0 + 2 * i) << 4))
        y = m.call(FIXMUL, s6, u32(m.lhs(B + 0x1B6 + 2 * i) << 4))
        z = m.call(FIXMUL, s7, u32(m.lhs(B + 0x1BC + 2 * i) << 4))
        m.sw(R + dst, u32(m.lw(B + dst) + x + y + z))
    for d, s in ((0x1BC, 0x1B6), (0x1BE, 0x1B8), (0x1C0, 0x1BA),   # 0x8008EA54..0x8008EA90
                 (0x1B0, 0x1B0), (0x1B2, 0x1B2), (0x1B4, 0x1B4)):
        m.sh(R + d, m.lhu(B + s))
    m.sh(R + 0x1B6, m.lhu(B + 0x1BC))                              # 0x8008EA9C
    m.sh(R + 0x1B8, m.lhu(B + 0x1BE))                              # 0x8008EAA8
    v1 = m.lhu(R + 0x1B6)
    a0 = m.lhu(B + 0x1C0)
    m.sh(R + 0x1C2, v1)                                            # 0x8008EAB4
    m.sh(R + 0x1C2, v1)                                            # 0x8008EAB8
    m.sh(R + 0x1BA, a0)                                            # 0x8008EAC0
    a0 = m.lhu(R + 0x1B8)
    a2 = m.lhu(R + 0x1BA)
    m.sh(R + 0x1C4, a0)                                            # 0x8008EAD4
    m.sh(R + 0x1C4, a0)                                            # 0x8008EAD8
    m.sh(R + 0x1C6, a2)                                            # 0x8008EADC
    m.sh(R + 0x1C6, a2)                                            # 0x8008EAE0
    m.sh(R + 0x1C2, u32(-v1))                                      # 0x8008EAE4
    m.sh(R + 0x1C4, u32(-a0))                                      # 0x8008EAF8
    m.sh(R + 0x1C6, u32(-a2))                                      # 0x8008EAFC
    if out:
        COVER.add("out")
        m.sw(out, m.lw(R + 0x138))                                 # 0x8008EB18
        v = m.lw(R + 0x138)
        m.sw(R + 0x138, u32(s32(u32(v + (v >> 31))) >> 1))        # 0x8008EB30: / 2
    sp = m.lws(B + 0x1E0)
    lim = 0x4FFFF if not P.MUTATE["on"] else 0x7FFFFFFF           # MUTATION: no pass-through, always 5.0
    m.sw(R + 0x1E0, u32(sp) if lim < sp else 0x50000)             # 0x8008EB54
    return None
