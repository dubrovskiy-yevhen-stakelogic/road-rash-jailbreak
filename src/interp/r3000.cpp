#include "interp/r3000.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "interp/devices.h"
#include "interp/trace.h"

namespace rr::interp {
namespace {

constexpr uint32_t kRegionMask[8] = {
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, // KUSEG
    0x7FFFFFFFu,                                        // KSEG0
    0x1FFFFFFFu,                                        // KSEG1
    0xFFFFFFFFu, 0xFFFFFFFFu                            // KSEG2
};

inline uint32_t Physical(uint32_t address) { return address & kRegionMask[address >> 29]; }

inline uint32_t SignExtend16(uint32_t v) { return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v))); }

std::string Hex(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", v);
    return buf;
}

} // namespace

const char* TrapKindName(TrapKind kind) {
    switch (kind) {
    case TrapKind::None: return "none";
    case TrapKind::Halted: return "halted";
    case TrapKind::StepLimit: return "step-limit";
    case TrapKind::UnknownInstruction: return "unknown-instruction";
    case TrapKind::UnimplementedInstruction: return "unimplemented-instruction";
    case TrapKind::MmioAccess: return "mmio-access";
    case TrapKind::UnmappedAddress: return "unmapped-address";
    case TrapKind::ReadOnlyWrite: return "write-to-rom";
    case TrapKind::AddressError: return "address-error";
    case TrapKind::ArithmeticOverflow: return "arithmetic-overflow";
    case TrapKind::Break: return "break";
    case TrapKind::Syscall: return "syscall";
    case TrapKind::BiosCall: return "bios-vector";
    case TrapKind::CoprocessorUnusable: return "coprocessor-unusable";
    case TrapKind::IsolatedCache: return "isolated-cache";
    case TrapKind::GteUnimplemented: return "gte-unimplemented";
    case TrapKind::DivisionByZero: return "division-by-zero";
    case TrapKind::Hook: return "hook";
    }
    return "?";
}

std::string Trap::ToString() const {
    std::string s = TrapKindName(kind);
    s += " at pc=" + Hex(pc) + " instr=" + Hex(instruction);
    if (kind == TrapKind::MmioAccess || kind == TrapKind::UnmappedAddress ||
        kind == TrapKind::AddressError || kind == TrapKind::ReadOnlyWrite ||
        kind == TrapKind::IsolatedCache || kind == TrapKind::BiosCall) {
        s += " address=" + Hex(address);
    }
    if (!detail.empty()) s += " (" + detail + ")";
    return s;
}

// ------------------------------------------------------------------------------------- Memory

Memory::Memory()
    : ram_(kRamSize, 0), scratchpad_(kScratchpadSize, 0), bios_(kBiosSize, 0) {}

Memory::Region Memory::Classify(uint32_t address, uint8_t** host) {
    const uint32_t phys = Physical(address);
    if (phys < 0x00800000u) {
        *host = ram_.data() + (phys & (kRamSize - 1));
        return Region::Ram;
    }
    if (phys >= 0x1F800000u && phys < 0x1F800000u + kScratchpadSize) {
        *host = scratchpad_.data() + (phys - 0x1F800000u);
        return Region::Scratchpad;
    }
    if (phys >= 0x1F801000u && phys < 0x1F803000u) {
        *host = nullptr;
        return Region::Mmio;
    }
    if (phys >= 0x1FC00000u && phys < 0x1FC00000u + kBiosSize) {
        *host = bios_.data() + (phys - 0x1FC00000u);
        return Region::Bios;
    }
    *host = nullptr;
    return Region::Unmapped;
}

Memory::Region Memory::Classify(uint32_t address, const uint8_t** host) const {
    uint8_t* p = nullptr;
    const Region r = const_cast<Memory*>(this)->Classify(address, &p);
    *host = p;
    return r;
}

bool Memory::IsRamAddress(uint32_t address) { return Physical(address) < 0x00800000u; }
uint32_t Memory::RamOffset(uint32_t address) { return Physical(address) & (kRamSize - 1); }

bool Memory::ReadBlock(uint32_t address, void* dst, size_t length) const {
    auto* out = static_cast<uint8_t*>(dst);
    for (size_t i = 0; i < length; ++i) {
        const uint8_t* p = nullptr;
        if (Classify(static_cast<uint32_t>(address + i), &p) == Region::Mmio || p == nullptr) return false;
        out[i] = *p;
    }
    return true;
}

bool Memory::WriteBlock(uint32_t address, const void* src, size_t length) {
    const auto* in = static_cast<const uint8_t*>(src);
    for (size_t i = 0; i < length; ++i) {
        uint8_t* p = nullptr;
        const Region r = Classify(static_cast<uint32_t>(address + i), &p);
        if (p == nullptr || r == Region::Mmio) return false;
        *p = in[i];
    }
    return true;
}

uint32_t Memory::PeekWord(uint32_t address) const {
    uint32_t v = 0;
    ReadBlock(address, &v, 4);
    return v;
}

void Memory::PokeWord(uint32_t address, uint32_t value) { WriteBlock(address, &value, 4); }

uint8_t Memory::PeekByte(uint32_t address) const {
    uint8_t v = 0;
    ReadBlock(address, &v, 1);
    return v;
}

