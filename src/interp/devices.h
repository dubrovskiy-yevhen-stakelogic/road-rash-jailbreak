#pragma once
// A development-only model of the PlayStation peripherals that the race loop touches, so that a
// captured mid-race machine state can be RESUMED and one real frame can be recorded.
//
// This is not a console emulator and it is not on the road to being one. It exists because the
// oracle interpreter otherwise traps on the first `sw` into 0x1F8018xx, and a frame trace is the
// only way to see what the original does within one frame.
//
// Three rules keep it honest (no stub fakes success):
//
//  1. Every register that is not explicitly modelled still TRAPS, with its address. Nothing reads
//     back as a convenient zero.
//  2. Every place where the model is a deliberate simplification of hardware is named in
//     `kKnownSimplifications` below and repeated in the doc. The two that matter: the GPU is never
//     busy, and VBlank is paced by retired instructions rather than by a dot clock.
//  3. Nothing is invented for data the guest could read back and act on. The VRAM the GPU serves is
//     the snapshot's own VRAM; SPU RAM is the snapshot's own SPU RAM.
#include <cstdint>
#include <string>
#include <vector>

#include "interp/trace.h"
#include "rrformats/mdec.h"

namespace rr::interp {

class Memory;

// Interrupt controller bit numbers (psx-spx I_STAT).
enum : int {
    kIrqVBlank = 0,
    kIrqGpu = 1,
    kIrqCdrom = 2,
    kIrqDma = 3,
    kIrqTimer0 = 4,
    kIrqTimer1 = 5,
    kIrqTimer2 = 6,
    kIrqPad = 7,
    kIrqSio = 8,
    kIrqSpu = 9,
};

struct DmaChannel {
    uint32_t madr = 0;
    uint32_t bcr = 0;
    uint32_t chcr = 0;
};

class Devices {
public:
    explicit Devices(Memory& memory);

    // Non-null enables the GPU word stream.
    Tracer* tracer = nullptr;

    // Kept in step by the Cpu so trace records can be attributed.
    uint32_t currentPc = 0;
    uint64_t seq = 0;

    // ---------------------------------------------------------------- MMIO
    // Both return false when the address is not modelled; `why` then says what it was.
    bool Read(uint32_t address, uint32_t size, uint32_t& out, std::string& why);
    bool Write(uint32_t address, uint32_t size, uint32_t value, std::string& why);

    // ---------------------------------------------------------------- interrupts
    uint32_t istat = 0;
    uint32_t imask = 0;
    bool IrqPending() const { return (istat & imask & 0x7FFu) != 0; }
    void RaiseIrq(int bit) { istat |= (1u << bit); }

    // ---------------------------------------------------------------- pacing
    // SIMPLIFICATION: no cycle counting exists in this interpreter, so VBlank is raised every
    // `instructionsPerFrame` retired instructions. 0 disables it.
    uint64_t instructionsPerFrame = 0;
    uint64_t nextVblankAt = 0;
    uint64_t vblankCount = 0;
    // Guest CPU cycles charged per retired instruction, used only to pace the root counters.
    uint64_t cyclesPerInstruction = 2;
    // Advances the timers and raises VBlank when due. Called once per retired instruction.
    void Advance(uint64_t retired);

    // ---------------------------------------------------------------- GPU
    std::vector<uint16_t> vram; // 1024 x 512, seeded from the snapshot's vram.bin
    uint32_t gpustat = 0x14802000u;
    uint32_t gpuread = 0;
    uint64_t gp0WordsSeen = 0;
    uint64_t gp1WordsSeen = 0;
    uint64_t vramWordsUploaded = 0;
    // GP1(05h) sets the display area start, i.e. it is the buffer swap. Counting it is the least
    // arbitrary definition of "a frame" available from the GPU side alone.
    uint64_t displayStartCount = 0;
    bool LoadVram(const std::vector<uint8_t>& bytes, std::string& error);

