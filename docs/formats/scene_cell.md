# The scene cell - chunk types 0, 8 and 9 (Road Rash: Jailbreak, USA, SLUS_01053)

This document covers the payload of the three scene-cell resource types of the `0x4000` world
chunk. The chunk *header* (`key`, resource type, residency windows) and the road geometry
(type 3) are in `docs\formats\road_chunk.md`; nothing here restates them. The model container the
cells reference is `docs\formats\rmd3.md`.

**Headline.** A scene cell is the world outside the tarmac, baked. It is **not** a list of model
instances with transforms; it is one chunk-local vertex pool plus two textured triangle/quad lists
that draw the terrain, the road surface and the buildings, **plus** a separate five-array table of
*placed objects* - each with a 16.16 world position, a unit orientation vector, a signed lateral
offset from the road centre line and an owning road-piece key. Those objects are what the race
populates the world with: the prop's class field is a **group index into model id 200**
(`HAZARD<n>.GEO`, 39 groups), and the cell's collision is not a mesh but the kind-6 array of
primitive volumes (section 6, `population.md` 4).

* Type **0** = a complete, self-contained cell.
* Type **8** = a cell whose last region is too big for `0x4000` and is shipped separately.
* Type **9** = that separate region, for the type-8 cell with the **same resource id**.

Confidence marks follow `road_chunk.md`: **[proven]** = an exact identity over the whole data set,
or read straight out of the dispatching code, or reproduced against live RAM; **[established]** =
read once out of our disassembly and holding over every sample; **[probable]**; **[guess]**.
Section 10 is the explicit unknown list. Section 11 is the cell residency as read in RAM.
**Section 12 is the texture binding** - which container and which palette a cell primitive samples,
with its own unknown list in 12.8. **4.1.1** is the draw dispatcher (which band is drawn when),
**4.1.2** decodes region 3, **4.3** gives the quad corner order and **12.7.1** measures what the
wrong corner order does to a frame. Section 13 is how the original draws a cell, section 14 which
cells a view draws.

Binaries cited (our own SHA-1 over `work\disc_us`):

* `SLUS_010.53`, `67ed165a2c517d4e6106fb0dfa324d66dd9a76f1`, text at `0x80010000`
  (file offset `f` -> address `0x80010000 + f - 0x800`).
* overlay `RASHCDG.BIN` (the race), `cfe43a7786759f2cb9c57751cf99e84d1074782c`, loaded at
  `0x8005B5E8` (file offset 0 = that address).
* overlay `RASHCDI.BIN` (the asset loader), `9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06`, same base.

Data cited: `work\disc_us\DATA\STREAM1.STR`, `STREAM2.STR`, the 99 `RACE<set>_<race>.STP` files, and
the savestate RAM images under `work\oracle\state\`.

Parser: `src\rrformats\cell.{h,cpp}` (gate "scene cells: every cell parses and its indices stay in
range", `rrtool cellcheck`). Independent probe: `tools\scout\cell.py` (outputs in `work\cell\`; gate
"probe: scene cell invariants"). Re-run everything with

```
python tools\scout\cell.py verify work\disc_us\DATA
```

---

## 0. Census

`python tools\scout\cell.py scan work\disc_us\DATA`, over both streams and all 99 `.STP`:

| | type 0 | type 8 | type 9 |
|---|---|---|---|
| chunk occurrences | 1742 | 1138 | 995 |
| distinct resource ids | 916 | 617 | 540 |

Within one file the type-8 and type-9 id sets coincide: in `STREAM1.STR` both are the same 227
ids; in `STREAM2.STR` 101 of the 123 type-8 ids have their type-9 half in the same file and the
other 22 do not (they are picked up from a `.STP`, or the cell simply runs without it, see 3.2).
**No type-9 id ever appears without a type-8 id of the same value**, in any of the 101 files that
contain both (`cell.py verify` check 15) **[proven]**. The type-0 id space is disjoint from the
type-8/9 id space in both streams.

Placed objects. Over the two `.STR` files, which hold every distinct cell of a set exactly once:
**2833** props (kind 4), **7584** kind 6, **353** kind 2, **0** kind 3, **0** kind 0 - 10770
records in total. Counting every file with per-file de-duplication (`cell.py scan`, which therefore
counts a resource once per file it appears in) the totals are 5146 / 13429 / 587.

---

## 1. Where the payload starts, and the three dispatcher arms

`road_chunk.md` 1.1 already gives the type table. The parts that matter here, all read out of the
EXE:

* `0x80031560` sets the resource record's payload pointer. For **types 0 and 8** (`0x800315A0`)
  it stores `rec+0x10 = chunk + 0x20` **and** `rec+0x18 = chunk + 0x20 + (word[chunk+0x20] * 4 - 48)`
  (`0x800315B0`..`0x800315CC`). For **type 9** it takes the default arm `0x800315D0`:
  `rec+0x10 = chunk + 0x20`, `rec+0x18 = 0` **[proven]**.
* Types 0 and 8 enter at `0x80031784`, which first calls `0x80031E1C` and then falls into
  `0x800317B4`; type 9 enters at `0x8003176C`, which jumps to the same `0x800317B4` with
  `a3 = 0` and the fifth argument `0`. Both land in **`0x80032A20`** with
  `(a0 = payload, a1 = resourceId, a2 = type, a3 = rec+0x18, arg4 = rec+0x1C, arg5 = ...)`
  **[proven]**.

### 1.1 The cell slot table - `0x800D87E8`, 24 slots of 112 bytes **[proven]**

`0x80032A20` keeps the resident cells in a fixed table. `0x80032AC4`..`0x80032AD8` builds
`0x800E0000 - 30744 + 112*i`, i.e. base **`0x800D87E8`** (*not* `0x800E87E8`, see 11), and the
slot count is capped by `slti v0,v0,24` at `0x80032A80` on the counter at `gp+0x250`.

| off | field | evidence |
|---|---|---|
| +0x00 | resource id (28 bit) | `0x80032AE8` |
| +0x04 | pointer to the cell **body** (2.) | written by `0x8003234C` at `0x800323E4` |
| +0x08 | resource id again | `0x80032AF4` |
| +0x0C | flags: bit0 = loaded, bit1 = region 7 present, bit2/bit4 = passes done | `0x80032B34`..`0x80032B48`, `0x80033F88`, `0x80034070` |
| +0x44 | `rec+0x1C` of the resource record | `0x80032B70`..`0x80032B78` |
| +0x48 | `B + 1` (u8) | `0x800323E8`..`0x800323F4` |
| +0x49 | `A + 2B` (u8) | `0x800323F8`..`0x8003240C` |
| +0x4C | pointer to the cell's 4-slot extent array | `0x80032AFC` / `0x80032B08` (`= rec+0x18`) |
| +0x50,+0x54 | the header texture pair values **without** bit 15 | `0x800323AC`..`0x800323C0` |
| +0x58,+0x5C | the header texture pair values **with** bit 15 | `0x8003238C`..`0x800323A8` |
| +0x60,+0x68 | resolved texture-page descriptors | filled by `0x800325BC` |

Read back from `work\oracle\state\rr-race\ram.bin`, 10 of the 24 slots are in use and every one of
them has `+0x04` equal to `chunkBase + 0x20 + nWords*4` and `+0x4C` equal to
`chunkBase + 0x20 + nWords*4 - 48` for the chunk found byte-for-byte in that same RAM image
(`cell.py verify`, oracle section) **[proven]**.

---

## 2. Payload layout

Offsets are inside the `0x4000` chunk.

```c
// --- types 0 and 8 -------------------------------------------------------
u32  nWords;                          // +0x20
u16  texPair[2 * (nWords - 13)];      // +0x24   packed texture ids
struct { u32 road, from, to; } extent[4];   //      0xFFFFFFFF = empty slot
CellBody body;                        // +0x20 + nWords*4
```

`nWords` is what `0x800315B0` reads. The pair area holds `nWords - 13` words, because the fixed
tail is exactly twelve words (the 4x3 extent array) plus the count word itself: the scanner at
`0x8003237C` runs `(nWords - 13) * 2` **u16** steps from `chunk+0x24` (`0x80032364`..`0x8003236C`)
and the body starts at `chunk + 0x20 + nWords*4` (`0x800323D4`..`0x800323E4`) **[proven]**.
Observed `nWords` is 14 (676 cells) or 15 (228), i.e. one or two pairs.

### 2.1 `texPair` **[proven]**

Each pair is `(0x8000 | t, t)` for the same `t`. The loader files the two values with bit 15 set
into slot `+0x58/+0x5C` and the two without into `+0x50/+0x54`, skipping `0xFFFF`
(`0x8003237C`..`0x800323C4`). `t` is the *packed* texture id that the type-1 dispatcher builds,
`((id >> 23) & 0x1F) << 10 | (id & 0x3FF)`, and the `0x8000` variant is the type-2 form
(`road_chunk.md` 1.1). Worked example, `STREAM1.STR` chunk 51 (key `0x8300007E`): the pair is
`(0x987E, 0x187E)`; `0x187E` unpacks to `(id >> 23) = 6`, `id & 0x3FF = 0x7E`, i.e. resource id
`0x0300007E` - the texture resource with the **same id as the cell**. In `work\oracle\state\rr-race`
slot 1 holds `+0x50 = 0x0C7C`, `+0x58 = 0x8C7C`, exactly the pair on the disc.

### 2.2 `extent[4]` **[established]**

Twelve words, four `{road, from, to}` slots, `0xFFFFFFFF` for an empty slot, the same shape as the
chunk-header residency windows but in **road units** (`/64` = world units) rather than world units.
`STREAM1.STR` chunk 51: `(13, 0, 7517) (12, 132501, 138145) (17, 121440, 133880)` = roads 13, 12
and 17, which are exactly the three roads meeting at node 9 - and the chunk's own header windows
name the same three roads (`road_chunk.md` 1.2). The extent is what the slot keeps a pointer to at
`+0x4C`, so it is the cell's residency description, not a placement.

### 2.3 `CellBody` - the 0x40 byte body header

```c
u32  unk00;                  // +0x00
u16  A;                      // +0x04
u16  B;                      // +0x06
i32  origin[3];              // +0x08  world units * 64
i32  vec2[3];                // +0x14  world units * 64, not identified
u32  region[8];              // +0x20  offsets relative to chunk+0x20
// region[0] is always (body - (chunk+0x20)) + 0x40, i.e. the header ends here
```

* **`origin` is the cell origin in world coordinates** **[proven]**. `RASHCDG 0x800A8584`..
  `0x800A85B8` loads the three words at `body+0x08/+0x0C/+0x10` and writes each one shifted left
  by 10 into a three-word world vector, i.e. `world16_16 = stored << 10`, i.e.
  `worldUnits = stored / 64`. 64 is the same road-unit-per-world-unit factor `road_chunk.md` 2.5
  proves for the road.
* `A` and `B` are the two counts the whole cell is organised around. `B` is 3 in 900 of the 904
  distinct objects of the two streams and 4 in the rest; `A` is 0..7. The loader copies `B+1` to
  slot `+0x48` and `A + 2B` to slot `+0x49` (`0x800323E8`..`0x8003240C`), and the draw pass at
  `0x800340A0` iterates the primitive-group table from `A+B` while `< A+2B` **[proven]**.
* `region[0..7]` are **relative offsets that the loader turns into absolute pointers in place**:
  `0x8003240C`..`0x800324C0` rewrites `body+0x24, +0x20, +0x28, +0x2C, +0x30, +0x34, +0x38, +0x3C`
  as `chunk + 0x20 + offset`. This is the same in-place relocation the `RMD3` loader does to
  `DOD3` (`rmd3.md` 2) **[proven]**.

**The relocation is reproduced exactly against live RAM.** In `work\oracle\state\rr-race\ram.bin`
eight cell chunks sit verbatim; **61 of 64** region pointers equal `chunkGuestBase + 0x20 + discOffset`
to the word, and the other three are the type-8 `region[7]` special case of 3.2 - `0` for one cell
whose type-9 half is not resident, and the type-9 chunk's own payload address for the other two
(`cell.py verify`) **[proven]**.

### 2.4 The eight regions

| region | body field | content | typical size |
|---|---|---|---|
| 0 | +0x20 | **object / placement table** (5.) | 56 .. 3436 |
| 1 | +0x24 | vertex bank table, 4 (900 cells) or 5 (4 cells) `{u32 byteOffset; u32 count;}` | 32 / 40 |
| 2 | +0x28 | **primitive group table**, `A + 3B` records of 12 bytes (4.1) | 108 .. 192 |
| 3 | +0x2C | u16 index lists, 5 offsets then data, `0xFFFF` terminated - **undecoded** | 44 .. 60 |
| 4 | +0x30 | `u8 tag; u8 len; u8 data[len]` records with tags `0x81..0x96`, preceded by an untagged run - **undecoded** | 64 .. 420 |
| 5 | +0x34 | `u32` then the **vertex array**, 8 bytes per vertex (4.2) | 1812 .. 7972 |
| 6 | +0x38 | **primitive list, band 0** (4.1) | 720 .. 8028 |
| 7 | +0x3C | **primitive list, bands 1 and 2**; for type 8 this is the type-9 chunk (3.2) | up to ~8.4 KiB |

Region 5's four-byte prefix is skipped by the consumer: `RASHCDG 0x800A8564` does
`lw v0,52(body); addiu v0,v0,4` before handing the array on, so the vertices start at
`region5 + 4` **[proven]**.

Region boundaries are exact. `cell.py verify` asserts over **1533 cell instances** across both
streams and all 99 `.STP` files, with **zero failures** on every one of these:

1. the body header fits in the chunk;
2. `region[0] == (body - chunk-0x20) + 0x40`;
3. the eight offsets ascend;
4. `sizeof(region 2) == 12 * (A + 3B)`;
5. region 1's bank offsets are the running prefix sum of its counts, times 8;
6. `8 * sum(bank counts) + 4 == sizeof(region 5)`;
7. `sizeof(region 6) == sum over the first A+B primitive groups of (tri*20 + quad*24)`;
8. region 0's five sub-array offsets ascend;
9. each region-0 sub-array is exactly `count * stride` bytes;
10. region 0 repeats the cell's own 28-bit resource id at `+0x0C`.

---

## 3. What each of the three types is for

### 3.1 Type 0 - a complete cell **[proven]**

`0x80032A20` allocates a free slot, calls `0x8003234C` (the header/relocation pass above), then at
`0x80032B40` sets `slot+0x0C |= 3` - both "loaded" **and** "region 7 present". Nothing further is
awaited.

### 3.2 Type 8 - a cell missing its region 7, and type 9 - that region

For a type-8 cell `0x80032A20` takes the other branch at `0x80032B4C` and executes

```
v0 = *(slot + 4);          // the body
*(v0 + 0x3C) = 0;          // region[7] = NULL
```

leaving `slot+0x0C` with bit 1 clear. A **type-9** chunk takes the lookup path at `0x80032B7C`:
it searches the slot table for the **same resource id**, requires the slot to exist with a body and
with bit 1 still clear, then calls `0x80032338`, whose whole body is

```
v0 = *(a0);                // a0 = &slot[+4], so v0 = the body
*(v0 + 0x3C) = a1;         // a1 = type-9 chunk + 0x20
```

and sets bit 1. **So the type-9 chunk's payload *is* the type-8 cell's region 7** **[proven, from
the code and from live RAM]**.

Two independent confirmations on our bytes:

* **Arithmetic.** For all **540** type-8 cells that have their type-9 half in the same file, walking
  the `A+B .. A+3B` primitive groups from offset 0 of the type-9 payload reproduces each group's
  stored byte offset exactly, with 0 mismatches, and the total lands inside the chunk
  (`cell.py verify`). Example, `STREAM1.STR` chunk 51 / chunk 57 (ids `0x0300007E`): bands 1 and 2
  need **8396** bytes; the type-8 chunk has only 4772 bytes left after region 6, while the type-9
  chunk has 16352 - which is precisely why the split exists.
* **Live RAM.** `rr-race` holds the type-8 cell `0x01800001` at guest `0x8011941C` and its type-9
  half at `0x8016141C`; the relocated `body+0x3C` in RAM is `0x8016143C` = that chunk `+0x20`.
  A second resident type-8 cell, `0x01800057`, has `body+0x3C == 0` because its type-9 half is not
  resident - exactly the `sw zero,60(v0)` above.

So the three types are **one job split three ways**: type 0 is a cell small enough to ship whole,
type 8 + type 9 are a cell that is not. They are not three different kinds of content.

---

## 4. The drawn geometry

### 4.1 Region 2 - the primitive group table, and the three bands

`A + 3B` records of 12 bytes:

```c
u32 byteOffset;   // into the primitive list this group belongs to
u32 triCount;     // 20 byte records
u32 quadCount;    // 24 byte records
```

`byteOffset` is the running sum of `triCount*20 + quadCount*24` inside its band, restarting at 0
for each band **[proven, checks 7 and the type-9 check]**. The three bands are

| band | records | primitive list |
|---|---|---|
| 0 | `[0, A+B)` | region 6 |
| 1 | `[A+B, A+2B)` | region 7, from offset 0 |
| 2 | `[A+2B, A+3B)` | region 7, continuing |

Band 1 is the one the texture fix-up pass walks: `0x800340A0` computes `s5 = A + B`, requires
`s5 < A + 2B`, indexes region 2 by `s5*12`, reads `triCount` at `+4` and `quadCount` at `+8`, and
advances `s5` by 1 and the region-2 cursor by 12 at `0x800343F8`..`0x8003441C` **[proven]**.

Band 2 is not touched by the fix-up pass, and every band-2 primitive in both streams uses the
texture reference `0x7C00` (25190 primitives; `0x7C00 = 31 << 10`, outside the packed-id range the
cell's own pairs use) **[established]**.

### 4.1.1 Which band is drawn - the dispatcher, `RASHCDG 0x80068FCC` **[proven]**

Overlay `RASHCDG.BIN`, sha1 `cfe43a7786759f2cb9c57751cf99e84d1074782c`, loaded at `0x8005B5E8`.
(`0x800A85FC` does not pick a band-1 group; 4.1.2 says what that code does.)

`0x80068FCC(ctx)` draws one resident cell. `ctx+0x00` is the cell body, `ctx+0x04` an id compared
against the "cell the camera is in" record, and **`ctx+0x34` is a 32-bit control word read four bits
at a time**. The function has exactly two loops:

```
A = *(u16*)(body+4);  B = *(u16*)(body+6)
for k in [0, A):                                  # 0x8006901C .. 0x80069058
    0x8006D350(ctx, k)                            #   region 6, group k
