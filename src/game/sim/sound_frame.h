#pragma once
// The per-frame half of the sound system above the engine note: `AudioFrame SLUS 0x80018FAC` and
// everything under it that was not already ported, plus the loading pieces that establish the voice
// lists and the per-player records. Ported function by
// function from our own disassembly of the player's `SLUS_010.53` (SHA-1
// 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1), `RASHCDG.BIN` (cfe43a7786759f2cb9c57751cf99e84d1074782c)
// and `RASHCDI.BIN` (9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06); every function has one
// `rrverify phys` row in tools\rrverify\rows_engine_note.inc.
//
//   AudioFrame 0x80018FAC -> SetListener, SetReverbDepth, EngineNote, RoadNote, PlaySound3D,
//                            ObjSoundPick, ObjSoundStart, ObjSoundClaim, GetSoundPitch,
//                            ObjSoundStop, ObjSoundUpdate, StopVoice
//   ObjSoundPick  0x800184AC -> SpatialQuery RASHCDG 0x8008AE94, SpeechCue 0x8001B244, ObjSoundStop
//   ObjSoundClaim 0x80017F64 -> ObjSoundStop, GetSoundPitch, ObjSoundStart
//   ObjSoundStart 0x8001769C -> Sound3DParams, StartVoice
//   ObjSoundUpdate 0x800179D8 -> Sound3DParams, UpdateVoice (0x8001F6A4)
//   ObjSoundStop  0x80017814 -> StopVoice, itself
//   SpeechCue     0x8001B244 -> StopVoice, StartVoice
//   the loading side: ResetSoundState 0x8001E5A8 -> ResetVoiceLists 0x8001FBD4; PatchBank
//   0x8001EAA4; ReleaseAllVoices 0x8001F0D4 -> ReleaseVoice, KeyOff 0x8001EB60, SoundService;
//   SoundRecordsInit RASHCDI 0x80063448 (the race loader's per-player reset of the engine record's
//   handles and the object slots).
//
// THE PER-PLAYER RECORDS, read out of the code below:
//   *(gp+1920) -> Listener[numPlayers], 72 bytes: +0x00..+0x14 SetListener's,
//      +0x18 the reverb target (0 / 2) and +0x1C the road id it was last seen on (-1 = none),
//      +0x20 the reverb depth ramp (0..0x4FFF), +0x24 the number of OBJECT slots (5 in a one-player
//      race), +0x28 the number of CUE slots after them (3), +0x2C "a rider's cue is playing",
//      +0x30/+0x34 two delayed sound ids, +0x38/+0x3C their entities, +0x40/+0x44 their countdowns.
//   *(gp+1924 + 4p) -> ObjSlot[+0x24 + +0x28], 44 bytes each:
//      +0x00 u16 the object's handle (class << 5 | index; 0xE0 = free), +0x04 1 = a voice is started,
//      +0x08 the slot's kind (1 an engine, 2 a rider's cue, 3..5 a claimed sound, 6 a siren),
//      +0x0C/+0x10 x/z, +0x14/+0x18 the velocity, +0x1C the pitch, +0x20 the voice handle,
//      +0x24 the frame clock at which a cue slot times out, +0x28 the cue bank slot it came from.
#include <cstdint>

#include "game/sim/sound_engine.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- addresses
constexpr uint32_t kObjSlotsGp       = 1924;       // gp+1924 + 4p -> ObjSlot[], 44 bytes each
constexpr uint32_t kObjSlotBytes     = 44;
constexpr uint32_t kListenerBytes    = 72;
constexpr uint32_t kFrameCounterGp   = 1956;       // gp+1956: AudioFrame's own frame counter
constexpr uint32_t kCueBankGp        = 1904;       // gp+1904: SpeechCue's default bank (-1 = none)
constexpr uint32_t kSirenSoundGp     = 1908;       // gp+1908: the siren's sound index (-1 = none)
constexpr uint32_t kPendingCueGp     = 1944;       // gp+1944: "play cue slot 0's sound 0 once"
constexpr uint32_t kViewRecords      = 0x800CD898; // View[2], 1132 bytes each
constexpr uint32_t kViewRecordBytes  = 1132;
constexpr uint32_t kCameraPtr        = 0x8005AEC0; // -> the camera; +0x7C s16 is its yaw
constexpr uint32_t kCueSlots         = 0x800D6AA0; // 9 x 32-byte taunt bank slots (RiderSpeech)
constexpr uint32_t kCueHandle        = 0x800D6BC0; // the cue voice: handle, end clock, armed,
                                                   // slot to release, two holdoff words
