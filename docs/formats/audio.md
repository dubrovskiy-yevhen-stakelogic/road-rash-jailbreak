# Road Rash: Jailbreak (USA, SLUS_01053) — audio containers

Derived from the disc bytes (extract in `work\disc_us`) and from our own disassembly of `SLUS_010.53`
(SHA-1 `67ed165a2c517d4e6106fb0dfa324d66dd9a76f1`, file `0x800` = vaddr `0x80010000`) and of `RASHCDF.BIN`
(SHA-1 `a3fec4b4e9292c358d0f6dc529843f5d8f25924a`, file `0` = vaddr `0x8005B5E8`), plus the four live machine
states in `work\oracle\state\*`. Authoritative parser: `src\rrformats\audio.{h,cpp}`; CLI `tools\rraudio`
(`scan` / `list` / `decode` / `tracks`).

**All game audio on this disc is PS1 SPU-ADPCM.**

*No XA-ADPCM.* A full sweep of all 249 159 raw sectors of the disc image (MODE2/2352) gives: every
sector is MODE2; the subheader coding byte is 0x08 (data) on 248 509 of them, 0x89 (data + EOR + EOF)
on 495, 0x09 on 1, and 0x20 (FORM2) on 154. **Not one sector has the audio bit (0x04) set**, so there
is no CD-XA ADPCM stream on the disc at all — all audio comes out of ordinary FORM1 data files.

*Not EA-XA either.* EA's own "EA-XA" 4-bit ADPCM uses 4 predictor pairs; the predictor index in
these files takes 5 values (0…4), which is the PS1 SPU filter set.

| Family | Files | Block | Channels | Rate |
|---|---|---|---|---|
| `au00`/`au01` chunks in `DATA\FE\*.WVE` | 11 | **15** bytes (no flags byte) | stereo, planar per chunk | 22050 Hz |
| `DATA\*.ALB` | 4 | 16 bytes (standard) | stereo, 8 KiB planar interleave | 15999.17 Hz (nominal 16000) |
| `DATA\AUDTAUNT.STR` | 1 | 16 bytes (standard) | mono | 11025 Hz (7999.58 Hz on records 141, 142) |

---

## 1. The codec

Identical nibble format in all three families:

```
block header byte:  bits 3..0 = shift (0..12)
                    bits 7..4 = filter index (0..4)
[flags byte]        only in the 16-byte form; standard SPU bits:
                    bit0 loop end, bit1 sustain/repeat, bit2 loop start
14 data bytes    -> 28 samples, low nibble of each byte first

s   = sign_extend_4(nibble)
out = ((s << 12) >> shift) + (prev1*f0 + prev2*f1 + 32) / 64      (clamped to int16)

filter  0      1      2      3      4
f0      0     60    115     98    122
f1      0      0    -52    -55    -60
```

### 1.1 Evidence for SPU-ADPCM

1. **Nibble ranges.** Over whole files the high nibble of the header byte only ever takes 0…4 and
   the low nibble only 0…12 — exactly the SPU's 5 filters and 13 shifts. EA-XA ADPCM would give
   0…3.
2. **Silence.** The tail of `EA_LOGO.WVE`'s last audio chunk is `0C 00×14` repeated with a period
   of **15** bytes. `0x0C` = shift 12, filter 0, i.e. the minimum-amplitude SPU header, with an
   all-zero payload. That pins both the block length (15) and the header-byte semantics.
   `AUDTAUNT.STR` samples end with the classic SPU terminator block `00 07 77 77 …`.
3. **Continuity.** Decoding with the SPU filters leaves no discontinuity at block boundaries: the
   mean |Δ| across the 28-sample seams divided by the mean |Δ| inside blocks is **0.96…1.03** on
   every file tested. A wrong filter set or a wrong channel layout pushes this to 1.6+ (measured).
4. **Signal sanity.** Zero clipped samples, DC offset within ±10 LSB on the `.WVE` streams, peak
   30 700…32 700 of 32 767 (mastered with a little headroom), plausible zero-crossing rates
   (360 Hz for the EA logo chord, 1.5 kHz for cutscene speech+music, 2.4 kHz for speech).

### 1.2 Sample rates — read off the SPU pitch register

