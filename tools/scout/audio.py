#!/usr/bin/env python3
r"""Scout probe: Road Rash Jailbreak (USA, SLUS_01053) audio containers.

An independent Python decoder, not product code: the acceptance run compares rraudio's C++ decoder with it
sample for sample (docs\formats\audio.md).  Three audio families, all of them
PS1 SPU-ADPCM (same 4-bit nibbles, same 5 predictor filters, same shift):

  * DATA\FE\*.WVE   "au00"/"au01" chunks inside the EA VLC0 stream container.
                    15-byte blocks (flags byte omitted), PLANAR stereo inside
                    each chunk, 22050 Hz.
  * DATA\*.ALB      raw SPU-ADPCM, ordinary 16-byte blocks, stereo interleaved
                    in 8 KiB halves of a 16 KiB unit.
  * DATA\AUDTAUNT.STR  rider taunt speech: 150 fixed 16 KiB records, each a
                    0x70-byte header + mono SPU-ADPCM (16-byte blocks).

Sub-commands
    info   <file>...              header / chunk / block report
    scan   <dir|file>...          inventory rows (+ --deep for a silence map)
    decode <file>... -o <dir>     -> 16-bit PCM WAV

Everything is written with struct only.
"""

import argparse
import os
import struct

# --------------------------------------------------------------------------
# SPU-ADPCM
# --------------------------------------------------------------------------

# The five PS1 SPU predictor filters (f0, f1), applied as
#   s = (nibble << 12 >> shift) + (prev1*f0 + prev2*f1 + 32) / 64
SPU_F0 = (0, 60, 115, 98, 122)
SPU_F1 = (0, 0, -52, -55, -60)

SAMPLES_PER_BLOCK = 28
WVE_BLOCK = 15          # shift/filter byte + 14 data bytes, no flags byte
SPU_BLOCK = 16          # shift/filter byte + flags byte + 14 data bytes

DEFAULT_RATE = 22050    # proven by the .WVE frame/sample ratio, see docs


class AdpcmState(object):
    __slots__ = ("h1", "h2", "clipped", "bad_filter", "bad_shift")

    def __init__(self):
        self.h1 = 0
        self.h2 = 0
        self.clipped = 0
        self.bad_filter = 0
        self.bad_shift = 0


def decode_blocks(buf, block, hdrlen, state=None, out=None):
    """Decode a run of ADPCM blocks into a list of int16."""
    st = state or AdpcmState()
    if out is None:
        out = []
    h1, h2 = st.h1, st.h2
    for i in range(0, len(buf) - block + 1, block):
        hb = buf[i]
        shift = hb & 0x0F
        filt = hb >> 4
        if filt > 4:
            st.bad_filter += 1
            filt = 0
        if shift > 12:
            st.bad_shift += 1
            shift = 9
        f0, f1 = SPU_F0[filt], SPU_F1[filt]
        for by in buf[i + hdrlen:i + block]:
            for nib in (by & 0x0F, by >> 4):
                s = nib - 16 if nib & 8 else nib
                v = ((s << 12) >> shift) + ((h1 * f0 + h2 * f1 + 32) >> 6)
                if v > 32767:
                    v = 32767
                    st.clipped += 1
                elif v < -32768:
                    v = -32768
                    st.clipped += 1
                out.append(v)
                h2 = h1
                h1 = v
    st.h1, st.h2 = h1, h2
    return out, st


# --------------------------------------------------------------------------
# container layouts
# --------------------------------------------------------------------------

def walk_chunks(data):
    off, n = 0, len(data)
    while off + 8 <= n:
        tag = data[off:off + 4]
        size = struct.unpack_from(">I", data, off + 4)[0]
        if size < 8 or off + size > n:
            raise ValueError("bad chunk at 0x%X: tag=%r size=%d" % (off, tag, size))
        yield off, tag, size
        off += size


def wve_audio_chunks(data):
    """Yield dict(off, tag, size, firstSample, payload) for au00/au01."""
    for off, tag, size in walk_chunks(data):
        if tag[:2] == b"au":
            yield dict(off=off, tag=tag, size=size,
                       first=struct.unpack_from(">I", data, off + 8)[0],
                       tail=data[off + 12:off + 16],
                       payload=data[off + 16:off + size])