for k in [0, B):                                  # 0x800690E8 .. 0x800691E0, s2 = 4*k
    n = (*(u32*)(ctx+0x34) >> (4*k)) & (*(u32*)0x800CC86C ? 0x0D : 0x0F)
    if !(n & 1): continue                         # 0x80069124  the group is skipped
    if  (n & 2):                                  # 0x8006912C  the FINE pair, region 7
        (n & 4) ? 0x8006E474(ctx, A + 2B + k)     #   band 2, quads   (0x80069150)
                : 0x8006F5D0(ctx, A + 2B + k)     #                   (0x8006918C)
        (n & 4) ? 0x8006A630(ctx, A + B + k)      #   band 1, tris    (0x80069168)
                : 0x8006C888(ctx, A + B + k)      #   band 1, tris+quads (0x800691A4)
    else:                                         # the COARSE group, region 6
        (n & 4) ? 0x8006D350(ctx, A + k)          #                   (0x800691BC)
                : 0x8006DC20(ctx, A + k)          #                   (0x800691CC)
```

The two arms jump to the same loop tail, so they are **exclusive**: a group is drawn either coarse
or fine, never both. Each leaf takes `(ctx, groupIndex)`, indexes region 2 by `groupIndex*12`, and
reads `group+4` (triangle count, 20-byte records) or `group+8` (quad count, 24-byte records) from
`body+0x38` (region 6: `0x8006D3D4`) or `body+0x3C` (region 7: `0x8006A6B4`, `0x8006E4D0`).

So, stated plainly:

* **groups `[0, A)` are always drawn coarse**, from region 6;
* **group `A+k` (region 6) and the pair `A+B+k` / `A+2B+k` (region 7) are the same piece of
  ground at two levels of detail**, and the nibble `k` of `ctx+0x34` picks which;
* the global at `0x800CC86C`, when non-zero, masks the nibble with `0x0D` and so forces **every**
  group coarse - the natural reading is "the fine half is not available", which is what a type-8
  cell whose type-9 chunk is not resident is (3.2), but that link is not traced **[probable]**;
* **band 2 is the quad half of the fine draw**, not a separate world. In the `n & 4` arm only
  band 1's triangles and band 2's quads are drawn; in the other arm band 1 contributes both.

The geometry agrees. Over 60 cells of `STREAM1.STR`, the XZ ground area of the coarse groups
`A+k` is 1 113 407 world units squared and that of the fine pairs 1 044 636 - **94 %**, of which
band 1 is 760 736 and band 2 283 900. Per group (`RACE1_1.STP` cell `0x00800027`): coarse `A+0`
covers 433 buckets of 4 world units, the fine pair 432, and 385 of them are shared.

`ctx+0x34` is slot `+0x38`, written once a frame by `SLUS 0x80035680` from the depth and frustum
code of region 3's sub-area polygons; the rule reproduces the `rr-race` slot words exactly (13.4).

### 4.1.2 What `RASHCDG 0x800A8498` really is: a point-in-cell test **[proven]**

`0x800A8498` is not a draw-time LOD selector. It takes a world point and a state word, and:

* `0x800A84DC`..`0x800A8528`: the sign of `*(state)` picks the half of the 24-slot cell table to
  search (slots 0..11 or 12..23) and `*(state) & 0x3F` is the slot, clamped into that half;
* `0x800A855C`..`0x800A85B8`: reads the slot's body, hands `region[5]+4` (the vertex array) to the
  caller and writes `origin << 10` - this is where 2.3's origin proof comes from;
* `0x800A85FC`..`0x800A8630`: `s1 = ((*state) >> 6) & 7`, clamped to `< B`, **indexes region 3**
  (`body+0x2C`, loaded into `s7` at `0x800A8580`);
* `0x800A8644`..`0x800A8750`: `count = region3[s1+2] - region3[s1+1]`, then walks `count` u16 entries
  from `region3[s1+1]`, looks each one up as a **vertex index**, shifts the vertex left by 10 into a
  16.16 polygon in the stack frame, and tracks the min/max of X and Z;
* `0x800A8754`..`0x800A87AC`: rejects the point on that bounding box and then calls the polygon test
  `0x800B6E08(point, polygon, count, 1)`;
* `0x800A87BC`..`0x800A87D8`: on success writes the slot back into the state word's low 6 bits and
  `s1` into bits 6..8.

So **region 3 is a table of `B` boundary polygons, one per fine group, given as vertex indices**, and
`(x >> 6) & 7` is "which sub-area of the cell the point is in", cached in the state word. It also
explains the `B`-sized control word of 4.1.1: the cell is divided into `B`
sub-areas and each has its own coarse/fine decision.

### 4.2 Region 5 - the vertex array **[proven]**

`u32` prefix, then 8 bytes per vertex: `s16 x, y, z, w`. `w` is **not** decoded (it is not a
constant pad: it spans -32624..32655).

`x, y, z` are in the same unit as the cell origin, i.e. **world units * 64**, and are relative to
the origin:

```
worldPosition = (origin + vertex) / 64
```

Tested against the independently decoded road: taking every 7th vertex of every 17th cell of set 1,
2111 samples, the horizontal distance to the nearest type-3 road slice is **median 19.1, p90 42.8**
world units - a terrain apron around a road whose half width is ~15 - and the Y matches the road's
own elevation (median difference -1.9 world units). At any other scale (`/32`, `/128`, `/256`) the
mesh does not land near a road at all.

Region 1 partitions this array into 4 **banks** (5 in 4 of the 904 cells), `{byteOffset, count}`, and the offsets
are the running prefix sum times 8 with 0 failures over all 1533 cells. Band-0 primitives index only
bank 0 (e.g. `STREAM1.STR` chunk 9: max index 208 against bank 0's 209 vertices) while band-1/2
primitives index the whole pool, so the banks look like per-LOD vertex sets **[probable]**. All
primitive indices are inside the pool in every cell (check 12).

### 4.3 The primitive records - 20 byte triangle, 24 byte quad **[proven]**

This is the `DPD3` primitive of `rmd3.md` 3.1 with a four-byte prefix, and with a triangle variant.

```c
// 20 bytes - textured triangle
u8  flags;      // +0x00
u8  pal;        // +0x01   palette / page selector handed to the resolver
u16 unk02;      // +0x02
u8  u0, v0;     // +0x04
u16 clut;       // +0x06   overwritten at load time
u8  u1, v1;     // +0x08
u16 texRef;     // +0x0A   overwritten at load time
u8  u2, v2;     // +0x0C
u16 i0, i1, i2; // +0x0E

