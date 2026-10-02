#include "game/sim/ai_globals.h"

#include <cstdio>

namespace rr::sim {

namespace {
constexpr uint32_t kGameStatePtr = 0x8005B2F8;
// RASHCDI 0x8006469C..0x800649C8: the 41 contiguous copies, in order (dst, file offset, bytes).
const AiGlobalsCopy kHead[] = {
    {0x8005ADC0, 0x000, 4},
    {0x8005ADC4, 0x004, 8},
    {0x80052F50, 0x00C, 80},
    {0x80052EE4, 0x05C, 108},
    {0x8005ADE0, 0x0C8, 4},
    {0x8005ADE4, 0x0CC, 4},
    {0x8005ADE8, 0x0D0, 4},
    {0x8005ADEC, 0x0D4, 4},
    {0x8005ADF0, 0x0D8, 4},
    {0x80053174, 0x0DC, 20},
    {0x80053018, 0x0F0, 12},
    {0x80052FAC, 0x0FC, 36},
    {0x80052FD0, 0x120, 36},
    {0x80052FF4, 0x144, 36},
    {0x80053024, 0x168, 12},
    {0x80053188, 0x174, 12},
    {0x80053194, 0x180, 12},
    {0x800531A0, 0x18C, 12},
    {0x80053030, 0x198, 12},
    {0x8005303C, 0x1A4, 12},
    {0x80053048, 0x1B0, 12},
    {0x80053054, 0x1BC, 12},
    {0x80053060, 0x1C8, 12},
    {0x8005306C, 0x1D4, 12},
    {0x80053078, 0x1E0, 12},
    {0x80053084, 0x1EC, 12},
    {0x80053090, 0x1F8, 12},
    {0x8005309C, 0x204, 12},
    {0x800530A8, 0x210, 24},
    {0x800530C0, 0x228, 24},
    {0x800530FC, 0x240, 24},
    {0x80053114, 0x258, 12},
    {0x80053120, 0x264, 24},
    {0x80053138, 0x27C, 12},
    {0x800530D8, 0x288, 12},
    {0x800530E4, 0x294, 12},
    {0x800530F0, 0x2A0, 12},
    {0x80053144, 0x2AC, 12},
    {0x80053150, 0x2B8, 12},
    {0x8005315C, 0x2C4, 12},
    {0x80053168, 0x2D0, 12},
};
} // namespace

const std::vector<AiGlobalsCopy>& AiGlobalsCopies() {
    static const std::vector<AiGlobalsCopy> list = [] {
        std::vector<AiGlobalsCopy> v(std::begin(kHead), std::end(kHead));
        // 0x80064A6C..0x80064B18: the profile (row 0 here), then the tail walked by s0 from sp+40+0xA5C.
        v.push_back({kAiProfileDst, kAiProfileBase, kAiProfileBytes});
        v.push_back({0x8005ADCC, 0xA5C, 8});
        v.push_back({0x8005ADD4, 0xA64, 8});
        v.push_back({0x8005ADDC, 0xA6C, 4});
        v.push_back({0x800531E8, 0xA70, 40});
        v.push_back({0x80053210, 0xA98, 20});
        return v;
    }();
    return list;
}

int32_t AiProfileRow(GuestRam& g) {
    const uint32_t gs = g.U32(kGameStatePtr);
    int32_t a1;
    const uint32_t flags = g.U8(gs + 4u);
    if (g.U32(kAiTwoPlayerSel) != 0u) {                   // 0x800649D8
        a1 = static_cast<int32_t>((0u - (flags & 1u)) & static_cast<uint32_t>(-5)) + 7;  // 7, or 2 with bit 0 (-5 + 7)
    } else if ((flags & 4u) && flags != 44u) {            // 0x80064A20 / 0x80064A28
        a1 = 6;
    } else {
        int32_t v1 = static_cast<int32_t>((flags & 1u) << 1);
        if (flags & 8u) v1 += 4;
        a1 = v1 + ((g.U32(gs + 48u) == 2u) ? 1 : 0);      // 0x80064A58..0x80064A68
    }
    return static_cast<int32_t>(g.S16(gs + 58u)) + 4 * a1; // 0x80064A80..0x80064A88
}

bool LoadAiGlobals(GuestRam& g, const std::vector<uint8_t>& file, std::string& why) {
    if (file.size() < kAiGlobalsBytes) {
        why = "GLOBALS.BI is shorter than the 2732 bytes the loader reads";
        return false;
    }
    const int32_t row = AiProfileRow(g);
    if (g.Faulted() || row < 0 || kAiProfileBase + kAiProfileBytes * static_cast<uint32_t>(row) + kAiProfileBytes >
                                      kAiGlobalsBytes) {
        why = "the profile selector named row " + std::to_string(row) + ", outside the file's 32";
        return false;
    }
    for (const AiGlobalsCopy& c : AiGlobalsCopies()) {
        const uint32_t off = (c.dst == kAiProfileDst) ? c.offset + kAiProfileBytes * static_cast<uint32_t>(row)
                                                      : c.offset;
        g.WriteBlock(c.dst, file.data() + off, c.bytes);
    }
    return !g.Faulted();
}

size_t CheckAiGlobals(uint8_t* ram, uint32_t gp, const std::vector<uint8_t>& file, bool mutate,
                      std::string& report) {
    GuestRam g(ram, gp);
    if (file.size() < kAiGlobalsBytes) {
        report = "GLOBALS.BI short";
        return 1;
    }
    int32_t row = AiProfileRow(g);
    if (mutate) row = (row + 1) % 32;
    size_t differ = 0, bytes = 0;
    for (const AiGlobalsCopy& c : AiGlobalsCopies()) {
        const uint32_t off = (c.dst == kAiProfileDst) ? c.offset + kAiProfileBytes * static_cast<uint32_t>(row)
                                                      : c.offset;
        for (uint32_t k = 0; k < c.bytes; ++k) {
            ++bytes;
            if (g.U8(c.dst + k) != file[off + k]) ++differ;
        }
    }
    char b[160];
    std::snprintf(b, sizeof(b), "%zu destination byte(s) of 46 copies, %zu differ; profile row %d", bytes, differ,
                  static_cast<int>(row));
    report = b;
    if (g.Faulted()) return differ + 1;
    return differ;
}

} // namespace rr::sim
