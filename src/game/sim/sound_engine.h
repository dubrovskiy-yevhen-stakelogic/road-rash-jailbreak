#pragma once
// The engine note, the road layer and the voice helpers under them, ported function by function
// from the resident executable `SLUS_010.53`, SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1, text
// base 0x80010000 (file offset 0x800). Every
// function below is transcribed from the replay-proven model of `tools\scout\engine_note.py` and
// re-read against our own disassembly, and is accepted by one `rrverify phys` row
// (tools\rrverify\rows_engine_note.inc).
//
//   EngineNote 0x800169D0 -> FixMul, Sound3DParams, GetSoundPitch, StartVoice, StopVoice, GetRCnt,
//                            SetVoiceLoopOffset, EngineStart 0x800167A4
//   EngineStart 0x800167A4 -> Sound3DParams, StartVoice x4, SetVoicePitchMod, StopVoice
//   RoadNote   0x80016E4C -> PlaySound3D, FixMul, Rand, GetSoundPitch, StopVoice, StartVoice,
//                            UpdateVoice 0x8001F6A4, Sound3DParams, and two effect spawners that are
//                            NOT sound and are NOT ported here (`EngineEffects`, below)
//   the helpers: StopVoice 0x8001F7EC, SetVoiceLoopOffset 0x8001F874, SetVoicePitchMod 0x8001F900,
//                GetSoundPitch 0x8001F934, ReleaseVoice 0x8001FB58, UpdateVoice 0x80019C54 (and its
//                byte-identical copy 0x8001F6A4), GetRCnt 0x80043F00
//   and the four pieces of Sony's libspu those helpers end in, ported ONLY as far as they touch
//   guest RAM or a register someone reads back:
//                SpuSetAnyVoice 0x800506A8 (behind SpuSetReverbVoice 0x80050678 and
//                SpuSetPitchLFOVoice 0x80051088), SpuSetVoiceAttr 0x80051C38 for the attribute
//                bits the game uses, and its address writer 0x8004F0A8.
//
// ============================================================================ THE MEMORY MODEL
//
// Guest addresses throughout (`GuestRam`, road_query.h), for the reasons road_query.h gives: these
// functions chase the engine record -> bike -> rider and -> stat block -> parameters on every call,
// re-read the chain after every callee, and three behaviours of the original are properties of
// ADDRESSES rather than of values (8A.6's StopVoice(0), which keys channel 0 off when voice 0's
// serial happens to be 0; the doppler slot `EngineNote` reads out of its own frame when the
// listener array is null; voice records read past the 24-voice table by a handle whose channel
// field is 24..31). A caller that has these structures as host copies lays them out in an arena.
//
// The hardware. libspu writes SPU registers and GetRCnt reads a root counter; a port cannot do
// either by itself, and it must not pretend to. Every such access goes through `SoundIo`, an
// aligned 16-bit load or store at the full guest address, and a `false` from it is a failure of the
// call, never a value. The bench answers it from the SAME concessions the interpreter uses
// (`Cpu::ConcessionLoad/Store`); the product answers it from its own register file
// and hands the voice registers to the mixer.
#include <cstdint>

#include "game/sim/road_query.h"
#include "game/sim/sound.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- the hardware
class SoundIo {
public:
    virtual ~SoundIo() = default;
    // An aligned 16-bit access to a hardware register at the full guest address (0x1F80xxxx or a
    // KSEG1 alias of it). `false` = this answerer cannot serve that register; the call fails.
    virtual bool Load16(uint32_t address, uint16_t& out) = 0;
    virtual bool Store16(uint32_t address, uint16_t value) = 0;
};

// The two visual-effect spawners `RoadNote` calls. They are not sound: they
// allocate from the 112-byte particle pool at 0x800D39B0 and are 2080 bytes with their closure.
// They are asked for, never stubbed; the bench has the oracle execute them.
class EngineEffects {
public:
    virtual ~EngineEffects() = default;
    // SLUS 0x80027778(bike, 1, 600, 0) - 4 o32 arguments.
    virtual void Burst(uint32_t bike, int32_t a1, int32_t a2, int32_t a3) = 0;
    // SLUS 0x80027974(bike, bike + 0xB8, kind) - 3 o32 arguments.
    virtual void Spray(uint32_t bike, uint32_t position, int32_t kind) = 0;
};

