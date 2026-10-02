#pragma once
// The native sound runtime of the product: the state the ported sound code of `src\game\sim` works
// on, the banks it looks sounds up in, and the step below it that turns what that code asks for into
// something our mixer plays.
//
// Two layers of ported code run here, and both are proven bit-exact by `rrverify phys`:
//   * the emitter of `sound.h` (PlaySound3D, StartVoice,
//     AllocVoice, LookupSound, Sound3DParams, KeyOn/KeyOff, SetListener, SurfaceSound,
//     QueueListenerSound);
//   * the engine note of `sound_engine.h`: AudioReset, EngineSetup,
//     EngineNote, EngineStart, RoadNote, AudioVSyncTick, SoundService, ProgramVoice, UpdateVoice,
//     StopVoice, SetVoiceLoopOffset, SetVoicePitchMod and the parts of libspu they end in.
//
// THE SOUND ARENA. The engine-note code works on guest addresses, so the runtime keeps a 2 MiB
// arena laid out at the original's own addresses for everything that code reaches by a fixed
// address - the sound-system record 0x800D6870, the gp-relative globals, the gear table, the
// volume sliders, libspu's globals - and at addresses of our own choosing for what the original
// reaches through a pointer (the voice table, the banks, the listener and engine records, the bike
// and its two blocks). The stage-A emitter is handed byte views onto the SAME arena, so there is one
// copy of the sound state, not two. What each region holds and who wrote it is named in the .cpp.
//
// Two modes, and a host picks one:
//   * EMITTER-ONLY (no `SetExecutable`): no engine note - `Service()` is a STAND-IN for
//     SoundService + ProgramVoice that drains the key masks and hands the six voice values
//     (SoundVoiceStart) to the host's own mixer voices;
//   * ENGINE (after `SetExecutable`, before `Reset`): the arena starts as the player's own
//     `SLUS_010.53`, the ported AudioReset runs, `EngineFrame` runs the ported per-frame note and
//     road layer, `VSync` runs the ported vsync tick INCLUDING the ported SoundService, and every SPU
//     register write that code makes goes to `Spu()` - the register-driven SPU voice model of
//     mixer.h (pitch modulation and live repeat addresses included, stages B5/B6). `Service()` then
//     returns nothing: the masks are the ported SoundService's to drain. The host plays `Spu()`
//     through its mixer (`SpuVoicesSource`).
//
// Named seams of the engine mode, each counted so it is a measurement and not a claim:
//   * the race loader's writes that set up the sound globals (RASHCDI 0x800627F8, not ported) are
//     done here with the values every capture shows: default bank 0, road bank 1, engine bank slot 2
//     = RASHNZ_E.DAT entry 3 (the loader's `3 + model/9` for model < 9), the listener and engine
//     arrays; the audio gate 0x8005ACA8 gets bit 2 (5 in every capture, 1 in the EXE image);
//   * RoadNote's two visual-effect spawners (SLUS 0x80027778 EffectBurst, 0x80027974 EffectSpray) are
//     PORTED (spine.h) but they write the RACE's effect records 0x800D39B0 and the bike's budget word
//     +0x24, which live in the host's arena, not in this one: the host that owns that memory runs them
//     through `SetEffectTarget`. Without a target a request is only counted (`EffectsNotRun`);
//   * root counter 2, which EngineNote reads for its jitter, is the console's free-running timer; the
//     product answers it from a deterministic generator of its own (see the .cpp);
//   * the SPU model's own limits are mixer.h's: no Gaussian interpolation, no reverb.
// The WORLD mode (`Attach`) runs the same code on the race's own arena, with
// AudioFrame, the loader's SoundRecordsInit and the music; its named seams are in the .cpp.
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "game/audio/mixer.h"
#include "game/sim/sound.h"
#include "game/sim/speech.h"

