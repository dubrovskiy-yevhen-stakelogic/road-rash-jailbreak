// Bench rows of the race loader's camera set-up, the level's light stores and the collision set-up
// (src\game\sim\camera_setup.{h,cpp}), RASHCDI SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06, RASHCDG and SLUS_010.53.
// Included LAST by verify_physics.cpp inside its anonymous namespace, after rows_loader.inc (ldr:: is visible).
//
// The machine is a race capture (rr-race by default); the RASHCDI rows lay the whole overlay over 0x8005B5E8 first.
// NAMED BOUNDS:
//   * the CD layer is the host's: open SLUS 0x8001458C a stub answering a handle (3..6) or -1, read SLUS 0x80014780 a
//     stub copying the planted copy of the player's own DATA\CAMERA*.CA (work\disc_us) - or random bytes - to a1, close
//     0x8001460C and the print 0x80044894 `jr ra`;
//   * a camera's handle is a pool-4 handle (0x80..0x9F): CameraInit's RouteBind resolves it, and the only callers pass
//     0x9F / 0x9E;
//   * a camera target is a live bike of pool 0 (road_runtime's RrEntity, stirred), whose +0x43C is its runtime record.
namespace l2c {

namespace s = rr::sim;
using pop::Chance;
using pop::Put8;

// read(handle, dst, n, off): copies `words` words from `src` to a1, v0 = 4 * words.
void StubReadWords(Memory& m, uint32_t fn, uint32_t src, uint32_t words) {
    const uint32_t w[11] = {0x3C080000u | (src >> 16), 0x35080000u | (src & 0xFFFFu), 0x24090000u | words, // li t1,words
                            0x8D0A0000u,  // lw t2,0(t0)
                            0x25080004u,  // addiu t0,t0,4
                            0xACAA0000u,  // sw t2,0(a1)
                            0x2529FFFFu,  // addiu t1,t1,-1
                            0x1520FFFBu,  // bnez t1,-5
                            0x24A50004u,  // addiu a1,a1,4 (delay)
                            0x03E00008u, 0x24020000u | (4u * words)};
    for (uint32_t k = 0; k < 11u; ++k) m.PokeWord(fn + 4u * k, w[k]);
}

const char* const kCamFiles[4] = {"DATA\\CAMERA.CA", "DATA\\CAMERAS.CA", "DATA\\CAMERA2.CA", "DATA\\CAMERA2S.CA"};

// The CD stubs and the planted file (at scratch + 4096) on both machines.
void CdStubs(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t src = c.scratch + 4096u;
    const std::vector<uint8_t>& f = ldr::DiscFile(kCamFiles[c.sample ? 0u : r.U32() % 4u]);
    if (!c.sample && Chance(r, 6)) {
        for (uint32_t k = 0; k < 224u; k += 4) m.PokeWord(src + k, r.U32());
    } else {
        ldr::Plant(m, f, src, 224u);
    }
    ldr::StubV0(m, s::kCdOpen, (!c.sample && Chance(r, 8)) ? 0xFFFFFFFFu : 3u + r.U32() % 4u);
    StubReadWords(m, s::kCdRead, src, 56u);
    ldr::JrRa(m, s::kCdClose);
    ldr::JrRa(m, s::kPrintf);
}

bool Ram(uint32_t a) { return a >= 0x80000000u && a < 0x80200000u; }

// A camera target: a live bike of pool 0 (stirred on random cases), its +0x43C a runtime record.
uint32_t Target(CaseContext& c) {
    Memory& m = *c.mem;
    const uint32_t e = c.sample ? m.PeekWord(0x8005B38Cu) : RrEntity(c, RqRead(m), true, 0);
    if (!Ram(m.PeekWord(e + 0x43Cu))) m.PokeWord(e + 0x43Cu, c.scratch + 2048u);
    if (!c.sample && Chance(*c.rng, 2)) Put8(m, m.PeekWord(e + 0x43Cu), c.rng->U32());
    return e;
}

void StirGameState(CaseContext& c) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t gs = ldr::Gs(m);
    Put8(m, gs + 4u, Chance(r, 3) ? r.U32() : ldr::AnyType(r));
    Put8(m, gs + 6u, r.U32() % 18u);                                     // NAMED BOUND: a player slot of pool 0
}

// ---------------------------------------------------------------------------- CameraInit SLUS 0x8002F308
void SetupInit(CaseContext& c, Args& a) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    a.a[0] = Target(c);
    a.a[1] = s::kCamView0 + (c.sample ? 0u : s::kCamViewStride * (r.U32() % 2u));
    a.a[2] = c.sample ? 0x9Fu : (Chance(r, 2) ? 0x9Fu - r.U32() % 2u : 0x80u + r.U32() % 32u);
    if (!c.sample && Chance(r, 3)) a.a[2] |= r.U32() & 0xFFFF0000u;      // only the halfword is stored
    a.a[3] = c.sample ? 0u : (Chance(r, 2) ? r.U32() % 8u : r.U32());
    a.a4 = c.sample ? 0u : (Chance(r, 2) ? 0u : (Chance(r, 2) ? 1u : r.U32()));
    a.more[0] = 1;
    if (c.sample) return;
    StirGameState(c);
    for (uint32_t k = 0; k < 16u; ++k) m.PokeWord(a.a[1] + 4u * (r.U32() % 283u), r.U32()); // the record's old words
}

