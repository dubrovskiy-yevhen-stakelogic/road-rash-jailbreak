#!/usr/bin/env python3
"""Scout probe for the Road Rash: Jailbreak road/track data.

Throwaway research tool (see docs/formats/road.md). Covers:
  * ROADGRF<n>.TXT  - the plain-text race graph shipped on the disc
  * RGTS (.GRF)     - road network: intersections + roads (+ PMTS sub-block)
  * COTS (.TOC)     - stream table of contents -> byte ranges in STREAM<n>.STR
  * CTLR (.RLS)     - stream release/cue lists (partially understood)
  * MAP_ (.MAP)     - map container: BTT_/BST_/BIT_/IPT_/PDT_/GPDT
  * MRPS (.STP)     - per-race pack of 16 KiB stream chunks

Usage:
  road.py graph  <ROADGRF1.TXT> [-o out.json]
  road.py info   <file>
  road.py scan   <DATA dir> [-o out.txt] [--chunks]
  road.py verify <DATA dir> [--deep]
  road.py plot   <DATA dir> <race-set 1|2> <raceid> <out.svg>

No game bytes are written into the repository: point -o at work\\road\\.
"""

import argparse
import hashlib
import json
import math
import os
import re
import struct
import sys
from collections import defaultdict, OrderedDict

CHUNK = 0x4000          # stream granularity, proven in verify()
U32 = "<I"


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def i32(b, o):
    return struct.unpack_from("<i", b, o)[0]


# --------------------------------------------------------------------------
# ROADGRF<n>.TXT
# --------------------------------------------------------------------------

# Field names follow the layout the game's own parser builds (overlay
# RASHCDI.BIN, sha1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06, base 0x8005B5E8,
# parser at 0x8006A0C8 - see docs/formats/road.md).
INT_FIELDS = ("node", "dist_to_finish", "node_span",
              "link_road", "link_dir",          # x4, interleaved
              "route_road",                     # x4, packed
              "next_node")                      # x4, positional


class Race(object):
    __slots__ = ("raceid", "start", "finish", "gmagic", "rmagic",
                 "start_checker", "finish_checker", "ints")


def _ints(s):
    return [int(x) for x in s.replace(",", " ").split()]


def parse_graph_text(path):
    """Parse ROADGRF<n>.TXT exactly the way the game's parser reads it."""
    txt = open(path, "rb").read().decode("ascii", "replace")
    lines = [l.strip() for l in txt.splitlines()]
    num_entries = None
    races = []
    cur = None
    pending_ints = 0
    for ln in lines:
        if not ln:
            continue
        if ln.startswith("[NUM_ENTRIES]"):
            num_entries = int(ln.split("=", 1)[1])
            continue
        if ln == "[BEGIN]":
            cur = Race()
            cur.raceid = None
            cur.start = cur.finish = None
            cur.gmagic = cur.rmagic = None
            cur.start_checker = cur.finish_checker = None
            cur.ints = []
            pending_ints = 0
            continue
        if ln == "[END]":
            races.append(cur)
            cur = None
            continue
        if cur is None:
            continue
        if ln.startswith("["):
            key, _, val = ln.partition("=")
            key = key.strip()
            val = val.strip()
            if key == "[RACEID]":
                cur.raceid = int(val)
            elif key == "[START]":
                cur.start = _ints(val)
            elif key == "[FINISH]":
                cur.finish = _ints(val)
            elif key == "[GMAGIC]":
                cur.gmagic = int(val)
            elif key == "[RMAGIC]":
                cur.rmagic = int(val)
            elif key == "[START_CHECKER]":
                cur.start_checker = _ints(val)
            elif key == "[FINISH_CHECKER]":
                cur.finish_checker = _ints(val)
            elif key == "[RACEINTS]":
                pending_ints = int(val)
            else:
                raise ValueError("unknown key %s in %s" % (key, path))
            continue
        if pending_ints > 0:
            row = _ints(ln)
            if len(row) != 19:
                raise ValueError("%s: intersection row with %d ints, expected 19"
                                 % (path, len(row)))
            cur.ints.append(row)
            pending_ints -= 1
            continue
        raise ValueError("%s: stray line %r" % (path, ln))
    if num_entries is not None and num_entries != len(races):
        raise ValueError("%s: NUM_ENTRIES=%d but %d blocks"
                         % (path, num_entries, len(races)))
    return races


def decode_row(row):
    """Split one 19-int intersection row into the fields the parser stores."""
    links = []
    for i in range(4):
        r, d = row[3 + 2 * i], row[4 + 2 * i]
        if r != -1:                       # parser packs, skipping -1
            links.append({"road": r, "dir": d})
    route = [v for v in row[11:15] if v != -1]      # packed, count at +0x10
    nxt = [(i, v) for i, v in enumerate(row[15:19]) if v != -1]  # positional
    return OrderedDict([
        ("node", row[0]),
        ("dist_to_finish", row[1]),
        ("node_span", row[2]),
        ("links", links),
        ("route_roads", route),
        ("next_nodes", [{"slot": i, "node": v} for i, v in nxt]),
        ("raw", row),
    ])


