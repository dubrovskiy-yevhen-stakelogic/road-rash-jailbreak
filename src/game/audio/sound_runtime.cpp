#include "game/audio/sound_runtime.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "game/sim/road_query.h"
#include "game/sim/sound_engine.h"
#include "game/sim/sound_frame.h"
#include "game/sim/takedown.h"
#include "game/sim/race_over.h" // RaceOverSignal SLUS 0x80018C1C
#include "game/sim/pause.h"     // SoundHold SLUS 0x80020E30
#include "game/sim/countdown_voice.h" // the countdown's voice SLUS 0x800164B4 / 0x80016528
#include "game/shell/handover.h"      // the options' volume sliders from the front end
#include "rrformats/audio.h"

namespace rr::game {
namespace {

constexpr uint32_t kRamBytes = 2u * 1024u * 1024u;
constexpr uint32_t kStateBytes = 0x188;
constexpr uint32_t kVoiceStride = 44;
constexpr uint32_t kVoiceCount = 24;   // `li s0,24` at 0x8001E664
constexpr uint32_t kReservedSlots = 22; // `li v1,21` at 0x8001FBD8
constexpr uint32_t kRingSlots = 25;     // `li v1,24` at 0x8001FC1C
constexpr uint32_t kBankCount = 12;     // the option-list value in every capture
constexpr uint32_t kListenerStride = 72;

// ---- the arena. The original's own addresses where the code reaches a fixed address:
constexpr uint32_t kGp = rr::sim::kSoundGp;            // 0x8005AC8C, SLUS_010.53's $gp
constexpr uint32_t kSys = rr::sim::kSoundSystem;       // 0x800D6870
constexpr uint32_t kExeBase = 0x80010000;              // SLUS_010.53's text base (file offset 0x800)
// ... and ours where the original reaches through a pointer. All inside the 2 MiB, clear of the EXE
// image (which ends at 0x8005B800) and of the fixed BSS words above (0x800D6870..0x800DC0B0).
// The voice table gets 2 KiB although it is 24 x 44 = 1056 bytes: a handle names its voice by
// `handle >> 27`, 0..31, so the original reads records 24..31 PAST the table (the -1 handles the
// adjacency arm of EngineStart leaves behind do exactly that); here they read zeroes, not a neighbour.
constexpr uint32_t kVoicesAt = 0x80100000;    // 24 x 44, S+0x0C, and 8 phantom records of zeroes
constexpr uint32_t kListenersAt = 0x80100800; // 4 x 72, *(gp+1920) - the emitter views four
constexpr uint32_t kEngineAt = 0x80100C00;    // 2 x 132, *(gp+1952)
constexpr uint32_t kBankTableAt = 0x80100E00; // 13 slots, S+0x04
constexpr uint32_t kBankAt = 0x80101000;      // resident bank records, 4 KiB apart
constexpr uint32_t kBikeAt = 0x80110000;      // the player's bike entity (1096 bytes)
constexpr uint32_t kOwnerAt = 0x80110800;     // the object at bike+0x354
constexpr uint32_t kStatsAt = 0x80111000;     // the stat block at bike+0x22C
constexpr uint32_t kGameStateAt = 0x80112000; // *(0x8005B2F8)
constexpr uint32_t kGameStateBytes = 0x60;
constexpr uint32_t kFrameSp = 0x801F0000;     // the stack pointer EngineNote is "called" at
constexpr uint32_t kSpuFirstBase = 0x1010;    // where every capture's first resident bank sits
// The world mode's heap layout (offsets into the host's block), the same relative placement.
constexpr uint32_t kHeapVoices = 0x0000, kHeapListeners = 0x0800, kHeapSlots = 0x0A00, kHeapEngine = 0x0C00;
constexpr uint32_t kHeapBankTable = 0x0E00, kHeapBanks = 0x1000, kHeapBytes = 0x4000;
// The riders' voices: the nine slots' 0x38-byte header blocks (SLUS 0x8001447C's, 0x40
// apart as the capture's malloc leaves them) and the staged head of the record being delivered, in the
// room the three resident bank records leave (their records end at 0x3074).
constexpr uint32_t kHeapSpeechBlocks = 0x3100, kHeapSpeechEnd = 0x3400, kHeapStaging = 0x3400;
// Our SPU heap for the speech banks: from the end of the music's second ring (0x534E0 + 64 KiB) to the
// top - the capture's six speech banks sit at 0x634E0 + 0x3FC0 k, exactly where first-fit puts them.
constexpr uint32_t kSpuSpeechBase = 0x634E0, kSpuSpeechEnd = 0x80000;
constexpr uint32_t kRecordBytes = 0x4000;
// The render camera the listener faces: SLUS 0x8002F17C (not ported) fills it every frame and
// copies the view record's +0x2E8 into its +0x7C (0x8002F2C4..0x8002F2CC); *(0x8005AEC0) points at
// it, 0x800D82B0 in every capture.
constexpr uint32_t kCameraPtr = 0x8005AEC0, kCameraAt = 0x800D82B0;

uint32_t ReadU32(const std::vector<uint8_t>& v, size_t at) {
    if (at + 4 > v.size()) return 0;
    return static_cast<uint32_t>(v[at]) | (static_cast<uint32_t>(v[at + 1]) << 8) |
           (static_cast<uint32_t>(v[at + 2]) << 16) | (static_cast<uint32_t>(v[at + 3]) << 24);
}
uint16_t ReadU16(const uint8_t* p) {
    return static_cast<uint16_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8));
}

} // namespace

// ---------------------------------------------------------------------------- the hardware
//
// What the ported engine code reaches through `rr::sim::SoundIo`, answered by the product:
//   * the SPU's register window 0x1F801C00..0x1F801DFF: every store goes to the SPU voice model
//     (mixer.h), and the control block 0x1F801D80..0x1F801D9B is also kept as a register file so
//     libspu's read-modify-write of EON/PMON reads back what was written - the same rule the bench's
//     register file follows;
//   * root counter 2 (0x1F801120), which EngineNote reads twice a frame for its jitter. The console
//     answers from a free-running hardware timer; the product answers from a 32-bit LCG of its own
//     (a*0x41C64E6D + 0x3039, the high half), so a run is deterministic and the jitter has the
//     timer's spread rather than a constant's. That generator is OURS and is not the game's LCG.
class SoundRuntime::Io final : public rr::sim::SoundIo {
public:
    explicit Io(SoundRuntime& r) : r_(r) {}
    bool Load16(uint32_t address, uint16_t& out) override {
        const uint32_t reg = address & 0x1FFFFFFFu;
        if (reg >= 0x1F801D80u && reg < 0x1F801D9Cu) {
            out = r_.spuControl_[(reg - 0x1F801D80u) >> 1];
            return true;
        }
        if (reg == 0x1F801120u && r_.counterPending_) { // the console's counter at this moment (root_counter.h)
            r_.counterPending_ = false;
            out = r_.counterValue_;
            r_.timer_ ^= static_cast<uint32_t>(r_.counterValue_) << 16;
            return true;
        }
        if (reg == 0x1F801100u || reg == 0x1F801110u || reg == 0x1F801120u) {
            r_.timer_ = r_.timer_ * 0x41C64E6Du + 0x3039u;
            out = static_cast<uint16_t>(r_.timer_ >> 16);
            return true;
        }
        return false;
    }
    bool Store16(uint32_t address, uint16_t value) override {
        const uint32_t reg = address & 0x1FFFFFFFu;
        if (reg < 0x1F801C00u || reg >= 0x1F801E00u) return false;
        if (reg >= 0x1F801D80u && reg < 0x1F801D9Cu) r_.spuControl_[(reg - 0x1F801D80u) >> 1] = value;
        r_.spu_->Write(reg - 0x1F801C00u, value);
        ++r_.spuWrites_;
        return true;
    }

private:
    SoundRuntime& r_;
};