// 24 bytes - textured quad: identical through +0x0D, then
u8  u3, v3;     // +0x0E
u16 i0, i1, i2, i3;  // +0x10
```

**The four corners of a quad go ROUND the quad, not in the PS1 strip order** **[proven]**. A PS1
`POLY_FT4` is drawn as the triangles `(v0,v1,v2)` and `(v1,v2,v3)`, i.e. `v3` is
diagonally opposite `v0`; a cell quad instead has `i2` opposite `i0`, so it splits as `(i0,i1,i2)`
and `(i0,i2,i3)`. The identity is over the whole data set - all 102 stream files, **545 770
quads**:

| band | quads | strip split `(0,1,2)+(1,3,2)` leaves the two triangles facing the same way | perimeter split `(0,1,2)+(0,2,3)` does |
|---|---|---|---|
| 0 | 155 461 | 89 | 154 495 |
| 1 | 349 525 | 192 | 347 873 |
| 2 | 40 784 | 0 | 40 418 |

(2 277 quads are degenerate - one of the two triangles has zero area - and 707 more satisfy neither
split, which is what a quad that is genuinely not planar looks like: terrain folded along a
diagonal. 545 770 = 542 786 + 707 + 2 277.) Under the strip split every quad is a **bow tie**: the two
triangles cover three of the four wedges the diagonals cut the quad into and leave the fourth empty,
with its apex at the quad's centre - a black wedge in the rendered terrain; 12.7.1 has the
before/after pixel counts. 13.5 gives the diagonal the GPU itself splits along. `u0..u3` are positionally tied to
`i0..i3`, so the UVs follow the same order and need no change.

The split is read straight off the fix-up pass at `0x800340DC`..`0x80034204` (triangle) and
`0x8003427C`..`0x800343F0` (quad), which for each record:

* reads `u16` at `+0x0A` into a scratch word and `u8` at `+0x01` into the byte above it
  (`0x800340DC`..`0x800340FC`; note `s1 = record + 6`, so `s1+4` is `+0x0A` and `s1-5` is `+0x01`);
* if `(texRef >> 10) & 0x1F == 30`, computes a VRAM page from the runtime page table (the
  `0x80053254` / `0x800533B4` configuration, 12.2)
  and then **adds a bias byte to the V of each corner** - at `+0x05`, `+0x09`, `+0x0D` for the
  20 byte form and additionally `+0x0F` for the 24 byte form. Three V bytes for the short record
  and four for the long one is what makes them a triangle and a quad;
* otherwise calls the generic resolver `0x80022758`;
* writes the resolved CLUT back to `+0x06` and the resolved tpage back to `+0x0A`
  (`0x80034204`..`0x8003421C`).

On disc the fields therefore hold placeholders and references, not GPU words. Over both streams:
`clut` is `0xFFFF` (374887 primitives), `0x0CBF` (24932) or `0x0BFF` (258); `texRef` is one of the
**cell's own header pair values** (365623), `0x7C00` (25190) or `0x7800` (9264) - and `0x7800` is
`30 << 10`, exactly the value the fix-up pass special-cases. `cell.py verify` checks 13 and 14
assert this set over all 1533 cells with 0 failures **[proven]**.

**Section 12 resolves all of this**: which image `texRef` names, which palette `pal` selects, and
what overwrites `clut`. In short - `texRef` is a key into the cell's own header pair, band 0 reads
the bit-15 half of that pair and band 1 the other half, the halves name the **type-2** and
**type-1** chunks of the same resource id sitting next to the cell in the stream, and `pal` is a row
in the 32-palette block that ships inside the image itself.

### 4.4 What the mesh looks like

`cell.py plot` on `RACE1_1.STP` draws its six cells top down, with the road
centre line decoded **independently** from the type-3 chunks laid over them. The six cells tile a
continuous ~2400 world unit ribbon of road 3: a dense strip of small polygons follows the centre
line exactly (the road surface and verges) and coarse terrain panels fan out 300+ units to each
side. Nothing was fitted - the mesh is `(origin + vertex)/64` and the road comes from a different
chunk type.

---

## 5. Region 0 - the object / placement table

This is the answer to "where does the race get its populated world from".

```c
// region 0 header, 0x38 bytes
u16 count[5];     // +0x00,+0x02,+0x04,+0x06,+0x08   one per sub-array
u16 0xFFFF;       // +0x0A
u32 resourceId;   // +0x0C  == the cell's own 28 bit id (check 10)
u32 unk10[5];     // +0x10..+0x23
u32 arrayOffset[5]; // +0x24,+0x28,+0x2C,+0x30,+0x34  relative to region 0
// then the five arrays back to back, in that order
```

The five arrays, their strides and their "kind" numbers come from the classify helper at
`RASHCDG 0x8009C41C`, which switches on the kind through the 7-entry table at `0x8005B8BC` and, per
kind, multiplies the index by 68 / 76 / 64 / 88 / 64 and adds `*(region0 + 0x24 / 0x28 / 0x2C /
0x30 / 0x34)` (`0x8009C474`, `0x8009C48C`, `0x8009C4AC`, `0x8009C4B8`, `0x8009C4D8`) **[proven]**:

| kind | array offset | stride | records in the two `.STR` files |
|---|---|---|---|
| 3 | +0x24 | 68 | **0** |
| 2 | +0x28 | 76 | 353 |
| 4 | +0x2C | 64 | 2833 |
| 6 | +0x30 | 88 | 7584 |
| 0 | +0x34 | 64 | **0** |

`cell.py verify` check 9 asserts `arrayOffset[k+1] - arrayOffset[k] == count[k] * stride` for all
five arrays of all 1533 cells, 0 failures **[proven]**.

The walker is `RASHCDG 0x8009CB90`..`0x8009CEEC`. For each array it reads the count from
`region0 + 0/2/4/6/8`, the base from `region0 + 0x24..0x34`, and per record: skips it if
`(s16)record[+0x04] > 0` ("already spawned"), runs a visibility/range test `0x8009C4FC`, calls the
spawner `0x8009C654`, and finally stores the new entity handle back into `record[+0x04]` through
`0x8009C41C` (`sh a3,4(rec)`). That is why `+0x04` is `0xFFFFFFFF` on disc in **every** record of
both streams **[proven]**.

### 5.1 The common record prefix **[proven]**

Every record of every array begins the same way:

```c
u16 kind;        // +0x00  equals the array's kind number
u16 cls;         // +0x02  class / selector, see below
u32 spawned;     // +0x04  0xFFFFFFFF on disc, the runtime entity handle in RAM
u32 pieceKey;    // +0x08  a PMTS road-piece key (0x7xxxxxxx / 0x6xxxxxxx)
u16 unk0C;       // +0x0C  always 2
i16 n[3];        // +0x0E  orientation, a unit vector, 4096 = 1.0
i32 pos[3];      // +0x14  world position, 16.16, Y down
i32 lateral;     // +0x20  signed offset from the road centre line, 16.16 world units
i32 along;       // +0x24  distance along the road, 16.16 world units
u32 zero[2];     // +0x28, +0x2C
```

* `kind` equals the array it is in, in **all 10770 records** of the two streams (check 11).
* `n` has length 4096 +-8 in **all 10770 records** (check 11) - it is a genuine unit vector. For
  kinds 2 and 4 the Y component is **exactly 0** in 100 % of records (353 and 2833), i.e. those
  objects are upright and carry only a yaw; for kind 6 it tilts, up to 0.583.
* `pieceKey` is a `PMTS` key in the same key space as `SEG_` (`road_chunk.md` 2.6); 59 distinct
  values in set 1.
* `pos` is a world position: over all **6975** objects of set 1 the horizontal distance to the
  nearest type-3 road slice is **median 14.2, p90 23.3, p99 35.3** world units.
* **`lateral` is the signed lateral offset, and it agrees with the road geometry.** Take the
  nearest road slice, project `pos - slicePos` on the slice's lateral axis (`SLCT` matrix row 0)
  and compare with `record[+0x20]`: over 6971 objects the residual is **median 0.056, p90 0.255,
  p99 4.893 world units**, the remainder being the 19..32 unit slice spacing. The sign is the
  **opposite** of `chunk.py`'s `Slice.right()` **[proven]**. Cross-check inside one cell
  (`RACE1_1.STP` chunk 12): records 1 and 2 are 22.5 world units apart in X at almost the same Z,
  and their `lateral` values are `+11.26` and `-11.24`.
* `along` is a distance along the road in world units. Where the road attribution is unambiguous it
  equals the `SLCT` `distance` of the nearest slice: 163 of 398 cells of set 1 have a p10..p90
  spread of `SLCT.distance - record[+0x24]` under 30 world units (about one slice spacing) with a
  median offset near 0. For the rest the offset is large and constant-ish per road piece, so the
  origin of the measurement is per-piece or per-road and is **not pinned** **[probable]**.

**This answers the placement question directly:** a placed object is
`(world position, unit orientation, lateral offset, along-road distance, owning road piece)`,
all absolute, no reconstruction needed.

### 5.2 Kind 4, 64 bytes - the roadside props, and the model id **[proven]**

Tail beyond the common prefix:

| off | value |
|---|---|
| +0x30 | `0x0009C400` (= 9.766 world units, the `XSAI` lane width) in 2098 of 2268 set-1 records, `0x0000FA00` (0.977) in the rest |
| +0x34 | `0xFFFFFFFF`, all records |
| +0x38 | `0x000001FF`, all records |
| +0x3C | a small integer, 0..~0x1FF |

The spawner arm is `RASHCDG 0x8009C85C`, and it switches on `cls`:

* `cls == 50` (19 records): `0x80012BA8(record, ..., pool)`, gated on a state flag.
* `cls == 9` or `cls == 0` (19 records): a separate path at `0x8009C8AC` that requires a counter at
  `0x8005B314` to be `< 3`, matches `cls` against a two-entry byte table at `0x8005B328`, finds a
  free entry in a 6-slot pool of 312-byte entries at `*(0x8005B24C)`, and calls
  `0x800A0A20(cls, 1, record + 0x14, 0, poolEntry)` - note `record + 0x14` is the world position.
* **everything else**: `0x8009C5E4(cls)`, which does

```
idx  = *(s16*)0x800CE592;            if (idx == -1) return -1;
reg  = 0x800CE1B0 + idx * 16;        // the model registry, 16 bytes per model
if (cls >= (u8)reg[+0x04]) return -1;            // groupCount
dod3 = *(u32*)(*(u32*)(reg + 0x08) + cls * 12);  // group descriptor -> DOD3
return (*(u16*)(dod3 + 0x0E) & 0x0F80) >> 7;
```

and then routes to `0x800A2630` if the returned code is `< 6` and to `0x800A2448` otherwise.

So **`cls` is a group index into one entry of the runtime model registry**. Which entry: in all
four savestates (`rr-race`, `rr-grid`, `rr-pack`, `quick`) `*(s16*)0x800CE592` is **12**, and
registry slot 12 is `modelId = 200, groupCount = 39` - and all six `HAZARD<n>.GEO` on the disc are
a single `RMD3` with `modelId 200` and `groupCount 39`, which `rmd3.md` 4 already identified as
"39 separate props, not LODs". The class histogram over both sets runs 1..38 plus 40, 41 and 50;
1..38 is exactly the legal group range **[proven]**.

That is the scenery placement: **prop = group `cls` of model id 200, at `pos`, yawed so that its
forward axis is `n`.** No scale field exists in the record; the prop's model unit is given in
5.2.1.

### 5.2.1 How a prop is sized, turned, chosen and drawn **[proven against rr-pack]**

`rr-pack` holds six live props (pool 4, `*(0x800CD6D4)`, 0x254 bytes each; class 20, the lamp posts)
and the original's frame of that state draws two primitives of one of them (`rrverify trace` +
`psxgpu.py`). Checked by `rrview --propcheck` (gate "render: the roadside props against the original's
packets (rr-pack)" and its four controls):

* **Unit.** A prop group has coordinate exponent 0 (`DOD3+0x0E` `0x0232`), so ModelVisible hands the GTE
  `(+0xB8 - eye) >> 10` - 1/64 world unit - and the .GEO vertices unscaled: **one prop model unit is
  1/64 world unit** (the entity's `+0x0C` is the record position x 64 exactly). A scale of 0.0046 per
  1/16 unit is 4.7 x too big and turns a curve's five chevron boards (class 25, 15 units apart - the
  data has them and the original shows them, at a fifth of that size) into rows of big road signs.
* **Rotation.** Part 0's matrix (`*(obj+4)+4`, what ModelDraw loads) is, for all six, the rows
  `(nz, 0, nx), (0, 1, 0), (-nx, 0, nz)` of the record's `n` (`+0x0E`, copied to `+0x1BC`): model Z is `n`.
  Negating model X is a reflection (mirrored sign faces, chevrons pointing the wrong way, the back
  plate of a pair facing the rider).
* **Faces.** The sign plates are one-sided front/back pairs (`rmd3.md` 3.3); drawing both z-fights.
  The renderer drops the side the emitter's NCLIP drops.
* **Draw range.** ModelVisible `0x80067CC8..0x80067D24`: a kind-6 object is invisible past
  `*(0x800CC6A4 + 24)` = 6400 of ViewDistance's `+0x2C` (100 world units), 38400 for group 0.
* **Which HAZARD file.** RASHCDI `0x8006383C` -> `0x8005C7F0` loads `HAZARD<s0 % 10>`: 0 in mode
  `(game_state+4 & 0x18) == 8`, 1 in mode bit 4, else `0x8006AD4C`'s `clamp(ENV.EN[0xCD + 2 (Rand() %
  ENV.EN[0xCC])], 1, 5)` - a random one of 1..5 per race (`ENV.EN` holds the five pairs `(1..5, 6)`).
  `rr-race` / `rr-pack` hold HAZARD4. The groups differ between the files only in group 0 and in their
  texture pages. (`race_scene.h` `PickHazardSet`; the product's seed is its own - named there.)
* **Class arms.** `0x8009C654`: class 50 -> `0x80012BA8` (mode bit 4 only), classes 0 and 9 -> the
  hazard-object pool (`0x800A0A20`), class 30 only while the view's bike is on its route
  (`0x8003B8F4`), every other class below 39 -> group `cls` of model 200.
* **Walker, spawn window, budget, release.** PORTED; the product draws the spawned props and
  `rrgame --worldcheck` rebuilds rr-pack's six props byte for byte. The rules: the spawn window
  (200 units, released at 230) and the pool-4 budget (8344 free bytes / 0x254 = 14 props alive, and 18
  part records shared with pool 5) - on race 1/20, 37 of its 491 props have more than 14 others
  within 200 units, and there the original shows fewer; class 30's route gate; the spawner's settling
  of `+0xB8` on the road (0.04 units in rr-pack).

### 5.3 Kind 6, 88 bytes - the static collision volumes

Tail beyond the common prefix: `+0x30 = 1`, `+0x34 = 0`, `+0x38 = 0` in all 4496 set-1 records;
`+0x3C` is a `u32` whose **high** half takes 22 small values (4 in 3837 of 7584, then 6, 5, 1, 0,
...) and whose low half is 0; `+0x40..+0x48` and `+0x4C..+0x54` are two 16.16 three-vectors, equal
in 2285 of 7584 records and different in the rest, with Z zero in 92 % of them and X/Y medians
0.64 / 3.55 world units - they are the volume's corner / half-extent vectors `A` and `B`
(`population.md` 4.1, 4.3).

`cls` is only ever 0 (1254 occurrences in the scan) or 1 (12175). The spawner is `0x8009C810`: a per-kind budget check
`0x8008CDF4(6)`, then `0x8009BB48(record, poolEntry)`, which allocates out of a pool of **280-byte**
entries at `*(0x800CD6A8 + 0x1C)` (`lui v0,0x800d; addiu a0,v0,-10584`) capped by `*(0x8005B214)`, keying on
`{pieceKey, (s16)record[+0x3C], record[+0x24]}`.

Laterally these sit further out than the props (median |lateral| ~15, peaks at +-10..25), and 37 %
of them have a tilted normal. **They are the static collision volumes of the baked scenery**:
`cls 1` a thin vertical prism (tree trunk, post, pole), `cls 0` a low box, instantiated into a
24- (or 32-) slot pool around the player and read by the crash resolver (`population.md` 4).

### 5.4 Kind 2, 76 bytes - pedestrians

353 records in the two streams, `cls` in {1, 19, 20, 22, 23} (23 alone is 431 in the scan). Normals strictly horizontal. Spawner
`0x8009C788`: budget check `0x8008CDF4(2)`, then it looks up an entity through the array of player
entity pointers at `0x8005B268` (indexed by player index), compares `record[+0x08]` against
`entity+0x168` and `record[+0x24]` against `entity+0x170`, and calls `0x800CB8C8(record, 1, entity)`,
the pedestrian spawner (`population.md` 6).

### 5.5 Kinds 3 and 0 - **empty on disc**

Both arrays have count 0 in **every** cell of both streams and all 99 `.STP` files. This matters,
because kind 0 is the only spawner in the whole table that is *probabilistic*: its arm at
`0x8009C6B0` calls the RNG `0x8001FC58`, reduces it mod 25 (the `0xBA2E8BA3` / `srl 3` idiom at
`0x8009C6CC`..`0x8009C6F0`), shifts left 16 and compares against `record[+0x30]` - and
`record[+0x30]` is the field that is `0x0009C400` / `0x0000FA00`, i.e. a **density**. On success it
allocates an entity via `0x80095848` and copies `record[+0x08]` (piece key) to `entity+0x168`,
`record[+0x24]` to `entity+0x170`, `(s16)record[+0x3C]` to `entity+0x16C` and `record[+0x20]` to
`entity+0x158`.

So the engine **has** a road-piece-keyed, density-driven spawner, and the shipped cells do not use
it. See 7.

---

## 6. Collision

**The cell's collision representation is region 0's kind-6 array** - 7584 primitive volumes in the
two streams (5.3, `population.md` 4) - **not a mesh**. No collision geometry distinct from the visual
geometry exists in the cell:

* The eight regions are fully accounted for by size. Regions 1, 2, 5, 6 and 7 are one vertex pool
  and the primitive lists that index it, and their sizes are *predicted exactly* by region 2
  (checks 5, 6, 7 and the type-9 reconstruction, 0 failures over 1533 cells). Region 0 is the
  object table and its size is predicted exactly by its own five counts (check 9). That leaves only
  regions 3 (44..60 bytes) and 4 (64..420 bytes) unexplained - two orders of magnitude too small
  to hold a collision mesh for a cell whose visual mesh is 300..800 vertices and 300..600 polygons.
* There is only one vertex array. Region 1 partitions region 5 exhaustively; there is no second
  pool, and every primitive index in every cell lands inside it.
* The things the player collides with are the **entities spawned from region 0**: the kind-6
  volumes, and the props and pedestrians whose shape travels with their model (`BBD3` bounding
  box, `rmd3.md` 1).
* The drivable surface is the type-3 road (centre line, surface frame, cross-section width),
  exactly as `road_chunk.md` 7 says.

No code doing polygon-level collision against the drawn region 5 / region 6-7 geometry was found.

---

## 7. Traffic, pedestrians, police

* **The cells carry no traffic or police spawn table.** The one array with a density
  field and an RNG-driven spawner - kind 0 - is empty in every cell on the disc (5.5).
* The cells *do* carry 5146 prop instances (kind 4), which resolve to groups of model id 200
  (`HAZARD<n>.GEO`) - the roadside obstacles.
* Traffic cars (`CAR<nn>A/B.GEO`, model ids 300..315), the pedestrian (`PED01A.GEO`, id 400) and the
  cop bikes are in the model registry at race time (`rr-race` has ids 301, 303, 306, 309, 400, 430
  resident) but **no cell names the cars or the cops**. They are spawned procedurally at run time
  by schedulers whose parameters are compiled in (`population.md` 3; the police spawner is
  `rules.md` 9.2), not by the `[VEHICLE_DEFAULT_DENSITY]` / `[VEHICLE_DEFAULT_RATE]` /
  `[REACTIVE_DEFAULT_DENSITY]` / `[REACTIVE_DEFAULT_RATE]` keys that `RASHCDI`'s parser knows
  (strings at `0x8005B990`, `0x8005B9AC`, `0x8005B9C4`, `0x8005B9E0`), which are absent from the
  shipped `ROADGRF<n>.TXT` (`road_chunk.md` 7). The run-time writer of the entity fields the kind-0
  arm fills (`+0x158`, `+0x168`, `+0x16C`, `+0x170`) is the road traversal of `population.md` 1.2.
* Pedestrians *are* placed by the cells: kind 2 (5.4).
* The class-0/class-9 path of kind 4 (5.2) spawns into a **six**-slot pool with a "< 3" gate, which
  is the right shape for something like cop or ambush placement, but that is a guess and it accounts
  for only 19 records.

---

## 8. The oracle

`cell.py verify` runs this automatically when `work\oracle\state\rr-race\ram.bin` is present.

1. **Eight cell chunks are resident verbatim** in `rr-race` (five type 0, three type 8), located by
   searching for a 64-byte window at chunk offset `0x1000`. Their `key` word in RAM matches the
   disc key including the type nibble.
2. **61 of 64 relocated region pointers** equal `chunkGuestBase + 0x20 + discOffset` to the word.
   The three exceptions are all the type-8 `region[7]` case and all behave as the code says
   (one null, two pointing at a type-9 chunk's `+0x20`).
3. **The cell slot table at `0x800D87E8`** has 10 of 24 entries live; for the 8 cells located
   on disc, `slot+0x04` and `slot+0x4C` match the predicted body and extent addresses exactly, and
   `slot+0x48`/`slot+0x49` equal `B+1` and `A+2B` computed from the disc bytes.
4. **The model registry** at `0x800CE1B0` holds `modelId 200, groupCount 39` in slot 12, and
   `*(s16*)0x800CE592` is 12, in all four savestates - which is what turns a kind-4 `cls` into a
   real prop model (5.2).
5. **Placement against the road.** 6975 objects of set 1 lie a median 14.2 world units from the
   nearest road slice, and only **118 of 6971 (1.69 %)** lie within 3 world units of the centre
   line. The lateral histogram is bimodal with a hole in the middle: the props line the road, they
   do not sit on it.

`work\oracle\vr_capture\scene.csv` is not used here: its `x,y,z` are the capture's own unprojected
screen-space triple (`H=237, OFX=192, OFY=120` in `scene.txt`), so predicting them needs the camera
matrix inverted as well as the cell decoded; the RAM-resident chunk comparison above is the more
direct proof. The draw stream is matched primitive by primitive in sections 12 and 13.

---

## 9. Tool and artifacts

`tools\scout\cell.py` (Python 3.12, no dependencies beyond the sibling `chunk.py`):

| command | what it does |
|---|---|
| `info <file>` | one line per cell: verts, primitive counts, object count, extent |
| `info <file> <chunk>` | full decode of one cell, all regions, all object records |
| `scan <dir>` | census + class histograms over every `.STR`/`.STP` |
| `place <file> <out.csv>` | every placed object with world position, orientation, lateral, along, and the distance to the nearest road slice |
| `obj <file> <chunk> <out.obj>` | the cell mesh in world units plus an oriented marker per object plus the road centre line |
| `plot <file> <out.svg>` | top down: road, cell mesh, objects coloured by kind (writes a `.png` too) |
| `verify [dir]` | the 15 structural checks, the type-9 reconstruction, the oracle and the placement tests |

The quad corner-order identity of 4.3, the band coverage figures of 4.1.1 and the texture checks of
12.4 / 12.5 were measured with separate analysis scripts over the same stream files.

```
python tools\scout\cell.py verify work\disc_us\DATA
python tools\scout\cell.py info   work\disc_us\DATA\RACE1_1.STP 12
python tools\scout\cell.py place  work\disc_us\DATA\STREAM1.STR work\cell\set1_placements.csv
python tools\scout\cell.py plot   work\disc_us\DATA\RACE1_1.STP work\cell\race1_1_cells.svg
```

Outputs in `work\cell\` (gitignored):

| file | what |
|---|---|
| `verify.txt` | the full verify run |
| `scan.txt` | census and class histograms over all 101 stream files |
| `cell_dump.txt` | a fully decoded type-0 cell and type-8 cell, plus the first records of a type-9 payload |
| `set1_placements.csv` | all 6981 placed objects of set 1 |
| `race1_1_placements.csv` | the 136 objects of the six cells of `RACE1_1.STP` |
| `race1_1_cell12.obj` | the single type-0 cell `0x00800027` on its own, 9923 triangles |
| `race1_1_cells.obj` | those six cells' meshes in world units, with object markers and the road centre line |
| `race1_1_cells.svg` / `.png` | the same, top down - the "do the trees line the road" picture |

---

## 10. Unknown

* `CellBody +0x00` (a 32-bit value that is not a count) and `+0x14` (three words in the same
  world*64 unit as the origin - a half extent, a second corner or a reference point; the numbers
  bracket the cell's own objects when read as a half extent about the origin, but not exactly).
* `region 3` (4.1.2 decodes it as `B` boundary polygons, one per fine group, each a run of vertex
  indices delimited by the `B+2` u16 offsets at its start): what the first offset (entry 0) is for,
  and the `0xFFFF` terminator.
* `region 4` entirely: an untagged byte run followed by `u8 tag; u8 len; u8 data[len]` records with
  tags `0x81,0x82,0x83,0x85,0x86,0x87,0x89,0x8A,0x8B,0x91,0x92,0x93,0x95,0x96`. All data bytes are
  small ordinals.
* The vertex `w` (`region5` record `+0x06`): not a pad, range -32624..32655; its low byte indexes the
  level colour table for bands 1 and 2 (13.1).
* The meaning of the 4 (sometimes 5) vertex **banks** in region 1, and what sets the global
  `0x800CC86C` that forces every group coarse (4.1.1).
* Primitive `flags` (`+0x00`) and `unk02` (`+0x02`). (`pal` at `+0x01`, `texRef` at `+0x0A` and the
  on-disc `clut` at `+0x06` are section 12; `clut` is a placeholder the loader overwrites, and its
  three on-disc values are still unexplained but are never read. The flags' texture-window use is
  13.2 and the low byte of `+0x02` the colour index of 13.1.)
* Region 0 header `+0x10..+0x23` (five words; small counts for some cells, large values for others).
* Object record `+0x0C` low half (always 2), and `+0x24`'s origin (5.1).
* Kind 4 tail `+0x30` (lane width / a tenth of it), `+0x34` (`0xFFFFFFFF`), `+0x38` (`0x1FF`),
  `+0x3C`.
* Kind 6 tail `+0x3C` high half (22 values; `population.md` 4.3).
* The 68-byte (kind 3) and the second 64-byte (kind 0) record layouts - no instance exists on the
  disc to read.

---

## 11. Residency in RAM

1. **The cell slot table is at `0x800D87E8`.** The instruction at `0x80032AC8` is
   `addiu s6,v0,-30744` with `v0 = 0x800E0000`, and `0x800E0000 - 30744 = 0x800D87E8`. At
   `0x800D87E8` in `work\oracle\state\rr-race\ram.bin` the 24x112 table is fully legible and its
   pointers match the resident chunks, while `0x800E87E8` holds unrelated bytes.
2. **The extent array of chunk key `0x8300007E`** reads
   `(13, 0, 7517) (12, 132501, 138145) (17, 121440, 133880)`.
3. **Type-9 chunks are resident verbatim** like the other cell types: the type-9 chunk with id
   `0x01800001` is present byte for byte at guest `0x8016141C` in `rr-race`, and the type-8 cell of
   the same id at `0x8011941C` points its `region[7]` at it. A search by 256-byte high-entropy
   windows misses them, because a chunk that is mostly small-integer primitive records has no such
   window.

With `road_chunk.md` section 5: the three types all go through `0x80032A20`; the payload is
`u32 nWords; u32 pair[]; extent[4]`; 24 slots of 112 bytes; the call chain `0x8003234C`,
`0x800135E8`, `0x800325BC`, `0x80033F14` x2.

---

## 12. The texture binding - which image and which palette a cell primitive samples

Section 4.3 leaves `texRef`, `pal` and `clut` as references with no named resource; this section
names them, derives the rule from `SLUS_010.53`, and checks it against the console's own VRAM and
against the product's renderer per pixel.

**Headline.** A cell's textures are the **type-1 and type-2 chunks of the same resource id, sitting
in the same stream next to the cell**. The cell's header pair (2.1) is the key: the values *with*
bit 15 name the type-2 chunks and are what **band 0** samples; the values *without* bit 15 name the
type-1 chunks and are what **band 1** samples. A chunk *is* a VRAM rectangle - 64 x 128 halfwords,
4bpp, i.e. 256 x 128 texels - uploaded from its own bytes, and the **palettes travel inside the
image**, 32 of them in the last 32 rows of its last chunk, at halfword columns 48..63. `pal`
(`prim+0x01`) is the row. The one reference that is not a stream resource, `0x7800`, is the fixed
page `DATA\G_OBJ01.GTP`.

### 12.1 The call chain **[proven]**

All addresses in `SLUS_010.53`, sha1 `67ed165a2c517d4e6106fb0dfa324d66dd9a76f1`.

```
0x80032A20  load a cell
  0x8003234C  header pass: file the header pair into the slot
                 values WITH    bit 15 -> slot +0x58, +0x5C      (0x8003238C..0x800323A8)
                 values WITHOUT bit 15 -> slot +0x50, +0x54      (0x800323AC..0x800323C0)
  0x800325BC  resolve each key to a texture record
                 slot +0x50/+0x54 -> records at slot +0x68/+0x6C
                 slot +0x58/+0x5C -> records at slot +0x60/+0x64
  0x80033F14(slot, 1, bank)  -> 0x80034428   BAND 0 fix-up: region 6, groups [0, A+B)
  0x80033F14(slot, 0, bank)  -> 0x80033F94   BAND 1 fix-up: region 7, groups [A+B, A+2B)
