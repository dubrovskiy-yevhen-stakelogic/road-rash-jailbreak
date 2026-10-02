#pragma once
// The sound emitter of Road Rash: Jailbreak, ported function by function from the original MIPS
// code of the resident executable `SLUS_010.53`, SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1,
// text base 0x80010000 (file offset 0x800). The bench that proves each function bit-exact is
// `rrverify phys`.
//
// These are the ten functions the whole race speaks to the sound system through:
//
//   PlaySound3D 0x80017BA0 -> Sound3DParams 0x80019E40 -> RatAtan2 0x80020018, FixDiv 0x80010028
//                          `-> StartVoice   0x8001F174 -> LookupSound 0x8001E86C
//                                                      -> AllocVoice  0x8001F9C4 -> KeyOff 0x8001EB44
//                                                      -> KeyOn       0x8001EB28
//   SetListener 0x80016768, SurfaceSound 0x80017B30, QueueListenerSound 0x80017B6C
//
// House rules this file follows, the same ones `bike.h`, `ai.h` and `race.h` follow:
//   * every pointer chase is the CALLER's, so a port never follows a guest pointer out of a byte
//     view. Everything the original reaches through a global arrives here
//     already resolved, in an `*Env` struct, as an address plus a byte view of what is behind it;
//   * a callee that is not ported is asked for through an interface, never stubbed;
//   * a case the port cannot serve - a bank offset outside the window the caller handed over -
//     is reported, not guessed.
//
// What this file does NOT contain, deliberately: anything below `ProgramVoice` / `UpdateVoice`.
// `SpuSetVoiceAttr SLUS 0x80051C38`, `SpuSetKey SLUS 0x80050D08` and the rest of Sony's `libspu`
// are not ported - our mixer is not the SPU.
#include <cstdint>

namespace rr::sim {

// ---------------------------------------------------------------------------- byte views
//
// A little-endian view over one block of guest memory the CALLER has resolved and handed over.
// The same shape as `EntityView` in bike.h, under a name that fits what it addresses here. All
// offsets are byte offsets inside the block, so the port never invents an address.
class SoundBytes {
public:
    SoundBytes() = default;
    SoundBytes(uint8_t* base, uint32_t size) : b_(base), n_(size) {}

    bool valid() const { return b_ != nullptr; }
    uint32_t size() const { return n_; }
    uint8_t* bytes() const { return b_; }

    uint32_t U32(uint32_t off) const {
        return static_cast<uint32_t>(b_[off]) | (static_cast<uint32_t>(b_[off + 1]) << 8) |
               (static_cast<uint32_t>(b_[off + 2]) << 16) |
               (static_cast<uint32_t>(b_[off + 3]) << 24);
    }
    int32_t S32(uint32_t off) const { return static_cast<int32_t>(U32(off)); }
    void SetU32(uint32_t off, uint32_t v) const {
        b_[off] = static_cast<uint8_t>(v);
        b_[off + 1] = static_cast<uint8_t>(v >> 8);
        b_[off + 2] = static_cast<uint8_t>(v >> 16);
        b_[off + 3] = static_cast<uint8_t>(v >> 24);
    }

private:
    uint8_t* b_ = nullptr;
    uint32_t n_ = 0;
};

// One entry of the bank pointer table `*(0x800D6874)`, resolved by the caller: the bank's guest
// address and a READABLE window over it. `address == 0` is a null slot, which `StartVoice`
// rejects exactly as the original does.
//
// A window, not "the bank", for the same reason `bike_engine` is handed a window around the stat
// block: the sound record's offset `bank[+0x10 + 4*i]` is a u32 read out
// of game data and the original bounds it by NOTHING. When the original's own index leaves the
// window this port reports it through `SoundSystemEnv::outOfWindow` instead of reading memory it
// was not given.
struct SoundBankRef {
    uint32_t address = 0;
    const uint8_t* data = nullptr;
    uint32_t size = 0;

