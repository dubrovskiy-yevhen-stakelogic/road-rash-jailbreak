#include "interp/devices.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "interp/r3000.h"

namespace rr::interp {
namespace {

std::string Hex(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", v);
    return buf;
}

inline uint32_t Physical(uint32_t address) { return address & 0x1FFFFFFFu; }

} // namespace

const char* const Devices::kKnownSimplifications[] = {
    "the GPU is never busy: GPUSTAT always reports ready-for-command, ready-for-DMA and "
    "ready-to-send, so every DrawSync/poll loop exits on its first iteration",
    "no rasterisation: draw primitives are recorded, not drawn. VRAM only changes for explicit "
    "CPU->VRAM uploads (GP0 0xA0), VRAM->VRAM copies (0x80) and fills (0x02)",
    "VBlank is paced by retired instructions, not by a dot clock, because this interpreter has no "
    "cycle counter",
    "the three root counters are driven by an estimate of two guest cycles per retired instruction, "
    "then divided by their real clock-source divisor; there is no cycle counter to do better",
    "SIO0 answers as a standard digital pad in slot 1 and no memory card. The byte time is "
    "modelled from the guest's own JOY_MODE/JOY_BAUD, but /ACK (JOY_STAT bit 7) is a pulse with no "
    "observable width, so that bit always reads 0; its latched effects (JOY_STAT bit 9, I_STAT "
    "bit 7) are modelled in full",
    "the SPU is a register file plus the snapshot's own SPU RAM; there is no voice engine, so "
    "ENDX and the capture buffers never change on their own",
    "the MDEC runs a command the moment its last parameter word arrives (rr::mdec, the product's own "
    "arithmetic); DMA1 completes as soon as the words it asked for exist, and waits busy until then",
    nullptr,
};

Devices::Devices(Memory& memory) : memory_(memory) {
    vram.assign(1024u * 512u, 0);
    spuram.assign(512u * 1024u, 0);
}

bool Devices::LoadVram(const std::vector<uint8_t>& bytes, std::string& error) {
    if (bytes.size() != 1024u * 512u * 2u) {
        error = "vram.bin is " + std::to_string(bytes.size()) + " bytes, expected 1 MiB";
        return false;
    }
    std::memcpy(vram.data(), bytes.data(), bytes.size());
    return true;
}

bool Devices::LoadSpuRam(const std::vector<uint8_t>& bytes, std::string& error) {
    if (bytes.size() != 512u * 1024u) {
        error = "spuram.bin is " + std::to_string(bytes.size()) + " bytes, expected 512 KiB";
        return false;
    }
    spuram = bytes;
    return true;
}

bool Devices::NoteUnmodelled(uint32_t address, uint32_t size, uint32_t value, bool isWrite,
                             const char* what, std::string& why) {
    if (!exploreUnmodelled) {
        why = std::string(what) + " " + Hex(address);
        return false;
    }
    for (Unmodelled& u : unmodelled) {
        if (u.address == address && u.isWrite == isWrite) {
            ++u.count;
            u.value = value;
            return true;
        }
    }
    unmodelled.push_back({address, currentPc, size, value, 1, isWrite});
    return true;
}

// ------------------------------------------------------------------------------------- VRAM

void Devices::VramPut(uint32_t x, uint32_t y, uint16_t v) {
    vram[(y & 511u) * 1024u + (x & 1023u)] = v;
}

uint16_t Devices::VramGet(uint32_t x, uint32_t y) const {
    return vram[(y & 511u) * 1024u + (x & 1023u)];
}

// ------------------------------------------------------------------------------------- GPU

void Devices::RecordGpuWord(uint32_t value, uint8_t port, GpuWordSource source, uint32_t address) {
    if (port == 0) ++gp0WordsSeen; else ++gp1WordsSeen;
    if (tracer == nullptr || !tracer->traceGpu) return;
    ++tracer->gpuWordCount;
    if (tracer->gpu.size() >= tracer->maxGpu) { tracer->gpuTruncated = true; return; }
    GpuWord w;
    w.seq = seq;
    w.pc = (source == GpuWordSource::CpuStore) ? currentPc : 0u;
    w.value = value;
    w.address = address;
    w.port = port;
    w.source = source;
    tracer->gpu.push_back(w);
}

uint32_t Devices::Gp0Length(uint32_t command) {
    const uint32_t c = command >> 24;
    if (c == 0x02) return 3;
    if (c >= 0x20 && c <= 0x3F) {
        const uint32_t verts = (c & 0x08u) ? 4u : 3u;
        const uint32_t textured = (c & 0x04u) ? 1u : 0u;
        const uint32_t gouraud = (c & 0x10u) ? 1u : 0u;
        return 1u + verts * (1u + textured) + (gouraud ? verts - 1u : 0u);
    }
    if (c >= 0x40 && c <= 0x5F) {
        const uint32_t gouraud = (c & 0x10u) ? 1u : 0u;
        if (c & 0x08u) return 0; // polyline: variable, terminated by 0x55555555
        return gouraud ? 4u : 3u;
    }
    if (c >= 0x60 && c <= 0x7F) {
        const uint32_t size = (c >> 3) & 3u;
        const uint32_t textured = (c & 0x04u) ? 1u : 0u;
        return 2u + textured + (size == 0 ? 1u : 0u);
    }
    if (c >= 0x80 && c <= 0x9F) return 4;
    if (c >= 0xA0 && c <= 0xDF) return 3;
    return 1;
}

void Devices::WriteGp0(uint32_t value, GpuWordSource source, uint32_t address) {
    RecordGpuWord(value, 0, source, address);

    if (blitRemaining_ > 0) {
        // CPU -> VRAM pixel data, two 16-bit texels per word.
        for (int half = 0; half < 2; ++half) {
            if (blitRow_ >= blitH_) break;
            const uint16_t px = static_cast<uint16_t>((value >> (16 * half)) & 0xFFFFu);
            VramPut(blitX_ + blitCol_, blitY_ + blitRow_, px);
            ++vramWordsUploaded;
            if (++blitCol_ >= blitW_) { blitCol_ = 0; ++blitRow_; }
        }
        --blitRemaining_;
        return;
    }

    if (gp0Polyline_) {
        if ((value & 0xF000F000u) == 0x50005000u) { gp0Polyline_ = false; gp0Have_ = 0; gp0Need_ = 0; }
        return;
    }

    if (gp0Need_ == 0) {
        gp0Have_ = 0;
        const uint32_t len = Gp0Length(value);
        if (len == 0) {
            // A polyline: the header plus at least two vertices, then words until the terminator.
            gp0Polyline_ = true;
            return;
        }
        gp0Need_ = len;
    }
    if (gp0Have_ < 16) gp0Fifo_[gp0Have_] = value;
    ++gp0Have_;
    if (gp0Have_ >= gp0Need_) {
        Gp0Command();
        gp0Have_ = 0;
        gp0Need_ = 0;
    }
}

void Devices::Gp0Command() {
    const uint32_t cmd = gp0Fifo_[0] >> 24;
    switch (cmd) {
    case 0x02: { // fill a rectangle in VRAM
        const uint16_t colour = static_cast<uint16_t>(((gp0Fifo_[0] >> 3) & 0x1Fu) |
                                                      (((gp0Fifo_[0] >> 11) & 0x1Fu) << 5) |
                                                      (((gp0Fifo_[0] >> 19) & 0x1Fu) << 10));
        const uint32_t x = gp0Fifo_[1] & 0x3F0u;
        const uint32_t y = (gp0Fifo_[1] >> 16) & 0x1FFu;
        const uint32_t w = ((gp0Fifo_[2] & 0x3FFu) + 0x0Fu) & ~0x0Fu;
        const uint32_t h = (gp0Fifo_[2] >> 16) & 0x1FFu;
        for (uint32_t j = 0; j < h; ++j)
            for (uint32_t i = 0; i < w; ++i) VramPut(x + i, y + j, colour);
        return;
    }
    case 0x80: { // VRAM -> VRAM
        const uint32_t sx = gp0Fifo_[1] & 0x3FFu, sy = (gp0Fifo_[1] >> 16) & 0x1FFu;
        const uint32_t dx = gp0Fifo_[2] & 0x3FFu, dy = (gp0Fifo_[2] >> 16) & 0x1FFu;
        uint32_t w = gp0Fifo_[3] & 0x3FFu, h = (gp0Fifo_[3] >> 16) & 0x1FFu;
        if (w == 0) w = 0x400u;
        if (h == 0) h = 0x200u;
        std::vector<uint16_t> tmp(static_cast<size_t>(w) * h);
        for (uint32_t j = 0; j < h; ++j)
            for (uint32_t i = 0; i < w; ++i) tmp[static_cast<size_t>(j) * w + i] = VramGet(sx + i, sy + j);
        for (uint32_t j = 0; j < h; ++j)
            for (uint32_t i = 0; i < w; ++i) VramPut(dx + i, dy + j, tmp[static_cast<size_t>(j) * w + i]);
        return;
    }
    case 0xA0: { // CPU -> VRAM
        blitX_ = gp0Fifo_[1] & 0x3FFu;
        blitY_ = (gp0Fifo_[1] >> 16) & 0x1FFu;
        blitW_ = gp0Fifo_[2] & 0xFFFFu;
        blitH_ = (gp0Fifo_[2] >> 16) & 0xFFFFu;
        if (blitW_ == 0) blitW_ = 0x400u;
        if (blitH_ == 0) blitH_ = 0x200u;
        blitW_ &= 0x3FFu;
        blitH_ &= 0x1FFu;
        if (blitW_ == 0) blitW_ = 0x400u;
        if (blitH_ == 0) blitH_ = 0x200u;
        blitCol_ = blitRow_ = 0;
        blitRemaining_ = (blitW_ * blitH_ + 1u) / 2u;
        return;
    }
    case 0xC0: { // VRAM -> CPU
        readX_ = gp0Fifo_[1] & 0x3FFu;
        readY_ = (gp0Fifo_[1] >> 16) & 0x1FFu;
        readW_ = gp0Fifo_[2] & 0x3FFu;
        readH_ = (gp0Fifo_[2] >> 16) & 0x1FFu;
        if (readW_ == 0) readW_ = 0x400u;
        if (readH_ == 0) readH_ = 0x200u;
        readCol_ = readRow_ = 0;
        readRemaining_ = (readW_ * readH_ + 1u) / 2u;
        return;
    }
    case 0xE1: // draw mode
        gpustat = (gpustat & ~0x87FFu) | (gp0Fifo_[0] & 0x7FFu) | ((gp0Fifo_[0] & 0x800u) << 4);
        return;
    case 0xE5: // drawing offset
        drawOffsetX_ = gp0Fifo_[0] & 0x7FFu;
        drawOffsetY_ = (gp0Fifo_[0] >> 11) & 0x7FFu;
        return;
    default:
        return; // E2/E3/E4/E6, NOPs, and every draw primitive: recorded, not rasterised
    }
}

void Devices::WriteGp1(uint32_t value) {
    RecordGpuWord(value, 1, GpuWordSource::CpuStore, 0);
    switch ((value >> 24) & 0xFFu) {
    case 0x00: // reset
        gpustat = 0x14802000u;
        gp0Have_ = gp0Need_ = 0;
        gp0Polyline_ = false;
        blitRemaining_ = readRemaining_ = 0;
        return;
    case 0x01: // reset the command buffer
        gp0Have_ = gp0Need_ = 0;
        gp0Polyline_ = false;
        blitRemaining_ = 0;
        return;
    case 0x02: gpustat &= ~(1u << 24); return;                                   // acknowledge IRQ1
    case 0x03: gpustat = (gpustat & ~(1u << 23)) | ((value & 1u) << 23); return;  // display enable
    case 0x04: gpustat = (gpustat & ~(3u << 29)) | ((value & 3u) << 29); return;  // DMA direction
    case 0x05: ++displayStartCount; return;                                       // display area start
    case 0x06: case 0x07: return;                                                 // display window
    case 0x08: // display mode
        gpustat = (gpustat & ~0x007F4000u) | ((value & 0x3Fu) << 17) | ((value & 0x40u) << 10) |
                  ((value & 0x80u) << 7);
        return;
    case 0x09: return; // texture disable
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
        // GP1(10h) get GPU info. Only the two that a game can meaningfully act on are answered.
        switch (value & 0x0Fu) {
        case 0x03: gpuread = drawOffsetX_ | (drawOffsetY_ << 10); return;
        case 0x07: gpuread = 2; return; // GPU version
        case 0x08: gpuread = 0; return;
        default: return;
        }
    default:
        return;
    }
}

// ------------------------------------------------------------------------------------- DMA

bool Devices::DmaRead(uint32_t address, uint32_t& out, std::string& why) {
    const uint32_t phys = Physical(address);
    if (phys >= 0x1F801080u && phys < 0x1F8010F0u) {
        const uint32_t ch = (phys - 0x1F801080u) / 0x10u;
        switch ((phys >> 2) & 3u) {
        case 0: out = dma[ch].madr; return true;
        case 1: out = dma[ch].bcr; return true;
        case 2: out = dma[ch].chcr; return true;
        default: break;
        }
        why = "DMA register " + Hex(address);
        return false;
    }
    if (phys == 0x1F8010F0u) { out = dpcr; return true; }
    if (phys == 0x1F8010F4u) { out = dicr; return true; }
    why = "DMA register " + Hex(address);
    return false;
}

bool Devices::DmaWrite(uint32_t address, uint32_t value, std::string& why) {
    const uint32_t phys = Physical(address);
    if (phys >= 0x1F801080u && phys < 0x1F8010F0u) {
        const uint32_t ch = (phys - 0x1F801080u) / 0x10u;
        switch ((phys >> 2) & 3u) {
        case 0: dma[ch].madr = value & 0x00FFFFFFu; return true;
        case 1: dma[ch].bcr = value; return true;
        case 2: {
            dma[ch].chcr = value;
            const bool enabled = (dpcr >> (ch * 4u + 3u)) & 1u;
            const bool start = (value & 0x01000000u) != 0;
            const uint32_t sync = (value >> 9) & 3u;
            const bool trigger = (sync != 0) || ((value & 0x10000000u) != 0);
            if (enabled && start && trigger) return RunDma(static_cast<int>(ch), why);
            return true;
        }
        default: break;
        }
        why = "DMA register " + Hex(address);
        return false;
    }
    if (phys == 0x1F8010F0u) { dpcr = value; return true; }
    if (phys == 0x1F8010F4u) {
        // Bits 24..30 are acknowledge-by-writing-1; bits 0..14 and 15..23 are plain.
        const uint32_t ack = (value >> 24) & 0x7Fu;
        dicr = (dicr & ~0x00FF803Fu & ~(ack << 24)) | (value & 0x00FF803Fu);
        dicr &= ~0x80000000u;
        if ((dicr & 0x00800000u) && (((dicr >> 16) & 0x7Fu) & ((dicr >> 24) & 0x7Fu)) != 0)
            dicr |= 0x80000000u;
        return true;
    }
    why = "DMA register " + Hex(address);
    return false;
}

void Devices::FinishDma(int channel) {
    dma[channel].chcr &= ~0x11000000u;
    const uint32_t enableBit = 1u << (16u + static_cast<uint32_t>(channel));
    if (dicr & enableBit) {
        dicr |= 1u << (24u + static_cast<uint32_t>(channel));
        const bool master = (dicr & 0x00800000u) != 0;
        const bool any = (((dicr >> 16) & 0x7Fu) & ((dicr >> 24) & 0x7Fu)) != 0;
        if (master && any && (dicr & 0x80000000u) == 0) {
            dicr |= 0x80000000u;
            RaiseIrq(kIrqDma);
        }
    }
}

bool Devices::RunDma(int channel, std::string& why) {
    const uint32_t chcr = dma[channel].chcr;
    const uint32_t sync = (chcr >> 9) & 3u;
    const bool fromRam = (chcr & 1u) != 0;
    const bool backwards = (chcr & 2u) != 0;
    uint32_t addr = dma[channel].madr & 0x001FFFFCu;

    if (channel == 6) { // OTC: build a reverse ordering table
        uint32_t count = dma[channel].bcr & 0xFFFFu;
        if (count == 0) count = 0x10000u;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t value = (i == count - 1u) ? 0x00FFFFFFu : ((addr - 4u) & 0x001FFFFFu);
            memory_.PokeWord(0x80000000u | addr, value);
            addr -= 4u;
        }
        dma[channel].madr = addr & 0x00FFFFFFu;
        FinishDma(channel);
        return true;
    }

