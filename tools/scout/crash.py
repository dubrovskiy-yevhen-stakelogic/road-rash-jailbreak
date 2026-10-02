"""Scout probe for the CRASH CHAIN and the COMBAT links of Road Rash: Jailbreak (USA, SLUS_01053).

The crash chain is the rider pass function `RASHCDG 0x80078DB4` that holds the class-8 knock-off request, the
bike-bike gate `0x80081D7C`, `Bounce 0x80084564` / `Spin 0x800849D8`, the remount `0x800903F4`, the five pose
initialisers of `RiderLaunch`, `SLUS 0x8001A760`, and for combat the target picker `0x8008B428`, `CanEngage
0x800BC1EC` and the body of `ApplyHit 0x800C17B0`.

This probe re-derives their behaviour independently of the C++ tree, with the harness of `impact.py` (pairs.py's
replay plus stack arguments against the guest's own words and every register argument at the callee's entry probe):

  * SKELETON: image hashes, true extents, frames, the `jal` word at every call site named, the callee MULTISET of
    every function read with its ported / unported status READ out of the bench's row table, the jump tables, the
    literal words the arithmetic rests on, a COP2 / jalr census;
  * LIVE: the transcriptions (tools\\scout\\crash_models\\, one module per function) are run as MODELS against the
    real functions executing in our interpreter, on snapshot copies that differ from the player's savestates by DATA
    only (tools\\scout\\crash_runs\\; `verify` runs the VERIFY_RUNS subset, `live --all` every one);
  * FACTS: what the runs show the unmodified game doing.

Reads ONLY work\\disc_us and work\\oracle\\state; writes ONLY under work\\crash (snapshot copies, trace outputs,
deleted after each run unless --keep). Uses <build>\\rrverify.exe read-only. Never copies game bytes into the repo.

    python tools\\scout\\crash.py info             # skeleton only
    python tools\\scout\\crash.py verify           # crash: <n> checks, <m> failures (80 runs, --jobs 8)
    python tools\\scout\\crash.py verify --mutate  # each mutated claim must fail its own check
    python tools\\scout\\crash.py live --all --cover [--only a,b] [--keep]     # every run, block coverage
    python tools\\scout\\crash.py census --tag X   # per run: calls, mutation catch, blocks -> work\\crash\\census_X.json
    python tools\\scout\\crash.py dev --tag X --models DIR --runs FILE [--cover]   # a reading pass's own loop
"""

from __future__ import annotations

import argparse

import glob
import importlib.util
import os
import re
import shutil
import struct
import sys
from collections import Counter

import warnings  # noqa: E402
warnings.filterwarnings("ignore", category=SyntaxWarning)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pairs as P    # noqa: E402
import impact as I   # noqa: E402

ROOT = P.ROOT
STATE_DIR = P.STATE_DIR
WORK = os.path.join(ROOT, "work", "crash")
MUTATE = P.MUTATE
u32, s32, iabs = P.u32, P.s32, P.iabs
word_at = P.word_at
_rd = I._rd
FX = I.FX
PLAYER = P.PLAYER


def cfg(orig, mutated):
    return mutated if MUTATE["on"] else orig


# ---------------------------------------------------------------------------
# the bench's row table: which guest functions are ported (read, not asserted by hand)
# ---------------------------------------------------------------------------

def load_ported():
    """{entry: row name} for every acceptance row of the bench, read from the bench's own row table
    (`rrverify phys --list`, pairs.bench_rows). Reading `Row{...}` out of the sources instead would miss every row
    built by a helper (PopRow, BindRow, WRow, ...)."""
    return P.bench_rows()


_PORTED = {}


def ported():
    if not _PORTED:
        _PORTED.update(load_ported())
    return _PORTED


# ---------------------------------------------------------------------------
# runs: snapshot copies that differ from a savestate by DATA only (every edit is named; a non-placement says so)
# ---------------------------------------------------------------------------

def prepare(tag, name, src, edits):
    """copy savestate `src` to work\\crash\\<tag>\\state-<name>, apply `edits` [(addr, size|'add', value)]"""
    dst = os.path.join(WORK, tag, "state-" + name)
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copytree(os.path.join(STATE_DIR, src), dst)
    p = os.path.join(dst, "ram.bin")
    ram = bytearray(open(p, "rb").read())
    for a, n, v in edits:
        o = a & 0x1FFFFF
        if n == "add":
            struct.pack_into("<I", ram, o, u32(struct.unpack_from("<I", ram, o)[0] + v))
        else:
            struct.pack_into({4: "<I", 2: "<H", 1: "<B"}[n], ram, o, v & ((1 << (8 * n)) - 1))
    open(p, "wb").write(ram)
    return dst, bytes(ram)


# the watch set of impact.py (none overlapping), plus the SLUS globals 0x80054000..0x8005B000
# (0x8001A760's per-player records) and the rest of 0x800D0000..0x800D6000 is inside impact's glob watch already
WATCH_ALL = I.WATCH_ALL + [(0x80053000, 0x8000, "sdata"), (0x801A4C00, 0x400, "speech")]


def _no_overlap(ws):
    s = sorted((lo, lo + n) for lo, n, _ in ws)
    return all(a[1] <= b[0] for a, b in zip(s, s[1:]))


assert _no_overlap(WATCH_ALL)

FRAME_TICK = 0x8008AB00


def do_run(tag, spec, funcs, probes=(), keep=False):
    name, src, build, frames, pad, explore, note = spec
    srcdir = os.path.join(STATE_DIR, src)
    ram0 = open(os.path.join(srcdir, "ram.bin"), "rb").read()
    edits = build(ram0) if build else []
    st, ram = prepare(tag, name, src, edits)
    out = os.path.join(WORK, tag, "tr-" + name)
    shutil.rmtree(out, ignore_errors=True)
    stop = run_trace(st, frames, funcs, pad, explore, list(probes) + FACT_PROBES, out)
    return I.load_trace(out, ram), stop, edits, st, out