// ---------------------------------------------------------------------------------------- Cpu

Cpu::Cpu(Memory& memory) : memory_(memory) {
    hookBitmap_.assign(Memory::kRamSize / 4 / 64, 0);
    Reset();
}

void Cpu::Reset() {
    std::memset(regs, 0, sizeof(regs));
    std::memset(cop0, 0, sizeof(cop0));
    hi = lo = 0;
    pc = npc = 0;
    loadDelayReg = kNoLoadDelay;
    loadDelayValue = 0;
    newLoadReg_ = kNoLoadDelay;
    newLoadValue_ = 0;
    inDelaySlot_ = branchTaken_ = false;
    instructionsRetired = 0;
    trap_ = {};
    gte_.Reset();
}

void Cpu::AddHook(uint32_t address, std::string name) {
    if (!Memory::IsRamAddress(address)) return;
    const uint32_t word = Memory::RamOffset(address) >> 2;
    hookBitmap_[word >> 6] |= (uint64_t{1} << (word & 63));
    hookNames_.emplace_back(address, std::move(name));
    hasHooks_ = true;
}

void Cpu::ClearHooks() {
    std::fill(hookBitmap_.begin(), hookBitmap_.end(), uint64_t{0});
    hookNames_.clear();
    hasHooks_ = false;
}

void Cpu::ObserveCalls(uint32_t address) {
    if (!Memory::IsRamAddress(address)) return;
    if (observeBitmap_.empty()) observeBitmap_.assign(Memory::kRamSize / 4 / 64, 0);
    const uint32_t word = Memory::RamOffset(address) >> 2;
    observeBitmap_[word >> 6] |= (uint64_t{1} << (word & 63));
    hasObservers_ = true;
}

void Cpu::ObserveCallFrameOut(uint32_t address, int argIndex, uint32_t bytes) {
    if (argIndex < 0 || argIndex > 7 || bytes == 0 || bytes > kMaxFrameOut) return;
    frameOutSpecs_.push_back(FrameOutSpec{address, argIndex, bytes});
}

void Cpu::ClearCallObservers() {
    std::fill(observeBitmap_.begin(), observeBitmap_.end(), uint64_t{0});
    hasObservers_ = false;
    callObservations.clear();
    frameOutSpecs_.clear();
    pendingFrameOut_.clear();
}

void Cpu::SetTrap(TrapKind kind, uint32_t at, uint32_t instruction, uint32_t address, std::string detail) {
    if (trap_.kind != TrapKind::None) return;
    trap_.kind = kind;
    trap_.pc = at;
    trap_.instruction = instruction;
    trap_.address = address;
    trap_.detail = std::move(detail);
}

bool Cpu::Fetch(uint32_t address, uint32_t& out) {
    if (address & 3u) {
        SetTrap(TrapKind::AddressError, address, 0, address, "misaligned instruction fetch");
        return false;
    }
    const uint8_t* host = nullptr;
    const Memory::Region r = memory_.Classify(address, &host);
    if (r == Memory::Region::Mmio) {
        SetTrap(TrapKind::MmioAccess, address, 0, address, "instruction fetch from MMIO");
        return false;
    }
    if (host == nullptr) {
        SetTrap(TrapKind::UnmappedAddress, address, 0, address, "instruction fetch from unmapped memory");
        return false;
    }
    std::memcpy(&out, host, 4);
    return true;
}

bool Cpu::ConcessionLoad(uint32_t address, uint32_t size, uint32_t& out) const {
    const uint32_t reg = address & 0x1FFFFFFFu;
    // The root-counter value registers, and only on a load. See `rootCountersAreConstant`.
    if (rootCountersAreConstant) {
        if (reg == 0x1F801100u || reg == 0x1F801110u || reg == 0x1F801120u) {
            out = rootCounterValue;
            if (size < 4) out &= (size == 1) ? 0xFFu : 0xFFFFu;
            return true;
        }
    }
    // The SPU control register file: aligned halfword or word loads only, ENDX excluded.
    if (spuControlRegisterFile && size >= 2 && (reg & (size - 1u)) == 0 && reg >= kSpuControlFirst &&
        reg + size <= kSpuControlEnd) {
        const uint32_t i = (reg - kSpuControlFirst) >> 1;
        out = spuControl[i];
        if (size == 4) out |= static_cast<uint32_t>(spuControl[i + 1]) << 16;
        return true;
    }
    return false;
}

bool Cpu::ConcessionStore(uint32_t address, uint32_t size, uint32_t value) {
    const uint32_t reg = address & 0x1FFFFFFFu;
    if (spuControlRegisterFile && size >= 2 && (reg & (size - 1u)) == 0 && reg >= kSpuControlFirst &&
        reg + size <= kSpuControlEnd) {
        const uint32_t i = (reg - kSpuControlFirst) >> 1;
        spuControl[i] = static_cast<uint16_t>(value);
        if (size == 4) spuControl[i + 1] = static_cast<uint16_t>(value >> 16);
        if (spuStoreLog != nullptr) spuStoreLog->push_back({reg, size, (size == 2) ? (value & 0xFFFFu) : value});
        return true;
    }
    // A byte store into the file's range is NOT silently dropped when the file is on: the file
    // would then disagree with what the guest believes it wrote. It traps instead.
    if (spuControlRegisterFile && size == 1 && reg >= kSpuControlFirst && reg < kSpuControlEnd)
        return false;
    // The SPU register window, and only on a store. See `spuWritesAreDropped`.
    if (spuWritesAreDropped && reg >= 0x1F801C00u && reg <= 0x1F801E7Fu) {
        if (spuStoreLog != nullptr) {
            const uint32_t v = (size == 1) ? (value & 0xFFu) : (size == 2) ? (value & 0xFFFFu) : value;
            spuStoreLog->push_back({reg, size, v});
        }
        return true;
    }
    return false;
}

