#!/usr/bin/env python3
"""How a route crosses a junction - the independent check.

tools\\scout holds the SECOND implementation of everything; the authoritative parsers are the C++
ones under src\\rrformats. Assembling a race route naively leaves exactly one geometric gap per
junction, 590..1003 world units wide. The cause is structural, and this file checks it without using
any of the C++ code:

  1. A junction ships TWO objects, not one. They share a `pieceKey` and carry `half` 0 and 1 in the
     chunk header at +0x2C, and for both of them the header word at +0x28 is the NODE id. Only the
     half-0 object also names the node in its GRPT record; the half-1 object's GRPT names one of the
     ROADS that meet there. Filing junction objects by GRPT therefore files one half under a road id
     and loses it - and that half holds the stubs that reach the roads the route joins.

  2. A route does NOT cross a junction on one arm. The nine arms (six in half 0, three in half 1)
     form one connected polyline network, and the crossing is a CHAIN through it: road -> stub ->
     core arm -> core arm -> stub -> road. No single arm spans the gap, which is why every
     single-arm search stalled at a seam of roughly half the gap.

What this file measures, and each can fail on its own:

  * `halves`  - every junction has exactly one half-0 and one half-1 object, and no plain road piece
                carries a half.
  * `connect` - the arms of each junction form ONE connected component when endpoints closer than
                the tolerance are treated as joined. If they did not, no chain could exist.
  * `reach`   - for every ordered pair of roads meeting at a node, a chain of at most six arms links
                the end of one road's geometry to the start of the other's.

Usage:
    python tools\\scout\\junction.py verify [--set 1] [--tol 45]
    python tools\\scout\\junction.py show --node 16 [--set 1]
"""
import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import chunk as ck  # noqa: E402

DATA = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                    "work", "disc_us", "DATA")
FX = 65536.0


def data(name):
    return os.path.join(DATA, name)


def load_objects(setno):
    """Every distinct type-3 object of STREAM<n>.STR, with the four header words at +0x20."""
    out = {}
    for _, b in ck.iter_chunks(data("STREAM%d.STR" % setno)):
        if b[3] >> 4 != 3:
            continue
        key = ck.u32(b, 0) & 0x0FFFFFFF
        if key in out:
            continue
        o = ck.RoadObject(b)
        o.piece_key_w = ck.u32(b, 0x24)
        o.header_owner = ck.u32(b, 0x28)
        o.half_w = ck.u32(b, 0x2C)
        out[key] = o
    return out


def arms_of(objs, node):
    """Every run of both halves of one junction, as lists of world points."""
    out = []
    for o in objs.values():
        if o.half_w == 0xFFFFFFFF or o.header_owner != node:
            continue
        for run in o.runs():
            if len(run) >= 2:
                out.append([s.world() for s in run])
    return out


def road_geometry(objs, road):
    """The plain pieces of one road, ordered along it, as (start, end) world points."""
    ps = [o for o in objs.values() if o.half_w == 0xFFFFFFFF and o.grpt[3] == road]
    ps.sort(key=lambda o: o.grpt[6])
    if not ps:
        return None
    return ps[0].slices[0].world(), ps[-1].slices[-1].world()


