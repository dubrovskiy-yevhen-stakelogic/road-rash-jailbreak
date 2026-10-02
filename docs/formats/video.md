# Road Rash: Jailbreak (USA, SLUS_01053) — video / MDEC containers

Everything below was derived from the disc bytes (extract in `work\disc_us`) and from the
game's own code in `SLUS_010.53` (SHA-1 `67ed165a2c517d4e6106fb0dfa324d66dd9a76f1`, load
`0x80010000`, PS-X EXE header `0x800`). The product's decoder is `src\rrformats\mdec.{h,cpp}` (the MDEC
arithmetic, shared with the interpreter's MDEC device).

Three containers carry the same payload — PS1 MDEC "BS" version‑2 bitstreams:

| Container | Files | Content |
|---|---|---|
| `.STR` (magic `MDEC`) | 48 in `DATA\FE` | frontend artwork and short animations |
| `.WVE` (magic `VLC0`) | 11 in `DATA\FE` | full-motion cutscenes, video **and** audio |
| `.TCM` (no magic) | `FSLOAD.TCM`, `SSLOAD.TCM` | animated loading screens |

`DATA\STREAM1.STR` / `STREAM2.STR` (118 MB / 74 MB) are **not** MDEC — they start `64 02 80 42`
and contain `PANO` records; they belong to the in-race road streamer, not to this family.

---

## 1. Containers

### 1.1 Chunk chain (`.STR`, `.WVE`)

Both are a flat chain of chunks, no directory:

| off | type | meaning |
|---|---|---|
| +0x00 | `char[4]` | tag |
| +0x04 | `u32be` | chunk size, **including** this 8-byte header |

The next chunk starts at `off + size`. Verified: on all 59 files the chain lands exactly on
`filesize`. Sizes are always a multiple of 4 (`.WVE`) / 16 (`.STR`).

Tags seen on this disc: `VLC0`, `MDEC`, `au00`, `au01`.
The dispatcher in `RASHCDF.BIN` (file offset 0x3F80‑0x41C8) compares against more tags than the
disc uses: `MDEC`, `MDC2`, `VLC0`, `au00`, `au01`, `ad10`, `ad11`, `ad20`, `ad21`, `Ad10`, `Ad11`.

`.STR` layout: `MDEC`, `MDEC`, … — one chunk per picture, no audio.
`.WVE` layout: `VLC0`, `au00`, `au00`, then repeating `{4 × MDEC, 1 × au00}`, final audio chunk
tagged `au01`. So audio runs two chunks ahead of video and one audio chunk covers 4 video frames.

### 1.2 `VLC0` chunk (`.WVE` only, always first)

| off | type | meaning |
|---|---|---|
| +0x00 | `char[4]` | `"VLC0"` |
| +0x04 | `u32be` | 456 (= 8 + 448) on all 11 files |
| +0x08 | `u16le[224]` | per-file entropy-coder **symbol table** (see §3) |

The payload pointer (`chunk + 8`) is handed straight to the table builder at `0x800202B8`
(`RASHCDF.BIN:0x4078 → jal 0x8002026C → jal 0x800202B8`).

### 1.3 `.TCM` archive

| off | type | meaning |
|---|---|---|
| +0x00 | `u32le` | entry count |
| +0x04 | `{u32le size; u32le offset}[count]` | each entry is one `MDEC` chunk |

`FSLOAD.TCM` count = 64, table ends at 0x204 = first offset; `SSLOAD.TCM` count = 36, table ends
at 0x124 = first offset. Entries are contiguous and the last `offset+size` equals the file size
exactly for both files. Every entry is a 384×240 `MDEC` chunk — the animated "Loading…" map.

### 1.4 `MDEC` chunk

| off | type | meaning | evidence |
|---|---|---|---|
| +0x00 | `char[4]` | `"MDEC"` | |
| +0x04 | `u32be` | chunk size incl. header | chain closes on file size |
| +0x08 | `u16be` | **width** in pixels | 320 / 256 / 384 / 336 / 304 / 224 |
| +0x0A | `u16be` | **height** in pixels | 224 / 192 / 112 / 128 / 240 / 144 / 32 |
| +0x0C | `u32be` | frame index, 0-based | increments per chunk in `.WVE`; **always 0** in `.STR` and `.TCM` |
| +0x10 | `u16le` | `nwords` — 32-bit words the entropy decoder will emit | reproduced exactly, see §5 |
| +0x12 | `u16le` | `0x3800` — MDEC "BS" magic | constant on all 8091 frames |
| +0x14 | `u16le` | `qscale` (1…13 observed) | fed to the MDEC DC halfword |
| +0x16 | `u16le` | version = 2 on every frame | |
| +0x18 | … | entropy-coded bitstream | |

