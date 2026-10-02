r"""The runs of the crash-chain probe: snapshot copies of the player's savestates that differ by DATA only (every
edit that is not a placement is named in the run's note), in impact.py's form (name, source, build, frames, pad,
explore, note). One module per reading pass; impact.py's runs reused by several passes appear
once. crash.py runs the VERIFY subset (crash.VERIFY_RUNS); `crash.py live --all` runs every one."""
from . import runs_a, runs_b, runs_c, runs_d, runs_e, runs_f

PASSES = {"A": runs_a, "B": runs_b, "C": runs_c, "D": runs_d, "E": runs_e, "F": runs_f}


def all_runs():
    """[(pass, spec)] in pass order, each name once (a name defined twice must be the same impact.py spec)"""
    out, seen = [], {}
    for k, mod in PASSES.items():
        for spec in mod.RUNS:
            if spec[0] in seen:
                assert seen[spec[0]] is spec, f"run {spec[0]} defined twice differently"
                continue
            seen[spec[0]] = spec
            out.append((k, spec))
    return out
