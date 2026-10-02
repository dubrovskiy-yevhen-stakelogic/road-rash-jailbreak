#pragma once
// An R3000A interpreter used ONLY as a development oracle: it exists so that a single original
// guest function can be executed against a captured machine snapshot and compared, bit for bit,
// against our native C++ replacement. It is never linked into the shipped game.
//
// Design rules that follow from that purpose:
//   * exact MIPS I semantics - branch delay slots, load delay slots, unaligned lwl/lwr/swl/swr,
//     hi/lo, R3000 division corner cases;
//   * NOTHING is silently ignored. An unimplemented instruction, an MMIO access, an unmapped
//     address, an unaligned access, an arithmetic overflow, a BREAK, a SYSCALL or a jump through a
//     BIOS vector all stop execution with a Trap that names the pc and the instruction. A stub that
//     fakes success would make the oracle worthless.
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "interp/gte.h"

namespace rr::interp {

class Devices;
class Tracer;

enum class TrapKind : uint32_t {
    None = 0,
    Halted,                   // pc reached the sentinel return address - a normal finish
    StepLimit,                // the instruction budget ran out
    UnknownInstruction,       // the opcode does not exist on an R3000A
    UnimplementedInstruction, // it exists, we refuse to guess at it
    MmioAccess,               // a load or store into 0x1F801000..0x1F802FFF
    UnmappedAddress,
    ReadOnlyWrite,            // a store into the BIOS image
    AddressError,             // unaligned lh/lw/sh/sw, or an odd instruction fetch
    ArithmeticOverflow,       // add / addi / sub
    Break,
    Syscall,
    BiosCall,                 // a jump through the 0xA0 / 0xB0 / 0xC0 kernel vectors
    CoprocessorUnusable,      // COP0 without kernel mode, COP1/COP3, COP2 with SR.CU2 clear
    IsolatedCache,            // a memory access while SR.IsC is set
    GteUnimplemented,
    DivisionByZero,           // never raised: R3000 defines it. Reserved so the enum stays stable.
    Hook,                     // a host-side hook asked for execution to stop
};

const char* TrapKindName(TrapKind kind);

struct Trap {
    TrapKind kind = TrapKind::None;
    uint32_t pc = 0;          // the instruction that raised it
    uint32_t instruction = 0;
    uint32_t address = 0;     // the offending data address, where one applies
    std::string detail;

    explicit operator bool() const { return kind != TrapKind::None; }
    std::string ToString() const;
};

// The guest address space. 2 MiB of RAM visible at 0x00000000 / 0x80000000 / 0xA0000000 (and
// mirrored every 2 MiB inside the first 8 MiB of physical space, as on real hardware), the 1 KiB
// scratchpad at 0x1F800000, and a 512 KiB BIOS ROM at 0xBFC00000. Everything else is a trap.
class Memory {
public:
    static constexpr uint32_t kRamSize = 2u * 1024u * 1024u;
    static constexpr uint32_t kScratchpadSize = 1024u;
    static constexpr uint32_t kBiosSize = 512u * 1024u;

    Memory();

    std::vector<uint8_t>& ram() { return ram_; }
    const std::vector<uint8_t>& ram() const { return ram_; }
    std::vector<uint8_t>& scratchpad() { return scratchpad_; }
    const std::vector<uint8_t>& scratchpad() const { return scratchpad_; }
    std::vector<uint8_t>& bios() { return bios_; }
    const std::vector<uint8_t>& bios() const { return bios_; }

    enum class Region { Unmapped, Ram, Scratchpad, Bios, Mmio };

    // Classifies a virtual address and returns the host pointer for it, or nullptr.
    Region Classify(uint32_t address, uint8_t** host);
    Region Classify(uint32_t address, const uint8_t** host) const;

    // Host-side access that bypasses trapping - used to load a snapshot and to diff the result.
    bool ReadBlock(uint32_t address, void* dst, size_t length) const;
    bool WriteBlock(uint32_t address, const void* src, size_t length);
    uint32_t PeekWord(uint32_t address) const;
    void PokeWord(uint32_t address, uint32_t value);
    uint8_t PeekByte(uint32_t address) const;

    // Convenience: "is this address inside guest RAM", used by the diff.
    static bool IsRamAddress(uint32_t address);
    static uint32_t RamOffset(uint32_t address); // only valid when IsRamAddress

private:
    std::vector<uint8_t> ram_;
    std::vector<uint8_t> scratchpad_;
    std::vector<uint8_t> bios_;
};

class Cpu {
public:
    static constexpr uint8_t kNoLoadDelay = 32;

    explicit Cpu(Memory& memory);

    Memory& memory() { return memory_; }
    Gte& gte() { return gte_; }
    const Gte& gte() const { return gte_; }

