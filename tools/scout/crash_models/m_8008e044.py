"""RASHCDG 0x8008E044 (sha1 cfe43a77...), 1224 B (0x8008E044..0x8008E50C), frame 80 - PoseE (name ours), the
pose initialiser of stances 42 / 91 (RiderLaunch case E):
    void PoseE(Rider *R, s32 *out)
B = the bike (R+0x254; for a passenger, R+0x254 -> +0x358). s7 = R+0x228 bit 27 (mirrors the pose).
The rider is put at B+0xB8 + c(t) * B.row204 + a * B.row20A + b * B.row1BC, c(t) = +-FixMul(c0, t) / (frames-1)
(c0 = -1.1118 - 1.8 * (1 - cos(|652 * B+0x28C >> 16|)) for a rider - B+0x28C an angle in 16.16 radians, 652 =
4096 / 2pi -; -1.1840 (-2.1840 with bit 27) for a passenger); a = -1.0743 (passenger -0.9127), b = -0.2872
(-0.7305);
its frame is B's with rows permuted (R.1BC = B.20A, R.1B6 = B.204, R.1B0 = B.210), rows 1B6 and 1B0 negated
unless bit 27; its velocity is B+0x1C8 + Scale(+-10.0 (12.0 passenger), B.row204): the speed +0x1E0 =
vec_length3 of it and the heading +0x1C2 = ScaleTo16(1/len, it). With `out`: while the phase t < 0.3,
*out = R+0x138 and R+0x138 /= 4; while t <= 3.3 (0x34CCB), *out = R+0x138 and R+0x138 /= 3.
Callers: 0x800916EC (RiderLaunch, a1 = 0), 0x8008F1EC (0x8008F138, a1 = sp+16)."""
import pairs as P
u32, s32 = P.u32, P.s32
FRAME = 80
NAME = "PoseE"
FIXMUL, SCALE, VLEN, SCALE16 = 0x8001FC90, 0x8002EE50, 0x8002E548, 0x8002EED8
TRIG = 0x8005624C
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


def mulhi(a, b):
    return s32(u32((s32(a) * s32(b)) >> 32))


def clip_phase(m, R):
    A = m.lw(R + 0x21C)
    frac = divs(u32(m.lw(A + 32) << 16), m.lw(A + 24))
    idx = m.lbu(u32(m.lw(A + 4) + 12 * m.lw(A + 12)))
    clip = m.lw(u32(m.lw(m.lw(A + 40) + 4) + 4 * idx))
    nf = s16(m.lhu(clip + 16) - 1)
    return u32((m.lw(A + 16) << 16) + frac), nf


def inv_len(L):
    L = s32(L)
    if L < 0:
        x = s32(u32(-L))
        dv = u32((x >> 1) + (s32(u32(x - 2)) >> 31))
        return u32(-((0x80000000 // dv) if dv else 0xFFFFFFFF))
    dv = u32((L >> 1) + (s32(u32(L - 2)) >> 31))
    return (0x80000000 // dv) if dv else 0xFFFFFFFF


def model(m, R, out, a2, a3):
    COVER.add('ra=%x' % m.regs['ra'])
    m.sw(m.entry_sp + 4, out)                                      # 0x8008E074 home slot of a1
    t, nf = clip_phase(m, R)
    m.sw(m.sp + 32, t)                                             # 0x8008E0F8 own frame: the phase, re-read
    s7 = (m.lw(R + 0x228) >> 27) & 1
    pas = m.lbu(R + 0x23C) & 0x20
    if pas:
        COVER.add("passenger")
        a0 = u32((u32(-s7) << 16) + 0xFFFED0E6)
        s6, s5 = 0xFFFF165A, 0xFFFF44FE
        B = m.lw(m.lw(R + 0x254) + 0x358)
    else:
        B = m.lw(R + 0x254)
        s6, s5 = 0xFFFEECFB, u32(-18821)
        p = s32(u32(652 * m.lw(B + 0x28C)))                        # mult by shifts
        ang = abs(p >> 16)
        c = m.lhs(TRIG + (((ang & 0xFFF) << 2) | 2))
        v = m.call(FIXMUL, u32(0x10000 - (c << 4)), 0x1CCCC)       # 1.8 * (1 - cos)
        a0 = u32(0xFFFEE362 - v)
    s8 = u32(-s7)
    v = m.call(FIXMUL, a0, t)
    q = divs(v, nf)
    s0 = u32(q + (s8 & u32(-q - q)))                               # bit 27 ? -q : q
    COVER.add("bit27" if s7 else "no27")
    for i, dst in enumerate((0xB8, 0xBC, 0xC0)):                   # 0x8008E224 / 274 / 2C4
        x = m.call(FIXMUL, s0, u32(m.lhs(B + 0x204 + 2 * i) << 4))
        y = m.call(FIXMUL, s6, u32(m.lhs(B + 0x20A + 2 * i) << 4))
        z = m.call(FIXMUL, s5, u32(m.lhs(B + 0x1BC + 2 * i) << 4))
        m.sw(R + dst, u32(m.lw(B + dst) + x + y + z))
    for d, s in ((0x1BC, 0x20A), (0x1BE, 0x20C), (0x1C0, 0x20E),   # 0x8008E2D0..0x8008E330
                 (0x1B6, 0x204), (0x1B8, 0x206), (0x1BA, 0x208),
                 (0x1B0, 0x210), (0x1B2, 0x212), (0x1B4, 0x214)):
        m.sh(R + d, m.lhu(B + s))
    if not s7:                                                     # 0x8008E340..0x8008E378
        for d in (0x1B6, 0x1B8, 0x1BA, 0x1B0, 0x1B2, 0x1B4):
            m.sh(R + d, u32(-m.lhu(R + d)))
    if pas:
        a0 = u32((s8 & 0x180000) + 0xFFF40000)                     # +-12.0
    else:
        a0 = u32((s8 & 0x140000) + 0xFFF60000)                     # +-10.0
    sp = m.sp
    m.call(SCALE, a0, B + 0x204, sp + 16)
    for i in range(3):
        m.sw(sp + 16 + 4 * i, u32(m.lw(sp + 16 + 4 * i) + m.lw(B + 0x1C8 + 4 * i)))   # own frame
    L = m.call(VLEN, sp + 16)
    m.sw(R + 0x1E0, u32(L + (1 if P.MUTATE["on"] else 0)))           # 0x8008E3FC  MUTATION: off by one unit
    m.call(SCALE16, inv_len(L), sp + 16, R + 0x1C2)
    if out:
        ts = s32(t)
        if ts < 19660:                                             # phase < 0.3
            COVER.add("out/4")
            m.sw(out, m.lw(R + 0x138))                             # 0x8008E480
            v = m.lws(R + 0x138)
            m.sw(R + 0x138, u32((v + 3 if v < 0 else v) >> 2))     # 0x8008E4D8
        elif not (0x34CCB < ts):
            COVER.add("out/3")
            m.sw(out, m.lw(R + 0x138))                             # 0x8008E4BC
            v = m.lws(R + 0x138)
            m.sw(R + 0x138, u32(mulhi(v, 0x55555556) - (v >> 31)))  # 0x8008E4D8
        else:
            COVER.add("out-late")
    else:
        COVER.add("no-out")
    return None
