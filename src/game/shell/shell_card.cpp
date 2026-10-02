// The career's memory-card file (shell_card.h, frontend.md 8).
#include "game/shell/shell_card.h"

#include "game/shell/shell_logic.h"

#include <cstdio>
#include <cstring>

namespace rr::shell {

namespace {

constexpr uint32_t kFrame = 128, kBlock = 8192;

void FrameXor(uint8_t* frame) {
    uint8_t x = 0;
    for (uint32_t i = 0; i < 127u; ++i) x ^= frame[i];
    frame[127] = x;
}
void Put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint32_t Get32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint8_t* Record(CardImage& card, int slot) {
    return card.raw.data() + static_cast<size_t>(card.block) * kBlock + kCardRecordsAt + kCardRecordBytes * static_cast<uint32_t>(slot);
}
const uint8_t* Record(const CardImage& card, int slot) {
    return card.raw.data() + static_cast<size_t>(card.block) * kBlock + kCardRecordsAt + kCardRecordBytes * static_cast<uint32_t>(slot);
}

// The two checksum words of a record (0x8005EA54 recomputes them before every write).
void Seal(uint8_t* rec, int slot) {
    uint32_t a = 0, b = 0;
    CardChecksumHost(rec, slot, &a, &b);
    Put32(rec + 0x1DC, a);
    Put32(rec + 0x1E0, b);
}

} // namespace

CardImage FormatCard() {
    CardImage c;
    uint8_t* raw = c.raw.data();
    for (uint32_t f : {0u, 63u}) { // the "MC" frames of a formatted card (an empty .mcr)
        raw[f * kFrame] = 'M';
        raw[f * kFrame + 1] = 'C';
        FrameXor(raw + f * kFrame);
    }
    for (uint32_t f = 1; f <= 15u; ++f) { // fifteen free directory frames
        uint8_t* d = raw + f * kFrame;
        Put32(d, 0xA0);
        d[8] = d[9] = 0xFF;
        FrameXor(d);
    }
    for (uint32_t f = 16; f <= 35u; ++f) { // the broken-sector list, all unused
        uint8_t* d = raw + f * kFrame;
        Put32(d, 0xFFFFFFFFu);
        d[8] = d[9] = 0xFF;
        FrameXor(d);
    }
    return c;
}

bool LoadCardFile(const std::string& path, CardImage& card, std::string& error) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    card.raw.assign(128u * 1024u, 0);
    const size_t got = std::fread(card.raw.data(), 1, card.raw.size(), f);
    std::fclose(f);
    if (got != card.raw.size() || card.raw[0] != 'M' || card.raw[1] != 'C') {
        error = path + " is not a 128 KiB PlayStation memory card image";
        return false;
    }
    card.block = FindGameBlock(card);
    return true;
}

bool SaveCardFile(const std::string& path, const CardImage& card, std::string& error) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        error = "cannot write " + path;
        return false;
    }
    const size_t put = std::fwrite(card.raw.data(), 1, card.raw.size(), f);
    std::fclose(f);
    if (put != card.raw.size()) {
        error = "short write to " + path;
        return false;
    }
    return true;
}

int FindGameBlock(const CardImage& card) {
    for (int f = 1; f <= 15; ++f) {
        const uint8_t* d = card.raw.data() + static_cast<size_t>(f) * kFrame;
        if (Get32(d) == 0x51 && std::memcmp(d + 10, kCardFileName, sizeof(kCardFileName) - 1) == 0) return f;
    }
    return -1;
}

