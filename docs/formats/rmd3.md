# RMD3 / DOD3 - 3D model container (`*.GEO`) and the DMD3 animation sibling (`ANIMTBL*.PSX`)

Target: Road Rash: Jailbreak, USA release, volume `SLUS_01053`, EXE `SLUS_010.53`
SHA-1 `67ed165a2c517d4e6106fb0dfa324d66dd9a76f1`.
All byte evidence below comes from the disc bytes (extract in `work\disc_us\DATA`, never in git).
Parser: `src\rrformats\rmd3.{h,cpp}` (`rrtool geo` / `geoscan` / `asmcheck`); independent probe:
`tools\scout\rmd3.py`. Both are gates in `tests\run_gates.ps1` ("models: every *.GEO parses, byte for
byte", "models: every multi-part group assembles", "probe: models parse").

Status: **the whole RMD3 container is decoded byte-exactly.** All 112 `*.GEO` files parse with
every single byte assigned to a named field - no holes, no overlaps, no trailing slack
(`rmd3.py scan`, `rrtool geoscan`). Section 9 covers part assembly: all 43 multi-part groups
stand up. The points of meaning still open are listed in section 8.
The DMD3 sibling container is decoded, its per-block animation payload is not.

All integers are little-endian. "Offset +N" inside a chunk is relative to that chunk's 4-byte tag.

---

## 1. File layout

```
file   := RMD3+                                 # chained, the last one ends exactly at EOF
RMD3   := hdr[0x10] group+                      # group count = byte at RMD3+0x0C
group  := DOD3 DPD3{n} BBD3                     # n = u32 at DOD3+0x18
DOD3   := hdr[0x34] blobA verts [normals] [quadsD] [vertNormIdx]
DPD3   := hdr[0x1C] prim{m}                     # prim = 20 bytes
BBD3   := hdr[0x10] bbox[0x10]
```

Every chunk has the same 8-byte prologue: `char tag[4]; u32 size;` where `size` covers the tag
itself, so `offset + size` is the start of the next sibling chunk.

Verified on `CAR01A.GEO` (8640 B): RMD3 chunks at `0x000000` (size `0x7F4`), `0x0007F4`
(`0x7F4`), `0x000FE8` (`0x804`), `0x0017EC` (`0x9D4`) - `0x17EC + 0x9D4 = 0x21C0 = 8640` = EOF.
The same walk lands exactly on EOF for all 112 files.

### RMD3 header (0x10 bytes)

| off | type | name | evidence |
|---|---|---|---|
| +0x00 | char[4] | `"RMD3"` | |
| +0x04 | u32 | `size` - total chunk size, tag included | `CAR01A.GEO@0x0004 = 0x7F4`, next `RMD3` at `0x7F4` |
| +0x08 | u32 | `modelId` - global model id | `CAR01A.GEO@0x0008 = 301`; see §6 |
| +0x0C | u8 | `groupCount` | `CAR01A.GEO@0x000C = 3`; asserted against the actual DOD3 count for all 506 objects, 0 mismatches |
| +0x0D | u8 | build stamp low - values `0x42..0x61` | `CAR01A.GEO@0x000D = 0x46`, `BBLEVEL1.GEO@0x000D = 0x57` |
| +0x0E | u8 | build stamp high - always `0x76` | all 506 objects |
| +0x0F | u8 | always 0 | all 506 objects |

`modelId` is repeated at `+0x08` of every DOD3 / DPD3 / BBD3 inside the object; the parser
asserts it against the parent for all 6030 sub-chunks (1792 DOD3 + 2446 DPD3 + 1792 BBD3),
0 mismatches.

### BBD3 - bounding volume (always 0x20 bytes)

| off | type | name |
|---|---|---|
| +0x00 | char[4] | `"BBD3"` |
| +0x04 | u32 | `size` = `0x20` (all 1792 instances) |
| +0x08 | u32 | `modelId` |
| +0x0C | u32 | 0 |
| +0x10 | s16[4] | centre x, y, z, pad(=0) |
| +0x18 | s16[3] | half extents x, y, z |
| +0x1E | s16 | bounding radius |

The radius is `round(sqrt(hx^2+hy^2+hz^2))` and it is duplicated at `DOD3+0x10`.
`CARSC.GEO@0x0474`: centre `(0,-725,232,0)`, half `(872,736,2360)`, radius `2622`;
`sqrt(872^2+736^2+2360^2) = 2621.4`, and `DOD3@0x0010+0x10 = 0x0A3E = 2622`.
Same relation holds for `CARSC.GEO@0x06B0` (`(52,45,137)` -> `153.3` vs stored `154`) and for
all 1792 BBD3 chunks.

---

## 2. DOD3 - vertex / normal chunk

Fixed header of 0x34 bytes followed by up to five variable-length regions addressed by five
*relative byte offsets* stored in the header. The offsets are strictly increasing, the first
non-null one is always `0x34` (= end of header), and the last region ends exactly at the chunk
end. Null (0) means "region absent".

| off | type | name | notes |
|---|---|---|---|
| +0x00 | char[4] | `"DOD3"` | |
| +0x04 | u32 | `size` | |
| +0x08 | u32 | `modelId` | equals parent RMD3 |
| +0x0C | u32 | `flags` | low u16 always `0x0600`. Bit 30 (`0x40000000`) is set on the first group of 500 of the 506 objects and on 55 later groups - it reads as "this group starts a LOD chain". The remaining high bits take 22 distinct values (`0x0008,0x0010,0x0018,0x0028,0x0030,0x0032,0x0090,0x00A1,0x00B0,0x00B2,0x0121,0x0130,0x01B2,0x0232,0x02B0,0x02B2` and their bit-30 variants) - **unknown** |
| +0x10 | u32 | `radius` | identical to the BBD3 radius of the same group (1792/1792) |
| +0x14 | u32 | `scale`? | `4096` in 1786 of 1792 groups; `3932` and `4915` only in `PED01A.GEO` (`@0x000024 = 0x0F5C`, `@0x001450 = 0x1333`). Reads as 12.12 fixed point with `0x1000 = 1.0`, but the evidence is thin - **unconfirmed** |
| +0x18 | u32 | `subMeshCount` | number of DPD3 chunks that follow this DOD3. Asserted for all 1792 groups, 0 mismatches |
| +0x1C | u32 | `slot`? | constant per model id for 28 of the 31 ids (e.g. id 301 -> 117 in every file that carries it). Ids 300..315 map to 116..129 + 76/149, i.e. `modelId - 184` for 300..314. Breaks for id 200 (`HAZARD0/1/2 -> 50`, `HAZARD3 -> 148`, `HAZARD4 -> 147`, `HAZARD5 -> 150`) - **unknown** |
| +0x20 | u32 | `ofsBlobA` | always `0x34` when present |
| +0x24 | u32 | `ofsVerts` | |
| +0x28 | u32 | `ofsNormals` | |
| +0x2C | u32 | `ofsQuadsD` | |
| +0x30 | u32 | `ofsVertNormIdx` | |

Region A (`ofsBlobA`) has **no** count prefix; its length is `ofsVerts - 0x34`.
Regions at `ofsVerts`, `ofsNormals`, `ofsQuadsD`, `ofsVertNormIdx` are all
`u32 count;` followed by `count * stride` bytes, padded up to a 4-byte boundary.

| region | stride | content |
|---|---|---|
| blobA | - | **unknown**, see §2.1 |
| verts | 8 | `s16 x, y, z; s16 pad(=0)` (PS1 `SVECTOR`) |
| normals | 8 | `s16 x, y, z; s16 pad(=0)`, magnitude ~4096 - see §2.2 |
| quadsD | 8 | `u16 idx[4]` into the vertex array - see §2.3 |
| vertNormIdx | 2 | `u16` per vertex, index into `normals` - see §2.2 |

Worked example, `BBLEVEL1.GEO` DOD3 at `0x000010`, size `0xA74`:
offsets `0x34 / 0x48 / 0x6BC / 0x840 / 0x8D4`.
* blobA `0x34..0x48` = 20 bytes.
* verts: count at file `0x000058` = `0xCE` = 206; `0x48 + 4 + 206*8 = 0x6BC` -> exactly `ofsNormals`.
* normals: count at file `0x0006CC` = `0x30` = 48; `0x6BC + 4 + 48*8 = 0x840` -> exactly `ofsQuadsD`.
* quadsD: count at file `0x000850` = 18; `0x840 + 4 + 18*8 = 0x8D4` -> exactly `ofsVertNormIdx`.
* vertNormIdx: count at file `0x0008E4` = 206; `0x8D4 + 4 + 206*2 = 0xA74` = chunk size.

Simple case, `CAR01A.GEO` DOD3 at `0x000010`, size `0x1A0`: offsets `0x34 / 0x38 / 0 / 0x194 / 0`.
blobA = 4 bytes, verts count `0x2B` = 43 at file `0x000048`, `0x38+4+43*8 = 0x194`,
quadsD count 1 at file `0x0001A4`, `0x194+4+8 = 0x1A0` = size.

### 2.1 blobA - unknown

Length is exactly `4 * subMeshCount` in 1758 of the 1792 groups, so a per-sub-mesh u32 is the
obvious reading, but it is **falsified** by 34 groups where the region is shorter:
`PED01A.GEO@0x000044` is 60 bytes for 17 sub-meshes (68 expected) and
`PED01A.GEO@0x000CF4` is 44 bytes for 12 sub-meshes (48 expected). The content is therefore
variable-length. Observed bytes are small ordinals that look like sub-mesh indices plus opcodes,
e.g. `BBLEVEL1.GEO@0x000044`: `03 00 00 03 f3 01 01 00 03 02 00 04 03 01 00 02 01 00 00 00`;
`CAR01A.GEO@0x000044` (one sub-mesh) is `00 00 00 00`.
**Meaning unknown.** The parser treats it as an opaque byte range, which is why byte coverage
is still exact.

### 2.2 Normals and per-vertex normal indices - confirmed

`normals` entries have `pad == 0` in all 1083 entries in the game and a Euclidean length of
3840..4096 in 1037 of them; the other 46 are exactly `(0,0,0,0)` - one null entry per table,
always at index 0. 4096 is the PS1 `ONE` constant, so these are unit normals in 1.3.12 fixed
point. `BBLEVEL1.GEO@0x0006D8` = `(-2751, 1399, -2692)`, length 4095.4.

`vertNormIdx` always has exactly one `u16` per vertex (asserted; 0 mismatches over the 46 groups
that carry it) and every value is `< len(normals)` (9492/9492).
`BBLEVEL1.GEO@0x0008E8`: `0,0,0,0,0,0,1,2,3,3,4,5,...`, range 0..47 for a 48-entry normal table.

Normals exist only in the bike/rider models (`BBLEVEL*.GEO`, `BBLEVJB*.GEO`): 46 groups of 1792.
Cars, pedestrians and hazards carry no normals, i.e. they are drawn unlit / flat.

### 2.3 quadsD - untextured quad list: the shadow hull

`u16 idx[4]` into the same vertex array. Every one of the 1624 entries in the game is in range
(0 violations). These quads are **not** the drawn geometry: only 377 of 1624 share their four
indices with a DPD3 primitive, and there is no UV, CLUT or colour field.

* Cars carry exactly one: `CAR01A.GEO@0x0001A8` = `(33, 34, 20, 17)` ->
  `(-894,-261,-2021) (-713,-126,1891) (712,-126,1891) (892,-261,-2021)`, i.e. a single large,
  near-horizontal quad spanning the whole footprint just above the lowest point of the model.
* Bikes and riders carry many (`BBLEVEL1.GEO` id 100: 18, id 150: 47), including vertical ones.

**It is the SHADOW hull.** `SLUS 0x80025EE0` projects every entry onto the ground along the level's
light and emits one flat 0x2A quad per entry; the `rr-race` frame holds 65 of them, the bike's 18 plus
the rider's 47. Present in 516 of 1792 groups (493 of which are the first group of an object). The
product's shadow packets are checked against the original's by the gate "render: the bike's shadow
quads against the original's packets (rr-race)".

---

## 3. DPD3 - drawn primitives (one per sub-mesh)

| off | type | name | notes |
|---|---|---|---|
| +0x00 | char[4] | `"DPD3"` | |
| +0x04 | u32 | `size` | |
| +0x08 | u32 | `modelId` | |
| +0x0C | u8 | 0 | all 2446 chunks |
| +0x0D | u8 | `subMeshIndex` | 0-based ordinal of this DPD3 inside its group; asserted == loop counter for all 2446 chunks |
| +0x0E | u16 | `vertCount` | vertices this sub-mesh owns |
| +0x10 | u32 | `vertBase` | index of this sub-mesh's first vertex in the DOD3 array; asserted == running prefix sum of `vertCount` for all 2446 chunks |
| +0x14 | u32 | `ofsCount` | always `0x18` (all 2446 chunks) |
| +0x18 | u32 | `primCount` | |
| +0x1C | prim[] | 20 bytes each | `0x1C + 20*primCount == size` for all 2446 chunks |

`vertBase` / `vertCount` partition the DOD3 vertex array exactly: the sum of `vertCount` over a
group's sub-meshes equals the group's vertex count, and `vertBase` is the running prefix sum.
Asserted for all 1792 groups, 0 mismatches.

Important: this partition is **not** a polygon partition. A sub-mesh's primitives freely
reference vertices outside `[vertBase, vertBase + vertCount)` - `PED01A.GEO@0x000668`
(sub-mesh 3, `vertBase = 24`, `vertCount = 16`) uses indices 16..88. The partition assigns
*vertices* to a part/bone while the polygons stitch parts together, which is exactly what a
segmented character needs.

`BBLEVEL1.GEO` DOD3@`0x000010` (206 verts) -> 5 DPD3 at `0x000A84, 0x000AA0, 0x000C4C,
0x000DA8, 0x0010E4` with `vertCount` `4, 45, 19, 43, 95` (sum 206) and `vertBase`
`0, 4, 49, 68, 111`.

An empty sub-mesh (`primCount == 0`, size `0x1C`, e.g. `BBLEVEL1.GEO@0x000A84`) is legal;
65 of the 2446 chunks are empty.

### 3.1 Primitive record - 20 bytes, textured quad

This is the PS1 `POLY_GT4`/`POLY_FT4` texture block with the GPU command words stripped and a
4x `u16` index list appended.

| off | type | field |
|---|---|---|
| +0x00 | u8 | `u0` |
| +0x01 | u8 | `v0` |
| +0x02 | u16 | `clut` |
| +0x04 | u8 | `u1` |
| +0x05 | u8 | `v1` |
| +0x06 | u16 | `tpage` |
| +0x08 | u8 | `u2` |
| +0x09 | u8 | `v2` |
| +0x0A | u8 | `u3` |
| +0x0B | u8 | `v3` |
| +0x0C | u16 | `i0` |
| +0x0E | u16 | `i1` |
| +0x10 | u16 | `i2` |
| +0x12 | u16 | `i3` |

`CAR01A.GEO@0x0001CC` = `36 0D 11 20 | 4C 0D 00 00 | 36 1E 4C 1E | 01 00 02 00 03 00 04 00`
-> texel pairs `(0x36,0x0D) (0x4C,0x0D) (0x36,0x1E) (0x4C,0x1E)`, an axis-aligned 22x17 texel
rectangle; clut `0x2011`, tpage 0, indices 1,2,3,4. The texel pairs are **not** in index order:
index `i0` takes the pair at +0x04, `i1` the pair at +0x00, `i2` +0x08 and `i3` +0x0A (3.3).

**Indices are 0-based and absolute into the DOD3 vertex array** (not relative to `vertBase`):
across all 1792 groups the maximum index used is exactly `vertexCount - 1` in every single
group (1792/1792), which rules out 1-based indexing; index 0 is actually referenced in 646
groups.

### 3.2 Texture reference - what the model side says

This is the model half; the texture side is in `textures.md` (section 5a).

* `clut` is a PS1 CLUT id in its low 15 bits, bit 15 is an extra flag. Only six values exist in
  the entire game: `0x2011` (24862 prims), `0xA011` (6549), `0x1811` (2908), `0x2091` (1058),
  `0x9811` (258), `0xA091` (18). Low 15 bits decode as VRAM CLUT positions
  `x = (v & 0x3F) * 16 = 272`, `y = v >> 6` -> `(272,96)`, `(272,128)`, `(272,130)`.
  Bit 15 is the two-sided flag and bit 13 the quad flag of the model emitter (3.3); neither
  correlates with `tpage`.
* `tpage` is **not** a raw PS1 GPU tpage word - the values form a dense run
  (`0` for every CAR/CARSC/PED primitive, `0..9` in `BBLEVEL*`/`BBLEVJB*`, `0..34` in
  `HAZARD*`), which a packed GPU attribute never does. It is a small **texture-page index into
  the page set of the paired `*.TEX`**, resolved to a real tpage word at upload time.
  Note `HAZARD0.TEX` is a single 24600-byte `LECT` block while `HAZARD0.GEO` uses indices 0..30,
  so the index is into sub-images inside the block, not into the `LECT` chain.
* `u,v` are 8-bit texel coordinates inside the selected page (max observed 254).
* `CAR*.GEO` and `PED01A.GEO` have **no paired `.TEX`**; the only car-side texture file is
  `CARSC.TEX` (2 chained `LECT` blocks), consistent with every car primitive using `tpage = 0`.

### 3.3 How the model emitter reads a record **[proven]**

Read from `SLUS_010.53` (SHA-1 `67ed165a2c517d4e6106fb0dfa324d66dd9a76f1`), the model primitive
emitter `0x800251E4`, and checked against the original's own packets (`rr-race`, `rr-pack`):

* **The texel of i0 is the pair at +0x04, that of i1 the pair at +0x00.** The FT4 / GT4 packet is
  built as `(SXY i0, uv +4), (SXY i1, uv +0), (SXY i3, uv +10), (SXY i2, uv +8)`
  (`0x80025A08..0x80025A68`; the GT4 arm `0x80025A6C..0x80025BD4` the same). Reading the pairs in
  file order gives i0 the texel of i1, i.e. every textured quad drawn with its first two texels
  swapped (a folded texture). Measured: of the model packets of `rr-race` and `rr-pack` whose texel
  shape names one primitive of `BBLEVEL1.GEO` / `HAZARD4.GEO`, **181 carry the emitter's association
  and none the file order**. `ParseGeo` fills `Primitive::u/v` per corner (`src\rrformats\rmd3.cpp`);
  `rrview --propcheck` checks it on the props.
* **`clut` bit 15 = two-sided.** The word at +0x00 is loaded whole; `bltz` at `0x80025940` skips the
  GTE NCLIP for a negative one (bit 15 of `clut`); every other primitive is dropped when NCLIP over the
  screen corners (i0, i1, i2) is negative (`0x8002595C..0x80025970`). So `0x2011` / `0x1811` / `0x2091`
  primitives are one-sided and `0xA011` / `0x9811` / `0xA091` two-sided. The sign plates of the
  `HAZARD*` props come in front/back pairs of one-sided quads on the same four vertices (group 12:
  `(1,2,3,4)` and `(2,1,4,3)`), so exactly one of the pair is drawn from any side.
* **`clut` bit 13 = a quad.** `lui 0x2000; and` at `0x80025978`: clear takes the 3-corner arm
  (`0x80025BDC`, GP0 `0x24` / `0x34`, corners i0, i1, i2) - the `0x1811` and `0x9811` records.
  **[proven]** That arm emits exactly ONE packet (FT3: tag length 7, `t4 += 32`; GT3: length 9) of
  `(SXY i0, uv +4), (SXY i1, uv +0), (SXY i2, uv +8)`; its trivial reject ANDs the outcodes of i0, i1, i2 only
  (`0x80025BDC..0x80025BE4`), and neither the fourth index (+0x12) nor the texel at +10 is read on that path (the quad
  arm reads i3 at `0x800259B4` / `0x800259DC`). In the data the fourth index and the +10 texel of all **3166**
  3-corner records of the 112 `.GEO` files are 0 - padding, not a corner. The captures agree: matching the texel
  shape of every textured packet against every record, rr-race / quick / rr-pack each carry 7 GT3 packets with the
  (i0 +4, i1 +0, i2 +8) texels of a 3-corner record (bike 100, rider 150), **no** 3-vertex packet with the texels of
  an (i0, i2, i3) triangle and no 4-vertex packet with a 3-corner record's four texels. `BuildTriangleSoup` therefore
  draws a 3-corner primitive as one triangle (`rmd3.h SoupCorners`: the second soup triangle is the zero-area
  (i2, i2, i2), keeping six soup vertices a primitive).

---

## 4. What the geometry actually is (plausibility check)

Two readings suggested by the file names are wrong; the bytes say:

* `CAR<nn>A.GEO` / `CAR<nn>B.GEO` are **not** two LODs of one vehicle. Each is a *bundle*:
  `A` files hold 4 RMD3 objects, `B` files hold 6, all drawn from a pool of 16 distinct traffic
  car models with ids 300..315. `CAR01A.GEO` = ids `301, 310, 305, 309`;
  `CAR01B.GEO` = `301, 302, 314, 305, 307, 309`. The id-301 object is **byte-identical**
  in both (`CAR01A.GEO@0x000000` vs `CAR01B.GEO@0x000000`, 2036 bytes). See §6.
  The real LODs are the three groups *inside* each RMD3.
* `BBLEVEL<n>.GEO` are **not** level/scenery chunks - they are the **bikes and riders** for
  level `n`. `BBLEVEL1.GEO` id 100 LOD0 has half extents `(371, 516, 1083)`, i.e.
  742 x 1032 x 2166 units - motorcycle proportions. Ids 150/159 are the riders (17 sub-meshes,
  the same part count as the pedestrian). Road scenery lives in the `MRPS` `.STP` race segments.

| export (`rmd3.py obj`) | objects | verts | tris | AABB (x,y,z units) | verdict |
|---|---|---|---|---|---|
| `CAR01A_lod0.obj` | 4 cars, LOD0 only | 184 | 288 | 2146 x 1678 x 5938 (all 4 at origin) | plausible: ~46 verts / 72 tris per PS1 traffic car |
| `CAR01A.obj` | 4 cars, all 3 LODs | 316 | 456 | same | plausible |
| `CAR01B.obj` | 6 cars, all LODs | 450 | 674 | same | plausible |
| single car, id 301 LOD0 | - | 43 | 68 | 2146 x 1612 x 4298 | **plausible**: at ~1 unit = 1 mm that is a 4.30 m x 2.15 m x 1.61 m sedan |
| `BBLEVEL1_lod0.obj` | 6 objects, LOD0 | 1204 | 1610 | 857 x 1699 x 1997 | plausible for a bike + rider set, not for a level chunk |
| bike id 100 LOD0 | - | 206 | 270 | 742 x 1032 x 2166 | **plausible motorcycle** (2.17 m long, 1.03 m tall, 0.74 m wide) |
| `PED01A.obj` | 2 peds, all LODs | 578 | 504 | 705 x 278 x 466 | **only plausible as unassembled parts** - see below |
| `HAZARD0.obj` | 39 props | 571 | 402 | 223 x 312 x 98 | shapes fine, absolute scale suspect - see below |

Pedestrian: `PED01A.GEO` id 400 LOD0 is 148 vertices split over **17 sub-meshes** with BBD3
half extents `(811, 134, 324)`. As a standing human that is wrong; as 17 rigid body parts each
stored in its own local frame and laid out along X it is right - the total X extent of 1622
units is exactly a 1.62 m human. The parts must be placed by the bone transforms in the DMD3
animation tables (whose part count is also 17, §5). The OBJ export therefore shows a pile of
loose limbs at the origin, which is the expected result and not a parse failure.

**Per-LOD unit scale.** LOD1/LOD2 coordinates are consistently ~1/16 of LOD0 for the same object:
`CAR01A.GEO` id 301 radii `2534 / 156 / 154` (ratios 16.2, 16.5), id 310 `2622 / 154 / 154`,
`BBLEVEL1.GEO` id 100 `1256 / 77 / 72 / 74` (16.3, 17.4, 17.0). `DOD3+0x14` is 4096 for all of them;
the factor is the coordinate shift exponent in `DOD3+0x0E` bits 12..15 (section 9.6). The raw
multi-LOD OBJ exports therefore contain a small copy of each model nested inside the large one.
`HAZARD*.GEO` is one RMD3 with 39 groups that are 39 separate props, not LODs (no group carries the
bit-30 flag, none has a quadsD list), with radii 34..189 - about 16x smaller than car LOD0, i.e.
stored in the same reduced unit.

---

## 5. DMD3 - `ANIMTBL*.PSX` (12 files)

Container solved, payload not.

```
file  := DMD3+          # flat chain, no nesting; ends exactly at EOF in all 12 files
```

| off | type | name | evidence |
|---|---|---|---|
| +0x00 | char[4] | `"DMD3"` | |
| +0x04 | u32 | `size` | `ANIMTBLW.PSX@0x0004 = 0xD0`, next `DMD3` at `0xD0`; 27 blocks reach 5876 = EOF |
| +0x08 | u32 | `id` = 102 | all 512 blocks in all 12 files |
| +0x0C | u8 | 2 | all 512 blocks |
| +0x0D | u8 | 3 | all 512 blocks |
| +0x0E | u8 | `blockIndex` | 0-based, runs 0..n-1 in every file (e.g. `ANIMTBL1.PSX` 0..122 for 123 blocks) |
| +0x0F | u8 | `partCount` | 17 in the nine rider tables, 3 in `ANIMTBLW/ANIMTBSW`, 1 in `ANIMTBLB/ANIMTBSB`. 17 matches the 17 sub-meshes of `PED01A.GEO` id 400 and of the riders `BBLEVEL1.GEO` ids 150/159 |
| +0x10 | u16 | `A` - sample/key count | see below |
| +0x12 | u16 | `B` - **unknown** (frame interval?) | |
| +0x14 | u16 | `0x18` - offset of the payload | all 512 blocks |
| +0x16 | u16 | 0 | all 512 blocks |
| +0x18 | ... | payload, `size - 0x18` bytes | **not decoded** |

Block counts: `ANIMTBJ3` 14, `ANIMTBL1` 123, `ANIMTBL2` 17, `ANIMTBL3` 14, `ANIMTBLB` 1,
`ANIMTBLJ` 69, `ANIMTBLP` 13, `ANIMTBLS` 69, `ANIMTBLW` 27, `ANIMTBS1` 123, `ANIMTBSB` 1,
`ANIMTBSW` 41. One block = one animation clip.

What the payload gives up so far, from the two single-part files:
`ANIMTBLB.PSX@0x0018` (`A = 16`, `partCount = 1`) is three runs of 16 `s16`, i.e. three smooth
curves - `80,80,81,63,52,41,19,1,-11,-24,-23,-27,-23,-5,7,4`, then
`-202,-203,-204,-166,-120,-91,-96,-82,-65,-54,-48,-47,-47,-48,-45,-41`, then a third - which
ends at `0x78`, followed by 40 bytes (`= B`) of a packed trailer.
`ANIMTBSB.PSX@0x0018` (`A = 11`) holds the *same* three curves resampled to 11 points, so the
`L`/`S` file pairs are long/short variants of one animation set. There `B = 60` and the trailer
is 34 bytes, so `B` is not the trailer size; `A*B` is 640 vs 660 for the two variants of the
same clip, which is consistent with `B` being a per-key frame interval - **unconfirmed**.
For `partCount = 17` blocks the payload is not `17 * 3 * A * 2` bytes and two blocks with equal
`A` and `B` can have different sizes (`ANIMTBLJ.PSX`: `A=10, B=20` appears with bodies of 704
and 736 bytes), so the per-part tracks are variable-length / compressed. **Not decoded.**

---

## 6. Model id cross-reference

506 RMD3 objects across the 112 `.GEO` files carry only **31 distinct model ids**, and for
30 of them every instance of an id is byte-identical (SHA-1 over `RMD3+0x10 .. RMD3+size`).
The `.GEO` files are bundles that re-pack a shared model pool.

| id range | what | instances |
|---|---|---|
| 100..120 | bikes, per level (`BBLEVEL*`, `BBLEVJB*`) | 1..3 each |
| 150, 159 | riders (17 sub-meshes) | 5 each |
| 200 | hazard bundle (39 groups) | 6, and the **only** id whose payload differs between files (one variant per `HAZARD0..5.GEO`) |
| 300..315 | traffic cars | 18..43 each |
| 400, 430 | pedestrians | 1 each |
| 800 | 10-group accessory bundle in `BBLEVEL*` | 5 |

---

## 7. Tool

`tools\scout\rmd3.py` (Python 3.12, no dependencies):

```
python rmd3.py info <file.GEO>            chunk tree, counts, AABBs, tpage/clut sets
python rmd3.py obj  <file.GEO> <out.obj>  export RAW geometry as Wavefront OBJ
python rmd3.py obj  <file.GEO> <out.obj> 0   export LOD 0 only
python rmd3.py asm  <file.GEO> <out.obj> [lod]   export the ASSEMBLED rest pose (section 9)
python rmd3.py asmscan <dir>              assemble every multi-part group, score against BBD3
python rmd3.py skel [overlay]             print the attachment programs from RASHCDG.BIN
python rmd3.py scan <dir> [glob...]       parse every match, pass/fail per file
python rmd3.py dmd3 <glob> [-v]           walk the DMD3 chain of ANIMTBL*.PSX
```

`asm`, `asmscan` and `skel` additionally need the race overlay, because the part attachment
topology is in the overlay and not in the `.GEO` (section 9.2). They default to
`work/disc_us/RASHCDG.BIN` and accept `--overlay <path>`. Nothing is cached in the repository.

`scan` only reports `pass` when (a) the chunk walk lands exactly on EOF, (b) every declared
count multiplies out to exactly the region it must fill, (c) no two fields claim the same byte,
(d) **no byte of the file is left unclaimed**, and (e) every primitive index is in range and the
sub-mesh vertex ranges tile the vertex array exactly. Current result:

```
112 files: 112 pass (every byte accounted for), 0 fail
totals: objects 506, groups 1792, submeshes 2446, verts 51893, quads 35653
```

OBJ export notes: Y is negated (PS1 is Y-down, OBJ is Y-up); UVs are written as `u/256`,
`1 - v/256` and are only meaningful once the `.TEX` page selected by `tpage` is known;
each quad becomes two triangles `(0,1,2)`, `(1,3,2)`.

---

## 8. Still unknown

1. `DOD3+0x34` region ("blobA"): variable-length, `4 * subMeshCount` bytes in 1758 of 1792
   groups and shorter in 34. Excluded as the part-attachment table (section 9.4); still unknown.
2. `DOD3+0x0C` flags beyond bit 30 and bits 12..15 (the LOD shift, 9.6), and the constant low
   half-word `0x0600`.
3. `DOD3+0x1C` (`slot`): stable per model id except for `HAZARD*`; role unknown.
4. `DOD3+0x14` = 4096 almost everywhere; "uniform scale" is a guess resting on two `PED01A`
   values. A separate scan of all four images found no code that reads it.
5. The table that turns `prim.tpage` (a small dense prop/sub-image index, `textures.md` 5a) into
   the finished GPU tpage word `RASHCDI` caches at page-table entry `+0x08` (`textures.md` 5b) is
   not described here; the model -> `LECT` chunk -> palette binding is re-derived from the disc and
   diffed against captured states by `rrverify texbind`.
6. The DMD3 per-block animation payload for `partCount > 1`. Not needed for the rest pose
   (section 9.5) - it supplies the per-part 3x3 rotations that ride on top of it.

Resolved elsewhere in this document: the `quadsD` list is the shadow hull (2.3), `clut` bit 15 is
the two-sided flag (3.3), the ~16x LOD factor is the shift exponent of 9.6, and the attachment
program is chosen by call site and sub-mesh count (11.6).

---

## 9. Part assembly - how a multi-part model stands up

A `.GEO` group stores every sub-mesh **in its own local frame, all sharing the origin**. Drawing
the raw vertices gives a pile of overlapping parts - for `BBLEVEL1.GEO` model 100 the two wheels
(sub-meshes 2 and 3) sit exactly on top of each other inside the frame. Assembly needs one
translation per part.

**Result: all 43 distinct multi-part groups in the game assemble, and the assembled bounding box
reproduces the authored `BBD3` box to within 9 units** (models are 1000-2200 units across).
Command: `rmd3.py asmscan work\disc_us\DATA`; the C++ side is `rrtool asmcheck`. Assembled, model 100
is a motorcycle in side view with both wheels resting on the `y = 0` ground line.

### 9.1 Where the translation comes from **[established]**

Three sources were checked. Two are refuted with evidence:

* **Not injected at load.** The RAM image of a loaded model is byte-identical to the `.GEO`
  except for 26 words per object: `DOD3+0x20..+0x30` and `DPD3+0x14`, each replaced by
  `chunkAddress + offset`. Measured by diffing `work\oracle\vr_capture\ramdumps\ram_000200.bin`
  against the disc bytes for all 13 resident models (`ram_000200.bin` `0x80185424` = model 100 =
  `BBLEVEL1.GEO@0x000000`, 11052 bytes, 104 differing bytes, all inside those six words).
  This independently confirms section 2's pointer reading.
* **Not in the runtime per-part slot.** The instance binder `SLUS_010.53 0x8002FDEC` allocates
  `subMeshCount(group 0) * 24` bytes at `obj+0x04` (`lhu s2,24(dod3)` then `malloc(s2*24)` at
  `0x8002FEA4`..`0x8002FEBC`). The slot is `u32 dpd3Pointer` at `+0x00` and a **3x3 s16 rotation
  matrix** at `+0x04..+0x15`; `+0x16` is never written. The binder fills it with the identity at
  `0x8002FF98`..`0x80030038` (`li a0,4096`, then `sh zero` to `+6,+8,+10,+14,+16,+18` and
  `sh a0` to `+4,+12,+20`). **There is no translation field.** Live values from
  `ram_000200.bin` `0x801BDF1C` (the player bike, `obj 0x801B65D4`, LOD0, 5 parts) read
  `dpd3=0x80185EA8` + a full orientation for part 0, exact identity
  `4096,0,0 / 0,4096,0 / 0,0,4096` for part 1, and X-axis rotations for the wheels.
* **Not `blobA`.** See 9.4.

**It comes from the model's own vertices plus a topology table in the race overlay.**

Each link is *parent part, vertex offset*:

```
origin(part c) = origin(part p) + verts[ vertBase(p) + k ]
```

`vertBase(p)` is `DPD3+0x10` of part `p` (section 3). In the rest pose the rotations are the
identity, so the origins are plain sums. Under animation the same walk runs with
`origin(c) = origin(p) + R(p) * verts[vertBase(p)+k]`, `R` being the 3x3 matrix in the runtime
slot.

### 9.2 The attachment programs live in `RASHCDG.BIN`, not in the `.GEO` **[established]**

The consumer is `RASHCDG 0x80067064`. It reads `obj->parts` (`lw a0,4(s8)` at `0x800670BC`),
starts at `parts + 24` (`addiu s5,a0,24`, `0x800670C8`) and steps it by 24 per command word
(`addiu s5,s5,24`, `0x800672EC`) - **so the child part is the loop index, not a field**. Per
command word `w` it computes:

| expression | site | meaning |
|---|---|---|
| `(w >> 13) & 0x1F` | `0x800670FC` | parent part index; used as `parts + idx*24` to reach that part's `DPD3` and then `DPD3+0x10` = `vertBase` (`0x80067114`..`0x8006711C`) |
| `((w >> 6) & 0x70)` | `0x80067118` | added to `vertBase << 4` (`0x80067124`..`0x8006712C`), i.e. a **vertex offset `k` = bits 10..12**, in a 16-byte-per-vertex work buffer |
| `(w >> 23) & 0x1F` | `0x80067188` | the part whose 3x3 matrix is loaded into the GTE (`parts + idx*24 + 4`, `0x800671A0`..`0x800671B4`) |
| `w & 3` | `0x800670F4` | `3` marks a control word that is **not** a link |

The table is static initialised data inside the overlay (read-only: the only references in any
of the four images are the two `addiu` at `0x80067074` and `0x800670B8`; nothing writes it):

| what | address | `RASHCDG.BIN` offset |
|---|---|---|
| 49 command words, `u32` | `0x800CC790` | `+0x711A8` |
| 8 program records, `u8 startWord, u8 linkCount, u8 passes` | `0x800CC854` | `+0x7126C` |

Verified byte-identical between `work\disc_us\RASHCDG.BIN`
(SHA-1 `cfe43a7786759f2cb9c57751cf99e84d1074782c`) and `ram_000200.bin` over all 220 bytes.
**This repository stores no copy of that table**; `rmd3.py skel` / `asm` / `asmscan` and
`src\rrformats\skeleton.{h,cpp}` read it out of the player's own overlay at run time
(`python tools/scout/rmd3.py skel` prints it); the decoding rule above is all that is needed to read
it. What it contains, described rather than copied: eight
programs covering models of 3, 4, 5, 6, 12 and 17 parts, two of which (4 parts and 3 parts) exist
in two variants. The 17-part program is a textbook humanoid skeleton - root, a three-joint spine
to a head, two three-joint arms branching off the top of the spine, two three-joint legs off the
root - and the 12-part program is the same skeleton with the limb chains one joint shorter, which
is the LOD1 of the same characters. The 5-part program (the bikes) hangs the front wheel off the
fork and everything else off the root.

`0x80067064` takes the program index as its first argument; it is not read from the `.GEO`, and the
engine's selection by call site is in 11.6. Programs 2/6 both describe 4 parts and 5/7 both describe
3, so part count alone does not disambiguate. `rmd3.py` tries every program with the right part count
and keeps the one whose assembled AABB is closest to `BBD3` - which is decided in every case here
(4-part `id 800` picks 6, 4-part rider LOD3 picks 2) and agrees with 11.6.

### 9.3 Proof: the assembled model matches the authored `BBD3` **[established]**

`BBD3` (section 1) turns out to be the bounding box of the **assembled** model, which makes it a
ready-made oracle. Two checks:

* For a single sub-mesh model it equals the raw vertex AABB. `CAR01A.GEO` id 301 group 0: raw
  `x[-1074..1072] z[-2175..2123]`, `BBD3` `x[-1073..1073] z[-2174..2124]`.
* For a multi-part model it does not, and the gap is exactly what assembly closes.

Across all 43 distinct multi-part groups:

| | worst bound error vs `BBD3` |
|---|---|
| raw vertices, no assembly | **768 units** |
| assembled with the program above | **9 units** |

Worked example, `BBLEVEL1.GEO` model 100 group 0 (program 4), `rmd3.py asm`:

| part | verts | quads | origin | role, from where it lands |
|---|---|---|---|---|
| 0 | 4 | 0 | `(0,0,0)` | no geometry - it exists only to carry the four attachment vertices |
| 1 | 45 | 20 | `(0,-2,525)` | fork + handlebars, world `y[-1033..-214]` = the top of the bike |
| 2 | 19 | 16 | `(0,-292,803)` | front wheel, radius 291, world `y[-582..-1]`, `z[512..1095]` |
| 3 | 43 | 40 | `(0,-324,-752)` | rear wheel, radius 316, world `y[-633..-1]`, `z[-1069..-436]` |
| 4 | 95 | 59 | `(0,-503,0)` | frame, world `y[-942..-128]` |

`BBD3` says `x[-373..369] y[-1032..0] z[-1070..1096]`; assembled gives
`x[-374..368] y[-1033..-1] z[-1069..1095]`. Both wheels' lowest point lands on `y = -1`, i.e. on
the ground plane `y = 0` that `BBD3` defines, and the wheelbase comes out `803 - (-752) = 1555`
units **= 1.55 m at 1 unit = 1 mm, which is a real motorcycle wheelbase.** Note part 2 is a child
of part 1: the front wheel hangs off the fork, so it only lands correctly if the chain is walked
in order.

Second worked example, `BBLEVEL1.GEO` model 150 group 0 (the rider, program 0): parts 5-7 land at
`z ~ -201` and parts 8-10 at `z ~ +202` (the two arms), parts 11-13 at `z = -111` and 14-16 at
`z = +112` (the two legs), and the spine chain runs along `+x` to the head at `x = 562`. Total
`x` span 1680 units = 1.68 m, a human. The character is authored lying along `X`; putting it on
the bike is a separate, object-level step (9.5).

### 9.4 `blobA` is not the attachment table **[negative result]**

Item 1 of section 8 stays open, but one hypothesis is excluded. The attachment topology
is fully accounted for by the overlay table for all 43 groups, and `blobA` does not encode it:
`BBLEVEL1.GEO@0x000044` (model 100, 5 parts) is
`03 00 00 03 f3 01 01 00 03 02 00 04 03 01 00 02 01 00 00 00` while the program the geometry
demands is `(0,1) (1,1) (0,2) (0,3)`; `BBLEVEL1.GEO@0x00F980` (model 800, 4 parts) is
`fd 00 00 03 00 01 01 00 02 01 00 03 01 00 00 00` against a demanded `(0,1) (1,1) (2,1)`. No
alignment of those bytes yields the parent/`k` pairs. `blobA`'s length is still `4 * subMeshCount`
in 1758 of 1792 groups and shorter in the 34 `PED01A`-family groups, so it is per-part
*something* - **meaning still unknown**.

### 9.5 What assembly does *not* cover

* **Object-to-object attachment** (rider onto bike) is a separate mechanism, already located:
  `RASHCDG 0x80066B98` computes `childObj.pos = parentObj.pos + R(parent) * v` where `v` is
  vertex **3** of sub-mesh 0 when `DOD3+0x18 < 6`, else vertex **4**
  (`lw a0,36(dod3)` = the vertex region pointer, `addiu a0,a0,4` to skip the count,
  `lw v1,16(dpd3)` = `vertBase`, `sll v1,v1,3`, then `addiu a1,a0,24` or `addiu a1,a0,32` at
  `0x80066CBC`/`0x80066CC4`). The object struct holds its world position as three `s32` at
  `obj+0x0C/0x10/0x14` and a 32-byte matrix at `obj+0x68`. Not implemented in `rmd3.py`.
* **Animation.** The 3x3 rotations that ride on the rest pose come from the `DMD3` tables
  (section 5). The rest pose itself needs none of it - the binder's identity initialisation is
  what a freshly loaded model draws with, and that is the pose that matches `BBD3`.

### 9.6 The LOD coordinate shift

`DOD3+0x0E` bits 12..15 is a **coordinate shift exponent**. `RASHCDG 0x80066CDC` computes
`shift = base - (flagsHi >> 12)` and applies it to each vertex component with `sllv` when
positive (`0x80066CF0`) and `srav` when negative (`0x80066D2C`). Measured over all 1792 groups
the field takes exactly two values: **4** in 555 groups and **0** in 1237, and it agrees with the
bit-30 LOD flag on 1786 of 1792 groups. A group with exponent 0 must therefore have its vertices
multiplied by `2^4 = 16` to sit in the same unit as an exponent-4 group - matching the ~16x ratio
measured in section 4 (`CAR01A.GEO` id 301 radii `2534 / 156 / 154`, ratios 16.2 and 16.5).
`rmd3.py asm` applies `1 << (4 - exponent)`.

---

## 10. The player's machine - which models, and how they go together

Section 9 stands a model up. This section says **which** models the player actually is, because the
answer is not in the `.GEO`: it is read out of the captured state
`work\oracle\state\rr-race\ram.bin`, and it settles a question the format alone cannot.

### 10.1 The two objects **[proven]**

| what | address | value |
|---|---|---|
| the player object | `0x801B65D4` | `obj+0x00 = 0x80185434` |
| the model that address is group 0 of | `RMD3` at `0x80185424` | `modelId` **100** - a bike |
| its parts array | `obj+0x04` | `0x801BDF1C`, 5 slots of 24 bytes |
| the child object | `obj+0x38` | `0x801BB2EC` |
| the child's model | `RMD3` at `0x8018D750` | `modelId` **150** - a rider, 17 sub-meshes |
| the child's parts array | `0x801BB2EC + 0x04` | `0x801BDF9C`, 17 slots of 24 bytes |

Both live in `DATA\BBLEVEL1.GEO` for level 1, and the whole resident model set of that state is
100, 109, 118 (bikes), 150, 159 (riders), 200 (props), 301, 303, 306, 309 (cars), 400, 430
(pedestrians) and 800 (the accessory bundle) - found by scanning the RAM for `RMD3` tags.

### 10.2 Where the rider sits on the bike **[position proven; orientation measured in 11.5]**

9.5 located `RASHCDG 0x80066B98`: `childObj.pos = parentObj.pos + R(parent) * v`, with `v` vertex
**3** of sub-mesh 0 when the parent group has fewer than 6 sub-meshes. Model 100 group 0 has 5, and
its sub-mesh 0 carries four vertices and **no polygons at all** - it exists only to hold attachment
points. Vertex 3 is `(0, -503, 0)`, i.e. 503 units above the ground plane on the centre line, which
is where a saddle is.

What that call does **not** give is the child's own 3x3. That comes from the animation tables, whose
payload is not decoded (section 5), so the rest pose - a human lying along +X with the limbs spread
along Z (9.3) - has to be rotated by something. The rotation is measured in section 11.5: the two
objects' own world orientations are slot 0 of each parts array, so
`transpose(bike slot 0) * rider slot 0` is the rider's orientation relative to the bike and nothing
else. This repository stores no copy of those matrices; `rrview --pose` reads them out of the
player's own dump.

### 10.3 Which sheet each sub-mesh takes **[proven]**

`prim.clut` bit 7 is read once per sub-mesh, from its **first** primitive
(`SLUS_010.53 0x800257B0`). For model 100 group 0 that is: sub-mesh 1 (fork) and 4 (frame) clear,
sub-meshes 2 and 3 (the two wheels) set - so the wheels take the shared rim sheet `DATA\RIMA1.TIM`
and everything else `BBLEVEL1.TEX` LECT `0x06`. All 17 rider sub-meshes are clear and take LECT
`0x01`. Checked per pixel against the captured frame by `rrview --bikecheck` (`textures.md` 1.3).

---

## 11. A pose: what the runtime 3x3 is made of, and how it composes

Section 9 stands a model up in its rest pose. This section says what the per-part rotations the rest
pose leaves at the identity actually are. Everything below is checked by `rrview --posecheck` and,
independently, by `tools\scout\anim.py`.

Sources, all read at run time out of the player's own files - this repository stores none of them:
`work\oracle\state\rr-race\ram.bin` (the live slots), `work\disc_us\SLUS_010.53` (the sine table)
and `work\disc_us\RASHCDG.BIN` (the attachment programs, SHA-1
`cfe43a7786759f2cb9c57751cf99e84d1074782c`).

### 11.1 Slot `i` belongs to sub-mesh `i` **[proven]**

The binder allocates `subMeshCount(group 0) * 24` bytes at `obj+0x04` and writes a `u32 dpd3`
pointer at `+0x00` of each slot (9.1). That pointer is the check: for the player's bike (5 slots at
`0x801BDF1C`) and its rider (17 slots at `0x801BDF9C`), **all 22 slots hold exactly the guest
address of the `DPD3` of the group-0 sub-mesh with the same index** - the RMD3 base in RAM comes
from `obj+0x00 - 0x10`, and the sub-mesh offsets come from the `.GEO` on the disc.
`anim.py slots` prints the pairing.