def run_trace(state, frames, funcs, pad, explore, probes, out):
    """impact.run_trace with the probes written as bare addresses (a probe's name defaults to its spec): with every
    block leader of the 28 modelled functions the named form overflows the Windows command line"""
    import subprocess
    cmd = [I.RRVERIFY, "trace", "--state", state, "--frames", str(frames), "--no-gpu", "--pad", str(pad),
           "--out", out, "--no-cop2", "--calls"]
    if explore:
        cmd.append("--explore")
    for a, n, w in WATCH_ALL:
        cmd += ["--watch", f"{a:#x}:{n}:{w}"]
    for p in sorted(set(I.probes_for(funcs, True)) | set(probes)):
        cmd += ["--probe", f"{p:#x}"]
    for attempt in range(5):         # a transient sharing violation on starting the exe (seen once): retry, then raise
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
            break
        except PermissionError:
            if attempt == 4:
                raise
            import time
            time.sleep(2 + 2 * attempt)
    return [ln.split(None, 1)[1].strip() for ln in r.stdout.splitlines() if ln.startswith(("stopped", "buffer swaps"))]


# ---------------------------------------------------------------------------
# models
# ---------------------------------------------------------------------------

def load_models(extra_dir=None):
    """[(entry, frame, model, name)] from tools\\scout\\crash_models (and a reading pass's directory)"""
    out = []
    try:
        import crash_models as CM
        out += CM.load()
    except ModuleNotFoundError:
        pass
    if extra_dir:
        for p in sorted(glob.glob(os.path.join(extra_dir, "m_*.py"))):
            spec = importlib.util.spec_from_file_location("crashdev_" + os.path.basename(p)[:-3], p)
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            f = int(os.path.basename(p)[2:10], 16)
            out = [o for o in out if o[0] != f]
            out.append((f, mod.FRAME, mod.model, getattr(mod, "NAME", f"f{f:08x}")))
    return out


ARGSTAT = Counter()