bool EnsureGameFile(CardImage& card, GuestRam& g, std::string& error) {
    card.block = FindGameBlock(card);
    if (card.block > 0) return true;
    int free = -1;
    for (int f = 1; f <= 15 && free < 0; ++f)
        if ((Get32(card.raw.data() + static_cast<size_t>(f) * kFrame) & 0xF0u) == 0xA0u) free = f;
    if (free < 0) {
        error = "the card has no free block";
        return false;
    }
    // The directory frame 0x8005E858 builds: state 0x51, size 8192, no next block, the name, XOR.
    uint8_t* d = card.raw.data() + static_cast<size_t>(free) * kFrame;
    std::memset(d, 0, kFrame);
    Put32(d, 0x51);
    Put32(d + 4, kBlock);
    d[8] = d[9] = 0xFF;
    std::memcpy(d + 10, kCardFileName, sizeof(kCardFileName) - 1);
    FrameXor(d);
    card.block = free;
    // The block: the SC title frame template RASHCDF 0x80080E8C (512 bytes), slot 0, records table not
    // initialised (zero, as a real card has it), ten EMPTY career records (rec[0] = 1, 0x8005EBD4),
    // each sealed (0x8005EA54 runs before the file is created).
    uint8_t* b = card.raw.data() + static_cast<size_t>(free) * kBlock;
    std::memset(b, 0, kBlock);
    g.ReadBlock(0x80080E8Cu, b, 512u);
    for (int slot = 0; slot < 10; ++slot) {
        uint8_t* rec = Record(card, slot);
        Put32(rec, 1);
        Seal(rec, slot);
    }
    return true;
}

bool SaveCareer(CardImage& card, GuestRam& g, int slot, std::string& error) {
    if (slot < 0 || slot > 9) {
        error = "career slot out of range";
        return false;
    }
    if (!EnsureGameFile(card, g, error)) return false;
    uint8_t* b = card.raw.data() + static_cast<size_t>(card.block) * kBlock;
    b[0x200] = static_cast<uint8_t>(slot); // 0x8006CDC4
    uint8_t* rec = Record(card, slot);
    Put32(rec, 0);                          // in use (0x8006CDFC)
    g.ReadBlock(kPlayers, rec + 4, 216u);   // 0x8006CDF8
    g.ReadBlock(kSession, rec + 0xDC, 256u); // 0x8006CE0C
    Seal(rec, slot);
    return true;
}

bool LoadCareer(const CardImage& card, GuestRam& g, int slot, std::string& error) {
    if (card.block <= 0 || slot < 0 || slot > 9) {
        error = "no career on the card";
        return false;
    }
    const uint8_t* rec = Record(card, slot);
    uint32_t a = 0, b = 0;
    CardChecksumHost(rec, slot, &a, &b);
    if (Get32(rec) != 0) {
        error = "career slot " + std::to_string(slot) + " is empty";
        return false;
    }
    if (a != Get32(rec + 0x1DC) || b != Get32(rec + 0x1E0)) {
        error = "career slot " + std::to_string(slot) + " fails its checksum";
        return false;
    }
    g.WriteBlock(kPlayers, rec + 4, 216u);   // 0x8006CEA8
    g.WriteBlock(kSession, rec + 0xDC, 256u); // 0x8006CEBC
    return true;
}

std::string CheckCard(const CardImage& card, int* mismatches) {
    std::string out;
    int bad = 0;
    char line[160];
    if (card.block <= 0) {
        if (mismatches) *mismatches = -1;
        return "no " + std::string(kCardFileName) + " file on the card\n";
    }
    std::snprintf(line, sizeof(line), "file %s in block %d, current slot %u\n", kCardFileName, card.block,
                  card.raw[static_cast<size_t>(card.block) * kBlock + 0x200]);
    out += line;
    for (int slot = 0; slot < 10; ++slot) {
        const uint8_t* rec = Record(card, slot);
        uint32_t a = 0, b = 0;
        CardChecksumHost(rec, slot, &a, &b);
        const bool ok = a == Get32(rec + 0x1DC) && b == Get32(rec + 0x1E0);
        if (!ok) ++bad;
        std::snprintf(line, sizeof(line), "  slot %d %-6s checksum %08X %08X  %s\n", slot, Get32(rec) == 0 ? "in use" : "empty",
                      a, b, ok ? "equal to the card" : "DIFFERS from the card");
        out += line;
    }
    if (mismatches) *mismatches = bad;
    return out;
}

} // namespace rr::shell