bool Cpu::Load(uint32_t address, uint32_t size, uint32_t& out, uint32_t at, uint32_t instruction) {
    if (CacheIsolated()) {
        SetTrap(TrapKind::IsolatedCache, at, instruction, address, "load while SR.IsC is set");
        return false;
    }
    if ((size == 2 && (address & 1u)) || (size == 4 && (address & 3u))) {
        SetTrap(TrapKind::AddressError, at, instruction, address, "misaligned load");
        return false;
    }
    const uint8_t* host = nullptr;
    const Memory::Region r = memory_.Classify(address, &host);
    if (r == Memory::Region::Mmio) {
        if (devices != nullptr) {
            devices->currentPc = at;
            devices->seq = instructionsRetired;
            std::string why;
            if (!devices->Read(address, size, out, why)) {
                SetTrap(TrapKind::MmioAccess, at, instruction, address, why);
                return false;
            }
            if (size < 4) out &= (size == 1) ? 0xFFu : 0xFFFFu;
            return true;
        }
        // The named concessions (root counters, the SPU control register file). See their fields.
        if (ConcessionLoad(address, size, out)) return true;
        SetTrap(TrapKind::MmioAccess, at, instruction, address, "load from a hardware register");
        return false;
    }
    if (host == nullptr) {
        // The cache control register lives in KSEG2 rather than in the MMIO window.
        if (devices != nullptr && address == 0xFFFE0130u) { out = cacheControl; return true; }
        SetTrap(TrapKind::UnmappedAddress, at, instruction, address, "load from unmapped memory");
        return false;
    }
    out = 0;
    std::memcpy(&out, host, size);
    if (tracer != nullptr && tracer->hasWatches() && tracer->MayWatch(address))
        tracer->OnMemory(instructionsRetired, at, address, size, out, false);
    return true;
}

bool Cpu::Store(uint32_t address, uint32_t size, uint32_t value, uint32_t at, uint32_t instruction) {
    if (CacheIsolated()) {
        SetTrap(TrapKind::IsolatedCache, at, instruction, address, "store while SR.IsC is set");
        return false;
    }
    if ((size == 2 && (address & 1u)) || (size == 4 && (address & 3u))) {
        SetTrap(TrapKind::AddressError, at, instruction, address, "misaligned store");
        return false;
    }
    uint8_t* host = nullptr;
    const Memory::Region r = memory_.Classify(address, &host);
    if (r == Memory::Region::Mmio) {
        // a watched hardware register (e.g. the SPU window): the store is recorded as it is issued
        if (tracer != nullptr && tracer->hasWatches() && tracer->MayWatch(address))
            tracer->OnMemory(instructionsRetired, at, address, size,
                             size == 1 ? (value & 0xFFu) : size == 2 ? (value & 0xFFFFu) : value, true);
        if (devices != nullptr) {
            devices->currentPc = at;
            devices->seq = instructionsRetired;
            std::string why;
            uint32_t masked = value;
            if (size == 1) masked &= 0xFFu;
            else if (size == 2) masked &= 0xFFFFu;
            if (!devices->Write(address, size, masked, why)) {
                SetTrap(TrapKind::MmioAccess, at, instruction, address, why);
                return false;
            }
            return true;
        }
        // The SPU control register file, then the SPU register window (store only). See
        // `spuControlRegisterFile` and `spuWritesAreDropped`.
        if (ConcessionStore(address, size, value)) return true;
        SetTrap(TrapKind::MmioAccess, at, instruction, address, "store to a hardware register");
        return false;
    }
    if (r == Memory::Region::Bios) {
        SetTrap(TrapKind::ReadOnlyWrite, at, instruction, address, "store into the BIOS image");
        return false;
    }
    if (host == nullptr) {
        if (devices != nullptr && address == 0xFFFE0130u) { cacheControl = value; return true; }
        SetTrap(TrapKind::UnmappedAddress, at, instruction, address, "store to unmapped memory");
        return false;
    }
    std::memcpy(host, &value, size);
    if (tracer != nullptr && tracer->hasWatches() && tracer->MayWatch(address))
        tracer->OnMemory(instructionsRetired, at, address, size, value, true);
    return true;
}

