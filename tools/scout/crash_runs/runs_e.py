# Reading pass E runs (combat: Pick 0x8008B428, CanEngage 0x800BC1EC, ApplyHit 0x800C17B0 and its leaves).
# impact.RUNS form: (name, src, build, frames, pad, explore, note). Every edit that is not a placement is named NOT.
import impact as I
import pairs as P

_R = {r[0]: r for r in I.RUNS}
RR, QUICK = I.RR, I.QUICK
PLAYER, BIKE = P.PLAYER, I.BIKE
COP = BIKE(16)
_rd, FX, orw, row = I._rd, I.FX, I.orw, I.row


def cop_beside2(forward, across=6.2):
    """impact.cop_beside with the player's lateral move `across` as a parameter (impact uses 6.2): the cop 16
    released (+0x3A0 |= 0x10, NOT a placement), its road state +0x144..+0x177 and +0x1EC copied from the player's
    (a placement in road coordinates), the player's box put `across` along -(his +0x1B0 row), `forward` along +0x210"""
    def f(ram):
        ed = [(COP + o, 4, _rd(ram, PLAYER + o, "<I")) for o in range(0x144, 0x178, 4)]
        ed += [(COP + 0x1EC, 4, _rd(ram, PLAYER + 0x1EC, "<I"))] + orw(ram, COP + 0x3A0, 0x10)
        lat, fwd = row(ram, PLAYER, 0x1B0), row(ram, PLAYER, 0x210)
        tx = _rd(ram, PLAYER + 0xB8) + int(round((-across * lat[0] + forward * fwd[0]) * 65536))
        tz = _rd(ram, PLAYER + 0xC0) + int(round((-across * lat[2] + forward * fwd[2]) * 65536))
        return ed + I.conv(P.move_box(ram, PLAYER, tx, tz))
    return f


def rdef(ram, e):
    return _rd(ram, e + 0x43C, "<I")


def rider(ram, e):
    return _rd(ram, e + 0x354, "<I")


def gs(ram):
    return _rd(ram, P.GS, "<I")


def plus(base, *extra):
    """base edits + extra builders"""
    return lambda ram: base(ram) + [x for e in extra for x in e(ram)]


cop = I.cop_beside(-0.3)
knock = I.cop_beside(-0.3, riderdef_hp=10)

RUNS = [_R[n] for n in ("punch-miss", "punch-cop", "knock-cop", "swing-cop", "punch-last", "ai-punch")] + [
    # --- Pick among many candidates
    ("quick-punch", QUICK, None, 14, "0xF7FF", False, "quick, R1 held, no edits: the picker among the pack's live bikes"),
    # --- ApplyHit / its leaves: planted states (all NOT placements beyond impact.cop_beside's own)
    ("punch-cop-x3", RR, plus(cop, lambda r: orw(r, rider(r, PLAYER) + 0x23C, 0x80, 1)), 24, "0xF7FF", False,
     "punch-cop with the player's rider +0x23C |= 0x80 (NOT a placement: FORCED)"),
    ("punch-cop-hp120", RR, plus(cop, lambda r: [(rdef(r, PLAYER) + 0x0F, 1, 120)]), 24, "0xF7FF", False,
     "punch-cop with the player's riderDef+0x0F := 120 (NOT a placement)"),
    ("punch-cop-bar", RR, plus(cop, lambda r: [(rdef(r, COP) + 0x0E, 1, 1)]), 24, "0xF7FF", False,
     "punch-cop with the cop's riderDef+0x0E := 1 (NOT a placement)"),
    ("knock-cop-regen", RR, plus(knock, lambda r: [(rdef(r, COP) + 0x24, 1, 0xF0), (rdef(r, COP) + 0x25, 1, 3)]),
     30, "0xF7FF", False, "knock-cop with the cop's riderDef+0x24 := 0xF0, +0x25 := 3 (NOT placements)"),
    ("knock-cop-p358", RR, plus(knock, lambda r: [(COP + 0x358, 4, BIKE(17))]), 30, "0xF7FF", False,
     "knock-cop with the cop's +0x358 := bike 17 (NOT a placement: a PLANTED passenger pointer; the guest later "
     "stops on an address error at 0x800B823C, after the knock-off)"),
    ("punch-cop-psg", RR, plus(cop, lambda r: orw(r, rider(r, COP) + 0x23C, 0x20, 1) + [(COP + 0x358, 4, BIKE(17))]),
     24, "0xF7FF", False, "punch-cop with the cop's rider +0x23C |= 0x20 (FORCED) and +0x358 := bike 17 (PLANTED): "
     "NOT placements; the guest stops inside StanceEvent's animation callees, ApplyHit is cut off"),
    ("ai-knock", QUICK, plus(I.ai_fight, lambda r: [(rdef(r, PLAYER) + 0x0F, 1, 3)]), 14, "0xBFFF", False,
     "ai-punch with the player's riderDef+0x0F := 3 (NOT a placement): the AI knocks the player off"),
] + [
    (f"punch-cop-a{int(a * 10)}", RR, cop_beside2(-0.3, a), 24, "0xF7FF", False,
     f"punch-cop with the player put {a} (not 6.2) across (placement)")
    for a in (5.6, 7.4)
]