### 11.2 The stored 3x3 is a LOCAL rotation, and the chain multiplies **[proven]**

`RASHCDG 0x800671DC`..`0x800672E4` is the composition, one column at a time:

| site | what it does |
|---|---|
| `0x800671B4`..`0x800671D8` | copies the 20-byte slot matrix to the scratchpad work pointer |
| `0x800671EC`..`0x80067210` | `ctc2 $0..$4` from the stack entry 32 bytes BELOW it - the parent's accumulated matrix |
| `0x80067214` | `lhu` at `+0`, `+6`, `+12` of the copied matrix, i.e. its column 0 |
| `0x80067234` | `c2 0x49E012` - `MVMVA`, rotation matrix times the IR vector, `sf = 1` |
| `0x80067244` | `sh` to `+0`, `+6`, `+12` of the destination: column 0 of the product |
| `0x80067250`..`0x800672E4` | the same twice more, at `+2` and `+4`: columns 1 and 2 |

So `world(child) = world(parent) * local(child)`, both row-major in the GTE's own order
(`R11 R12 R13 R21 R22 R23 R31 R32 R33`), `4096 = 1.0`. The vertices of the part are then put through
`SLUS_010.53 0x800220A4` with that matrix and the accumulated translation
(`ctc2 $5..$7` from `+20`, `+24`, `+28` of the same 32-byte stack entry).