namespace rr::game {

// One voice the emitter keyed on this frame, as the six values ProgramVoice programs plus the decoded
// samples (emitter-only mode). `adsr1`/`adsr2` are passed through unchanged: no envelope yet.
struct SoundVoiceStart {
    int32_t channel = 0;
    uint32_t spuAddress = 0; // the byte offset inside the bank's own samples
    uint16_t pitch = 0;      // 0x1000 = 44100 Hz, the SPU's own pitch register
    uint16_t adsr1 = 0;
    uint16_t adsr2 = 0;
    int32_t volumeLeft = 0;  // the original's own units, straight out of the voice record
    int32_t volumeRight = 0;
    const std::vector<int16_t>* pcm = nullptr; // decoded once and cached by the runtime
};

// Where RoadNote's two spawners run (see the file header). `kind`, `life`, `tag` are the o32
// arguments the original passes after the bike: EffectBurst(bike, kind, life, tag) and
// EffectSpray(bike, (unused), kind). `bike` is a player's bike in the world (RoadNote runs once per player's
// engine: player 1's too in a two-player race), or 0 in the own-arena mode (player 0's, the one EngineFrame
// was given).
class SoundEffectTarget {
public:
    virtual ~SoundEffectTarget() = default;
    virtual void Burst(uint32_t bike, int32_t kind, int32_t life, int32_t tag) = 0;
    virtual void Spray(uint32_t bike, int32_t kind) = 0;
};

class SoundRuntime {
public:
    SoundRuntime();
    ~SoundRuntime();

    // `DATA\RASHNZ_E.DAT`. Returns false - with a reason - when the 8-entry directory does not
    // close exactly on the file size, which is the acceptance test (docs\formats\audio.md).
    bool LoadBanks(std::span<const uint8_t> rashnz, std::string& error);
    // The player's own `SLUS_010.53` exactly as read off the disc (with its 0x800-byte header). Given
    // BEFORE `Reset`, it switches the runtime to the engine mode (file header).
    void SetExecutable(std::span<const uint8_t> slus);
    // THE WORLD MODE. Given before `Reset` (with `SetExecutable`): the sound state
    // lives in the HOST's guest arena - the race's one arena, which already holds the executable
    // and the overlay at their load addresses - at the original's fixed addresses, with the blocks
    // the original allocates placed in `heap` (at least 16 KiB of the host's that nothing else
    // uses). Then `Frame` runs the PORTED AudioFrame on the world itself: the listener from the
    // view record, the engine note of the player's own bike, the object sounds of the nearest
    // bikes and cars, the delayed sounds and the cue voice - no copies in or out.
    void Attach(uint8_t* hostRam, uint32_t heap, uint32_t heapBytes);
    bool Attached() const { return attached_; }
    // World mode: one game frame of sound, as GameFrame's last call. `sp` is a stack pointer in the
    // host arena that nothing live is below (AudioFrame's frames are < 1 KiB deep).
    bool Frame(uint32_t sp);
    // World mode, for the log: how many object slots of player 0 hold an object, and how many of
    // those have a voice started.
    int ObjectSlotsBusy(int* withVoice) const;
    // World mode, after `Reset` and before the first `Frame`: the race's music. The
    // PORTED MusicPickShuffle SLUS 0x80024B20 picks the ALBUM.ALB track; its lead-in (INTRO.ALB,
    // 8 units at track * 0x20000) and then ALBUM.ALB from the track's start plus its skip
    // (0x8005365C) are streamed into the two 64 KiB rings at SPU 0x434E0 / 0x534E0; the two voices
    // are started by the PORTED StreamVoiceStart + KeyOnHandles as MusicStart SLUS 0x800212FC
    // starts them - hard left / hard right, at the music slider. `read(file, offset, dst, bytes)`
    // fetches bytes of ALBUM.ALB (file 0) or INTRO.ALB (file 1).
    using FileRead = std::function<bool(int file, uint32_t offset, uint8_t* dst, uint32_t bytes)>;
    bool StartMusic(const FileRead& read);
    int MusicTrack() const { return musicTrack_; }
    uint64_t MusicUnits() const { return musicUnits_; }
    // How many times the stream went on to the next track (0x80024CB4 -> 0x80024EF4(-1), PORTED
    // MusicPickShuffle): `read` of StartMusic is kept for it, so the disc it reads must outlive the race.
    int MusicTracksChained() const { return musicChained_; }