    bool Holds(uint32_t off, uint32_t n) const {
        return data != nullptr && off <= size && n <= size - off;
    }
    uint32_t U32(uint32_t off) const {
        return static_cast<uint32_t>(data[off]) | (static_cast<uint32_t>(data[off + 1]) << 8) |
               (static_cast<uint32_t>(data[off + 2]) << 16) |
               (static_cast<uint32_t>(data[off + 3]) << 24);
    }
    uint16_t U16(uint32_t off) const {
        return static_cast<uint16_t>(static_cast<uint32_t>(data[off]) |
                                     (static_cast<uint32_t>(data[off + 1]) << 8));
    }
};

// ---------------------------------------------------------------------------- SLUS 0x80050678
//
// The ONE callee of the emitter that this file does not port, and it is Sony's, not EA's:
// `SLUS 0x80050678` -> `0x800506A8`, which keys a voice off through the SPU register mirror
// (`sound_engine.h` ports `0x800506A8`). `StartVoice`'s `restart` argument - really `noReverb` -
// is the only thing that reaches it, and `PlaySound3D` always passes 0, so nothing the race plays
// reaches it at all.
//
// It is NOT put on the oracle seam, and the reason is measured rather than assumed: with
// `*(0x8005A408) & 1 == 0` - its value in every capture - `0x800506A8` takes the arm at
// `0x800507A0`, which does an `lhu` from `*(0x8005A41C) + 2*204 = 0x1F801D98`. That is an MMIO
// *load*, and `Cpu::spuWritesAreDropped` deliberately drops SPU stores only: every SPU load still
// traps, so an oracle-supplied call here would stop the bench rather than answer. The bench's SPU
// control register file serves it instead: row `start_voice` holds `restart` at 0, and row
// `start_voice_noreverb` runs this function with a sink that calls the port.
struct VoiceRestartSink {
    virtual ~VoiceRestartSink() = default;
    // SLUS 0x80050678(0, 1 << channel) - "key this voice off right now, before it is restarted".
    virtual void RestartVoice(int32_t channel) = 0;
};

// ---------------------------------------------------------------------------- the voice side
//
// The sound-system record at `SLUS 0x800D6870` and everything the allocator reaches through it.
// The caller hands over the whole 0x188-byte record as bytes, because every field
// the emitter touches - the three lists, the two key masks, the two pointers - belongs to it.
struct SoundSystemEnv {
    SoundBytes state;            // 0x800D6870, 0x188 bytes, WRITABLE
    // `*(0x800D687C)`, 44 bytes per voice, WRITABLE. Only the BLOCK is needed and not its guest
    // address: the one address the emitter ever stores is the descriptor's, which it builds from
    // `SoundBankRef::address`. A caller that wants a pointer out of `AllocVoice`'s index makes it
    // itself, from the same word of the record it resolved this view from.
    SoundBytes voices;
    uint32_t* serial = nullptr;  // *(gp+2068) = 0x8005B4A0, the sound-handle serial counter.
                                 // NOT the LCG seed at gp+2076.
    // The bank pointer table `*(0x800D6874)`, resolved slot by slot. It must hold
    // `state[+0x00] + 1` entries, not `state[+0x00]`, because the original's bound test is
    // `slt v0, bankCount, bank` at 0x8001F1B8 - it rejects only `bank > count`, so `bank == count`
    // reads ONE WORD PAST the `malloc(4*count)` table. Reproducing that off-by-one is
    // the whole reason this is a caller-supplied array rather than a pointer.
    const SoundBankRef* banks = nullptr;
    uint32_t bankSlots = 0;      // how many entries `banks` has
    VoiceRestartSink* restart = nullptr; // SLUS 0x80050678, only on the `restart != 0` arm
    // Set when the original's own bank offset would leave the window the caller handed over, or
    // when the caller supplied fewer bank slots than the record's own bank count asks for. A port
    // that cannot read what the original read says so; it never guesses.
    bool* outOfWindow = nullptr;
};

// The 12-byte parameter record `PlaySound3D` keeps on its stack and hands to `StartVoice`
// (0x80017C70/0x80017C7C/0x80017C74).
struct SoundParams {
    int32_t pitch = -1; // +0x00; -1 means "use the descriptor's own"
    int32_t volume = 0; // +0x04
    int32_t pan = 0;    // +0x08
};

// ---------------------------------------------------------------------------- the listener side
//
// The listener array `*(gp+1920)`, 72 bytes per player, and the two tables
// `Sound3DParams` reads out of the EXE's own data.
struct Sound3DEnv {
    // *(gp+1920), resolved. An INVALID view is the original's NULL pointer, which is a real arm of
    // the function (0x80019E88: volume 127, pan 64, return), not an error.
    SoundBytes listeners;
    const uint8_t* gameState = nullptr; // *(0x8005B2F8); +0x30 is the player count
    const int32_t* atanTable = nullptr; // SLUS 0x8005285C, RatAtan2's 18-word table
    const int16_t* sinCos = nullptr;    // SLUS 0x8005624C, {s16 sin; s16 cos}[4096]
    // Set when the listener index the caller asked for leaves the block it handed over. The
    // original bounds `p` by nothing; this port reports rather than reads what it was not given.
    bool* outOfWindow = nullptr;
};

// ---------------------------------------------------------------------------- SLUS 0x8001EB28 / 44
// void KeyOn (u32 mask) - S[+0x14] |= mask
// void KeyOff(u32 mask) - S[+0x18] |= mask
//
// Seven instructions each. They do not touch the SPU: the mask is committed later, from the VSync
// interrupt, by `SoundService SLUS 0x8001EE94`.
void SoundKeyOn(const SoundBytes& state, uint32_t mask);
void SoundKeyOff(const SoundBytes& state, uint32_t mask);

// ---------------------------------------------------------------------------- SLUS 0x8001E86C
// u8 *LookupSound(Bank *b, int i)
//
// Returns the sound record's offset INSIDE the bank, or 0 for "no such sound" - the original
// returns `b + offset[i]`, and returning the offset keeps the one address addition in the caller:
//
//   if (!(i < (s32)(u8)b[+0x04])) return 0;      // slt, SIGNED, against the u8 sound count
//   if (i < 0) return 0;
//   o = *(u32*)(b + 0x10 + 4*i);  return o;      // 0 = the bank has no sound i
//
// `ok` is cleared when the count or the offset word lies outside the window the caller handed over.
uint32_t LookupSound(const SoundBankRef& bank, int32_t index, bool& ok);

// ---------------------------------------------------------------------------- SLUS 0x8001F9C4
// Voice *AllocVoice(int reserved), 404 bytes
//
// Returns the voice INDEX, or -1 for the original's NULL. The free stack, the 25-slot steal ring
// and the 22-slot reserved table live in the record; the one word outside the record it touches is
// the serial counter at gp+2068, and that - not the LCG - is the whole of the emitter's effect on
// the simulation.
//
// Nothing ever refills the free stack, so after the first 24 allocations every one-shot sound
// STEALS the least recently started stealable voice and keys it off. A port that wants the
// original's mix has to reproduce the ring order exactly, which is why this is ported rather than
// replaced with a policy of our own.
int32_t AllocVoice(int32_t reserved, const SoundSystemEnv& env);

// ---------------------------------------------------------------------------- SLUS 0x80019E40
// void Sound3DParams(int p, s32 x, s32 z, s32 vx2, s32 vz2,
//                    s32 *outVol, s32 *outPan, s32 *outPitch, s32 shift), 640 bytes
//
// Volume is a linear octagonal-distance falloff, pan is a full 360 degrees, and the pitch is a
// Doppler ratio `PlaySound3D` never asks for (it passes `outPitch = NULL`).
//
// Read out of the branch at 0x80019F3C: the atan2 and the whole pan
// computation are on the `d > 0xFFFF` arm ONLY. Inside 1.0 world unit the function stores
// `*outVol = 127` and jumps straight to the doppler test at 0x80019FC0, so a sound at the
// listener's own position keeps the listener's default pan and never calls `RatAtan2`.
void Sound3DParams(int32_t p, int32_t x, int32_t z, int32_t vx2, int32_t vz2, int32_t* outVol,
                   int32_t* outPan, int32_t* outPitch, int32_t shift, const Sound3DEnv& env);

// ---------------------------------------------------------------------------- SLUS 0x8001F174
// s32 StartVoice(int bank, int soundIndex, int restart, int reserved, SoundParams *p), 520 bytes
//
// Returns the voice handle `(channel << 27) | (serial & 0x07FFFFFF)`, or 0.
//
// `restart` is a boolean in disguise: the code computes the descriptor as `snd + 4 + 12*restart`,
// but the only path that sees a non-zero value first sets it to 0 (0x8001F1FC..0x8001F20C), so the
// descriptor is ALWAYS descriptor 0 and a non-zero `restart` only means "key the voice off first".
uint32_t StartVoice(int32_t bank, int32_t soundIndex, int32_t restart, int32_t reserved,
                    const SoundParams& params, const SoundSystemEnv& env);

// ---------------------------------------------------------------------------- SLUS 0x80017BA0
// void PlaySound3D(s32 x, s32 z, s32 soundIndex, s32 bank), 336 bytes
//
// The one door the whole race speaks to the sound system through - 46 call sites, 31 of them in
// `RASHCDG`. It is a LOOP over players: in a two-player game the same event
// starts two voices, one per listener, at two different volumes and pans.
struct PlaySound3DEnv {
    int32_t mutedBank = 0;   // *(gp+1940); a call naming this bank is dropped silently. 1 in every
                             // capture, and nothing in the race passes bank 1.
    int32_t defaultBank = 0; // *(gp+1912); substituted for `bank == 0`. 0 in every capture.
    int32_t master3d = 0;    // *(0x800D6C0C); the 3D master volume, 65 in every capture, so a
                             // sound at zero distance reaches 64 rather than 127.
    Sound3DEnv listener;
    SoundSystemEnv system;
};
void PlaySound3D(int32_t x, int32_t z, int32_t soundIndex, int32_t bank,
                 const PlaySound3DEnv& env);

// ---------------------------------------------------------------------------- SLUS 0x80016768
// void SetListener(int p, s32 x, s32 z, s32 vx, s32 vz, s32 camYaw), 60 bytes, a leaf
//
// SIX o32 arguments: `vz` at sp+16 and `camYaw` at sp+20. The listener faces the camera's way -
// `yaw = 2048 - camYaw`, half a turn of 4096 - but stands where the player's BIKE is.
void SetListener(int32_t p, int32_t x, int32_t z, int32_t vx, int32_t vz, int32_t camYaw,
                 const SoundBytes& listeners);

// ---------------------------------------------------------------------------- SLUS 0x80017B30
// u8 SurfaceSound(s32 k), 60 bytes, a leaf
//
// `table[clamp(k, 0, 51)]` over the 52-entry u8 table at `SLUS 0x800525C0`, with the clamp written
// as the compiler's branchless `(k & ~(k>>31)) + min(51 - k, 0)`. The table is game data, so the
// caller passes a pointer into the player's own image.
uint32_t SurfaceSound(int32_t k, const uint8_t* table52);

// ---------------------------------------------------------------------------- SLUS 0x80017B6C
// void QueueListenerSound(s32 a, s32 id, s32 t, s32 p), 52 bytes, a leaf
//
// The listener record carries a two-slot delayed-sound queue; slot 1 is reserved for id 109.
// `AudioFrame` decrements the countdowns and fires a slot when one reaches zero.
void QueueListenerSound(int32_t a, int32_t id, int32_t t, int32_t p,
                        const SoundBytes& listeners);

} // namespace rr::sim