// RoadNote's two effect spawners are not sound: they are PORTED (spine.h EffectBurst / EffectSpray)
// and run by the host on the race's own arena, where the effect records and the bike's budget word
// live (`SetEffectTarget`). The bike RoadNote passes is this arena's copy of player 0's (kBikeAt);
// anything else - or no target - is counted and not run.
class SoundRuntime::Effects final : public rr::sim::EngineEffects {
public:
    explicit Effects(SoundRuntime& r) : r_(r) {}
    void Burst(uint32_t bike, int32_t kind, int32_t life, int32_t tag) override {
        if (r_.effectTarget_ != nullptr && Mine(bike)) r_.effectTarget_->Burst(Other(bike), kind, life, tag);
        else ++r_.effectsNotRun_;
    }
    void Spray(uint32_t bike, uint32_t, int32_t kind) override {
        if (r_.effectTarget_ != nullptr && Mine(bike)) r_.effectTarget_->Spray(Other(bike), kind);
        else ++r_.effectsNotRun_;
    }

private:
    // the target's bike: 0 for player 0's (the target's own), player 1's world address otherwise
    uint32_t Other(uint32_t bike) const {
        return (r_.attached_ && bike != r_.R32(rr::sim::kSoundPlayerBikes)) ? bike : 0u;
    }
    // a player's bike: our copy of player 0's in the own-arena mode; in the world mode the world's own, player 1's
    // too in a two-player race (RoadNote runs per player's engine record)
    bool Mine(uint32_t bike) const {
        if (!r_.attached_) return bike == kBikeAt;
        if (bike == 0u) return false;
        if (bike == r_.R32(rr::sim::kSoundPlayerBikes)) return true;
        const uint32_t gs = r_.R32(0x8005B2F8u);
        return gs != 0u && r_.R32(gs + 0x30u) == 2u && bike == r_.R32(rr::sim::kSoundPlayerBikes + 4u);
    }
    SoundRuntime& r_;
};

// The speech code's callees in the product (speech.h). PORTED elsewhere and run by the race: ComputePlace
// and AiPushCommand. OURS, named: the heap block of SLUS 0x8001447C (the sound heap's speech blocks), the
// CD streamer SLUS 0x80023148 (the request is kept and the records are read from the disc at the start
// of the next Frame), the SPU heap and DMA of SLUS 0x8001E938 (our first-fit heap over the capture's
// region, the samples copied into the SPU model's RAM, the transfer complete at once) and SpuFree
// SLUS 0x8004F998; the stream buffer release SLUS 0x80030FA0 has nothing to release here.
class SoundRuntime::Voices final : public rr::sim::SpeechCallees {
public:
    explicit Voices(SoundRuntime& r) : r_(r) {}
    const PushFn* push = nullptr;
    struct Callback {
        uint32_t fn, arg;
    };
    std::vector<Callback> done; // the uploads' completion callbacks

    bool ComputePlace(uint32_t bike, int32_t mode, uint32_t& v0) override {
        v0 = 0;
        return r_.speechPlace_ && r_.speechPlace_(bike, mode, v0);
    }
    bool PushCommand(uint32_t cmd, int32_t mode, uint32_t e) override {
        if (push == nullptr || !*push) return false;
        ++r_.speechPushes_;
        return (*push)(cmd, mode, e);
    }
    bool Malloc(uint32_t bytes, uint32_t, uint32_t& v0) override {
        v0 = 0; // out of room: 0, as the original's allocator answers
        if (bytes == 0 || bytes > 0x40u || r_.speechMalloc_ + 0x40u > kHeapSpeechEnd) return true;
        v0 = r_.heap_ + r_.speechMalloc_;
        r_.speechMalloc_ += 0x40u;
        return true;
    }
    bool StreamRequest(uint32_t st, uint32_t n, uint32_t, uint32_t, uint32_t& v0) override {
        const uint32_t off = r_.R32(st + 4u);
        r_.speechPending_.push_back(SpeechRequest{off, n});
        r_.W32(st + 4u, off + kRecordBytes * n); // 0x800231F0: the stream moves on one record per request
        v0 = 1;
        return true;
    }
    bool UploadSamples(uint32_t bank, uint32_t src, uint32_t cb, uint32_t arg, uint32_t, uint32_t& v0) override {
        v0 = 0xFFFFFFFFu; // SpuMalloc 0x8004F3C8 < 0: no SPU memory
        const bool staged = src == r_.heap_ + kHeapStaging + 0x60u && r_.speechRecord_.size() >= 0x60u;
        if (!staged && !r_.streamBank_) return false;
        uint32_t bytes = r_.R32(bank + 0x0Cu);
        uint32_t at = kSpuSpeechBase;
        if (r_.spuHeapPorted_) { // PORTED SpuMalloc SLUS 0x8004F3C8 on the arena's table (sound_loader.cpp)
            at = r_.SpuHeapMalloc(bytes);
            if (static_cast<int32_t>(at) < 0) return true;
            constexpr uint32_t kTop = static_cast<uint32_t>(rr::audio::SpuVoices::kRamBytes);
            if (at + bytes > kTop) bytes = at < kTop ? kTop - at : 0u;
        } else {                 // RRJB_SPUHEAP=off: our first fit
        size_t k = 0;
        for (; k < r_.spuHeap_.size(); ++k) {
            if (at + bytes <= r_.spuHeap_[k].address) break;
            at = r_.spuHeap_[k].address + r_.spuHeap_[k].bytes;
        }
        if (bytes == 0 || at + bytes > kSpuSpeechEnd) return true;
        r_.spuHeap_.insert(r_.spuHeap_.begin() + static_cast<ptrdiff_t>(k), SpuBlock{at, bytes});
        }
        // the transfer: sampleBytes from the record's +0x60 - 0x20 more than the record holds, which on
        // the console come from whatever follows it in the cache; here the file's next bytes
        if (staged) {
            const size_t have = std::min<size_t>(bytes, r_.speechRecord_.size() - 0x60u);
            std::memcpy(r_.spu_->Ram().data() + at, r_.speechRecord_.data() + 0x60, have);
        } else { // a streamed bank: from the chunk's table buffer in guest RAM, as the DMA reads it
            for (uint32_t b = 0; b < bytes; ++b) r_.spu_->Ram()[at + b] = r_.ram_[(src + b) & 0x1FFFFFu];
        }
        done.push_back(Callback{cb, arg});
        v0 = at;
        return true;
    }
    bool SpuFree(uint32_t addr) override {
        if (r_.spuHeapPorted_) { // PORTED SpuFree SLUS 0x8004F998
            r_.SpuHeapFree(addr);
            return true;
        }
        for (size_t k = 0; k < r_.spuHeap_.size(); ++k)
            if (r_.spuHeap_[k].address == addr) {
                r_.spuHeap_.erase(r_.spuHeap_.begin() + static_cast<ptrdiff_t>(k));
                break;
            }
        return true;
    }
    bool ReleaseBuffer(uint32_t index) override { return r_.speechRelease_ ? r_.speechRelease_(index) : true; }

private:
    SoundRuntime& r_;
};

SoundRuntime::SoundRuntime()
    : arena_(kRamBytes, 0), spu_(std::make_shared<rr::audio::SpuVoices>()),
      io_(std::make_unique<Io>(*this)), fx_(std::make_unique<Effects>(*this)) {
    ram_ = arena_.data();
    voicesAt_ = kVoicesAt;
    listenersAt_ = kListenersAt;
    engineAt_ = kEngineAt;
    bankTableAt_ = kBankTableAt;
    bankAt_ = kBankAt;
    slotsAt_ = 0;
}
SoundRuntime::~SoundRuntime() = default;

uint8_t* SoundRuntime::At(uint32_t address) { return ram_ + (address & (kRamBytes - 1u)); }
void SoundRuntime::W32(uint32_t address, uint32_t v) { std::memcpy(At(address), &v, 4); }
uint32_t SoundRuntime::R32(uint32_t address) const {
    uint32_t v = 0;
    std::memcpy(&v, ram_ + (address & (kRamBytes - 1u)), 4);
    return v;
}

void SoundRuntime::Attach(uint8_t* hostRam, uint32_t heap, uint32_t heapBytes) {
    if (hostRam == nullptr || heapBytes < kHeapBytes) return;
    attached_ = true;
    ram_ = hostRam;
    heap_ = heap;
    heapBytes_ = heapBytes;
    voicesAt_ = heap + kHeapVoices;
    listenersAt_ = heap + kHeapListeners;
    slotsAt_ = heap + kHeapSlots;
    engineAt_ = heap + kHeapEngine;
    bankTableAt_ = heap + kHeapBankTable;
    bankAt_ = heap + kHeapBanks;
}
uint32_t SoundRuntime::ArenaWord(uint32_t address) const { return R32(address); }