    if (channel == 2) { // GPU
        if (sync == 2) { // linked list
            if (!fromRam) { why = "DMA2 linked list towards RAM, which the hardware cannot do"; return false; }
            uint32_t guard = 0;
            for (;;) {
                const uint32_t header = memory_.PeekWord(0x80000000u | addr);
                const uint32_t count = header >> 24;
                for (uint32_t i = 1; i <= count; ++i) {
                    const uint32_t at = (addr + 4u * i) & 0x001FFFFCu;
                    WriteGp0(memory_.PeekWord(0x80000000u | at), GpuWordSource::DmaLinkedList,
                             0x80000000u | at);
                    ++dmaLinkedListWords;
                }
                ++dmaLinkedListPackets;
                const uint32_t next = header & 0x00FFFFFFu;
                if (next & 0x00800000u) break;
                addr = next & 0x001FFFFCu;
                if (++guard > 2000000u) { why = "DMA2 linked list did not terminate"; return false; }
            }
            dma[channel].madr = 0x00FFFFFFu;
            FinishDma(channel);
            return true;
        }
        uint32_t words = 0;
        if (sync == 0) {
            words = dma[channel].bcr & 0xFFFFu;
            if (words == 0) words = 0x10000u;
        } else {
            uint32_t size = dma[channel].bcr & 0xFFFFu;
            uint32_t blocks = (dma[channel].bcr >> 16) & 0xFFFFu;
            if (size == 0) size = 0x10000u;
            if (blocks == 0) blocks = 0x10000u;
            words = size * blocks;
        }
        for (uint32_t i = 0; i < words; ++i) {
            if (fromRam) {
                WriteGp0(memory_.PeekWord(0x80000000u | addr), GpuWordSource::DmaBlock,
                         0x80000000u | addr);
            } else {
                uint32_t v = 0;
                std::string ignored;
                if (readRemaining_ > 0) {
                    uint16_t lo = VramGet(readX_ + readCol_, readY_ + readRow_);
                    if (++readCol_ >= readW_) { readCol_ = 0; ++readRow_; }
                    uint16_t hi = VramGet(readX_ + readCol_, readY_ + readRow_);
                    if (++readCol_ >= readW_) { readCol_ = 0; ++readRow_; }
                    --readRemaining_;
                    v = static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 16);
                } else {
                    v = gpuread;
                }
                memory_.PokeWord(0x80000000u | addr, v);
            }
            addr = backwards ? (addr - 4u) : (addr + 4u);
            addr &= 0x001FFFFCu;
        }
        dma[channel].madr = addr & 0x00FFFFFFu;
        FinishDma(channel);
        return true;
    }

    if (channel == 4) { // SPU
        uint32_t size = dma[channel].bcr & 0xFFFFu;
        uint32_t blocks = (dma[channel].bcr >> 16) & 0xFFFFu;
        if (size == 0) size = 0x10000u;
        if (sync == 0) blocks = 1;
        else if (blocks == 0) blocks = 0x10000u;
        const uint32_t words = size * blocks;
        for (uint32_t i = 0; i < words; ++i) {
            const uint32_t spuAddr = (spuTransferAddress_ + 4u * i) & 0x0007FFFCu;
            if (fromRam) {
                const uint32_t v = memory_.PeekWord(0x80000000u | addr);
                std::memcpy(spuram.data() + spuAddr, &v, 4);
            } else {
                uint32_t v = 0;
                std::memcpy(&v, spuram.data() + spuAddr, 4);
                memory_.PokeWord(0x80000000u | addr, v);
            }
            addr = backwards ? (addr - 4u) : (addr + 4u);
            addr &= 0x001FFFFCu;
        }
        spuTransferAddress_ = (spuTransferAddress_ + 4u * words) & 0x0007FFFCu;
        dma[channel].madr = addr & 0x00FFFFFFu;
        FinishDma(channel);
        return true;
    }

    if (channel == 0) { // MDEC in: the command's parameter words
        if (!fromRam) { why = "DMA0 towards RAM, which the MDEC's input cannot do"; return false; }
        uint32_t size = dma[channel].bcr & 0xFFFFu;
        uint32_t blocks = (dma[channel].bcr >> 16) & 0xFFFFu;
        if (size == 0) size = 0x10000u;
        if (sync == 0) blocks = 1;
        else if (blocks == 0) blocks = 0x10000u;
        for (uint32_t i = 0; i < size * blocks; ++i) {
            MdecWord(memory_.PeekWord(0x80000000u | addr));
            addr = backwards ? (addr - 4u) : (addr + 4u);
            addr &= 0x001FFFFCu;
        }
        dma[channel].madr = addr & 0x00FFFFFFu;
        FinishDma(channel);
        return true;
    }
    if (channel == 1) { // MDEC out
        if (fromRam) { why = "DMA1 from RAM, which the MDEC's output cannot do"; return false; }
        return MdecDma1(why);
    }

    why = "DMA channel " + std::to_string(channel) + " (chcr " + Hex(chcr) +
          ") is not modelled; only 0 / 1 (MDEC), 2 (GPU), 4 (SPU) and 6 (OTC) are";
    return false;
}

