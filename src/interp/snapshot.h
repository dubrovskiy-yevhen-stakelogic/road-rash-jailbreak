#pragma once
// Loads a `work\oracle\state\<name>\` directory - an extracted machine state - into a
// Memory + Cpu pair.
//
// The directory holds ram.bin (2 MiB), scratchpad.bin (1 KiB), bios.bin (512 KiB), vram.bin,
// spuram.bin, cpu.json and gpu.json. Only the first three plus cpu.json are guest-CPU state; VRAM
// and SPU RAM belong to device models this interpreter deliberately does not have.
#include <cstdint>
#include <string>
#include <vector>

#include "interp/r3000.h"

namespace rr::interp {

struct SnapshotInfo {
    std::string directory;
    uint32_t frameNumber = 0;
    uint32_t pc = 0;
    uint32_t npc = 0;
    bool hasScratchpad = false;
    bool hasBios = false;
    // `spuctl.bin`, when present: the 16 halfwords a read of 0x1F801D80 + 2*i returned at the
    // instant of the capture, cut out of the savestate's SPU block by tools\scout\spu_regs.py.
    // Only the loader reads it; what a caller does with it - the
    // bench seeds `Cpu::spuControl` - is the caller's decision.
    bool hasSpuControl = false;
    uint16_t spuControl[16] = {};
};

// Reads a whole file. Returns false and fills `error` when it cannot.
bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& out, std::string& error);

// Loads RAM, the scratchpad, the BIOS image and the full CPU/GTE register file.
bool LoadSnapshot(const std::string& directory, Memory& memory, Cpu& cpu, SnapshotInfo& info,
                  std::string& error);

// Copies a file into guest memory at `address`, which is what `load_overlay` (0x800118A0) does for
// the three RASHCD*.BIN overlays: a straight read of the whole file to a fixed address, no
// relocation, no header. Fails if the image would run past the end of RAM.
bool PlaceImage(Memory& memory, const std::vector<uint8_t>& image, uint32_t address, std::string& error);

} // namespace rr::interp