The SPU pitch register (`0x1F801C00 + voice*0x10 + 4`, written at EXE `0x80051C8C`, file `0x4248C`)
is `0x1000` for 44100 Hz and linear in between, so `rate = pitch * 44100 / 4096`.

| family | pitch | rate | where the pitch comes from |
|---|---|---|---|
| `.WVE` `au00`/`au01` | `0x0800` | **22050.00 Hz** | `li v0,2048` at `RASHCDF.BIN` file `0x126A0` and `0x149B8`, passed down to `SpuSetVoiceAttr` |
| `.ALB` (all four) | `0x05CE` | **15999.17 Hz** (nominal 16000) | `li a1,16000` at EXE file `0x14F00`, converted by `pitch = rate*4096/44100` at EXE file `0x1150C`; the live value `0x5CE` sits at RAM `0x800D7550` in all four oracle states; also hardcoded `li v0,1486` at `RASHCDF.BIN` file `0x23868` for `FEALBUM.ALB` |
| `AUDTAUNT.STR` | `0x0400` (148 records) / `0x02E7` (records 141, 142) | **11025.00 Hz** / **7999.58 Hz** | the record's own descriptor, see §4.1 |

---

## 2. `.WVE` — `au00` / `au01` chunks

Container and chunking are documented in `video.md` §1. Audio chunk:

| off | type | meaning | evidence |
|---|---|---|---|
| +0x00 | `char[4]` | `"au00"`, or `"au01"` for the last one in the file | |
| +0x04 | `u32be` | chunk size incl. header | chain closes on file size |
| +0x08 | `u32be` | **sample index of this chunk's first sample, per channel** | the delta to the next chunk equals `blocks/2 × 28` for every chunk of every file |
| +0x0C | `u8[4]` | `08 00 02 00` on all 1339 audio chunks — **never read by the game** | see below |
| +0x10 | | payload | |

Field +0x0C: the `au00`/`au01` handler at `RASHCDF.BIN` `0x80060780` (file `0x5198`) touches only
`+0x04` (size) and `+0x10` (payload); a sweep of the whole dispatcher region for `lw|lh|lhu <r>,12(<r>)`
returns zero hits. Read as `u32le` the bytes are `0x00020008` (the rest of the chunk header is
big-endian; as `u32be` the value is `0x08000200`). Read as `u16be` they are `(0x0800, 0x0200)`, and
`0x0800` is numerically the 22050 Hz pitch — but since nothing reads the field, that is a coincidence,
not a meaning. The `.WVE` pitch is the hardcoded `0x0800` of §1.2.

Payload = `2·N` blocks of 15 bytes: **N blocks of left, then N blocks of right** (planar inside the
chunk), plus 0 or 2 bytes of padding so the chunk size is a multiple of 4.

Layout proof: correlation between the two halves is **+0.90…+0.99** (genuine stereo pair), while
treating even/odd blocks as the two channels gives **−0.12**. Predictor state must be carried
across chunks per channel; doing so keeps the seam ratio at 1.00.

N is 210 in most chunks and 211 in roughly every fifth one (payload 6300 vs 6332 bytes),
i.e. 5880 or 5908 samples per channel; the 6332-byte payload is the one that carries the 2 bytes
of tail padding.

### 2.1 Why 22050 Hz and 15 fps

A `.WVE` interleaves one audio chunk per four `MDEC` frames. Over each file,
`total audio samples / total video frames` = **1471.5 ± 0.03** and the steady-state chunk delta is
exactly **5880 = 4 × 1470** samples per channel. 1470 samples/frame with an NTSC PS1 (60 Hz vsync)
means one video frame every 4 vsyncs — 15 fps — and 15 × 1470 = **22050 Hz** exactly, which the
pitch register value of §1.2 confirms.
The alternative pairing (30 fps / 44100 Hz) is ruled out by the container bitrate: `JAILBRAK.WVE`
would then need 405 KB/s from a 300 KB/s double-speed drive.

### 2.2 Inventory

