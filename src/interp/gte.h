#pragma once
// COP2 - the PlayStation Geometry Transformation Engine, integer-exact.
//
// Derived from the public hardware documentation (psx-spx / no$psx "Geometry Transformation Engine"
// chapter): register file, command word encoding, the 44-bit MAC accumulators with per-term overflow
// detection, the IR/SXY/SZ/colour saturation rules, the FLAG register, and the UNR reciprocal table
// used by the RTPS/RTPT perspective divide. No emulator source was consulted or copied.
//
// Anything whose hardware behaviour is a documented *bug* and which no shipped game is expected to
// use (MVMVA matrix select 3) raises `unimplemented` instead of guessing. The caller is expected to
// treat that as a hard failure, not as a no-op.
#include <cstdint>
#include <string>

namespace rr::interp {

class Gte {
public:
    // cop2r0..31 (data) and cop2r32..63 (control) as raw 32-bit words, which is exactly how a
    // savestate stores them and how mfc2/mtc2/cfc2/ctc2 see them.
    uint32_t dr[32]{};
    uint32_t cr[32]{};

    // Set when a command this implementation refuses to guess at is executed. The CPU turns this
    // into a trap; it is never silently ignored.
    bool unimplemented = false;
    std::string unimplementedDetail;

    void Reset();

    // Register access with the hardware's read/write quirks (IRGB/ORGB, the SXYP mirror, LZCS/LZCR,
    // the sign-extending control registers).
    uint32_t ReadData(uint32_t reg) const;
    void WriteData(uint32_t reg, uint32_t value);
    uint32_t ReadControl(uint32_t reg) const;
    void WriteControl(uint32_t reg, uint32_t value);

    // `command` is the full 32-bit COP2 instruction word.
    void Execute(uint32_t command);

    uint32_t flag() const { return cr[31]; }

private:
    // Decoded from the current command word.
    bool sf_ = false; // 1 -> results are shifted right by 12
    bool lm_ = false; // 1 -> IR1..3 clamp to 0..0x7FFF instead of -0x8000..0x7FFF
    int shift_ = 0;   // sf_ ? 12 : 0

    void SetFlag(int bit) { cr[31] |= (1u << bit); }

    // 44-bit signed accumulator handling for MAC1..3 (n = 1..3).
    int64_t CheckMac(int n, int64_t value);
    int32_t CheckMac0(int64_t value);

    int32_t ClampIr(int n, int32_t value, bool lm);
    void SetIr(int n, int32_t value, bool lm);
    void SetMacAndIr(int n, int64_t acc, bool lm);

    int32_t ClampSxy(int32_t value, int flagBit);
    uint16_t ClampSz(int64_t value);
    uint8_t ClampColor(int32_t value, int flagBit);

    void PushSz(int64_t value);
    void PushSxy(int32_t x, int32_t y);
    void PushColor(int32_t r, int32_t g, int32_t b);
    void PushColorFromMac();

    // Matrix / vector / translation accessors (all sign-extended out of the packed registers).
    int16_t Rt(int row, int col) const;
    int16_t Llm(int row, int col) const;
    int16_t Lcm(int row, int col) const;
    int32_t Tr(int i) const;
    int32_t Bk(int i) const;
    int32_t Fc(int i) const;
    int16_t V(int vec, int i) const;
    int16_t Ir(int i) const;

    // The shared "multiply matrix by vector, add translation" core. `matrix`: 0 = rotation,
    // 1 = light, 2 = colour. `translation`: 0 = TR, 1 = BK, 2 = FC (hardware-bugged), 3 = none.
    void MatrixVector(int matrix, int translation, const int16_t v[3], bool lm);

    void DoRtps(int vector, bool last);
    void DoNclip();
    void DoAvsz(int count);
    void DoMvmva(uint32_t command);
    void DoSquare();
    void DoOuterProduct();
    void DoGpf(bool useIr0Only);
    void DoInterpolateTail(int64_t mac1, int64_t mac2, int64_t mac3);
    void DoDpcs(bool fromRgbFifo);
    void DoIntpl();
    void DoDcpl();
    void DoNormalColour(int vector, bool depthCue, bool colourMul, bool useRgbc);
    void DoCc();
    void DoCdp();

    uint32_t Divide(uint32_t h, uint32_t sz3);
};

} // namespace rr::interp