def race_to_dict(r):
    def ep(v):
        return OrderedDict([("road", v[0]), ("dist", v[1]),
                            ("dir", v[2]), ("node_row", v[3])])
    return OrderedDict([
        ("raceid", r.raceid),
        ("gmagic", r.gmagic),
        ("rmagic", r.rmagic),
        ("start", ep(r.start)),
        ("finish", ep(r.finish)),
        ("start_checker", r.start_checker),
        ("finish_checker", r.finish_checker),
        ("raceints", len(r.ints)),
        ("intersections", [decode_row(x) for x in r.ints]),
    ])


# --------------------------------------------------------------------------
# RGTS / PMTS
# --------------------------------------------------------------------------

class RoadGraph(object):
    def __init__(self, path):
        d = open(path, "rb").read()
        self.path = path
        self.raw = d
        assert d[:4] == b"RGTS", d[:4]
        (self.block_size, self.gmagic, self.n_nodes, self.n_roads,
         self.off_nodes, self.off_roads) = struct.unpack_from("<6I", d, 4)
        self.nodes = []
        for i in range(self.n_nodes):
            w = struct.unpack_from("<10i", d, self.off_nodes + i * 40)
            n = w[1]
            self.nodes.append({"id": w[0], "nlinks": n,
                               "links": [{"road": w[2 + 2 * k],
                                          "dir": w[3 + 2 * k]} for k in range(n)]})
        self.roads = []
        for i in range(self.n_roads):
            w = struct.unpack_from("<4i", d, self.off_roads + i * 16)
            self.roads.append({"id": w[0], "length": w[1], "a": w[2], "b": w[3]})
        # PMTS sub-block
        self.pmts = None
        if self.block_size + 8 <= len(d) and d[self.block_size:self.block_size + 4] == b"PMTS":
            o = self.block_size
            (sz, gm, na, nb, _z0, _z1, offa, offb, end) = struct.unpack_from("<9I", d, o + 4)
            self.pmts = {"off": o, "size": sz, "gmagic": gm,
                         "nodes": [], "pieces": []}
            for i in range(na):
                k, n = struct.unpack_from("<2I", d, o + offa + i * 8)
                self.pmts["nodes"].append({"key": k, "node": n})
            for i in range(nb):
                k, road, start, flag, ln = struct.unpack_from("<5i", d, o + offb + i * 20)
                self.pmts["pieces"].append({"key": k & 0xFFFFFFFF, "road": road,
                                            "start": start, "flag": flag,
                                            "length": ln})

    def road(self, i):
        return self.roads[i]

    def node(self, i):
        return self.nodes[i]


# --------------------------------------------------------------------------
# COTS
# --------------------------------------------------------------------------

class StreamToc(object):
    def __init__(self, path):
        d = open(path, "rb").read()
        self.path = path
        assert d[:4] == b"COTS", d[:4]
        (self.block_size, self.gmagic, self.n_roads, self.n_nodes,
         self.off_roads, self.off_nodes) = struct.unpack_from("<6I", d, 4)
        self.nodes = []
        for i in range(self.n_nodes):
            w = struct.unpack_from("<17i", d, self.off_nodes + i * 68)
            slots = [{"road": w[1 + 3 * k], "off": w[2 + 3 * k] & 0xFFFFFFFF,
                      "size": w[3 + 3 * k]} for k in range(4)]
            extra = [{"off": w[13 + 2 * k] & 0xFFFFFFFF, "size": w[14 + 2 * k]}
                     for k in range(2)]
            self.nodes.append({"node": w[0], "slots": slots, "extra": extra})
        self.roads = []
        for i in range(self.n_roads):
            rid, o0, s0, o1, s1 = struct.unpack_from("<5i", d, self.off_roads + i * 20)
            self.roads.append({"road": rid,
                               "fwd": (o0 & 0xFFFFFFFF, s0),
                               "rev": (o1 & 0xFFFFFFFF, s1)})


# --------------------------------------------------------------------------
# MRPS (.STP)
# --------------------------------------------------------------------------

def mrps_header(path):
    d = open(path, "rb").read(0x30)
    if d[:4] != b"MRPS":
        raise ValueError("%s: not MRPS" % path)
    w = struct.unpack_from("<11I", d, 4)
    return OrderedDict([
        ("hdr_size", w[0]), ("gmagic", w[1]), ("raceid", w[2]),
        ("rmagic", w[3]), ("data_off", w[4]), ("data_size", w[5]),
        ("n_chunks", w[6]), ("zero", w[7]), ("field_0x24", w[8]),
        ("file_size", os.path.getsize(path)),
    ])