| file | audio chunks | samples / channel | duration @22050 | video frames |
|---|---:|---:|---:|---:|
| `EA_LOGO.WVE` | 19 | 110 376 | 5.01 s | 75 |
| `CREDITS.WVE` | 67 | 391 412 | 17.75 s | 266 |
| `GAUNTLET.WVE` | 89 | 523 852 | 23.76 s | 356 |
| `MOVINGUP.WVE` | 111 | 651 868 | 29.56 s | 443 |
| `BUSTED.WVE` | 123 | 722 512 | 32.77 s | 491 |
| `SPAZPUNT.WVE` | 131 | 768 124 | 34.84 s | 522 |
| `THESETUP.WVE` | 133 | 781 368 | 35.44 s | 531 |
| `GANGS.WVE` | 134 | 788 732 | 35.77 s | 536 |
| `INTRO.WVE` | 158 | 929 992 | 42.18 s | 632 |
| `GOT_OINK.WVE` | 182 | 1 071 252 | 48.58 s | 728 |
| `JAILBRAK.WVE` | 192 | 1 128 624 | 51.18 s | 767 |

Audio and video lengths agree to within the two lead-in audio chunks (audio runs ~0.27 s ahead),
which is what the `au00, au00, {4×MDEC, au00}…` interleave implies. Decoded `EA_LOGO` and `BUSTED`:
stereo, 0 clipped samples, 0 invalid filter nibbles, seam/interior 1.00–1.03.

---

## 3. `.ALB` — streamed music banks

No magic, no header, no in-file index: the whole file is SPU-ADPCM.

* Ordinary **16-byte** SPU blocks (header, flags, 14 data).
* **Stereo, planar with an 8192-byte interleave**: `[8 KiB left][8 KiB right]` repeated. Every
  `.ALB` size is an exact multiple of 16384.
* Layout proof: correlation between the two halves is +0.52…+0.85 across all four files, while a
  16-byte block interleave gives −0.07…+0.03. Independent confirmation from `FEALBUM.ALB`, whose
  loop-end-flagged blocks always occur in pairs exactly 8192 bytes apart — one end marker per
  channel at the same position inside each half. §3.3 shows where the interleave comes from.
* Rate: 15999.17 Hz (§1.2).

| file | bytes | 16 KiB units | samples / channel | duration @15999.17 | loop-end blocks |
|---|---:|---:|---:|---:|---:|
| `ALBUM.ALB` | 63 537 152 | 3878 | 55 595 008 | 3474.91 s (57.9 min) | 0 |
| `ALBUM2.ALB` | 63 537 152 | 3878 | 55 595 008 | 3474.91 s | 0 |
| `FEALBUM.ALB` | 8 257 536 | 504 | 7 225 344 | 451.61 s | 36 |
| `INTRO.ALB` | 2 359 296 | 144 | 2 064 384 | 129.03 s | 0 |

**`ALBUM.ALB` and `ALBUM2.ALB` are byte-identical** (SHA-1 `53ca568b02735d781143f965e8159665f0a81166`).
They are the same music album duplicated on the disc so the CD head can reach the music from either
end of the layout — a classic PS1 seek-time trick. The EXE's file-name table at file offset 0x13F0
lists `ALBUM.ALB`, `ALBUM2.ALB` and `DATA\INTRO.ALB` next to each other.

Decoded excerpts of `ALBUM`, `INTRO` and `FEALBUM` (12 units each): RMS 8000–13000, ≤1 clipped
sample, seam/interior 0.99–1.03.

### 3.1 The `ALBUM.ALB` track table, in the EXE

`ALBUM.ALB` and `INTRO.ALB` contain no loop-end markers, so the track table is external. It sits at
`SLUS_010.53` file offset **`0x43D78`** = vaddr `0x80053578`:

```
u32 trackCount = 18
u32 totalBytes = 0x03C98000            (= 63 537 152 = the exact size of DATA\ALBUM.ALB)
{ u32 start; u32 length; u32 flags; } [18]        at file 0x43D80
      flags bit0 = track selectable, bit1 = already played in this shuffle cycle
```