constexpr uint32_t kCopRecordsPtr    = 0x8005B304; // -> 280-byte records (police), 3 live
constexpr uint32_t kCopRecordsOn     = 0x8005B314;
constexpr uint32_t kObjSoundTable    = 0x80052630; // s8[6], the sound of a non-bike object
constexpr uint32_t kSpuReverbDepthL  = 0x8005A3C4; // libspu's RAM copy of the reverb depth
constexpr uint32_t kGridOriginX      = 0x800CCF98; // the collision grid: origin
constexpr uint32_t kGridOriginZ      = 0x800CCFA0; //   x[2], z[2], chain nodes {next, id}[128],
constexpr uint32_t kGridNodes        = 0x800CCFA8; //   cells u8[2][24][24]
constexpr uint32_t kGridCells        = 0x800CD0B0;
constexpr uint32_t kGridPoolOffsets  = 0x800CCA68; // per-class record offset for classes 2..
constexpr uint32_t kPool1Ptr         = 0x8005B3A4; // -> pool-1 slot 0, stride 628

// ---------------------------------------------------------------------------- the loading side
// SLUS 0x8001FBD4: 22 reserved slots -1, free[i] = i with freeTop 23, 25 ring slots -1, both ring
// indices 0.
void ResetVoiceLists(SoundMachine& s);
// SLUS 0x8001E5A8: the record's first seven words and the three streaming records' first word 0,
// master volume 127, panning on, then ResetVoiceLists.
void ResetSoundState(SoundMachine& s);
// SLUS 0x8001EAA4 `PatchBank(bank, base)`: adds `base` to every descriptor's SPU address.
void PatchBank(SoundMachine& s, uint32_t bank, uint32_t base);
// SLUS 0x8001F0D4: releases every voice whose serial is non-zero, keys them all off, services.
// Returns SoundService's v0.
uint32_t ReleaseAllVoices(SoundMachine& s);
// RASHCDI 0x80063448 `SoundRecordsInit(p)`, the race loader's leaf: player p's engine record gets
// -1 in +0x10, +0x14, +0x18 (layers L0, L1, L2), +0x24, +0x28 (the road voices) and +0x08 (the
// bank slot, which the loader sets right after), the listener's +0x1C gets -1, and every object and
// cue slot of the player gets the free handle 0xE0. THIS is why the first EngineNote of a race
// does not start the idle loop a second time: its partner test is `f18 == 0` and -1 is not 0.
void SoundRecordsInit(SoundMachine& s, int32_t p);

// ---------------------------------------------------------------------------- libspu
// SLUS 0x800505F8 `SpuSetReverbDepth(attr)` for the attr SetReverbDepth builds (mask 0 = both):
// the reverb output volume 0x1F801D84/86 and libspu's RAM copy 0x8005A3C4/C6.
// SLUS 0x8001F054 `SetReverbDepth(left, right)`. Returns 0.
uint32_t SetReverbDepth(SoundMachine& s, uint32_t left, uint32_t right);

// ---------------------------------------------------------------------------- the object sounds
// RASHCDG 0x8008AE94 `SpatialQuery(hits, &count, pos, reach, classMask, self)`, the broad-phase
// query: every entity of a class in `classMask` whose octagonal distance from pos
// (x at +0, z at +8) is within `reach`, excluding the handle `self`, into `hits` (u16), up to
// `*count`; `*count` gets the number found. Guest addresses for `hits`, `count` and `pos`.
void SpatialQuery(SoundMachine& s, uint32_t hits, uint32_t count, uint32_t pos, int32_t reach,
                  uint32_t classMask, uint32_t self);
// SLUS 0x8001B244 `SpeechCue(kind)`: 4 = the finish line's cue of slot 6, anything else sound
// `kind` of the bank at gp+1904; plays it on the one cue voice 0x800D6BC0 with a 1500-tick life.
void SpeechCue(SoundMachine& s, int32_t kind);
// SLUS 0x8001769C `ObjSoundStart(slot, p, bank, sound)`: advances the slot's position by its
// velocity >> 6, asks Sound3DParams (with the doppler, shift 3 for a siren), scales by the slider
// of the slot's kind and starts a RESERVED voice. `sp` is the stack pointer it is called at (for
// Sound3DParams's unwritten outputs when the listener array is null).
void ObjSoundStart(SoundMachine& s, int32_t slot, int32_t p, int32_t bank, int32_t sound, uint32_t sp);
// SLUS 0x80017814 `ObjSoundStop(slot, p)`: stops the slot's voice, recursively the cue slots of the
// same object, releases its cue bank slot, and frees the slot (handle 0xE0, stamped with the clock).
void ObjSoundStop(SoundMachine& s, int32_t slot, int32_t p);
// SLUS 0x800179D8 `ObjSoundUpdate(slot, p)`: the live retune through UpdateVoice 0x8001F6A4.
void ObjSoundUpdate(SoundMachine& s, int32_t slot, int32_t p, uint32_t sp);
// SLUS 0x80017F64 `ObjSoundClaim(handle, p)`: puts sound 11 of the road bank on a cue slot for
// the object `handle` (kind 5 for a stopped one, 3/4 by the handle's low bit otherwise).
void ObjSoundClaim(SoundMachine& s, uint32_t handle, int32_t p, uint32_t sp);
// SLUS 0x800184AC `ObjSoundPick(p)`, every fourth frame: the nearest objects within 128.0 of the
// player's view (up to the object slot count), police records appended; frees the slots whose
// object is no longer among them, gives the new ones a free slot, removes duplicates.
void ObjSoundPick(SoundMachine& s, int32_t p, uint32_t sp);

