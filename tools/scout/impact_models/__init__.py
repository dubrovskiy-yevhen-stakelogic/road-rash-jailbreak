r"""The executable transcriptions of the impact outcome (RASHCDG cfe43a77..., SLUS 67ed165a...), one module per
guest function, each `model(m, a0, a1, a2, a3) -> v0` against the pairs.Machine interface and `FRAME`, the
function's own stack frame. Run by tools\scout\impact.py. Every model's mutation control is switched by
impact.py --mutate."""
import importlib

# (entry, name, section tag)
FUNCS = [
    # 23 - the impact outcome
    (0x800AF224, "ImpactGate", "23.2"), (0x800AF0A0, "PoleReact", "23.2"), (0x800B11B4, "BoxReact", "23.2"),
    (0x800AF3B0, "ImpactSolve", "23.3"),
    (0x80083928, "HitOutcome", "23.4"), (0x80083F30, "ImpactTurn", "23.4"), (0x80083864, "MassExchange", "23.4"),
    (0x80080D1C, "HitSpeed", "23.5"), (0x80080B10, "TakePartnerHeading", "23.5"),
    (0x800B0510, "PropHitFace", "23.6"), (0x800B5EB4, "FaceCrossing", "23.6"), (0x800B3838, "PropKnock", "23.6"),
    (0x800B16F4, "TouchDown", "23.6"), (0x800AD9BC, "LandOnTop", "23.6"), (0x800B2F94, "PropKick", "23.6"),
    # 24 - the rider stance layer
    (0x800C4550, "StanceEvent", "24.1"), (0x800C3E9C, "StanceGate", "24.2"), (0x800C37B0, "StanceGateLeaf", "24.2"),
    (0x800C4500, "StanceLeave", "24.3"), (0x800C4454, "StanceEnter", "24.3"), (0x800C2FF4, "SetRiderState", "24.3"),
    (0x800BFD24, "FightStanceLeave", "24.3"), (0x800BFC5C, "FightStanceEnter", "24.3"),
    (0x800C29F0, "RiderShoved", "24.4"), (0x800BFE58, "RiderEventByte", "24.4"),
    (0x80090D84, "RiderKnockOff", "24.5"), (0x80091468, "RiderLaunch", "24.6"),
    # 25 - combat
    (0x800C2178, "CombatEdge", "25.1"), (0x800C2348, "CombatDecode", "25.1"), (0x800C1DD4, "FightPush", "25.1"),
    (0x800C035C, "FightUpdate", "25.2"), (0x800C09B0, "FightStep", "25.2"), (0x800C159C, "ReachTest", "25.3"),
]


def load():
    """[(entry, frame, model, name)] for every module present"""
    out = []
    for f, name, _ in FUNCS:
        try:
            mod = importlib.import_module(f"{__name__}.m_{f:08x}")
        except ModuleNotFoundError:
            continue
        out.append((f, mod.FRAME, mod.model, name))
    return out
