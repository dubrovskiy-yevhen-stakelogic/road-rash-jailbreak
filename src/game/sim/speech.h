#pragma once
// The riders' voices: the taunt and crash lines of DATA\AUDTAUNT.STR and
// the provocation a player's taunt makes, ported function by function from our own
// disassembly of the player's `SLUS_010.53` (SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1). Every
// function has one `rrverify phys` row in tools\rrverify\rows_speech.inc.
//
//   SpeechInit      0x8001A424 -> Malloc 0x8001447C, GetRCnt, StreamRequest 0x80023148 x1-2
//   SpeechBankLoad  0x8001A0C0 -> SpeechSlotFind, ObjSoundStop, BankFree 0x8001EE28, memcpy 0x8001E0B4,
//                                 LoadBank 0x8001ED28 (the CD streamer's dispatch hands it a record of
//                                 type 10, 0x80031604)
//   SpeechSlotFind  0x8001A2E4 -> ComputePlace 0x800138E8, GetRCnt
//   SpeechUploaded  0x80016464 -> the stream buffer release 0x80030FA0 (LoadBank's upload callback)
//   LoadBank        0x8001ED28 -> UploadSamples 0x8001E938, PatchBank 0x8001EAA4
//   BankFree        0x8001EE28 -> UnloadBank 0x8001EA54 -> SpuFree 0x8004F998
//   RiderSpeech     0x8001A760 -> ObjSoundStop, GetRCnt x2 per candidate, Pick RASHCDG 0x8008B428,
//                                 CanEngage 0x800BC1EC, NextMove 0x800B92C0, AiPushCommand 0x800BCA68,
//                                 AiProject 0x800B6AAC, GetSoundPitch, ObjSoundStart, PlaySound3D
//
// THE NINE SPEECH SLOTS, 32 bytes each at 0x800D6AA0 (with their categories u32[9] at 0x800D6C40):
//   +0x00 the LoadBank slot of the bank the slot holds (-1 none), +0x04 the record id (sound << 0 |
//   speaker << 8), +0x08 the state (0 empty, 1 uploading, 2 ready, 3 a line playing on a cue voice,
//   4 a line played), +0x0C/+0x10 the categories of its two sounds ("cat[col]", 255/-1 = none or used
//   up), +0x14/+0x18 their sound indices in the bank (0, 1), +0x1C the 0x38-byte bank header block.
// Slot 5 is always the default bank's crash exclamations 88/89 (category 11), slot 8 the default bank's
// sound 108 (category 15) when its category is 15; the others take AUDTAUNT.STR records whose sound id
// equals the slot's category.
//
// Everything the functions reach that is not ported, or is ported somewhere else and run by the host, is
// a `SpeechCallees` call; a false return fails the call (the caller must not trust what it wrote).
#include <cstdint>

#include "game/sim/sound_frame.h"