// ---------------------------------------------------------------------------- DATA\RASHNZ_E.DAT
//
// `struct { u32 offset; u32 bankBytes; u32 sampleBytes; } entry[8];` at 0, then at
// `96 + entry[i].offset` the bank record followed by its raw SPU-ADPCM (docs\formats\audio.md). The
// directory closing exactly on the file size is the check, and a file that fails it is rejected
// rather than half-read.
bool SoundRuntime::LoadBanks(std::span<const uint8_t> file, std::string& error) {
    banks_.clear();
    bankDirectory_.assign(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(file.size(), 96)));
    if (file.size() < 96) {
        error = "RASHNZ_E.DAT is shorter than its own 96-byte directory";
        return false;
    }
    auto word = [&file](size_t at) {
        return static_cast<uint32_t>(file[at]) | (static_cast<uint32_t>(file[at + 1]) << 8) |
               (static_cast<uint32_t>(file[at + 2]) << 16) |
               (static_cast<uint32_t>(file[at + 3]) << 24);
    };
    size_t running = 0;
    std::vector<Bank> banks;
    for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t offset = word(12u * i + 0);
        const uint32_t bankBytes = word(12u * i + 4);
        const uint32_t sampleBytes = word(12u * i + 8);
        if (offset != running) {
            error = "RASHNZ_E.DAT entry " + std::to_string(i) + " does not follow the previous one";
            return false;
        }
        const size_t at = 96 + offset;
        if (at + bankBytes + sampleBytes > file.size()) {
            error = "RASHNZ_E.DAT entry " + std::to_string(i) + " runs past the end of the file";
            return false;
        }
        Bank b;
        b.record.assign(file.begin() + static_cast<ptrdiff_t>(at),
                        file.begin() + static_cast<ptrdiff_t>(at + bankBytes));
        b.samples.assign(file.begin() + static_cast<ptrdiff_t>(at + bankBytes),
                         file.begin() + static_cast<ptrdiff_t>(at + bankBytes + sampleBytes));
        if (ReadU32(b.record, 0) != 2) {
            error = "RASHNZ_E.DAT entry " + std::to_string(i) + " is not a bank (magic != 2)";
            return false;
        }
        banks.push_back(std::move(b));
        running += bankBytes + sampleBytes;
    }
    if (96 + running != file.size()) {
        error = "RASHNZ_E.DAT's directory does not close on the file size";
        return false;
    }
    banks_ = std::move(banks);
    return true;
}

void SoundRuntime::SetExecutable(std::span<const uint8_t> slus) {
    exe_.assign(slus.begin(), slus.end());
    engineMode_ = exe_.size() > 0x800;
}

void SoundRuntime::Reset() {
    if (attached_) {
        ResetWorld();
        return;
    }
    std::fill(arena_.begin(), arena_.end(), uint8_t{0});
    spu_ = std::make_shared<rr::audio::SpuVoices>();
    std::memset(spuControl_, 0, sizeof(spuControl_));
    pcm_.clear();
    requested_ = allocated_ = programmed_ = 0;
    engineFrames_ = vsyncs_ = spuWrites_ = effectsNotRun_ = 0;
    outOfWindow_ = false;
    engineSetUp_ = false;
    engineFault_.clear();
    timer_ = 0;
    counterPending_ = false;

    // The EXE image first, in the engine mode: EngineSetup reads its parameter records
    // (0x800525F4), AudioReset its slider defaults (0x80052638), libspu its globals (the SPU base
    // pointer 0x8005A41C, the address alignment 0x8005A440..4C) and GetRCnt its counter base
    // (0x800549B8) - all initialised data of `SLUS_010.53`, all equal to the four captures.
    if (engineMode_) {
        const size_t n = std::min<size_t>(exe_.size() - 0x800, kRamBytes - (kExeBase & (kRamBytes - 1u)));
        std::memcpy(At(kExeBase), exe_.data() + 0x800, n);
    }

    // SoundInit + ResetSoundState + ResetVoiceLists, read instruction by instruction: 22 reserved
    // slots free, `free[i] = i` with `freeTop = 23`, a 25-slot ring of -1 and both ring indices at 0.
    std::memset(At(kSys), 0, kStateBytes);
    W32(kSys + 0x000, kBankCount);
    W32(kSys + 0x004, kBankTableAt);
    W32(kSys + 0x008, kVoiceCount);
    W32(kSys + 0x00C, kVoicesAt);
    W32(kSys + 0x180, 127); // the master volume, clamped to 0..127 by 0x8001EFAC
    W32(kSys + 0x184, 0);   // panning ON
    for (uint32_t i = 0; i < kReservedSlots; ++i) W32(kSys + 0x058 + 4u * i, 0xFFFFFFFFu);
    for (uint32_t i = 0; i < kVoiceCount; ++i) W32(kSys + 0x0B0 + 4u * i, i);
    for (uint32_t i = 0; i < kRingSlots; ++i) W32(kSys + 0x110 + 4u * i, 0xFFFFFFFFu);
    W32(kSys + 0x174, 23);
    // `SoundInit` writes each voice's own array index into +0x1C.
    for (uint32_t v = 0; v < kVoiceCount; ++v) W32(kVoicesAt + kVoiceStride * v + 0x1C, v);
    // The listener's default pan is 64 - centre - in every capture.
    for (uint32_t p = 0; p < 4; ++p) W32(kListenersAt + kListenerStride * p + 0x14, 64);

    // The resident banks, as every race capture has them: file entry 0 is
    // the default bank, entry 2 is bank 1 (the road bank), entry 3 is bank 2 (the engine bank of a
    // model < 9). Each record is copied into the arena and its samples into the SPU model's RAM,
    // one after the other from 0x1010 - which is exactly where the captures have them (bases
    // 0x1010, 0x2FB80, 0x40AC0) - and patched as `LoadBank` + `PatchBank SLUS 0x8001EAA4` patch
    // them: the bank's +0x08 and every descriptor's +0x08 get the base added.
    bankSlots_.assign(kBankCount + 1u, rr::sim::SoundBankRef{});
    bankSource_.assign(kBankCount + 1u, -1);
    bankSpuBase_.assign(kBankCount + 1u, 0);
    const int32_t resident[3] = {0, 2, 3};
    uint32_t spuNext = kSpuFirstBase;
    for (uint32_t slot = 0; slot < 3; ++slot) {
        const int32_t entry = resident[slot];
        if (static_cast<size_t>(entry) >= banks_.size()) continue;
        const Bank& b = banks_[static_cast<size_t>(entry)];
        const uint32_t addr = kBankAt + 0x1000u * slot;
        if (b.record.size() > 0x1000 || spuNext + b.samples.size() > rr::audio::SpuVoices::kRamBytes) continue;
        std::memcpy(At(addr), b.record.data(), b.record.size());
        std::memcpy(spu_->Ram().data() + spuNext, b.samples.data(), b.samples.size());
        uint8_t* rec = At(addr);
        W32(addr + 8, spuNext);
        const uint32_t count = rec[4];
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t o = R32(addr + 0x10u + 4u * i);
            if (o == 0 || o + 4u > b.record.size()) continue;
            const uint32_t descs = rec[o];
            for (uint32_t d = 0; d < descs && o + 16u + 12u * d <= b.record.size(); ++d)
                W32(addr + o + 12u + 12u * d, R32(addr + o + 12u + 12u * d) + spuNext);
        }
        bankSource_[slot] = entry;
        bankSpuBase_[slot] = spuNext;
        bankSlots_[slot].address = addr;
        bankSlots_[slot].data = At(addr);
        bankSlots_[slot].size = static_cast<uint32_t>(b.record.size());
        W32(kBankTableAt + 4u * slot, addr);
        spuNext = (spuNext + static_cast<uint32_t>(b.samples.size()) + 15u) & ~15u;
    }

    const uint32_t gp = kGp;
    if (engineMode_) {
        // The ported AudioReset, then the race loader's writes (not ported; the values of every
        // capture - file header).
        rr::sim::GuestRam g(arena_.data(), gp);
        rr::sim::SoundMachine s{g, ram_, *io_, fx_.get()};
        rr::sim::AudioReset(s);
        W32(0x8005ACA8u, R32(0x8005ACA8u) | 4u); // the audio gate
    } else {
        // Emitter-only: the two sliders the emitter reads, as measured in all four captures.
        W32(rr::sim::kEngineSlider, 41);
        W32(rr::sim::kEffectsSlider, 65); // the 3D master: a sound at zero distance reaches 64
    }
    W32(gp + rr::sim::kDefaultBankGp, 0);
    W32(gp + rr::sim::kRoadBankGp, 1);   // also PlaySound3D's "muted" bank: nothing passes 1
    W32(gp + rr::sim::kListenerGp, kListenersAt);
    W32(gp + rr::sim::kEngineRecGp, kEngineAt);
    W32(gp + rr::sim::kSerialGp, 0);
    std::memset(At(kEngineAt), 0, 2u * rr::sim::kEngineRecBytes);
    if (engineMode_) {
        // The race loader's per-player reset, PORTED (RASHCDI 0x80063448): the
        // engine record's handles start at -1, which is what keeps the first EngineNote from
        // starting the idle loop a second time.
        W32(0x8005B40Cu, kListenersAt);
        rr::sim::GuestRam g(arena_.data(), gp);
        rr::sim::SoundMachine s{g, ram_, *io_, fx_.get()};
        rr::sim::SoundRecordsInit(s, 0);
        rr::sim::SoundRecordsInit(s, 1);
    }
    for (uint32_t p = 0; p < 2; ++p) {
        W32(kEngineAt + rr::sim::kEngineRecBytes * p + 0x08, 2);            // the engine bank's slot
        W32(kEngineAt + rr::sim::kEngineRecBytes * p + 0x0C, kBankAt + 0x2000u); // its memory
    }
    W32(0x8005B2F8u, kGameStateAt);
    spu_->Write(0x180, 0x3FFF); // the main volume every capture holds (libspu's SpuInit is not ported)
    spu_->Write(0x182, 0x3FFF);
}

