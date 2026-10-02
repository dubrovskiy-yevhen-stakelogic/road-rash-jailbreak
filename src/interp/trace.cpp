#include "interp/trace.h"

#include <cstring>

namespace rr::interp {
namespace {

// KUSEG / KSEG0 / KSEG1 all reach the same 2 MiB, so a watch must fire whichever window the guest
// used. Normalise everything that lands in RAM to the KSEG0 view.
inline uint32_t Normalise(uint32_t address) {
    const uint32_t phys = address & 0x1FFFFFFFu;
    if (phys < 0x00800000u) return 0x80000000u | (phys & 0x001FFFFFu);
    return address;
}

} // namespace

Tracer::Tracer()
    : watchBitmap_(512u * 1024u / 64u, 0), probeBitmap_(512u * 1024u / 64u, 0) {}

namespace {
void SetBit(std::vector<uint64_t>& bitmap, uint32_t address) {
    const uint32_t phys = address & 0x1FFFFFFFu;
    if (phys >= 0x00800000u) return;
    const uint32_t word = (phys & 0x001FFFFFu) >> 2;
    bitmap[word >> 6] |= (uint64_t{1} << (word & 63));
}
} // namespace

void Tracer::AddWatch(uint32_t address, uint32_t length, std::string name, bool onRead, bool onWrite) {
    Watch w;
    w.low = Normalise(address);
    w.high = w.low + (length == 0 ? 0 : length - 1);
    w.name = std::move(name);
    w.onRead = onRead;
    w.onWrite = onWrite;
    // Mark every word the range can touch, plus one on each side so an unaligned or wider access
    // that overlaps the range still lands on a marked word.
    for (uint32_t a = (w.low & ~3u) - 4u; a <= (w.high | 3u) + 4u; a += 4u) SetBit(watchBitmap_, a);
    watches.push_back(std::move(w));
}

void Tracer::AddProbe(uint32_t pc, std::string name) {
    SetBit(probeBitmap_, pc);
    probes.emplace_back(pc, std::move(name));
}

void Tracer::OnProbe(uint64_t seq, uint32_t pc, const uint32_t* regs, uint32_t hi, uint32_t lo) {
    bool known = false;
    for (const auto& p : probes) {
        if (p.first == pc) { known = true; break; }
    }
    if (!known) return; // a bitmap collision, not a real probe
    if (probeHits.size() >= maxProbeHits) { probesTruncated = true; return; }
    ProbeHit h;
    h.seq = seq;
    h.pc = pc;
    std::memcpy(h.regs, regs, sizeof(h.regs));
    h.hi = hi;
    h.lo = lo;
    probeHits.push_back(h);
}

void Tracer::OnMemory(uint64_t seq, uint32_t pc, uint32_t address, uint32_t size, uint32_t value,
                      bool isWrite) {
    const uint32_t lo = Normalise(address);
    const uint32_t hi = lo + (size == 0 ? 0 : size - 1);
    for (size_t i = 0; i < watches.size(); ++i) {
        Watch& w = watches[i];
        if (hi < w.low || lo > w.high) continue;
        if (isWrite ? !w.onWrite : !w.onRead) continue;
        if (isWrite) ++w.writeHits; else ++w.readHits;
        if (mem.size() >= maxMem) { memTruncated = true; continue; }
        MemEvent e;
        e.seq = seq;
        e.pc = pc;
        e.address = address;
        e.value = value;
        e.size = static_cast<uint8_t>(size);
        e.isWrite = isWrite ? 1u : 0u;
        e.watchIndex = static_cast<uint16_t>(i);
        mem.push_back(e);
    }
}

void Tracer::Clear() {
    cop2.clear();
    gpu.clear();
    mem.clear();
    calls.clear();
    cop2Count = 0;
    gpuWordCount = 0;
    probeHits.clear();
    cop2Truncated = gpuTruncated = memTruncated = callsTruncated = probesTruncated = false;
    for (Watch& w : watches) { w.readHits = 0; w.writeHits = 0; }
}

} // namespace rr::interp