    // ---- the riders' voices, world mode, after StartMusic
    // SLUS 0x800138E8 ComputePlace(bike, mode) and RASHCDG 0x800BCA68 AiPushCommand(cmd, mode, e): PORTED
    // elsewhere, run by the race on its arena.
    using PlaceFn = std::function<bool(uint32_t bike, int32_t mode, uint32_t& place)>;
    using PushFn = std::function<bool(uint32_t cmd, int32_t mode, uint32_t e)>;
    // The PORTED SpeechInit SLUS 0x8001A424 (the slots' categories, the two default-bank slots, the
    // request for seven records of AUDTAUNT.STR from a random place). `read(2, offset, dst, bytes)` reads
    // AUDTAUNT.STR (`fileBytes` long). The records arrive at the start of the first `Frame`, after
    // EngineSetup (as the CD delivers them in the original), each through the PORTED SpeechBankLoad
    // SLUS 0x8001A0C0 -> SpeechSlotFind -> LoadBank; the SPU heap and the transfer are ours (the bases
    // are the capture's: 0x634E0 on, 0x3FC0 apart), the upload completes at once (PORTED callback
    // SLUS 0x80016464).
    bool StartSpeech(const FileRead& read, uint32_t fileBytes, const PlaceFn& place);
    // The PORTED RiderSpeech SLUS 0x8001A760(h, crash) - a rider's taunt (crash 0) or crash line, and for
    // a player's taunt the provocation of the nearest AI. `sp` is where the call is made.
    bool Speech(uint32_t h, int32_t crash, uint32_t sp, const PushFn& push);
    // A type-10 chunk the CD streamer's dispatch 0x80031604 hands over - the
    // PORTED SpeechBankLoad SLUS 0x8001A0C0(rec, id, index) on the chunk in its table buffer (rec = its payload,
    // the chunk + 0x20). The samples go to SPU memory from the buffer (+0x40 of rec, the bank's size, the
    // bytes after the buffer included as the console's DMA takes them); the upload's completion
    // (the PORTED callback SLUS 0x80016464) is returned in `done` (fn, arg pairs) for the caller to run when
    // the transfer is over - its buffer release SLUS 0x80030FA0 is the caller's `release`. v0: 1 loaded, 2
    // dropped; false when the voices are not running.
    bool StreamSpeechBank(uint32_t rec, uint32_t id, uint32_t index, uint32_t& v0,
                          std::vector<std::pair<uint32_t, uint32_t>>& done);
    // SLUS 0x80016464 for a completion StreamSpeechBank returned; `release(index)` frees the table record.
    bool StreamSpeechDone(uint32_t arg, const std::function<bool(uint32_t)>& release);
    size_t StreamBanksLoaded() const { return streamBanksLoaded_; }
    size_t StreamBanksDropped() const { return streamBanksDropped_; }
    // For the log: speech slots holding a bank (state 2..4), lines started, provocations pushed.
    int SpeechSlotsLoaded() const;
    // "slot:category0/category1 state@bank ..." of the nine speech slots, for the log.
    std::string SpeechTable() const;
    size_t SpeechCalls() const { return speechCalls_; }
    size_t SpeechLines() const { return speechLines_; }
    size_t SpeechPushes() const { return speechPushes_; }
    const std::string& SpeechFault() const { return speechFault_; }
    // SoundInit + ResetSoundState + ResetVoiceLists in the layout those three produce, the resident
    // bank assignment of every race capture (slot 0 = entry 0, slot 1 = entry 2, slot 2 = entry 3),
    // and in the engine mode the ported AudioReset plus the loader's values (file header).
    void Reset();
    // The next read of root counter 2 answers `value` - the console's counter at this moment
    // (root_counter.h ConsoleRootCounter2: a race start's SpeechInit and MusicPickShuffle read it once
    // each); the reads after it go on from the generator, which that value re-seeds.
    void ReadRootCounterAs(uint16_t value) {
        counterPending_ = true;
        counterValue_ = value;
    }