def connected(arms, tol):
    """Union-find over arm endpoints: how many components the arm network falls into."""
    parent = list(range(2 * len(arms)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    def union(i, j):
        a, b = find(i), find(j)
        if a != b:
            parent[a] = b

    ends = []
    for i, arm in enumerate(arms):
        ends.append(arm[0])
        ends.append(arm[-1])
        union(2 * i, 2 * i + 1)
    for i in range(len(ends)):
        for j in range(i + 1, len(ends)):
            if math.dist(ends[i], ends[j]) <= tol:
                union(i, j)
    return len({find(i) for i in range(len(ends))})


def chain_exists(arms, start, goal, tol, depth=6):
    """Is there a chain of at most `depth` arms from `start` to `goal`, every seam within `tol`?"""
    used = [False] * len(arms)

    def walk(at, left):
        if math.dist(at, goal) <= tol:
            return True
        if left == 0:
            return False
        for i, arm in enumerate(arms):
            if used[i]:
                continue
            for entry, exit_ in ((arm[0], arm[-1]), (arm[-1], arm[0])):
                if math.dist(at, entry) > tol:
                    continue
                used[i] = True
                if walk(exit_, left - 1):
                    used[i] = False
                    return True
                used[i] = False
        return False

    return walk(start, depth)


def junction_nodes(objs):
    return sorted({o.header_owner for o in objs.values() if o.half_w != 0xFFFFFFFF})


def cmd_verify(args):
    objs = load_objects(args.set)
    tol = args.tol
    plain = [o for o in objs.values() if o.half_w == 0xFFFFFFFF]
    parts = [o for o in objs.values() if o.half_w != 0xFFFFFFFF]
    nodes = junction_nodes(objs)

    bad_halves = []
    for n in nodes:
        mine = [o for o in parts if o.header_owner == n]
        halves = sorted(o.half_w for o in mine)
        keys = {o.piece_key_w for o in mine}
        if halves != [0, 1] or len(keys) != 1:
            bad_halves.append((n, halves, sorted("%08X" % k for k in keys)))

    split, arms_seen = [], []
    for n in nodes:
        arms = arms_of(objs, n)
        arms_seen.append(len(arms))
        c = connected(arms, tol)
        if c != 1:
            split.append((n, len(arms), c))

    # `reach`: the half-0 object's GRPT owner is the node; the roads that meet there are the GRPT
    # owners of the half-1 object plus the roads whose own geometry ends within reach of the arms.
    unreachable = []
    for n in nodes:
        arms = arms_of(objs, n)
        ends = [p for arm in arms for p in (arm[0], arm[-1])]
        touching = []
        for o in plain:
            g = road_geometry(objs, o.grpt[3])
            if g is None:
                continue
            for p in g:
                if any(math.dist(p, e) <= tol for e in ends):
                    touching.append((o.grpt[3], p))
                    break
        seen = {}
        for road, p in touching:
            seen[road] = p
        roads = sorted(seen)
        for i, a in enumerate(roads):
            for b in roads[i + 1:]:
                if not chain_exists(arms, seen[a], seen[b], tol):
                    unreachable.append((n, a, b))

    print("set                       : %d" % args.set)
    print("type-3 objects            : %d" % len(objs))
    print("plain road pieces         : %d" % len(plain))
    print("junction halves           : %d over %d nodes" % (len(parts), len(nodes)))
    print("arms per junction         : min %d max %d" % (min(arms_seen), max(arms_seen)))
    print("nodes with bad halves     : %d" % len(bad_halves))
    print("junctions in >1 component : %d" % len(split))
    print("road pairs with no chain  : %d" % len(unreachable))
    for row in (bad_halves + split + unreachable)[:10]:
        print("   %s" % (row,))
    ok = not bad_halves and not split and not unreachable
    print("VERIFY %s" % ("OK" if ok else "FAILED"))
    return 0 if ok else 1


def cmd_show(args):
    objs = load_objects(args.set)
    n = args.node
    for o in sorted(objs.values(), key=lambda o: o.half_w):
        if o.half_w == 0xFFFFFFFF or o.header_owner != n:
            continue
        print("half %d  pieceKey %08X  GRPT owner %d  along %.1f..%.1f  %d slices" % (
            o.half_w, o.piece_key_w, o.grpt[3], o.grpt[6] / FX, o.grpt[7] / FX, len(o.slices)))
        for j, run in enumerate(o.runs()):
            a, b = run[0].world(), run[-1].world()
            L = sum(math.dist(p.world(), q.world()) for p, q in zip(run, run[1:]))
            print("   arm %d  %3d slices  %6.0f units   (%7.0f,%7.0f) -> (%7.0f,%7.0f)"
                  % (j, len(run), L, a[0], a[2], b[0], b[2]))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["verify", "show"])
    ap.add_argument("--set", type=int, default=1)
    ap.add_argument("--node", type=int, default=16)
    ap.add_argument("--tol", type=float, default=45.0)
    args = ap.parse_args()
    return {"verify": cmd_verify, "show": cmd_show}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