def chunk_header(b, o):
    """Common 16 KiB chunk header: 5 u16 fields + u16 list ended by 0xFFFE."""
    f = list(struct.unpack_from("<5H", b, o))
    lst = []
    i = o + 10
    term = False
    while i < o + 0x80:
        v = struct.unpack_from("<H", b, i)[0]
        i += 2
        if v == 0xFFFE:
            term = True
            break
        lst.append(v)
    return {"fields": f, "list": [x for x in lst if x != 0xFFFF],
            "list_len": len(lst), "body": i - o + 2, "terminated": term}


# --------------------------------------------------------------------------
# MAP_
# --------------------------------------------------------------------------

def map_blocks(path):
    d = open(path, "rb").read()
    out = []
    o = 0
    while o + 8 <= len(d):
        tag = d[o:o + 4]
        sz = u32(d, o + 4)
        if sz < 8 or o + sz > len(d):
            out.append((o, tag, sz, "BAD"))
            break
        out.append((o, tag, sz, ""))
        o += sz
    return d, out


def btt_records(d, off, size):
    n = (size - 8) // 32
    return [struct.unpack_from("<3I 2i 2I i", d, off + 8 + i * 32) for i in range(n)], n


# --------------------------------------------------------------------------
# commands
# --------------------------------------------------------------------------

def cmd_graph(args):
    races = parse_graph_text(args.file)
    # if the matching RGTS sits next to the text file, resolve the route too
    rg = None
    m = re.search(r"(\d)\.TXT$", args.file.upper())
    if m:
        cand = os.path.join(os.path.dirname(args.file) or ".",
                            "STREAM%s.GRF" % m.group(1))
        if os.path.exists(cand):
            rg = RoadGraph(cand)
    doc = OrderedDict([
        ("source", os.path.basename(args.file)),
        ("num_entries", len(races)),
        ("field_meaning", {
            "start/finish": "road, distance-along-road, travel dir (+/-1), "
                            "index of the matching intersection row",
            "int_row": "node, dist_to_finish, node_span, 4x(link_road,link_dir), "
                       "4x route_road (packed), 4x next_node (positional)",
        }),
        ("races", [race_to_dict(r) for r in races]),
    ])
    if rg is not None:
        for r, jd in zip(races, doc["races"]):
            w = walk_route(r, rg)
            total = None
            if not w["error"]:
                first = w["rows"][0]
                sr = rg.roads[r.start[0]]
                L = _road_len_units(rg, r.start[0])
                run_in = (L - r.start[1]) if r.start[2] > 0 else r.start[1]
                total = run_in + first["dist_to_finish"]
            jd["route"] = OrderedDict([
                ("nodes", [d["node"] for d in w["rows"]]),
                ("roads", w["roads"]),
                ("error", w["error"]),
                ("run_in_from_start", None if w["error"] else run_in),
                ("total_distance", total),
            ])
    txt = json.dumps(doc, indent=1)
    if args.out:
        open(args.out, "w").write(txt)
        print("wrote %s (%d races)" % (args.out, len(races)))
    else:
        print(txt)