def leaders(f):
    """the basic-block leaders of [f, end): the entry, every in-function branch / `j` target, the instruction after
    every branch's or `j`'s delay slot (a `jal` does not end a block) and every in-function word of a jump table
    that a `jr` other than `jr ra` dispatches through (found by the `lui/addiu` or `lui/lw` base in front of it)"""
    lo, hi = I.extent(f)
    L = {lo}
    for a in range(lo, hi, 4):
        w = word_at(a)
        op = w >> 26
        if op in (1, 4, 5, 6, 7, 0x14, 0x15, 0x16, 0x17):
            t = u32(a + 4 + 4 * (s32((w & 0xFFFF) << 16) >> 16))
            if lo <= t < hi:
                L.add(t)
            L.add(a + 8)
        elif op == 2:
            t = ((a + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            if lo <= t < hi:
                L.add(t)
            L.add(a + 8)
        elif op == 0 and (w & 0x3F) == 8:
            L.add(a + 8)
    for t in jt_targets(f):
        L.add(t)
    return sorted(x for x in L if lo <= x < hi)


# entry -> [(table address, count)]: the jump tables inside the modelled functions
JT = {0x800BF860: [(0x8005BA04, 6)], 0x800BF978: [(0x8005BA1C, 7), (0x8005BA3C, 7), (0x8005BA5C, 9)]}


def jt_targets(f):
    out = []
    for tab, n in JT.get(f, []):
        out += [word_at(tab + 4 * i) for i in range(n)]
    return out


COVER = {}


def all_run_specs(runs_file=None):
    """{name: spec}: crash_runs (every reading pass's runs) plus a dev runs file's"""
    import crash_runs
    out = {spec[0]: spec for _, spec in crash_runs.all_runs()}
    if runs_file:
        spec = importlib.util.spec_from_file_location("crashdev_runs", runs_file)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        out.update({s[0]: s for s in mod.RUNS})
    return out


def eval_run(tag, spec, mods, cover=False, census=False, facts=False, keep=False):
    """one run: build the snapshot copy, trace it, run every model on every call of its function; the result is
    plain data (it crosses a process boundary with --jobs)"""
    funcs = [f for f, _, _, _ in mods]
    extra = sorted({x for f in funcs for x in leaders(f)}) if cover else []
    watched = I.watched_in(WATCH_ALL)
    I.ARGSTAT.clear()
    tr, stop, edits, st, out = do_run(tag, spec, funcs, probes=extra, keep=keep)
    res = {"name": spec[0], "src": spec[1], "frames": spec[3], "pad": spec[4], "stop": (stop or ["?"])[0],
           "edits": edits, "models": {}, "cover": {}, "census": {}, "facts": None}
    if cover:
        for f in funcs:
            res["cover"][f] = sorted(x for x in leaders(f) if tr.probes.get(x))
    for f, frame, model, nm in mods:
        rr = I.run_model(tr, model, f, frame, watched, strict=True)
        ok = sum(1 for r in rr if r[1] is True)
        bad = [r for r in rr if r[1] is False]
        res["models"][f] = (ok, len(bad), sum(1 for r in rr if r[1] is None), [f"seq {r[0]}: {r[2]}" for r in bad[:3]])
        if census and rr:
            mut0 = MUTATE["on"]
            MUTATE["on"] = True
            mr = I.run_model(tr, model, f, frame, watched, strict=True)
            MUTATE["on"] = mut0
            res["census"][f] = (ok, ok + len(bad), sum(1 for r in mr if r[1] is True))
    if facts:
        res["facts"] = run_facts(spec[0], tr)
    res["argstat"] = dict(I.ARGSTAT)
    if not keep:
        shutil.rmtree(out, ignore_errors=True)
        shutil.rmtree(st, ignore_errors=True)
    return res


def _worker(job):
    """a --jobs worker: re-creates the models and the run spec in its own process"""
    tag, name, mutate, cover, census, facts, keep, models_dir, runs_file, only_funcs = job
    MUTATE["on"] = mutate
    mods = load_models(models_dir)
    if only_funcs is not None:
        mods = [m for m in mods if m[0] in only_funcs]
    return eval_run(tag, all_run_specs(runs_file)[name], mods, cover, census, facts, keep)


def check_live(bench, mods, runs, tag, verbose=True, only=None, keep=False, facts=False, not_run=(), cover=False,
               census=None, jobs=1, models_dir=None, runs_file=None):
    funcs = [f for f, _, _, _ in mods]
    runs = [s for s in runs if not only or s[0] in only]
    if jobs > 1:
        from concurrent.futures import ProcessPoolExecutor
        job = lambda s: (tag, s[0], MUTATE["on"], cover, census is not None, facts, keep, models_dir, runs_file,
                         set(funcs))
        with ProcessPoolExecutor(max_workers=jobs) as ex:
            results = list(ex.map(_worker, [job(s) for s in runs]))
    else:
        results = (eval_run(tag, s, mods, cover, census is not None, facts, keep) for s in runs)
    agg = {f: Counter() for f in funcs}
    seen, edits = {}, {}
    ARGSTAT.clear()
    names = {f: nm for f, _, _, nm in mods}
    for res in results:
        line = []
        for f, (ok, nbad, trunc, msgs) in res["models"].items():
            agg[f]["calls"] += ok + nbad
            agg[f]["match"] += ok
            agg[f]["truncated"] += trunc
            if ok + nbad:
                line.append(f"{names[f]} {ok}/{ok + nbad}")
            for mm in msgs:
                print(f"    MISMATCH {names[f]} {res['name']} {mm}")
        for f, got in res["cover"].items():
            COVER.setdefault(f, set()).update(got)
        if census is not None:
            d = census.setdefault(res["name"], {})
            d.update({f"{f:#x}": list(v) for f, v in res["census"].items()})
            cv = sorted(f"{x:#x}" for got in res["cover"].values() for x in got)
            if cv:
                d["cover"] = cv
        if facts:
            seen[res["name"]] = res["facts"]
            edits[res["name"]] = res["edits"]
        ARGSTAT.update(res["argstat"])
        if verbose:
            print(f"  {res['name']} ({res['src']}, {len(res['edits'])} edits, {res['frames']} frames, pad {res['pad']}): "
                  f"{res['stop'][:48]}")
            print("    " + ", ".join(line))
    for f, frame, model, nm in mods:
        a = agg[f]
        if only and not a["calls"] and f not in not_run:
            continue
        if f in not_run:
            bench.check(a["calls"] == 0, f"{nm} {f:#x}: entered in no run ({a['calls']} calls) - transcription only")
            continue
        bench.check(a["calls"] > 0 and a["match"] == a["calls"],
                    f"{nm} {f:#x}: model == interpreter on {a['match']}/{a['calls']} calls")
        if verbose:
            print(f"  {nm:18s} {f:#x}: {a['match']}/{a['calls']} calls ({a['truncated']} cut off by the end of a run)")
    if cover:
        for f, frame, model, nm in mods:
            L = leaders(f)
            got = COVER.get(f, set())
            print(f"  cover {nm} {f:#x}: {len(got)}/{len(L)} blocks; never: "
                  f"{' '.join(f'{x:08X}' for x in L if x not in got)}")
    if verbose:
        print(f"  calls made by the models: {ARGSTAT['calls']}; register arguments compared at the callee's entry "
              f"probe on {ARGSTAT['entry']}, by re-executing the delay slot on the rest; left uncompared: "
              f"{ARGSTAT['unchecked']}")
    return seen, edits


# ---------------------------------------------------------------------------
# facts: what the unmodified game does in the runs; every one re-checked on every verify
# ---------------------------------------------------------------------------

FACT_PROBES = [FRAME_TICK, 0x80079A58]          # the frame tick; the return of ImpactStatePass's GetRCnt
RAND, RCNT, PS3D = 0x8001FC58, 0x80043F00, 0x80017BA0
BIKE = lambda k: PLAYER + 1096 * k
RIDER0, RIDER6 = 0x801BB514 - 0x228, P.RIDER6     # the player's rider (pool 1 slot 0) and the downed rider 6
RD0 = 0x800D5758                                  # the player's riderDef


def run_facts(name, tr):
    """the measurements one run contributes (small: the trace is deleted after the run)"""
    import bisect as _b  # noqa: E402
    frames = sorted(s for s, _ in tr.probes.get(FRAME_TICK, []))
    fr = lambda sq: _b.bisect_right(frames, sq)
    pcs = {0x8007916C, 0x80079634, 0x80083E30, 0x800836C4, 0x80082358, 0x8007D81C, 0x80093F7C, 0x80094F7C,
           0x8001AED4, 0x800C1A58, 0x800C00A0, 0x800C00EC, 0x800905C8, 0x800C306C}
    w = {}
    for sq, pc, a, n, v in tr.writes:
        if pc in pcs:
            w.setdefault(pc, []).append((fr(sq), a, v))
    c = {}
    for sq, frm, tgt, args, sp in tr.calls:
        if tgt in (RAND, RCNT, PS3D, 0x8001A760, 0x80084564, 0x800849D8, 0x800903F4, 0x800BCA68):
            c.setdefault(tgt, []).append((fr(sq), frm, args))
    rc = [(fr(s), r["v0"]) for s, r in tr.probes.get(0x80079A58, [])]
    return {"w": w, "c": c, "rc": rc}


def check_facts(bench0, seen, edits, verbose=True):
    mut = MUTATE["on"]

    class _B:                                   # print every fact, not only the failed ones
        def check(self, ok_, what):
            if ok_ and verbose:
                print("  ok", what)
            return bench0.check(ok_, what)
    bench = _B()
    W = lambda n, pc: seen[n]["w"].get(pc, [])
    C = lambda n, t: seen[n]["c"].get(t, [])
    ok = lambda n: n in seen
    # 1. a class-8 car hit: the request in the impact frame, the THROW the next frame, three Rand
    if ok("crash"):
        req = [(f, a, v) for f, a, v in W("crash", 0x8007916C) if a == RIDER0 + 0x228 and v & 0x8000]
        thr = [(f, v) for f, a, v in W("crash", 0x80079634) if a == PLAYER + 0x238]
        cls = [(f, v & 0x3F) for f, a, v in W("crash", 0x80083E30) if a == PLAYER + 0x238]
        rnd = [(f, frm) for f, frm, _ in C("crash", RAND) if 0x80078DB4 <= frm < 0x80079B20]
        good = (req and thr and cls == [(req[0][0], 8)] and (thr[0][1] & 0x40FFF) == 0x40A00
                and thr[0][0] == req[0][0] + cfg(1, 0)          # mutated: the throw in the request's frame
                and rnd == [(thr[0][0], s) for s in (0x800792C0, 0x80079548, 0x800795AC)])
        bench.check(good, f"crash: class 8 {cls}, knock-off request (frame, word) {[(f, hex(v)) for f, _, v in req]}, "
                          f"throw {[(f, hex(v)) for f, v in thr]}, Rand {[(f, hex(s)) for f, s in rnd]}")
    # 2. a NATURAL class 4 knocks the rider off, without a throw
    if ok("hard-4-45"):
        cls = [v & 0x3F for f, a, v in W("hard-4-45", 0x80083E30) if a == PLAYER + 0x238]
        req = [f for f, a, v in W("hard-4-45", 0x8007916C) if a == RIDER0 + 0x228 and v & 0x8000]
        thr = [f for f, a, v in W("hard-4-45", 0x80079634) if a == PLAYER + 0x238]
        st = [v for f, a, v in W("hard-4-45", 0x800C306C) if a == RIDER0 + 0x220]
        bench.check(cls == [4] and req and (bool(thr) == cfg(False, True)) and 40 in st,     # mutated: a throw
                    f"hard-4-45 (no forced flag): class {cls}, request frames {req}, throws {thr}, stances {st}")
    # 3. a head-on bike pair: class 8 on BOTH bikes, both riders knocked off
    if ok("head"):
        cl = sorted((a, v & 0xF) for f, a, v in W("head", 0x800836C4))
        req = sorted({a for f, a, v in W("head", 0x8007916C) if v & 0x8000})
        bench.check(len(cl) == 2 and cl[0][0] != cl[1][0] and all(k == cfg(8, 4) for _, k in cl) and len(req) == 2,
                    f"head: BikeBikeGate classes {[(hex(a), k) for a, k in cl]}, requests on {[hex(a) for a in req]}")
    # 4. the knock-on crash of a crashed bike's partner: 0x200840, RiderSpeech(14, 1), the rider pass's OTHER writer
    if ok("c15-ai"):
        ko = [(f, v) for f, a, v in W("c15-ai", 0x80082358) if a == BIKE(14) + 0x238]
        sp = [(f, frm) for f, frm, _ in C("c15-ai", 0x8001A760)]
        r14 = [f for f, a, v in W("c15-ai", cfg(0x8007D81C, 0x8007916C)) if v & 0x8000]   # mutated: 0x8007916C
        bench.check(bool(ko) and (ko[0][1] & 0x200840) == 0x200840 and sp == [(ko[0][0], 0x80083828)] and bool(r14),
                    f"c15-ai: bike 14 flagsC {[(f, hex(v)) for f, v in ko]} at 0x80082358, RiderSpeech from "
                    f"{[(f, hex(s)) for f, s in sp]}, its rider's request at 0x8007D81C in frames {r14}")
    # 5. Bounce and Spin without a forced flag: an AI crash into a crashed AI bike. Frame 3: bike 13 into the
    #    crashed 15 - the one-crashed arm, Bounce(C) 0x80082088 >= 0 -> Spin(C) 0x8008209C. Frame 5 (reached because the
    #    interpreter models the MDEC status read 0x1F801824): both bikes crashed - Bounce(a)
    #    0x80081F70 < 0, no Spin(a); Bounce(b) 0x80081FE0 >= 0 -> Spin(b) 0x80081FF4. Three Rand per Spin.
    if ok("pair13-4.5-0.0"):
        e = edits.get("pair13-4.5-0.0", [])
        b = [(f, frm) for f, frm, _ in C("pair13-4.5-0.0", 0x80084564)]
        s = [(f, frm) for f, frm, _ in C("pair13-4.5-0.0", 0x800849D8)]
        r = [(f, frm) for f, frm, _ in C("pair13-4.5-0.0", RAND) if 0x800849D8 <= frm < 0x80084BE8]
        bench.check(bool(e) and all(n == "add" for _, n, _ in e)
                    and b == [(3, cfg(0x80082088, 0x80081F70)), (5, 0x80081F70), (5, 0x80081FE0)]
                    and s == [(3, 0x8008209C), (5, 0x80081FF4)]
                    and r == [(f, x) for f in (3, 5) for x in (0x80084A0C, 0x80084A54, 0x80084A94)],
                    f"pair13-4.5-0.0 ({len(e)} edits, all box placements): Bounce (frame, from) "
                    f"{[(f, hex(x)) for f, x in b]}, Spin {[(f, hex(x)) for f, x in s]}, its Rand "
                    f"{[(f, hex(x)) for f, x in r]}")
    # 6. a downed AI rider out of view is remounted at once and the bike put back in the race
    if ok("down-far"):
        rm = [(f, frm) for f, frm, _ in C("down-far", 0x800903F4)]
        live = [f for f, a, v in W("down-far", 0x80093F7C) if a == P.BIKE6 + 0x140 and v == 1]
        seat = [f for f, a, v in W("down-far", 0x80094F7C) if a == RIDER6 + 0x25C and v == 1]
        bench.check(rm == [(1, cfg(0x800953F4, 0x80094078))] and live and seat and min(live) > 1 and min(seat) > 1,
                    f"down-far: Remount {[(f, hex(s)) for f, s in rm]}, bike 6 live again in frames {live[:3]}, "
                    f"its rider seated (+0x25C := 1) in frames {seat}")
    # 7. the player's taunt provokes an AI: grudge 15, {16, speaker} pushed from RiderSpeech
    if ok("taunt-q"):
        g = [v for f, a, v in W("taunt-q", 0x8001AED4)]
        pu = [frm for f, frm, _ in C("taunt-q", 0x800BCA68) if 0x8001A760 <= frm < 0x8001B244]
        bench.check(g == [cfg(15, 14)] and pu == [0x8001AF38],
                    f"taunt-q: grudge byte := {g} at 0x8001AED4, AiPushCommand from {[hex(x) for x in pu]}")
    # 8. the charged blow: base x 3
    if ok("punch-cop-x3"):
        h = [v for f, a, v in W("punch-cop-x3", 0x800C1A58)]
        want = 64 - ((cfg(3, 2) * 20 * 96 * 124) >> 14)
        bench.check(h == [want], f"punch-cop-x3: the cop's health := {h} = 64 - (3*20 * 96 * 124 >> 14) = {want}")
    # 9. a weapon steal, and a block
    if ok("fight-c148w1") and ok("fight-c143w4"):
        st = [(a, v) for f, a, v in W("fight-c148w1", 0x800C00A0)] + [(a, v) for f, a, v in W("fight-c148w1", 0x800C00EC)]
        st4 = W("fight-c143w4", 0x800C00A0)
        bl = [args[2] for f, frm, args in C("fight-c143w4", PS3D) if frm == 0x800C0330]
        bench.check(len(st) == 2 and st[1] == (RD0 + 0x2E, 9) and (bool(st4) == cfg(False, True)) and bl == [88],
                    f"fight-c148w1: the steal {[(hex(a), v) for a, v in st]}; fight-c143w4: no steal, sound {bl} "
                    f"from 0x800C0330 (blocked)")
    # 10. the race director's instant remount of a two-rider bike seats the passenger
    if ok("two-up"):
        rm = [frm for f, frm, _ in C("two-up", 0x800903F4)]
        seat = [(a, v) for f, a, v in W("two-up", 0x800905C8)]
        bench.check(rm == [0x800B9538] and seat == [(RIDER6 + 0x25C, 1)],
                    f"two-up: Remount from {[hex(x) for x in rm]}, passenger +0x25C := {[(hex(a), v) for a, v in seat]}")
    # 11. the impact-end sound is picked from the HARDWARE counter
    if ok("b6-c2-end"):
        rc = seen["b6-c2-end"]["rc"]
        snd = [args for f, frm, args in C("b6-c2-end", PS3D) if 0x80078DB4 <= frm < 0x80079B20]
        want = [50 + (((v & 0xFF) * cfg(5, 4)) >> 8) for _, v in rc]      # mutated: x 4
        bench.check(len(rc) == 1 and [a[2] for a in snd] == want,
                    f"b6-c2-end: GetRCnt {[hex(v) for _, v in rc]} -> sound {[a[2] for a in snd]} (50 + (rc & 0xFF) * 5 >> 8)")


# The verify subset of crash_runs: the fact runs, then a greedy cover of the 218 runs' census
# (`crash.py census`) that keeps every block any run executes, every model's calls and every model's mutation catch.
VERIFY_RUNS = [
    "crash", "hard-4-45", "head", "c15-ai", "pair13-4.5-0.0", "down-far", "taunt-q", "punch-cop-x3", "fight-c148w1",
    "fight-c143w4", "two-up", "b6-c2-end", "knock-cop", "punch-cop", "hit", "bump", "pole", "knock-cop-p358",
    "ai-taunt-full", "air-knock", "head-60-30", "ai-taunt-33c7", "ride-over-p", "tb1-b29", "air-throw",
    "b6-c201-vert", "b6-ramp-neg2", "ai-vs-ai", "b6-c204-m1", "punch-cop-psg", "prop-side", "ai-knock", "cboth-side",
    "ai-taunt-own", "decode8wNone", "fight-c143w0", "ai-chain", "q15-c2", "m2p-90", "ai-behind13", "b6-ramp",
    "fight-c148w3", "b6-c8-t-up", "fight-c146w0", "air-up", "m2p-91", "fight-c143w6", "rand-side", "fight-c146w4",
    "b6-dir3", "air-c15b", "head-b29", "m2p-89", "ai-taunt", "punch-cop-hp120", "punch-cop-a56", "fight-c71w9",
    "fight-c39w9", "steal-cls0", "a-c4", "b6-b4", "m2p-88", "police", "punch-cop-bar", "fight-c75w9",
    "far-15on14-slow", "stance-l1", "retire5-done", "knock-cop-regen", "two-up-rev", "fight-c78w9", "ai-end77",
    "slow-side2", "head-30-60", "fight-c143w8", "ai-redraw", "f80-side", "taunt-cop", "ai-nunchaku",
    "pack-otherroad",
]
NOT_RUN = set()           # every modelled function is entered by some run


def verify_runs():
    specs = all_run_specs()
    return [specs[n] for n in VERIFY_RUNS]


# ---------------------------------------------------------------------------
# skeleton: the claims read straight off the images
# ---------------------------------------------------------------------------

# entry: (bytes from the prologue to the last `jr ra` + its delay slot, the callee MULTISET {target: jal sites})
CALLEES = {
    0x80078DB4: (3436, {0x80010028: 12, 0x80017BA0: 1, 0x8001FC58: 3, 0x8001FC90: 19, 0x8001FF3C: 1, 0x80020018: 1,
                        0x8002E468: 1, 0x8002E698: 3, 0x8002EAD8: 2, 0x8002EE50: 1, 0x80043F00: 1, 0x8004CF74: 2,
                        0x80073D74: 1}),
    0x80081D7C: (6888, {0x80010028: 2, 0x8001A760: 1, 0x8001FC90: 9, 0x80020018: 2, 0x8002E548: 5, 0x8002E570: 1,
                        0x8002E698: 6, 0x8002EE50: 2, 0x8002EED8: 1, 0x80080B10: 1, 0x80080D1C: 1, 0x80083864: 1,
                        0x80083F30: 1, 0x80084564: 4, 0x800849D8: 3}),
    0x80084564: (1140, {0x8001FC90: 20, 0x8001FEB4: 1, 0x8001FF3C: 1, 0x8002E468: 1, 0x8002E698: 1, 0x8002EA20: 1,
                        0x8002EE50: 1, 0x8004CF74: 5}),
    0x800849D8: (528, {0x8001FC58: 3, 0x8001FC90: 1}),
    0x8008EE60: (520, {0x8001FC90: 10, 0x8003FA18: 1}),
    0x8008E818: (880, {0x8001FC90: 11}),
    0x8008E50C: (780, {0x8001FC90: 10, 0x8003FA18: 1}),
    0x8008EB88: (728, {0x8001FC90: 11, 0x8003FA18: 1}),
    0x8008E044: (1224, {0x8001FC90: 11, 0x8002E548: 1, 0x8002EE50: 1, 0x8002EED8: 1}),
    0x8001A760: (2788, {0x8001769C: 1, 0x80017814: 1, 0x80017BA0: 1, 0x8001F934: 1, 0x80043F00: 2, 0x8008B428: 1,
                        0x800B6AAC: 1, 0x800B92C0: 1, 0x800BC1EC: 1, 0x800BCA68: 1}),
    0x800903F4: (1056, {0x80012838: 2, 0x80018440: 1, 0x8002090C: 1, 0x800235B0: 1, 0x8002EAD8: 1, 0x8007EC30: 1,
                        0x80086AF8: 1, 0x80095AEC: 1, 0x80096564: 1, 0x800BCA68: 1, 0x800BCD10: 1, 0x800C3104: 2}),
    0x8008B428: (1060, {}),
    0x800BC1EC: (784, {0x800B6AAC: 1}),
    0x800C17B0: (1572, {0x80017B6C: 2, 0x80017BA0: 1, 0x800A8BE0: 2, 0x800BF424: 1, 0x800BF604: 1, 0x800BF674: 1,
                        0x800BF860: 1, 0x800BFD74: 2, 0x800BFE58: 4, 0x800C4550: 1}),
    0x800BF860: (280, {}),
    0x800BF424: (248, {}),
    0x800BF604: (112, {}),
    0x800BFD74: (228, {0x8001DD74: 1}),
    0x800BF674: (492, {0x80096F30: 1}),
    0x800C1014: (248, {0x800BC8DC: 1}),
    0x800C110C: (288, {0x8001FC58: 1, 0x800B6AAC: 2, 0x800C12D8: 1}),
    0x800C12D8: (152, {0x800BF978: 1, 0x800C4550: 1}),
    0x800BF978: (740, {0x8001E08C: 1}),
    0x800C0BE8: (256, {0x800BB8FC: 1, 0x800BC8DC: 1, 0x800BCA68: 1, 0x800C4550: 1}),
    0x800C0CE8: (432, {0x8001A760: 1, 0x800BC4FC: 1, 0x800BC618: 1, 0x800C0E98: 1}),
    0x800C0E98: (380, {}),
    0x800C122C: (172, {0x800B92C0: 1, 0x800C12D8: 1}),
    0x800BFF04: (1112, {0x80017BA0: 2, 0x800273EC: 1, 0x800958F0: 1, 0x800BFE58: 2, 0x800C2030: 1, 0x800C2E9C: 1,
                        0x800C2F84: 1, 0x800C4550: 1}),
}

# the callees of the 28 functions that are NOT a bench row: everything else they call is ported (among them
# MulAdd16 0x8002EA20, ScaleTo16 0x8002EED8, MassExchange 0x80083864, Bounce 0x80084564, Spin 0x800849D8,
# 0x80080D1C HitSpeed, 0x80083F30 ImpactTurn, 0x8001A760 RiderSpeech, 0x800BF978 FightPickRecord, ...).
# Left: the FightPickRecord byte copy and the camera-target leaf.
UNPORTED = {0x8001E08C, 0x800235B0}

# entry: every `jal` to it in RASHCDG and SLUS text (the exact set)
SITES = {
    0x80078DB4: {0x8007DCD8, 0x8007DCE4},
    0x80081D7C: {0x800AC2E4},
    0x80084564: {0x80081F70, 0x80081FE0, 0x80082088, 0x800824E4, 0x80083A38, 0x80084030, 0x800A9718, 0x800B1FD4,
                 0x800B3600},
    0x800849D8: {0x80081F84, 0x80081FF4, 0x8008209C, 0x80083A4C, 0x80084044, 0x800B1FF8},
    0x8008EE60: {0x80091590, 0x8008F1D8}, 0x8008E818: {0x800918F0, 0x8008F1B4}, 0x8008E50C: {0x80091B78, 0x8008F1C4},
    0x8008EB88: {0x80091F60, 0x8008F200}, 0x8008E044: {0x800916EC, 0x8008F1EC},
    0x8001A760: {0x80083828, 0x8009183C, 0x80091AF8, 0x80091EBC, 0x800920D0, 0x800AD8F4, 0x800B90C4, 0x800C0DF0,
                 0x8001D548},
    0x800903F4: {0x80092864, 0x800939DC, 0x80094078, 0x800953F4, 0x800A071C, 0x800B9538, 0x800C9124},
    0x8008B428: {0x800C1E28, 0x800C1FA8, 0x800C4E8C, 0x8001AE3C},
    0x800BC1EC: {0x8009E0F8, 0x800B9094, 0x800B917C, 0x800B91AC, 0x800BBE60, 0x800C04D4, 0x8001AF04},
    0x800C17B0: {0x800C0978, 0x800C1508}, 0x800BF860: {0x800C1808}, 0x800BF424: {0x800C1C18},
    0x800BF604: {0x800C1C90}, 0x800BFD74: {0x800C1CF4, 0x800C1D98}, 0x800BF674: {0x800C1B78},
    0x800C1014: {0x800BFD5C, 0x800C0430}, 0x800C110C: {0x800C0460}, 0x800C12D8: {0x800C120C, 0x800C12BC},
    0x800BF978: {0x800C12FC}, 0x800C0BE8: {0x800C060C}, 0x800C0CE8: {0x800C064C}, 0x800C0E98: {0x800C0E50},
    0x800C122C: {0x800C0824, 0x800C14C0}, 0x800BFF04: {0x800C0938},
}

# every Rand (SLUS 0x8001FC58) and hardware-counter (SLUS 0x80043F00) call site inside the 28 functions
RAND_SITES = {0x800792C0, 0x80079548, 0x800795AC, 0x80084A0C, 0x80084A54, 0x80084A94, 0x800C1150}
RCNT_SITES = {0x80079A50, 0x8001AB54, 0x8001AB68}

# the words the claims rest on (our objdump listing)
WORDS = [
    (0x8007916C, 0xAEE30228, "sw v1,552(s7) - ImpactStatePass: the rider's knock-off request +0x228 |= 0x8000"),
    (0x800791BC, 0xAC620228, "sw v0,552(v1) - the same request for the +0x358 bike's rider"),
    (0x80079634, 0xAE430238, "sw v1,568(s2) - the THROW: flagsC := (flagsC & 0xFFF00200) | 0x40A00"),
    (0x80079A50, 0x0C010FC0, "jal 0x80043F00 - GetRCnt picks the impact-end sound 50..54"),
    (0x800830F0, 0x4B78000C, "c2 0x178000C - BikeBikeGate's one GTE OP"),
    (0x80082358, 0xAC820238, "sw v0,568(a0) - BikeBikeGate: the knock-on crash flagsC |= 0x200840"),
    (0x800836C4, 0xAEA20238, "sw v0,568(s5) - BikeBikeGate ORs the CLASS into flagsC"),
    (0x80084AB0, 0xAE710290, "sw s1,656(s3) - Spin: +0x290 := 2 (Rand #1 % 45752 - 22876)"),
    (0x80084AB4, 0xAE700280, "sw s0,640(s3) - Spin: +0x280 := 2 (Rand #2 % 45752 - 22876)"),
    (0x80084B08, 0xAE6201E8, "sw v0,488(s3) - Spin: +0x1E8 x (0.8 + Rand #3 % 26215)"),
    (0x8008F394, 0xAE020138, "sw v0,312(s0) - 0x8008F138 restores the rider's +0x138 after BuildObbAlt"),
    (0x800905C8, 0xAE12025C, "sw s2,604(s0) - Remount: the passenger's +0x25C := 1"),
    (0x800904E8, 0xAE220238, "sw v0,568(s1) - Remount: flagsC |= 0x08000000 (re-file the bike)"),
    (0x8001AED4, 0xA0820010, "sb v0,16(a0) - RiderSpeech: the provoked AI's grudge byte := 15"),
    (0x8001ACF4, 0xA6240366, "sh a0,870(s1) - RiderSpeech: the rate stamp +0x366"),
    (0x800C183C, 0xA082003D, "sb v0,61(a0) - ApplyHit: the combo nibble riderDef+0x3D"),
    (0x800C19C0, 0xA0A0000F, "sb zero,15(a1) - ApplyHit: health below zero -> 0"),
    (0x800C1A58, 0xA044000F, "sb a0,15(v0) - ApplyHit: the new health (0 included)"),
    (0x800BF7CC, 0xACA30228, "sw v1,552(a1) - KnockOff: the rider's +0x228 |= 0x8000 | side bit"),
    (0x800BF82C, 0xAC45008C, "sw a1,140(v0) - KnockOff: the dash flash for a player attacker"),
    (0x800BF654, 0xAD020234, "sw v0,564(t0) - HitShove: flagsB |= 0x01000000"),
    (0x800C00A0, 0xA062002E, "sb v0,46(v1) - WeaponSteal: the thief's weapon := the victim's"),
    (0x800C00FC, 0xA0620046, "sb v0,70(v1) - WeaponSteal: victim riderDef+0x46 := thief"),
    (0x800C0110, 0xA0620047, "sb v0,71(v1) - WeaponSteal: thief riderDef+0x47 := victim"),
    (0x800C0F34, 0xAD040234, "sw a0,564(t0) - FightPace: flagsB |= 0x200 when chasing a player"),
]

# the jump tables: (table, targets per index) - read out of RASHCDG
JT_HITSTANCE = (0x8005BA04, [0x800BF964, 0x800BF94C, 0x800BF94C, 0x800BF954, 0x800BF95C, 0x800BF964],
                [83, 85, 85, 86, 84, 83])     # the li v1,N in each target's delay slot
JT_PICKREC = [(0x8005BA1C, [0x800BF9D4] * 4 + [0x800BF9DC] * 2 + [0x800BF9E4]),
              (0x8005BA3C, [0x800BFA38] * 4 + [0x800BFA40] * 2 + [0x800BFA48]),
              (0x8005BA5C, [0x800BFAC4, 0x800BFB38, 0x800BFAFC, 0x800BFAFC, 0x800BFA8C, 0x800BFB38, 0x800BFB88,
                            0x800BFB88, 0x800BFB90])]
POSE_TICK_JT = 0x8005B69C                     # 0x8008F138: stance - 38 -> the pose initialiser call
POSE_CASE = {38: 0x8008F1D4, 39: 0x8008F1B0, 89: 0x8008F1B0, 40: 0x8008F1C4, 88: 0x8008F1C4, 42: 0x8008F1E8,
             91: 0x8008F1E8}
POSE_D = 0x8008F1FC                            # 41, 43..87, 90 and (through the bounds branch) everything else

# COP2 words (all, OP commands) per function; every other one of the 28 has none, and none has a jalr
COP2 = {0x80081D7C: (10, 1)}


def check_skeleton(bench, verbose=True):
    im = P.images()
    bench.check(im["SLUS"].sha1 == P.SHA1["SLUS"], "SLUS sha1")
    bench.check(im["G"].sha1 == P.SHA1["G"], "RASHCDG sha1")
    frames = {f: fr for f, fr, _, _ in load_models()}
    rows = ported()
    for f, (size, cs) in CALLEES.items():
        lo, hi = I.extent(f)
        bench.check(hi - lo == size, f"extent of {f:#x}: {hi - lo} bytes (claimed {size})")
        bench.check(I.frame_of(f) == frames.get(f), f"{f:#x}: frame {I.frame_of(f)} == the model's {frames.get(f)}")
        bench.check(dict(I.callees(f)) == cs, f"{f:#x}: callee multiset ({sum(cs.values())} jal, {len(cs)} targets)")
        want = set(SITES[f]) | ({0x800915A0} if MUTATE["on"] and f == 0x8001A760 else set())   # mutated: case A speaks
        bench.check(set(I.call_sites(f)) == want, f"{f:#x}: called from exactly {len(want)} sites")
    unp = cfg(UNPORTED, UNPORTED - {0x800235B0})       # mutated: the camera-target leaf SLUS 0x800235B0 claimed ported
    callees = {c for _, cs in CALLEES.values() for c in cs}
    wrong = sorted(c for c in callees if (c in rows) == (c in unp))
    bench.check(not wrong, f"ported status of {len(callees)} callees read from the row table: "
                           f"{len(callees) - len(unp & callees)} ported, {len(unp & callees)} not"
                           + (f"; WRONG {[hex(c) for c in wrong]}" if wrong else ""))
    fs = list(CALLEES)
    rs = {a for f in fs for a in I.jals_in(*I.extent(f)) if P.jal_target(a) == 0x8001FC58}
    cs = {a for f in fs for a in I.jals_in(*I.extent(f)) if P.jal_target(a) == 0x80043F00}
    bench.check(rs == RAND_SITES and cs == RCNT_SITES,
                f"Rand sites {sorted(hex(x) for x in rs)}, GetRCnt sites {sorted(hex(x) for x in cs)}")
    for a, w, what in WORDS:
        bench.check(word_at(a) == w, f"word at {a:#x}: {what}")
    t, tg, li = JT_HITSTANCE
    bench.check([word_at(t + 4 * i) for i in range(6)] == tg
                and [word_at(x + 4) for x in tg] == [0x24030000 | n for n in li],
                f"HitStance jump table {t:#x}: 6 cases -> stances {li}")
    for t, tg in JT_PICKREC:
        bench.check([word_at(t + 4 * i) for i in range(len(tg))] == tg,
                    f"FightPickRecord jump table {t:#x}: {len(tg)} cases")
    want = {st: POSE_CASE.get(st, POSE_D) for st in range(38, 92)}
    if MUTATE["on"]:
        want.update({st: 0 for st in range(43, 88)})    # mutated: 43..87 do nothing, as in RiderLaunch's table
    got = {38 + i: word_at(POSE_TICK_JT + 4 * i) for i in range(54)}
    bench.check(got == want, "0x8008F138 table 0x8005B69C: 38 A, 39/89 B, 40/88 C, 42/91 E, 41 / 43..87 / 90 D")
    census, jalr = {}, 0
    for f in fs:
        ws = [word_at(a) for a in range(*I.extent(f), 4)]
        c = sum(1 for w in ws if (w >> 26) in (0x12, 0x32, 0x3A))
        o = sum(1 for w in ws if w == 0x4B78000C)
        if c:
            census[f] = (c, o)
        jalr += sum(1 for w in ws if (w >> 26) == 0 and (w & 0x3F) == 9)
    bench.check(census == cfg(COP2, {0x80081D7C: (10, 10)}) and jalr == 0,
                f"COP2 census of {len(fs)} functions: {[(hex(k), v) for k, v in census.items()]} (words, OP), "
                f"jalr {jalr}")
    if verbose:
        print(f"  {len(CALLEES)} functions: extents, frames, callee multisets, call sites; {len(WORDS)} words, "
              f"5 jump tables, the Rand / GetRCnt sites, the COP2 census")


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["info", "live", "verify", "dev", "census"])
    ap.add_argument("--mutate", action="store_true")
    ap.add_argument("--only", default=None, help="comma-separated run names")
    ap.add_argument("--all", action="store_true", help="live: every run of crash_runs, not the verify subset")
    ap.add_argument("--jobs", type=int, default=8, help="runs traced in parallel (default 8)")
    ap.add_argument("--keep", action="store_true", help="keep the snapshot copies and traces under work/crash")
    ap.add_argument("--tag", default="probe", help="subdirectory of work/crash for this invocation")
    ap.add_argument("--cover", action="store_true", help="probe every block leader, print the never-run ones")
    ap.add_argument("--chunk", default="0/1", help="census: i/n, the i-th of n slices of all runs")
    ap.add_argument("--models", default=None, help="dev: a directory of m_XXXXXXXX.py models")
    ap.add_argument("--jt", default=None, help="dev: jump tables entry:table:count[,..] for --cover")
    ap.add_argument("--runs", default=None, help="dev: a python file defining RUNS = [...] (impact.py's spec form)")
    a = ap.parse_args(argv)
    MUTATE["on"] = a.mutate
    bench = P.Bench()
    only = set(a.only.split(",")) if a.only else None
    for spec_ in (a.jt.split(",") if a.jt else []):
        e_, t_, n_ = spec_.split(":")
        JT.setdefault(int(e_, 16), []).append((int(t_, 16), int(n_)))
    if a.cmd == "census":            # every run, per run: calls equal / total / equal under --mutate, blocks run
        import json
        i, n = (int(x) for x in a.chunk.split("/"))
        runs = list(all_run_specs().values())[i::n]
        cen = {}
        check_live(bench, load_models(), runs, a.tag, cover=True, census=cen, jobs=a.jobs)
        os.makedirs(WORK, exist_ok=True)
        json.dump(cen, open(os.path.join(WORK, f"census_{a.tag}.json"), "w"), indent=0)
        print(f"census: {bench.n} checks, {bench.fail} failures")
        return 0
    if a.cmd == "dev":
        runs = list(all_run_specs(a.runs).values()) if a.runs else verify_runs()
        if a.runs:
            spec = importlib.util.spec_from_file_location("crashdev_runs", a.runs)
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            runs = mod.RUNS
        mods = load_models(a.models)
        if a.models:
            fs = {int(os.path.basename(p)[2:10], 16) for p in glob.glob(os.path.join(a.models, "m_*.py"))}
            mods = [m for m in mods if m[0] in fs]
        check_live(bench, mods, runs, a.tag, only=only, keep=a.keep, cover=a.cover, jobs=a.jobs,
                   models_dir=a.models, runs_file=a.runs)
        print(f"dev: {bench.n} checks, {bench.fail} failures")
        return 0
    print("skeleton:")
    check_skeleton(bench)
    if a.cmd in ("live", "verify"):
        print("live:")
        runs = list(all_run_specs().values()) if a.all else verify_runs()
        seen, edits = check_live(bench, load_models(), runs, a.tag, only=only, keep=a.keep, facts=True,
                                 not_run=NOT_RUN, cover=a.cover, jobs=a.jobs)
        print("facts:")
        check_facts(bench, seen, edits)
    print(f"crash: {bench.n} checks, {bench.fail} failures")
    return 1 if (a.cmd == "verify" and bench.fail and not a.mutate) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