// ------------------------------------------------------------------------------------- MDEC
void Devices::MdecWord(uint32_t w) {
    if (mdecNeed_ == 0) { // a command word
        mdecCmd_ = w;
        mdecParams_.clear();
        const uint32_t op = w >> 29;
        mdecNeed_ = op == 1u ? (w & 0xFFFFu) : op == 2u ? ((w & 1u) ? 32u : 16u) : op == 3u ? 32u : 0u;
        if (mdecNeed_ == 0) MdecRun();
        return;
    }
    mdecParams_.push_back(w);
    if (--mdecNeed_ == 0) MdecRun();
}

void Devices::MdecRun() {
    ++mdecCommands;
    const uint32_t op = mdecCmd_ >> 29;
    if (op == 1u) {
        std::vector<uint16_t> hw(mdecParams_.size() * 2u);
        for (size_t i = 0; i < mdecParams_.size(); ++i) {
            hw[2 * i] = static_cast<uint16_t>(mdecParams_[i]);
            hw[2 * i + 1] = static_cast<uint16_t>(mdecParams_[i] >> 16);
        }
        if (mdecOutPos_ == mdecOut_.size()) {
            mdecOut_.clear();
            mdecOutPos_ = 0;
        }
        const bool colour = ((mdecCmd_ >> 27) & 3u) >= 2u;
        const size_t cap = colour ? (hw.size() / 6u + 1u) * 192u : (hw.size() + 1u) * 16u;
        const size_t base = mdecOut_.size();
        mdecOut_.resize(base + cap);
        rr::mdec::Input in{hw.data(), hw.size(), 0};
        size_t produced = 0;
        mdecMacroblocks += rr::mdec::DecodeCommand(mdecCmd_, in, mdecTables, mdecModel, mdecOut_.data() + base, cap,
                                                   produced);
        mdecOut_.resize(base + std::min(produced, cap));
    } else if (op == 2u) {
        for (size_t i = 0; i < 64u; ++i) {
            const uint8_t b = static_cast<uint8_t>(mdecParams_[i / 4u] >> (8u * (i % 4u)));
            mdecTables.luma[i] = b;
            if (mdecParams_.size() >= 32u) mdecTables.chroma[i] = static_cast<uint8_t>(mdecParams_[16u + i / 4u] >> (8u * (i % 4u)));
        }
    } else if (op == 3u) {
        for (size_t i = 0; i < 64u; ++i) mdecTables.idct[i] = static_cast<int16_t>(mdecParams_[i / 2u] >> (16u * (i % 2u)));
    }
    mdecNeed_ = 0;
    if (mdecDma1Waiting_) {
        std::string why;
        MdecDma1(why);
    }
}