Trap Cpu::Step() {
    trap_ = {};
    const uint32_t at = pc;
    uint32_t instruction = 0;
    if (!Fetch(at, instruction)) return trap_;

    // A probe records exactly what the instruction at `at` reads: the architectural register file
    // WITHOUT the pending load-delay value, because on an R3000 that value only becomes visible to
    // the instruction after this one. A probe placed immediately after an `lw` therefore shows the
    // old register, which is the truth rather than a convenience.
    if (tracer != nullptr && tracer->hasProbes() && tracer->MayProbe(at))
        tracer->OnProbe(instructionsRetired, at, regs, hi, lo);

    pc = npc;
    npc = pc + 4;
    inDelaySlot_ = branchTaken_;
    branchTaken_ = false;

    newLoadReg_ = kNoLoadDelay;
    newLoadValue_ = 0;

    Execute(instruction, at);

    // The load issued by the previous instruction lands now, after this one read its operands.
    if (loadDelayReg != kNoLoadDelay && loadDelayReg != 0) regs[loadDelayReg] = loadDelayValue;
    loadDelayReg = newLoadReg_;
    loadDelayValue = newLoadValue_;
    regs[0] = 0;

    ++instructionsRetired;
    return trap_;
}

// A device interrupt is visible in CAUSE.IP2 (hardware IRQ line 0 on the PlayStation, which the
// interrupt controller drives). It is taken only when SR.IEc and SR.IM2 are both set.
bool Cpu::InterruptPending() {
    if (devices == nullptr) return false;
    if (devices->IrqPending()) cop0[13] |= (1u << 10);
    else cop0[13] &= ~(1u << 10);
    if (!takeInterrupts) return false;
    if ((cop0[12] & 1u) == 0) return false;               // SR.IEc
    return (cop0[12] & cop0[13] & 0x0000FF00u) != 0;      // SR.IM & CAUSE.IP
}

void Cpu::TakeInterrupt() {
    // A load issued by the previous instruction has already reached write-back on real hardware by
    // the time the exception is recognised, so commit it before the handler runs.
    if (loadDelayReg != kNoLoadDelay && loadDelayReg != 0) regs[loadDelayReg] = loadDelayValue;
    loadDelayReg = kNoLoadDelay;
    loadDelayValue = 0;
    regs[0] = 0;

    cop0[14] = pc;                                        // EPC
    cop0[13] = (cop0[13] & ~0x8000007Cu);                 // BD = 0, ExcCode = 0 (interrupt)
    cop0[12] = (cop0[12] & ~0x3Fu) | ((cop0[12] << 2) & 0x3Fu); // push the KU/IE stack
    lastExceptionVector = (cop0[12] & (1u << 22)) ? 0xBFC00180u : 0x80000080u;
    pc = lastExceptionVector;
    npc = pc + 4;
    ++interruptsTaken;
}

// The general exception entry, used for SYSCALL and BREAK when `handleExceptions` is on. `at` is
// the address of the offending instruction; when it sits in a branch delay slot EPC points at the
// branch and CAUSE.BD is set, exactly as the hardware does it, because the kernel's handler adds 4
// to EPC to resume and would otherwise skip the branch.
void Cpu::RaiseException(uint32_t excCode, uint32_t at) {
    const bool inDelay = inDelaySlot_;
    cop0[14] = inDelay ? (at - 4u) : at;
    cop0[13] = (cop0[13] & ~0x8000007Cu) | ((excCode & 0x1Fu) << 2) | (inDelay ? 0x80000000u : 0u);
    cop0[12] = (cop0[12] & ~0x3Fu) | ((cop0[12] << 2) & 0x3Fu);
    lastExceptionVector = (cop0[12] & (1u << 22)) ? 0xBFC00180u : 0x80000080u;
    pc = lastExceptionVector;
    npc = pc + 4;
    branchTaken_ = false;
}

Trap Cpu::Run(uint32_t sentinel, uint64_t maxSteps) {
    uint64_t steps = 0;
    for (;;) {
        // Never between a branch and its delay slot: the delay slot has to retire first, which is
        // also why CAUSE.BD never has to be set here.
        if (!branchTaken_ && InterruptPending()) TakeInterrupt();
        if (pc == sentinel) {
            Trap t;
            t.kind = TrapKind::Halted;
            t.pc = pc;
            return t;
        }
        if (hasHooks_) {
            if (Memory::IsRamAddress(pc)) {
                const uint32_t word = Memory::RamOffset(pc) >> 2;
                if (hookBitmap_[word >> 6] & (uint64_t{1} << (word & 63))) {
                    lastHookName_.clear();
                    for (const auto& h : hookNames_) {
                        if (h.first == pc) { lastHookName_ = h.second; break; }
                    }
                    Trap t;
                    t.kind = TrapKind::Hook;
                    t.pc = pc;
                    t.detail = lastHookName_;
                    return t;
                }
            }
        }
        if (hasObservers_) {
            if (Memory::IsRamAddress(pc)) {
                const uint32_t word = Memory::RamOffset(pc) >> 2;
                if (observeBitmap_[word >> 6] & (uint64_t{1} << (word & 63))) {
                    CallObservation o;
                    o.address = pc;
                    o.a[0] = regs[4];
                    o.a[1] = regs[5];
                    o.a[2] = regs[6];
                    o.a[3] = regs[7];
                    // The eight o32 stack-argument slots sp+16..sp+44 in the CALLER's frame. They
                    // are already stored by the time `pc` reaches the entry, so reading them here
                    // is reading the arguments, not guessing them. PeekWord is the non-faulting
                    // read: a callee that takes fewer arguments simply leaves leftovers here, and
                    // the bench only compares as many as the callee's own disassembly says it
                    // takes (HitSpeed 0x80080D1C takes seven).
                    for (int k = 0; k < 8; ++k)
                        o.a[4 + k] =
                            memory_.PeekWord(regs[29] + 16u + 4u * static_cast<uint32_t>(k));
                    o.sp = regs[29];
                    callObservations.push_back(o);
                    // A caller-frame output buffer cannot be read yet - the callee has not written
                    // it. Remember where to pick it up from when the callee returns.
                    for (const FrameOutSpec& s : frameOutSpecs_) {
                        if (s.address != pc) continue;
                        pendingFrameOut_.push_back(PendingFrameOut{
                            regs[31], regs[29], o.a[s.argIndex], s.bytes,
                            callObservations.size() - 1, s.argIndex});
                    }
                }
            }
        }
        if (!pendingFrameOut_.empty()) {
            for (size_t i = pendingFrameOut_.size(); i-- > 0;) {
                const PendingFrameOut& p = pendingFrameOut_[i];
                if (pc != p.ra || regs[29] < p.sp) continue;
                CallObservation& o = callObservations[p.observation];
                if (FrameOutCapture* f = o.FrameOutFor(p.arg)) {
                    f->len = p.bytes;
                    memory_.ReadBlock(p.src, f->bytes, p.bytes);
                }
                pendingFrameOut_.erase(pendingFrameOut_.begin() + static_cast<std::ptrdiff_t>(i));
            }
        }
        const Trap t = Step();
        if (t) return t;
        if (devices != nullptr) devices->Advance(instructionsRetired);
        if (maxSteps != 0 && ++steps >= maxSteps) {
            Trap limit;
            limit.kind = TrapKind::StepLimit;
            limit.pc = pc;
            return limit;
        }
    }
}