```

The two `0x80033F14` calls are at `0x80032C18` and `0x80032C28`; the second argument is the only
difference and it is what picks the band. **Band 2 is never fixed up at all**, which is why every
band-2 primitive still carries the untouched `0x7C00`.

`0x80022218(key, bank)` is the record lookup: a linear scan of **24 records of 48 bytes** at
`0x800D9268 + bank*1152`, matching `record+0x08 == key`, returning the flat index `bank*24 + i` or
`-1` (`0x80022218`..`0x80022274`). Every cell texture of a race is in **bank 0** - read back from
`work\oracle\state\rr-race\ram.bin`, the ten live cell slots point only at records 0..7.

`0x80034dec` is the type-1/type-2 loader both texture dispatcher arms call. It fills that record:

| off | written | by |
|---|---|---|
| +0x00 | the page slot index, `-1` until a page is assigned | `0x80033b50` |
| +0x04 | 1 for a type-2 chunk, 0 for type-1 | `0x80034f58` |
| +0x08 | the **packed key** | `0x80034f34` / `0x80034f50` |
| +0x0C | the chunk - type 2, or type 1 half 0 | `0x80034f44` / `0x80034f54` |
| +0x10 | the chunk - type 1 half 1 | `0x80034f4c` |
| +0x18 | 24 bytes copied from the chunk's residency-window block | `0x80034f68` |

Two `+0x0C`/`+0x10` slots is why a type-1 id ships as **two** chunks and a type-2 id as **one**.

### 12.2 The two resolvers **[proven]**

Both take `(scratch, keys[2], records[2], bank)` where `scratch+0x00` is the primitive's `texRef`
and `scratch+0x02` its `pal`, and both write `scratch+0x04 = clut`, `scratch+0x06 = tpage`. The
fix-up pass copies those back to `prim+0x06` and `prim+0x0A` (`0x80034204`, `0x80034630`).

Two EXE config tables are involved, both indexed by the race set,
`level = *(*(u32*)0x8005B2F8 + 0x30) - 1`:

* **`cfg` at `0x800533B4`, stride `0x58`** - 22 groups of 4 bytes `(uBias/4, vBias, pageSelA, pageSelB)`.
  A page selector decodes as `x = (p & 0x0F) * 64`, `y = (p & 0x10) ? 256 : 0`. The model draw uses
  the same table.
* **`cfg2` at `0x80053254`, stride `0xB0`** - 22 sub-records of 8 bytes
  `(s16 x0, s16 y0, u8 n, u8 count, u8 step, u8)`.

Set 1 (`level = 0`) is what everything below is worked through with.

**Band 1, `0x80022758`** (type-1 pages):

```
i     = the key slot equal to texRef, else 0        (0x80022764..0x800227c0)
rec   = records[i];   w0 = *(s32*)rec
tpage = *(u16*)(0x800D7710 + (bank*3 + w0) * 8 + 4)
sub   = 0x80053254 + level*0xB0 + bank*8
clutX = ((w0 & 0xF) + (cfg[bank*4 + 2] & 0xF)) * 64 + (s16)sub[0] + (pal % sub[4]) * sub[6]
clutY = (s16)sub[2] + pal / sub[4]
clut  = (clutY << 6) | ((clutX >> 4) & 0x3F)
```

Set 1, bank 0: `cfg` group 0 is `00 00 06 08`, so `pageSelA = 6`; `cfg2` sub-record 0 is
`x0 = 48, y0 = 224, n = 1, count = 32, step = 16`. So the page column is `w0 + 6` and
**the palette for `pal` is the 16 halfwords at VRAM `(column*64 + 48, 224 + pal)`** - 32 of them,
`count` exactly.

**Band 0, `0x8002289C`** (type-2 pages):

```
i     = the key slot equal to texRef, else 0        (0x800228a0..0x800228f4)
rec   = records[i];   w0 = *(s32*)rec
entry = 0x800D76D0 + (bank*4 + w0) * 8
tpage = *(u16*)(entry + 4);   vBias = *(u8*)(entry + 7)
col   = (w0 >> 1) + cfg[(bank + 2)*4 + 2]
clutX = (col & 0xF) * 64 + 48
clutY = ((col & 0x10) ? 256 : 0) + ((w0 & 1) << 7) + 96 + pal
clut  = (clutY << 6) | (((col & 0xF) << 2) | 3)
```

Set 1, bank 0: `cfg` group 2 is `00 00 09 0A`, so `pageSelA = 9`. **A type-2 image is half a page**:
`w0 >> 1` picks the column and `w0 & 1` the 128-row half, which is exactly what `vBias` (0 or 128)
then adds to every corner's `v` (`0x80034658`..`0x80034690`, and the band-1 equivalent at
`0x800341b0`..`0x800341ec`). A renderer that binds the 128-row image on its own samples at
`(prim.u, prim.v)` unchanged, as for models.

The two `+ 48` are the same constant in both resolvers, so **the palette block is at halfword
columns 48..63 of the page column in both cases** - texels 192..255 at 4bpp - and it is the last 32
rows of the image: rows 96..127 of a type-2 half page, rows 224..255 of a type-1 full page.

**The `0x7800` arm.** Both fix-up passes test `(texRef >> 10) & 0x1F == 30` first
(`0x800340f4`, `0x80034294`, `0x80034564`, `0x800346f0`) and then take an inline path that ignores
the cell's pair entirely: `tpage = *(u16*)0x800D6160`, `vBias = *(u8*)0x800D6168`, and the CLUT is
built from **`cfg` group 8** and **`cfg2` sub-record 8** by the band-1 formula. Set 1: `cfg` group 8
is `00 00 0E 0E` -> page at VRAM `(896, 0)`; `cfg2` sub-record 8 is
`x0 = 0, y0 = 0, n = 4, step = 16` -> **palette `pal` at `(896 + (pal % 4) * 16, pal / 4)`**. In
`rr-race` `*(u16*)0x800D6160` is `0x000E`, i.e. tpage X = 14 = VRAM x 896, and `*(u8*)0x800D6168`
is 0.

### 12.2a The key list is SORTED before the search **[proven]**

The step "`i` = the key slot equal to `texRef`, else 0" above is over the list the fix-up pass hands
the resolver, and that is **not** the slot's disc order. `0x80033F14` copies the two keys (slot
`+0x50/+0x54` for band 1, `+0x58/+0x5C` for band 0) and their records into a local array, stores
`-1` after them and calls **`SLUS 0x80033DAC`**: a three-element exchange network (`slt` on the
words, swaps by `0x80033D70`, which exchanges the key AND the record) that sorts the keys
**descending**. The resolver's walk is `while (texRef < key[i]) i++` (`0x80022774`, `0x80022794`:
`slt v0,a0,v0` with `a0 = texRef`), which is a search of a descending list; the `-1` stops it, and a
miss falls back to entry 0 of the SORTED list, the larger key.

The sort matters: 292 of the 456 two-key lists over both streams are stored ascending, with
**17 203** primitives on the second key. The same walk over the disc order stops at entry 0 at once
and sends every primitive of the second key to the FIRST key's page - on race 1/20 (cell
`0x0580017B`, keys `0x2D66 0x2D7C`) that puts the concrete and house page of the neighbouring cell
`0x05800166` on the tree trunks and the leaf canopy that meets over the road. `rrview --cellcheck`
cannot see such an error, since it checks each pixel against the page the renderer's own rule names.

The check is **`rrverify cellbind --state <dir> --data work\disc_us\DATA`**: for every resident
cell whose passes ran (slot `+0x0C` bit 3 = band 0, bit 2 = band 1) it finds the disc chunk with the
slot's id and RAM's vertex array, and compares, primitive by primitive, the tpage the console wrote
at `+0x0A` with the tpage of the record the product's rule (`rr::CellTextureSlot`) names (`0x800D7710 + (bank*3 + w0)*8 + 4` for
band 1, `0x800D76D0 + (bank*4 + w0)*8 + 4` for band 0, `*(u16*)0x800D6160` for `0x7800`):

| state | cells | band 0 prims | band 1 prims | disc-order and sorted disagree | differ from the console | control (`--mutate`, disc order) |
|---|---|---|---|---|---|---|
| `rr-race` | 10 | 1041 | 1622 | 38 + 100 | 0 | 138 differ, caught |
| `rr-pack` | 7 | 772 | 1906 | 38 + 100 | 0 | 138 differ, caught |
| `quick` | 9 | 945 | 1069 | 38 + 100 | 0 | 138 differ, caught |

The 138 are slot 3, `0x01800082`, keys `0C7C 0C8D` / `8C7C 8C8D` stored ascending. The gates
"oracle: every cell primitive samples the page the console resolved" run it.

### 12.3 What that page is: `DATA\G_OBJ01.GTP` **[proven]**

VRAM `(896, 0)`, 64 x 128 halfwords, is **byte-identical to all 16384 bytes of `DATA\G_OBJ01.GTP`**
in `work\oracle\state\rr-race\vram.bin` - and `docs\formats\textures.md` 5 establishes that file as a headerless pre-baked 4bpp page whose **14 CLUTs ship in its own first four rows**,
four per row, at `(896 + (n % 4) * 16, n / 4)`. That is the same address the formula above produces,
with `n = pal`.

The confirmation is the shipped data. Over all 102 stream files, the 10732 cell primitives with
`texRef == 0x7800` carry `pal` in **0..13** - exactly the 14 CLUTs that file holds - and the three
most common are `pal` 6 (4672), 2 (3650) and 11 (1488), which the formula turns into PS1 CLUT ids
**122, 58 and 187**. `textures.md` 5a reports that in the captured draw stream
(`work\oracle\vr_capture\scene.csv`) **every** primitive on tpage X = 14 carries `palette` in
**{58, 122, 187}** and nothing else. Two independent captures, the same three ids.

So `0x7800` is the fixed **roadside object page** - trees, hedges, lamp posts, guardrails, benches, bollards - which the cell mesh uses
for the vegetation and street furniture baked into the terrain.

### 12.4 The chunks are the VRAM rectangles, byte for byte **[proven]**

Walking the resident cell slots of `rr-race`, resolving each key exactly as 12.2 does, and comparing
the VRAM rectangle with the chunk on the disc:

| key | record | `w0` | tpage | vBias | VRAM | bytes equal | differing page rows |
|---|---|---|---|---|---|---|---|
| `0C7C` | 4 | 1 | `0007` | 0 | (448, 0) 64x256 | 32662 / 32768 | 0, 128, 254, 255 |
| `0C80` | 2 | 0 | `0006` | 0 | (384, 0) 64x256 | 32668 / 32768 | 0, 128, 254, 255 |
| `0C8D` | 5 | 2 | `0008` | 0 | (512, 0) 64x256 | 32648 / 32768 | 0, 128, 254, 255 |
| `8C80` | 0 | 0 | `0809` | 0 | (576, 0) 64x128 | 16320 / 16384 | 0, 127 |
| `8C8D` | 1 | 1 | `0009` | 128 | (576, 128) 64x128 | 16322 / 16384 | 0, 127 |
| `8C7C` | 3 | 2 | `080A` | 0 | (640, 0) 64x128 | 16330 / 16384 | 0, 127 |
| `8C01` | 7 | 3 | `000A` | 128 | (640, 128) 64x128 | 16324 / 16384 | 0, 127 |

(`0C01`'s record still has `w0 = -1`, i.e. its type-1 halves were resident but not yet uploaded.)

Every difference is accounted for, and there is nothing else:

* **page row 0** of each uploaded chunk, 32 bytes: the chunk's own `key` + residency-window header.
  It is not uploaded - VRAM reads zero there - so **texels 0..63 of the image's first row are
  transparent on the console**, and the product's decoder forces them to index 0 for the same reason.
  A two-chunk type-1 page therefore has this at rows 0 **and** 128.
* **the last row(s) of the palette block**: one row per uploaded chunk, holding a verbatim copy of
  that chunk's 32-byte header. A type-2 page has one chunk and loses row 127 (`pal` 31); a type-1
  page has two and loses rows 254 and 255 (`pal` 30 and 31). **No shipped primitive selects `pal` 30
  or 31** - the maximum over all 102 files is 29 - so the engine is parking its bookkeeping in
  palette rows the art never asks for. Which code writes it was not traced.

### 12.5 The structural checks on the shipped data

Over both `.STR` and all 99 `.STP` (102 files, 1742 type-0 cells, 684860 primitives):

1. **Every band-0 `texRef` is one of the cell's bit-15 keys or `0x7800`; every band-1 `texRef` is
   one of its non-bit-15 keys or `0x7800`; every band-2 `texRef` is `0x7C00`.** 0 failures. This is
   the data-side proof of the band split the code shows **[proven]**.
2. `pal` reaches **29** in band 0 and **19** in band 1 - inside the 32 rows, and clear of the two
   bookkeeping rows.
3. **The art avoids the palette block exactly.** Counting corners with `u >= 192` (the palette
   columns): in band 0 the highest `v` is **95**, one row below where the palettes start, and
   **0 of 684860 primitives** put a corner inside the block. Band 1 has 219 such corners, so the
   same statement holds there only to three decimal places and is marked **[established]** rather
   than proven.
4. **A key names one image, unambiguously.** Within one file, all copies of a type-2 key have
   **exactly 1** distinct payload (718 file/key pairs, 1..3 copies each) and all copies of a type-1
   key have **exactly 2** (628 pairs, 2..6 copies each) - the two halves. A resource is re-shipped
   along the stream for streaming, never re-authored. Across *files* a packed key can collide,
   because it keeps only `(id >> 23) & 0x1F` and `id & 0x3FF`, so the key identifies an image only
   together with the file it was read from **[proven]**.

### 12.6 The rule, as a renderer needs it

```
band 0 (region 6)  ->  keys = the cell's header-pair values WITH bit 15
band 1 (region 7)  ->  keys = the cell's header-pair values WITHOUT bit 15