bool Devices::MdecDma1(std::string& why) {
    (void)why;
    const uint32_t chcr = dma[1].chcr;
    uint32_t size = dma[1].bcr & 0xFFFFu;
    uint32_t blocks = (dma[1].bcr >> 16) & 0xFFFFu;
    if (size == 0) size = 0x10000u;
    if (((chcr >> 9) & 3u) == 0) blocks = 1;
    else if (blocks == 0) blocks = 0x10000u;
    const size_t words = static_cast<size_t>(size) * blocks;
    if (MdecOutAvailable() < words) { // the words are not there yet: the channel stays busy
        mdecDma1Waiting_ = true;
        return true;
    }
    mdecDma1Waiting_ = false;
    uint32_t addr = dma[1].madr & 0x001FFFFCu;
    for (size_t i = 0; i < words; ++i) {
        memory_.PokeWord(0x80000000u | addr, mdecOut_[mdecOutPos_++]);
        addr = (chcr & 2u) ? (addr - 4u) : (addr + 4u);
        addr &= 0x001FFFFCu;
    }
    mdecWordsOut += words;
    dma[1].madr = addr & 0x00FFFFFFu;
    FinishDma(1);
    return true;
}

uint32_t Devices::MdecStatus() const {
    uint32_t s = 0;
    if (MdecOutAvailable() == 0) s |= 1u << 31;                                    // data-out FIFO empty
    if (mdecNeed_ != 0 || MdecOutAvailable() != 0) s |= 1u << 29;                   // command busy
    if ((mdecControl_ & 0x40000000u) != 0 && mdecNeed_ != 0) s |= 1u << 28;         // data-in request
    if ((mdecControl_ & 0x20000000u) != 0 && MdecOutAvailable() != 0) s |= 1u << 27; // data-out request
    s |= ((mdecCmd_ >> 25) & 0xFu) << 23; // depth, signed, bit 15 of the current command
    s |= 4u << 16;                        // the current block: none (a command runs at once)
    s |= mdecNeed_ == 0 ? 0xFFFFu : ((mdecNeed_ - 1u) & 0xFFFFu);
    return s;
}