void Cpu::Execute(uint32_t instruction, uint32_t at) {
    const uint32_t op = instruction >> 26;
    const uint32_t rs = (instruction >> 21) & 0x1F;
    const uint32_t rt = (instruction >> 16) & 0x1F;
    const uint32_t imm = instruction & 0xFFFF;
    const uint32_t simm = SignExtend16(imm);

    const auto branch = [&](bool taken) {
        if (inDelaySlot_) {
            SetTrap(TrapKind::UnimplementedInstruction, at, instruction, 0,
                    "branch inside a branch delay slot (architecturally undefined)");
            return;
        }
        if (taken) {
            npc = at + 4 + (simm << 2);
            branchTaken_ = true;
        }
    };

    switch (op) {
    case 0x00: ExecuteSpecial(instruction, at); return;

    case 0x01: { // REGIMM
        const uint32_t sub = rt;
        const int32_t v = static_cast<int32_t>(regs[rs]);
        const bool link = (sub & 0x1E) == 0x10;
        const bool taken = (sub & 1) ? (v >= 0) : (v < 0);
        if (link) SetReg(31, at + 8);
        branch(taken);
        return;
    }

    case 0x02: // j
    case 0x03: // jal
        if (inDelaySlot_) {
            SetTrap(TrapKind::UnimplementedInstruction, at, instruction, 0,
                    "jump inside a branch delay slot (architecturally undefined)");
            return;
        }
        if (op == 0x03) SetReg(31, at + 8);
        npc = ((at + 4) & 0xF0000000u) | ((instruction & 0x03FFFFFFu) << 2);
        branchTaken_ = true;
        if (op == 0x03 && tracer != nullptr && tracer->traceCalls) {
            if (tracer->calls.size() >= tracer->maxCalls) tracer->callsTruncated = true;
            else tracer->calls.push_back({instructionsRetired, at, npc, {regs[4], regs[5], regs[6], regs[7]},
                                          regs[29]});
        }
        if (trapOnBiosVector && (npc == 0xA0 || npc == 0xB0 || npc == 0xC0)) {
            SetTrap(TrapKind::BiosCall, at, instruction, npc,
                    "jump through a BIOS vector; $t1 selects the kernel function");
        }
        return;

    case 0x04: branch(regs[rs] == regs[rt]); return;                                    // beq
    case 0x05: branch(regs[rs] != regs[rt]); return;                                    // bne
    case 0x06: branch(static_cast<int32_t>(regs[rs]) <= 0); return;                     // blez
    case 0x07: branch(static_cast<int32_t>(regs[rs]) > 0); return;                      // bgtz

    case 0x08: { // addi
        const int32_t a = static_cast<int32_t>(regs[rs]);
        const int32_t b = static_cast<int32_t>(simm);
        const int32_t r = static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
        if (((a ^ r) & (b ^ r)) < 0) {
            SetTrap(TrapKind::ArithmeticOverflow, at, instruction, 0, "addi overflow");
            return;
        }
        SetReg(rt, static_cast<uint32_t>(r));
        return;
    }
    case 0x09: SetReg(rt, regs[rs] + simm); return;                                     // addiu
    case 0x0A: SetReg(rt, static_cast<int32_t>(regs[rs]) < static_cast<int32_t>(simm) ? 1u : 0u); return;
    case 0x0B: SetReg(rt, regs[rs] < simm ? 1u : 0u); return;                           // sltiu
    case 0x0C: SetReg(rt, regs[rs] & imm); return;                                      // andi
    case 0x0D: SetReg(rt, regs[rs] | imm); return;                                      // ori
    case 0x0E: SetReg(rt, regs[rs] ^ imm); return;                                      // xori
    case 0x0F: SetReg(rt, imm << 16); return;                                           // lui

    case 0x10: ExecuteCop0(instruction, at); return;
    case 0x12: ExecuteCop2(instruction, at); return;
    case 0x11: // COP1
    case 0x13: // COP3
        SetTrap(TrapKind::CoprocessorUnusable, at, instruction, 0,
                "COP1/COP3 do not exist on the PlayStation");
        return;

    case 0x20: { // lb
        uint32_t v = 0;
        const uint32_t a = regs[rs] + simm;
        if (!Load(a, 1, v, at, instruction)) return;
        newLoadReg_ = static_cast<uint8_t>(rt);
        newLoadValue_ = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(v)));
        return;
    }
    case 0x21: { // lh
        uint32_t v = 0;
        const uint32_t a = regs[rs] + simm;
        if (!Load(a, 2, v, at, instruction)) return;
        newLoadReg_ = static_cast<uint8_t>(rt);
        newLoadValue_ = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(v)));
        return;
    }
    case 0x24: { // lbu
        uint32_t v = 0;
        const uint32_t a = regs[rs] + simm;
        if (!Load(a, 1, v, at, instruction)) return;
        newLoadReg_ = static_cast<uint8_t>(rt);
        newLoadValue_ = v & 0xFFu;
        return;
    }
    case 0x25: { // lhu
        uint32_t v = 0;
        const uint32_t a = regs[rs] + simm;
        if (!Load(a, 2, v, at, instruction)) return;
        newLoadReg_ = static_cast<uint8_t>(rt);
        newLoadValue_ = v & 0xFFFFu;
        return;
    }
    case 0x23: { // lw
        uint32_t v = 0;
        const uint32_t a = regs[rs] + simm;
        if (!Load(a, 4, v, at, instruction)) return;
        newLoadReg_ = static_cast<uint8_t>(rt);
        newLoadValue_ = v;
        return;
    }
    case 0x22:   // lwl
    case 0x26: { // lwr
        const uint32_t a = regs[rs] + simm;
        uint32_t word = 0;
        if (!Load(a & ~3u, 4, word, at, instruction)) return;
        // LWL/LWR is the one case where a load reads the value still sitting in the load delay
        // slot, which is what makes the canonical unaligned-load pair work.
        const uint32_t base = (loadDelayReg == rt) ? loadDelayValue : regs[rt];
        const uint32_t shift = (a & 3u) * 8u;
        uint32_t v;
        if (op == 0x22) {
            const uint32_t s = 24u - shift;
            v = (base & (0x00FFFFFFu >> shift)) | (word << s);
        } else {
            const uint32_t mask = (shift == 0) ? 0u : (0xFFFFFF00u << (24u - shift));
            v = (base & mask) | (word >> shift);
        }
        newLoadReg_ = static_cast<uint8_t>(rt);
        newLoadValue_ = v;
        return;
    }

    case 0x28: Store(regs[rs] + simm, 1, regs[rt], at, instruction); return;            // sb
    case 0x29: Store(regs[rs] + simm, 2, regs[rt], at, instruction); return;            // sh
    case 0x2B: Store(regs[rs] + simm, 4, regs[rt], at, instruction); return;            // sw
    case 0x2A:   // swl
    case 0x2E: { // swr
        const uint32_t a = regs[rs] + simm;
        uint32_t word = 0;
        if (!Load(a & ~3u, 4, word, at, instruction)) return;
        const uint32_t shift = (a & 3u) * 8u;
        uint32_t v;
        if (op == 0x2A) {
            const uint32_t s = 24u - shift;
            v = (word & ~(0xFFFFFFFFu >> s)) | (regs[rt] >> s);
        } else {
            v = (word & ~(0xFFFFFFFFu << shift)) | (regs[rt] << shift);
        }
        Store(a & ~3u, 4, v, at, instruction);
        return;
    }

    case 0x32: { // lwc2
        uint32_t v = 0;
        const uint32_t a = regs[rs] + simm;
        if (!Cop2Enabled()) {
            SetTrap(TrapKind::CoprocessorUnusable, at, instruction, 0, "lwc2 with SR.CU2 clear");
            return;
        }
        if (!Load(a, 4, v, at, instruction)) return;
        gte_.WriteData(rt, v);
        return;
    }
    case 0x3A: { // swc2
        if (!Cop2Enabled()) {
            SetTrap(TrapKind::CoprocessorUnusable, at, instruction, 0, "swc2 with SR.CU2 clear");
            return;
        }
        Store(regs[rs] + simm, 4, gte_.ReadData(rt), at, instruction);
        return;
    }
    case 0x30: case 0x31: case 0x33: // lwc0 / lwc1 / lwc3
    case 0x38: case 0x39: case 0x3B: // swc0 / swc1 / swc3
        SetTrap(TrapKind::CoprocessorUnusable, at, instruction, 0,
                "coprocessor load/store for a coprocessor that does not exist");
        return;

    default:
        SetTrap(TrapKind::UnknownInstruction, at, instruction, 0, "undefined primary opcode");
        return;
    }
}