namespace rr::sim {

constexpr uint32_t kSpeechSlots      = 0x800D6AA0; // = kCueSlots: 9 x 32 bytes
constexpr uint32_t kSpeechSlotBytes  = 32;
constexpr uint32_t kSpeechCategories = 0x800D6C40; // u32[9]
constexpr uint32_t kSpeechFile       = 0x800D6858; // the AUDTAUNT.STR stream record: +0 handle, +4 offset
constexpr uint32_t kSpeechToggleGp   = 1972;       // gp+1972: the column alternation of RiderSpeech
constexpr uint32_t kSpeechUploadCb   = 0x80016464; // LoadBank's callback for a speech bank
constexpr uint32_t kGrudgeIndex      = 0x800D38B0; // u8 per handle: the grudge byte a handle owns

struct SpeechCallees {
    virtual ~SpeechCallees() = default;
    // --- ported elsewhere, run by the host
    // SLUS 0x800138E8 ComputePlace(bike, mode) (race.h).
    virtual bool ComputePlace(uint32_t bike, int32_t mode, uint32_t& v0) = 0;
    // RASHCDG 0x800BCA68 AiPushCommand(cmd, mode, e) (ai.h); `cmd` is the guest address of the 8-byte
    // record in RiderSpeech's frame (only its first four bytes are written; the rest is the frame's).
    virtual bool PushCommand(uint32_t cmd, int32_t mode, uint32_t e) = 0;
    // --- not ported: the heap, the CD streamer and the SPU transfer
    virtual bool Malloc(uint32_t bytes, uint32_t a1, uint32_t& v0) = 0;                     // SLUS 0x8001447C
    // SLUS 0x80023148(stream, count, a2, a3): queue `count` 0x4000-byte records from stream+4 on.
    virtual bool StreamRequest(uint32_t stream, uint32_t count, uint32_t a2, uint32_t a3, uint32_t& v0) = 0;
    // SLUS 0x8001E938(bank, src, cb, cbArg, a4): SPU memory for bank+0x0C bytes and the transfer from
    // `src`; v0 = the SPU address (<= 0 = failure).
    virtual bool UploadSamples(uint32_t bank, uint32_t src, uint32_t cb, uint32_t cbArg, uint32_t a4,
                               uint32_t& v0) = 0;
    virtual bool SpuFree(uint32_t spuAddress) = 0;                                           // SLUS 0x8004F998
    virtual bool ReleaseBuffer(uint32_t index) = 0;                                          // SLUS 0x80030FA0
};

// SLUS 0x8001ED28 LoadBank(bank, src, slot, cb, cbArg, a5): the first free bank slot when `slot` < 0 (-1
// when none up to the bank count), -2 when the slot is taken, -3 when the upload fails; else the slot,
// with bank+0x08 = the SPU address and the descriptors patched.
int32_t LoadBank(SoundMachine& s, SpeechCallees& c, uint32_t bank, uint32_t src, int32_t slot, uint32_t cb,
                 uint32_t cbArg, uint32_t a5);
// SLUS 0x8001EA54 UnloadBank(slot) and 0x8001EE28 BankFree(slot) (bounds, then unload and clear).
void UnloadBank(SoundMachine& s, SpeechCallees& c, int32_t slot);
void BankFree(SoundMachine& s, SpeechCallees& c, int32_t slot);

// SLUS 0x8001A2E4 SpeechSlotFind(id): the speech slot for a record of id `id`, walking the nine slots
// from a hardware-counter start; -1 when none takes it. Categories 0/1 of the table take the player's
// place band (1 in the top five, else 2) into bits 4.. on the way.
int32_t SpeechSlotFind(SoundMachine& s, SpeechCallees& c, uint32_t id);
// SLUS 0x8001A0C0 SpeechBankLoad(rec, id, buffer): an AUDTAUNT record (`rec` = the record + 0x20) into
// its slot. Returns 1 (loaded; the buffer is kept until the upload callback) or 2 (dropped).
uint32_t SpeechBankLoad(SoundMachine& s, SpeechCallees& c, uint32_t rec, uint32_t id, uint32_t buffer);
// SLUS 0x80016464, LoadBank's completion callback: arg != 0 marks slot (arg >> 16) & 0x7F ready (state 2)
// and releases stream buffer arg & 0xFF; arg == 0 sets gp+1876.
void SpeechUploaded(SoundMachine& s, SpeechCallees& c, uint32_t arg);
// SLUS 0x8001A424 SpeechInit(): the slots' categories for this race type, their header blocks, the two
// resident default-bank slots and the stream of seven records from a random place in AUDTAUNT.STR.
void SpeechInit(SoundMachine& s, SpeechCallees& c);
// SLUS 0x8001A760 RiderSpeech(h, crash): bike `h`'s voice line (a taunt when crash == 0) for every
// listener; a player's taunt provokes the nearest AI. `sp` is the stack pointer
// it is CALLED at (its 128-byte frame holds the command record it pushes).
void RiderSpeech(SoundMachine& s, SpeechCallees& c, uint32_t h, int32_t crash, uint32_t sp);

} // namespace rr::sim