// ------------------------------------------------------------------------------------- SIO0
//
// The controller port, modelled from the GAME'S OWN driver rather than from a generic description
// of the hardware. Everything below is what `SLUS_010.53`
// (SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1) actually does; addresses are that EXE's.
//
//   0x80040B10  the VBlank chain entry: bails out if JOY_CTRL bit 1 is still set, otherwise resets
//               the state counter and runs one poll for each port
//   0x80040DC4  starts a port: JOY_CTRL = 0x40 (reset), JOY_CTRL = 0, JOY_MODE = 0x000D,
//               JOY_BAUD = 0x0088, then JOY_CTRL = 0x1003 (TXEN | DTR | ACK-interrupt-enable)
//   0x800411E8  exchange one byte: wait for JOY_STAT bit 1 (RX FIFO not empty), read JOY_RX_DATA,
//               write JOY_BAUD, WAIT FOR I_STAT BIT 7, then write the next JOY_TX_DATA
//   0x80041670  after every byte: I_STAT = 0xFFFFFF7F, wait until JOY_STAT bit 7 (/ACK level) is
//               clear, then JOY_CTRL |= 0x10 to drop the latched interrupt
//
// Two consequences decide the model:
//
//  1. THE BYTE TIME HAS TO EXIST. 0x800410F8 calls 0x80041670 a handful of instructions after
//     0x800411E8 wrote JOY_TX_DATA, and 0x80041670's first act is to clear I_STAT bit 7. If the
//     exchange completed inside the store, the interrupt it raised would be cleared by that very
//     sequence and the next 0x800411E8 would spin on `I_STAT & 0x80` until its timeout. That is
//     exactly the deadlock that makes `--pad` inert. So a byte is
//     scheduled and lands `JoyByteInstructions()` retired instructions later, from Advance().
//     The figure comes from the guest's own JOY_MODE/JOY_BAUD: 8 data bits at
//     `JOY_BAUD * reload factor` guest cycles per bit, divided by `cyclesPerInstruction`. With the
//     driver's 0x000D / 0x0088 that is 8*136*1/2 = 544 instructions, i.e. ~32 us of guest time,
//     which is the real transfer rate of a PlayStation controller port.
//
//  2. SIMPLIFICATION, stated as one: /ACK (JOY_STAT bit 7) is modelled as a pulse with no
//     observable width, so that bit always reads 0. The LATCHED effects of the pulse - JOY_STAT
//     bit 9 and I_STAT bit 7 - are modelled fully, and they are the only ones this driver's
//     success path uses. The level is read at exactly one place, 0x80041670, and it is read while
//     the NEXT byte is still on the wire, i.e. at a moment when the previous pulse is long over on
//     hardware too.
//
// `joySeq_` counts bytes since the port was selected. Byte 0 is the address: 0x01 selects the
// controller, 0x81 a memory card. Only a digital pad in slot 1 answers; everything else never
// acknowledges, which is exactly how the guest's own code concludes that nothing is there.

