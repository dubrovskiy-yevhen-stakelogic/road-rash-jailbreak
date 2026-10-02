# The 16 KiB world chunk - Road Rash: Jailbreak (USA, SLUS_01053)

**The road geometry is out.** A `0x4000` stream chunk is not a terrain tile: it is one *resource*,
and the chunk's first word says which kind. One of the eleven kinds (**type 3**) carries the road
itself - centre line, heading, banking, elevation, cross-section width and the distance along the
road - as an exact 16.16 fixed-point world position per slice. Everything else in the stream is
textures, panorama, scenery/collision cells and streamed audio.

This document only covers the chunk. The containers around it (`COTS`, `RGTS`, `MRPS`, `PMTS`,
`MAP_`) are in `docs\formats\road.md`, whose section 6 only summarises the chunk.

Parser: `src\rrformats\chunk.{h,cpp}`. Independent probe: `tools\scout\chunk.py` (outputs in
`work\chunk\`; gate "probe: chunk invariants" in `tests\run_gates.ps1`). Re-run everything with

```
python tools\scout\chunk.py verify work\disc_us\DATA
```

Binaries cited:

* `SLUS_010.53`, sha1 `67ed165a2c517d4e6106fb0dfa324d66dd9a76f1`, text at `0x80010000`
  (file offset `f` -> address `0x80010000 + f - 0x800`).
* overlay `RASHCDG.BIN`, sha1 `cfe43a7786759f2cb9c57751cf99e84d1074782c`, load address `0x8005B5E8`.

Data cited: `work\disc_us\DATA\STREAM1.STR` (7218 chunks), `STREAM2.STR` (4521),
`RACE<set>_<race>.STP` (99 files), and the oracle captures in
`work\oracle\vr_capture\ramdumps\` and `work\oracle\state\rr-race\`.

Confidence is marked inline: **[proven]** = an exact arithmetic identity over the whole data set or
a direct read of the dispatching code; **[established]** = read once out of our disassembly or holds
over every sample checked; **[probable]**; **[guess]**. Section 11 is the explicit unknown list.

---

## 0. TL;DR

1. A chunk begins with `u32 key`. **`key >> 28` is the resource type** and `key & 0x0FFFFFFF` the
   resource id; the id is `(group << 16) | index` with `group` a multiple of `0x80`.
   Proven from the dispatcher at `0x80031604`, which does `srl s4,a0,0x1c` (`0x8003168c`) and jumps
   through the 11-entry table at `0x80010D48`.
2. After the key comes a list of **residency windows** `{u16 road; u16 from; u16 to;}` in *world
   units*, `0xFFFF` padded and terminated by `0xFFFE 0xFFFF`. This is the distance-along-road <->
   chunk relation the streamer runs on.
3. **Type 3 is the road.** Its payload is a chain of tagged sub-blocks; `SLCT` is an array of road
   slices, each `{u16 index; i16 m[9]; i32 pos[3]; u32 chord; ...; u32 distance; ...}`.
   `m` is a GTE rotation matrix (4096 = 1.0) whose **row 2 is the unit tangent and row 0 the lateral
   axis**; `pos` is 16.16 world; `distance` is the running path length.
4. **1 RGTS/PMTS road unit = 1024 raw world units = 1/64 of a world unit.** So the distances in
   `ROADGRF<n>.TXT` (which are `RGTS length >> 6`) *are* world units, exactly.
5. There are **113 road objects per set** - one per `BTT_` record of `ROAD<n>.MAP`: 63 road pieces
   and 2 per intersection - and they tile every road's parameter space end-to-start with no gaps.
6. Verified against the live game: the type-3 chunk sits in guest RAM byte for byte, and a live
   traffic vehicle's bounding box lies within a road width of our decoded centre line in all 14
   RAM dumps.

---

## 1. Chunk header

Offsets are inside a `0x4000` chunk. `.STR` chunk `i` is at file offset `i * 0x4000`; a `.STP`
chunk `i` is at `0x800 + i * 0x4000`.

```c
u32 key;                                   // +0x00
struct { u16 road; u16 from; u16 to; } window[];   // +0x04, 0xFFFF = empty slot
u16 0xFFFE;  u16 0xFFFF;                   // end of list
```

The list area is `0x1C` bytes and the payload starts at `+0x20` for **every** chunk of both sets:
of the 11739 chunks, the 10933 that carry a terminator all have it at `+0x1C`, and the other 806 are
continuation chunks (1.3). A chunk whose list is empty is padded with `0xFFFF`.

### 1.1 `key` **[proven]**

| bits | meaning |
|---|---|
| 31..28 | resource **type**, 0..10 |
| 27..16 | **group**, always a multiple of `0x80` (13 groups `0x000`..`0x600` in set 1) |
| 15..0 | **index** inside the group |

The dispatcher is `0x80031604` in `SLUS_010.53`. It loads the resource record's first word,
splits it with `srl s4,a0,0x1c` / `and s3,a0,0x0FFFFFFF` (`0x8003168c`, `0x80031690`), bounds-checks
`sltiu v0,s4,11` (`0x800316a4`) and jumps through `*(0x80010D48 + 4*type)`. The mirror table for
release is at `0x80010D78`, used by `0x800318e8`.

| type | load target | then | what it is | set 1 | set 2 |
|---|---|---|---|---|---|
| 0 | `0x80031784` | `0x80031e1c`, `0x80032a20` | scene cell (see 5) | 576 | 804 |
| 1 | `0x80031734` | `0x80032cc8` -> `0x80034dec` | texture | 926 | 686 |
| 2 | `0x80031704` | `0x80032c6c` -> `0x80034dec` | texture, id OR `0x8000` | 476 | 467 |
| 3 | `0x800317c4` | `0x8003cec4` (`id & 0xFFFF`) | **road geometry** | 274 | 274 |
| 4 | `0x800316d0` | `0x800136bc` | panorama / sky | 2032 | 0 |
| 5,6,7 | `0x80031804` | - | returns error 2, unused | 0 | 0 |
| 8 | `0x80031784` | `0x80032a20` | scene cell | 571 | 300 |
| 9 | `0x8003176c` | `0x80032a20` (`a3 = 0`) | scene cell | 549 | 234 |
| 10 | `0x800317ec` | `0x8001a0c0` | SPU ADPCM | 1814 | 1756 |

`0x80031560` decides where a type's payload starts (`*(rec+0x10)`, with `*(rec+0x0C)` = the chunk):

* types 1 and 2 -> the chunk base itself (`0x80031590`);
* types 0 and 8 -> `chunk + 0x20`, and the block length is taken from the word there:
  `end = chunk + 0x20 + (word * 4 - 48)` (`0x800315a0`..`0x800315cc`);
* everything else -> `chunk + 0x20` (`0x800315d0`).

Types 1 and 2 repack the id into what is clearly a VRAM/tpage coordinate before calling the loader -
`((id >> 23) & 0x1F) << 10 | (id & 0x3FF)`, with type 2 also setting bit 15
(`0x80032c74`..`0x80032c8c` and `0x80032cd0`..`0x80032d04`) **[proven]**. That is why types 1/2 are
named `tex` / `tex_sprite`.

Type 10 is PS1 SPU ADPCM **[established]**: the payload is a run of 16-byte blocks whose first two
bytes are the shift/filter and flag byte (`0x30 0x00`, `0x31 0x00`, ...), preceded at `chunk+0x40`
by two identical 12-byte records that look like an SPU voice setup
(`01 7f 40 00 7f 7f ff 80 c0 1f 00 04`).

### 1.2 Residency windows **[proven]**

Each triple is `(road id, from, to)` in **world units** = `RGTS road length >> 6`.

Proof, three independent ways:

* For every type-3 chunk the triple's `road` equals the road that the chunk's own `GRPT` piece key
  resolves to through `PMTS` - 88/88 objects with a piece key, 0 exceptions
  (`chunk.py verify`, check "header residency window names the object's road").
* The upper bound never exceeds that road's length in world units, and lands on it exactly when the
  resource reaches the end of a road. `STREAM1.STR` chunk 53 (key `0x30000013`, the node-9
  intersection object) carries `(13, 0, 568) (12, 1511, 2108) (17, 1453, 2033)`: road 12 is 134945
  road units = **2108.5** world units and road 17 is 130168 = **2033.9**, and roads 12, 13 and 17
  are exactly the three roads that meet at node 9 in `RGTS`.
* The window is the object's own extent widened by a constant margin. Object 55 (road 3) spans
  world 498.1..1955.7 from `GRPT`; its window is `[424, 2028]`, i.e. 74 units of run-in and 72 of
  run-out. Object 71 (road 12) spans 506.4..1584.6 and its window is `[435, 1657]` - the same
  ~72-unit margin **[established]**.

So the window answers "which chunks must be resident at distance *d* along road *r*" directly, with
no extra table. `python tools\scout\chunk.py info <file>` prints it per chunk.

### 1.3 Continuation chunks **[established]**

463 of 7218 chunks in `STREAM1.STR` (343 of 4521 in set 2) repeat the previous chunk's key and carry
**no** `0xFFFE` terminator. They are the tail of a resource longer than `0x4000`; all observed cases
are types 1/2 (textures). `chunk.py` marks them `CNT`.

---

## 2. Type 3 - the road object

`274` chunk occurrences in each set, but only **113 distinct ids, 0..112 with no holes** (the same
resource appears several times in the stream, once per stream position that needs it). 113 is
exactly the `BTT_` record count of `ROAD<n>.MAP` (`docs\formats\road.md` section 7): **the type-3
object id is the `BTT_` `seq`** **[proven by the count, the split and the owner field]**.

### 2.1 Fixed header, `chunk + 0x20`, `0x6C` bytes

```c
u32 objectIndex;   // +0x20  == key & 0xFFFF, 0..112
u32 pieceKey;      // +0x24  a PMTS key: 0x6xxxxxxx = intersection, 0x7xxxxxxx = road piece
u32 owner;         // +0x28  node id (ids 0..49) or road id (ids 50..112)
u32 half;          // +0x2C  0 or 1 for an intersection, 0xFFFFFFFF for a road piece
...                // +0x30..+0x8B, partly decoded (see 11)
```

Ids 0..49 are the 50 intersection objects - node `k` owns ids `2k` and `2k+1`, matching `BTT_`
"exactly two records per intersection" - and ids 50..112 are the 63 road-piece objects
**[proven, `chunk.py verify` check 4]**.

### 2.2 Sub-block chain, from `chunk + 0x8C`

`char tag[4]; u32 size;` walked by `offset += size`, exactly like every other container in this
game. The chain start `0x8C` is fixed for all 226 type-3 objects of both sets.

| tag | record | typical n | content |
|---|---|---|---|
| `GRPT` | 32 | 1 | object descriptor (2.3) |
| `SUBT` | 28 | 1 per sub-object | sub-object descriptor (2.3) |
| `SLCT` | **52** | 35..157 | **the road slices** (2.4) |
| `XSIH` | 16 | 1..6 | cross-section index: `{u32 idx; u32 next; u32 from; u32 to;}` |
| `XSDH` | 44 | 6 per `XSIH` entry | cross-section detail, lateral offsets (2.5) |
| `XSAI` | 20 | 1 per `XSIH` entry | lane count and lane width (2.5) |
| `DIST` | 4 | 22..74 | `{u16 bucket; u16 firstSlice;}` distance -> slice index |
| `SEG_` | 20 | 1..6 | `PMTS` piece records, verbatim (2.6) |
| `BGDT` | 20 | 22..115 | roadside strip descriptors, texture ids (2.7) |
| `BSDT` | 40 | 0..250 | roadside segment endpoints (2.7) |
| `BZDT` | 4 | 42..190 | `{u16 idx; u8; u8;}` index list, undecoded |
| `NMBD` | 4 | 25 objects only | undecoded |

All 1259 sub-blocks per set divide exactly by these record sizes
(`chunk.py verify` check 2, 0 exceptions).

### 2.3 `GRPT` / `SUBT` - the object's place on the road

`GRPT`, one record of 32 bytes:

```c
u32 flags;        // 0x00010000 for an intersection core, 0 otherwise
u32 chainIndex;   // small, a BST_/BTT_ style index  [probable]
u32 pieceKey;     // the road's FIRST PMTS piece key - resolves to the road
u32 owner;        // road id (or node id for an intersection core)
u32 length;       // end - start
u32 0x00010000;
u32 start;        // position along the road, raw units
u32 end;
```

`SUBT`, 28 bytes: `{u32 pieceKey; u32 (isNode<<30)|(id<<16); u32 nSLCT<<16; u32 length; u32 nSEG_;
u32 nDIST<<16; u32 0;}`. Checked on object 55: `nSLCT<<16 = 0x004A0000` = 74 and the `SLCT` block
holds 74 records; `nDIST<<16 = 0x001D0000` = 29 and `DIST` holds 29 **[established]**.

`start`/`end` are a 1-D parameter along the whole road, origin at the road's `nodeA`. The objects of
one road **chain exactly**: `end` of one equals `start` of the next, 43 of 51 joins in set 1 with the
other 8 being road<->intersection joins where the intersection approach object is absent (see 2.8)
**[proven, `chunk.py verify` check 7]**. Example, road 20 (`0x74000000`):

```
id 29 (junction)  16455332 .. ?
id 80              33205582 .. 131648640
id 81             131648640 .. 290049093
id 82             290049093 .. 410765593
id 83             410765593 .. 477889843
```

### 2.4 `SLCT` - the road slice. This is the geometry.

52 bytes:

```c
u16 index;        // +0x00  0,1,2,...
i16 m[9];         // +0x02  3x3, 4096 = 1.0, row major
i32 pos[3];       // +0x14  world position, 16.16 fixed point, Y is down
u32 chord;        // +0x20  distance to the next slice
i16 a; i8 b; i8 c;// +0x24  not decoded (see 11)
u32 distance;     // +0x28  running distance along the road, same units as GRPT start/end
u8  idx[4];       // +0x2C  two (first, count) runs into BGDT/BSDT  [probable]
u32 tail;         // +0x30  not decoded
```

Proven identities, over all 113 objects of set 1 and all 113 of set 2, `chunk.py verify`:

* **`m` row 2 is the unit tangent.** Over 9142 linked slice pairs the dot product of row 2 with the
  normalised chord vector is 1.0 with a maximum error of **0.00036**.
* **`m` row 0 is the lateral axis**: its dot product with the tangent is 0 to within **0.00063**.
  Row 1 is therefore the surface normal, and its tilt off world Y is the **banking**.
* **`chord` is the straight-line distance to the next slice** and `distance` its running sum:
  `|chord - |pos[i+1]-pos[i]||` is at most 0.046 % of `chord`.
* A junction object holds several disjoint arms in one `SLCT` array. The run break is visible in the
  data itself - `distance + chord == next.distance` only inside a run - so no heuristic is needed
  (`RoadObject.runs()`); 171 of 9313 consecutive pairs in set 1 are run breaks.

So a slice gives, directly and with no reconstruction: **position** (`pos`), **heading** (row 2),
**banking and pitch** (rows 0 and 1), **elevation** (`pos[1]`) and **distance along the road**
(`distance`). Slice spacing is 19..32 world units.

Set 1 and set 2 ship **different** type-3 chunks (105 of 113 differ), but the `SLCT` arrays are
identical - consistent with `road.md`'s finding that the two sets share the road network and differ
only in the streamed world content **[established: first byte difference of object 71 is at
`+0x1FFC`, inside `BSDT`, well past `SLCT`]**.

### 2.5 The unit, and the road width

**1 road unit = 1024 raw = 1/64 world unit** **[proven]**. Two independent derivations:

1. *Arithmetic.* For the 18 cases where a `SEG_` piece boundary `s` falls strictly inside an
   object's `[start,end]`, 16 of them coincide with an `XSIH` cross-section boundary once multiplied
   by 1024; the worst residual is 972 raw units = **0.0148 world units**. Example, object 55: the
   piece boundary is at road unit 42105, `42105 * 1024 = 43115520`, and the `XSIH` boundary is
   `0x0291E63D = 43115581` - 61 raw units apart.
2. *Geometry.* For the 15 roads whose whole length is covered by road-piece objects alone (no
   intersection approach object at either end), `RGTS length / (3D polyline length of the slices)` is
   63.5..65.6 with a mean of 64.2.

Consequence: the `[START]` / `[FINISH]` / `dist_to_finish` numbers of `ROADGRF<n>.TXT`, which
`road.md` proved to be `RGTS length >> 6`, **are world units**. `RGTS length >> 6` also bounds the
header residency windows exactly (1.2).

Width: `XSAI` is one 20-byte record per cross-section index,
`{u16 idx; u16 nLanes; u16 prev; u16 0xFFFF; u16 6; u16 0xFFFF; u16 0xFFFF; u16 0; u32 laneWidth;}`.
`laneWidth` is `0x0009C400` = 640000 raw = **9.766 world units** for every road object sampled, with
`nLanes` = 2 **[established]**. `XSDH` carries six 44-byte records per cross-section index holding
16.16 lateral offsets that are integer multiples of a per-object lane pitch: object 55 uses
`+-4.883 / +-9.766 / +-14.648`, object 67 `+-4.883 / +-9.766 / +-14.648 / +-19.531`, object 80
multiples of 6.104. The exact field split of `XSDH` is **not** decoded, so `chunk.py` takes the
largest magnitude offset in the block as the half width - a measurement, not a claim.
That gives a road 29..39 world units wide, which agrees with the oracle (section 8: a live vehicle sits
6.4..11.7 world units off the centre line).

### 2.6 `SEG_` - the pieces this object touches

20 bytes, **byte-for-byte the `PMTS` road-piece record** of `STREAM<n>.GRF`
(`road.md` 2.1): `{u32 key; i32 road; i32 startAlongRoad; i32 flag; i32 length;}`. An intersection
object lists every piece meeting that junction (4..6 records), a road-piece object lists the pieces
of its own road (1..3). This is what ties a chunk to the `PMTS`/`BTT_` world **[proven: every
`SEG_` key is a `PMTS` key and the road/start/length agree]**.

### 2.7 `BSDT` / `BGDT` - the roadside

`BSDT`, 40 bytes: `{u32 flags; u16 a; u16 b; i32 p0[3]; i32 p1[3]; u32 c; u32 d;}`. `p0` and `p1`
are 16.16 world points and `p1` of one record is `p0` of the next inside a run, i.e. it is a
polyline. Drawn top down the 11623 segments of set 1 reproduce the
road network as a pair of lines flanking each road, so this is the **roadside boundary / barrier**,
not the centre line **[probable]**. Objects 46, 47 and 110 have an empty `BSDT`.

`BGDT`, 20 bytes: `{u16; u16; u16; u16; u16; u16; u16 texA; u16 idxA; u16 texB; u16 idxB;}` - the
last four fields are of the form `0x8000 | small` and `0x9Bxx`, i.e. the same packed texture id shape
that the type 1/2 dispatcher builds (1.1) **[probable]**. Not decoded further.

### 2.8 Coverage, and what is missing

Of the 74 road ends, only 25 have an intersection *approach* object (the `half == 1` object of a
node, which carries a road piece key). The other 49 ends are covered by the intersection *core*
object (`half == 0`), whose `GRPT` start/end are both 0, i.e. it has no 1-D parameter. Drawn, the
core objects are the fans that visibly join the roads (red in `work\chunk\set1_network.svg`), so the
geometry is complete; it is only the 1-D parameterisation that has the 8 breaks reported by
`verify`. A race route therefore recovers 80..87 % of its `RGTS` length from road-piece objects
alone, the rest being inside the junction fans.

**How a route actually crosses the fan [proven]**. It does not take one arm of it. Measured on node
16 of set 1 (the arms of both halves, either way round, joined end to end): the route leaves road 28,
runs 239 world units up a stub of the `half == 1` object, 257 up an arm of the core to the centre,
then back down a second core arm (251) and a second stub (254) onto road 21 - four arms and about
1000 units against a straight-line gap of 918. No single arm spans a junction, in any of them.

Two properties of the fan make the crossing findable without any of the game's own turn tables:

* the arms of one node form **exactly one connected component** when endpoints closer than 42 world
  units are treated as joined - all 25 nodes of set 1 and all 25 of set 2, and the network falls
  apart at 41, so 42 is measured rather than chosen (`tools\scout\junction.py verify`);
* every ordered pair of roads meeting at a node is linked by a chain of **at most six** arms, and a
  search limited to three arms fails on every junction of set 1 race 20 while four succeeds.

`src\game\world.cpp` crosses junctions this way, and `rrtool raceworldall` reports the result over
the whole game: 100 races, 441 of 441 junctions crossed, worst join 42 world units, no route left
with a jump.

---

## 3. Chunk <-> distance along a road

This is the streaming question, and the chunk answers it twice.

1. The **header residency window** (1.2) gives, per chunk, `(road, from, to)` in world units.
2. For a type-3 chunk the **`GRPT` start/end** give its exact extent in raw units
   (`/1024` = road units, `/65536` = world units).

And the stream is laid out in that order: walking a road's forward or reverse range from `COTS`,
the type-3 chunks belonging to that road appear with **strictly monotonic** `GRPT` start -
increasing forward, decreasing in reverse - for **50 of 50** directional streams that contain more
than one such chunk, in both sets **[proven, `chunk.py verify` check 10]**. Example, road 26
forward (229 chunks):

```
chunk   8  object 91  road units  32448..122735
chunk  51  object 92             122735..216737
chunk  95  object 93             216737..315077
chunk 141  object 94             315077..374296
chunk 170  object 95             374296..440920
chunk 200  object  7  (the junction the road ends at)
chunk 207  object  6
```

This also explains two properties of the stream:

* **Chunk content is not unique in a stream** (4084 of 7218 occurrences in `STREAM1.STR` are
  repeats). Confirmed and explained: a chunk *is* a resource, and the same resource is re-emitted at
  every stream position that needs it. Of 274 type-3 occurrences only 113 are distinct. The
  forward and reverse streams of a road necessarily share resources, which is why a chunk cannot be
  attributed to a direction by hashing.
* **A `.STP` does not begin with the start road's own chunks.** `RACE1_1.STP` (start road 3) begins
  with 7 chunks of group `0x100` whose residency windows name roads 2, 3 **and** 26 - shared
  resources - before the group `0x080` chunks that are road 3's own. `chunk.py info` prints it:

```
 0 key=21000089 tex_sprite  r2[1760,2561] r3[0,1210] r26[6577,7387]
 3 key=8100007d geom8       r3[0,842]  r26[7108,7387] r2[2005,2561]
 7 key=20800008 tex_sprite  r3[188,1577]
14 key=30000037 road        r3[424,2028]
17 key=408000e1 pano        r3[522,842]
24 key=20800036 tex_sprite  r3[937,2474] r0[0,50] r4[0,50]
```

The windows are in ascending `from` order, so the `.STP` is simply "every resource live over the
first ~2500 world units of road 3, in the order the streamer would meet them".

---

## 4. Type 4 - the panorama chunk

2032 chunks in set 1 and **none in set 2** **[proven by the scan]**. `chunk + 0x20` holds
`u32 3; u32 road; u32 distanceAlongRoad;` and then a tagged chain with the tags stored reversed
(`ONAP` = `PANO`), walked the same way:

| tag | size | content |
|---|---|---|
| `PANO` | 0x30..0x34 | `u32 1; u32 0; u32 a; u32 b; u32 0; u32 n; u32 ref[n];` - `ref` are resource **ids with the type nibble cleared**, e.g. `0x00800008`, `0x0100007D` |
| `STEN` | 0xE4 | 110 x `{u8; u8;}`, values from a small set (`0x0900`, `0x2900`, `0x2500`, `0x0295`) |
| `OFFS` | 0x78 | 112 bytes, values 1..2 |
| `HORZ` | 0x148..0x400 | ~840..1008 bytes, small values 0..4 then `0x99` fill - a per-column horizon height **[guess]** |
| `MDEC` | 0x10..0x110 | up to 60 per chunk |

`PANO` is the only chunk tag the code compares against a literal: `0x800136bc` builds `'PANO'` with
`lui v1,0x5041` / `ori v1,v1,0x4e4f` (`0x800136c8`, `0x800136d4`) and tests `lw v0,12(t5)`
(`0x800136dc`), i.e. the tag lies 12 bytes into the record it is handed - which is exactly
`chunk + 0x2C` given the three-word prologue. It then registers the block in a 20-entry table at
`*(gp+0x5EC) + 740`, 12 bytes per entry (`0x8001370C`..`0x80013820`); `0x80013828` removes it.
No other chunk tag appears as an immediate anywhere in the EXE or the three overlays (a scan of
every `lui`+`ori`/`addiu` pair in all four images for 4-character ASCII constants), so the sub-block
chains are walked by offset, not matched by tag **[established]**.

`MDEC` is **standard PS1 BS (MDEC) video** **[proven]**: `char tag[4]; u32 size; u32 0x10;
u32 0x40;` then at `+0x10` the classic frame header `u16 numWords; u16 0x3800; u16 quant;
u16 version` - the `0x3800` magic is there verbatim. So the panorama is a set of compressed image
strips plus a per-column horizon/stencil description, consistent with the captured frames, where the
distance is drawn as screen-aligned impostor layers.

---

## 5. Types 0, 8, 9 - scene cells

All three go to `0x80032a20`, which allocates one of **24 slots of 112 bytes at `0x800D87E8`**
(`addiu s6,v0,-30744` with `v0 = 0x800E0000` at `0x80032AC8`; `(i<<3)-i)<<4 = 112*i`,
`0x80032acc`..`0x80032ad8`; the cap is `slti v0,v0,24` at `0x80032a80` on
the counter at `gp+0x250`) and then calls `0x8003234c`, `0x800135e8`, `0x800325bc` and twice
`0x80033f14` **[established]**.

Payload, `chunk + 0x20`:

```c
u32 nWords;                 // the block is nWords*4 bytes long; end-48 is kept at rec+0x18
u32 pair[];                 // values of the form (0x9400|k, 0x1400|k) etc.
struct { u32 road; u32 fromRoadUnits; u32 toRoadUnits; } extent[4];   // 0xFFFFFFFF = empty
```

The 12-word tail is a fixed 4-slot array; for chunk key `0x8300007E` it reads
`(13, 0, 7517) (12, 132501, 138145) (17, 121440, 133880)` and the next chunk of the same road continues
`(13, 7517, 22552)` **[established]**. The rest of the cell payload (geometry, props, collision) is
decoded in `scene_cell.md`.

Evidence they are data, not images: in the `rr-race` savestate all type 0, 3, 4 and 8 chunks of the
best-matching `.STP` appear **verbatim in main RAM** (`work\oracle\state\rr-race\ram.bin`, 256-byte
high-entropy windows), while no type 1 or 2 chunk does. Type 9 chunks are resident verbatim too
(`0x01800001` at guest `0x8016141C`); the window search misses them because a chunk of mostly
small-integer records has no high-entropy window (`scene_cell.md` 11).

---

## 6. Types 1 and 2 - textures

The dispatcher repacks the id into a VRAM-coordinate-shaped word (1.1) and hands the chunk to
`0x80034dec`, so these are the texture resources **[established from the code]**.

**A chunk is a VRAM rectangle, uploaded from its own bytes**: 64 x 128 halfwords of 4bpp, one chunk
for a type-2 id and two for a type-1 id. Resolving the resident cells' keys and comparing the VRAM
rectangles of `work\oracle\state\rr-race\vram.bin` with the chunks on the disc matches every byte
except the rows that hold the chunks' 32-byte headers (`scene_cell.md` 12.1, 12.4) **[proven]**.

A blind search does not find them: probing 2000 chunks of `STREAM1.STR` and the whole of
`RACE1_4.STP` / `RACE1_13.STP` (the two `.STP` files whose chunks best match `rr-race`'s RAM) with
256-byte high-entropy windows gives **zero** byte-exact occurrences in `vram.bin`, for any type - the
savestate's resident road objects are 14, 15 and 67 (node 7, road 9) while the best `.STP` match is
only 43 %, so the textures resident at that moment are other ones.

---

## 7. What the chunk does *not* carry

* **No texture or palette reference inside the road geometry** except the packed ids in `BGDT`.
* **No traffic or hazard spawn table** was found in any type-3 block. The `[VEHICLE_DEFAULT_DENSITY]`
  / `[REACTIVE_DEFAULT_RATE]` keys that `RASHCDI`'s parser knows about (road.md 1.1) are absent from
  both shipped `.TXT` files, so if spawns are data-driven they are in the type 0/8/9 cells, not here.
* **No explicit collision mesh** in type 3. What type 3 gives is the drivable corridor: centre line,
  surface frame and width, plus the `BSDT` roadside polyline.
* **No lap or checkpoint markers**; those live in `ROADGRF<n>.TXT` (`[START_CHECKER]`).

---

## 8. The oracle check

`chunk.py verify` runs this automatically when `work\oracle\vr_capture\ramdumps\` is present.

1. **The chunk is resident verbatim.** In `ram_000200.bin` the sub-block area of type-3 objects 14
   and 15 is found unmodified; the containing chunk base is guest `0x8010D41C` and the word there is
   `0x3000000E`, the chunk's own key. (The header window list at `+0x04` *is* rewritten at runtime;
   the payload from `+0x20` is not.) Objects 14/15 are node 7 and object 67 is road 9, which is
   consistent - roads 9, 10 and 8 meet at node 7.
2. **A live vehicle lies on our centre line.** At guest `0x800CF718` there are eight 12-byte 16.16
   world points - the oriented bounding box of traffic-vehicle slot 0 (pool 3 base `0x800CF660` +
   `0xB8`, see `population.md` 1.3). Across all 14 dumps each point moves 6.0..7.0 world units per
   capture, its Y tracks the road's own elevation (-22.9 -> -18.1), and its horizontal distance to
   the nearest slice of the resident object 15 stays between **6.4 and 11.7 world units** - i.e. on
   the tarmac, given a half width of ~14.6. Nothing was fitted: the points are read raw and compared
   against coordinates decoded from the disc.

That is the proof that the decoding is right in absolute world coordinates, not merely
self-consistent.

---

## 9. Tool

`tools\scout\chunk.py` (Python 3.12, no dependencies):

| command | what it does |
|---|---|
| `info <file> [chunk]` | list every chunk with key/type/group/residency, or decode one fully |
| `scan <dir>` | type histogram over every `.STR`/`.STP` in a directory |
| `road <file> <out.svg>` | top-down centre line, plus a `.png` beside it |
| `road <file> <out.obj> [ids...]` | the road surface as a quad strip |
| `route <dir> <set> <race> <out.svg>` | one race's route highlighted on the network |
| `verify [dir]` | the 13 checks above, re-runnable |

```
python tools\scout\chunk.py verify work\disc_us\DATA
python tools\scout\chunk.py road  work\disc_us\DATA\STREAM1.STR work\chunk\set1_network.svg
python tools\scout\chunk.py road  work\disc_us\DATA\STREAM1.STR work\chunk\road26_surface.obj 91 92 93 94 95
python tools\scout\chunk.py route work\disc_us\DATA 1 20 work\chunk\set1_race20_route.svg
```

---

## 10. Probe outputs (`work\chunk\`, gitignored)

| file | what |
|---|---|
| `chunk_dump.txt` | `scan`, `verify` and a fully decoded chunk of each type from `RACE1_1.STP` |
| `set1_network.svg` / `.png` | all 113 road objects of set 1, top down; junction fans in red |
| `set2_network.svg` / `.png` | the same for set 2 |
| `set1_race1_route.svg`, `set1_race20_route.svg`, `set2_race5_route.svg` | one race's route on the network |
| `set1_race20_route.png` | the 8-road route of set 1 race 20, drawn as one continuous path |
| `race1_1_road.svg` / `.obj` | the single road object preloaded by `RACE1_1.STP` |
| `road26_surface.obj` | road 26, all five objects (four chunk boundaries), 968 vertices / 479 quads |

The acceptance test: `set1_race20_route.png` is a connected 15870-world-unit path through 8 roads
and 7 intersections, assembled from 32 polylines that come from 20 separate 16 KiB chunks, joined
only by the `GRPT` start/end chaining - no smoothing, no fitting. Its `RGTS` length sum is
1257359 road units = 19646 world units, and the road-piece objects account for 15870 of that
(80.8 %), the remainder being the junction fans which are separate objects.

---

## 11. Unknown

* `SLCT +0x24` (`i16` + two `i8`) - varies smoothly along the road, plausibly curvature or a
  lighting/shade term. `SLCT +0x30` - a `u32` that is constant within some objects (`0x6B61`) and
  steps by ~256 in others.
* `SLCT +0x2C`: four bytes that read as two `(first, count)` runs, almost certainly into `BGDT` and
  `BSDT`, but the target array of each is not pinned.
* `XSDH`'s 44-byte record layout. Only the lateral offsets are readable; the six records per
  cross-section index are a fixed table whose slots are not identified, so the road half width is
  measured (extreme offset) rather than read.
* `XSAI` fields 2..7; `XSIH` fields 0..1.
* `BSDT` `flags` (`+0x00`), `a`, `b` and the two trailing `u32`; whether the polyline is the road
  edge, a wall or a shadow strip.
* `BGDT`'s first six `u16`; `BZDT` and `NMBD` entirely.
* Type-3 fixed header `+0x30`..`+0x8B`: it holds small counts that mirror the block record counts
  and three large values that also appear in `GRPT`/`SUBT`, but the field split is not established.
* The type 0/8/9 payload past the 4-slot extent array is covered by `scene_cell.md`, not here.
* `STEN` / `OFFS` / `HORZ` semantics; the panorama's projection.
* The `0x80` group step: `group` distinguishes 13 resource banks in set 1, but what selects a bank
  at runtime is not traced. It is the same `0x80`-stepping key space as `PMTS` and `CTLR`
  (road.md 2.1, 4), with the type nibble explained.
* `PANO` fields `a` and `b`; the meaning of the three-word prologue's first word (always 3).