// ---------------------------------------------------------------------------- the world mode
//
// The sound state in the host's arena, laid out as the original lays it out, with every step that
// is ported run PORTED and every step that is not named:
//   * SLUS 0x8001E5A8 ResetSoundState -> 0x8001FBD4 ResetVoiceLists: PORTED;
//   * SLUS 0x8001E614 SoundInit: NOT ported - its two `malloc`s are ours (the heap block), the
//     layout is its own: 12 bank slots, 24 voices, each voice's +0x1C its own index;
//   * the three resident banks through SLUS 0x8001ED28 LoadBank: the SPU heap and the DMA upload
//     are ours (the samples go into the SPU model's RAM at the bases every capture has), the
//     registration and SLUS 0x8001EAA4 PatchBank are PORTED;
//   * SLUS 0x80019D9C AudioReset: PORTED;
//   * the race loader RASHCDI 0x800627F8: NOT ported - its writes are done with the values of every
//     one-player capture (default bank 0, road bank 1, engine bank slot 2, the siren's sound 0x17,
//     no cue bank, one listener record with 5 object slots and 3 cue slots, pan 64), its three
//     `malloc(...)+memset 0` are blocks of the heap; its leaf RASHCDI 0x80063448 SoundRecordsInit
//     is PORTED and run;
//   * libspu's SpuInit / SpuSetCommonAttr: NOT ported - the main volume is the 0x3FFF every
//     capture holds.
void SoundRuntime::ResetWorld() {
    spu_ = std::make_shared<rr::audio::SpuVoices>();
    std::memset(spuControl_, 0, sizeof(spuControl_));
    pcm_.clear();
    requested_ = allocated_ = programmed_ = 0;
    engineFrames_ = vsyncs_ = spuWrites_ = effectsNotRun_ = 0;
    outOfWindow_ = false;
    engineSetUp_ = false;
    engineFault_.clear();
    timer_ = 0;
    counterPending_ = false;
    std::memset(At(heap_), 0, heapBytes_);
    musicEnded_ = false;
    musicChained_ = 0;
    speechPending_.clear();
    spuHeap_.clear();
    speechStarted_ = false;
    speechCalls_ = speechLines_ = speechPushes_ = 0;
    speechFault_.clear();

    rr::sim::GuestRam g(ram_, kGp);
    rr::sim::SoundMachine s{g, ram_, *io_, fx_.get()};
    rr::sim::ResetSoundState(s);                                  // PORTED
    W32(kSys + 0x000, kBankCount);                                // SoundInit's layout, our blocks
    W32(kSys + 0x004, bankTableAt_);
    W32(kSys + 0x008, kVoiceCount);
    W32(kSys + 0x00C, voicesAt_);
    for (uint32_t v = 0; v < kVoiceCount; ++v) W32(voicesAt_ + kVoiceStride * v + 0x1C, v);

    bankSlots_.assign(kBankCount + 1u, rr::sim::SoundBankRef{});
    bankSource_.assign(kBankCount + 1u, -1);
    bankSpuBase_.assign(kBankCount + 1u, 0);
    loaderRan_ = false;
    spuHeapPorted_ = false; // LoaderSetUp runs SpuInitMalloc (sound_loader.cpp)
    if (LoaderSound()) { // the race loader (sound_loader.cpp): the set-up PORTED; what it does not write, named here
        W32(kGp + rr::sim::kCueBankGp, 0xFFFFFFFFu);   // (SoundLoad sets it for a police race only)
        W32(kGp + rr::sim::kSirenSoundGp, 0x17);       // the siren's sound index as every capture holds it: SirenSet SLUS
                                                       // 0x80018DC8 (SetUpRace's) writes it - PORTED, loader2; the session
                                                       // puts its value back after this reset (race_session.cpp)
        if (LoaderSetUp(s)) {
            static const uint32_t kSliders[7] = {41, 29, 29, 65, 65, 49, 24}; // the options' sliders (named below)
            const rr::shell::Handover& fh = rr::shell::PendingHandover(); // a race the front end started: the shell's
            const bool shell = fh.active && fh.sliders;                   // (VolumeSet RASHCDF 0x8007F20C wrote them)
            for (uint32_t k = 0; k < 7; ++k) {
                W32(rr::sim::kEngineSlider + 4u * k, shell ? fh.volume[k] : kSliders[k]);
                W32(0x800D6C20u + 4u * k, shell ? fh.volumeSaved[k] : kSliders[k]);
            }
            W32(0x8005ACA8u, R32(0x8005ACA8u) | 4u);    // the audio gate
            if (R32(kCameraPtr) == 0) W32(kCameraPtr, kCameraAt);
            if (s.Faulted()) engineFault_ = "the world-mode reset faulted";
            return;
        }
        bankSlots_.assign(kBankCount + 1u, rr::sim::SoundBankRef{}); // refused: the named layout below
        bankSource_.assign(kBankCount + 1u, -1);
        bankSpuBase_.assign(kBankCount + 1u, 0);
        spuHeap_.clear();
        for (uint32_t k = 0; k <= kBankCount; ++k) W32(bankTableAt_ + 4u * k, 0);
    }
    const int32_t resident[3] = {0, 2, 3};
    uint32_t spuNext = kSpuFirstBase;
    for (uint32_t slot = 0; slot < 3; ++slot) {
        const int32_t entry = resident[slot];
        if (static_cast<size_t>(entry) >= banks_.size()) continue;
        const Bank& b = banks_[static_cast<size_t>(entry)];
        const uint32_t addr = bankAt_ + 0x1000u * slot;
        if (b.record.size() > 0x1000 || spuNext + b.samples.size() > rr::audio::SpuVoices::kRamBytes) continue;
        std::memcpy(At(addr), b.record.data(), b.record.size());
        std::memcpy(spu_->Ram().data() + spuNext, b.samples.data(), b.samples.size());
        W32(addr + 8, spuNext);                                   // LoadBank 0x8001ED5C
        rr::sim::PatchBank(s, addr, spuNext);                     // PORTED
        W32(bankTableAt_ + 4u * slot, addr);
        bankSource_[slot] = entry;
        bankSpuBase_[slot] = spuNext;
        bankSlots_[slot].address = addr;
        bankSlots_[slot].data = At(addr);
        bankSlots_[slot].size = static_cast<uint32_t>(b.record.size());
        spuNext = (spuNext + static_cast<uint32_t>(b.samples.size()) + 15u) & ~15u;
    }

    const uint32_t gp = kGp;
    if (engineMode_) rr::sim::AudioReset(s);                      // PORTED
    // The seven volume sliders as the player's options leave them: AudioReset derives
    // 62 45 45 99 99 74 37 from the executable's defaults, the options (the shell and the memory
    // card, not ported) scale them, and every race capture holds 41 29 29 65 65 49 24 - engine,
    // object engines, claimed sounds, the 3D master, cues, music, and [6] - in both copies.
    // A race the front end started: the shell's own sliders (handover.h; VolumeSet RASHCDF 0x8007F20C wrote them).
    static const uint32_t kOptionSliders[7] = {41, 29, 29, 65, 65, 49, 24};
    const rr::shell::Handover& fh = rr::shell::PendingHandover();
    const bool shellSliders = fh.active && fh.sliders;
    for (uint32_t k = 0; k < 7; ++k) {
        W32(rr::sim::kEngineSlider + 4u * k, shellSliders ? fh.volume[k] : kOptionSliders[k]);
        W32(0x800D6C20u + 4u * k, shellSliders ? fh.volumeSaved[k] : kOptionSliders[k]);
    }
    W32(0x8005ACA8u, R32(0x8005ACA8u) | 4u);                      // the audio gate
    W32(gp + rr::sim::kDefaultBankGp, 0);
    W32(gp + rr::sim::kRoadBankGp, 1);
    W32(gp + rr::sim::kCueBankGp, 0xFFFFFFFFu);
    W32(gp + rr::sim::kSirenSoundGp, 0x17);
    W32(gp + rr::sim::kListenerGp, listenersAt_);
    W32(gp + rr::sim::kObjSlotsGp, slotsAt_);
    W32(gp + rr::sim::kEngineRecGp, engineAt_);
    const uint32_t gsp = R32(0x8005B2F8u);
    if (gsp != 0 && R32(gsp + 0x30u) == 2) {                      // two players, RASHCDI 0x800629E0..0x80062A14
        W32(listenersAt_ + 0x5C, 127);                            // listener 1 +0x14 (72-byte stride)
        W32(listenersAt_ + 0x14, 0);
        W32(listenersAt_ + 0x24, 3);
        W32(listenersAt_ + 0x28, 2);
        W32(listenersAt_ + 0x6C, 3);
        W32(listenersAt_ + 0x70, 2);
        rr::sim::SoundRecordsInit(s, 0);                          // PORTED, RASHCDI 0x80063448, once per player
        rr::sim::SoundRecordsInit(s, 1);
    } else {
    W32(listenersAt_ + 0x14, 64);                                 // RASHCDI 0x8006296C..0x8006297C
    W32(listenersAt_ + 0x24, 5);
    W32(listenersAt_ + 0x28, 3);
    rr::sim::SoundRecordsInit(s, 0);                              // PORTED, RASHCDI 0x80063448
    }
    W32(engineAt_ + 0x08, 2);                                     // RASHCDI 0x80062C20: the LoadBank slot
    W32(engineAt_ + 0x0C, bankAt_ + 0x2000u);                     // RASHCDI 0x80062AD0: its memory
    if (R32(kCameraPtr) == 0) W32(kCameraPtr, kCameraAt);
    spu_->Write(0x180, 0x3FFF);
    spu_->Write(0x182, 0x3FFF);
    if (s.Faulted()) engineFault_ = "the world-mode reset faulted";
}