    uint32_t regs[32]{};
    uint32_t hi = 0;
    uint32_t lo = 0;
    uint32_t pc = 0;   // the instruction to execute next
    uint32_t npc = 0;  // the one after it (differs from pc + 4 inside a branch delay slot)
    uint32_t cop0[16]{};

    // Pending load-delay slot. `reg` == kNoLoadDelay means none.
    uint8_t loadDelayReg = kNoLoadDelay;
    uint32_t loadDelayValue = 0;

    uint64_t instructionsRetired = 0;

    void Reset();

    // Execution stops when pc == `sentinel` (Trap::Halted), when `maxSteps` instructions have run
    // (Trap::StepLimit), or on the first real trap. `maxSteps` == 0 means unlimited.
    Trap Run(uint32_t sentinel, uint64_t maxSteps);

    // Single step, for tracing. Returns an empty Trap when the instruction retired normally.
    Trap Step();

    // Breakpoint-style hooks. AddHook marks a RAM word address; when pc reaches it, Run stops with
    // Trap::Hook and `detail` naming the hook. Cheap: one bitmap probe per instruction.
    void AddHook(uint32_t address, std::string name);
    void ClearHooks();
    const std::string& lastHookName() const { return lastHookName_; }

    // ---------------------------------------------------------------- call observation
    //
    // Like a hook, but it RECORDS and lets execution continue: every time `pc` reaches a watched
    // entry address the o32 arguments and the stack pointer are appended to `callObservations`. It
    // exists for the bench's "callee supplied by the oracle" mode:
    // the guest run is watched at the unported callee's entry and the native run records the calls
    // its port made, and the two sequences must match exactly - which is what proves that a native
    // port calls such a callee as often, in the same order and with the same arguments as the
    // original. One predictable branch per instruction when nothing is watched.
    //
    // `a[0..3]` are the argument registers. `a[4..7]` are the four STACK argument slots o32 puts in
    // the caller's frame at sp+16, sp+20, sp+24 and sp+28; they are read out of memory at the
    // moment of the call, because the caller has already stored them by the time `pc` reaches the
    // callee's entry. A row declares how many of the eight a callee really takes
    // (`OracleCallee::arity` / `::stackArgs` in the bench) - the rest are leftovers and are not
    // compared. Reading the four slots costs four word loads per observed call and nothing at all
    // when no address is watched.
    //
    // `frameOuts` is the extension a seven-argument callee needed. When one of a callee's arguments
    // is a pointer to an OUTPUT BUFFER IN THE CALLER'S OWN FRAME, the bytes behind it cannot be
    // read when the call is observed (the callee has not written them yet) and they cannot be read
    // when the row finishes either, because whatever the caller does next reuses that part of the
    // stack. So they are captured at the instant the callee RETURNS: the observer remembers the
    // callee's `ra` and `sp`, and the first time `pc` comes back to that `ra` with the frame popped
    // the declared number of bytes is copied here. Declared per callee with ObserveCallFrameOut.
    //
    // There may be MORE THAN ONE per call, as `RASHCDG 0x800A7BF8` requires:
    // its third and fourth arguments are both output buffers, and `RASHCDG 0x80075048`'s last call
    // site keeps both of them in its own frame (`sp+24` and `sp+72`). One buffer per observation
    // would silently overwrite the first with the second. Each slot
    // remembers WHICH argument it came from, so the bench pairs guest and port by argument index
    // rather than by position.
    static constexpr uint32_t kMaxFrameOut = 64;
    static constexpr int kMaxFrameOuts = 4; // caller-frame output buffers per call
    struct FrameOutCapture {
        int arg = -1;       // the o32 argument index it was taken from; -1 = unused slot
        uint32_t len = 0;   // 0 = declared but the callee never returned
        uint8_t bytes[kMaxFrameOut]{};
    };
    struct CallObservation {
        uint32_t address = 0;
        // a0..a3, then the o32 stack arguments sp+16..sp+44. Twelve, not eight:
        // `RASHCDG 0x80080D1C` (HitSpeed) takes SEVEN stack arguments.
        uint32_t a[12]{};
        uint32_t sp = 0;
        FrameOutCapture frameOuts[kMaxFrameOuts];

        // The slot for argument `arg`, creating it on first use. Null when all slots are taken.
        FrameOutCapture* FrameOutFor(int arg) {
            for (FrameOutCapture& f : frameOuts)
                if (f.arg == arg) return &f;
            for (FrameOutCapture& f : frameOuts)
                if (f.arg < 0) { f.arg = arg; return &f; }
            return nullptr;
        }
        const FrameOutCapture* FrameOutFor(int arg) const {
            for (const FrameOutCapture& f : frameOuts)
                if (f.arg == arg && f.len != 0) return &f;
            return nullptr;
        }
        bool HasFrameOut() const {
            for (const FrameOutCapture& f : frameOuts)
                if (f.arg >= 0 && f.len != 0) return true;
            return false;
        }
    };
    void ObserveCalls(uint32_t address);
    // `argIndex` is 0-based over the whole o32 argument list (0..3 = a0..a3, 4..11 = sp+16..sp+44).
    void ObserveCallFrameOut(uint32_t address, int argIndex, uint32_t bytes);
    void ClearCallObservers(); // forgets the watched addresses AND the recorded calls
    std::vector<CallObservation> callObservations;