    // The two tables `Sound3DParams` and `SurfaceSound` read out of the EXE image. Game data, so the
    // caller passes pointers into the player's own `SLUS_010.53`. Copied into the arena.
    void SetTables(const int32_t* atan, const int16_t* sincos, const uint8_t* surface52);
    // `*(0x8005B2F8)`, whose `+0x30` is the player count and `+0x48` the bike model. 0x60 bytes are
    // copied into the arena before every call that reads it.
    void SetGameState(const uint8_t* gameState) { gameState_ = gameState; }
    // The host that runs RoadNote's two PORTED effect spawners on its own arena (file header).
    void SetEffectTarget(SoundEffectTarget* target) { effectTarget_ = target; }

    void SetListener(int32_t p, int32_t x, int32_t z, int32_t vx, int32_t vz, int32_t camYaw);
    void PlaySound3D(int32_t x, int32_t z, int32_t soundIndex, int32_t bank);
    // Observation: the PlaySound3D calls since the last take, at most 256 kept.
    struct Played { int32_t id, x, z; };
    std::vector<Played> TakePlayed() { std::vector<Played> o; o.swap(played_); return o; }
    // SLUS 0x8001B244 SpeechCue(kind), PORTED (sound_frame.h), on the attached world (race_modes.cpp: the
    // player cop's arrest and its FSM). False when no world is attached (the call is not made).
    bool SpeechCue(int32_t kind);
    // SLUS 0x80018E54 AnimSounds(desc), PORTED (sound_frame.h): GameFrame step 5,
    // the animation objects' ANIMNOIZ.DAT events - the punch, kick and swing sounds - on the attached world. False
    // when no world is attached or the port faulted (the call is not made / its effects are partial).
    bool AnimSounds(uint32_t desc);
    size_t AnimSoundCalls() const { return animSoundCalls_; }
    // SLUS 0x80018440 RiderOffSound(h, mode) and 0x8001B3C8 BustedMusic(), PORTED (sim\takedown.h), on the
    // attached world; BustedMusic's stream request is the speech stream's (served at the next Frame).
    // False when no world is attached or the port faulted (the call is not made / its effects are partial).
    bool RiderOffSound(uint32_t h, uint32_t mode, uint32_t* v0 = nullptr);
    bool BustedMusic();
    // SLUS 0x80018C1C RaceOverSignal(pause), PORTED (sim/race_over.h), on the attached world.
    // False when no world is attached or the port faulted.
    bool RaceOverSignal(uint32_t pause);
    // SLUS 0x80016528 (the countdown's end: StopVoice on gp+1964, gp+1964 = 0), PORTED (sim/countdown_voice.h),
    // on the attached world; its start 0x800164B4 runs in Frame right after EngineSetup, as
    // 0x80011BF8 follows 0x80011B94. False when no world is attached or the port faulted.
    bool CountdownVoiceStop();
    uint32_t CountdownVoiceStarted() const { return countdownStarted_; }
    // SLUS 0x80020E30 SoundHold(v), PORTED (sim/pause.h), on the attached world.
    bool SoundHold(uint32_t v);
    void QueueListenerSound(int32_t a, int32_t id, int32_t t, int32_t p);
    uint32_t SurfaceSound(int32_t k) const;

    // Emitter-only mode: drains the key-on and key-off masks the ported emitter set. A STAND-IN for
    // `SoundService` and `ProgramVoice`, not itself proven. Returns nothing in the engine mode.
    void Service(std::vector<SoundVoiceStart>& started, std::vector<int32_t>& stopped);

