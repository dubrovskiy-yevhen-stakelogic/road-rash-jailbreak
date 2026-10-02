r"""The executable transcriptions of the crash chain (RASHCDG cfe43a77..., SLUS 67ed165a...), one module
per guest function, each `model(m, a0, a1, a2, a3) -> v0` against the pairs.Machine interface
and `FRAME`, the function's own stack frame. Run by tools\scout\crash.py with impact.py's strict harness.
Every model's mutation control is switched by crash.py --mutate (pairs.MUTATE)."""
import importlib

# (entry, name, section tag)
FUNCS = [
    # 2 - the impact state machine of the rider pass (the knock-off request, the throw)
    (0x80078DB4, "ImpactStatePass", "2"),
    # 3 - the bike-bike impact outcome
    (0x80081D7C, "BikeBikeGate", "3"),
    # 4 - the crashed bike's bounce and spin
    (0x80084564, "Bounce", "4.1"), (0x800849D8, "Spin", "4.2"),
    # 5 - the pose initialisers of RiderLaunch and 0x8008F138
    (0x8008EE60, "PoseA", "5"), (0x8008E818, "PoseB", "5"), (0x8008E50C, "PoseC", "5"),
    (0x8008EB88, "PoseD", "5"), (0x8008E044, "PoseE", "5"),
    # 6, 7 - the rider's voice line (SLUS) and the instant remount
    (0x8001A760, "RiderSpeech", "6"), (0x800903F4, "Remount", "7"),
    # 8 - combat: the picker, the engage test, the blow
    (0x8008B428, "Pick", "8.1"), (0x800BC1EC, "CanEngage", "8.2"), (0x800C17B0, "ApplyHit", "8.3"),
    (0x800BF860, "HitStance", "8.4"), (0x800BF424, "NoteHit", "8.5"), (0x800BF604, "HitShove", "8.5"),
    (0x800BFD74, "HitRumble", "8.5"), (0x800BF674, "KnockOff", "8.6"),
    # 9 - FightUpdate's other seams
    (0x800C1014, "FightEnd", "9.1"), (0x800C110C, "FightStart", "9.2"), (0x800C12D8, "FightBegin", "9.2"),
    (0x800BF978, "FightPickRecord", "9.3"), (0x800C0BE8, "FightContinue", "9.4"), (0x800C0CE8, "FightSteer", "9.5"),
    (0x800C0E98, "FightPace", "9.5"), (0x800C122C, "FightRestart", "9.6"), (0x800BFF04, "WeaponSteal", "9.7"),
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