uint64_t Devices::JoyByteInstructions() const {
    // JOY_MODE bits 0..1 select the baud reload factor (1, 1, 16, 64), bits 2..3 the character
    // length (5..8 bits). One bit costs `JOY_BAUD * factor` guest cycles.
    static const uint32_t kFactor[4] = {1, 1, 16, 64};
    const uint32_t factor = kFactor[joyMode_ & 3u];
    const uint32_t bits = 5u + ((joyMode_ >> 2) & 3u);
    const uint64_t cycles = static_cast<uint64_t>(joyBaud_ == 0 ? 1u : joyBaud_) * factor * bits;
    const uint64_t perInstruction = cyclesPerInstruction == 0 ? 1 : cyclesPerInstruction;
    const uint64_t n = cycles / perInstruction;
    return n == 0 ? joyFallbackByteInstructions : n;
}

void Devices::JoyReset() {
    joyBusy_ = false;
    joyPendingAck_ = false;
    joyRxFull_ = false;
    joyIrq_ = false;
    joySeq_ = -1;
}

void Devices::JoyStart(uint8_t tx, uint64_t retired) {
    ++padBytesSent;
    // A byte already on the wire is overwritten; hardware would corrupt the transfer, and so does
    // this - the point is not to silently serialise something the guest did not ask for.
    joyPendingRx_ = 0xFF;
    joyPendingAck_ = false;

    const bool selected = (joyCtrl_ & 0x0002u) != 0;      // /JOYn output asserted
    const bool slot2 = (joyCtrl_ & 0x2000u) != 0;
    if (!selected || slot2 || !padConnected) {
        // Nothing drives the data line and nothing acknowledges. The byte still "transfers": the
        // shift register clocks in the idle level, which reads back as 0xFF.
        joySeq_ = -1;
        joyBusy_ = true;
        joyDueAt_ = retired + JoyByteInstructions();
        return;
    }

    if (joySeq_ < 0) {
        joySeq_ = 0;
        if (tx == 0x01u) { joyPendingRx_ = 0xFF; joyPendingAck_ = true; }
        // else: not the controller's address, so no device on this port answers.
    } else {
        ++joySeq_;
        switch (joySeq_) {
        case 1: joyPendingRx_ = 0x41; joyPendingAck_ = true; break;   // digital pad id, 1 halfword
        case 2: joyPendingRx_ = 0x5A; joyPendingAck_ = true; break;
        case 3: joyPendingRx_ = static_cast<uint8_t>(padButtons & 0xFFu); joyPendingAck_ = true; break;
        case 4: joyPendingRx_ = static_cast<uint8_t>((padButtons >> 8) & 0xFFu);
                joyPendingAck_ = false;                                // last byte: no ACK
                ++padPollCount; joySeq_ = -1; break;
        default: joyPendingRx_ = 0xFF; joyPendingAck_ = false; joySeq_ = -1; break;
        }
    }
    joyBusy_ = true;
    joyDueAt_ = retired + JoyByteInstructions();
}

// JOY_CTRL. Bit 1 is the /JOYn (DTR) output that selects the port, bit 4 acknowledges (drops the
// latched interrupt and the error bits), bit 6 is a full reset, bit 12 enables the ACK interrupt
// and bit 13 picks slot 2. A 0 -> 1 edge on bit 1 starts a new device selection, so the byte
// counter restarts there and not only on a reset.
void Devices::JoyWriteCtrl(uint16_t value) {
    const bool wasSelected = (joyCtrl_ & 0x0002u) != 0;
    joyCtrl_ = value;
    if (value & 0x0040u) {          // reset
        joyCtrl_ = 0;
        JoyReset();
        return;
    }
    if (value & 0x0010u) {          // acknowledge: clears JOY_STAT bit 9, not the byte in flight
        joyIrq_ = false;
    }
    const bool selected = (value & 0x0002u) != 0;
    if (!selected) JoyReset();
    else if (!wasSelected) { joySeq_ = -1; }
}

void Devices::JoyComplete() {
    joyBusy_ = false;
    joyRx_ = joyPendingRx_;
    joyRxFull_ = true;
    if (joyPendingAck_ && (joyCtrl_ & 0x1000u)) {   // JOY_CTRL bit 12: ACK interrupt enable
        joyIrq_ = true;                              // JOY_STAT bit 9
        RaiseIrq(kIrqPad);                           // I_STAT bit 7
    }
    joyPendingAck_ = false;
}

// ------------------------------------------------------------------------------------- timers