Self-proving, and checked by `ParseAlbumTrackTable` (`rraudio tracks <SLUS_010.53>` prints it): the 18
entries are contiguous from 0, every length is a whole number of 16 KiB units, and they sum to exactly
the file size. Confirmed live: in all four `work\oracle\state\*` dumps the current-track index at
`0x8005B1F0` and the stream context at `0x800CD670` (`+0x04` position, `+0x2C` end) agree with the
entry they point at. The file-name strings are reached through `$gp` data slots (`[0x8005AE68]` =
`0x80010BF0` `"ALBUM.ALB"`, `[0x8005AE6C]` = `0x80010BFC` `"ALBUM2.ALB"`), never by a `lui/addiu`
immediate, which is why an immediate scan does not find the table's users.

| n | start | length | start unit | units | length @ 15999 Hz |
|---:|---|---|---:|---:|---:|
|0|`0x00000000`|`0x003B0000`|0|236|211.47 s|
|1|`0x003B0000`|`0x00520000`|236|328|293.91 s|
|2|`0x008D0000`|`0x0026C000`|564|155|138.89 s|
|3|`0x00B3C000`|`0x00278000`|719|158|141.58 s|
|4|`0x00DB4000`|`0x00474000`|877|285|255.38 s|
|5|`0x01228000`|`0x0031C000`|1162|199|178.32 s|
|6|`0x01544000`|`0x00408000`|1361|258|231.18 s|
|7|`0x0194C000`|`0x002D0000`|1619|180|161.29 s|
|8|`0x01C1C000`|`0x00248000`|1799|146|130.82 s|
|9|`0x01E64000`|`0x0049C000`|1945|295|264.34 s|
|10|`0x02300000`|`0x003DC000`|2240|247|221.33 s|
|11|`0x026DC000`|`0x002D8000`|2487|182|163.08 s|
|12|`0x029B4000`|`0x00298000`|2669|166|148.75 s|
|13|`0x02C4C000`|`0x00200000`|2835|128|114.70 s|
|14|`0x02E4C000`|`0x00340000`|2963|208|186.38 s|
|15|`0x0318C000`|`0x0028C000`|3171|163|146.06 s|
|16|`0x03418000`|`0x00490000`|3334|292|261.65 s|
|17|`0x038A8000`|`0x003F0000`|3626|252|225.81 s|

Independent check: a per-unit energy scan (mean shift nibble > 8.5 ⇒ near silence) finds seven fully
silent gaps, at units 234, 563, 875, 2236, 2833, 3331 and 3878; every one lands on a table boundary
(236, 564, 877, 2240, 2835, 3334, 3878). The other eleven boundaries are cross-fades the silence scan
cannot see.

### 3.2 `FEALBUM.ALB` and `INTRO.ALB`

`FEALBUM.ALB` is self-delimiting: 18 sample pairs carry the SPU loop-end flag, followed by zero-filled
padding. It needs no table: `RASHCDF.BIN` bounds the index with `sltiu v0,a0,18` and computes the
offset as `n * 0x70000` (file `0x23840`…`0x238D0`); 18 x `0x70000` is exactly the file's size.
`INTRO.ALB` is streamed linearly.

### 3.3 SPU streaming layout

Two voices, one 64 KiB ring per channel in SPU RAM at `0x000434E0` and `0x000534E0`, eight chunks of
8192 bytes each, fed from two 8192-byte main-RAM staging buffers. In `work\oracle\state\rr-pack` those
buffers hold `ALBUM.ALB` at `0x02CF0000` and `0x02CF2000` — i.e. the file's 8192-byte planar
interleave maps 1:1 onto chunk<->channel, which is where that interleave comes from.

---

## 4. `DATA\AUDTAUNT.STR` — rider taunt speech

Not an `MDEC` file despite the extension. It is a flat array of **150 records of 0x4000 bytes**
(2 457 600 = 150 × 16384). Each record:

| off | type | meaning | evidence |
|---|---|---|---|
| +0x00 | `u8` | sound id — 8 distinct values `{3,4,6,7,22,23,32,33}`, cycling in a fixed order per speaker | |
| +0x01 | `u8` | voice id, `0x81`…`0x9B` (27 distinct speakers) | increments after each group of 7 records |
| +0x02 | `u16le` | `0xA000` | constant on all 150 |
| +0x04 | `u32le` | 0 | constant |
| +0x08…+0x1F | | byte-identical on all 150 records | unknown |
| +0x20 | `u32le` | sound id of the first sample (= the +0x00 sound id) | |
| +0x24 | `u32le` | sound id of the second sample; `255` when there is none | e.g. record 2 carries sounds 4 and 5 |
| +0x28 | `u32le` | 2 - the magic of the sound bank that starts here | §4.2 |
| +0x2C | `u8[3]` | **sample count** (1 or 2), stored three times | equals the number of samples found by walking the SPU flags, 150/150 |
| +0x2F | `u8` | `0x7F` | |
| +0x30 | `u32le` | the bank's SPU address, patched at load | §4.2 |
| +0x34 | `u32le` | `0x3FC0` - the bank's sample byte count | §4.2 |
| +0x38 | `u32le` | **offset of the first sound record, relative to +0x28**: 20 in a one-sample record, 24 in a two-sample one | §4.1, §4.2 |
| +0x3C | `u32le` | 40 in a two-sample record: the second sound record's offset | §4.2 |
| +0x5C | `u32le` | **body offset of the second sample**; `0` when the record holds only one | |
| +0x60…+0x6F | | zero | |
| +0x70 | | body: one or two mono SPU-ADPCM samples, 16-byte blocks | |

Each record body is **one or two complete SPU samples**, delimited exactly the way the SPU does it:
the first block carries the loop-start flag `0x04`, the last audio block carries the loop-end flag
`0x01`, and one dummy terminator block follows. All **277** terminator blocks on the disc are the
byte-identical `00 07 77 77 77 77 77 77 77 77 77 77 77 77 77 77`. The first sample ends before
`+0x5C` and the second one runs on well past it, so `+0x5C` is not a length: decoding
`[+0x70, +0x70+field)` loses the second take entirely. The 23 records with `+0x5C == 0` each hold one
full sample (ordinary speech: RMS 8900–11000, peak ~32750, seam/interior 0.98–1.02).

Counts: 150 records -> **277 samples**, 125 261 audio blocks -> **3 507 308 samples**, **319.60 s**.

### 4.1 The descriptor list, and why `C0 1F` is not the pitch

The list at `+0x28 + [+0x38]` has one 16-byte entry per sample — the 4-byte sound record header
`01 7F 40 00` (`descCount` = 1, then three bytes), then the 12-byte engine sound descriptor. Because
the list offset depends on the sample count, the descriptors sit 4 bytes further on in two-sample
records:

```
+0x00 u8   volume scale          (0x7F)
+0x01 u8   ?
+0x02 u16  ADSR1                 (0x80FF)
+0x04 u16  ADSR2                 (0x1FC0)   <- the bytes C0 1F are ADSR2, not a pitch
+0x06 u16  SPU PITCH             (0x0400 = 11025 Hz; 0x02E7 = 8000 Hz on records 141 and 142)
+0x08 u32  SPU RAM address, patched after upload
```

read by the engine at EXE `0x8001F2F4` (file `0xFAF4`: `lhu` at descriptor `+6` ->
`SpuVoiceAttr+0x14`). The model holds on **150/150** records: the count at `+0x2C` equals the number of
samples found by walking the SPU flags, and the list has exactly that many entries, all with the same
pitch.

### 4.2 The record as a sound bank

Bytes `+0x28` onward of a record are one sound bank in the engine's general bank format, the one
`LookupSound` (`SLUS 0x8001E86C`) and `PatchBank` (`SLUS 0x8001EAA4`) read:

```
+0x00 u32  magic = 2
+0x04 u8   soundCount; u8 nonEmpty; u8 nonEmpty; u8 0x7F
+0x08 u32  spuAddr         0 in the file; the upload address after loading
+0x0C u32  sampleBytes
+0x10 u32  offset[soundCount]      relative to the bank start, 0 = no such sound
then per sound: { u8 descCount; u8 ?; u8 ?; u8 ?; descriptor[descCount] }   (descriptor: 4.1)
```