The decoder at `0x80020400` reads the BS header from `chunk+0x10`, copies `nwords` and `0x3800`
to the MDEC command stream unchanged, keeps `qscale`, **skips the version halfword**, and starts
the bitstream at `chunk+0x18`.

---

## 2. Bitstream mechanics

The bitstream is a sequence of **16-bit little-endian words**; bits are consumed **MSB first**
within each word. `0x80020400` keeps a 32-bit accumulator `a3` and refills it from
`*(u16*)ptr++` whenever `bitpos & 0x10` is set.

Per 8×8 block:

1. `dc = next 10 bits`.
   * `dc == 0x1FF` → **end of frame** (`0x80020454: li v0,511; beq a0,v0,end`). This is the
     only frame terminator — there is no coded block count.
   * otherwise emit the MDEC halfword `(qscale << 10) | dc`.
2. AC loop: decode one codeword (§3) → a 16-bit MDEC halfword `sym`.
   * `sym == 0x7C1F` → **escape**: emit the *next raw 16 bits* of the bitstream as the halfword
     and keep looping (`0x80020528…0x80020568`).
   * emit `sym`; if `sym == 0xFE00` (MDEC EOB) the block ends.
3. When the frame ends, the emitted halfword stream is padded with `0xFE00` until its length is a
   multiple of 128 bytes (`0x8002056C…0x800205B4`). Hence
   `nwords == ceil(emitted_halfwords / 64) * 64 / 2` — this is checked for every frame (§5).

Block order inside a macroblock: **Cr, Cb, Y1, Y2, Y3, Y4** (Y1 = top-left … Y4 = bottom-right).
Macroblock order across the frame is **column-major**: down a 16-pixel column, then the next
column to the right (the standard PS1 `.STR` order). Proven visually — row-major produces sheared
images, column-major produces the correct artwork.

---

## 3. Entropy coder (the "VLC" layer)

`vlc_build_tables` at `0x800202B8` builds two lookup tables from three arrays in the EXE:

| VA | file off | size | content |
|---|---|---|---|
| `0x800528A0` | 0x430A0 | 96 × 4 | short codes: `u8 len (2…13); u8 pad(=0); u16 code<<(16-len)` |
| `0x80052A20` | 0x43220 | 128 × 4 | long codes: `u8 len (6…9) *after* 8 leading zero bits; u8 pad; u16 code<<(16-len)` |
| `0x80052C20` | 0x43420 | 224 × 2 | **default symbol table** (used when a stream has no `VLC0`) |

`0x800202E0`: if the caller passes a null symbol pointer, `0x80052C20` is substituted — that is how
`.STR` and `.TCM` decode without a `VLC0` chunk.

Decode path (`0x80020494`): peek the top 13 bits. If the value is ≥ 32 (i.e. the top 8 bits are
not all zero) it is a **short** code — index the 13-bit table directly. Otherwise consume 8 bits
and use the next 9 bits as an index into the **long** table.

So there are exactly **224 codewords**, and their lengths are

```
1×2, 2×3, 2×4, 4×5, 7×6, 8×7, 8×8, 16×9, 16×11, 32×13, 32×14, 32×15, 32×16, 32×17
```

This codeword *set* is byte-for-byte the MPEG-1 intra AC code space (ISO 11172-2 table B-14),
including the `000001` escape word — re-derived here from our own EXE, not assumed. Kraft sum
= 4095/4096; the unused space is the all-zeros 12-bit prefix.

Index order (as the builder walks the descriptor arrays): ascending code length, ascending numeric
value within a length, **except** that the 6-bit word `000001` is moved to index 23, i.e. placed
after the 7-bit group. Index 23 is exactly the slot that carries the escape sentinel `0x7C1F` in
the EXE default table *and* in all 11 `VLC0` chunks.

### Symbol table (`VLC0` payload, or the EXE default)

224 little-endian `u16`, one per codeword, each already in **MDEC halfword** form:

```
bits 15..10  run   (number of zero coefficients to skip)
bits  9..0   level (signed 10-bit)
0xFE00       EOB
0x7C1F       ESCAPE -> next raw 16 bits are the halfword
```