// Everything a call needs. `ram` is the same 2 MiB block `m` views; it is needed only to hand the
// stage-A emitter (sound.h) byte views straight onto guest memory, so that function and this file
// work on ONE copy of the state.
struct SoundMachine {
    GuestRam& m;
    uint8_t* ram;
    SoundIo& io;
    EngineEffects* fx = nullptr;
    // The first thing this port could not serve: an I/O register `io` refused, a libspu arm that is
    // not ported, an address the console faults on, a bank window too small. The call carries on
    // with 0 for the value; the caller FAILS the case.
    bool failed = false;
    uint32_t failAddress = 0;
    const char* failWhat = nullptr;
    void Fail(const char* what, uint32_t address) {
        if (failed) return;
        failed = true;
        failWhat = what;
        failAddress = address;
    }
    bool Faulted() const { return failed || m.Faulted(); }
};

// ---------------------------------------------------------------------------- addresses
// Every one read out of the functions' own lui/lw pairs (and engine_note.py `static`).
constexpr uint32_t kSoundGp          = 0x8005AC8C; // $gp of SLUS_010.53
constexpr uint32_t kEngineRecGp      = 1952;       // gp+1952 -> EngineRec[numPlayers], 132 bytes each
constexpr uint32_t kEngineRecBytes   = 132;
constexpr uint32_t kRoadBankGp       = 1940;       // gp+1940: the road bank, also PlaySound3D's mute
constexpr uint32_t kDefaultBankGp    = 1912;       // gp+1912: PlaySound3D's default bank
constexpr uint32_t kRoadMuteGp       = 1960;       // gp+1960: 1 = road layer silenced
constexpr uint32_t kListenerGp       = 1920;       // gp+1920 -> Listener[72]
constexpr uint32_t kSerialGp         = 2068;       // gp+2068: the sound-handle serial counter
constexpr uint32_t kTickToggleGp     = 1888;       // gp+1888: which player the vsync tick services
constexpr uint32_t kSoundSystem      = 0x800D6870; // the sound-system record
constexpr uint32_t kGearTable        = 0x800D6BD8; // s32[10], EngineSetup's per-gear table
constexpr uint32_t kEngineSlider     = 0x800D6C00; // the engine volume slider (41 live)
constexpr uint32_t kEffectsSlider    = 0x800D6C0C; // the effects volume = PlaySound3D's 3D master
constexpr uint32_t kSoundGameStatePtr = 0x8005B2F8;
constexpr uint32_t kAtanTableAddr    = 0x8005285C; // RatAtan2's table, 18 reachable words
constexpr uint32_t kSinCosAddr       = 0x8005624C; // {s16 sin; s16 cos}[4096]
constexpr uint32_t kSpuShadowFlag    = 0x8005A408; // bit 0: libspu writes the RAM shadow, not the SPU
constexpr uint32_t kSpuShadow        = 0x800DBF20; // the RAM shadow of the SPU registers, u16[256]
constexpr uint32_t kSpuDirtyMask     = 0x8005A3D4; // the shadow's dirty bits
constexpr uint32_t kSpuBasePtr       = 0x8005A41C; // -> 0x1F801C00 in every capture
constexpr uint32_t kSpuAlignOn       = 0x8005A440; // 0x8004F0A8: round addresses up when non-zero
constexpr uint32_t kSpuAlignShift    = 0x8005A444;
constexpr uint32_t kSpuAlignUnit     = 0x8005A448;
constexpr uint32_t kSpuAlignMask     = 0x8005A44C;
constexpr uint32_t kRootCounterBase  = 0x800549B8; // -> 0x1F801100, read by GetRCnt
constexpr uint32_t kSpuKeyShadow     = 0x800DC0A8; // SpuSetKey's RAM shadow: KON lo/hi, KOFF lo/hi
constexpr uint32_t kSpuKeyDirty      = 0x8005A3D0; // SpuSetKey's shadow-arm key-on accumulator
constexpr uint32_t kSpuKeyedMask     = 0x8005A3A8; // SpuSetKey's "voices keyed on" mask
constexpr uint32_t kAudioGate        = 0x8005ACA8; // bit 2: the vsync tick services the engines
constexpr uint32_t kSoundPlayerBikes = 0x8005B268; // -> player p's bike, stride 4
constexpr uint32_t kEngineParams     = 0x800525F4; // 20-byte parameter records in the EXE's data