void Cpu::ExecuteSpecial(uint32_t instruction, uint32_t at) {
    const uint32_t funct = instruction & 0x3F;
    const uint32_t rs = (instruction >> 21) & 0x1F;
    const uint32_t rt = (instruction >> 16) & 0x1F;
    const uint32_t rd = (instruction >> 11) & 0x1F;
    const uint32_t sa = (instruction >> 6) & 0x1F;

    switch (funct) {
    case 0x00: SetReg(rd, regs[rt] << sa); return;                                        // sll
    case 0x02: SetReg(rd, regs[rt] >> sa); return;                                        // srl
    case 0x03: SetReg(rd, static_cast<uint32_t>(static_cast<int32_t>(regs[rt]) >> sa)); return; // sra
    case 0x04: SetReg(rd, regs[rt] << (regs[rs] & 31u)); return;                          // sllv
    case 0x06: SetReg(rd, regs[rt] >> (regs[rs] & 31u)); return;                          // srlv
    case 0x07: SetReg(rd, static_cast<uint32_t>(static_cast<int32_t>(regs[rt]) >> (regs[rs] & 31u))); return;

    case 0x08:   // jr
    case 0x09: { // jalr
        if (inDelaySlot_) {
            SetTrap(TrapKind::UnimplementedInstruction, at, instruction, 0,
                    "jump inside a branch delay slot (architecturally undefined)");
            return;
        }
        const uint32_t target = regs[rs];
        if (funct == 0x09) SetReg(rd, at + 8);
        if (target & 3u) {
            SetTrap(TrapKind::AddressError, at, instruction, target, "jump to a misaligned address");
            return;
        }
        npc = target;
        branchTaken_ = true;
        if (funct == 0x09 && tracer != nullptr && tracer->traceCalls) {
            if (tracer->calls.size() >= tracer->maxCalls) tracer->callsTruncated = true;
            else tracer->calls.push_back({instructionsRetired, at, target, {regs[4], regs[5], regs[6], regs[7]},
                                          regs[29]});
        }
        if (trapOnBiosVector && (target == 0xA0 || target == 0xB0 || target == 0xC0)) {
            SetTrap(TrapKind::BiosCall, at, instruction, target,
                    "jump through a BIOS vector; $t1 selects the kernel function");
        }
        return;
    }

    case 0x0C:
        if (handleExceptions) { ++syscallsTaken; RaiseException(8, at); return; }
        SetTrap(TrapKind::Syscall, at, instruction, 0, "syscall");
        return;
    case 0x0D:
        if (handleExceptions) { RaiseException(9, at); return; }
        SetTrap(TrapKind::Break, at, instruction, (instruction >> 6) & 0xFFFFF, "break");
        return;

    case 0x10: SetReg(rd, hi); return;                                                    // mfhi
    case 0x11: hi = regs[rs]; return;                                                     // mthi
    case 0x12: SetReg(rd, lo); return;                                                    // mflo
    case 0x13: lo = regs[rs]; return;                                                     // mtlo

    case 0x18: { // mult
        const int64_t r = static_cast<int64_t>(static_cast<int32_t>(regs[rs])) *
                          static_cast<int64_t>(static_cast<int32_t>(regs[rt]));
        lo = static_cast<uint32_t>(static_cast<uint64_t>(r) & 0xFFFFFFFFu);
        hi = static_cast<uint32_t>(static_cast<uint64_t>(r) >> 32);
        return;
    }
    case 0x19: { // multu
        const uint64_t r = static_cast<uint64_t>(regs[rs]) * static_cast<uint64_t>(regs[rt]);
        lo = static_cast<uint32_t>(r & 0xFFFFFFFFu);
        hi = static_cast<uint32_t>(r >> 32);
        return;
    }
    case 0x1A: { // div - the R3000 defines every corner case, no exception is raised
        const int32_t n = static_cast<int32_t>(regs[rs]);
        const int32_t d = static_cast<int32_t>(regs[rt]);
        if (d == 0) {
            lo = (n >= 0) ? 0xFFFFFFFFu : 1u;
            hi = static_cast<uint32_t>(n);
        } else if (static_cast<uint32_t>(n) == 0x80000000u && d == -1) {
            lo = 0x80000000u;
            hi = 0;
        } else {
            lo = static_cast<uint32_t>(n / d);
            hi = static_cast<uint32_t>(n % d);
        }
        return;
    }
    case 0x1B: { // divu
        const uint32_t n = regs[rs];
        const uint32_t d = regs[rt];
        if (d == 0) {
            lo = 0xFFFFFFFFu;
            hi = n;
        } else {
            lo = n / d;
            hi = n % d;
        }
        return;
    }

    case 0x20: { // add
        const int32_t a = static_cast<int32_t>(regs[rs]);
        const int32_t b = static_cast<int32_t>(regs[rt]);
        const int32_t r = static_cast<int32_t>(regs[rs] + regs[rt]);
        if (((a ^ r) & (b ^ r)) < 0) {
            SetTrap(TrapKind::ArithmeticOverflow, at, instruction, 0, "add overflow");
            return;
        }
        SetReg(rd, static_cast<uint32_t>(r));
        return;
    }
    case 0x21: SetReg(rd, regs[rs] + regs[rt]); return;                                   // addu
    case 0x22: { // sub
        const int32_t a = static_cast<int32_t>(regs[rs]);
        const int32_t b = static_cast<int32_t>(regs[rt]);
        const int32_t r = static_cast<int32_t>(regs[rs] - regs[rt]);
        if (((a ^ b) & (a ^ r)) < 0) {
            SetTrap(TrapKind::ArithmeticOverflow, at, instruction, 0, "sub overflow");
            return;
        }
        SetReg(rd, static_cast<uint32_t>(r));
        return;
    }
    case 0x23: SetReg(rd, regs[rs] - regs[rt]); return;                                   // subu
    case 0x24: SetReg(rd, regs[rs] & regs[rt]); return;                                   // and
    case 0x25: SetReg(rd, regs[rs] | regs[rt]); return;                                   // or
    case 0x26: SetReg(rd, regs[rs] ^ regs[rt]); return;                                   // xor
    case 0x27: SetReg(rd, ~(regs[rs] | regs[rt])); return;                                // nor
    case 0x2A: SetReg(rd, static_cast<int32_t>(regs[rs]) < static_cast<int32_t>(regs[rt]) ? 1u : 0u); return;
    case 0x2B: SetReg(rd, regs[rs] < regs[rt] ? 1u : 0u); return;                         // sltu

    default:
        SetTrap(TrapKind::UnknownInstruction, at, instruction, 0, "undefined SPECIAL function");
        return;
    }
}