    // ---------------------------------------------------------------- DMA
    DmaChannel dma[7];
    uint32_t dpcr = 0x07654321u;
    uint32_t dicr = 0;
    uint64_t dmaLinkedListPackets = 0;
    uint64_t dmaLinkedListWords = 0;

    // Seeds a root counter from a captured machine state. Storing the mode directly (rather than
    // going through the register write path) is deliberate: a write to the mode register resets the
    // counter on hardware, which would throw away the captured value.
    void SetTimer(int index, uint32_t mode, uint32_t counter, uint32_t target);

    // ---------------------------------------------------------------- SPU
    std::vector<uint8_t> spuram; // 512 KiB, seeded from the snapshot's spuram.bin
    bool LoadSpuRam(const std::vector<uint8_t>& bytes, std::string& error);

    // ---------------------------------------------------------------- SIO0 (controller port)
    // A standard digital pad in slot 1 with nothing pressed, and no memory cards. The protocol is
    // the documented one (0x01 address, 0x42 read command, 0x41/0x5A id, two button bytes); any
    // other address byte answers "no device", which is what the guest's own code then concludes.
    //
    // The exchange is NOT instantaneous. See the long comment in devices.cpp: the game's own SIO0
    // driver (`SLUS_010.53` 0x80040B10 / 0x80040DC4 / 0x800411E8 / 0x80041670) writes JOY_TX_DATA
    // and then, a few instructions later, clears I_STAT bit 7 - so a byte that acknowledged inside
    // the store instruction would have its interrupt wiped before the driver ever waits for it, and
    // the transfer would deadlock. Modelling the byte time is what re-arms the driver.
    uint16_t padButtons = 0xFFFF; // active low: 0xFFFF = nothing pressed
    bool padConnected = true;
    uint64_t padPollCount = 0;   // completed 5-byte digital-pad exchanges
    uint64_t padBytesSent = 0;   // bytes the guest wrote into JOY_TX_DATA
    // Retired instructions charged to one byte when the baud divider cannot be read (never, in
    // practice: the driver programs JOY_BAUD before every transfer).
    uint64_t joyFallbackByteInstructions = 544;

    // Unmodelled-register policy: `false` (the default) traps, which is what an oracle should do.
    // A tool may set it to record-and-continue when it is explicitly exploring, in which case every
    // such access lands in `unmodelled` instead of stopping the run. Reads then return 0, which is
    // a LIE, so a run with this on is only ever used to find out what to model next.
    bool exploreUnmodelled = false;
    struct Unmodelled {
        uint32_t address;
        uint32_t pc;
        uint32_t size;
        uint32_t value;
        uint64_t count;
        bool isWrite;
    };
    std::vector<Unmodelled> unmodelled;

    // ---------------------------------------------------------------- MDEC (0x1F801820 / 0x1F801824, DMA 0 / 1)
    // The unit's arithmetic is rr::mdec (src\rrformats\mdec.h), the one the product's sky and films run - the
    // commands 1 (decode), 2 (quant tables) and 3 (IDCT matrix) as the hardware takes them, a command's parameter
    // words from the register or DMA0, its output words to the register or DMA1. SIMPLIFICATION (named in
    // kKnownSimplifications): a command runs the moment its last parameter word arrives, and DMA1 completes when
    // its words are there (it waits, busy, until a decode produced them).
    rr::mdec::Tables mdecTables;
    rr::mdec::Model mdecModel = rr::mdec::ActiveModel(); // a check may set another (rrverify mdec --device-model)
    uint64_t mdecCommands = 0, mdecMacroblocks = 0, mdecWordsOut = 0;