for a primitive:
  if (texRef >> 10) & 0x1F == 30:            # texRef == 0x7800
      image   = DATA\G_OBJ01.GTP, 256 x 128 texels, 4bpp, no header
      palette = its own rows 0..3: 16 halfwords at byte (pal / 4) * 128 + (pal % 4) * 32
  else:
      keys  = sorted DESCENDING (0x80033DAC, 12.2a)
      key   = the entry of `keys` equal to texRef, else keys[0] (the larger key)
      id    = ((key >> 10) & 0x1F) << 23 | (key & 0x3FF)        # the cell's own id
      band 0: image = THE type-2 chunk of that id      -> 256 x 128 texels
      band 1: image = the TWO type-1 chunks of that id -> 256 x 256, first then second
      palette = the LAST chunk's rows 96..127, halfword columns 48..63:
                16 halfwords at byte (96 + pal) * 128 + 96
  texel = image pixel (prim.u, prim.v), low nibble = the left pixel, no offset
  the first 16 halfwords of each chunk (its header) are transparent
```

`src\rrformats\cell.cpp` splits the header pair and `src\rrformats\model_texture.cpp`
(`BuildCellTexturePage`, `BuildRuntimeObjectPage`) builds the pages.

### 12.7 The rule drawn: `rrview --cellcheck`

`rrview --cellcheck` compares every cell pixel of a rendered frame with the palette entry the rule
predicts:

```
rrview <disc.bin> --race 1 20 --drive --at <d> --tex ^
    --shot <out.png> --cellcheck <report.txt>