void Cpu::ExecuteCop0(uint32_t instruction, uint32_t at) {
    const uint32_t rs = (instruction >> 21) & 0x1F;
    const uint32_t rt = (instruction >> 16) & 0x1F;
    const uint32_t rd = (instruction >> 11) & 0x1F;

    switch (rs) {
    case 0x00: // mfc0 - like any load, it lands one instruction later
        newLoadReg_ = static_cast<uint8_t>(rt);
        newLoadValue_ = cop0[rd & 15u];
        return;
    case 0x04: // mtc0
        if ((rd & 15u) == 13u) {
            // CAUSE: only the two software interrupt bits are writable.
            cop0[13] = (cop0[13] & ~0x300u) | (regs[rt] & 0x300u);
        } else if ((rd & 15u) == 15u) {
            // PRID is read-only.
        } else {
            cop0[rd & 15u] = regs[rt];
        }
        return;
    case 0x10: // rfe (the only COP0 "co-processor operation" the PS1 uses)
        if ((instruction & 0x3F) != 0x10) {
            SetTrap(TrapKind::UnimplementedInstruction, at, instruction, 0, "unsupported COP0 operation");
            return;
        }
        cop0[12] = (cop0[12] & ~0xFu) | ((cop0[12] >> 2) & 0xFu);
        return;
    default:
        SetTrap(TrapKind::UnimplementedInstruction, at, instruction, 0, "unsupported COP0 access");
        return;
    }
}

