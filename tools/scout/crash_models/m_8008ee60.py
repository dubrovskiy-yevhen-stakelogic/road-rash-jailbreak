"""RASHCDG 0x8008EE60 (sha1 cfe43a77...), 520 B (0x8008EE60..0x8008F068), frame 40 - PoseA (name ours), the
pose initialiser of stance 38 (RiderLaunch case A):
    void PoseA(Rider *R, s32 *out /* unused */)
No passenger test: B = R+0x254 always. Puts the rider at B+0xB8 + 0.5 * B.row1B0 - 0.9219 * B.row1B6
+ c(t) * B.row1BC, where c(t) = FixMul(-1.0938, t) / (frames - 1) runs with the clip phase t (the animation
object R+0x21C: t = (A+16 << 16) + (A+32 << 16) / A+24); copies B's frame +0x1B0..+0x1C0 and heading +0x1C2..
and sets the speed +0x1E0 = max(B+0x1E0, 5.0) (the compare is `5.0 - 1 < v`).
Callers: 0x80091590 (RiderLaunch, a1 = 0), 0x8008F1D8 (0x8008F138, a1 = sp+16)."""
import pairs as P
u32, s32 = P.u32, P.s32
FRAME = 40
NAME = "PoseA"
FIXMUL, COPYH = 0x8001FC90, 0x8003FA18
COVER = set()


def s16(v):
    return ((v & 0xFFFF) ^ 0x8000) - 0x8000


def divs(a, b):
    """div a,b; mflo with the R3000's results for b == 0 and INT_MIN / -1"""
    a, b = s32(a), s32(b)
    if b == 0:
        return u32(-1 if a >= 0 else 1)
    if a == -0x80000000 and b == -1:
        return 0x80000000
    q = abs(a) // abs(b)
    return u32(q if (a < 0) == (b < 0) else -q)


def clip_phase(m, R):
    """the common head of the five pose initialisers: (phase t in 16.16 frames, s16 frames - 1)"""
    A = m.lw(R + 0x21C)
    frac = divs(u32(m.lw(A + 32) << 16), m.lw(A + 24))
    idx = m.lbu(u32(m.lw(A + 4) + 12 * m.lw(A + 12)))
    clip = m.lw(u32(m.lw(m.lw(A + 40) + 4) + 4 * idx))
    nf = s16(m.lhu(clip + 16) - 1)
    return u32((m.lw(A + 16) << 16) + frac), nf


def model(m, R, out, a2, a3):
    COVER.add('ra=%x' % m.regs['ra'])
    t, nf = clip_phase(m, R)
    B = m.lw(R + 0x254)
    v = m.call(FIXMUL, 0xFFFEE7FD, t)                              # -1.0938 * t
    c = divs(v, nf)
    for i, (dst, pc) in enumerate(((0xB8, "0x8008EF50"), (0xBC, "0x8008EFA4"), (0xC0, "0x8008F004"))):
        # MUTATION: the 0.5 coefficient off by one unit (li a0,0x8000: a recorded jal argument)
        x = m.call(FIXMUL, 0x8001 if P.MUTATE["on"] else 0x8000, u32(m.lhs(B + 0x1B0 + 2 * i) << 4))
        y = m.call(FIXMUL, 0xFFFF13FF, u32(m.lhs(B + 0x1B6 + 2 * i) << 4))
        z = m.call(FIXMUL, c, u32(m.lhs(B + 0x1BC + 2 * i) << 4))
        m.sw(R + dst, u32(m.lw(B + dst) + x + y + z))              # pc (the +0xC0 one in CopyHalfwords' delay slot)
    m.call(COPYH, 9, B + 0x1B0, R + 0x1B0)                         # the frame rows +0x1B0/+0x1B6/+0x1BC
    for i, pc in enumerate(("0x8008F010", "0x8008F01C", "0x8008F028")):
        m.sh(R + 0x1C2 + 2 * i, m.lhu(B + 0x1C2 + 2 * i))           # the heading
    sp = m.lws(B + 0x1E0)
    m.sw(R + 0x1E0, u32(sp) if 0x4FFFF < sp else 0x50000)         # 0x8008F044
    COVER.add("slow" if not (0x4FFFF < sp) else "fast")
    return None