void Devices::AdvanceTimers(uint64_t retired) {
    if (lastSeq_ == 0) { lastSeq_ = retired; return; }
    const uint64_t cycles = (retired - lastSeq_) * cyclesPerInstruction;
    lastSeq_ = retired;
    if (cycles == 0) return;
    for (int t = 0; t < 3; ++t) {
        Timer& tm = timer_[t];
        // Clock source select, mode bits 8..9 (psx-spx). Divisors are the documented hardware ones:
        // the dot clock of the 320-wide mode is 7/11 of the CPU clock, one scanline is 2160 CPU
        // cycles, and timer 2's alternate source is the CPU clock divided by 8.
        const uint32_t source = (tm.mode >> 8) & 3u;
        uint64_t divisor = 1;
        if (t == 0) divisor = (source == 1 || source == 3) ? 11u / 7u + 1u : 1u;
        else if (t == 1) divisor = (source == 1 || source == 3) ? 2160u : 1u;
        else divisor = (source == 2 || source == 3) ? 8u : 1u;
        tm.accumulated += cycles;
        const uint64_t delta = tm.accumulated / divisor;
        if (delta == 0) continue;
        tm.accumulated -= delta * divisor;
        const uint64_t next = static_cast<uint64_t>(tm.counter) + delta;
        const uint32_t target = tm.target & 0xFFFFu;
        const bool hitTarget = (target != 0) && (tm.counter < target) && (next >= target);
        const bool overflow = next > 0xFFFFu;
        if ((tm.mode & (1u << 3)) && hitTarget) {
            tm.counter = static_cast<uint32_t>(next - target) & 0xFFFFu; // reset on target
        } else {
            tm.counter = static_cast<uint32_t>(next & 0xFFFFu);
        }
        if (hitTarget) tm.mode |= (1u << 11);
        if (overflow) tm.mode |= (1u << 12);
        const bool wantIrq = ((tm.mode & (1u << 4)) && hitTarget) || ((tm.mode & (1u << 5)) && overflow);
        if (wantIrq) RaiseIrq(kIrqTimer0 + t);
    }
}

void Devices::SetTimer(int index, uint32_t mode, uint32_t counter, uint32_t target) {
    if (index < 0 || index > 2) return;
    timer_[index].mode = mode;
    timer_[index].counter = counter;
    timer_[index].target = target;
    timer_[index].accumulated = 0;
}

void Devices::Advance(uint64_t retired) {
    seq = retired;
    if (joyBusy_ && retired >= joyDueAt_) JoyComplete();
    AdvanceTimers(retired);
    if (instructionsPerFrame == 0) return;
    if (nextVblankAt == 0) { nextVblankAt = retired + instructionsPerFrame; return; }
    if (retired >= nextVblankAt) {
        nextVblankAt = retired + instructionsPerFrame;
        ++vblankCount;
        RaiseIrq(kIrqVBlank);
    }
}

// ------------------------------------------------------------------------------------- MMIO

bool Devices::Read(uint32_t address, uint32_t size, uint32_t& out, std::string& why) {
    const uint32_t phys = Physical(address);

    // --- memory control / RAM size
    if (phys >= 0x1F801000u && phys < 0x1F801024u) { out = 0; return true; }
    if (phys == 0x1F801060u) { out = 0x00000B88u; return true; }

    // --- SIO0 (controllers / memory cards)
    if (phys >= 0x1F801040u && phys < 0x1F801050u) {
        switch (phys) {
        case 0x1F801040u: case 0x1F801041u: case 0x1F801042u: case 0x1F801043u: // JOY_RX_DATA
            out = joyRx_;
            joyRxFull_ = false;
            return true;
        case 0x1F801044u: case 0x1F801046u: { // JOY_STAT
            // bit 0  TX FIFO ready to take another byte - always, there is a one-byte FIFO
            // bit 1  RX FIFO not empty
            // bit 2  TX idle (the shift register has finished), clear while a byte is on the wire
            // bit 7  /ACK input level - see the SIO0 comment: modelled as a zero-width pulse
            // bit 9  the latched ACK interrupt
            uint32_t s = 0x00000001u;
            if (!joyBusy_) s |= 0x0004u;
            if (joyRxFull_) s |= 0x0002u;
            if (joyIrq_) s |= 0x0200u;
            out = (phys == 0x1F801046u) ? (s >> 16) : s;
            return true;
        }
        case 0x1F801048u: out = joyMode_ | (static_cast<uint32_t>(joyCtrl_) << 16); return true;
        case 0x1F80104Au: out = joyCtrl_; return true;
        case 0x1F80104Cu: out = static_cast<uint32_t>(joyBaud_) << 16; return true;
        case 0x1F80104Eu: out = joyBaud_; return true;
        default: break;
        }
        return NoteUnmodelled(address, size, 0, false, "SIO0 register", why);
    }
    if (phys >= 0x1F801050u && phys < 0x1F801060u) { out = 0x00000005u; return true; } // SIO1

    // --- interrupt controller
    if (phys == 0x1F801070u) { out = istat; return true; }
    if (phys == 0x1F801074u) { out = imask; return true; }

    // --- DMA
    if (phys >= 0x1F801080u && phys < 0x1F801100u) return DmaRead(address, out, why);

    // --- root counters
    if (phys >= 0x1F801100u && phys < 0x1F801130u) {
        const uint32_t t = (phys - 0x1F801100u) / 0x10u;
        switch ((phys >> 2) & 3u) {
        case 0: out = timer_[t].counter & 0xFFFFu; return true;
        case 1: out = timer_[t].mode; timer_[t].mode &= ~0x1800u; return true;
        case 2: out = timer_[t].target & 0xFFFFu; return true;
        default: break;
        }
        return NoteUnmodelled(address, size, 0, false, "root counter register", why);
    }

    // --- GPU
    if (phys == 0x1F801810u) {
        if (readRemaining_ > 0) {
            const uint16_t lo = VramGet(readX_ + readCol_, readY_ + readRow_);
            if (++readCol_ >= readW_) { readCol_ = 0; ++readRow_; }
            const uint16_t hi = VramGet(readX_ + readCol_, readY_ + readRow_);
            if (++readCol_ >= readW_) { readCol_ = 0; ++readRow_; }
            --readRemaining_;
            gpuread = static_cast<uint32_t>(lo) | (static_cast<uint32_t>(hi) << 16);
        }
        out = gpuread;
        return true;
    }
    if (phys == 0x1F801814u) {
        // Ready for a command (26), ready to send VRAM (27), ready for DMA (28); DMA request (25)
        // follows the direction field. Bit 31 alternates so that a poll on the field flag cannot
        // spin for ever.
        uint32_t s = gpustat | (1u << 26) | (1u << 27) | (1u << 28);
        const uint32_t dir = (s >> 29) & 3u;
        uint32_t req = 0;
        if (dir == 1) req = 1;
        else if (dir == 2) req = (s >> 28) & 1u;
        else if (dir == 3) req = (s >> 27) & 1u;
        s = (s & ~(1u << 25)) | (req << 25);
        s = (s & ~0x80000000u) | ((seq & 0x400u) ? 0x80000000u : 0u);
        out = s;
        return true;
    }

    // --- SPU
    if (phys >= 0x1F801C00u && phys < 0x1F802000u) {
        const uint32_t o = phys - 0x1F801C00u;
        if (phys == 0x1F801DAEu) { // SPUSTAT mirrors the low bits of SPUCNT, never busy
            uint16_t cnt = 0;
            std::memcpy(&cnt, spureg_ + 0x1AA, 2);
            out = static_cast<uint32_t>(cnt & 0x003Fu);
            return true;
        }
        uint32_t v = 0;
        std::memcpy(&v, spureg_ + o, size < 4 ? size : 4);
        out = v;
        return true;
    }

    // --- CD-ROM and anything else: not modelled
    if (phys >= 0x1F801800u && phys < 0x1F801804u)
        return NoteUnmodelled(address, size, 0, false, "CD-ROM register", why);
    if (phys == 0x1F801820u) { // MDEC data out (the CPU reading the output FIFO)
        out = 0;
        if (MdecOutAvailable() != 0) {
            out = mdecOut_[mdecOutPos_++];
            ++mdecWordsOut;
        }
        return true;
    }
    if (phys == 0x1F801824u) { out = MdecStatus(); return true; }
    if (phys >= 0x1F801820u && phys < 0x1F801828u)
        return NoteUnmodelled(address, size, 0, false, "MDEC register (not at a word address)", why);
    return NoteUnmodelled(address, size, 0, false, "unmodelled hardware register", why);
}

