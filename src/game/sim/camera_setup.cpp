// The camera set-up, the level's light stores and the collision set-up (camera_setup.h), ported from our own
// disassembly of RASHCDI.BIN (SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06), RASHCDG.BIN and SLUS_010.53
// (SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1). The instruction addresses in the comments are the original's.
#include "game/sim/camera_setup.h"

#include "game/sim/fixed.h"
#include "game/sim/vec.h"

namespace rr::sim {
namespace {

bool Call(LoaderCallees& c, uint32_t fn, std::initializer_list<uint32_t> args, uint32_t sp, uint32_t* v0 = nullptr) {
    uint32_t a[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int n = 0;
    for (uint32_t x : args)
        if (n < 8) a[n++] = x;
    uint32_t r = 0;
    const bool ok = c.Call(fn, a, n, sp, r);
    if (v0 != nullptr) *v0 = r;
    return ok;
}

constexpr uint32_t kGameState = 0x8005B2F8, kPlayer1Bike = 0x8005B38C, kPlayers = 0x8005B3A0;
constexpr uint32_t kSinTable = 0x8005624C;

} // namespace

// ---------------------------------------------------------------------------- SLUS 0x8002F308
bool CameraInit(GuestRam& g, uint32_t t, uint32_t v, uint32_t handle, uint32_t mode, uint32_t director, uint32_t sp,
                RoadRuntimeCallees& road) {
    const uint32_t F = sp - kCamInitFrame;
    g.W32(v + 0x238u, t);                                                  // 0x8002F33C
    for (uint32_t k = 0; k < 0xACu; k += 4) g.W32(v + k, 0);              // memset(v, 0, 172), by words
    g.W32(v + 0x130u, 0x18000u);
    g.W32(v + 0x134u, 0x2000u);
    g.W16(v + 0x140u, 1);
    g.W32(v + 0x224u, 0x100u);
    g.W16(v + 0xACu, static_cast<uint16_t>(handle));
    g.W32(v + 0x220u, mode);
    g.W32(v + 0x21Cu, mode);
    g.W32(v + 0xB4u, 0);
    g.W32(v + 0x138u, 0x1CCCCu);
    g.W32(v + 0x13Cu, 0);
    g.W32(v + 0x228u, 0);
    g.W32(v + 0x294u, 0);
    g.W32(v + 0x248u, 0);
    g.W32(v + 0x26Cu, 0);
    g.W32(v + 0x268u, 0);
    g.W32(v + 0x45Cu, 0x10000u);                                           // 0x8002F3AC (the delay slot)
    if (director != 0) {                                                   // 0x8002F3B0
        const uint32_t w = g.U32(v + 0x228u);
        g.W32(v + 0x304u, 1);
        g.W32(v + 0x228u, w | 0x40u);
    } else {                                                               // 0x8002F3C8
        uint32_t on = 0;
        if (g.U8(g.U32(t + 0x43Cu)) & 0x20u) on = 1;
        else if (g.U8(g.U32(kGameState) + 4u) & 1u) on = 1;
        g.W32(v + 0x304u, on);
        if (on != 0) {
            g.W32(v + 0x21Cu, 7);
            g.W32(v + 0x224u, g.U32(v + 0x224u) & 0xFFFFFEFFu);
        } else {
            g.W32(v + 0x224u, g.U32(v + 0x224u) | 6u);
        }
    }
    GuestCopyWords(g, v + 0x148u, t + 0x148u, 32);                         // 0x8002F440
    GuestCopyWords(g, v + 0x168u, t + 0x168u, 12);                         // 0x8002F450
    RouteBind(g, v + 0xACu, 0, t, F, road);                                // 0x8002F460
    g.W32(v + 0x1ECu, g.U32(t + 0x1ECu));
    g.W32(v + 0x1F0u, g.U32(t + 0x1F0u));
    GuestCopyWords(g, v + 0x174u, t + 0x174u, 56);                         // 0x8002F480
    g.W32(v + 0xB8u, g.U32(t + 0x1F8u));
    g.W32(v + 0xBCu, g.U32(t + 0x1FCu));
    const uint32_t z = g.U32(t + 0x200u);                                  // 0x8002F4A0
    g.W32(v + 0x2D4u, 0);
    g.W32(v + 0x2D0u, 0);
    g.W32(v + 0x2E0u, 0);
    g.W32(v + 0x2DCu, 0);
    g.W32(v + 0xC0u, z);
    return !g.Faulted();
}

// ---------------------------------------------------------------------------- RASHCDI 0x80069618
bool CameraFileRead(GuestRam& g, uint32_t name, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kCamFileReadFrame;
    uint32_t h = 0;
    if (!Call(c, kCdOpen, {name, g.U32(kLdCdMode)}, F, &h)) return false;
    if (static_cast<int32_t>(h) < 0) return Call(c, kPrintf, {0x8005B804u, name}, F); // "Cannot open %s for reading"
    if (!Call(c, kCdRead, {h, kCamFileBuf, kCamFileRead, 0}, F)) return false;
    return Call(c, kCdClose, {h}, F);
}

// ---------------------------------------------------------------------------- RASHCDI 0x80067564
bool CameraSetUp(GuestRam& g, uint32_t sp, LoaderCallees& c) {
    const uint32_t F = sp - kCamSetUpFrame;
    const uint32_t s1 = kCamView0;
    g.W32(s1 + 0x21Cu, g.U32(s1 + 0x220u));                                // 0x80067584
    const uint32_t players = g.U32(g.U32(kGameState) + 0x30u);
    const uint32_t kind = g.U32(g.U32(kPlayer1Bike) + 0xB4u);
    const bool fast = (kind - 6u) < 3u || (kind - 15u) < 3u;              // the sltiu pairs
    const uint32_t suffix = players == 2u ? (fast ? 0x8005B900u : 0x8005B904u) : (fast ? 0x8005B908u : 0x8005B90Cu);
    if (!Call(c, kLdSprintf, {kCamNameBuf, 0x8005B8F8u, 0x800524C8u, suffix}, F)) return false; // 0x80067644
    if (!Call(c, kLdSprintf, {F + 24u, 0x8005B910u, kCamNameBuf}, F)) return false;             // "%s.ca"
    if (!Call(c, kCamFileReadFn, {F + 24u}, F)) return false;
    uint32_t s2 = 0;
    const uint32_t intro = g.U32(kCamIntroSwitch);
    g.W32(0x800CDBA0u, 0);                                                 // 0x8006767C (the delay slot): view 0 +0x308
    if (intro != 0) s2 = g.U32(0x800D83B0u) != 0u ? 1u : 0u;               // `sltu s2,zero,v0`: the shot table's word
    const uint32_t gs = g.U32(kGameState);
    uint32_t s0 = 0;
    if (g.U8(gs + 4u) & 1u) s0 = g.U32(kPlayers) + 1096u * g.U8(gs + 6u); // 0x800676B0: the mission's player record
    const uint32_t a0 = s0 != 0 ? s0 : g.U32(kPlayer1Bike);
    if (!Call(c, kCameraInitFn, {a0, s1, 0x9Fu, 0, s2, 1}, F)) return false;
    const uint32_t p2 = g.U32(kCamPlayer2Bike);
    g.W32(s1 + 0x2Cu, 0);                                                  // 0x80067720 (the delay slot)
    if (p2 == 0) return !g.Faulted();
    if (!Call(c, kCameraInitFn, {s0 != 0 ? s0 : p2, s1 + kCamViewStride, 0x9Eu, 0, s2, 1}, F)) return false;
    g.W32(s1 + 0x30u, 0x8000u);                                            // 0x80067748
    g.W32(s1 + 1176u, 0x8000u);
    const uint32_t w = g.U32(s1 + 1680u);
    g.W32(s1 + 1180u, 0);
    g.W32(s1 + 1680u, w | 0x01000000u);
    return !g.Faulted();
}

// ---------------------------------------------------------------------------- RASHCDI 0x8006250C
bool LevelLight(GuestRam& g, uint32_t s2, uint32_t sp) {
    (void)sp; // the frame holds only Scale's output (sp - 64 + 16), which the port keeps on the host
    const uint32_t s1 = kLightBlock;
    const int32_t w7 = g.S32(s2 + 28u), w8 = g.S32(s2 + 32u);
    const int32_t s5 = static_cast<int32_t>(static_cast<uint32_t>(w7) * 11u); // (x * 3 << 2) - x
    const uint32_t i = static_cast<uint32_t>(s5) & 0xFFFu;
    g.W16(s1 + 40u, static_cast<uint16_t>(0u - g.U16(kSinTable + 4u * i)));     // 0x8006257C
    const int32_t s0 = static_cast<int32_t>(static_cast<uint32_t>(w8) * 11u);
    g.W16(s1 + 44u, static_cast<uint16_t>(0u - g.U16(kSinTable + 4u * i + 2u))); // 0x800625A0
    const int32_t v1 = s0 < 0 ? s0 + 3 : s0;                               // the divide by 4's bias
    const int32_t s3 = v1 >> 2;
    g.W16(s1 + 42u, g.U16(kSinTable + (static_cast<uint32_t>(v1) & 0x3FFCu))); // 0x800625C8 (the delay slot)
    const int16_t dir[3] = {g.S16(s1 + 40u), g.S16(s1 + 42u), g.S16(s1 + 44u)};
    int32_t out[3];
    Scale(static_cast<int32_t>(0xFF9C0000u), dir, out);                    // 0x800625C4: Scale(-100 << 16, s1 + 40, sp + 16)
    g.W16(s1 + 16u, static_cast<uint16_t>(out[0] >> 10));
    g.W16(s1 + 18u, static_cast<uint16_t>(out[1] >> 10));
    g.W16(s1 + 20u, static_cast<uint16_t>(out[2] >> 10));
    const uint16_t c = g.U16(kSinTable + 4u * (static_cast<uint32_t>(s0) & 0xFFFu));
    g.W32(s1 + 76u, static_cast<uint32_t>(s5));
    g.W16(s1 + 42u, c);
    g.W32(s1 + 80u, static_cast<uint32_t>(s3));
    g.W32(s1 + 24u, g.U32(s2 + 24u));                                      // 0x80062610
    g.W32(s1 + 28u, g.U32(s2 + 0u));
    g.W32(s1 + 32u, g.U32(s2 + 4u));
    g.W32(s1 + 36u, g.U32(s2 + 8u));
    g.W32(s1 + 48u, g.U32(s2 + 12u));
    for (uint32_t k = 0; k < 3u; ++k) {                                    // 0x80062650 .. 0x80062680: FixMul x 3
        const int32_t a = static_cast<int32_t>(0x10000u - g.U32(s1 + 48u));
        g.W32(s1 + 52u + 4u * k, static_cast<uint32_t>(FixMul(a, g.S32(s2 + 4u * k))));
    }
    g.W32(s1 + 4u, g.U32(s2 + 16u));                                       // 0x80062684
    g.W32(s1 + 8u, g.U32(s2 + 20u));
    const uint32_t b10 = g.U8(s2 + 10u), b6 = g.U8(s2 + 6u), b2 = g.U8(s2 + 2u);
    g.W32(s1, (b10 << 16) | (b6 << 8) | b2);                               // 0x800626D4
    return !g.Faulted();
}

// ---------------------------------------------------------------------------- RASHCDI 0x80062430
bool LevelShade(GuestRam& g, uint32_t a1) {
    const uint32_t s1 = kLightBlock;
    g.W32(s1 + 64u, g.U32(a1 + 0x480u));                                   // 0x8006245C
    const uint32_t step = (g.U16(a1 + 0x484u) - 256u) & 0xFFFFu;
    g.W16(s1 + 68u, static_cast<uint16_t>(step));
    g.W32(s1 + 72u, 256u - step);                                          // 0x80062480 (the delay slot)
    GuestCopyWords(g, kShadeTable, a1, 0x480u);                            // 0x8006247C
    const auto sixth = [](int32_t x) {                                     // mult 0x2AAAAAAB, mfhi, minus the sign
        const int32_t hi = static_cast<int32_t>((static_cast<int64_t>(x) * 0x2AAAAAAB) >> 32);
        return static_cast<uint32_t>(hi - (x >> 31));
    };
    const int32_t x2 = 255 - static_cast<int32_t>(g.U8(kShadeTable + 2u));
    const int32_t x1 = 255 - static_cast<int32_t>(g.U8(kShadeTable + 1u));
    const int32_t x0 = 255 - static_cast<int32_t>(g.U8(kShadeTable));
    g.W32(s1 + 12u, (sixth(x2) << 16) | (sixth(x1) << 8) | sixth(x0));    // 0x800624F0
    return !g.Faulted();
}

// ---------------------------------------------------------------------------- RASHCDG 0x800A41EC
void CollisionSetUp(GuestRam& g) {
    g.W8(0x800CCFA8u + 256u, 128);                                         // node 128: the pair loops' sentinel
    g.W8(0x800CCFA8u + 257u, 255);
    g.W32(0x800CCF78u, 1);                                                 // the chain words
    g.W32(0x800CCF7Cu, 0);
}

} // namespace rr::sim
