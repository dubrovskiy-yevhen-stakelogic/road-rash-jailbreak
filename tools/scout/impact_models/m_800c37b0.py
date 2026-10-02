# StancePath(rider, ev) RASHCDG 0x800C37B0, 416 B, leaf, frame 448 (a u16 buffer at sp+0)
from .h_dcommon import MUT
FRAME = 448
T = 0x800CCBAC     # (u8 fromStance, u8 offset into U) pairs, ascending
U = 0x800CCBBC     # at U[off]: u8 n, u8 path[n] ...; the target key is searched from U[off + 2]


def model(m, a0, a1, a2, a3):
    r = a0
    cur = m.lhu(r + 0x220)
    i = 0
    if m.lbu(T) < cur:
        i = 1
        while m.lbu(T + 2 * i) < cur:
            i += 1
    if m.lbu(T + 2 * i) != m.lhu(r + 0x220):
        return 224
    t0 = m.lbu(T + 2 * i + 1)
    e = a1 & 0xFFFF
    j = t0 + 2
    if m.lbu(U + j) < e:
        j = t0 + 3
        while m.lbu(U + j) < e:
            j += 1
    if m.lbu(U + j) != e:
        return 224
    buf = {0: a1 & 0xFFFF}
    if m.lbu(U + t0) != 0:
        k = 0
        while True:
            k += 1
            buf[k] = m.lbu(U + t0 + k)
            if not (k < m.lbu(U + t0)):
                break
    n = m.lbu(U + t0)
    buf[n + 1] = m.lhu(r + 0x220)
    n = m.lbu(U + t0)
    for k in range(n):
        m.sh(r + 610 + 2 * k, buf[k])
    m.sh(r + 608, n if not MUT['on'] else n + 1)
    return buf[n]