// ---------------------------------------------------------------------------- the music
constexpr uint32_t kAlbumTable       = 0x80053578; // u32 count (18), u32 bytes, {start, length, flags}[18]
constexpr uint32_t kMusicTrackGp     = 0x8005B1F0; // the current track (gp+1380 is where 0x80024EF4 keeps it)
// SLUS 0x80024B20 `MusicPickShuffle()`: the next ALBUM.ALB track - counts the selectable tracks
// (flags bit 0) and those not yet played this cycle (bit 1 clear), starts a new cycle when none is
// left, then walks forward from `18 * (GetRCnt(2) & 0xFF) >> 8` to the first selectable unplayed one,
// marks it played and returns it; -1 when none is selectable.
uint32_t MusicPickShuffle(SoundMachine& s);
// SLUS 0x8001F37C `StreamVoiceStart(adsrVariant, SoundParams*, spuAddr)`: a RESERVED voice for a
// streamed channel, its descriptor one of the three streaming records S+0x1C/+0x30/+0x44 (ADSR
// 0x98FF or 0xB9FF / 0x5FCE, volume bytes 0x7F, the ring's SPU address), reverb off. Returns the
// handle, or 0 (no voice, or all three records busy - then the voice is released again).
uint32_t StreamVoiceStart(SoundMachine& s, int32_t adsrVariant, const SoundParams& p, uint32_t spuAddr);
// SLUS 0x8001F544 `KeyOnHandles(n, handles*)`: keys on, in one mask, every handle of the list whose
// serial is still its voice's.
uint32_t KeyOnHandles(SoundMachine& s, int32_t n, uint32_t handles);

// ---------------------------------------------------------------------------- the frame
// SLUS 0x80018FAC, the last call of GameFrame's body. `sp` is the stack pointer it is called at.
void AudioFrame(SoundMachine& s, uint32_t sp);

// ---------------------------------------------------------------------------- the animation's sounds
// The animation objects carry at +0x6DC a pointer into the event lists of DATA\ANIMNOIZ.DAT
// (PoseStart copies the play call's `ex` there):
// records { u32 key, u32 word, Record *next }, `word` = the gate / kind byte (bits 24..31) over a u16
// sound id. These two functions are the ONLY sound a punch, a kick or a swing makes in the original.
//
// SLUS 0x80017DA0 `AnimSoundFire(x, z, rec, flags)` (113 instructions, frame 48): `flags` = bit 7
// the object was struck (anim +0x24 bit 7), bits 0..3 the rider's weapon (riderDef +0x2E), bits 4..5
// riderDef +0x01 & 3, bit 6 a player's bike. The event fires when its word has bit 24 (always), bit
// 26 and struck, bit 25 and not struck, bit 30 and (flags >> 4 & 3) != 2, or bit 29 with
// (flags >> 4 & 3) == 0, not a player and root counter 2's low byte * 9 >> 8 == 0 (GetRCnt). The
// sound: kind 0x14 -> 61 + weapon; kind 0x40 -> 90 + (rc & 0xFF) >> 7, or 92 + ... when
// (flags >> 4 & 3) == 1; kind 0x09 -> id + ((word >> 16 & 0xFF) * (rc & 0xFF) >> 8); else the id.
// PlaySound3D(x, z, sound, 0). Returns the record's `next`.
uint32_t AnimSoundFire(SoundMachine& s, int32_t x, int32_t z, uint32_t rec, uint32_t flags);
// SLUS 0x80018E54 `AnimSounds(desc)` (86 instructions, frame 40), GameFrame step 5 (0x80011DBC, after
// ViewPass, before AnimationPass; every state): over the descriptor's objects (desc+0: the array,
// 2108 bytes each; +8 non-zero; +0x0C the count, re-read each object) that are playing (+0x24 bit 1),
// fires every event of the list at +0x6DC whose key is not above the object's frame (+0x10) and
// stores the next record there. The position is the owner's (+0) box centre +0xB8/+0xC0, or, for a
// rider (owner class 1) on or near his bike (+0x25C < 3), the bike's (+0x254), with the flags above.
// NAMED BOUND: a list that does not end within 65536 records (a cycle - the original loops forever)
// fails the call.
void AnimSounds(SoundMachine& s, uint32_t desc);

} // namespace rr::sim