// ---------------------------------------------------------------------------- libspu, the part
// SLUS 0x800506A8 `_SpuSetAnyVoice(on, mask, r, r1)`: reads the register pair `r1`, `r` (in that
// order) from the RAM shadow when `*(0x8005A408) & 1`, else from the SPU; then on == 1 ORs the mask
// in, on == 0 clears it, on == 8 writes it, anything else writes nothing; the shadow arms also set
// a dirty bit. Returns the resulting 24-bit mask. The ONLY reason it is ported: it READS the SPU.
uint32_t SpuSetAnyVoice(SoundMachine& s, int32_t on, uint32_t mask, uint32_t r, uint32_t r1);
// SLUS 0x80050678 / 0x80051088: the pair (204, 205) = EON and (200, 201) = PMON.
inline uint32_t SpuSetReverbVoice(SoundMachine& s, int32_t on, uint32_t mask) {
    return SpuSetAnyVoice(s, on, mask, 204, 205);
}
inline uint32_t SpuSetPitchLfoVoice(SoundMachine& s, int32_t on, uint32_t mask) {
    return SpuSetAnyVoice(s, on, mask, 200, 201);
}
// SLUS 0x8004F0A8(reg, addr): aligns `addr` (0x8005A440..0x8005A44C), shifts it, stores it to
// register `reg` unless reg is -1 or -2. Returns addr (-2 and the store arm) or the shifted value.
uint32_t SpuWriteAddress(SoundMachine& s, int32_t reg, uint32_t addr);

// The fields of Sony's `SpuVoiceAttr` that SpuSetVoiceAttr 0x80051C38 reads for the attribute
// bits this game passes (0x13 from UpdateVoice, 0x10000 from SetVoiceLoopOffset, 0x60093 from
// ProgramVoice). Offsets are the struct's.
struct SpuVoiceAttr {
    uint32_t mask = 0;       // +0x04
    uint16_t volL = 0;       // +0x08
    uint16_t volR = 0;       // +0x0A
    uint16_t volModeL = 0;   // +0x0C
    uint16_t volModeR = 0;   // +0x0E
    uint16_t pitch = 0;      // +0x14
    uint32_t addr = 0;       // +0x1C  start address
    uint32_t loopAddr = 0;   // +0x20  repeat address
    uint16_t adsr1 = 0;      // +0x3A
    uint16_t adsr2 = 0;      // +0x3C
};
// SLUS 0x80051C38(channel, attr) for the bits VOLL 0x1, VOLR 0x2, VOLMODEL 0x4, VOLMODER 0x8,
// PITCH 0x10, WDSA 0x80, LSAX 0x10000, ADSR1 0x20000, ADSR2 0x40000. Any other bit, or mask 0
// ("everything"), is not ported and FAILS the call. Returns 0, as the original does.
uint32_t SpuSetVoiceAttr(SoundMachine& s, uint32_t channel, const SpuVoiceAttr& a);

// SLUS 0x80050D08 `SpuSetKey(on, mask)`: on == 1 writes KON (0x1F801D88/8A), on == 0 writes
// KOFF (0x1F801D8C/8E) - or, with the shadow flag, the RAM shadow at 0x800DC0A8 plus two
// bookkeeping words - and keeps the "keyed" mask at 0x8005A3A8. Any other `on` returns 1.
uint32_t SpuSetKey(SoundMachine& s, int32_t on, uint32_t mask);

// ---------------------------------------------------------------------------- the helpers
// SLUS 0x80043F00: `id & 0xFFFF < 3` ? the 16-bit value register of that root counter : 0.
uint32_t GetRCnt(SoundMachine& s, uint32_t id);
// SLUS 0x8001E86C over guest addresses: the sound record's ADDRESS, or 0.
uint32_t GuestLookupSound(SoundMachine& s, uint32_t bank, int32_t index);
// SLUS 0x8001F934: the u16 pitch of descriptor `desc` of sound `sound` of bank slot `bank`, or 0.
uint32_t GetSoundPitch(SoundMachine& s, int32_t bank, int32_t sound, int32_t desc);
// SLUS 0x8001FB58 (a0 = the voice record's address). Returns what the original leaves in v0.
uint32_t ReleaseVoice(SoundMachine& s, uint32_t voice);
// SLUS 0x8001F7EC. Returns what the original leaves in v0: the voice's masked serial on the stale
// path, SpuSetReverbVoice's answer on the live one.
uint32_t StopVoice(SoundMachine& s, uint32_t handle);
// SLUS 0x8001F874. Returns 1 on the stale path (the delay slot's `li v0,1`), else 0.
uint32_t SetVoiceLoopOffset(SoundMachine& s, int32_t offset, uint32_t handle);
// SLUS 0x8001F900. NO serial test. Returns SpuSetPitchLFOVoice's answer.
uint32_t SetVoicePitchMod(SoundMachine& s, uint32_t handle, int32_t on);
// SLUS 0x80019C54 and its byte-identical copy 0x8001F6A4. `params` is the caller's SoundParams;
// only its low halfword of `pitch` is read (`lhu 0(t2)`). Returns the voice's masked serial on the
// stale path, 0 on the live one.
uint32_t UpdateVoice(SoundMachine& s, uint32_t handle, const SoundParams& params);