bool SoundRuntime::Frame(uint32_t sp) {
    if (!attached_ || !engineMode_ || bankSlots_.empty() || !engineFault_.empty()) return false;
    rr::sim::GuestRam g(ram_, kGp);
    rr::sim::SoundMachine s{g, ram_, *io_, fx_.get()};
    if (!engineSetUp_) {
        rr::sim::EngineSetup(s);                                  // PORTED, SLUS 0x8001654C
        engineSetUp_ = true;
        countdownStarted_ = rr::sim::CountdownVoiceStart(s);      // PORTED, SLUS 0x800164B4 (called from 0x80011BF8)
    }
    if (!speechPending_.empty()) SpeechStream();                  // the taunt records arrive
    // SLUS 0x8002F2C4..0x8002F2CC: the render camera's +0x7C is the view record's +0x2E8 (the rest
    // of 0x8002F17C, the render camera, is not ported and nothing else here reads it)
    const uint32_t cam = R32(kCameraPtr);
    uint16_t yaw = 0;
    std::memcpy(&yaw, At(rr::sim::kViewRecords + 0x2E8), 2);
    std::memcpy(At(cam + 0x7C), &yaw, 2);
    rr::sim::AudioFrame(s, sp);                                   // PORTED, SLUS 0x80018FAC
    ++engineFrames_;
    MusicNext();
    if (s.Faulted()) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s at 0x%08X (AudioFrame)", s.failWhat ? s.failWhat : "a guest fault",
                      s.failed ? s.failAddress : g.FaultAddress());
        engineFault_ = buf;
        return false;
    }
    return true;
}

int SoundRuntime::ObjectSlotsBusy(int* withVoice) const {
    int busy = 0, on = 0;
    if (attached_) {
        for (uint32_t k = 0; k < 8; ++k) {
            const uint32_t e = slotsAt_ + 44u * k;
            uint16_t id = 0;
            std::memcpy(&id, ram_ + (e & (kRamBytes - 1u)), 2);
            if (id == 0xE0) continue;
            ++busy;
            if (R32(e + 4) != 0) ++on;
        }
    }
    if (withVoice) *withVoice = on;
    return busy;
}

// ---------------------------------------------------------------------------- the music
//
// PORTED: the track pick (MusicPickShuffle SLUS 0x80024B20), the two stream voices
// (StreamVoiceStart SLUS 0x8001F37C, KeyOnHandles SLUS 0x8001F544) with MusicStart SLUS
// 0x800212FC's arguments (pitch 0x5CE = 16000 Hz, the music slider 0x800D6C14, pan 0 / 127, the
// rings 0x434E0 / 0x534E0 of every capture). NOT ported, named: the CD reads of the stream player
// (0x80024630 / 0x800247E8 / 0x80024CB4 / 0x80023148) and its SPU-IRQ refill 0x8002169C - the
// rings are refilled here, chunk by chunk as the voices leave them, from the same bytes in the same
// order (the lead-in's 8 units, then the album from start + skip * 0x4000), and the rings' loop
// flags (the first block's loop start, the last block's loop end + repeat) are ours; the pause
// state 0x800D7549 (the voices start at pitch 0 until 0x80020E30 releases them) - they start at the
// playing pitch. The next track when this one ends: MusicNext below.
bool SoundRuntime::StartMusic(const FileRead& read) {
    if (!attached_ || !engineMode_ || !engineFault_.empty() || !read) return false;
    // the rings' SPU buffers: the PORTED MusicSpuAlloc SLUS 0x800210C4 on the heap (sound_loader.cpp), after the
    // banks and before the speech banks as the captures have them; RRJB_SPUHEAP=off: every capture's 0x434E0 / 0x534E0
    uint32_t kRing[2] = {0x434E0u, 0x534E0u};
    if (spuHeapPorted_ && !SpuHeapMusic(kRing)) return false;
    rr::sim::GuestRam g(ram_, kGp);
    rr::sim::SoundMachine s{g, ram_, *io_, fx_.get()};
    const uint32_t track = rr::sim::MusicPickShuffle(s);                   // PORTED
    if (track >= 18u) return false;
    W32(rr::sim::kMusicTrackGp, track);
    musicTrack_ = static_cast<int>(track);
    const uint32_t start = R32(rr::sim::kAlbumTable + 8u + 12u * track);
    const uint32_t length = R32(rr::sim::kAlbumTable + 12u + 12u * track);
    const int32_t skip = static_cast<int8_t>(*At(0x8005365Cu + track));    // 0x80024DA8
    static constexpr uint32_t kUnit = 0x4000, kLeadIn = 0x20000;
    const uint32_t albumFrom = start + static_cast<uint32_t>(skip) * kUnit;
    const uint32_t albumBytes = (albumFrom < start + length) ? start + length - albumFrom : 0u;
    {
        std::lock_guard<std::mutex> lock(musicMutex_);
        music_.assign(kLeadIn + albumBytes, 0);
        if (!read(1, track * kLeadIn, music_.data(), kLeadIn)) return false;
        if (albumBytes != 0 && !read(0, albumFrom, music_.data() + kLeadIn, albumBytes)) return false;
        musicNext_[0] = musicNext_[1] = 0;
        musicUnits_ = 0;
    }
    musicRead_ = read;
    musicEnded_ = false;

    static constexpr uint32_t kChunk = 0x2000;
    static constexpr int kChunks = 8;
    auto fill = [this](int ring, int chunk, uint8_t* dst, uint32_t bytes) {
        std::lock_guard<std::mutex> lock(musicMutex_);
        const uint32_t unit = musicNext_[ring]++;
        const size_t at = static_cast<size_t>(unit) * kUnit + static_cast<size_t>(ring) * kChunk;
        if (at + bytes <= music_.size()) std::memcpy(dst, music_.data() + at, bytes);
        else std::memset(dst, 0, bytes); // past the stream's end: silence (only when no next track came)
        if (ring == 0) ++musicUnits_;
        for (uint32_t b = 0; b < bytes; b += 16) dst[b + 1] = 0;
        if (chunk == 0) dst[1] = 4;                          // the ring's loop start
        if (chunk == kChunks - 1) dst[bytes - 16 + 1] = 3;   // the ring's loop end + repeat
    };
    spu_->SetStreamRing(0, kRing[0], kChunk, kChunks, fill);
    spu_->SetStreamRing(1, kRing[1], kChunk, kChunks, fill);

    const uint32_t handles = heap_ + kHeapBytes - 0x100u;
    for (uint32_t c = 0; c < 2; ++c) {
        rr::sim::SoundParams prm;
        prm.pitch = static_cast<int32_t>((16000u << 12) / 44100u);        // 0x800D7550 = 0x5CE
        prm.volume = static_cast<int32_t>(R32(0x800D6C14u));              // the music slider
        prm.pan = c ? 127 : 0;
        W32(handles + 4u * c, rr::sim::StreamVoiceStart(s, 0, prm, kRing[c])); // PORTED
    }
    rr::sim::KeyOnHandles(s, 2, handles);                                  // PORTED
    return !s.Faulted();
}

