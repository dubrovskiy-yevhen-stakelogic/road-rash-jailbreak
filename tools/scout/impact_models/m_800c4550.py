# StanceEvent(ev, r, p) RASHCDG 0x800C4550, 136 B, frame 40
from .h_dcommon import MUT
FRAME = 40


def model(m, a0, a1, a2, a3):
    ev = a0 & 0xFFFF                                   # s0
    m.sw(m.sp + 16, a2)                                # p parked in the own frame; its address is the gate's a2
    v = m.call(0x800C3E9C, ev, a1, m.sp + 16)          # the gate (may rewrite [sp+16])
    if v == (2 if MUT['on'] else 1):
        m.call(0x800C4500, ev, a1, m.lw(m.sp + 16))    # leave the old stance
        m.call(0x800C4454, ev, a1, m.lw(m.sp + 16))    # enter the new one
        m.call(0x800C2FF4, ev, a1, m.lw(m.sp + 16))    # SetRiderState
    return v