Slot 0 is different in kind: it is the **object's own world orientation**. `RASHCDG 0x80066DC8`
reads `obj->parts + 4` and feeds it to the GTE as the object matrix when `obj+0x48` is not 3.

### 11.3 A link names WHICH slot's matrix it loads - and it is not always the child **[proven]**

The field `(w >> 23) & 0x1F` read at `0x80067188` (9.2) names the slot whose 3x3 matrix is loaded
into the GTE for a link, and that slot is not always the child:

* in the 17-part rider program the field equals the child index for every link, so the two coincide;
* in the **5-part bike program** the links place parts 1, 2, 3, 4 with the matrices of slots
  **1, 3, 4, 2**.

That is exactly what the captured state contains and what makes it read as a motorcycle: slots 3 and
4 hold the *same* X-axis rotation and slot 2 the identity, so the two wheels share one spin angle and
the frame stands still. Applying slot `i` to part `i` instead folds the frame over by the wheel
angle. `AttachLink::matrixPart` in `src\rrformats\skeleton.h` carries the field.

### 11.4 The 3x3 is `RotMatrix` of an integer Euler triple **[proven]**

> Every live part matrix is `M = Rz(vz) * Ry(vy) * Rx(vx)` of an integer triple in the PS1 unit of
> **4096 per full turn**, built from the game's own 4096-entry `(s16 sin, s16 cos)` table at guest
> `0x8005624C` (`SLUS_010.53 +0x4644C`, entry stride 4, index `angle & 0xFFF`).