def wve_decode(data):
    """Decode every au chunk of a .WVE into (left, right).

    Each chunk payload holds 2*N blocks of 15 bytes: N for the left channel
    then N for the right.  Any remainder (0 or 2 bytes) is chunk padding to a
    4-byte boundary.
    """
    left, right = [], []
    sl, sr = AdpcmState(), AdpcmState()
    nchunks = 0
    expected = None
    mismatch = 0
    for c in wve_audio_chunks(data):
        pay = c["payload"]
        nb = len(pay) // WVE_BLOCK
        half = nb // 2
        if expected is not None and c["first"] != expected:
            mismatch += 1
        expected = c["first"] + half * SAMPLES_PER_BLOCK
        decode_blocks(pay[:half * WVE_BLOCK], WVE_BLOCK, 1, sl, left)
        decode_blocks(pay[half * WVE_BLOCK:2 * half * WVE_BLOCK],
                      WVE_BLOCK, 1, sr, right)
        nchunks += 1
    return left, right, dict(chunks=nchunks, counter_mismatch=mismatch,
                             state_l=sl, state_r=sr)


ALB_HALF = 8192                     # per-channel run
ALB_UNIT = ALB_HALF * 2             # 16 KiB stereo unit


def alb_decode(data, start_unit=0, units=None):
    """Decode .ALB stereo: 8 KiB of left, then 8 KiB of right, repeated."""
    total = len(data) // ALB_UNIT
    if units is None:
        units = total - start_unit
    units = max(0, min(units, total - start_unit))
    left, right = [], []
    sl, sr = AdpcmState(), AdpcmState()
    for u in range(start_unit, start_unit + units):
        o = u * ALB_UNIT
        decode_blocks(data[o:o + ALB_HALF], SPU_BLOCK, 2, sl, left)
        decode_blocks(data[o + ALB_HALF:o + ALB_UNIT], SPU_BLOCK, 2, sr, right)
    return left, right, dict(units=units, state_l=sl, state_r=sr)


TAUNT_REC = 0x4000
TAUNT_HDR = 0x70