// ---------------------------------------------------------------------------- CameraFileRead RASHCDI 0x80069618
void SetupFile(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    CdStubs(c);
    const char name[] = "DATA\\CAMERA.ca";
    c.mem->WriteBlock(c.scratch, name, sizeof(name));
    a.a[0] = c.scratch;
    if (c.sample) return;
    c.mem->PokeWord(s::kLdCdMode, c.rng->U32() % 4u);
    for (uint32_t k = 0; k < 224u; k += 4) c.mem->PokeWord(s::kCamFileBuf + k, c.rng->U32());
}

// ---------------------------------------------------------------------------- CameraSetUp RASHCDI 0x80067564
void SetupSetUp(CaseContext& c, Args&) {
    gridr::InjectOverlay(c);
    CdStubs(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const uint32_t p1 = m.PeekWord(0x8005B38Cu);
    if (!Ram(m.PeekWord(p1 + 0x43Cu))) m.PokeWord(p1 + 0x43Cu, c.scratch + 2048u);
    if (c.sample) return;
    const uint32_t gs = ldr::Gs(m);
    m.PokeWord(gs + 0x30u, Chance(r, 6) ? r.U32() : 1u + r.U32() % 2u);
    m.PokeWord(p1 + 0xB4u, Chance(r, 6) ? r.U32() : r.U32() % 20u);
    m.PokeWord(s::kCamIntroSwitch, Chance(r, 2) ? 0u : (Chance(r, 2) ? 1u : r.U32()));
    m.PokeWord(0x800D83B0u, Chance(r, 3) ? 0u : r.U32());
    StirGameState(c);
    const uint32_t pool = m.PeekWord(kPoolBasePtr);
    m.PokeWord(s::kCamPlayer2Bike, Chance(r, 2) ? 0u : pool + 1096u * (r.U32() % 18u));
    for (uint32_t k = 0; k < 18u; ++k) {                                  // every target's runtime record
        const uint32_t e = pool + 1096u * k;
        if (!Ram(m.PeekWord(e + 0x43Cu))) m.PokeWord(e + 0x43Cu, c.scratch + 2048u);
    }
    Put8(m, m.PeekWord(p1 + 0x43Cu), r.U32());
    for (uint32_t v = 0; v < 2u; ++v)
        for (uint32_t k = 0; k < 8u; ++k) m.PokeWord(s::kCamView0 + s::kCamViewStride * v + 4u * (r.U32() % 283u), r.U32());
}

// ---------------------------------------------------------------------------- the level bundle's sections
struct Sections {
    std::vector<size_t> light, shade; // payload offsets in GAMEBIN1.DAT
};
const Sections& BundleSections() {
    static Sections x = [] {
        Sections sc;
        const std::vector<uint8_t>& f = ldr::DiscFile("DATA\\GAMEBIN1.DAT");
        if (f.empty()) return sc;
        const size_t n = rr::LevelBundleCount(f);
        for (size_t i = 0; i < n; ++i) {
            try {
                const rr::LevelBundle b = rr::ParseLevelBundle(f, i);
                if (const rr::LevelBundleSection* s2 = b.Find(2); s2 && s2->payload + 36 <= f.size()) sc.light.push_back(s2->payload);
                if (const rr::LevelBundleSection* s7 = b.Find(7); s7 && s7->payload + 0x488 <= f.size()) sc.shade.push_back(s7->payload);
            } catch (const std::exception&) {
            }
        }
        return sc;
    }();
    return x;
}

void SetupLight(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const Sections& sc = BundleSections();
    const uint32_t at = c.scratch;
    const std::vector<uint8_t>& f = ldr::DiscFile("DATA\\GAMEBIN1.DAT");
    if (!sc.light.empty() && (c.sample || !Chance(r, 3))) {
        m.WriteBlock(at, f.data() + sc.light[c.sample ? 0u : r.U32() % sc.light.size()], 36u);
        if (!c.sample && Chance(r, 3)) m.PokeWord(at + 4u * (r.U32() % 9u), r.U32());
    } else {
        for (uint32_t k = 0; k < 36u; k += 4) m.PokeWord(at + k, r.U32());
    }
    a.a[0] = c.sample ? 0u : r.U32();
    a.a[1] = at;
    if (c.sample) return;
    for (uint32_t k = 0; k < 0x54u; k += 4) m.PokeWord(s::kLightBlock + k, r.U32());
}

void SetupShade(CaseContext& c, Args& a) {
    gridr::InjectOverlay(c);
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    const Sections& sc = BundleSections();
    const uint32_t at = c.scratch;
    const std::vector<uint8_t>& f = ldr::DiscFile("DATA\\GAMEBIN1.DAT");
    if (!sc.shade.empty() && (c.sample || !Chance(r, 4))) {
        m.WriteBlock(at, f.data() + sc.shade[c.sample ? 0u : r.U32() % sc.shade.size()], 0x488u);
        if (!c.sample && Chance(r, 2)) m.PokeWord(at + 4u * (r.U32() % 0x122u), r.U32());
    } else {
        for (uint32_t k = 0; k < 0x488u; k += 4) m.PokeWord(at + k, r.U32());
    }
    a.a[0] = c.sample ? 0u : r.U32();
    a.a[1] = at;
    if (c.sample) return;
    for (uint32_t k = 0; k < 0x54u; k += 4) m.PokeWord(s::kLightBlock + k, r.U32());
}

void SetupColl(CaseContext& c, Args&) {
    if (c.sample) return;
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    Put8(m, 0x800CCFA8u + 256u, r.U32());
    Put8(m, 0x800CCFA8u + 257u, r.U32());
    m.PokeWord(0x800CCF78u, r.U32());
    m.PokeWord(0x800CCF7Cu, r.U32());
}

} // namespace l2c