    // COP0 status register bits we care about.
    bool Cop2Enabled() const { return (cop0[12] & (1u << 30)) != 0; }
    bool CacheIsolated() const { return (cop0[12] & (1u << 16)) != 0; }

    // A jump to 0xA0 / 0xB0 / 0xC0 is the PsyQ BIOS vector thunk. By default it stops the machine,
    // because servicing the kernel is a decision the bench has to make explicitly rather than
    // something the core should paper over. Clearing this lets the real kernel code in the snapshot
    // run (it will then trap on its own if it reaches MMIO).
    bool trapOnBiosVector = true;

    // ---------------------------------------------------------------- development facilities
    //
    // Both are null by default and cost one predictable branch when they are. `devices` turns an
    // MMIO access from a trap into a modelled peripheral access (src\interp\devices.h); `tracer`
    // records COP2 commands, GPU words, watched memory and calls (src\interp\trace.h).
    Devices* devices = nullptr;
    Tracer* tracer = nullptr;

    // With `devices` attached, a pending device interrupt is taken at the next instruction boundary
    // that is not a branch delay slot: EPC = pc, CAUSE.ExcCode = 0, SR's KU/IE stack shifts, and
    // execution continues at the general exception vector. The handler in the snapshot's own kernel
    // then runs like any other guest code.
    bool takeInterrupts = true;
    uint64_t interruptsTaken = 0;
    uint32_t lastExceptionVector = 0;

    // Off by default, so the existing gates keep stopping on a SYSCALL/BREAK instead of running
    // into the kernel. With it on, both are dispatched through the general exception vector and the
    // snapshot's own kernel services them - which is what a real frame needs (the game brackets its
    // critical sections with SYSCALL 1 / SYSCALL 2).
    bool handleExceptions = false;
    uint64_t syscallsTaken = 0;

    // 0xFFFE0130, which sits in KSEG2 rather than in the MMIO window. Only reachable with
    // `devices` attached; otherwise it is an unmapped-address trap as before.
    uint32_t cacheControl = 0x0001E988;

    // A named concession for a bench that runs whole frames of the original with NO device model.
    //
    // `SLUS 0x80043F00` is `GetRCnt(id)`: for `id & 0xFFFF < 3` it loads the 16-bit VALUE register
    // of root counter `id` (0x1F801100 + 16*id) and for anything else it returns 0. Six sites in
    // `RASHCDG` call it with 0xF2000002 inside the race tick, so a bench that executes the tick
    // meets a hardware read it has no peripheral for and stops - not because the port is wrong but
    // because the harness has no timer. Attaching the full `Devices` model is not an answer here:
    // its counters advance with retired instructions, the two sides of a comparison retire
    // different numbers of them, and the "oracle" would then disagree with itself.
    //
    // With this on, a LOAD from one of the three counter VALUE registers yields `rootCounterValue`
    // and nothing else changes: every other MMIO load, and EVERY MMIO STORE, still traps exactly as
    // before. Both sides of a comparison read the same constant, so the comparison stays
    // meaningful; what it does not prove is any behaviour that depends on the counter really
    // advancing - which this harness could never prove anyway. Off by default.
    bool rootCountersAreConstant = false;
    uint32_t rootCounterValue = 0;

    // The same kind of concession, for the other peripheral a race frame reaches: with this on, a
    // STORE into the SPU's register window 0x1F801C00..0x1F801E7F is dropped instead of trapping.
    // Nothing else changes - every other MMIO store still traps, and every SPU LOAD still traps, so
    // guest code that reads a voice register back still stops the bench rather than being fed a
    // fiction. The reason this is safe for a whole-RAM comparison is narrow and worth stating: a
    // plain write to an SPU register cannot move a byte of guest RAM. SPU transfers that do are
    // driven by DMA channel 4, whose registers are at 0x1F8010C0 and still trap. Off by default.
    bool spuWritesAreDropped = false;