def taunt_records(data):
    """Yield dict(index, off, sound_id, voice_id, length, data) per record.

    Record header (0x70 bytes), fields proven constant/varying over all 150
    records of the USA disc:
      +0x00 u8   sound id     (cycles through 3,6,4,32,33,22,7,...)
      +0x01 u8   voice id     (0x81..0x9B, one group of ids per speaker)
      +0x02 u16  0xA000       constant
      +0x08..0x1F             constant over every record (unknown)
      +0x20 u32  = the +0x00 sound id
      +0x24 u32  second id (sometimes differs from +0x20)
      +0x28 u32  2
      +0x2C u32  0x7F020202   (0x7F010101 in one record)  unknown
      +0x34 u16  0x3FC0       looks like an SPU volume
      +0x38 u32  24 / 20      unknown
      +0x3C u32  40           unknown
      +0x40 / +0x50  12-byte blocks "01 7F 40 00 7F 7F FF 80 C0 1F 00 04",
                     only 3 distinct values disc-wide -- unknown, probably
                     SPU voice / ADSR presets
      +0x5C u32  ADPCM byte length
      +0x70      SPU-ADPCM, 16-byte blocks, mono
    """
    for i in range(len(data) // TAUNT_REC):
        o = i * TAUNT_REC
        ln = struct.unpack_from("<I", data, o + 0x5C)[0]
        yield dict(index=i, off=o, sound_id=data[o], voice_id=data[o + 1],
                   id20=struct.unpack_from("<I", data, o + 0x20)[0],
                   id24=struct.unpack_from("<I", data, o + 0x24)[0],
                   length=ln, data=data[o + TAUNT_HDR:o + TAUNT_HDR + ln])


def spu_samples(data):
    """Split a raw 16-byte-block SPU bank at loop-end flags (bit 0)."""
    out = []
    start = 0
    for i in range(0, len(data) - SPU_BLOCK + 1, SPU_BLOCK):
        if data[i + 1] & 1:
            out.append((start, i + SPU_BLOCK - start))
            start = i + SPU_BLOCK
    if start < len(data):
        out.append((start, len(data) - start))
    return out


# --------------------------------------------------------------------------
# measurements used as proof
# --------------------------------------------------------------------------

def envelope(x, win):
    out = []
    for i in range(0, len(x) - win + 1, win):
        s = 0
        for v in x[i:i + win]:
            s += v * v
        out.append((s / win) ** 0.5)
    return out


def measure(x, rate):
    n = len(x)
    if n == 0:
        return dict(n=0)
    dc = sum(x) / n
    rms = (sum(v * v for v in x) / n) ** 0.5
    peak = max(abs(v) for v in x)
    zc = sum(1 for a, b in zip(x, x[1:]) if (a < 0) != (b < 0))
    env = envelope(x, max(1, rate // 10))
    silent = sum(1 for e in env if e < 40)
    # seam test: mean |delta| across ADPCM block boundaries vs inside them
    p = SAMPLES_PER_BLOCK
    seam = [abs(x[i] - x[i - 1]) for i in range(p, n, p)]
    inter = [abs(x[i] - x[i - 1]) for i in range(1, n) if i % p]
    ratio = (sum(seam) / len(seam)) / (sum(inter) / len(inter) + 1e-9) \
        if seam and inter else 0.0
    return dict(n=n, secs=n / float(rate), dc=dc, rms=rms, peak=peak,
                zcr=zc / float(n), zc_hz=zc / 2.0 / (n / float(rate)),
                env_min=min(env) if env else 0, env_max=max(env) if env else 0,
                silent_frac=silent / float(len(env)) if env else 0,
                seam_ratio=ratio)


def fmt_measure(m, label):
    if not m.get("n"):
        return "%s: empty" % label
    return ("%-6s %8.2fs n=%-9d dc=%+7.1f rms=%7.1f peak=%6d "
            "zc=%6.0fHz env=%.0f..%.0f silent=%.0f%% seam/interior=%.3f"
            % (label, m["secs"], m["n"], m["dc"], m["rms"], m["peak"],
               m["zc_hz"], m["env_min"], m["env_max"],
               m["silent_frac"] * 100, m["seam_ratio"]))


# --------------------------------------------------------------------------
# WAV
# --------------------------------------------------------------------------

def write_wav(path, channels, rate):
    n = min(len(c) for c in channels)
    nch = len(channels)
    if nch == 1:
        pcm = struct.pack("<%dh" % n, *channels[0][:n])
    else:
        inter = []
        for i in range(n):
            for c in channels:
                inter.append(c[i])
        pcm = struct.pack("<%dh" % (n * nch), *inter)
    byte_rate = rate * nch * 2
    hdr = (b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " +
           struct.pack("<IHHIIHH", 16, 1, nch, rate, byte_rate, nch * 2, 16) +
           b"data" + struct.pack("<I", len(pcm)))
    with open(path, "wb") as f:
        f.write(hdr)
        f.write(pcm)
    return n


# --------------------------------------------------------------------------
# family detection
# --------------------------------------------------------------------------

def family(path, data):
    if data[:4] == b"VLC0":
        return "wve"
    if path.upper().endswith(".ALB"):
        return "alb"
    if os.path.basename(path).upper() == "AUDTAUNT.STR":
        return "taunt"
    return "unknown"


def load(path):
    with open(path, "rb") as f:
        return f.read()


def block_stats(data, block, hdrlen, start=0):
    sh = [0] * 16
    fl = [0] * 16
    fg = {}
    for i in range(start, len(data) - block + 1, block):
        sh[data[i] & 15] += 1
        fl[data[i] >> 4] += 1
        if hdrlen == 2:
            fg[data[i + 1]] = fg.get(data[i + 1], 0) + 1
    return sh, fl, fg


# --------------------------------------------------------------------------
# commands
# --------------------------------------------------------------------------

def cmd_info(args):
    for path in args.files:
        data = load(path)
        fam = family(path, data)
        print("=== %s (%d bytes) family=%s ===" % (path, len(data), fam))
        if fam == "wve":
            chunks = list(wve_audio_chunks(data))
            print("  %d audio chunks" % len(chunks))
            prev = None
            for c in chunks[:args.limit]:
                nb = len(c["payload"]) // WVE_BLOCK
                delta = "" if prev is None else " (+%d)" % (c["first"] - prev)
                print("  0x%08X %s size=%-6d payload=%-6d blocks=%-4d "
                      "half=%-4d rem=%d firstSample=%-9d%s tail=%s"
                      % (c["off"], c["tag"].decode(), c["size"],
                         len(c["payload"]), nb, nb // 2,
                         len(c["payload"]) - nb * WVE_BLOCK, c["first"], delta,
                         c["tail"].hex(" ")))
                prev = c["first"]
            if len(chunks) > args.limit:
                print("  ... %d more" % (len(chunks) - args.limit))
            sh = [0] * 16
            fl = [0] * 16
            for c in chunks:                      # per chunk: the 0/2 padding
                nb = len(c["payload"]) // WVE_BLOCK   # bytes must not shift the
                for i in range(nb):                   # block grid
                    b = c["payload"][i * WVE_BLOCK]
                    sh[b & 15] += 1
                    fl[b >> 4] += 1
            print("  shift nibble histogram : %s" % sh)
            print("  filter nibble histogram: %s  (SPU has 5 filters, 0..4)" % fl)
        elif fam == "alb":
            print("  %d bytes = %d x 16 KiB stereo units = %d ADPCM blocks"
                  % (len(data), len(data) // ALB_UNIT, len(data) // SPU_BLOCK))
            sh, fl, fg = block_stats(data[:4 << 20], SPU_BLOCK, 2)
            print("  shift nibble histogram : %s" % sh)
            print("  filter nibble histogram: %s" % fl)
            print("  flag byte values       : %s"
                  % sorted(fg.items(), key=lambda kv: -kv[1])[:8])
            ends = [o for o in range(0, len(data) - 16, 16) if data[o + 1] & 1]
            print("  loop-end flagged blocks: %d %s"
                  % (len(ends), ends[:8] if ends else ""))
        elif fam == "taunt":
            recs = list(taunt_records(data))
            print("  %d records of 0x%X bytes" % (len(recs), TAUNT_REC))
            for r in recs[:args.limit]:
                print("    #%-4d @0x%06X sound=%-3d voice=0x%02X id20=%-3d "
                      "id24=%-3d adpcm=%-6d blocks=%d"
                      % (r["index"], r["off"], r["sound_id"], r["voice_id"],
                         r["id20"], r["id24"], r["length"],
                         r["length"] // SPU_BLOCK))
            print("    sound ids : %s"
                  % sorted({r["sound_id"] for r in recs}))
            print("    voice ids : 0x%02X..0x%02X (%d distinct)"
                  % (min(r["voice_id"] for r in recs),
                     max(r["voice_id"] for r in recs),
                     len({r["voice_id"] for r in recs})))
            body = b"".join(r["data"] for r in recs)
            sh, fl, fg = block_stats(body, SPU_BLOCK, 2)
            print("  shift nibble histogram : %s" % sh)
            print("  filter nibble histogram: %s" % fl)
            print("  flag byte values       : %s"
                  % sorted(fg.items(), key=lambda kv: -kv[1])[:8])
        else:
            print("  unrecognised; first 64 bytes: %s" % data[:64].hex(" "))


def cmd_scan(args):
    files = []
    for p in args.paths:
        if os.path.isdir(p):
            for n in sorted(os.listdir(p)):
                if n.upper().endswith((".WVE", ".ALB")) or n.upper() == "AUDTAUNT.STR":
                    files.append(os.path.join(p, n))
        else:
            files.append(p)
    print("%-14s %10s %-4s %9s %10s %s"
          % ("file", "bytes", "fam", "seconds", "samples/ch", "notes"))
    for path in files:
        data = load(path)
        fam = family(path, data)
        name = os.path.basename(path)
        if fam == "wve":
            chunks = list(wve_audio_chunks(data))
            nsamp = sum(len(c["payload"]) // WVE_BLOCK // 2 for c in chunks) \
                * SAMPLES_PER_BLOCK
            last = chunks[-1]
            counter_end = last["first"] + \
                len(last["payload"]) // WVE_BLOCK // 2 * SAMPLES_PER_BLOCK
            nframes = sum(1 for _, t, _ in walk_chunks(data) if t == b"MDEC")
            note = "%d au chunks, %d video frames" % (len(chunks), nframes)
            if nframes:
                note += ", %.3f samples/frame" % (nsamp / float(nframes))
            note += ", counter %s" % ("matches" if counter_end == nsamp
                                      else "DIFFERS (%d)" % counter_end)
            print("%-14s %10d %-4s %9.2f %10d %s"
                  % (name, len(data), "wve", nsamp / float(args.rate), nsamp, note))
        elif fam == "alb":
            nsamp = len(data) // ALB_UNIT * (ALB_HALF // SPU_BLOCK) * SAMPLES_PER_BLOCK
            ends = sum(1 for o in range(0, len(data) - 16, 16) if data[o + 1] & 1)
            print("%-14s %10d %-4s %9.2f %10d %d units, %d loop-end blocks"
                  % (name, len(data), "alb", nsamp / float(args.rate), nsamp,
                     len(data) // ALB_UNIT, ends))
        elif fam == "taunt":
            recs = list(taunt_records(data))
            nsamp = sum(r["length"] for r in recs) // SPU_BLOCK * SAMPLES_PER_BLOCK
            print("%-14s %10d %-4s %9.2f %10d %d records, %d sounds x %d voices"
                  % (name, len(data), "tnt", nsamp / float(args.rate), nsamp,
                     len(recs), len({r["sound_id"] for r in recs}),
                     len({r["voice_id"] for r in recs})))
        else:
            print("%-14s %10d %-4s %9s %10s unrecognised"
                  % (name, len(data), "?", "-", "-"))


def cmd_decode(args):
    os.makedirs(args.out, exist_ok=True)
    for path in args.files:
        data = load(path)
        fam = family(path, data)
        base = os.path.splitext(os.path.basename(path))[0]
        if fam == "wve":
            left, right, meta = wve_decode(data)
            out = os.path.join(args.out, "%s.wav" % base)
            n = write_wav(out, [left, right], args.rate)
            print("%s  stereo %d Hz  %d frames  %.2f s  chunks=%d counterMismatch=%d"
                  % (out, args.rate, n, n / float(args.rate), meta["chunks"],
                     meta["counter_mismatch"]))
            print("   " + fmt_measure(measure(left, args.rate), "left"))
            print("   " + fmt_measure(measure(right, args.rate), "right"))
            print("   clipped L=%d R=%d  badFilter L=%d R=%d"
                  % (meta["state_l"].clipped, meta["state_r"].clipped,
                     meta["state_l"].bad_filter, meta["state_r"].bad_filter))
        elif fam == "alb":
            left, right, meta = alb_decode(data, args.start, args.units)
            out = os.path.join(args.out, "%s_u%05d_n%d.wav"
                               % (base, args.start, meta["units"]))
            n = write_wav(out, [left, right], args.rate)
            print("%s  stereo %d Hz  units %d..%d  %.2f s"
                  % (out, args.rate, args.start,
                     args.start + meta["units"] - 1, n / float(args.rate)))
            print("   " + fmt_measure(measure(left, args.rate), "left"))
            print("   " + fmt_measure(measure(right, args.rate), "right"))
            print("   clipped L=%d R=%d  badFilter L=%d R=%d"
                  % (meta["state_l"].clipped, meta["state_r"].clipped,
                     meta["state_l"].bad_filter, meta["state_r"].bad_filter))
        elif fam == "taunt":
            recs = list(taunt_records(data))
            sel = recs[args.start:args.start + (args.units or len(recs))]
            for r in sel:
                if not r["length"]:
                    continue
                pcm, st = decode_blocks(r["data"], SPU_BLOCK, 2)
                out = os.path.join(args.out, "%s_%03d_s%02d_v%02X.wav"
                                   % (base, r["index"], r["sound_id"],
                                      r["voice_id"]))
                n = write_wav(out, [pcm], args.rate)
                print("%s  mono %d Hz  %.3f s  %s  clipped=%d badFilter=%d"
                      % (out, args.rate, n / float(args.rate),
                         fmt_measure(measure(pcm, args.rate), "mono"),
                         st.clipped, st.bad_filter))
        else:
            print("%s: unrecognised family" % path)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rate", type=int, default=DEFAULT_RATE)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("info")
    p.add_argument("files", nargs="+")
    p.add_argument("--limit", type=int, default=12)
    p.set_defaults(func=cmd_info)

    p = sub.add_parser("scan")
    p.add_argument("paths", nargs="+")
    p.set_defaults(func=cmd_scan)

    p = sub.add_parser("decode")
    p.add_argument("files", nargs="+")
    p.add_argument("-o", "--out", required=True)
    p.add_argument("--start", type=int, default=0,
                   help=".ALB: first 16 KiB unit; AUDTAUNT: first run index")
    p.add_argument("--units", type=int, default=None,
                   help=".ALB: number of 16 KiB units; AUDTAUNT: number of runs")
    p.add_argument("--join", action="store_true",
                   help="AUDTAUNT: concatenate the selected runs into one WAV")
    p.set_defaults(func=cmd_decode)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