That is the psyq `RotMatrix` order. The table is indexed exactly this way by the sky routine at
`RASHCDG 0x80063CE8` and `0x80063E20`, and it checks itself: entry 0 is `(0, 4096)` and entry 1024
is `(4096, 0)`.

The measurement: recover the triple by the closed-form inverse of that product
(`vy = asin(-m20)`, `vx = atan2(m21, m22)`, `vz = atan2(m10, m00)`), refine it over a +-3
neighbourhood, rebuild the matrix in the same fixed point and compare element by element.

| | matrices compared | matching | differing | worst element |
|---|---|---|---|---|
| `rrview --posecheck` | 22 | **22** | 0 | **3** of 4096 |
| `anim.py rot` (independent) | 22 | **22** | 0 | **3** of 4096 |

**Tolerance 3 of 4096, and why it is not generous.** One angle unit moves an element of the matrix
by up to `2*pi*4096/4096 = 6.3`, so a tolerance of 3 cannot absorb even the smallest possible wrong
angle; it only absorbs the sine table's own quantisation and the order of the two 12-bit shifts in
the product. The negative control `--posecheck-mutate` builds the same three angles in the opposite
order (`Rx * Ry * Rz`): **5 of the 22 matrices it cannot move at all** (the identities and the pure
X rotations, where the two orders agree by construction) and **0 of the 17 it does move still
match**, the worst element reaching 5799. `anim.py rot --mutate` reproduces both numbers.