// The end of a track (0x80024CB4): when the album stream has no bytes left it calls 0x80024EF4(-1),
// whose PORTED MusicPickShuffle picks the next track, and the stream goes on from that track's start to
// its end (0x80024AE8: table +8 start, + length) - no INTRO.ALB lead-in and no skip, those are the
// race start's (0x80024DA8). No selectable track: the stream stops (0x800CD694 = 0, 0x80020DB8).
// OURS: the moment - the pick is made when four 16 KiB units (about 3.6 s) are left rather than at the
// last byte, because the refill runs on the render thread and the disc is read here; and the units both
// rings have passed are dropped so the buffer does not grow over a long race.
void SoundRuntime::MusicNext() {
    if (musicEnded_ || !musicRead_ || musicTrack_ < 0) return;
    {
        std::lock_guard<std::mutex> lock(musicMutex_);
        const size_t units = music_.size() / kRecordBytes;
        const size_t consumed = std::max(musicNext_[0], musicNext_[1]);
        if (consumed + 4u < units) return;
    }
    rr::sim::GuestRam g(ram_, kGp);
    rr::sim::SoundMachine s{g, ram_, *io_, fx_.get()};
    const uint32_t track = rr::sim::MusicPickShuffle(s);                   // PORTED, SLUS 0x80024B20
    W32(rr::sim::kMusicTrackGp, track);                                    // 0x80024F10
    if (s.Faulted() || static_cast<int32_t>(track) < 0 || !(track < R32(rr::sim::kAlbumTable))) {
        musicEnded_ = true;
        return;
    }
    const uint32_t start = R32(rr::sim::kAlbumTable + 8u + 12u * track);
    const uint32_t length = R32(rr::sim::kAlbumTable + 12u + 12u * track);
    std::vector<uint8_t> next(length, 0);
    if (length == 0 || !musicRead_(0, start, next.data(), length)) {
        musicEnded_ = true;
        return;
    }
    std::lock_guard<std::mutex> lock(musicMutex_);
    uint32_t drop = std::min(musicNext_[0], musicNext_[1]);
    drop = drop > 1u ? drop - 1u : 0u;
    if (static_cast<size_t>(drop) * kRecordBytes > music_.size()) drop = static_cast<uint32_t>(music_.size() / kRecordBytes);
    music_.erase(music_.begin(), music_.begin() + static_cast<ptrdiff_t>(static_cast<size_t>(drop) * kRecordBytes));
    musicNext_[0] -= drop;
    musicNext_[1] -= drop;
    music_.insert(music_.end(), next.begin(), next.end());
    musicTrack_ = static_cast<int>(track);
    ++musicChained_;
}

// ---------------------------------------------------------------------------- the riders' voices
//
// PORTED (speech.h, rows_speech.inc): SpeechInit, SpeechBankLoad, SpeechSlotFind, LoadBank,
// BankFree, the upload callback and RiderSpeech. OURS, named: the race loader's open of AUDTAUNT.STR
// (RASHCDI, not ported) - the stream record's handle (1, as every capture) and size; the slots as the
// previous owner leaves them (bank -1, id -1, state 0: the capture's untouched slot 7 - the writer is
// not located); and the callees of `Voices` above.
bool SoundRuntime::StartSpeech(const FileRead& read, uint32_t fileBytes, const PlaceFn& place) {
    if (!attached_ || !engineMode_ || !engineFault_.empty() || !read) return false;
    speechRead_ = read;
    speechFileBytes_ = fileBytes;
    speechPlace_ = place;
    speechMalloc_ = kHeapSpeechBlocks;
    spuHeap_.clear();
    speechPending_.clear();
    if (!loaderRan_) { // otherwise the race loader's SoundLoad wrote them (the open, the size, the nine slots)
    W32(rr::sim::kSpeechFile + 0x00u, 1);
    W32(rr::sim::kSpeechFile + 0x10u, fileBytes);
    for (uint32_t i = 0; i < 9; ++i) {
        W32(rr::sim::kSpeechSlots + 32u * i + 0u, 0xFFFFFFFFu);
        W32(rr::sim::kSpeechSlots + 32u * i + 4u, 0xFFFFFFFFu);
        W32(rr::sim::kSpeechSlots + 32u * i + 8u, 0);
    }
    }
    rr::sim::GuestRam g(ram_, kGp);
    rr::sim::SoundMachine s{g, ram_, *io_, fx_.get()};
    Voices v(*this);
    rr::sim::SpeechInit(s, v);                                             // PORTED, SLUS 0x8001A424
    if (s.Faulted()) {
        speechFault_ = "SpeechInit faulted";
        return false;
    }
    speechStarted_ = true;
    return true;
}

// The records SpeechInit asked for, one 0x4000-byte record at a time as the CD streamer's dispatch
// 0x80031604 hands them over: a record of type 10 (its first word's top nibble) goes to the PORTED
// SpeechBankLoad with the record + 0x20 and the id `word & 0x0FFFFFFF`; the stream ends at the file's
// end (0x80023300).
void SoundRuntime::SpeechStream() {
    std::vector<SpeechRequest> requests;
    requests.swap(speechPending_);
    if (!speechRead_ || !speechFault_.empty()) return;
    rr::sim::GuestRam g(ram_, kGp);
    rr::sim::SoundMachine s{g, ram_, *io_, fx_.get()};
    Voices v(*this);
    for (const SpeechRequest& q : requests) {
        for (uint32_t k = 0; k < q.count; ++k) {
            const uint32_t off = q.offset + kRecordBytes * k;
            if (off + kRecordBytes > speechFileBytes_) break;
            speechRecord_.assign(kRecordBytes + 0x20u, 0);
            if (!speechRead_(2, off, speechRecord_.data(), kRecordBytes)) break;
            if (off + kRecordBytes + 0x20u <= speechFileBytes_)
                speechRead_(2, off + kRecordBytes, speechRecord_.data() + kRecordBytes, 0x20u);
            uint32_t word = 0;
            std::memcpy(&word, speechRecord_.data(), 4);
            if ((word >> 28) != 10u) continue; // not a speech record: another arm of the dispatch
            g.WriteBlock(heap_ + kHeapStaging, speechRecord_.data(), 0x60u);
            v.done.clear();
            rr::sim::SpeechBankLoad(s, v, heap_ + kHeapStaging + 0x20u, word & 0x0FFFFFFFu, k); // PORTED
            for (const Voices::Callback& d : v.done)
                if (d.fn == rr::sim::kSpeechUploadCb) rr::sim::SpeechUploaded(s, v, d.arg);  // PORTED
            if (s.Faulted()) {
                char buf[160];
                std::snprintf(buf, sizeof(buf), "%s at 0x%08X (a taunt record)", s.failWhat ? s.failWhat : "a guest fault",
                              s.failed ? s.failAddress : g.FaultAddress());
                speechFault_ = buf;
                return;
            }
        }
    }
}

bool SoundRuntime::Speech(uint32_t h, int32_t crash, uint32_t sp, const PushFn& push) {
    if (!speechStarted_ || !speechFault_.empty() || !engineFault_.empty()) return false;
    rr::sim::GuestRam g(ram_, kGp);
    rr::sim::SoundMachine s{g, ram_, *io_, fx_.get()};
    Voices v(*this);
    v.push = &push;
    const uint32_t serial = R32(kGp + rr::sim::kSerialGp);
    rr::sim::RiderSpeech(s, v, h, crash, sp);                              // PORTED, SLUS 0x8001A760
    ++speechCalls_;
    if (R32(kGp + rr::sim::kSerialGp) != serial) ++speechLines_;          // a voice was started for it
    if (s.Faulted()) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s at 0x%08X (RiderSpeech h %u)", s.failWhat ? s.failWhat : "a guest fault",
                      s.failed ? s.failAddress : g.FaultAddress(), h);
        speechFault_ = buf;
        return false;
    }
    return true;
}