```

(Add `--no-ribbon` for a picture: the synthetic road ribbon `rrview` builds from the type-3 slices
has a guessed half width and z-fights the cell's own textured tarmac. It changes the pixel
counts below slightly by occlusion, not the verdict.)

The frame is drawn **five** times. Pass 1 is the frame. Pass 2 has every textured cell fragment
report the palette row it selected and the index it sampled; pass 3 has it report which draw run it
belongs to, which is what says *which* page the comparison must use; pass 4 suppresses the
transparent-texel discard so the mesh's coverage can be counted; pass 5 has
*every* surface write blue 255 through the real discard, which is the silhouette the hole count of
12.7.1 is measured against. Everything that is not a cell is
drawn in those passes with unchanged geometry and unchanged discard but blue 0, so a reported
fragment can never be confused with another surface. A cell primitive carries **no normal** - a cell
has one vertex pool of `s16 x,y,z,w` and no normal array anywhere - so it is drawn unlit and its
pixel *is* the palette entry: the comparison is exact equality on all three bytes, which tells right
from wrong for a grey wall exactly as well as for a green field.

Race 1/20, eight camera positions:

| `--at` | cell runs drawn | runs fully matching | cell pixels | matching |
|---|---|---|---|---|
| 1000 | 6 | 4 | 377036 | 377036 |
| 3000 | 5 | 0 | 0 | no cell in view; reported EMPTY, not PASS |
| 6000 | 7 | 5 | 475982 | 475982 |
| 9000 | 6 | 4 | 423899 | 423899 |
| 12000 | 6 | 3 | 383149 | 383149 |
| 15000 | 7 | 5 | 478492 | 478492 |
| 18000 | 8 | 6 | 448906 | 448906 |
| 21000 | 8 | 3 | 353095 | 353095 |

**2 940 559 cell pixels, 0 mismatching, 0 unreadable ids**, over 30 fully visible runs. Race 1/1 at
`--at` 500, 1500, 2500, 6000 and 8000 adds 1 594 039 pixels, all matching.

Two negative controls, because a check that cannot fail proves nothing:

| `--at` | control | cell pixels | still matching | runs fully matching |
|---|---|---|---|---|
| 6000 | `--cellcheck-mutate` (next palette row) | 475982 | 6468 | **0 of 7** |
| 6000 | `--cellcheck-mutate-page` (right row, other page) | 475982 | **0** | **0 of 7** |
| 9000 | `--cellcheck-mutate` | 423899 | 1394 | **0 of 6** |
| 9000 | `--cellcheck-mutate-page` | 423899 | **0** | **0 of 6** |
| 15000 | `--cellcheck-mutate` | 478492 | 57433 | **0 of 7** |
| 15000 | `--cellcheck-mutate-page` | 478492 | 116906 | **0 of 7** |

A one-row palette error still lands on the right colour for the odd pixel, because two rows of one
terrain page do share entries - so the pass criterion for a control is **no run matching
throughout**, which needs no threshold to be tuned. Both controls fail on every position.

### 12.7.1 The quad split, before and after **[proven]**

Drawn with the strip split, every frame of the roadside world has large black triangles cut out of
the terrain. They are **not** a texturing failure (the transparent-texel discard removes 1.0 % of
the mesh's coverage) and **not** missing band-1 geometry: they are the quarter of every quad that
the wrong split throws away (4.3).

The measure is two numbers from the same frame, both reported by `--cellcheck`:

* **not background** - how much of the frame any surface covers. Read off a fifth pass in which
  every surface writes blue 255 through the real discard, so it does not depend on guessing what
  the clear colour rounds to in 8 bits.
> The type-4 panorama backdrop is deliberately **excluded** from the coverage (pass 5) and
> silhouette (pass 6) passes. Both numbers below are about how much of the frame the WORLD covers,
> and a backdrop that fills the sky would drive the hole count to zero without filling a single
> hole. Measured with the backdrop on, race 1/20 at `--at` 6000 gives the same 622 313 cell pixels,
> 0 mismatching, and 72 hole pixels.

* **background enclosed by the skyline** - the hole count. In each column the highest row that is
  not background is the skyline, and every background pixel below it in that column is a hole. A
  column that never leaves the background contributes nothing, which is why the two numbers are
  always read together: filling holes drives one down and the other up, while drawing nothing at all
  drives both to zero and cannot be mistaken for success.

`--quad-strip` selects the strip split, so before and after come from one binary:

```
rrview <disc.bin> --race 1 20 --drive --at <d> --tex --no-ribbon ^
    [--quad-strip] --shot <out.png> --cellcheck <report.txt>