Of the 22 triples, 7 have at least one angle exactly 0 and the two bike wheels share the same
non-zero angle to the unit - the kind of structure a wrong convention does not produce.

### 11.5 The rider's orientation on the bike **[measured]**

Slot 0 of each parts array is that object's world orientation (11.2), so

```
riderRelative = transpose(bike slot 0) * rider slot 0
```

is the rider's orientation **relative to the bike**, free of the console's world frame. Read out of
`rr-race` it maps, to three decimals:

| rider model axis | lands on, in bike model coordinates |
|---|---|
| +X (the spine) | (0.001, -0.914, -0.409) - up, with a 24 degree forward lean |
| +Y | (0.145, -0.405, 0.903) - forward and slightly up |
| +Z (the arms) | (-0.990, -0.060, 0.131) - the lateral axis, **negative** |

Against the naive permutation "+X up, +Y forward, +Z lateral" (the only axis assignment that puts
the head above the tank and the legs either side of it), two axes agree, the third has the opposite
sign, and there is a 24 degree forward lean. `rrview --pose` uses the measured matrix; without it the
rider is drawn upright with the arms out sideways.

### 11.6 How the engine picks an attachment program **[proven]**

Three call sites read `DOD3+0x18` (the sub-mesh count) and switch on it.

| site | sub-mesh count -> program |
|---|---|
| `RASHCDG 0x8006745C` | 17 -> 0, 12 -> 1, 4 -> 2 |
| `RASHCDG 0x80066F50` | 6 -> 3, 5 -> 4, fewer than 5 -> 5 |
| `RASHCDG 0x80066B28` | 4 -> 6, 3 -> 7 |