// sound_runtime.h StreamSpeechBank: a type-10 chunk of the CD streamer into the speech slots.
bool SoundRuntime::StreamSpeechBank(uint32_t rec, uint32_t id, uint32_t index, uint32_t& v0,
                                    std::vector<std::pair<uint32_t, uint32_t>>& done) {
    v0 = 2;
    if (!speechStarted_ || !speechFault_.empty() || !engineFault_.empty()) return false;
    rr::sim::GuestRam g(ram_, kGp);
    rr::sim::SoundMachine s{g, ram_, *io_, fx_.get()};
    Voices v(*this);
    streamBank_ = true;
    v0 = rr::sim::SpeechBankLoad(s, v, rec, id, index); // PORTED
    streamBank_ = false;
    for (const Voices::Callback& d : v.done) done.push_back({d.fn, d.arg});
    if (s.Faulted()) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s at 0x%08X (a stream bank)", s.failWhat ? s.failWhat : "a guest fault",
                      s.failed ? s.failAddress : g.FaultAddress());
        speechFault_ = buf;
        return false;
    }
    if (v0 == 1) ++streamBanksLoaded_;
    else ++streamBanksDropped_;
    return true;
}

bool SoundRuntime::StreamSpeechDone(uint32_t arg, const std::function<bool(uint32_t)>& release) {
    if (!speechStarted_ || !speechFault_.empty()) return false;
    rr::sim::GuestRam g(ram_, kGp);
    rr::sim::SoundMachine s{g, ram_, *io_, fx_.get()};
    Voices v(*this);
    speechRelease_ = release;
    rr::sim::SpeechUploaded(s, v, arg); // PORTED, SLUS 0x80016464
    speechRelease_ = nullptr;
    return !s.Faulted();
}

std::string SoundRuntime::SpeechTable() const {
    std::string out;
    for (uint32_t i = 0; i < 9; ++i) {
        const uint32_t t = rr::sim::kSpeechSlots + 32u * i;
        char one[64];
        std::snprintf(one, sizeof(one), "%s%u:%X/%X s%u@%d id%X", i ? " " : "", i, R32(t + 12u), R32(t + 16u),
                      R32(t + 8u), static_cast<int32_t>(R32(t)), R32(t + 4u));
        out += one;
    }
    return out;
}

int SoundRuntime::SpeechSlotsLoaded() const {
    int n = 0;
    for (uint32_t i = 0; i < 9; ++i) {
        const uint32_t t = rr::sim::kSpeechSlots + 32u * i;
        const uint32_t state = R32(t + 8u);
        if (state >= 2u && state <= 4u && static_cast<int32_t>(R32(t)) >= 0) ++n;
    }
    return n;
}

void SoundRuntime::SetTables(const int32_t* atan, const int16_t* sincos, const uint8_t* surface52) {
    surface_ = surface52;
    if (atan) std::memcpy(At(rr::sim::kAtanTableAddr), atan, 20 * sizeof(int32_t));
    if (sincos) std::memcpy(At(rr::sim::kSinCosAddr), sincos, 8192 * sizeof(int16_t));
    tables_ = atan != nullptr && sincos != nullptr;
}

void SoundRuntime::CopyGameState() {
    if (gameState_) std::memcpy(At(kGameStateAt), gameState_, kGameStateBytes);
}

rr::sim::SoundSystemEnv SoundRuntime::System() {
    rr::sim::SoundSystemEnv e;
    e.state = rr::sim::SoundBytes(At(kSys), kStateBytes);
    e.voices = rr::sim::SoundBytes(At(voicesAt_), kVoiceStride * kVoiceCount);
    e.banks = bankSlots_.empty() ? nullptr : bankSlots_.data();
    e.bankSlots = static_cast<uint32_t>(bankSlots_.size());
    e.restart = nullptr; // PlaySound3D always passes noReverb = 0
    e.outOfWindow = &outOfWindow_;
    return e;
}

rr::sim::Sound3DEnv SoundRuntime::Listener() {
    rr::sim::Sound3DEnv e;
    e.listeners = rr::sim::SoundBytes(At(listenersAt_), 4u * kListenerStride);
    // SLUS 0x80019E40 / 0x80017BA0 read the player count through `lw *(0x8005B2F8)`, so this follows
    // the same pointer: our own block (Reset writes kGameStateAt there) in the own-arena mode, the
    // race's game_state (0x800D5D38) in the world mode. A fixed kGameStateAt in the world mode read
    // whatever the race arena happens to hold at 0x80112000 as the player count.
    e.gameState = At(R32(0x8005B2F8u));
    e.atanTable = reinterpret_cast<const int32_t*>(At(rr::sim::kAtanTableAddr));
    e.sinCos = reinterpret_cast<const int16_t*>(At(rr::sim::kSinCosAddr));
    e.outOfWindow = &outOfWindow_;
    return e;
}

void SoundRuntime::SetListener(int32_t p, int32_t x, int32_t z, int32_t vx, int32_t vz,
                               int32_t camYaw) {
    rr::sim::SetListener(p, x, z, vx, vz, camYaw,
                         rr::sim::SoundBytes(At(listenersAt_), 4u * kListenerStride));
}

void SoundRuntime::PlaySound3D(int32_t x, int32_t z, int32_t soundIndex, int32_t bank) {
    ++requested_;
    if (played_.size() < 256) played_.push_back(Played{soundIndex, x, z});
    if (bankSlots_.empty() || (gameState_ == nullptr && !attached_) || !tables_) return;
    if (!attached_) CopyGameState();
    const uint32_t before = R32(kSys + 0x14);
    rr::sim::PlaySound3DEnv e;
    e.mutedBank = static_cast<int32_t>(R32(kGp + rr::sim::kRoadBankGp));
    e.defaultBank = static_cast<int32_t>(R32(kGp + rr::sim::kDefaultBankGp));
    e.master3d = static_cast<int32_t>(R32(rr::sim::kEffectsSlider));
    e.listener = Listener();
    e.system = System();
    uint32_t serial = R32(kGp + rr::sim::kSerialGp);
    e.system.serial = &serial;
    rr::sim::PlaySound3D(x, z, soundIndex, bank, e);
    W32(kGp + rr::sim::kSerialGp, serial);
    if (R32(kSys + 0x14) != before) ++allocated_;
}

bool SoundRuntime::SpeechCue(int32_t kind) {
    if (!attached_ || !engineMode_ || !engineFault_.empty()) return false;
    rr::sim::GuestRam g(ram_, kGp);
    rr::sim::SoundMachine s{g, ram_, *io_, fx_.get()};
    rr::sim::SpeechCue(s, kind);                                  // PORTED, SLUS 0x8001B244
    return !s.Faulted();
}

bool SoundRuntime::AnimSounds(uint32_t desc) {
    if (!attached_ || !engineMode_ || !engineFault_.empty()) return false;
    rr::sim::GuestRam g(ram_, kGp);
    rr::sim::SoundMachine s{g, ram_, *io_, fx_.get()};
    rr::sim::AnimSounds(s, desc);                                 // PORTED, SLUS 0x80018E54
    ++animSoundCalls_;
    return !s.Faulted();
}

bool SoundRuntime::RiderOffSound(uint32_t h, uint32_t mode, uint32_t* v0) {
    if (!attached_ || !engineMode_ || !engineFault_.empty()) return false;
    rr::sim::GuestRam g(ram_, kGp);
    rr::sim::SoundMachine s{g, ram_, *io_, fx_.get()};
    const uint32_t r = rr::sim::RiderOffSound(s, h, mode);        // PORTED, SLUS 0x80018440
    if (v0 != nullptr) *v0 = r;
    return !s.Faulted();
}

bool SoundRuntime::BustedMusic() {
    if (!attached_ || !engineMode_ || !engineFault_.empty()) return false;
    rr::sim::GuestRam g(ram_, kGp);
    rr::sim::SoundMachine s{g, ram_, *io_, fx_.get()};
    Voices v(*this);
    bool ok = true;
    rr::sim::BustedMusic(s, v, ok);                               // PORTED, SLUS 0x8001B3C8
    return ok && !s.Faulted();
}

bool SoundRuntime::RaceOverSignal(uint32_t pause) {
    if (!attached_ || !engineMode_ || !engineFault_.empty()) return false;
    rr::sim::GuestRam g(ram_, kGp);
    rr::sim::SoundMachine s{g, ram_, *io_, fx_.get()};
    rr::sim::RaceOverSignal(s, pause);                            // PORTED, SLUS 0x80018C1C
    return !s.Faulted();
}