`PatchBank` adds the bank's `spuAddr` to every descriptor's SPU address. All 150 records parse this
way - magic 2, count 1 or 2, first offset `0x10 + 4*count`, every sound record holding exactly one
descriptor - and the **277** descriptors match the sample count of the SPU-flag walk. The same format
is used by `DATA\RASHNZ_E.DAT`, a directory of 8 such banks (`struct { u32 offset; u32 bankBytes;
u32 sampleBytes; } entry[8]`, 96 bytes, then each bank followed by its raw SPU-ADPCM samples).

---

## 5. Reproducing

```
rraudio scan   work\disc_us\DATA work\disc_us\DATA\FE
rraudio list   work\disc_us\DATA\FE\EA_LOGO.WVE
rraudio tracks work\disc_us\SLUS_010.53
rraudio decode work\disc_us\DATA\FE\EA_LOGO.WVE   <outdir>
rraudio decode work\disc_us\DATA\ALBUM.ALB        <outdir> --start 1200 --units 12
rraudio decode work\disc_us\DATA\AUDTAUNT.STR     <outdir> --all
```

### 5.1 Cross-check of the C++ decoder against an independent decoder

`src\rrformats\audio.cpp` was checked against an independent Python implementation of the same
decoder, both decoded to WAV and diffed sample by sample:

| family | files | samples compared | differing |
|---|---:|---:|---:|
| `.WVE` (`EA_LOGO`, `BUSTED`, `CREDITS`, whole files) | 3 | 2 448 600 | **0** |
| `.ALB` (`INTRO` all, `FEALBUM` all, `ALBUM` units 1200–1263) | 3 | 20 414 464 | **0** |
| `AUDTAUNT.STR` (one span per record, first-sample reading) | 127 | 1 624 980 | **0** |
| **total** | **133** | **24 488 044** | **0** |

The AUDTAUNT rows use `rraudio decode --probe-compat`, which reproduces the single-span reading of
`+0x5C` for the comparison; the default output is the two-sample reading of §4 — 277 WAVs instead of
127.

---

## 6. Open questions

* **`au01` vs `au00`.** `au01` appears exactly once, as the last audio chunk of each `.WVE`; the
  chunk body is identical in layout. Presumably an end-of-stream marker. The dispatcher in
  `RASHCDF.BIN` treats them as separate cases, which has not been traced further.
* The `.WVE` container also accepts `ad10/ad11/ad20/ad21/Ad10/Ad11` audio tags that this disc
  never uses.
* `DATA\ANIMNOIZ.DAT` (magic `ARec`, 3080 bytes): `char[4] "ARec"; u32 0x87C (payload size);
  u32 13;` then 12-byte records `{u32 packed; u32 0; u32 count}` with `packed` values such as
  `0x01000057`, `0x09070050`; a second table of `u32` offsets into the 0x87C payload follows at
  0x8A0. Full layout **unknown**; it is a sound-trigger table for animations, not audio data.

---

## 7. Related string tables (not audio)

`DATA\GAMESTRG.LOC` and `DATA\FE\FESTRING.LOC`, magic `LOCH`:

```
LOCH header (20 bytes):
  +0x00 char[4] "LOCH"
  +0x04 u32     header size = 20
  +0x08 u32     0
  +0x0C u32     number of language chunks = 1
  +0x10 u32     offset of the first LOCL chunk = 20

LOCL chunk:
  +0x00 char[4] "LOCL"
  +0x04 u32     chunk size, header included (20 + size == file size)
  +0x08 u32     0
  +0x0C u32     string count
  +0x10 u32[count]  offsets, relative to the LOCL chunk start
  ...           NUL-terminated strings, in offset order
```

| file | bytes | LOCL chunks (= languages) | strings | string length min/max/avg |
|---|---:|---:|---:|---|
| `DATA\GAMESTRG.LOC` | 3 292 | 1 | 181 | 4 / 43 / 14.0 |
| `DATA\FE\FESTRING.LOC` | 89 232 | 1 | 1 945 | 1 / 237 / 41.9 |

Both are verified: offsets strictly increasing, all inside the chunk, every string NUL-terminated,
first offset exactly at the end of the offset table. The USA build ships **one** language;
`GAMESTRG.LOC` is pure 7-bit ASCII, `FESTRING.LOC` contains 98 bytes ≥ 0x80 in ~82 KB of text
(most likely formatting/glyph codes). No string content is reproduced in this repository.