The EXE default table is the classic MPEG-1 (run, level) assignment. A `.WVE`'s own table is the
same 224 codewords re-assigned by *frequency for that movie*: e.g. `EA_LOGO.WVE` gives the 2-bit
word to `EOB` (a mostly black film), `BUSTED.WVE` gives it to `(0, +1)` and pushes `EOB` to a
3-bit word.

Beyond the codeword set, our EXE differs from the textbook MPEG-1 B-14 assignment in exactly 4
entries: `00000011010 / 00000011011` are `(15, ±1)` and `00000010000 / 00000010001` are
`(16, ±1)` — i.e. runs 15 and 16 are swapped relative to the published table. Reading the table
from the EXE avoids the question entirely.

---

## 4. MDEC back end (halfwords → pixels)

* Coefficient `k` walks the zig-zag order; `k += run + 1` per halfword; a block also ends at
  `k == 63`.
* Dequantisation: `coef[zigzag[k]] = (level * QUANT[zigzag[k]] * qscale) >> 3`, and
  `coef[0] = dc * QUANT[0]` (the DC term is **not** scaled by `qscale`).
* `QUANT` is the MPEG-1 default intra matrix with entry 0 replaced by 2:
  `2,16,19,22,26,27,29,34, 16,16,22,24,… ,69,83`.
* 8×8 IDCT, then `+128`, then
  `R = Y + 1.402·Cr`, `G = Y − 0.3437·Cb − 0.7143·Cr`, `B = Y + 1.772·Cb`, chroma upsampled 2×2.

Confirmation that the matrix orientation and colour matrix are right: `MODES.STR` frame 0 decodes
to the clean blue/steel "ROAD RASH" logo, `TROPHY.STR` to a gold trophy, `BIKES.STR` to a
correctly lit motorcycle, `FSLOAD.TCM` #20 to the readable "Loading…" course map.

---

## 5. Evidence that the decode is exact

A full scan decodes **every frame of every file** (8091 frames: 2644 in `.STR`, 5347 in `.WVE`,
100 in `.TCM`) and requires, per frame:

1. the DC sentinel `0x1FF` is reached (never an undefined codeword, never a run past 63);
2. the number of 8×8 blocks decoded equals `ceil(w/16)·ceil(h/16)·6` from the chunk header;
3. `ceil(emitted_halfwords/64)·64/2` equals the header's `nwords` field, exactly;
4. what is left over in the chunk is only its zero padding.

All four hold for all 8091 frames.
Note that checks 1–3 validate the code *lengths* and `EOB`/escape placement but not the
(run, level) values; those are validated visually (§4) and by construction (§3, read from the EXE).

Frames inspected visually: `MODES` 0, `LEGAL` 0, `BIKES` 0, `TROPHY` 0, `RR_LOGO` 0, `CNTRLLR` 0,
`TTL_CON2` 0, `BGRND3` 0, `EA_LOGO` 20, `INTRO` 300, `JAILBRAK` 400, `FSLOAD` 0/20, `SSLOAD` 0.

The product's decoders are also checked word for word against the original's own decode path
(`DctVlc` + `DecDCTReset` / `DecDCTin` / `DecDCTout`) running on the interpreter's MDEC device:
`rrverify mdec` (gate "mdec: the product's decoders against the original's code on the MDEC device" in
`tests\run_gates.ps1`, 66 film frames of 10 files, with a negative control `--device-model legacy`
that must fail).

---

## 6. Inventory

### 6.1 `.WVE` cutscenes (15 fps — see `audio.md` §2.1 for the derivation)

| file | bytes | frames | size | duration | qscale range |
|---|---:|---:|---|---:|---|
| `EA_LOGO.WVE` | 763 328 | 75 | 320×224 | 5.00 s | 1–2 |
| `CREDITS.WVE` | 3 893 252 | 266 | 320×192 | 17.73 s | 1–4 |
| `GAUNTLET.WVE` | 4 790 520 | 356 | 320×192 | 23.73 s | 1–3 |
| `MOVINGUP.WVE` | 5 506 476 | 443 | 320×192 | 29.53 s | 1–3 |
| `BUSTED.WVE` | 6 817 304 | 491 | 320×192 | 32.73 s | 1–4 |
| `SPAZPUNT.WVE` | 7 503 492 | 522 | 320×192 | 34.80 s | 1–5 |
| `THESETUP.WVE` | 7 664 192 | 531 | 320×192 | 35.40 s | 1–5 |
| `GANGS.WVE` | 7 874 056 | 536 | 320×192 | 35.73 s | 1–5 |
| `INTRO.WVE` | 9 656 268 | 632 | 320×224 | 42.13 s | 1–7 |
| `GOT_OINK.WVE` | 9 649 008 | 728 | 320×192 | 48.53 s | 1–4 |
| `JAILBRAK.WVE` | 10 356 400 | 767 | 320×192 | 51.13 s | 1–4 |