bool Devices::Write(uint32_t address, uint32_t size, uint32_t value, std::string& why) {
    const uint32_t phys = Physical(address);

    if (phys >= 0x1F801000u && phys < 0x1F801024u) return true; // memory control
    if (phys == 0x1F801060u) return true;                        // RAM_SIZE

    if (phys >= 0x1F801040u && phys < 0x1F801050u) { // SIO0
        switch (phys) {
        case 0x1F801040u: case 0x1F801041u: case 0x1F801042u: case 0x1F801043u:
            JoyStart(static_cast<uint8_t>(value & 0xFFu), seq);
            return true;
        case 0x1F801048u:
            joyMode_ = static_cast<uint16_t>(value & 0xFFFFu);
            if (size == 4) JoyWriteCtrl(static_cast<uint16_t>(value >> 16));
            return true;
        case 0x1F80104Au:
            JoyWriteCtrl(static_cast<uint16_t>(value & 0xFFFFu));
            return true;
        case 0x1F80104Cu: joyBaud_ = static_cast<uint16_t>(value >> 16); return true;
        case 0x1F80104Eu: joyBaud_ = static_cast<uint16_t>(value & 0xFFFFu); return true;
        default: break;
        }
        return NoteUnmodelled(address, size, value, true, "SIO0 register", why);
    }
    if (phys >= 0x1F801050u && phys < 0x1F801060u) return true;  // SIO1: nothing is attached

    if (phys == 0x1F801070u) { // I_STAT: writing 0 to a bit acknowledges it
        istat &= value & 0x7FFu;
        return true;
    }
    if (phys == 0x1F801074u) { imask = value & 0x7FFu; return true; }

    if (phys >= 0x1F801080u && phys < 0x1F801100u) return DmaWrite(address, value, why);

    if (phys >= 0x1F801100u && phys < 0x1F801130u) {
        const uint32_t t = (phys - 0x1F801100u) / 0x10u;
        switch ((phys >> 2) & 3u) {
        case 0: timer_[t].counter = value & 0xFFFFu; return true;
        case 1: timer_[t].mode = (value & 0x03FFu) | 0x0400u; timer_[t].counter = 0; return true;
        case 2: timer_[t].target = value & 0xFFFFu; return true;
        default: break;
        }
        return NoteUnmodelled(address, size, value, true, "root counter register", why);
    }

    if (phys == 0x1F801810u) { WriteGp0(value, GpuWordSource::CpuStore, 0); return true; }
    if (phys == 0x1F801814u) { WriteGp1(value); return true; }

    if (phys >= 0x1F801C00u && phys < 0x1F802000u) {
        const uint32_t o = phys - 0x1F801C00u;
        std::memcpy(spureg_ + o, &value, size < 4 ? size : 4);
        if (phys == 0x1F801DA6u) spuTransferAddress_ = (value & 0xFFFFu) * 8u;
        return true;
    }

    if (phys >= 0x1F802000u && phys < 0x1F803000u) return true; // expansion 2 / POST

    if (phys >= 0x1F801800u && phys < 0x1F801804u)
        return NoteUnmodelled(address, size, value, true, "CD-ROM register", why);
    if (phys == 0x1F801820u) { MdecWord(value); return true; } // MDEC command / parameter
    if (phys == 0x1F801824u) {                                  // MDEC control
        if (value & 0x80000000u) { // reset: abort the command, drop the FIFOs
            mdecCmd_ = 0;
            mdecNeed_ = 0;
            mdecParams_.clear();
            mdecOut_.clear();
            mdecOutPos_ = 0;
            mdecDma1Waiting_ = false;
        }
        mdecControl_ = value & 0x60000000u;
        return true;
    }
    if (phys >= 0x1F801820u && phys < 0x1F801828u)
        return NoteUnmodelled(address, size, value, true, "MDEC register (not at a word address)", why);
    return NoteUnmodelled(address, size, value, true, "unmodelled hardware register", why);
}

} // namespace rr::interp