void Cpu::ExecuteCop2(uint32_t instruction, uint32_t at) {
    if (!Cop2Enabled()) {
        SetTrap(TrapKind::CoprocessorUnusable, at, instruction, 0,
                "COP2 used while SR.CU2 is clear");
        return;
    }
    const uint32_t rs = (instruction >> 21) & 0x1F;
    const uint32_t rt = (instruction >> 16) & 0x1F;
    const uint32_t rd = (instruction >> 11) & 0x1F;

    if (instruction & (1u << 25)) { // a GTE command
        if (tracer != nullptr && tracer->traceCop2) {
            ++tracer->cop2Count;
            if (tracer->cop2.size() >= tracer->maxCop2) {
                tracer->cop2Truncated = true;
                gte_.Execute(instruction);
            } else {
                Cop2Record rec;
                rec.seq = instructionsRetired;
                rec.pc = at;
                rec.instruction = instruction;
                std::memcpy(rec.inDr, gte_.dr, sizeof(rec.inDr));
                std::memcpy(rec.inCr, gte_.cr, sizeof(rec.inCr));
                gte_.Execute(instruction);
                std::memcpy(rec.outDr, gte_.dr, sizeof(rec.outDr));
                rec.outFlag = gte_.cr[31];
                tracer->cop2.push_back(rec);
            }
            if (gte_.unimplemented) {
                gte_.unimplemented = false;
                SetTrap(TrapKind::GteUnimplemented, at, instruction, 0, gte_.unimplementedDetail);
            }
            return;
        }
        gte_.Execute(instruction);
        if (gte_.unimplemented) {
            gte_.unimplemented = false;
            SetTrap(TrapKind::GteUnimplemented, at, instruction, 0, gte_.unimplementedDetail);
        }
        return;
    }

    switch (rs) {
    case 0x00: // mfc2
        newLoadReg_ = static_cast<uint8_t>(rt);
        newLoadValue_ = gte_.ReadData(rd);
        return;
    case 0x02: // cfc2
        newLoadReg_ = static_cast<uint8_t>(rt);
        newLoadValue_ = gte_.ReadControl(rd);
        return;
    case 0x04: gte_.WriteData(rd, regs[rt]); return;    // mtc2
    case 0x06: gte_.WriteControl(rd, regs[rt]); return; // ctc2
    default:
        SetTrap(TrapKind::UnimplementedInstruction, at, instruction, 0, "unsupported COP2 access");
        return;
    }
}

} // namespace rr::interp