```

| race / `--at` | holes before | holes after | covered before | covered after | cell pixels after | matching |
|---|---|---|---|---|---|---|
| 1/20 1000 | 102 857 | 14 322 | 389 086 | 479 126 | 476 609 | 476 609 |
| 1/20 6000 | 111 011 | **70** | 518 412 | 629 485 | 623 923 | 623 923 |
| 1/20 9000 | 89 518 | 21 008 | 449 769 | 518 860 | 509 837 | 509 837 |
| 1/20 12000 | 97 410 | 25 867 | 421 564 | 504 604 | 495 979 | 495 979 |
| 1/20 15000 | 87 884 | 21 315 | 534 627 | 606 213 | 576 697 | 576 697 |
| 1/20 18000 | 88 240 | **167** | 487 412 | 576 189 | 569 556 | 569 556 |
| 1/20 21000 | 166 537 | 86 138 | 385 873 | 466 739 | 464 826 | 464 826 |
| 1/1 500 | 220 507 | 139 726 | 318 936 | 399 975 | 391 515 | 391 515 |
| 1/1 1500 | 161 383 | 112 617 | 270 431 | 326 599 | 307 712 | 307 712 |
| 1/1 2500 | 111 222 | 31 342 | 411 407 | 491 414 | 481 513 | 481 513 |
| 1/1 6000 | 169 504 | 132 658 | 281 774 | 319 526 | 312 956 | 312 956 |
| 1/1 8000 | 47 696 | 32 722 | 493 139 | 508 134 | 502 021 | 502 021 |

Frame is 1280 x 720 = 921 600 pixels throughout. **1 453 769 hole pixels before, 617 952 after** - a
57 % drop - while the terrain's own coverage rises from 4 962 430 to 5 826 864. **5 713 144 cell
pixels, 0 mismatching, 0 unreadable ids**, i.e. the newly uncovered geometry passes the same
per-pixel palette check as the rest. `RACE1_1.STP` at `--at` 4000 and race 1/20 at 3000 report
EMPTY (no cell in view) and are excluded.

The holes that remain are not wedges. At `--at` 21000 and on race 1/1 they are the edge of the
loaded world - the far side of a cell that the residency rule does not bring in - plus the
transparent texels of the terrain art itself.

### 12.7.2 Band 1 drawn and checked **[proven for the binding, [probable] for when to draw it]**

`--band1` takes the B groups from region 7 instead of region 6, i.e. the fine arm of 4.1.1;
`--band2` adds band 2; `--only-band1` draws the region-7 groups on their own. A type-8 cell's
region 7 is joined from its type-9 chunk by resource id, exactly as `0x80032B7C` / `0x80032338` do
it (3.2): on race 1/20, 65 of 118 cells carry region 7 in their own chunk and **52 more are joined
from a type-9 chunk in the same stream, 0 failures**, leaving 117 of 118 cells able to draw fine.

The band-1 pages are the **two type-1 chunks** of the cell's own resource id, in file order, built
into one 256 x 256 image whose palettes are the second chunk's rows 96..127 - page rows 224..255,
which is where the band-1 resolver's `clutY = 224 + pal` lands (12.2). Both chunks' 32-byte headers
are excluded, at page rows 0 and 128.

Per-pixel result, race 1/20 with `--band1 --band2`, the seven camera positions above:
**3 328 914 cell pixels,
0 mismatching**, with the two mutation controls still failing to leave any run matching throughout.
Band 1 is checked on a frame, not only on the disc.

`--only-band1` shows what band 1 and 2 are: a **narrow high-detail corridor along the road** - the
tarmac (band 2, untextured, its `0x7C00` reference never resolved), the verges and the hedges -
against band 0's coarse panels. Substituting it for every group of every cell is *not* what the
console does, and the numbers say so: holes fall at seven of the twelve camera positions and rise
at the rest, worst at race 1/20 `--at` 18000 (167 -> 138 796), because the fine corridor is slightly
narrower than the coarse groups it replaces and the ring between it and the always-coarse `A` groups
is left empty. The console makes that choice per group through `ctx+0x34` (13.4); `rrview`'s
default is every group coarse and band 1 is opt-in.

### 12.7.3 The `--cellcheck-mutate-page` control and byte-identical pages

With band-1 pages in the map the "other page" the control binds can be a page that is
**byte-identical in the palette rows the run samples** - two cells of one road often ship the same
terrain art under different resource ids. Binding such a page is not a wrong binding, so the run
cannot fail and counting it as a failure would be a false alarm (four band-0 runs at race 1/20
`--at` 18000). The control therefore counts, per run, how many pixels the mutation actually moved,
excludes runs it moved on **no** pixel from the verdict, and prints how many those were. That is a
statement of what the control tests, not a tuned threshold: 16 of 16 control runs across four camera
positions and both bands pass, with exactly one position reporting 4 such runs.

### 12.8 What section 12 does NOT settle

1. **The on-disc `clut` placeholder** (`0xFFFF` / `0x0CBF` / `0x0BFF`) is overwritten before it is
   ever read, so what those three values meant to the authoring tool is unknown.
2. **`bank` is taken as 0.** It is an argument threaded down from the caller of `0x80032A20` and was
   read off live RAM (all resident cell texture records are in bank 0), not off the code that chooses
   it. Everything above is bank-0 arithmetic.
3. **Who writes the chunk header into the last palette rows** (12.4) was not found.

Which band the console draws when is 4.1.1, with the per-group value of 13.4; band 2's texture is the
level's road page (13.3). Drawing band 1 in place of band 0 does not fill the quad-split wedges of
12.7.1 - at race 1/20 `--at` 18000 it raises the hole count from 167 to 138 796 (12.7.2).

## 13. How the original DRAWS a cell: colour, texture windows, level of detail, the road

Sections 4 and 12 say which primitives exist and which texel page and palette they bind. This section is what the draw routines then do with them, read from `RASHCDG.BIN`
(sha1 `cfe43a7786759f2cb9c57751cf99e84d1074782c`, at `0x8005B5E8`), `SLUS_010.53`
(`67ed165a2c517d4e6106fb0dfa324d66dd9a76f1`) and `RASHCDI.BIN`, and checked primitive by primitive
against the original's own draw packets for the `rr-race` state (`rrview --roadcheck`, 13.6). Code:
`src\render\scene_geometry.cpp` (`BuildCellSoup` mode 3, `CellLodWord`, `CollectBand2`),
`src\rrformats\cell_draw_tables.h`, `src\rrformats\level_bundle.h`.

### 13.1 Every cell primitive is coloured by the level's colour table **[proven]**

The packets are GP0 `0x2C`/`0x24` (band 0, flat) and `0x3C`/`0x34` (bands 1 and 2, Gouraud), all
textured and NOT raw, so the GPU multiplies each texel by a vertex colour (0x80 = 1.0).

* Band 0: one colour per primitive, `*(0x800D4CA8 + 4 * (attr & 0xFF))` - `0x8006DB14..0x8006DB34`
  (`lbu 62(scratch)` = record `+0x02`, i.e. the low byte of the halfword whose top nibble is the
  surface, 4.3).
* Bands 1 and 2: one per vertex, `*(0x800D4CA8 + 4 * (w & 0xFF))` where `w` is the vertex's fourth
  halfword (4.2) - band 1 `0x8006CBF0..0x8006CC0C`, band 2 `0x8006E7C0..0x8006E8E4`.
* `0x800D4CA8` is filled at level load by the type-7 section handler of the level bundle,
  `RASHCDI 0x80062430` (`memcpy(0x800D4CA8, payload, 1152)`): the 256-entry table is the first 1024
  bytes of that section of `DATA\GAMEBIN1.DAT` bundle `raceId - 1` (`level_bundle.h`). In `rr-race`
  the RAM table equals bundle 3's payload byte for byte.

Checked: 84 cell packets matched one-to-one to a primitive of the product carry exactly the colours
this rule gives (0 of 84 differ), and 173 road vertices agree within 4 (the worst 0.1).
`rrview --cellcheck` predicts `round(palette * colour / 128)` per channel and passes on every cell
pixel.

### 13.2 Bands 0 and 1 are drawn under texture windows **[proven]**

Each packet is preceded by a GP0(E2) word from a table, and a primitive's UVs are tile-relative:

* Band 0: `RASHCDG 0x800CC994`, 32 words, indexed by the flags byte `>> 3` (`0x8006DAFC..0x8006DB10`).
  The flags are rewritten at load by `SLUS 0x80033EA0` (called from both fix-up passes): a disc byte
  `t << 4 | low` with t < 15 becomes `(t + 15 * [the page is the lower half of its VRAM page]) << 3 |
  low` and no V bias is added; t = 15 becomes 30 (`0xE2000000`, no window) and the V bias IS added.
  Entries 0..14 are 32 x 32 tiles at ((t % 8) * 32, (t / 8) * 32) of the image, 15..29 the same 128
  rows down. So for the 256 x 128 image the renderer binds: window table[t] for t < 15, none for 15.
  Measured on `rr-race`'s RAM against the disc: `0x20` -> `0x88`, `0x22` -> `0x8A`.
* Band 1: `RASHCDG 0x800CC954`, 16 words, copied to scratchpad `0x1F800078` by `0x80068FF0..
  0x80069018` and indexed by the flags byte `>> 4` as shipped (`0x8006CBBC..0x8006CBDC`; not rewritten
  at load): 64 x 64 tiles at ((t % 4) * 64, (t / 4) * 64), entry 15 none.

Sampling the raw UV instead makes every windowed band-0 primitive take the top-left 32 x 32 of its
image instead of its own tile. Checked: 84 packets, 0 windows differ; with
the table index moved by one (`--roadcheck-mutate-window`), 40 differ.

### 13.3 Band 2 is the road surface, drawn from the road page **[proven]**

`RASHCDG 0x8006E474` (nibble bit 2 set) and `0x8006F5D0` draw band 2's QUADS (they read the group's
quad count; nothing draws a band-2 triangle). Its `texRef 0x7C00` is never resolved because the page
is fixed: tpage `*(u16*)0x8005B370`, palettes `0x800D5EC8[0..11]`, both written by the level
bundle's type-4 handler `RASHCDI 0x80061D18`, which uploads the section's TIMs 0, 1, 2 to the
rectangles the EXE's texture config (`SLUS 0x800533B4`, groups 7, 6, 5) names and its twelve raw
CLUTs in order (`level_bundle.h` `BuildRoadPage`; the product's page equals the capture's VRAM on all 18432 texels
the TIMs cover and on all 192 CLUT entries). Per quad (corners i0..i3, depths from the cell
transform):

* **far** - the largest corner depth >= 4096 cell units (`0x8006E640`, `0x8006F748`): one GT4 in the
  vertex order (i0, i1, i3, i2) with the record's own UVs (they address TIM 0, a small pre-shrunk
  road strip in rows 0..15) and palette `0x800D5EC8[g]`, `g = (attr >> 2) & 0xC`;
* **near** - `n = 0x800CC914[attr & 0xF] >> 28` lane strips (1..6) between P_j on i0 -> i1 and Q_j on
  i3 -> i2, stepped by `(end - start) * 0x800CCA10[n] >> 12` (`0x8006EC44..0x8006EE98`); strip j
  takes 5 bits of `0x800CC874[attr & 0xF]`: bits 0..1 the UV template kind, 2..3 a palette offset.
  Its corners (P_j, Q_j, P_j+1, Q_j+1) get the four UVs of template `2 * kind + near` of
  `0x800CC8B4` (8 records of 16 bytes) and palette `0x800D5EC8[g + 1 + offset]`
  (`0x8006F434..0x8006F548`), where `near = 0x800CCA58[maxZ / 512]` is 1 below 2560 cell units: the
  full-size asphalt of TIM 2 (u 16..255, v 16..63) close up, the 32-texel-wide TIM 1 further out;
* **lane lines** - before the strips (so on top, being later in the packet chain), 2 bits per strip
  edge of `0x800CC914[attr & 0xF]` name a colour of `SLUS 0x800523AC` (white `A2A2A2`, yellow
  `4789A2`); the line is a flat untextured quad from 1/64 to 3/64 of the strip's width in from that
  edge (`0x8006EEC4..0x8006F424`);
* the near strips and lines are then subdivided by `0x80069CF0` / `0x8006A25C` - recursive midpoints in
  view space with outcode culling, UVs and colours averaged - and each new edge midpoint is pushed out
  by about a pixel perpendicular to its edge (`0x80069E50..0x80069EE4`), which closes cracks. A
  perspective-correct rasteriser needs neither.

Checked against the packets: 16 far quads corner for corner (UV, palette, colour exact), 92 near-path
pieces placed in the product's strips with 173 vertices within tolerance, 14 of 14 lane lines of the right
colour; controls: kind, palette, depth each caught.

### 13.4 Which groups are drawn fine: `SLUS 0x80035680` **[proven]**

This is the value of `ctx+0x34` that 4.1.1 reads. Once a frame, for each resident cell of the
player's list (`0x800D9B80`, count at gp+2268, from `0x800358C0`) and each sub-area k in [0, B), the
routine clears slot `+0x38` (the dispatcher's `ctx+0x34`) and calls `0x800351EC` on region 3's
polygon k: the polygon's vertices through the cell's GTE matrix and translation (slot `+0x10`,
`+0x24`) give the minimum depth, the mean depth of the first four, and a horizontal frustum code
(`|16 z| < 21 |x|` outside, `|z| < |x|` straddling, `z < -40` behind). Then:

* all vertices outside one side or behind: nibble 0 (the group is not drawn at all);
* fine if the cell has a region 7, the minimum depth is below 9600 cell units (150 world units; 4800
  on road set 2, `0x800356C0`) and fewer than four cells have already gone fine this frame: nibble
  7 when straddling or the mean depth is below 2048, else 3;
* otherwise coarse: 5 or 1 by the same test;
* then bits 2|3 are ORed in when the mean depth is below the fine threshold (`0x80035830`).

`CellLodWord` is this. On `rr-race` it reproduces the captured slot words of all three listed cells
exactly (`FF3`, `111`, `111`). The global `0x800CC86C` that masks bit 1 away reads 0 in `rr-race`.

### 13.5 The GPU vertex order of a quad

The corners go round the quad (4.3). The draw routines hand them to the GPU as (i0, i1, i3, i2) -
band 0 writes record `+0x10, +0x12, +0x16, +0x14` at `0x8006DB3C..0x8006DBA8`, band 2's far path the
same at `0x8006E7C0..0x8006E918` - so the GT4 is split along the i1-i3 diagonal, not the i0-i2 one
of 4.3's perimeter split. For planar quads it is the same picture; for the 707 non-planar
ones of 4.3 it decides the fold. Checked: matching the packets IN GPU VERTEX ORDER is how the 84 cell
packets and the 16 far quads were found.

### 13.6 The check: `rrview --roadcheck`

```
rrverify trace --frames 2 --no-cop2 --out <dir>
python tools\scout\psxgpu.py frame --gpu <dir>\gpu.bin ^
       --vram work\oracle\state\rr-race\vram.bin --frame 1 --out <dir>\orig.png ^
       --prims <dir>\orig_prims.csv
