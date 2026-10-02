# Road / track data - Road Rash: Jailbreak (USA, SLUS_01053)

What a race's road actually is: a **route through a road network of 25 intersections and 37 roads**.
The network is described three times on the disc - as plain text (`ROADGRF<n>.TXT`), as a binary graph
(`RGTS`), and as a stream directory (`COTS`) - and the visible world is a byte stream
(`STREAM<n>.STR`) sliced into 16 KiB chunks, two per road (one per driving direction).

Everything below is derived from the disc bytes (extract in `work\disc_us`) and from our own
disassembly. Parser: `src\rrformats\road.{h,cpp}` (checked by `rrtool roadcheck`); independent probe:
`tools\scout\road.py` (outputs in `work\road\`). Both are gates in `tests\run_gates.ps1`. Re-run the probe with:

```
python tools\scout\road.py verify work\disc_us\DATA --deep
```

The contents of the 16 KiB stream chunks are decoded in `road_chunk.md`, `scene_cell.md` and
`population.md`.

Binaries cited:
* `SLUS_010.53`, sha1 `67ed165a2c517d4e6106fb0dfa324d66dd9a76f1`, PS-X EXE header 0x800 bytes,
  text loaded at `0x80010000` (file offset `f` -> address `0x80010000 + f - 0x800`).
* overlay `RASHCDI.BIN`, sha1 `9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06`, **load address `0x8005B5E8`**.
  Derived, not assumed: with that base, all 68 string constants in the overlay's string area
  (file offsets `0x004`..`0x4A8`) are hit exactly by `lui`/`addiu` pairs in the same overlay, and every
  intra-overlay `jal` target (`0x80060028`..`0x8006B3FC`) lands in the code area after the strings.

## 0. Which file the game opens, and how

| string | in | address | used for |
|---|---|---|---|
| `DATA\ROAD` | SLUS_010.53 @0x42C00 | 0x80052400 | path prefix |
| `%sgrf%1d.txt` | RASHCDI.BIN @0x460 | 0x8005BA48 | -> `DATA\ROADgrf1.txt` |
| `%s%d.MAP` | RASHCDI.BIN @0x4A8 | 0x8005BA90 | -> `DATA\ROAD1.MAP` |
| `race%d_%ld.stp` | SLUS_010.53 @0x13C8 | 0x80010BC8 | -> `RACE1_7.STP` (set, race id) |
| `STREAM1.GRF` / `STREAM2.GRF` | SLUS_010.53 @0x13E4 / @0x13D8 | 0x80010BE4 / 0x80010BD8 | road graph |
| `STREAM` | SLUS_010.53 @0x4B640 | 0x8005AE40 | prefix for `.TOC`/`.RLS`/`.STR` |

So `ROADGRF<n>.TXT` is **not** leftover documentation: the retail game parses this ASCII file at
runtime (parser in `RASHCDI.BIN`, see section 1.3). That makes it an exact oracle for the binaries.

Two "road sets" exist; every file of a set carries the same `GMAGIC` at +0x08:

| set | GMAGIC | files |
|---|---|---|
| 1 | `0x3862BB66` (945994598) | `ROADGRF1.TXT`, `ROAD1.MAP`, `STREAM1.{GRF,TOC,RLS,STR}`, `RACE1_*.STP` (63) |
| 2 | `0x3862B8BD` (945993917) | `ROADGRF2.TXT`, `ROAD2.MAP`, `STREAM2.{GRF,TOC,RLS,STR}`, `RACE2_*.STP` (36) |

**Both sets use the same road network.** `STREAM1.GRF` and `STREAM2.GRF` are byte-identical except
for four bytes - 0x08..0x09 and 0x65C..0x65D, the low halves of the two `GMAGIC` fields. Node link
tables, road endpoints, all 37 road lengths and the whole `PMTS` sub-block are bit-identical.
The two sets differ in the streamed world content, not in the topology.
`ROADGRF1.TXT` lists 64 races (ids 1..64; `RACE1_31.STP` is absent from the disc),
`ROADGRF2.TXT` lists 36 (ids 1..18 and 38..55), which matches the `RACE2_*.STP` numbering exactly.

Common container convention (all of `MRPS`, `RGTS`, `COTS`, `CTLR`, `MAP_`, `PMTS`):
`char tag[4]; u32 blockSize; u32 gmagic; ...`, where `blockSize` is the offset of the next block
(verified: `ROAD1.MAP` +0x04 = 0x5C = the offset of `BTT_`; `STREAM1.GRF` +0x04 = 0x654 = the offset
of `PMTS`; `STREAM1.TOC` +0x04 = 0x9A4 = the file size; `RACE1_1.STP` +0x04 = 0x800 = the data start).
All multi-byte fields are little-endian. Unused array slots are left as `0xCDCDCDCD`, i.e. the files
were produced by an MSVC debug-build PC tool; that fill is a reliable "this slot is unused" marker.

---

## 1. `ROADGRF<n>.TXT` - the race graph

### 1.1 Grammar

```
file    := "[NUM_ENTRIES]=" int  block*
block   := "[BEGIN]" entry* "[END]"
entry   := "[RACEID]="          int
         | "[START]="           int int int int
         | "[FINISH]="          int int int int
         | "[GMAGIC]="          int
         | "[RMAGIC]="          int
         | "[START_CHECKER]="   int int int
         | "[FINISH_CHECKER]="  int int int
         | "[RACEINTS]=" n  intersection{n}
intersection := 19 space-separated ints
```

The parser (`0x8006A0C8`) is line based, uses `strncmp` against the literal key including the
brackets, then reads the value after the first `'='` and the rest after each `' '`. Keys may appear in
any order inside a block; a key that is absent simply leaves its field zero. Blank lines are skipped.
Three further keys exist in the parser but occur in neither shipped file, so their values stay 0:
`[VEHICLE_DEFAULT_DENSITY]`, `[VEHICLE_DEFAULT_RATE]`, `[REACTIVE_DEFAULT_DENSITY]`,
`[REACTIVE_DEFAULT_RATE]` (strings at `RASHCDI.BIN` +0x3A8, +0x3C4, +0x3DC, +0x3F8).
`-1` is the "none" value everywhere.

### 1.2 Field meaning (all proven, see 1.4)

`[START]` / `[FINISH]` = `road, distance, direction, node`
* `road` - index into the `RGTS` road table.
* `distance` - position along that road, in *road units >> 6* (the unit used by every distance in
  this file; `RGTS` stores lengths 64x larger).
* `direction` - `+1` = travelling from `road.nodeA` to `road.nodeB`, `-1` = the other way.
* `node` - the intersection where the route begins / ends. `-1` = the race has no intersections at
  all (one case: set 1 race 32, `[RACEINTS]=0`).

`[START_CHECKER]` / `[FINISH_CHECKER]` = 3 ints, stored verbatim at +0x10..+0x18 of the corresponding
28-byte runtime record: a `(scene-cell resource id, primitive-group index, quad index)` triple naming
the quad of the finish banner, e.g. `33554538 1 3` = `0x020000AA`, and `-1 -1 -1` for every
`[START_CHECKER]` in both files. Decoded in `rules.md` 6.1.

Intersection row, 19 ints (field index -> meaning):

| # | name | meaning |
|---|---|---|
| 0 | `node` | intersection id, index into the `RGTS` node table |
| 1 | `dist_to_finish` | distance from this intersection to the finish line, road units >> 6. `0x7FFFFFFF` = this row is not on the route |
| 2 | `node_span` | length the intersection itself contributes to the race distance (20..55 observed) |
| 3..10 | 4 x (`road`, `dir`) | every road meeting this intersection, with a travel direction |
| 11..14 | 4 x `route_road` | the road the route leaves by (only one is ever set) |
| 15..18 | 4 x `next_node` | the intersection reached from here; **slot i belongs to link i** of fields 3..10 |

Fields 3..10 and 11..14 are *packed* by the parser (entries equal to -1 are skipped and the rest
shift down), fields 15..18 are *positional* (stored at their slot index). This asymmetry is in the
code, not an artefact: `0x8006A648` writes `link[count++]`, `0x8006A6D0` writes `routeRoad[count++]`,
`0x8006A72C` writes `nextNode[slot]` with `slot` running 0..3.

Direction convention of fields 3..10: **the route road carries the outgoing direction, every other
road carries the incoming direction**. In other words the row describes "all roads that feed this
junction, plus the one you leave by". Verified for every link of every row of all 100 races (0 bad).

The rows are **not in route order** and may contain intersections that are not on the route at all.
The route is walked as: start at `[START].node`, take `route_road`, leave through the `next_node` in
the matching slot, repeat until `[FINISH].node`.

### 1.3 What the parser builds (for the native port)

Global at `0x800D6170`, 40 bytes, `memset` at `0x80069E24`:

| off | type | content |
|---|---|---|
| +0x00 | ptr | the single allocation below |
| +0x04 | u32 | `[RMAGIC]` |
| +0x08 | u16 | `[NUM_ENTRIES]` |
| +0x0A | u16 | `[VEHICLE_DEFAULT_DENSITY]` |
| +0x0C | u16 | `[VEHICLE_DEFAULT_RATE]` |
| +0x0E | u16 | `[REACTIVE_DEFAULT_DENSITY]` |
| +0x10 | u16 | `[REACTIVE_DEFAULT_RATE]` |
| +0x12 | i16 | `[RACEINTS]` |
| +0x14 | ptr | START record (28 B) |
| +0x18 | ptr | FINISH record (28 B) |
| +0x1C | ptr | the text buffer |
| +0x20 | i32 | its length |
| +0x24 | ptr | intersection array, `RACEINTS + 1` entries of 120 B |

One allocation of `120 * (RACEINTS + 1) + 56` bytes (`0x8006A138`), or 28 bytes when `RACEINTS == -1`.

START/FINISH record, 28 B (filled at `0x8006A21C` and `0x8006A388`, checkers at `0x8006A41C`/`0x8006A47C`):
`i32 road; i32 dist << 16; i32 dir; i32 node; i32 checker[3];`

Intersection record, 120 B (`0x8006A588`..`0x8006A748`):

| off | type | content |
|---|---|---|
| +0x00 | i32 | node id (field 0) |
| +0x04 | i32 | `dist_to_finish << 12` |
| +0x08 | i32 | `node_span << 12` |
| +0x0C | i32 | number of links stored |
| +0x10 | i32 | number of route roads stored |
| +0x14 | 4x16 B | link: `i32 road; i32 dir; i32 (RGTS length >> 6) << 16; u16 road.nodeA; u16 road.nodeB` |
| +0x54 | i32[4] | route road ids |
| +0x64 | i32[4] | next node ids |
| +0x74 | u16, u16 | zeroed by the parser; runtime state |

**In live memory both `routeRoad[]` (`+0x54`) and `nextNode[]` (`+0x64`) are packed**, with `+0x10`
their common count. Race 4 of set 1, node 7: the text gives `route = [-1, 10, -1, -1]`,
`next = [-1, 1, -1, -1]`; memory holds `route = (10, -1, -1, -1)`, `next = (1, -1, -1, -1)`. The route
walk at `SLUS 0x8003B270` (bounded by `+0x10`) depends on this, so the positional reading above is right
for the *text* but a runtime record must be built packed (`rules.md` 6.5).

Note the two different fixed-point scales: `[START]`/`[FINISH]` distance is shifted by 16
(`sll v0,v0,0x10` at `0x8006A258`), the intersection distances by 12 (`sll v0,v0,0xc` at
`0x8006A5EC`). The last three link fields are filled by the helper at `0x8006A014` from the `RGTS`
road record, so the runtime does not re-read the graph while driving.

Entry `RACEINTS` (the extra one) is synthesised from `[START]` at `0x8006A2B4`: node `-1`,
+0x04 = +0x08 = `-4096`, one link `{[START].road, [START].dir}`, `routeRoad[0] = [START].road`,
`nextNode[0] = [START].node`, +0x74 = `0xFFFF`. It is the virtual "start line" junction.

Helper functions: `0x80069B10` parse one int after a separator char; `0x80069B8C` find
`[NUM_ENTRIES]`; `0x80069C60` find the `[BEGIN]`/`[RACEID]=n`/`[END]` block of a given race id;
`0x80069DEC` fill the header struct; `0x8006A0C8` the main parser.

### 1.4 Proven invariants (`road.py verify`, all 100 races, 0 failures)

1. `GMAGIC` is identical in `.TXT`, `.GRF` and `.TOC` of a set.
2. The road set of every intersection row equals the link set of that node in `RGTS`.
3. The direction convention of 1.2 holds for every link of every row.
4. `next_node[i]` is the far endpoint of `link[i]` under `link[i].dir`.
5. `[START].road` is a link of `[START].node`, listed with `dir == [START].dir` (the incoming one).
6. The route walk closes on `[FINISH].node` for every race.
7. **Distance identity, exact integer arithmetic, 441 checks:**
   `dist_to_finish(n) = node_span(n) + (RGTS.road[route_road].length >> 6) + dist_to_finish(next)`
   and at the finish node
   `dist_to_finish = node_span + (dir > 0 ? [FINISH].dist : (length >> 6) - [FINISH].dist)`.
   This pins the unit of every distance in the text file and proves that `RGTS` road lengths are the
   same quantity scaled by 64.

Derived per race: run-in from the start point to the first junction, plus `dist_to_finish` of that
junction = total race distance. Set 1: 2405..23090 units (mean 13714); set 2: 6315..15808.
Dumped to `work\road\roadgrf1.json` / `roadgrf2.json`.

---

## 2. `RGTS` - `STREAM<n>.GRF` (3044 bytes) - the road network

Header (offsets in the file):

| off | value in `STREAM1.GRF` | meaning |
|---|---|---|
| +0x00 | `"RGTS"` | tag |
| +0x04 | `0x654` | block size = offset of the `PMTS` sub-block |
| +0x08 | `0x3862BB66` | GMAGIC |
| +0x0C | `25` | node count |
| +0x10 | `37` | road count |
| +0x14 | `0x1C` | offset of the node array |
| +0x18 | `0x404` | offset of the road array |

Node record, 40 bytes (`0x1C + 40*i`), accessor in the EXE at `0x800245F4` (`base + i*40`,
base from the gp-relative global at `gp+0x1D8`, field +0x14):

```
i32 id;                  // == i
i32 nLinks;              // 2 or 3 in both sets
struct { i32 road; i32 dir; } link[4];   // unused slots are 0xCDCDCDCD
```
`0x1C + 25*40 = 0x404` = the road array offset, exactly.

Road record, 16 bytes (`0x404 + 16*i`), accessor at `0x800245DC` (`base + i*16`, field +0x18):

```
i32 id;      // == i
i32 length;  // 114311 .. 498632; the text graph uses length >> 6
i32 nodeA;
i32 nodeB;
```
`0x404 + 37*16 = 0x654` = the `PMTS` offset, exactly.

`link.dir` is the **outgoing** direction: `+1` when the node is `road.nodeA`, `-1` when it is
`road.nodeB`. Verified for all 25x(2..3) links of both sets, 0 exceptions.

### 2.1 `PMTS` sub-block (`STREAM1.GRF` +0x654, size 0x590)

`char[4] "PMTS"; u32 size; u32 gmagic; u32 25; u32 59; u32 0; u32 0; u32 0x2C; u32 0xF4; u32 0x590;`
then two arrays, both relative to the block start:

* +0x2C: 25 x 8 B = `{ u32 key; u32 nodeIndex; }` (`0x2C + 25*8 = 0xF4`, exact)
* +0xF4: 59 x 20 B = `{ u32 key; i32 road; i32 startAlongRoad; i32 flag; i32 length; }`
  (`0xF4 + 59*20 = 0x590`, exact = the block size)

The 59 records **tile every road exactly**: for road 0 the pieces are `[0, 111772)` and
`[111772, +24537)`, and `111772 + 24537 = 136309` = `RGTS` road 0 length. `flag` is 0 or 1, meaning
unknown. `key` is a 32-bit resource id of the form `0x6xxxxxxx` for intersections and `0x7xxxxxxx`
for road pieces; the high half steps by `0x80` per group (`0x6080`, `0x6100`, `0x6180`, ...) and the
low half is a small index. The same ids appear in `ROAD<n>.MAP` (section 7).

---

## 3. `COTS` - `STREAM<n>.TOC` (2468 bytes) - the TOC -> STR relation

| off | value in `STREAM1.TOC` | meaning |
|---|---|---|
| +0x00 | `"COTS"` | tag |
| +0x04 | `0x9A4` | block size = file size |
| +0x08 | `0x3862BB66` | GMAGIC |
| +0x0C | `37` | road count |
| +0x10 | `25` | node count |
| +0x14 | `0x6C0` | offset of the road array |
| +0x18 | `0x1C` | offset of the node array |

Node record, 68 bytes (`0x1C + 68*i`, `0x1C + 25*68 = 0x6C0`, exact):

```
i32 node;                                        // == i
struct { i32 road; u32 offset; i32 size; } slot[4];   // road == -1 for unused slots
struct { u32 offset; i32 size; } extra[2];
```
The `road` ids of the slots equal the `RGTS` link roads of that node, in the same order (25/25 in
both sets). In both shipped sets **every** node offset is the EOF marker with size 0, i.e. junctions
are not streamed separately here.

Road record, 20 bytes (`0x6C0 + 20*i`, `0x6C0 + 37*20 = 0x9A4` = file size, exact):

```
i32 road;        // NOT equal to i: the array is sorted by stream offset
u32 fwdOffset;   // byte offset into STREAM<n>.STR
u32 fwdSize;
u32 revOffset;
u32 revSize;
```

**The TOC->STR relation (exact):** walking the 37 records in file order and taking
`fwd` then `rev` of each yields a gapless, overlap-free tiling of the whole `.STR`:

* every offset and size is a multiple of `0x4000` (16 KiB);
* record 0 starts at offset 0; each range starts exactly where the previous ended;
* the last range ends at `0x70C8000` = 118259712 = the size of `STREAM1.STR` (7218 chunks);
  for set 2 at `0x46A4000` = 74072064 = `STREAM2.STR` (4521 chunks).

So a road has **two streams, one per driving direction**, and `0x4000` is the streaming granularity
(8 CD sectors of 2048 bytes). The value `0x070C8000` (= file size) doubles as the "no data" marker in
the node records; set 2 uses its own file size `0x046A4000` the same way.

---

## 4. `CTLR` - `STREAM<n>.RLS` (30308 bytes)

```
+0x00  char[4] "CTLR"
+0x04  u32 0x9C                  // block size
+0x08  u32 offset[37]            // 0x9C = 8 + 37*4, exact; one entry per road
+0x9C  char[4] "TDLR"
+0xA0  u32 0x75C8                // 0x9C + 0x75C8 = 0x7664 = file size, exact
+0xA4  records
```

The 37 offsets are absolute file offsets, strictly ascending, from `0xA4` to `0x7404`; every gap
between consecutive offsets (and to EOF) is a multiple of 32, giving 942 records of 32 bytes in
total, 17..41 per road. Every one of the 942 records starts with a u32 resource key whose high half
takes one of 13 values `0x4000 + 0x80*k` (the same "group steps by 0x80" pattern as the `PMTS`
keys of section 2.1, with a different type nibble), and every one ends with `0xFFFF`; the tail of a
record is a `0xFFFF`-terminated list of u16 ids in the `0x04xx` range. Two of the middle u32 fields
are multiples of `0x4000` and therefore look like a stream offset/size pair. The exact field split
is **unknown**. Both sets' `.RLS` have identical sizes and identical offset tables but different
contents (first difference at 0xAD).

---

## 5. `MRPS` - `RACE<set>_<race>.STP` (99 files, 215..670 KB)

Header, 0x800 bytes (rest zero-filled):

| off | `RACE1_1.STP` | meaning |
|---|---|---|
| +0x00 | `"MRPS"` | tag |
| +0x04 | `0x800` | block size = start of the payload |
| +0x08 | `0x3862BB66` | GMAGIC of the road set |
| +0x0C | `1` | **race id** - equals the number in the file name for all 99 files |
| +0x10 | `945995209` | `[RMAGIC]` of that race in `ROADGRF<n>.TXT` |
| +0x14 | `0x800` | payload offset |
| +0x18 | `0x64000` | payload size |
| +0x1C | `25` | chunk count |
| +0x20 | `0` | always 0 |
| +0x24 | `0x68000` | stream resume offset, see below |

Proven for all 99 files: `payloadSize == chunkCount * 0x4000` and
`fileSize == payloadOffset + payloadSize`, `+0x0C` == file number, `+0x08`/`+0x10` == the `GMAGIC`
and `[RMAGIC]` of the matching text entry. Chunk counts run 13..37.

**The payload is a preload cache of stream chunks.** All 2200 chunks of all 99 `.STP` files are
byte-identical to chunks of the matching `STREAM<n>.STR` (verified by hash, 1562/1562 and 638/638).
Most of them are the leading chunks of the *start road's* stream in the *start direction*; the
remainder are chunks that also occur near the junction the race starts from.

**`+0x24` is the stream resume offset** (proven, 99/99): let `(off,size)` be the `COTS` range of
`[START].road` in `[START].dir`; then `+0x24 == (p_last + 1) * 0x4000` where `p_last` is the highest
chunk position inside that range that the `.STP` contains. I.e. the engine seeks the start road's
stream to `+0x24` and continues reading from there, everything before it being already in memory.

`work\road\stp_report.txt` is the full per-file table.

---

## 6. The 16 KiB stream chunk

Every `0x4000` unit in `.STR` (and therefore in `.STP`) is one streamed resource. It starts with a
`u32 key` whose top nibble is the resource type, followed by a `0xFFFE 0xFFFF`-terminated list of
residency windows `{u16 road; u16 from; u16 to;}`; the payload starts at `+0x20`. Chunks that repeat
the previous chunk's key and carry no terminator are continuations of a resource longer than
`0x4000`. The full decoding - the eleven resource types, the road geometry (type 3), the panorama
(type 4, a tagged chain `PANO` / `STEN` / `OFFS` / `HORZ` / `MDEC` stored with reversed tags) - is in
`road_chunk.md`; the scene cells (types 0, 8, 9) are in `scene_cell.md`.

---

## 7. `MAP_` - `ROAD<n>.MAP` (12388 bytes)

Header block, 0x5C bytes: `"MAP_"; u32 0x5C; u32 gmagic; u32 113; u32 26; u32 0x00810600;
u32 0x44A56E66; u32 0x00811C20;` then zeros (last three **unknown**; `0x44A56E66` is 1323.2 as an
IEEE float, the other two are ~8.45 MB values).

Block chain, each `tag + u32 size`, `offset += size`, ending exactly at the file size:

| offset | tag | size | contents (proven by exact division) |
|---|---|---|---|
| 0x005C | `BTT_` | 0x0E28 | 113 records x 32 B (`8 + 113*32 = 0xE28`); 113 = header +0x0C |
| 0x0E84 | `BST_` | 0x0200 | 63 records x 8 B |
| 0x1084 | `BIT_` | 0x1458 | 26 records x 200 B (`8 + 26*200 = 0x1458`); 26 = header +0x10 |
| 0x24DC | `IPT_` | 0x0134 | 25 records x 12 B - one per intersection |
| 0x2610 | `PDT_` | 0x06E0 | 146 records x 12 B |
| 0x2CF0 | `GPDT` | 0x0374 | 73 records x 12 B |

### `BTT_` - object table

```
u32 seq; u32 flags; u32 key; u32 zero; i32 index; u32 a; u32 b; i32 -1;
```
File order is 63 road-piece records (`seq` 50..112, `flags = i << 16`, `index` = road id, `key` =
a `0x7xxxxxxx` `PMTS` piece key) followed by 50 intersection records (`seq` 0..49,
`flags = (i << 16) | 1`, `index` = node id, `key` = a `0x6xxxxxxx` `PMTS` node key) - exactly two
records per intersection, i.e. node `k` owns `seq` `2k` and `2k+1`. For intersection records
`a == b == 0`; for road pieces `a`/`b` are large values that chain (`b` of one piece == `a` of the
next piece of the same road). Their unit is **unknown** - they are not proportional to the piece
length (the ratio varies from 617 to 2269 across pieces).

### `BST_` - the doubly linked map chain

```
u16 bttSeq; u16 next; u16 prev; u16 0xCDCD;
```
63 records, one per road-piece `BTT_` record, threading pieces and intersections into one chain.
For the 19 records whose `next` and `prev` are both intersection records, those two are exactly the
node records of `road.nodeA` (`prev`) and `road.nodeB` (`next`) - 19 of 19, using `node k -> seq 2k`.

### `IPT_` - per-intersection index

```
u16 bttSeqBase;  // == 2*i, the first BTT_ record of this intersection
u16 node;        // == i
u16 nLinks;      // == RGTS node nLinks (2 or 3)
u16 pdtBase;     // packed running offset into PDT_
u16 pdtCount;    // == nLinks * (nLinks - 1)
u16 0xCDCD;
```
`pdtBase` of the last node (24) is 140 and `140 + 6 = 146` = the `PDT_` record count, exact.

### `PDT_` - the turn table

146 records of 12 bytes, grouped per intersection as `IPT_` says. For every one of the 25
intersections the records are exactly the **ordered pairs of distinct roads** meeting there
(3 links -> 6 records, 2 links -> 2 records; 24*6 + 2 = 146). Verified against `RGTS` for 25/25 nodes.
Layout: `u16 pairIndex; u16 ?; u16 1; i16 dir(+-1); u16 roadFrom; u16 roadTo;` - the two middle
fields are **unknown**.

### `BIT_` / `GPDT`

`BIT_` is 26 x 200 B; inside a record there are repeated 20-byte entries keyed by a `BTT_` `seq`,
containing three i16 that look like a direction vector and two i32, one of which is constant
(`0xFFDA801A`) across entries while the other varies around `0x1B4E76F5`. If those i32 are 16.16
fixed point they are ~6990 and ~-37, i.e. the same order of magnitude as race distances - a
plausible but **unproven** world coordinate. `GPDT` is 73 x 12 B (3 per intersection, 1 for the
2-link node 8) and is mostly zero in set 1. Both are **not decoded**.

---

## 8. What is still unknown

* The road's **geometry** (centre line, banking, elevation, width) is not in `RGTS`/`COTS`/`ROADGRF`;
  it is the type-3 stream chunk decoded in `road_chunk.md`. Everything in this document is topology +
  distance. Note that `RGTS` lengths are path lengths, not distances: roads 23 and 24 both join nodes
  17 and 18 yet are 119334 and 264254 long.
* `MAP_` header fields +0x14, +0x18, +0x1C.
* `BTT_` `a`/`b` unit; `BIT_` and `GPDT` record layout; the two middle `PDT_` fields;
  `PMTS` piece `flag`.
* `CTLR`/`TDLR` record field split; the meaning of the u16 id lists (they resemble the chunk header
  lists, so probably resource ids to load/release at a stream position).
* The exact `.STP` preload selection rule, as opposed to the resume offset which is proven: 7 of the
  25 chunks of `RACE1_1.STP` are not inside the start road's own stream range. `road_chunk.md`
  section 3 shows they are resources shared with the roads meeting the start junction, whose
  residency windows cover the start of the race.

## 9. Probe outputs (`tools\scout\road.py`, written to `work\road\`)

* `roadgrf1.json`, `roadgrf2.json` - all 100 races, decoded rows plus the resolved route.
* `stp_report.txt` - the 99 `.STP` headers with the consistency verdict
  (`road.py scan <DATA> --chunks` adds every chunk header: `stp_chunks.txt`).
* `verify_deep.txt` - the full cross-file check including chunk hashing.
* `set1_race{1,20,52}_route.svg`, `set2_race5_route.svg` - the network with the race route
  highlighted, plus a distance strip that is derived only from proven quantities. The node placement
  in the upper panel is a spring layout, **not** game geometry, and the edges are drawn with a
  uniform length. `RGTS` lengths are path lengths of curved roads, not straight-line distances:
  three node pairs are joined by two different roads each - (17,18) by roads 23 and 24 with lengths
  119334 and 264254, (21,22) by roads 30 and 32, (23,24) by roads 34 and 36 - so no embedding can
  reproduce them. The real road shape comes from the type-3 chunks (`road_chunk.md`).