Two counts appear twice (4 -> 2 or 6, 3 -> 5 or 7), so it is the *call site* - which kind of object
is being placed - that disambiguates, not the model. For the player's machine the site is
`0x80066F50`, which gives the bike (5 sub-meshes) program 4 and the rider (17) program 0 through
`0x8006745C`; both agree with the `BBD3` score `rmd3.py` uses (9.2).

### 11.7 What section 11 does NOT settle

1. **Where the angles come from frame to frame.** The `DMD3` payload (section 5) is still not
   decoded, so a pose has to be measured out of a capture. What 11.4 adds is the shape of what to
   look for: three integer angle tracks per part in 4096-per-turn units. A direct search of all
   twelve `ANIMTBL*.PSX` for the 22 measured triples, to +-4 and at strides of 1, 2 and 3
   halfwords, finds **21 of them nowhere**; the twenty-second is the near-identity triple
   `(-1, -7, -38)`, which turns up three times in `ANIMTBL1.PSX` and four in `ANIMTBS1.PSX` at
   unrelated offsets, i.e. at the rate coincidence predicts for three small numbers. That is what
   a live pose interpolated BETWEEN keys looks like, and it is a negative result, not a decode.
2. **The scratchpad stack discipline.** Bits 0..1 and 2..3 of a link word move the 32-byte matrix
   stack at `0x1F8002BC` and later fields unwind it; `pose.h` composes through `world[parent]`
   instead, which is the same hierarchy expressed without the stack. The two agree on the rest pose
   for all 43 multi-part groups (that is what `rmd3.py asmscan` measures against `BBD3`), but no
   posed case has been compared against the console's own scratchpad.
3. **`blobA`** (item 1 of section 8) is still unknown, and the `passes` byte of a program record is
   still only used as a loop count.