rrview <disc> --race 1 4 --tex --state work\oracle\state\rr-race --size 384 240 ^
       --orig-prims <dir>\orig_prims.csv --shot ours.png --roadcheck road.txt
       [--roadcheck-mutate-kind | -palette | -depth | -window]
```

It is in `tests\run_gates.ps1` with its four negative controls. What it judges and what it counts
without judging:

* slivers - a packet under 16 px, or under 4 px across, has no inside to cast a ray into: a far-path
  one must still be a far quad of the product's at its four corners (and be judged exactly), a near-path one
  is counted as "not judged" (68 in this frame, all at the horizon);
* a near-path vertex whose pixel spans more than four texels is skipped; the others must be within
  1.5 texels plus five pixels' worth of UV change (the subdivider's truncation and crack nudge);
* a cell packet is judged only when its corners fit exactly one primitive of the product (84 of 718; the rest
  are subdivided band-1 pieces or horizon slivers).

### 13.7 What section 13 does NOT settle

1. Why the view record's eye `+0xB8` is 0.04 units from the eye the frame was drawn with; the check
   uses the slots'.
2. The `0x8006F5D0` arm is read only as far as its near/far test and tables (the same as
   `0x8006E474`'s); the frame had no near-path quad drawn by it.
3. Templates 6 and 7 of `0x800CC8B4` (kind 3) do not look like UVs; no `0x800CC874` entry uses kind 3.

(The visit order for the four-fine-cells cap is the draw list's order, section 14; the bike's shadow
quads and the effects have their own packet checks in `tests\run_gates.ps1`.)

### 13.8 The subdividers: near polygons are cut before the GPU maps them affinely **[proven]**

The PS1 GPU steps texels and Gouraud colours linearly in SCREEN space per triangle (a GT4 is its two triangles
(v0, v1, v2) + (v1, v2, v3)). The cell draw hides the error close up by cutting near polygons into pieces
(`RASHCDG` sha1 cfe43a77..., our disassembly; ported in `src\game\sim\subdiv.{h,cpp}`, bench rows
`tools\rrverify\rows_subdiv.inc`: `subdiv_tri`, `subdiv_quad`, `subdiv_road`, `subdiv_line`, 0 mismatches / 0 traps
on the default seed and on `--seed 0xA5A5C3C312345678 --cases 2048`, every `--mutate` FAILs):

| function | caller | cuts | leaves |
|---|---|---|---|
| `0x80069CF0` | band 2 near strips (`0x8006F534`, `0x80070608`): `(4, 0x01030200, 5)` | quad: 4 edge midpoints + centre, children `0x800CCA40` | GT4, nudge `(z + 0x40) >> 8`, near-plane refinement below base 14 |
| `0x80069784` | fine near band 1 quads (`0x8006A630`): `(4, 0x03020100, 5 - (attr & 15))` | the same | GT4, nudge `(z + 0x80) >> 8`, below base 19 |
| `0x8006929C` | fine near band 1 triangles (`0x8006A630`): `(3, 0x20100, 5 - (attr & 15))` | 3 edge midpoints, children `0x800CCA30` | GT3, nudge 0x80, below base 12 |
| `0x8006A25C` | the lane lines (`0x8006E474`): `(4, 0x01030200, colour)` | midpoints of edges 1-2 and 3-0, children `0x800CCA50` | F4, only while a piece reaches below line 240 |

The records (32 bytes at `0x1F800100`): view x, y, z (MVMVA LLM x V + BK, sf 1, `<< 8`; `0x8001064C` moved the view
into LLM / BK and zeroed RT, so RTPS with TR = point `>> 5` projects the point), the texel (carry-safe floor
average), the colour index (averaged, then looked up in `0x800D4CA8` again - a piece's colours are the table's,
not interpolated), SXY and an outcode byte: `0x80` / `0x40` / `0x20` for z < `0xC800` / `0x19000` / farther (200 /
400 cell units), `0x10` nearer than 40 units, 8 above, 4 below line 240, 2 left, 1 right. A child whose corners
share one of the low five bits is dropped; it is cut again while its OR has a bit at or above `level + 1` (so
level 5 stops once every corner is 400+ units away, level 6 at 200+, level 7 always) or touches the near plane.
Each EDGE midpoint is nudged about a pixel along x or y away from its edge's screen direction (closes cracks).
`0x8006A630` sends a band-1 primitive to the subdividers only when one of its vertices is nearer than 1024 units
(the vertex pass `SLUS 0x8001034C` byte `0x20`); the coarse and far routines and the model emitter
`SLUS 0x800251E4` never cut - their polygons are mapped affinely whole. The model emitter's GT4 is
(i0, i1, i3, i2), i.e. the i1-i3 diagonal (`rmd3.h SoupCorners`), as the cells' (13.5).

**In the product** (`scene_geometry.h AppendBand2SoupSubdivided / AppendFineNearGroup`, `race_scene.cpp`): the
PORTED subdividers run per frame on the near road strips and the fine near groups with the view as the cell draw
sees it (the GL view's own frame; y scaled by the GTE row aspect; SXY by the GTE's RTPS arithmetic, H 237, centre
192 / 120); the pieces go back to world space and every textured polygon is drawn with `noperspective` texels and
colours (`shaders.cpp uAffine`). Ours: no nudge in the product (`Subdivider::nudge = false`: a depth-buffered
rasteriser has no cracks, and a point moved at constant depth stands off the road plane and hid the lane lines);
in a window wider than 384 columns the outcodes' x is pulled to the centre (`DrawRequest::sideSqueeze`). Switches:
`RRJB_AFFINE=off` (a perspective-correct renderer, the negative control), `RRJB_SUBDIV=off` (affine, not cut - a
diagnostic). The run log's `subdiv:` line counts strips, pieces, cut primitives.

Measured (`rrgame --parity`, `psxgpu.py parity`; before = `RRJB_AFFINE=off` on the same build):

| capture | pixels close | primitives matched | affine, not cut |
|---|---|---|---|
| rr-race | 87.8 -> 88.1 % | 86.6 -> 85.9 % | 88.0 / 85.6 |
| quick | 89.7 -> 90.2 % | 91.3 -> 91.3 % | 90.2 / 91.3 |
| rr-pack | 82.6 -> 85.0 % | 82.3 -> 82.7 % | 73.3 / 80.7 |

With the subdividers rr-pack's near wall (the largest differing area without them) matches the original's post and
window edges; its page-7 cells go from 89.2 to 92.8 % close. The i1-i3 quad diagonal for models alone: rr-race prims
324 -> 328, quick 373 -> 379 with affine mapping. Not reproduced: the corners' own outcode byte of the stepped road points (ours:
all the subdivider's); `0x8006B75C`, a variant of `0x8006A630` that no `jal` reaches (it also calls the two
band-1 subdividers), is not read; the lane lines are not cut (a flat F4 looks the same cut or whole).

## 14. Which cells a view draws, and where an entity stands **[proven]**

Ported, one bench row each in `tools\rrverify\rows_vis.inc` (PASS on the default seed and on `--seed
0xA5A5C3C312345678 --cases 2048`, `--mutate` failing each), `src\game\sim\cell_draw.*`:

| function | row | what |
|---|---|---|
| `SLUS 0x800363F0` | `cell_ready` | a slot's two texture keys `+0x58/+0x5C` resolved (`+0x60/+0x64`) |
| `SLUS 0x800329BC` | `cell_slot_of` | the slot of view p (12 p .. 12 p + 11) holding a resource id |
| `SLUS 0x80035E60` | `cell_resident_list` | every ready resident slot, in slot order |
| `SLUS 0x80035F48` | `cell_draw_list` | **the draw list** `0x800D9B80 + 48 p` (count gp+0x8DC) |
| `SLUS 0x80030500` | `cell_at` | the loaded cell whose extent holds (road, d), through the resource list `*(0x8005ACBC)` |
| `SLUS 0x80030410` | `position_cell` | an entity's cell (in a junction: its road's end) |
| `RASHCDG 0x8008B99C` | `entity_cell` | `e+0xB0` = that cell, the radius pushing the position toward view 0 |

**The draw list is keyed on the PLAYER, not on the camera.** `DATA\STREAM<n>.RLS` (road.md 4: 37 per-road offsets,
32-byte records) is loaded by `SLUS 0x80023498` into gp+0x1A4 / gp+0x88C (`0x8002428C` makes the offsets absolute).
Per view, 0x80035F48 takes the bike `*(0x8005B268 + 4p)` - or its rider while `+0x25C` is 3 or 4 - and, when its
road word's high half is 0, the record `{road +0x0C, from +0x0E, to +0x10}` holding `(+0x168, +0x172)`: kept from
the cursor gp+0x87C, stepped one record by `+0x16C`, or scanned (at most 140) from the road's first. Its up to six
u16 keys `+0x12` (`(k & 0x7C00) << 13 | k & 0x3FF`) are the cells drawn; one not resident and ready goes to the
missing list `0x800D9B68`. No record: the previous list stays. The camera only culls and sorts that list
(`0x800353C4`, `0x80036438`). The streamer's own key is the player too: `0x80023A14` reads the entity at
`0x80053478 + 0x80 p + 4`, which CamTarget `SLUS 0x800235B0` sets to the bike or, while thrown, the rider
(RiderLaunch `0x80091468`, Remount `0x800903F4`, `0x8009277C`). The original does not make residency or drawing
follow the camera.

**In the product** (`src\game\cell_view.*`): the release list in the arena; after the race step and AnimationPass the
cell streamer runs (every frame, the countdown included, so the intro fly-in has resident cells) keyed on the
CamTarget entity, then the draw list, then 0x8008D56C's cell tests (0x8008B99C on
the views, the live bikes, the riders off their bikes). `rrgame` draws exactly the listed cells
(`DrawRequest::cellOrder`; the fine-cells cap visits the list in its order) and a
rival only when its `+0xB0` is one of them (the model draw `0x80067690`'s condition); `BikeInstance` runs only for a
bike with `+0xB0 > 0` (0x8008D5CC). OURS, named: `0x800363F0`'s page test is answered "resolved" (the renderer holds
every cell texture from the start); the resource list `*(0x8005ACBC)` is rewritten each frame from the resident
cells, one record per cell in buffer order (flags 0x33 as every capture's loaded cell), because the streamer itself
is not ported. Measured, 100 races x 600 frames: 60000 passes, 272545 cells listed, 0 missing, 0 empty lists, 0
passes without a record. Race 1/3 `--start 7825`: the finish director's shots (mode 13, frames 632..) show the road,
the airfield and the pack; keying the cells on the route distance instead, which ends at the finish, leaves them
black below the horizon.
