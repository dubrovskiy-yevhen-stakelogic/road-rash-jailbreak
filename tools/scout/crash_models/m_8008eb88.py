"""RASHCDG 0x8008EB88 (sha1 cfe43a77...), 728 B (0x8008EB88..0x8008EE60), frame 56 - PoseD (name ours), the
pose initialiser of stances 41 / 90 (RiderLaunch case D) and, through 0x8008F138's jump table, of every stance
43..87 and every stance outside 38..91:
    void PoseD(Rider *R, s32 *out)
B = the bike (R+0x254; passenger: -> +0x358). With the clip phase t and n = frames - 1:
  a = 0.0186 (passenger 0.5606)
  b(t) = b1 + FixMul(b0 - b1, t) / n   (rider -0.7952 -> -0.9952; passenger -1.1057 -> -1.3291)
  c(t) = FixMul(c0, t) / n             (rider c0 = -1.0333; passenger -1.6583)
R+0xB8.. = B+0xB8 + a * B.row1B0 + b * B.row1B6 + c * B.row1BC; the frame := B's (CopyHalfwords 9); the
heading R.1C2 := R.row1BC; speed: B+0x1E0 > 5.0 ? B+0x1E0 - 5.0 : 5.0 - B+0x1E0 with the heading negated
(stores 1C2, 1C6, 1C4); with `out`: *out = R+0x138, R+0x138 /= 2.
Callers: 0x80091F60 (RiderLaunch, a1 = 0), 0x8008F200 (0x8008F138, a1 = sp+16)."""
import pairs as P
u32, s32 = P.u32, P.s32
FRAME = 56
NAME = "PoseD"
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


def model(m, R, out, a2, a3):
    COVER.add('ra=%x' % m.regs['ra'])
    t, nf = clip_phase(m, R)
    if m.lbu(R + 0x23C) & 0x20:
        COVER.add("passenger")
        s6, s4, s5, s0 = 0x8F83, 0xFFFEB2BE, 0xFFFE577A, 0xFFFEE5F1
        B = m.lw(m.lw(R + 0x254) + 0x358)
    else:
        B = m.lw(R + 0x254)
        s6, s4, s5, s0 = 1218, 0xFFFF013B, 0xFFFEF77A, 0xFFFF346E
    s5 = divs(m.call(FIXMUL, s5, t), nf)
    v = m.call(FIXMUL, u32(s4 - s0), t)
    s4 = u32(s0 + divs(v, nf))
    for i, dst in enumerate((0xB8, 0xBC, 0xC0)):                   # 0x8008ECF8 / D48 / DA4 (CopyHalfwords' slot)
        x = m.call(FIXMUL, s6, u32(m.lhs(B + 0x1B0 + 2 * i) << 4))
        y = m.call(FIXMUL, s4, u32(m.lhs(B + 0x1B6 + 2 * i) << 4))
        z = m.call(FIXMUL, s5, u32(m.lhs(B + 0x1BC + 2 * i) << 4))
        m.sw(R + dst, u32(m.lw(B + dst) + x + y + z))
    m.call(COPYH, 9, B + 0x1B0, R + 0x1B0)
    h = [m.lhu(R + 0x1BC + 2 * i) for i in range(3)]
    for i in range(3):
        m.sh(R + 0x1C2 + 2 * i, h[i])                              # 0x8008EDB4 / B8 / BC
    sp = m.lws(B + 0x1E0)
    if 0x50000 < sp:
        COVER.add("fast")
        v = u32(sp - 0x50000)
    else:
        COVER.add("slow")
        m.sh(R + 0x1C2, u32(-m.lhu(R + 0x1C2)))                    # 0x8008EDE0
        m.sh(R + 0x1C6, u32(-m.lhu(R + 0x1C6)))                    # 0x8008EDEC
        m.sh(R + 0x1C4, u32(-m.lhu(R + 0x1C4)))                    # 0x8008EDF4
        v = u32(0x50000 - m.lw(B + 0x1E0))
    m.sw(R + 0x1E0, u32(v + (1 if P.MUTATE["on"] else 0)))         # 0x8008EE08  MUTATION: off by one unit
    if out:
        COVER.add("out")
        m.sw(out, m.lw(R + 0x138))                                 # 0x8008EE14
        v = m.lw(R + 0x138)
        m.sw(R + 0x138, u32(s32(u32(v + (v >> 31))) >> 1))        # 0x8008EE2C: / 2
    return None