    // ---- the engine note (engine mode only; false / no-op otherwise)
    bool EngineMode() const { return engineMode_; }
    // One game frame of player 0's engine: the bike's 1096-byte entity, the object at its +0x354
    // (the "owner", whose +0x25C says the rider is on the bike) and the stat block at its +0x22C,
    // copied into the arena; on the first call the ported EngineSetup; then the ported EngineNote
    // and RoadNote. `lcgSeed` is the game's shared LCG, which RoadNote pulls while the bike slides
    // - in and out, so the host's random stream stays the original's.
    bool EngineFrame(std::span<const uint8_t> bike, std::span<const uint8_t> owner,
                     std::span<const uint8_t> stats, uint32_t& lcgSeed);
    // One vertical blank: the ported AudioVSyncTick, which retunes the engine layers and runs the
    // ported SoundService (key-offs, voice programming, key-ons) into `Spu()`.
    void VSync();
    // The SPU voice model every register write of the engine mode lands in (mixer.h).
    std::shared_ptr<rr::audio::SpuVoices> Spu() const { return spu_; }

    bool Ready() const { return !bankSlots_.empty(); }
    // Counters, so "the game makes a sound" is a measurement rather than a claim.
    size_t Requested() const { return requested_; }
    size_t Allocated() const { return allocated_; }
    size_t Programmed() const { return programmed_; }
    size_t LoadedSounds() const { return pcm_.size(); }
    int32_t SoundCountOfBank(int32_t slot) const;
    size_t EngineFrames() const { return engineFrames_; }
    size_t VSyncs() const { return vsyncs_; }
    size_t SpuWrites() const { return spuWrites_; }
    size_t EffectsNotRun() const { return effectsNotRun_; }
    // The first thing the ported engine code could not serve, or empty.
    const std::string& EngineFault() const { return engineFault_; }
    // Guest reads of the arena, for a probe that wants to print the engine record.
    uint32_t ArenaWord(uint32_t address) const;
    // The race loader (sound_loader.cpp): AUDTAUNT.STR's size for SoundLoad's stream record (given before
    // `Reset`), and the set-up's log line after it. RRJB_LOADER=off: the named layout of ResetWorld stands.
    void SetLoaderSpeechBytes(uint32_t n) { loaderSpeechBytes_ = n; }
    const std::string& LoaderLine() const { return loaderLine_; }
    bool LoaderRan() const { return loaderRan_; }
    // libspu's SPU-RAM allocator PORTED (sim\spu_heap.h, sound_loader.cpp): every SPU block of the world mode -
    // the banks, the music rings, the speech banks - placed by SpuMalloc / SpuFree on the arena's table 0x800D6A38.
    // RRJB_SPUHEAP=off: the old placement (the negative control). For the log.
    bool SpuHeapPorted() const { return spuHeapPorted_; }
    std::string SpuHeapLine() const;

private:
    class Io;
    class Effects;
    class Voices;
    void SpeechStream();
    void MusicNext();
    rr::sim::SoundSystemEnv System();
    rr::sim::Sound3DEnv Listener();
    const std::vector<int16_t>* Pcm(int32_t slot, uint32_t offset);
    uint8_t* At(uint32_t address);
    void ResetWorld();
    bool LoaderSound() const;   // sound_loader.cpp (the race loader's set-up)
    bool LoaderSetUp(rr::sim::SoundMachine& sm); // sound_loader.cpp: SoundSpuAttr, SoundInit, AudioReset, SoundLoad PORTED
    void W32(uint32_t address, uint32_t v);
    uint32_t R32(uint32_t address) const;
    void CopyGameState();