// SLUS 0x8001EB7C `ProgramVoice(voice, pitch, volL, volR)`: SpuSetVoiceAttr with mask 0x60093 -
// volume, pitch, the descriptor's start address and its two ADSR words.
uint32_t ProgramVoice(SoundMachine& s, uint32_t voice, uint32_t pitch, uint32_t volL, uint32_t volR);
// SLUS 0x8001EE94 `SoundService()`: the key-off commit, then every voice whose
// channel bit is pending is programmed, then the key-on commit. Returns what the original leaves
// in v0: 1 when the re-entrancy guard is set, else 0x800D6870.
uint32_t SoundService(SoundMachine& s);
// SLUS 0x80019990 `AudioVSyncTick()`: the two ramps,
// the four layer retunes, the player toggle, then SoundService. Returns SoundService's v0.
uint32_t AudioVSyncTick(SoundMachine& s);

// ---------------------------------------------------------------------------- the emitter, over guest memory
// The ported emitter of sound.h, handed byte views straight onto guest memory. `noReverb != 0`
// reaches SpuSetReverbVoice(0, ...) exactly as 0x8001F31C does.
uint32_t GuestStartVoice(SoundMachine& s, int32_t bank, int32_t sound, int32_t noReverb,
                         int32_t reserved, const SoundParams& params);
void GuestSound3DParams(SoundMachine& s, int32_t p, int32_t x, int32_t z, int32_t vx2, int32_t vz2,
                        int32_t* outVol, int32_t* outPan, int32_t* outPitch, int32_t shift);
void GuestPlaySound3D(SoundMachine& s, int32_t x, int32_t z, int32_t sound, int32_t bank);
// SLUS 0x8001F9C4 over guest memory: the voice RECORD's address, or 0 for the original's NULL.
uint32_t GuestAllocVoice(SoundMachine& s, int32_t reserved);

// ---------------------------------------------------------------------------- the engine note
// `sp` is the stack pointer the original is CALLED at. Only EngineNote uses it, for one read: when
// the listener array is null, Sound3DParams returns without writing the doppler slot, and the
// original then stores whatever its own frame held at `sp - 96 + 64` into E+0x78.
// SLUS 0x80019D9C, 164 bytes: the sound globals' reset - the default bank (gp+1912), the road bank
// (gp+1940) and gp+1904 to -1, the listener and engine pointers and eleven more words to 0, gp+1876
// to 1, and the seven volume sliders 0x800D6C00 (and their copy at 0x800D6C20) to
// `7 * v >> 4` of the EXE's defaults at 0x80052638. The race loader then sets the banks and the two
// pointers. Returns 0, as the original does.
uint32_t AudioReset(SoundMachine& s);
// SLUS 0x8001654C, 540 bytes, called once per race from 0x80011B94: for every player, E+0x00 (the
// bike from *(0x8005B268 + 4p)), E+0x2C (the parameter record 0x800525F4 + 20 * (model / 9)), the
// ten-gear table 0x800D6BD8 and the scales E+0x3C/+0x44/+0x48/+0x50 from the bike's stat block.
// `divu` by zero is the R3000's: quotient 0xFFFFFFFF, no trap.
void EngineSetup(SoundMachine& s);
void EngineStart(SoundMachine& s, int32_t p);                 // SLUS 0x800167A4, 556 bytes
void EngineNote(SoundMachine& s, int32_t p, uint32_t sp);     // SLUS 0x800169D0, 1148 bytes
void RoadNote(SoundMachine& s, int32_t p);                    // SLUS 0x80016E4C, 2128 bytes

} // namespace rr::sim