    static const char* const kKnownSimplifications[];

private:
    uint32_t mdecCmd_ = 0;              // the command being fed
    uint32_t mdecNeed_ = 0;             // parameter words it still wants (0: waiting for a command word)
    std::vector<uint32_t> mdecParams_;  // its parameter words so far
    std::vector<uint32_t> mdecOut_;     // the output FIFO
    size_t mdecOutPos_ = 0;
    uint32_t mdecControl_ = 0;          // bits 30 / 29: DMA in / out requests enabled
    bool mdecDma1Waiting_ = false;
    void MdecWord(uint32_t w);          // a word into the command / parameter port
    void MdecRun();
    uint32_t MdecStatus() const;
    size_t MdecOutAvailable() const { return mdecOut_.size() - mdecOutPos_; }
    bool MdecDma1(std::string& why);

    Memory& memory_;

    // GP0 command assembly.
    uint32_t gp0Fifo_[16]{};
    uint32_t gp0Have_ = 0;
    uint32_t gp0Need_ = 0;     // 0 = waiting for a command word
    bool gp0Polyline_ = false;
    // CPU -> VRAM transfer in progress.
    uint32_t blitX_ = 0, blitY_ = 0, blitW_ = 0, blitH_ = 0, blitCol_ = 0, blitRow_ = 0;
    uint32_t blitRemaining_ = 0;
    // VRAM -> CPU transfer in progress.
    uint32_t readX_ = 0, readY_ = 0, readW_ = 0, readH_ = 0, readCol_ = 0, readRow_ = 0;
    uint32_t readRemaining_ = 0;

    uint32_t drawOffsetX_ = 0, drawOffsetY_ = 0;

    // Root counters. There is no cycle counter in this interpreter, so guest cycles are estimated
    // as `cyclesPerInstruction` per retired instruction and then divided down by each counter's
    // selected clock source. What matters for a frame trace is only that a counter advances much
    // more slowly than the CPU, because libetc reads one twice and compares.
    struct Timer {
        uint32_t counter = 0;
        uint32_t mode = 0;
        uint32_t target = 0;
        uint64_t accumulated = 0; // guest cycles not yet turned into ticks
    } timer_[3];
    uint64_t lastSeq_ = 0;

    // SPU register file, 0x1F801C00..0x1F801FFF.
    uint8_t spureg_[0x400]{};
    uint32_t spuTransferAddress_ = 0;

    // SIO0.
    uint16_t joyCtrl_ = 0;
    uint16_t joyMode_ = 0;
    uint16_t joyBaud_ = 0x0088;
    uint8_t joyRx_ = 0xFF;
    bool joyRxFull_ = false;
    bool joyIrq_ = false;         // JOY_STAT bit 9, the latched ACK interrupt
    int joySeq_ = -1;   // -1 = idle, 0 = the address byte has been sent
    // The byte currently on the wire.
    bool joyBusy_ = false;
    uint64_t joyDueAt_ = 0;       // retired-instruction count at which it lands
    uint8_t joyPendingRx_ = 0xFF;
    bool joyPendingAck_ = false;
    void JoyStart(uint8_t tx, uint64_t retired);
    void JoyWriteCtrl(uint16_t value);
    void JoyComplete();
    void JoyReset();              // JOY_CTRL bit 6, or a deselect: drop everything in flight
    uint64_t JoyByteInstructions() const;

    void RecordGpuWord(uint32_t value, uint8_t port, GpuWordSource source, uint32_t address);
    void WriteGp0(uint32_t value, GpuWordSource source, uint32_t address);
    void WriteGp1(uint32_t value);
    void Gp0Command();
    static uint32_t Gp0Length(uint32_t command);
    void VramPut(uint32_t x, uint32_t y, uint16_t v);
    uint16_t VramGet(uint32_t x, uint32_t y) const;

    bool DmaWrite(uint32_t address, uint32_t value, std::string& why);
    bool DmaRead(uint32_t address, uint32_t& out, std::string& why);
    bool RunDma(int channel, std::string& why);
    void FinishDma(int channel);

    void AdvanceTimers(uint64_t retired);
    bool NoteUnmodelled(uint32_t address, uint32_t size, uint32_t value, bool isWrite,
                        const char* what, std::string& why);
};

} // namespace rr::interp