    struct Bank {
        std::vector<uint8_t> record;  // the Bank container, as in the file
        std::vector<uint8_t> samples; // raw SPU-ADPCM, 16-byte blocks
    };
    std::vector<Bank> banks_;                      // the eight file entries
    std::vector<uint8_t> bankDirectory_;           // the file's first 96 bytes (the race loader reads them)
    uint32_t loaderSpeechBytes_ = 0;
    std::string loaderLine_;
    bool loaderRan_ = false;
    // the SPU heap PORTED (sound_loader.cpp)
    bool spuHeapPorted_ = false;
    size_t spuMallocs_ = 0, spuMallocFails_ = 0, spuFrees_ = 0;
    uint32_t SpuHeapMalloc(uint32_t bytes); // SpuMalloc SLUS 0x8004F3C8 on the arena: the address, or >= 0x80000000
    void SpuHeapFree(uint32_t address);     // SpuFree SLUS 0x8004F998 on the arena
    bool SpuHeapMusic(uint32_t ring[2]);    // MusicSpuAlloc SLUS 0x800210C4: the two rings' buffers
    std::vector<rr::sim::SoundBankRef> bankSlots_; // the registered slots, bankCount + 1 of them
    std::vector<int32_t> bankSource_;              // which file entry each slot holds, or -1
    std::vector<uint32_t> bankSpuBase_;            // where each slot's samples sit in SPU RAM
    std::vector<uint8_t> arena_;                   // 2 MiB, see the file header (own-arena modes)
    uint8_t* ram_ = nullptr;                       // arena_ or the host's (world mode)
    uint32_t countdownStarted_ = 0;                // the handle 0x800164B4 stored (0: none)
    bool attached_ = false;
    uint32_t heap_ = 0, heapBytes_ = 0;
    // where the blocks the original reaches through a pointer are, in either mode
    uint32_t voicesAt_ = 0, listenersAt_ = 0, engineAt_ = 0, bankTableAt_ = 0, bankAt_ = 0, slotsAt_ = 0;
    std::vector<uint8_t> exe_;
    std::vector<uint8_t> music_;                   // the streamed track: lead-in, then the album
    uint32_t musicNext_[2] = {0, 0};               // the next 16 KiB unit each ring takes its half of
    int musicTrack_ = -1;
    uint64_t musicUnits_ = 0;
    FileRead musicRead_;
    bool musicEnded_ = false;                      // 0x80024EF4 found no track: the stream stops
    int musicChained_ = 0;
    std::mutex musicMutex_;                        // music_ / musicNext_: the render thread refills
    // the riders' voices
    FileRead speechRead_;
    PlaceFn speechPlace_;
    uint32_t speechFileBytes_ = 0;
    struct SpeechRequest { uint32_t offset, count; };
    std::vector<SpeechRequest> speechPending_;
    struct SpuBlock { uint32_t address, bytes; };
    std::vector<SpuBlock> spuHeap_;                // our SPU heap for the speech banks
    std::vector<uint8_t> speechRecord_;            // the record being delivered (0x4000 + 0x20)
    bool streamBank_ = false;                       // SpeechBankLoad runs on a stream chunk (UploadSamples' source)
    size_t streamBanksLoaded_ = 0, streamBanksDropped_ = 0; // type-10 chunks of the CD streamer
    std::function<bool(uint32_t)> speechRelease_;   // during StreamSpeechDone: the table record's release
    uint32_t speechMalloc_ = 0;
    bool speechStarted_ = false;
    size_t speechCalls_ = 0, speechLines_ = 0, speechPushes_ = 0;
    std::string speechFault_;
    std::shared_ptr<rr::audio::SpuVoices> spu_;
    std::unique_ptr<Io> io_;
    std::unique_ptr<Effects> fx_;
    const uint8_t* gameState_ = nullptr;
    SoundEffectTarget* effectTarget_ = nullptr;
    const uint8_t* surface_ = nullptr;
    bool tables_ = false;
    std::map<uint64_t, std::vector<int16_t>> pcm_;
    bool outOfWindow_ = false;
    bool engineMode_ = false;
    bool engineSetUp_ = false;
    std::string engineFault_;
    size_t requested_ = 0, allocated_ = 0, programmed_ = 0;
    std::vector<Played> played_;
    size_t animSoundCalls_ = 0;
    size_t engineFrames_ = 0, vsyncs_ = 0, spuWrites_ = 0, effectsNotRun_ = 0;
    uint32_t timer_ = 0; // the product's root counter 2
    bool counterPending_ = false; // ReadRootCounterAs: the next read answers counterValue_
    uint16_t counterValue_ = 0;
    uint16_t spuControl_[16] = {};
};

} // namespace rr::game