    // A third named concession, narrower than it looks: a 16-halfword REGISTER FILE for the SPU's
    // global control block 0x1F801D80..0x1F801D9B (main volume, reverb output volume, KON, KOFF,
    // PMON, NON, EON):
    //
    //   * with it on, an aligned 16- or 32-bit LOAD there returns what the last store there wrote,
    //     starting from `spuControl` as the caller seeded it (the bench seeds it from the snapshot's
    //     own SPU block, `spuctl.bin`); an aligned 16- or 32-bit STORE there updates the file;
    //   * ENDX (0x1F801D9C..0x1F801D9F) is NOT in it: its value is produced by voices playing, which
    //     this harness does not model, so a load there still traps and a store is only dropped;
    //   * byte accesses there still trap - the SPU's 8-bit bus behaviour is not modelled;
    //   * every other SPU register keeps the old rule (store dropped, load traps).
    //
    // Why it exists: `SLUS 0x800506A8` - libspu's "set a bit in a voice-mask register pair", behind
    // `SpuSetReverbVoice 0x80050678` and `SpuSetPitchLFOVoice 0x80051088` - READS the pair before it
    // writes it, and returns the combined mask. `StopVoice` and `SetVoicePitchMod` of the engine
    // note go through it on every live call. Reads return what a real SPU returns for these
    // registers (they are plain read/write registers on the hardware), so this is a device model of
    // eight registers, not a fiction; what it does NOT model is anything the SPU would do with them
    // (voices keying on, reverb, pitch modulation). Off by default.
    static constexpr uint32_t kSpuControlFirst = 0x1F801D80u;
    static constexpr uint32_t kSpuControlEnd = 0x1F801D9Cu; // exclusive: ENDX is left out
    bool spuControlRegisterFile = false;
    uint16_t spuControl[16] = {};
    // Optional: every store the two SPU concessions accept (into the register file or dropped),
    // in order, so a bench can compare what a port wrote to the SPU with what the original wrote
    // even where nothing reads it back. Recording only - it changes no answer. Null = off.
    struct SpuStore {
        uint32_t address = 0; // physical, 0x1F801C00..0x1F801E7F
        uint32_t size = 0;
        uint32_t value = 0;
        bool operator==(const SpuStore& o) const {
            return address == o.address && size == o.size && value == o.value;
        }
    };
    std::vector<SpuStore>* spuStoreLog = nullptr;

    // The MMIO answers the concessions above give, as one function the interpreter's own Load/Store
    // use AND a native port can call on the same machine, so a port that reaches the hardware
    // through an interface sees exactly what the guest sees. `address` is the full virtual address;
    // returns false where the interpreter would trap.
    bool ConcessionLoad(uint32_t address, uint32_t size, uint32_t& out) const;
    bool ConcessionStore(uint32_t address, uint32_t size, uint32_t value);

private:
    Memory& memory_;
    Gte gte_;
    Trap trap_;

    // The load issued by the instruction currently executing; it becomes `loadDelayReg` once the
    // instruction retires, which is what makes the delay slot visible to exactly one instruction.
    uint8_t newLoadReg_ = kNoLoadDelay;
    uint32_t newLoadValue_ = 0;
    bool inDelaySlot_ = false;
    bool branchTaken_ = false;

    std::vector<uint64_t> hookBitmap_;
    std::vector<std::pair<uint32_t, std::string>> hookNames_;
    bool hasHooks_ = false;
    std::string lastHookName_;

    std::vector<uint64_t> observeBitmap_;
    bool hasObservers_ = false;
    // The caller-frame output buffers declared by ObserveCallFrameOut, and the calls whose return
    // is still being waited for. Both are tiny - one entry each in every use so far.
    struct FrameOutSpec {
        uint32_t address = 0;
        int argIndex = 0;
        uint32_t bytes = 0;
    };
    struct PendingFrameOut {
        uint32_t ra = 0;
        uint32_t sp = 0;
        uint32_t src = 0;
        uint32_t bytes = 0;
        size_t observation = 0;
        int arg = 0;
    };
    std::vector<FrameOutSpec> frameOutSpecs_;
    std::vector<PendingFrameOut> pendingFrameOut_;

    void SetTrap(TrapKind kind, uint32_t at, uint32_t instruction, uint32_t address, std::string detail);

    // Register write that also cancels a pending load into the same register (an ALU write in the
    // load delay slot wins on real hardware).
    inline void SetReg(uint32_t r, uint32_t v) {
        if (r == 0) return;
        regs[r] = v;
        if (loadDelayReg == r) loadDelayReg = kNoLoadDelay;
    }

    bool Fetch(uint32_t address, uint32_t& out);
    bool Load(uint32_t address, uint32_t size, uint32_t& out, uint32_t at, uint32_t instruction);
    bool Store(uint32_t address, uint32_t size, uint32_t value, uint32_t at, uint32_t instruction);

    bool InterruptPending();
    void TakeInterrupt();
    void RaiseException(uint32_t excCode, uint32_t at);

    void Execute(uint32_t instruction, uint32_t at);
    void ExecuteSpecial(uint32_t instruction, uint32_t at);
    void ExecuteCop0(uint32_t instruction, uint32_t at);
    void ExecuteCop2(uint32_t instruction, uint32_t at);
};

} // namespace rr::interp