Sustained bitrate is 150–205 KB/s, i.e. within double-speed CD-ROM (300 KB/s); at 30 fps these
files would need 300–410 KB/s, which the drive cannot deliver. That is an independent check on the
15 fps figure.

### 6.2 `.TCM` loading screens

| file | bytes | frames | size |
|---|---:|---:|---|
| `FE\FSLOAD.TCM` | 1 189 636 | 64 | 384×240 |
| `FE\SSLOAD.TCM` | 674 740 | 36 | 384×240 |

### 6.3 `.STR` frontend pictures

All use the EXE default symbol table. `frames` is the number of `MDEC` chunks; the +0x0C index
field is 0 in every one of them, so the ordering is purely positional.

**Single frame (still artwork):** `BGRND3.STR` 304×144, `TTL_CON2.STR` 224×32.

**Short sequences / sprite sheets (2–13 frames):** `BGRND2.STR` 4×256×128, `LEGAL.STR` 4×256×128,
`LOSE.STR` 5×256×112, `WIN.STR` 5, `WRECK.STR` 5, `ESCAPE.STR` 6, `FIVEO.STR` 6, `ALIAS.STR` 8,
`JAILED.STR` 8, `DES_LOSE.STR` 10, `DES_WIN.STR` 10, `KAF_LOSE.STR` 10, `KAF_WIN.STR` 10,
`BUST.STR` 11, `CNTRLLR.STR` 12×256×128, `MISCRES.STR` 12, `MODES.STR` 13.

**Longer sequences (21–120 frames):** `BIKES.STR` 21, `TITLES.STR` 23×336×32, `MODEH2H.STR` 30,
`MODEJB.STR` 30, `MODETT.STR` 30, `MODESCAR.STR` 31, `TROPHY.STR` 31, `OPTIONS.STR` 34,
`SSCOURSE.STR` 36, `MODEFIVO.STR` 44, `RR_LOGO.STR` 45, `FSCOURSE.STR` 57, `MODECNR.STR` 60,
`MODEH2HS.STR` 120.

**91-frame course/rider animations, all 256×112:** `COP1‑3.STR`, `CRUISEA1‑3.STR`,
`CRUISEB1‑3.STR`, `CRUISES1‑3.STR`, `SPORTA1‑3.STR`, `SPORTB1‑3.STR`, `SPORTS1‑3.STR` (21 files).

Sizes in use: 256×112 (39 files), 256×128 (3), 304×144 (1), 336×32 (1), 224×32 (1),
320×192 (9 `.WVE`), 320×224 (2 `.WVE`), 384×240 (2 `.TCM`).

---

## 7. Open questions

* `MDEC` chunk +0x0C: proven to be a frame index in `.WVE`; in `.STR`/`.TCM` it is always 0, so
  its meaning there is unconfirmed (could still be a frame index that nothing sets).
* `.STR` sequences: whether a given file is a film, a sprite sheet or a set of alternative
  screens is not encoded in the file — it is decided by the caller. Not yet traced.
* The container supports `MDC2`, `ad10/ad11/ad20/ad21`, `Ad10/Ad11` chunk tags that this disc
  never uses; their layout is unknown.
* Chunk padding: `.STR` chunk sizes are multiples of 16 and leave up to ~33 bytes of zero padding
  after the DC sentinel. The exact padding rule the *encoder* used is not pinned down (the
  decoder does not care).
* `DATA\WARNING.QTI` (23 136 bytes) starts `30 FB 02 80`, uses all 256 byte values, contains no
  `0x3800` and no chunk structure — it is compressed/packed by an unidentified scheme. Not MDEC.
* `DATA\FE\FEMISC.PSH`, `DATA\FRONTEND.VUK` (61 660 bytes; `u32 2; u32 0x7F0F0F0F; u32 0;
  u32 0xEFA0;` then a 0x10-stride offset table from 0x4C) — not investigated here.