bool SoundRuntime::CountdownVoiceStop() {
    if (!attached_ || !engineMode_ || !engineFault_.empty()) return false;
    rr::sim::GuestRam g(ram_, kGp);
    rr::sim::SoundMachine s{g, ram_, *io_, fx_.get()};
    rr::sim::CountdownVoiceStop(s);                               // PORTED, SLUS 0x80016528
    return !s.Faulted();
}

bool SoundRuntime::SoundHold(uint32_t v) {
    if (!attached_ || !engineMode_ || !engineFault_.empty()) return false;
    rr::sim::GuestRam g(ram_, kGp);
    rr::sim::SoundMachine s{g, ram_, *io_, fx_.get()};
    rr::sim::SoundHold(s, v);                                     // PORTED, SLUS 0x80020E30
    return !s.Faulted();
}

void SoundRuntime::QueueListenerSound(int32_t a, int32_t id, int32_t t, int32_t p) {
    rr::sim::QueueListenerSound(a, id, t, p,
                                rr::sim::SoundBytes(At(listenersAt_), 4u * kListenerStride));
}

uint32_t SoundRuntime::SurfaceSound(int32_t k) const {
    if (surface_ == nullptr) return 0;
    return rr::sim::SurfaceSound(k, surface_);
}

int32_t SoundRuntime::SoundCountOfBank(int32_t slot) const {
    if (slot < 0 || static_cast<size_t>(slot) >= bankSlots_.size()) return 0;
    const rr::sim::SoundBankRef& b = bankSlots_[static_cast<size_t>(slot)];
    return (b.data != nullptr && b.size > 4) ? static_cast<int32_t>(b.data[4]) : 0;
}

// Decodes one sound by walking the SPU loop flags from its own start offset, exactly as
// `audio.md` 4 walks `AUDTAUNT.STR`: 16-byte blocks, and the block whose flags byte has bit 0
// set is the last one. Decoded once and cached (emitter-only mode).
const std::vector<int16_t>* SoundRuntime::Pcm(int32_t slot, uint32_t offset) {
    if (slot < 0 || static_cast<size_t>(slot) >= bankSource_.size()) return nullptr;
    const int32_t entry = bankSource_[static_cast<size_t>(slot)];
    if (entry < 0 || static_cast<size_t>(entry) >= banks_.size()) return nullptr;
    const uint64_t key = (static_cast<uint64_t>(entry) << 32) | offset;
    const auto it = pcm_.find(key);
    if (it != pcm_.end()) return &it->second;

    const std::vector<uint8_t>& samples = banks_[static_cast<size_t>(entry)].samples;
    if (offset + rr::kSpuBlockSize > samples.size()) return nullptr;
    size_t end = offset;
    while (end + rr::kSpuBlockSize <= samples.size()) {
        const uint8_t flags = samples[end + 1];
        end += rr::kSpuBlockSize;
        if (flags & 1u) break; // loop end - the last block of this sample
    }
    rr::AdpcmState st;
    std::vector<int16_t> out;
    rr::DecodeAdpcm(std::span<const uint8_t>(samples.data() + offset, end - offset),
                    rr::AdpcmBlock::Spu16, st, out);
    if (out.empty()) return nullptr;
    return &pcm_.emplace(key, std::move(out)).first->second;
}

// ---------------------------------------------------------------------------- the stage-B stand-in
//
// Emitter-only mode. `SoundService SLUS 0x8001EE94` keys off whatever is in `S+0x18`, then walks
// every voice and programs the ones whose channel bit is in `S+0x14` through `ProgramVoice SLUS
// 0x8001EB7C`. This reproduces the SHAPE of that pass - including `v->state = 2` for EVERY voice -
// and hands the six voice values (SoundVoiceStart) to the caller. In the engine mode the PORTED
// SoundService drains the masks from `VSync`, so this returns nothing.
void SoundRuntime::Service(std::vector<SoundVoiceStart>& started, std::vector<int32_t>& stopped) {
    started.clear();
    stopped.clear();
    if (engineMode_ || bankSlots_.empty()) return;

    const uint32_t keyOff = R32(kSys + 0x18);
    if (keyOff != 0) {
        for (uint32_t i = 0; i < kVoiceCount; ++i) {
            const int32_t ch = static_cast<int32_t>(R32(kVoicesAt + kVoiceStride * i + 0x1C));
            if ((1u << (static_cast<uint32_t>(ch) & 31u)) & keyOff) stopped.push_back(ch);
        }
        W32(kSys + 0x18, 0);
    }
    const uint32_t keyOn = R32(kSys + 0x14);
    if (keyOn != 0) {
        for (uint32_t i = 0; i < kVoiceCount; ++i) {
            const uint32_t v = kVoicesAt + kVoiceStride * i;
            const int32_t ch = static_cast<int32_t>(R32(v + 0x1C));
            if ((1u << (static_cast<uint32_t>(ch) & 31u)) & keyOn) {
                const uint32_t descAddr = R32(v + 0x08);
                for (size_t slot = 0; slot < bankSlots_.size(); ++slot) {
                    const rr::sim::SoundBankRef& b = bankSlots_[slot];
                    if (b.data == nullptr || descAddr < b.address || descAddr + 12 > b.address + b.size)
                        continue;
                    const uint8_t* d = At(descAddr);
                    SoundVoiceStart s;
                    s.channel = ch;
                    s.adsr1 = ReadU16(d + 2);
                    s.adsr2 = ReadU16(d + 4);
                    s.spuAddress = R32(descAddr + 8) - bankSpuBase_[slot];
                    s.pitch = static_cast<uint16_t>(R32(v + 0x20));
                    s.volumeLeft = static_cast<int32_t>(R32(v + 0x24));
                    s.volumeRight = static_cast<int32_t>(R32(v + 0x28));
                    s.pcm = Pcm(static_cast<int32_t>(slot), s.spuAddress);
                    if (s.pcm != nullptr) {
                        started.push_back(s);
                        ++programmed_;
                    }
                    break;
                }
            }
            W32(v + 0x10, 2); // outside the `if`, exactly as 0x8001EF54 is
        }
        W32(kSys + 0x14, 0);
    }
}

// ---------------------------------------------------------------------------- the engine note
bool SoundRuntime::EngineFrame(std::span<const uint8_t> bike, std::span<const uint8_t> owner,
                               std::span<const uint8_t> stats, uint32_t& lcgSeed) {
    if (!engineMode_ || attached_ || bankSlots_.empty() || !tables_ || !engineFault_.empty()) return false;
    if (bike.size() < 1096) return false;
    CopyGameState();
    std::memcpy(At(kBikeAt), bike.data(), 1096);
    std::memcpy(At(kOwnerAt), owner.data(), std::min<size_t>(owner.size(), 0x800));
    std::memcpy(At(kStatsAt), stats.data(), std::min<size_t>(stats.size(), 0x800));
    W32(kBikeAt + 0x354, kOwnerAt);
    W32(kBikeAt + 0x22C, kStatsAt);
    W32(rr::sim::kSoundPlayerBikes, kBikeAt);
    W32(kGp + rr::sim::kRandSeedGp, lcgSeed);

    rr::sim::GuestRam g(ram_, kGp);
    rr::sim::SoundMachine s{g, ram_, *io_, fx_.get()};
    if (!engineSetUp_) {
        rr::sim::EngineSetup(s);
        engineSetUp_ = true;
    }
    rr::sim::EngineNote(s, 0, kFrameSp);
    rr::sim::RoadNote(s, 0);
    lcgSeed = R32(kGp + rr::sim::kRandSeedGp);
    ++engineFrames_;
    if (s.Faulted()) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s at 0x%08X", s.failWhat ? s.failWhat : "a guest fault",
                      s.failed ? s.failAddress : g.FaultAddress());
        engineFault_ = buf;
        return false;
    }
    return true;
}

void SoundRuntime::VSync() {
    if (!engineMode_ || !engineFault_.empty()) return;
    rr::sim::GuestRam g(ram_, kGp);
    rr::sim::SoundMachine s{g, ram_, *io_, fx_.get()};
    rr::sim::AudioVSyncTick(s);
    ++vsyncs_;
    if (s.Faulted()) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s at 0x%08X (vsync)", s.failWhat ? s.failWhat : "a guest fault",
                      s.failed ? s.failAddress : g.FaultAddress());
        engineFault_ = buf;
    }
}

} // namespace rr::game