def cmd_info(args):
    p = args.file
    d = open(p, "rb").read(16)
    tag = d[:4]
    print("%s  %d bytes  tag=%r" % (p, os.path.getsize(p), tag))
    if tag == b"MRPS":
        h = mrps_header(p)
        for k, v in h.items():
            print("  %-12s 0x%X (%d)" % (k, v, v))
        ok = h["file_size"] == h["data_off"] + h["data_size"]
        print("  size check: %s   chunks: %d x 0x%X = 0x%X  %s"
              % ("ok" if ok else "MISMATCH", h["n_chunks"], CHUNK,
                 h["n_chunks"] * CHUNK,
                 "ok" if h["n_chunks"] * CHUNK == h["data_size"] else "MISMATCH"))
        raw = open(p, "rb").read()
        for c in range(min(h["n_chunks"], args.limit or 8)):
            o = h["data_off"] + c * CHUNK
            ch = chunk_header(raw, o)
            print("   chunk %2d @0x%06X  fields=%s list=%s%s" %
                  (c, o, ["%04X" % x for x in ch["fields"]],
                   ["%04X" % x for x in ch["list"][:12]],
                   "" if ch["terminated"] else "  (no 0xFFFE terminator)"))
    elif tag == b"RGTS":
        g = RoadGraph(p)
        print("  block_size 0x%X gmagic 0x%08X nodes %d @0x%X roads %d @0x%X"
              % (g.block_size, g.gmagic, g.n_nodes, g.off_nodes,
                 g.n_roads, g.off_roads))
        for n in g.nodes:
            print("   N%02d links=%d %s" % (n["id"], n["nlinks"],
                  " ".join("r%d%+d" % (l["road"], l["dir"]) for l in n["links"])))
        for r in g.roads:
            print("   R%02d len=%-7d %d->%d  (len>>6=%d)"
                  % (r["id"], r["length"], r["a"], r["b"], r["length"] >> 6))
        if g.pmts:
            print("  PMTS @0x%X size 0x%X: %d node keys, %d road pieces"
                  % (g.pmts["off"], g.pmts["size"], len(g.pmts["nodes"]),
                     len(g.pmts["pieces"])))
    elif tag == b"COTS":
        t = StreamToc(p)
        print("  block_size 0x%X gmagic 0x%08X roads %d @0x%X nodes %d @0x%X"
              % (t.block_size, t.gmagic, t.n_roads, t.off_roads,
                 t.n_nodes, t.off_nodes))
        for r in t.roads:
            print("   entry road %2d  fwd 0x%08X+0x%X  rev 0x%08X+0x%X"
                  % (r["road"], r["fwd"][0], r["fwd"][1], r["rev"][0], r["rev"][1]))
    elif tag == b"CTLR":
        raw = open(p, "rb").read()
        bs = u32(raw, 4)
        n = (bs - 8) // 4
        offs = [u32(raw, 8 + i * 4) for i in range(n)] + [len(raw)]
        print("  block_size 0x%X -> %d u32 offsets (0x%X .. 0x%X)"
              % (bs, n, offs[0], offs[-2]))
        print("  sub-block at 0x%X: %r size 0x%X -> ends at 0x%X (file 0x%X)"
              % (bs, raw[bs:bs + 4], u32(raw, bs + 4), bs + u32(raw, bs + 4), len(raw)))
        recs = [(offs[i + 1] - offs[i]) for i in range(n)]
        print("  records of 32 B per entry: %s (total %d, remainders %s)"
              % ([x // 32 for x in recs[:12]], sum(recs) // 32,
                 sorted(set(x % 32 for x in recs))))
        for i in range(min(n, args.limit or 2)):
            o = offs[i]
            print("   entry %d @0x%X: %s" % (i, o, ["%04X" % x for x in
                  struct.unpack_from("<16H", raw, o)]))
    elif tag == b"MAP_":
        d, blocks = map_blocks(p)
        hdr = struct.unpack_from("<7I", d, 4)
        print("  block_size 0x%X gmagic 0x%08X counts %d/%d rest %s"
              % (hdr[0], hdr[1], hdr[2], hdr[3], ["0x%X" % x for x in hdr[4:]]))
        for o, t, sz, bad in blocks:
            print("   0x%05X %r size 0x%X %s" % (o, t, sz, bad))
        for o, t, sz, bad in blocks:
            if t == b"BTT_":
                recs, n = btt_records(d, o, sz)
                print("  BTT_: %d records x 32 B starting at 0x%X" % (n, o + 8))
                for r in recs[:args.limit or 6]:
                    print("    seq=%-3d flags=0x%08X key=0x%08X z=%d idx=%-3d "
                          "a=0x%08X b=0x%08X tail=%d" % r)
    else:
        print("  (no parser; first 32 bytes) %s" % open(p, "rb").read(32).hex(" "))


def cmd_scan(args):
    data = args.dir
    out = []
    w = out.append
    files = sorted([f for f in os.listdir(data) if f.upper().endswith(".STP")],
                   key=lambda f: (f.split("_")[0], int(re.search(r"_(\d+)", f).group(1))))
    graphs = {}
    for n in (1, 2):
        gp = os.path.join(data, "ROADGRF%d.TXT" % n)
        if os.path.exists(gp):
            graphs[n] = {r.raceid: r for r in parse_graph_text(gp)}
    w("MRPS scan of %s (%d files)" % (data, len(files)))
    w("%-14s %9s %10s %6s %8s %5s %10s %9s %s"
      % ("file", "size", "gmagic", "race", "datasize", "nchk", "+0x24", "rmagic", "txt"))
    bad = 0
    for f in files:
        p = os.path.join(data, f)
        h = mrps_header(p)
        setno = 1 if f.upper().startswith("RACE1") else 2
        txt = ""
        r = graphs.get(setno, {}).get(h["raceid"])
        if r is None:
            txt = "no-race"
        else:
            txt = ("rmagic-ok" if r.rmagic == h["rmagic"] else "RMAGIC-MISMATCH")
            if r.gmagic != h["gmagic"]:
                txt += " GMAGIC-MISMATCH"
        oksize = (h["file_size"] == h["data_off"] + h["data_size"]
                  and h["n_chunks"] * CHUNK == h["data_size"])
        if not oksize or "MISMATCH" in txt:
            bad += 1
        w("%-14s %9d 0x%08X %6d %8X %5d 0x%08X %9d %s%s"
          % (f, h["file_size"], h["gmagic"], h["raceid"], h["data_size"],
             h["n_chunks"], h["field_0x24"], h["rmagic"], txt,
             "" if oksize else "  SIZE-MISMATCH"))
        if args.chunks:
            raw = open(p, "rb").read()
            if r is not None:
                w("    [START]=%s  [FINISH]=%s" % (r.start, r.finish))
            for c in range(h["n_chunks"]):
                ch = chunk_header(raw, h["data_off"] + c * CHUNK)
                w("    chunk %2d  fields=%s  list=%s%s"
                  % (c, " ".join("%04X" % x for x in ch["fields"]),
                     " ".join("%04X" % x for x in ch["list"][:12]),
                     "" if ch["terminated"] else "  (no 0xFFFE)"))
    w("")
    w("problems: %d" % bad)
    txt = "\n".join(out)
    if args.out:
        open(args.out, "w").write(txt + "\n")
        print("wrote %s" % args.out)
    else:
        print(txt)


def _road_len_units(rg, rid):
    """Length of a road in the unit the text graph uses (game does len>>6)."""
    return rg.roads[rid]["length"] >> 6


INT_MAX = 0x7FFFFFFF


def walk_route(race, rg):
    """Follow [START].node -> next_node -> ... -> [FINISH].node.

    The intersection rows are NOT stored in route order and may contain rows
    that are not on the route at all (those carry dist_to_finish = INT_MAX).
    Returns the ordered rows plus the per-step distance identity.
    """
    rows = {}
    for raw in race.ints:
        d = decode_row(raw)
        rows[d["node"]] = d
    res = {"rows": [], "steps": [], "error": None, "roads": []}
    if race.start[3] == -1 or not rows:
        res["error"] = "no-start"
        return res
    cur = race.start[3]
    guard = 0
    while True:
        guard += 1
        if guard > 64:
            res["error"] = "cycle"
            return res
        d = rows.get(cur)
        if d is None:
            res["error"] = "node %d not in rows" % cur
            return res
        res["rows"].append(d)
        if cur == race.finish[3]:
            rid = race.finish[0]
            L = _road_len_units(rg, rid)
            travel = race.finish[1] if race.finish[2] > 0 else L - race.finish[1]
            res["steps"].append({"node": cur, "road": rid, "dir": race.finish[2],
                                 "want": d["node_span"] + travel,
                                 "got": d["dist_to_finish"], "final": True})
            res["roads"].append(rid)
            return res
        if not d["route_roads"]:
            res["error"] = "dead end at node %d" % cur
            return res
        rid = d["route_roads"][0]
        slot = [s for s in d["next_nodes"]
                if s["slot"] < len(d["links"]) and d["links"][s["slot"]]["road"] == rid]
        if not slot:
            res["error"] = "no next_node slot for route road %d at node %d" % (rid, cur)
            return res
        nxt = slot[0]["node"]
        if nxt not in rows:
            res["error"] = "next node %d missing" % nxt
            return res
        res["steps"].append({"node": cur, "road": rid,
                             "dir": d["links"][slot[0]["slot"]]["dir"],
                             "want": (d["node_span"] + _road_len_units(rg, rid)
                                      + rows[nxt]["dist_to_finish"]),
                             "got": d["dist_to_finish"], "final": False})
        res["roads"].append(rid)
        cur = nxt


def verify_set(data, setno, deep=False, log=print):
    ok = True
    grf = os.path.join(data, "ROADGRF%d.TXT" % setno)
    rgts = os.path.join(data, "STREAM%d.GRF" % setno)
    cots = os.path.join(data, "STREAM%d.TOC" % setno)
    strf = os.path.join(data, "STREAM%d.STR" % setno)
    races = parse_graph_text(grf)
    rg = RoadGraph(rgts)
    toc = StreamToc(cots)
    log("== set %d: %d races, %d nodes, %d roads" %
        (setno, len(races), rg.n_nodes, rg.n_roads))

    # 1. magics
    if rg.gmagic != toc.gmagic or any(r.gmagic != rg.gmagic for r in races):
        log("  FAIL gmagic mismatch"); ok = False
    else:
        log("  ok   GMAGIC 0x%08X identical in .TXT/.GRF/.TOC" % rg.gmagic)

    # 2. node link tables in RGTS vs road endpoints
    bad = 0
    for n in rg.nodes:
        for l in n["links"]:
            r = rg.roads[l["road"]]
            want = 1 if r["a"] == n["id"] else (-1 if r["b"] == n["id"] else 0)
            if want != l["dir"]:
                bad += 1
    log("  %s RGTS link dir == 'outgoing' (+1 at road.a, -1 at road.b): %d bad"
        % ("ok  " if bad == 0 else "FAIL", bad))
    ok &= (bad == 0)

    # 3. text rows vs RGTS adjacency + dir convention
    bad_set = bad_dir = bad_next = 0
    for r in races:
        for row in r.ints:
            d = decode_row(row)
            n = rg.nodes[d["node"]]
            if sorted(l["road"] for l in d["links"]) != sorted(l["road"] for l in n["links"]):
                bad_set += 1
            route = set(d["route_roads"])
            for l in d["links"]:
                out_dir = next(x["dir"] for x in n["links"] if x["road"] == l["road"])
                want = out_dir if l["road"] in route else -out_dir
                if want != l["dir"]:
                    bad_dir += 1
            for slot in d["next_nodes"]:
                if slot["slot"] >= len(d["links"]):
                    bad_next += 1
                    continue
                l = d["links"][slot["slot"]]
                rd = rg.roads[l["road"]]
                arrive = rd["b"] if l["dir"] > 0 else rd["a"]
                if arrive != slot["node"]:
                    bad_next += 1
    log("  %s text row road set == RGTS node links: %d bad" %
        ("ok  " if bad_set == 0 else "FAIL", bad_set))
    log("  %s text dir = outgoing for route road, incoming otherwise: %d bad" %
        ("ok  " if bad_dir == 0 else "FAIL", bad_dir))
    log("  %s next_node[slot] == far end of link[slot]: %d bad" %
        ("ok  " if bad_next == 0 else "FAIL", bad_next))
    ok &= (bad_set == 0 and bad_dir == 0 and bad_next == 0)

    # 4. route chain and distance arithmetic
    bad_d = bad_chain = bad_start = checked = skipped = 0
    for r in races:
        route = walk_route(r, rg)
        if route["error"]:
            if route["error"] == "no-start":
                skipped += 1
            else:
                bad_chain += 1
                log("       chain problem in race %d: %s" % (r.raceid, route["error"]))
            continue
        # [START].road must be a link of the first node, listed with the
        # incoming travel direction == [START].dir
        first = route["rows"][0]
        link = next((l for l in first["links"] if l["road"] == r.start[0]), None)
        if link is None or link["dir"] != r.start[2]:
            bad_start += 1
        for step in route["steps"]:
            checked += 1
            if step["want"] != step["got"]:
                bad_d += 1
    log("  %s route chain [START].node -(next_node)-> ... -> [FINISH].node closes"
        " (%d races, %d without intersections, %d broken)"
        % ("ok  " if bad_chain == 0 else "FAIL", len(races), skipped, bad_chain))
    log("  %s [START].road is a link of the first node with dir == [START].dir:"
        " %d bad" % ("ok  " if bad_start == 0 else "FAIL", bad_start))
    log("  %s dist_to_finish == node_span + (road.len>>6) + dist_to_finish(next),"
        " finish node uses the run-out to [FINISH] (%d checks, %d bad)"
        % ("ok  " if bad_d == 0 else "FAIL", checked, bad_d))
    ok &= (bad_d == 0 and bad_chain == 0 and bad_start == 0)

    # 5. COTS tiling of the .STR
    size = os.path.getsize(strf)
    pos = 0
    bad_t = 0
    for e in toc.roads:
        for key in ("fwd", "rev"):
            o, s = e[key]
            if o != pos or s % CHUNK or o % CHUNK:
                bad_t += 1
            pos = o + s
    log("  %s COTS tiles %s exactly: end 0x%X vs file 0x%X, %d gaps, all 0x%X-aligned"
        % ("ok  " if (bad_t == 0 and pos == size) else "FAIL",
           os.path.basename(strf), pos, size, bad_t, CHUNK))
    ok &= (bad_t == 0 and pos == size)
    eof_marker = size
    nulls = sum(1 for n in toc.nodes for s in n["slots"]
                if s["off"] == eof_marker and s["size"] == 0)
    log("  info COTS node slots pointing at EOF marker 0x%X (unused): %d of %d"
        % (eof_marker, nulls, 4 * len(toc.nodes)))

    # 6. COTS node slot road ids == RGTS node links
    bad_n = 0
    for n in toc.nodes:
        rgn = rg.nodes[n["node"]]
        got = [s["road"] for s in n["slots"] if s["road"] != -1]
        if got != [l["road"] for l in rgn["links"]]:
            bad_n += 1
    log("  %s COTS node slot road ids == RGTS node links in order: %d bad"
        % ("ok  " if bad_n == 0 else "FAIL", bad_n))
    ok &= (bad_n == 0)

    # 7. MRPS headers
    stps = sorted([f for f in os.listdir(data)
                   if re.match(r"RACE%d_\d+\.STP$" % setno, f.upper())],
                  key=lambda f: int(re.search(r"_(\d+)", f).group(1)))
    byid = {r.raceid: r for r in races}
    bad_m = 0
    for f in stps:
        h = mrps_header(os.path.join(data, f))
        rid = int(re.search(r"_(\d+)", f).group(1))
        if (h["raceid"] != rid or h["gmagic"] != rg.gmagic
                or rid not in byid or byid[rid].rmagic != h["rmagic"]
                or h["n_chunks"] * CHUNK != h["data_size"]
                or h["file_size"] != h["data_off"] + h["data_size"]):
            bad_m += 1
    log("  %s %d .STP: +0x0C == file number, GMAGIC/RMAGIC match the text, "
        "size == 0x800 + n*0x4000: %d bad"
        % ("ok  " if bad_m == 0 else "FAIL", len(stps), bad_m))
    ok &= (bad_m == 0)

    # 8. deep: are .STP chunks byte-identical copies of .STR chunks?
    if deep:
        idx = defaultdict(list)
        with open(strf, "rb") as fh:
            i = 0
            while True:
                b = fh.read(CHUNK)
                if len(b) < CHUNK:
                    break
                idx[hashlib.md5(b).digest()].append(i)
                i += 1
        tot = hit = 0
        for f in stps:
            d = open(os.path.join(data, f), "rb").read()
            h = mrps_header(os.path.join(data, f))
            for c in range(h["n_chunks"]):
                b = d[h["data_off"] + c * CHUNK: h["data_off"] + (c + 1) * CHUNK]
                tot += 1
                if hashlib.md5(b).digest() in idx:
                    hit += 1
        log("  %s every .STP chunk is byte-identical to a chunk of %s: %d/%d"
            % ("ok  " if hit == tot else "FAIL", os.path.basename(strf), hit, tot))
        ok &= (hit == tot)
    return ok


def cmd_verify(args):
    allok = True
    for setno in (1, 2):
        allok &= verify_set(args.dir, setno, deep=args.deep)
        print("")
    print("ALL CHECKS PASSED" if allok else "THERE WERE FAILURES")
    return 0 if allok else 1


# --------------------------------------------------------------------------
# plot: schematic of the network + one race route
# --------------------------------------------------------------------------

def layout(rg, iterations=4000):
    """Deterministic spring layout of the road network - a SCHEMATIC.

    No XY coordinates for nodes have been found in the shipped data yet, and
    RGTS road lengths are path lengths of curved roads, so they do not embed
    in the plane at all (cycles violate the triangle inequality).  Rest length
    is therefore uniform; the real lengths are printed as edge labels.
    """
    n = rg.n_nodes
    rnd = 12345

    def rand():
        nonlocal rnd
        rnd = (rnd * 1103515245 + 12345) & 0x7FFFFFFF
        return rnd / 0x7FFFFFFF
    pos = [[math.cos(2 * math.pi * i / n) * 3000 + rand() * 200,
            math.sin(2 * math.pi * i / n) * 3000 + rand() * 200] for i in range(n)]
    edges = [(r["a"], r["b"], 2200.0) for r in rg.roads]
    for it in range(iterations):
        # weak repulsion first (only early on, to unfold the graph)
        if it < iterations // 2:
            w = 1.0 - 2.0 * it / float(iterations)
            for i in range(n):
                for j in range(i + 1, n):
                    dx = pos[j][0] - pos[i][0]
                    dy = pos[j][1] - pos[i][1]
                    d = math.hypot(dx, dy) or 1e-6
                    if d > 2500:
                        continue
                    f = (2500 - d) * 0.05 * w / d
                    pos[i][0] -= dx * f; pos[i][1] -= dy * f
                    pos[j][0] += dx * f; pos[j][1] += dy * f
        # distance constraints (Verlet style relaxation, exact at convergence)
        for a, b, L in edges:
            dx = pos[b][0] - pos[a][0]
            dy = pos[b][1] - pos[a][1]
            d = math.hypot(dx, dy) or 1e-6
            f = (d - L) / d * 0.25
            pos[a][0] += dx * f; pos[a][1] += dy * f
            pos[b][0] -= dx * f; pos[b][1] -= dy * f
    return pos


def race_route(race):
    """(road, dir) sequence of the race, from [START] through the rows."""
    seq = [{"road": race.start[0], "dir": race.start[2],
            "from_dist": race.start[1], "node": None}]
    for row in race.ints:
        d = decode_row(row)
        if not d["route_roads"]:
            continue
        rid = d["route_roads"][0]
        link = next((l for l in d["links"] if l["road"] == rid), None)
        seq.append({"road": rid, "dir": link["dir"] if link else None,
                    "node": d["node"]})
    return seq


def cmd_plot(args):
    data, setno, rid = args.dir, args.set, args.raceid
    rg = RoadGraph(os.path.join(data, "STREAM%d.GRF" % setno))
    races = {r.raceid: r for r in parse_graph_text(
        os.path.join(data, "ROADGRF%d.TXT" % setno))}
    race = races[rid]
    pos = layout(rg)
    xs = [p[0] for p in pos]; ys = [p[1] for p in pos]
    pad = 400
    minx, maxx, miny, maxy = min(xs) - pad, max(xs) + pad, min(ys) - pad, max(ys) + pad
    W, H = 1000.0, 1000.0 * (maxy - miny) / (maxx - minx)

    def X(v): return (v - minx) / (maxx - minx) * W

    def Y(v): return (v - miny) / (maxy - miny) * H
    route = race_route(race)
    used = set(s["road"] for s in route)
    walk = walk_route(race, rg)
    STRIP = 120.0
    H += STRIP
    out = []
    out.append('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
               'viewBox="0 0 %d %d">' % (W, H, W, H))
    out.append('<rect width="100%" height="100%" fill="#12141a"/>')
    for r in rg.roads:
        a, b = pos[r["a"]], pos[r["b"]]
        hot = r["id"] in used
        out.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="%s" '
                   'stroke-width="%s" stroke-linecap="round"/>'
                   % (X(a[0]), Y(a[1]), X(b[0]), Y(b[1]),
                      "#ff9c3f" if hot else "#3b4252", "7" if hot else "3"))
        mx, my = (X(a[0]) + X(b[0])) / 2, (Y(a[1]) + Y(b[1])) / 2
        out.append('<text x="%.1f" y="%.1f" fill="#6b7280" font-size="11" '
                   'font-family="monospace">r%d (%d)</text>'
                   % (mx + 4, my - 4, r["id"], r["length"] >> 6))
    for n in rg.nodes:
        p = pos[n["id"]]
        out.append('<circle cx="%.1f" cy="%.1f" r="7" fill="#e5e7eb"/>' % (X(p[0]), Y(p[1])))
        out.append('<text x="%.1f" y="%.1f" fill="#e5e7eb" font-size="13" '
                   'font-family="monospace">%d</text>' % (X(p[0]) + 9, Y(p[1]) + 4, n["id"]))
    out.append('<text x="12" y="22" fill="#ff9c3f" font-size="16" font-family="monospace">'
               'road set %d - race %d: %s</text>'
               % (setno, rid, " -&gt; ".join("r%d%s" % (s["road"],
                  "+" if (s["dir"] or 0) > 0 else "-") for s in route)))
    out.append('<text x="12" y="40" fill="#8b93a1" font-size="12" font-family="monospace">'
               'SCHEMATIC: node placement is a spring layout, not game geometry. '
               'Edge label = road id (RGTS length&gt;&gt;6). The strip below is '
               'fully derived from proven data.</text>')

    # ---- proven part: the race as a distance strip -----------------------
    y0 = H - STRIP + 34
    if not walk["error"]:
        first = walk["rows"][0]
        L0 = _road_len_units(rg, race.start[0])
        run_in = (L0 - race.start[1]) if race.start[2] > 0 else race.start[1]
        total = run_in + first["dist_to_finish"]
        sx = (W - 80) / float(total)

        def SX(v):
            return 40 + v * sx
        out.append('<text x="12" y="%.0f" fill="#8b93a1" font-size="12" '
                   'font-family="monospace">race profile (proven): total %d units, '
                   'run-in %d on r%d%s, then %d intersections</text>'
                   % (y0 - 16, total, run_in, race.start[0],
                      "+" if race.start[2] > 0 else "-", len(walk["rows"])))
        out.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#ff9c3f" '
                   'stroke-width="6"/>' % (SX(0), y0, SX(total), y0))
        marks = [(0, "START r%d" % race.start[0])]
        for d in walk["rows"]:
            marks.append((total - d["dist_to_finish"], "%d" % d["node"]))
        marks.append((total, "FINISH r%d" % race.finish[0]))
        for x, lab in marks:
            out.append('<line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#e5e7eb" '
                       'stroke-width="2"/>' % (SX(x), y0 - 12, SX(x), y0 + 12))
            out.append('<text x="%.1f" y="%.1f" fill="#e5e7eb" font-size="11" '
                       'font-family="monospace" text-anchor="middle">%s</text>'
                       % (SX(x), y0 + 26, lab))
        for i, step in enumerate(walk["steps"]):
            a = total - step["got"]
            b = total - (walk["steps"][i + 1]["got"] if i + 1 < len(walk["steps"]) else 0)
            out.append('<text x="%.1f" y="%.1f" fill="#ff9c3f" font-size="11" '
                       'font-family="monospace" text-anchor="middle">r%d%s</text>'
                       % ((SX(a) + SX(b)) / 2, y0 - 18, step["road"],
                          "+" if step["dir"] > 0 else "-"))
    out.append("</svg>")
    open(args.out, "w").write("\n".join(out))
    print("wrote %s" % args.out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd")
    p = sub.add_parser("graph"); p.add_argument("file"); p.add_argument("-o", "--out")
    p.set_defaults(fn=cmd_graph)
    p = sub.add_parser("info"); p.add_argument("file")
    p.add_argument("--limit", type=int, default=0); p.set_defaults(fn=cmd_info)
    p = sub.add_parser("scan"); p.add_argument("dir"); p.add_argument("-o", "--out")
    p.add_argument("--chunks", action="store_true")
    p.set_defaults(fn=cmd_scan)
    p = sub.add_parser("verify"); p.add_argument("dir")
    p.add_argument("--deep", action="store_true"); p.set_defaults(fn=cmd_verify)
    p = sub.add_parser("plot"); p.add_argument("dir"); p.add_argument("set", type=int)
    p.add_argument("raceid", type=int); p.add_argument("out")
    p.set_defaults(fn=cmd_plot)
    a = ap.parse_args()
    if not a.cmd:
        ap.print_help()
        return 2
    return a.fn(a) or 0


if __name__ == "__main__":
    sys.exit(main())