void AddLoader2CamRows(std::vector<Row>& rows) {
    namespace s = rr::sim;
    using ldr::Make;
    using ldr::Run;
    rows.push_back(Make("camera_init", "void CameraInit(Entity *t, View *v, u16 handle, s32 mode, s32 director, s32)  [SLUS 0x8002F308]",
                        s::kCameraInitFn, l2c::SetupInit, [](NativeEnv& env, const Args& a) {
                            s::GuestRam g = pop::View(env);
                            s::RoadRuntimeNative road;
                            const bool ok = s::CameraInit(g, a.a[0], a.a[1], a.a[2], a.a[3], a.a4, env.sp, road);
                            return pop::PopDone(env, g, ok, "CameraInit");
                        }, false));
    rows.push_back(Make("camera_file_read", "void CameraFileRead(char *name)  [RASHCDI 0x80069618]", s::kCamFileReadFn,
                        l2c::SetupFile, [](NativeEnv& env, const Args& a) {
                            return Run(env, "CameraFileRead", [&](s::GuestRam& g, ldr::Seams& c, uint32_t&) {
                                return s::CameraFileRead(g, a.a[0], env.sp, c);
                            });
                        }, false,
                        ldr::Callees({{s::kCdOpen, 2}, {s::kCdRead, 4}, {s::kCdClose, 1}, {s::kPrintf, 2}})));
    rows.push_back(Make("camera_set_up", "void CameraSetUp(void)  [RASHCDI 0x80067564]", s::kCamSetUpFn, l2c::SetupSetUp,
                        [](NativeEnv& env, const Args&) {
                            return Run(env, "CameraSetUp", [&](s::GuestRam& g, ldr::Seams& c, uint32_t&) {
                                return s::CameraSetUp(g, env.sp, c);
                            });
                        }, false,
                        [] {
                            std::vector<OracleCallee> k = ldr::Callees({{s::kLdSprintf, 3}, {s::kCamFileReadFn, 1}});
                            k.push_back(OracleCallee{s::kCameraInitFn, 4, 1});
                            return k;
                        }()));
    rows.back().allowBiosCalls = true; // sprintf's strlen / strcpy go through the BIOS vectors (the kernel runs them)
    rows.push_back(Make("level_light", "void LevelLight(u32, u32 *section)  [RASHCDI 0x8006250C]", s::kLevelLightFn,
                        l2c::SetupLight, [](NativeEnv& env, const Args& a) {
                            s::GuestRam g = pop::View(env);
                            return pop::PopDone(env, g, s::LevelLight(g, a.a[1], env.sp), "LevelLight");
                        }, false));
    rows.push_back(Make("level_shade", "void LevelShade(u32, u8 *section)  [RASHCDI 0x80062430]", s::kLevelShadeFn,
                        l2c::SetupShade, [](NativeEnv& env, const Args& a) {
                            s::GuestRam g = pop::View(env);
                            return pop::PopDone(env, g, s::LevelShade(g, a.a[1]), "LevelShade");
                        }, false));
    rows.push_back(Make("collision_set_up", "void CollisionSetUp(void)  [RASHCDG 0x800A41EC]", s::kCollSetUpFn, l2c::SetupColl,
                        [](NativeEnv& env, const Args&) {
                            s::GuestRam g = pop::View(env);
                            s::CollisionSetUp(g);
                            return pop::PopDone(env, g, true, "CollisionSetUp");
                        }, false));
}
