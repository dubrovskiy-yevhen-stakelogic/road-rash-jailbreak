// The race loader's set-up path in the product (loader_product.h).
#include "game/loader_product.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game/audio/root_counter.h" // GetRCnt(2) (BuildRace)
#include "game/sim/ai.h"         // MemSet32 SLUS 0x8001E100
#include "game/sim/road_query.h" // GuestCopyWords SLUS 0x8001E0B4

namespace rr::game {

namespace s = rr::sim;
using s::GuestRam;

namespace {
LoaderCounts g_counts;
constexpr uint32_t kOverlayAt = 0x8005B5E8u;
constexpr uint32_t kOverlayMax = 0x80080000u - 0x8005B5E8u; // RASHCDI.BIN is ~0x16000 bytes

std::string GuestString(GuestRam& g, uint32_t a, size_t max = 256) {
    std::string s;
    for (size_t k = 0; k < max; ++k) {
        const uint8_t c = g.U8(a + static_cast<uint32_t>(k));
        if (c == 0 || g.Faulted()) break;
        s.push_back(static_cast<char>(c));
    }
    return s;
}

void Refuse(const std::string& why) {
    ++g_counts.refused;
    if (g_counts.firstRefusal.empty()) g_counts.firstRefusal = why;
}
} // namespace

bool LoaderPorted() {
    static const bool on = [] {
        const char* v = std::getenv("RRJB_LOADER");
        return !(v != nullptr && std::strcmp(v, "off") == 0);
    }();
    return on;
}

LoaderCounts& LoaderTotals() { return g_counts; }

bool Loader2On() {
    static const bool on = [] {
        const char* v = std::getenv("RRJB_LOADER2");
        return !(v != nullptr && std::strcmp(v, "off") == 0);
    }();
    return on && LoaderPorted();
}

namespace {
Loader2Counts g_counts2;
}
Loader2Counts& Loader2Totals() { return g_counts2; }
void Loader2Answered(const std::string& what) {
    for (const std::string& s : g_counts2.answered)
        if (s == what) return;
    g_counts2.answered.push_back(what);
}

std::string Loader2Line() {
    const Loader2Counts& c = g_counts2;
    if (!Loader2On()) return "loader2: RRJB_LOADER2=off - BuildRace's and SetUpRace's children stay the session's transcriptions (the negative control)";
    std::string answered;
    for (const std::string& s : c.answered) answered += (answered.empty() ? "" : "; ") + s;
    char b[1600];
    std::snprintf(b, sizeof(b),
                  "loader2: BuildRace and SetUpRace's children PORTED (sim\\loader2.h, rows_loader2.inc): BuildRace %zu, "
                  "WorldPoolsInit %zu, PoolTableInit %zu, TrialLimits %zu, PopBlockInit %zu (PedTablesInit %zu, PartSlotsInit %zu, "
                  "CensusInit %zu), SpeedClassCopy %zu (0x80052FA0 = %u %u %u), EscapeLoad %zu (STARTJBA records %u / %u), "
                  "AnimNoise %zu (%u record(s)), FightLoad %zu, SirenSet %zu (gp+1908 = %d), FxGlobalsInit %zu, FxReset %zu; answered at the "
                  "session's own points: %s%s%s",
                  c.buildRace, c.worldPools, c.poolTable, c.trialLimits, c.popBlock, c.pedTables, c.partSlots, c.census,
                  c.speedClass, c.speedClasses[0], c.speedClasses[1], c.speedClasses[2], c.escapeLoad, c.jailRecords[0],
                  c.jailRecords[1], c.animNoise, c.noiseRecords, c.fightLoad, c.sirenSet, c.siren, c.fxGlobals, c.fxReset,
                  answered.empty() ? "none" : answered.c_str(), c.refused.empty() ? "" : " - REFUSED: ", c.refused.c_str());
    return b;
}


std::string LoaderFnName(uint32_t fn) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08X", fn);
    return b;
}

std::string LoaderTotalsLine() {
    const LoaderCounts& c = g_counts;
    if (!LoaderPorted()) return "loader: RRJB_LOADER=off - the set-up path's transcriptions stand (the negative control)";
    std::string deferred;
    for (uint32_t fn : c.deferredFns) deferred += (deferred.empty() ? "" : " ") + LoaderFnName(fn);
    char b[1400];
    std::snprintf(b, sizeof(b),
                  "loader: the set-up path PORTED (sim\\loader.h, rows_loader.inc): EnterRace %zu, RaceReset %zu, SetUpRace %zu, "
                  "FinishTableInit %zu, RaceBlockCopy %zu, PieceListInit %zu, SkyInit %zu, ResTableInit %zu, TexSetUp %zu (the "
                  "page table 0x800D5F70: LectSheet / KnbpClut / RimTim / GtpLoad; VRAM uploads %zu the renderer's), "
                  "ModelTablesInit %zu, LoadBikeBank %zu (RigLoad %zu), TexFile on the bundle %zu, GeoLoad on its own %zu (ChunkWalk "
                  "and the four handlers under it), CarModels %zu, HazardModels %zu, RenderCamInit %zu, PopulationReset %zu, "
                  "EffectSheet %zu, SoundInit %zu, "
                  "SoundLoad %zu, SoundSpuAttr %zu, ModelBind (bikes / riders / weapons) %zu; host blocks %zu, host reads "
                  "%zu; %zu call(s) DEFERRED to the session's own points (%s); %zu refused%s%s",
                  c.enterRace, c.raceReset, c.setUpRace, c.finishInit, c.blockCopy, c.pieceList, c.skyInit, c.resTable,
                  c.texSetUp, c.vramUploads,
                  c.modelTables, c.bikeBank, c.rigLoads, c.texFiles, c.geoLoads, c.carModels, c.hazardModels, c.renderCams,
                  c.popReset, c.effectSheet,
                  c.soundInit, c.soundLoad, c.spuAttr, c.modelBinds, c.hostBlocks, c.hostReads, c.deferred, deferred.c_str(), c.refused,
                  c.firstRefusal.empty() ? "" : " - first: ", c.firstRefusal.c_str());
    return b;
}

// ============================================================================ the overlay
namespace {
// The overlay as the loader leaves it: RASHCDI.BIN from the disc at the race's start, then every write the ported
// functions make into its data while it is laid (the TSLP records 0x8006B898, the hazard block 0x8006B8B4 / 0x8006EB10)
// kept for the next scope - and for the grid (grid_session.cpp), as the console keeps the overlay resident.
std::vector<uint8_t> g_overlay;
} // namespace

void LoaderOverlayReset(const DiscImage& disc) {
    g_overlay.clear();
    if (const auto f = disc.Find("RASHCDI.BIN")) g_overlay = disc.ReadFile(*f);
}

const std::vector<uint8_t>& LoaderOverlayImage() { return g_overlay; }

LoaderOverlay::LoaderOverlay(GuestRam& g, const DiscImage& disc) : g_(g) {
    if (g_overlay.empty()) LoaderOverlayReset(disc);
    const std::vector<uint8_t>& bytes = g_overlay;
    if (bytes.empty() || bytes.size() > kOverlayMax) return;
    // nested: already laid when the overlay's first bytes are there
    std::vector<uint8_t> head(64);
    g_.ReadBlock(kOverlayAt, head.data(), 64);
    ok_ = true;
    if (std::memcmp(head.data(), bytes.data(), 64) == 0) return;
    saved_.resize(bytes.size());
    g_.ReadBlock(kOverlayAt, saved_.data(), static_cast<uint32_t>(saved_.size()));
    g_.WriteBlock(kOverlayAt, bytes.data(), static_cast<uint32_t>(bytes.size()));
    laid_ = true;
}

LoaderOverlay::~LoaderOverlay() {
    if (!laid_) return;
    g_.ReadBlock(kOverlayAt, g_overlay.data(), static_cast<uint32_t>(g_overlay.size())); // the loader's writes, kept
    g_.WriteBlock(kOverlayAt, saved_.data(), static_cast<uint32_t>(saved_.size()));
}

// ============================================================================ the callees
uint32_t ProductLoaderCallees::Block(uint32_t n) {
    if (n == 0u || from_ == nullptr) return 0; // SLUS 0x8001447C: 0 for n == 0
    if (*from_ < 0x80008000u) {                // no region handed over: never the bottom of RAM (BuildRace's reserve is 0x8000E000..)
        error = "no block region for the host's reads";
        return 0;
    }
    const uint32_t size = (n + 11u) & ~7u;
    const uint32_t at = (*from_ + 7u) & ~7u;
    if (at + size > limit_) {
        error = "the host's block region is full (" + std::to_string(n) + " bytes asked)";
        return 0;
    }
    *from_ = at + size;
    lastBlock_ = at;
    ++g_counts.hostBlocks;
    return at + 4u;
}

bool ProductLoaderCallees::Call(uint32_t fn, const uint32_t* a, int n, uint32_t sp, uint32_t& v0) {
    v0 = 0;
    if (site_) {
        bool handled = false;
        const bool ok = site_(fn, a, n, sp, v0, handled);
        if (handled) {
            if (!ok && error.empty()) error = "the session refused " + LoaderFnName(fn);
            return ok;
        }
    }
    switch (fn) {
    case s::kLdMalloc:                                                    // the host's block
        v0 = (a[1] < 2u) ? Block(a[0]) : 0u;
        return error.empty();
    case s::kLdFree:                                                      // the bump region gives back its top block
        if (from_ != nullptr && lastBlock_ != 0u && a[0] == lastBlock_ + 4u) {
            *from_ = lastBlock_;
            lastBlock_ = 0;
        }
        return true;
    case s::kLdMemset: {                                                  // PORTED MemSet32
        const uint32_t dst = a[0], len = a[2];
        if ((len & 3u) != 0u || (dst & 0x1FFFFFu) + len > GuestRam::kRamSize) {
            error = "memset " + LoaderFnName(dst) + " length " + std::to_string(len) + " (the console would not return)";
            return false;
        }
        if (ram_ != nullptr) {
            s::MemSet32(ram_ + (dst & 0x1FFFFFu), a[1], len);
        } else { // the same word loop on the view (SLUS 0x8001E100)
            const uint32_t w = a[1] | (a[1] << 8) | (a[1] << 16) | (a[1] << 24); // the full argument, as MemSet32
            for (uint32_t k = 0; k < len; k += 4) g_.W32(dst + k, w);
        }
        return !g_.Faulted();
    }
    case s::kLdMemcpy:                                                    // PORTED GuestCopyWords
        v0 = s::GuestCopyWords(g_, a[0], a[1], a[2]);
        return !g_.Faulted();
    case s::kLdBzero:                                                     // BIOS A(28h), the host's
        for (uint32_t k = 0; k < a[1]; ++k) g_.W8(a[0] + k, 0);
        return !g_.Faulted();
    case s::kLdSprintf: {                                                 // the host's: only %s and %% are used
        const std::string fmt = GuestString(g_, a[1]);
        std::string out;
        int arg = 2;
        for (size_t k = 0; k < fmt.size(); ++k) {
            if (fmt[k] != '%') {
                out.push_back(fmt[k]);
                continue;
            }
            const char c = k + 1 < fmt.size() ? fmt[++k] : '\0';
            if (c == '%') {
                out.push_back('%');
            } else if (c == 's' && arg < n) {
                out += GuestString(g_, a[arg++]);
            } else {
                error = "sprintf format '" + fmt + "' has a conversion the host does not serve";
                return false;
            }
        }
        for (size_t k = 0; k <= out.size(); ++k) g_.W8(a[0] + static_cast<uint32_t>(k), k < out.size() ? static_cast<uint8_t>(out[k]) : 0u);
        v0 = static_cast<uint32_t>(out.size());
        return !g_.Faulted();
    }
    case s::kLdLoadFile: {                                                // the CD read: the disc image, at once
        std::string name = GuestString(g_, a[0]);
        for (char& ch : name)
            if (ch == '\\') ch = '/';
        std::vector<uint8_t> bytes;
        if (disc_ != nullptr)
            if (const auto f = disc_->Find(name)) bytes = disc_->ReadFile(*f);
        if (bytes.empty()) {
            v0 = 0xFFFFFFFFu;
            return true;
        }
        const uint32_t at = Block(static_cast<uint32_t>(bytes.size()));
        if (at == 0u) return false;
        g_.WriteBlock(at, bytes.data(), static_cast<uint32_t>(bytes.size()));
        lastFile = at;
        g_.W32(a[2], at);
        g_.W32(a[3], static_cast<uint32_t>(bytes.size()));
        v0 = static_cast<uint32_t>(bytes.size());                         // 0x80014B40's return: the length
        ++g_counts.hostReads;
        return !g_.Faulted();
    }
    case s::kL2StrRChr: {                                                 // BIOS A(1Fh) strrchr: the host's
        v0 = 0;
        for (uint32_t k = 0; k < 4096u; ++k) {
            const uint8_t ch = g_.U8(a[0] + k);
            if (ch == static_cast<uint8_t>(a[1])) v0 = a[0] + k;
            if (ch == 0 || g_.Faulted()) break;
        }
        return !g_.Faulted();
    }
    case s::kL2StrCpy:                                                    // BIOS A(19h) strcpy: the host's
    case s::kL2StrCat: {                                                  // BIOS A(15h) strcat: the host's
        uint32_t d = a[0];
        if (fn == s::kL2StrCat)
            while (g_.U8(d) != 0 && !g_.Faulted() && d - a[0] < 4096u) ++d;
        for (uint32_t k = 0; k < 4096u; ++k) {
            const uint8_t ch = g_.U8(a[1] + k);
            g_.W8(d + k, ch);
            if (ch == 0 || g_.Faulted()) break;
        }
        v0 = a[0];
        return !g_.Faulted();
    }
    case s::kL2Printf:                                                    // BIOS A(3Fh) printf: the debug print, not run
        return true;
    case s::kL2GetRCnt:                                                   // GetRCnt(2): the product's root counter
        v0 = ConsoleRootCounter2();
        return true;
    case s::kLdReadTim:                                                   // libgs OpenTIM + ReadTIM: the host's
        return HostReadTim(a[0], a[1]);
    case s::kLdTimFile: {                                                 // a .TIM off the CD, read: the host's
        std::string name = GuestString(g_, a[0]);
        for (char& ch : name)
            if (ch == '\\') ch = '/';
        std::vector<uint8_t> bytes;
        if (disc_ != nullptr)
            if (const auto f = disc_->Find(name)) bytes = disc_->ReadFile(*f);
        const uint32_t at = bytes.empty() ? 0u : Block(static_cast<uint32_t>(bytes.size()));
        if (at == 0u) {
            v0 = 0xFFFFFFFFu;
            return true;
        }
        g_.WriteBlock(at, bytes.data(), static_cast<uint32_t>(bytes.size()));
        g_.W32(a[1], at);
        ++g_counts.hostReads;
        return HostReadTim(a[2], at);
    }
    case s::kLdLoadImage:                                                 // VRAM: the renderer's own uploads
        ++g_counts.vramUploads;
        return true;
    case s::kLdDrawSync:
        return true;
    default:
        break;
    }
    ++g_counts.deferred;
    if (std::find(g_counts.deferredFns.begin(), g_counts.deferredFns.end(), fn) == g_counts.deferredFns.end())
        g_counts.deferredFns.push_back(fn);
    return true;
}

bool ProductLoaderCallees::HostReadTim(uint32_t out, uint32_t tim) {
    const uint32_t flag = g_.U32(tim + 4u);
    uint32_t p = tim + 8u, crect = 0, caddr = 0;
    if (g_.U32(tim) != 0x10u) {
        g_.W32(out + 16u, 0);                                             // SLUS 0x80014070: the failure arm
        return !g_.Faulted();
    }
    if (flag & 8u) {
        crect = p + 4u;
        caddr = p + 12u;
        p += g_.U32(p);
    }
    g_.W32(out, flag);
    g_.W32(out + 4u, crect);
    g_.W32(out + 8u, caddr);
    g_.W32(out + 12u, p + 4u);
    g_.W32(out + 16u, p + 12u);
    return !g_.Faulted();
}

// ============================================================================ the hooks
std::string LoaderEnterRace(GuestRam& g, uint8_t* ram, const DiscImage& disc, uint32_t sp, uint32_t from, uint32_t limit) {
    LoaderOverlayReset(disc); // a new race: the overlay fresh off the disc
    LoaderOverlay ov(g, disc);
    if (!ov.ok()) return "loader: EnterRace NOT run - RASHCDI.BIN is not on the disc";
    std::string why;
    ProductLoaderCallees* self = nullptr;
    uint32_t next = from; // the gtp / rim files' blocks: given back as they are freed
    ProductLoaderCallees c(g, ram, &disc, &next, limit,
                           [&](uint32_t fn, const uint32_t*, int, uint32_t csp, uint32_t&, bool& handled) {
                               if (fn == s::kLdTexSetUpFn) { // the texture page table, PORTED
                                   handled = true;
                                   ++g_counts.texSetUp;
                                   return s::TexSetUp(g, csp, *self);
                               }
                               if (fn == s::kL2FxResetFn && Loader2On()) { // FxReset PORTED
                                   handled = true;
                                   s::FxReset(g);
                                   ++g_counts2.fxReset;
                                   return !g.Faulted();
                               }
                               if (fn != s::kLdRaceResetFn) return true;
                               handled = true;                                    // RaceReset, PORTED
                               ++g_counts.raceReset;
                               return s::RaceReset(g, csp, *self);
                           });
    self = &c;
    ++g_counts.enterRace;
    const bool ok = s::EnterRace(g, sp, c);
    s::FinishTableInit(g);                                                // RASHCDI 0x8006A7C0
    ++g_counts.finishInit;
    if (!ok || g.Faulted()) {
        g.ClearFault();
        Refuse("EnterRace: " + c.error);
        return "loader: EnterRace RASHCDI 0x80063B90 (PORTED) REFUSED: " + c.error;
    }
    char b[400];
    std::snprintf(b, sizeof(b),
                  "loader: EnterRace RASHCDI 0x80063B90 and RaceReset 0x80063500 PORTED (game_state+0x03 = 1, +0x00 = %u, "
                  "the pedestrian switch 0x8005B254 = %u, the players' blocks 0x800D81F0 cleared), the finish table "
                  "0x8006A7C0 PORTED; their loaders run at the session's own points (DEFERRED, the LOADER line)",
                  g.U8(g.U32(s::kLdGameStatePtr)), g.U32(s::kLdPedSwitch));
    return b;
}

bool LoaderModelTables(GuestRam& g, uint8_t* ram, const DiscImage& disc, uint32_t sp, std::string& error) {
    LoaderOverlay ov(g, disc);
    if (!ov.ok()) {
        error = "RASHCDI.BIN is not on the disc";
        return false;
    }
    ProductLoaderCallees c(g, ram, &disc, nullptr, 0);
    ++g_counts.modelTables;
    if (!s::ModelTablesInit(g, sp, c) || g.Faulted()) {
        g.ClearFault();
        error = "ModelTablesInit RASHCDI 0x8005BE40 (PORTED) refused " + c.error;
        Refuse(error);
        return false;
    }
    return true;
}

namespace {
std::vector<uint32_t> RegistryIds(GuestRam& g) {
    std::vector<uint32_t> v;
    for (uint32_t k = 0; k < 50u; ++k) v.push_back(g.U32(s::kLdRegistry + 16u * k));
    return v;
}
void NewModels(GuestRam& g, const std::vector<uint32_t>& before, std::vector<uint32_t>& models) {
    for (uint32_t k = 0; k < 50u; ++k) {
        const uint32_t id = g.U32(s::kLdRegistry + 16u * k);
        if (id != 0u && id != before[k]) models.push_back(id);
    }
}
} // namespace

bool LoaderGeo(GuestRam& g, uint8_t* ram, const DiscImage* disc, const std::vector<uint8_t>& file, uint32_t& from,
               uint32_t limit, uint32_t sp, std::vector<uint32_t>& models, uint32_t* fileAt, std::string& error) {
    uint32_t next = from;
    ProductLoaderCallees* self = nullptr;
    uint32_t placed = 0;
    ProductLoaderCallees c(g, ram, disc, &next, limit,
                           [&](uint32_t fn, const uint32_t* a, int, uint32_t, uint32_t&, bool& handled) {
                               if (fn != s::kLdLoadFile) return true;
                               handled = true;                                    // the file the caller read
                               placed = self->Block(static_cast<uint32_t>(file.size()));
                               if (placed == 0u) return false;
                               g.WriteBlock(placed, file.data(), static_cast<uint32_t>(file.size()));
                               g.W32(a[2], placed);
                               g.W32(a[3], static_cast<uint32_t>(file.size()));
                               ++g_counts.hostReads;
                               return true;
                           });
    self = &c;
    // the name the original passes lives in its caller's frame; here a scratch string on the stack
    const uint32_t name = sp - 512u;
    for (uint32_t k = 0; k < 8u; ++k) g.W8(name + k, static_cast<uint8_t>("FILE.GEO"[k]));
    g.W8(name + 8u, 0);
    const std::vector<uint32_t> before = RegistryIds(g);
    int32_t v0 = 0;
    ++g_counts.geoLoads;
    const bool ok = s::GeoLoad(g, name, 0, sp, c, v0);
    if (!ok || v0 != 0 || g.Faulted()) {
        g.ClearFault();
        error = "GeoLoad RASHCDI 0x8005CA10 (PORTED) returned " + std::to_string(v0) + (c.error.empty() ? "" : ": " + c.error);
        Refuse(error);
        return false;
    }
    NewModels(g, before, models);
    if (fileAt != nullptr) *fileAt = placed;
    from = next;
    return true;
}

bool LoaderCarModels(GuestRam& g, uint8_t* ram, const DiscImage& disc, int raceId, uint32_t& from, uint32_t limit,
                     uint32_t sp, std::vector<uint32_t>& models, std::string& files, uint32_t* fileAt, std::string& error) {
    LoaderOverlay ov(g, disc);
    if (!ov.ok()) {
        error = "RASHCDI.BIN is not on the disc";
        return false;
    }
    uint32_t next = from;
    ProductLoaderCallees* self = nullptr;
    ProductLoaderCallees c(g, ram, &disc, &next, limit,
                           [&](uint32_t fn, const uint32_t* a, int, uint32_t, uint32_t&, bool&) {
                               if (fn == s::kLdLoadFile) files += (files.empty() ? "" : ", ") + GuestString(g, a[0]);
                               return true;                                       // answered by the defaults
                           });
    self = &c;
    (void)self;
    const std::vector<uint32_t> before = RegistryIds(g);
    int32_t v0 = 0;
    ++g_counts.carModels;
    const bool ok = s::CarModels(g, raceId, sp, c, v0);
    if (!ok || g.Faulted()) {
        g.ClearFault();
        error = "CarModels RASHCDI 0x8005C630 (PORTED) refused" + (c.error.empty() ? "" : ": " + c.error);
        Refuse(error);
        return false;
    }
    NewModels(g, before, models);
    if (fileAt != nullptr) *fileAt = c.lastFile;
    from = next;
    return true;
}

std::string LoaderBikeBank(GuestRam& g, const DiscImage& disc, int bank, uint32_t& from, uint32_t limit) {
    LoaderOverlay ov(g, disc);
    if (!ov.ok()) return "loader: the bike bank NOT loaded - RASHCDI.BIN is not on the disc";
    std::string error;
    if (!LoaderModelTables(g, nullptr, disc, kLoaderSp, error)) return "loader: " + error;
    g_counts.tablesReady = true;
    uint32_t next = from;
    std::string files;
    ProductLoaderCallees* self = nullptr;
    ProductLoaderCallees c(g, nullptr, &disc, &next, limit,
                           [&](uint32_t fn, const uint32_t* a, int, uint32_t, uint32_t& v0, bool& handled) {
                               if (fn == s::kLdLoadFile) files += (files.empty() ? "" : ", ") + GuestString(g, a[0]);
                               if (fn != s::kLdReadInto) return true;
                               handled = true;                                    // the rig .MRO into the given buffer
                               std::string name = GuestString(g, a[0]);
                               for (char& ch : name)
                                   if (ch == '\\') ch = '/';
                               files += (files.empty() ? "" : ", ") + name;
                               std::vector<uint8_t> bytes;
                               if (const auto f = disc.Find(name)) bytes = disc.ReadFile(*f);
                               if (bytes.empty()) {
                                   v0 = 0xFFFFFFFFu;
                                   return true;
                               }
                               g.WriteBlock(a[2], bytes.data(), static_cast<uint32_t>(bytes.size()));
                               g.W32(a[3], static_cast<uint32_t>(bytes.size()));
                               ++g_counts.hostReads;
                               ++g_counts.rigLoads;
                               (void)self;
                               return !g.Faulted();
                           });
    self = &c;
    const std::vector<uint32_t> before = RegistryIds(g);
    int32_t v0 = 0;
    ++g_counts.bikeBank;
    const bool ok = s::LoadBikeBank(g, bank, kLoaderSp, c, v0);
    if (!ok || v0 != 0 || g.Faulted()) {
        g.ClearFault();
        Refuse("LoadBikeBank: " + c.error);
        return "loader: LoadBikeBank RASHCDI 0x8005C45C (PORTED) REFUSED (" + std::to_string(v0) + "): " + c.error;
    }
    std::vector<uint32_t> models;
    NewModels(g, before, models);
    std::string ids;
    for (uint32_t k = 0; k < 50u; ++k) {
        const uint32_t id = g.U32(s::kLdRegistry + 16u * k);
        if (id != 0u && std::find(models.begin(), models.end(), id) != models.end())
            ids += (ids.empty() ? "" : " ") + std::to_string(id) + "@" + std::to_string(k);
    }
    // heap 0 (0x800D6500) for the PORTED ModelBind's part arrays (RegistryBind's malloc SLUS 0x8001447C -> 0x800142B4,
    // spine.h SpineMalloc, PORTED): one free block of the arena after the bank (OURS: where and how big)
    uint32_t heapAt = 0;
    constexpr uint32_t kHeap0Bytes = 0x8000u;
    if (((next + 7u) & ~7u) + kHeap0Bytes <= limit) {
        heapAt = (next + 7u) & ~7u;
        next = heapAt + kHeap0Bytes;
        g.W32(0x800D6500u, heapAt);
        g.W32(heapAt, 0);
        g.W32(heapAt + 4u, kHeap0Bytes);
    }
    const uint32_t start = from;
    from = next;
    char b[700];
    std::snprintf(b, sizeof(b),
                  "loader: RaceReset's ModelTablesInit 0x8005BE40 and LoadBikeBank RASHCDI 0x8005C45C PORTED (GeoLoad / "
                  "TexFile / ChunkWalk / RigLoad under it): %s; models %s (registry slot after '@'), placed OURS at "
                  "0x%08X..0x%08X; the LECT / KNBP / TSLP arms (VRAM) the renderer's; heap 0 = one free block of 32 KiB "
                  "at 0x%08X (the PORTED ModelBind's part arrays)",
                  files.c_str(), ids.c_str(), start, next, heapAt);
    return b;
}

bool LoaderTexSection(GuestRam& g, const DiscImage& disc, const uint8_t* bytes, uint32_t n, uint32_t at, int& ctkp,
                      int& skipped, std::string& error) {
    LoaderOverlay ov(g, disc);
    std::vector<uint8_t> saved(n);
    g.ReadBlock(at, saved.data(), n);
    g.WriteBlock(at, bytes, n);
    ProductLoaderCallees c(g, nullptr, &disc, nullptr, 0,
                           [&](uint32_t fn, const uint32_t*, int, uint32_t, uint32_t&, bool& handled) {
                               if (fn == s::kLdLect || fn == s::kLdKnbp || fn == s::kLdTslp) {
                                   handled = true;                                // VRAM: the renderer's
                                   ++skipped;
                               }
                               return true;
                           });
    for (uint32_t p = 0; p + 8u <= n;) {                                  // the CTKP chunks (for the log)
        uint32_t tag = 0, len = 0;
        std::memcpy(&tag, bytes + p, 4);
        std::memcpy(&len, bytes + p + 4, 4);
        if (tag == 0x504B5443u) ++ctkp;
        if (len == 0) break;
        p += len;
    }
    ++g_counts.texFiles;
    const bool ok = s::TexFile(g, at, static_cast<int32_t>(n), kLoaderSp, c) && !g.Faulted();
    g.WriteBlock(at, saved.data(), n);                                    // the bundle's buffer: transient
    if (!ok) {
        g.ClearFault();
        error = "TexFile RASHCDI 0x8005C920 (PORTED) refused " + c.error;
        Refuse(error);
    }
    return ok;
}

bool LoaderHazardModels(GuestRam& g, const DiscImage& disc, int set, uint32_t& from, uint32_t limit,
                        std::vector<uint32_t>& models, uint32_t* fileAt, std::string& error) {
    LoaderOverlay ov(g, disc);
    if (!ov.ok()) {
        error = "RASHCDI.BIN is not on the disc";
        return false;
    }
    uint32_t next = from;
    ProductLoaderCallees c(g, nullptr, &disc, &next, limit);
    const std::vector<uint32_t> before = RegistryIds(g);
    int32_t v0 = 0;
    ++g_counts.hazardModels;
    const bool ok = s::HazardModels(g, set, kLoaderSp, c, v0);
    if (!ok || v0 != 1 || g.Faulted()) {
        g.ClearFault();
        error = "HazardModels RASHCDI 0x8005C7F0 (PORTED) refused" + (c.error.empty() ? "" : ": " + c.error);
        Refuse(error);
        return false;
    }
    NewModels(g, before, models);
    if (fileAt != nullptr) *fileAt = c.lastFile; // the .GEO: the second read (the .TEX's block was given back)
    from = next;
    return true;
}

bool LoaderPopReset(GuestRam& g, uint8_t* ram, const DiscImage& disc, uint32_t sp, std::string& error) {
    LoaderOverlay ov(g, disc);
    ProductLoaderCallees c(g, ram, &disc, nullptr, 0);
    ++g_counts.popReset;
    if (!s::PopulationReset(g, sp, c) || g.Faulted()) {
        g.ClearFault();
        error = "PopulationReset RASHCDI 0x80068D54 (PORTED) refused " + c.error;
        Refuse(error);
        return false;
    }
    return true;
}

bool LoaderBlockCopy(GuestRam& g, uint8_t* ram, const DiscImage& disc, const uint8_t* block, uint32_t blockAt, uint32_t sp,
                     int32_t& eventCount, uint8_t (&events)[48], std::string& error) {
    LoaderOverlay ov(g, disc);
    if (!ov.ok()) {
        error = "RASHCDI.BIN is not on the disc";
        return false;
    }
    std::vector<uint8_t> savedBlock(292);
    g.ReadBlock(blockAt, savedBlock.data(), 292);
    g.WriteBlock(blockAt, block, 292);
    ProductLoaderCallees c(g, ram, &disc, nullptr, 0);
    ++g_counts.blockCopy;
    const bool ok = s::RaceBlockCopy(g, blockAt, sp, c) && !g.Faulted();
    eventCount = g.S32(s::kLdHazardEvents);                               // the overlay's data, read before it goes
    g.ReadBlock(s::kLdHazardTemplates, events, 48);
    g.WriteBlock(blockAt, savedBlock.data(), 292);                        // BuildRace's frame: transient
    if (!ok) {
        g.ClearFault();
        error = "RaceBlockCopy RASHCDI 0x80068470 (PORTED) refused " + c.error;
        Refuse(error);
    }
    return ok;
}

std::string LoaderSetUpRace(GuestRam& g, uint8_t* ram, const DiscImage* disc, uint32_t sp,
                            const ProductLoaderCallees::Site& site) {
    // SetUpRace reads no data of the overlay's own (its strings and tables are the executable's): no overlay
    ProductLoaderCallees c(g, ram, disc, nullptr, 0, site);
    ++g_counts.setUpRace;
    if (!s::SetUpRace(g, sp, c) || g.Faulted()) {
        g.ClearFault();
        Refuse("SetUpRace: " + c.error);
        return "loader: SetUpRace RASHCDI 0x80063670 (PORTED) REFUSED: " + c.error;
    }
    const uint32_t gs = g.U32(s::kLdGameStatePtr);
    char b[400];
    std::snprintf(b, sizeof(b),
                  "SetUpRace RASHCDI 0x80063670 PORTED (race type 0x%02X): the clock triple 0, time limit %d ticks (%d s), "
                  "the fight table *(0x8005AD4C) = 0x%08X, game_state+0x34 = %u; its children at the session's own "
                  "points (the start record, BuildRace, HazardPick / HazardSetup, the animation objects with its count)",
                  g.U8(gs + 4u), g.S32(s::kLdTimeLimit), g.S32(s::kLdTimeLimit) / 300, g.U32(s::kLdFightPtr),
                  g.U32(gs + 52u));
    return b;
}

bool LoaderStreamSetUp(GuestRam& g, uint8_t* ram, const DiscImage* disc, uint32_t buffers, uint32_t sky, uint32_t sp,
                       std::string& error) {
    // neither function reads data of the overlay's own: no overlay
    ProductLoaderCallees c(g, ram, disc, nullptr, 0,
                           [&](uint32_t fn, const uint32_t* a, int, uint32_t, uint32_t& v0, bool& handled) {
                               if (fn != s::kLdMalloc) return true;
                               handled = true;                                    // the session's two blocks
                               v0 = a[1] == 1u ? sky : buffers;
                               ++g_counts.hostBlocks;
                               return true;
                           });
    bool ok = true;
    if (buffers != 0u) {                                                  // the road-map load's and ResInit's part
        s::PieceListInit(g);
        ++g_counts.pieceList;
        ++g_counts.resTable;
        ok = s::ResTableInit(g, sp, c);
    }
    if (ok && sky != 0u) {
        ++g_counts.skyInit;
        ok = s::SkyInit(g, sp, c);
    }
    if (!ok || g.Faulted()) {
        g.ClearFault();
        error = "the stream set-up (PORTED ResTableInit / SkyInit) refused " + c.error;
        Refuse(error);
        return false;
    }
    return true;
}

bool LoaderEffectSheet(GuestRam& g, uint8_t* ram, const DiscImage& disc, const std::vector<uint8_t>& section, uint32_t at,
                       uint32_t sp, const LoaderUpload& upload, std::string& error) {
    LoaderOverlay ov(g, disc);
    if (!ov.ok()) {
        error = "RASHCDI.BIN is not on the disc";
        return false;
    }
    g.WriteBlock(at, section.data(), static_cast<uint32_t>(section.size()));
    ProductLoaderCallees c(g, ram, &disc, nullptr, 0,
                           [&](uint32_t fn, const uint32_t* a, int, uint32_t, uint32_t&, bool& handled) {
                               if (fn == s::kLdReadTim) {                         // libgs OpenTIM + ReadTIM: the host's
                                   handled = true;
                                   const uint32_t tim = a[1], out = a[0];
                                   const uint32_t flag = g.U32(tim + 4u);
                                   uint32_t p = tim + 8u, crect = 0, caddr = 0;
                                   if (g.U32(tim) != 0x10u) {
                                       g.W32(out + 16u, 0);                        // 0x80014070: the failure arm
                                       return true;
                                   }
                                   if (flag & 8u) {
                                       crect = p + 4u;
                                       caddr = p + 12u;
                                       p += g.U32(p);
                                   }
                                   g.W32(out, flag);
                                   g.W32(out + 4u, crect);
                                   g.W32(out + 8u, caddr);
                                   g.W32(out + 12u, p + 4u);
                                   g.W32(out + 16u, p + 12u);
                                   return !g.Faulted();
                               }
                               if (fn == s::kLdLoadImage) {                       // libgpu LoadImage: the renderer's
                                   handled = true;
                                   if (upload)
                                       upload(g.S16(a[0]), g.S16(a[0] + 2u), g.S16(a[0] + 4u), g.S16(a[0] + 6u), a[1]);
                                   return true;
                               }
                               return true;
                           });
    ++g_counts.effectSheet;
    const bool ok = s::EffectSheet(g, 0, at, sp, c) && !g.Faulted();
    if (!ok) {
        g.ClearFault();
        error = "EffectSheet RASHCDI 0x80061FAC (PORTED) refused " + c.error;
        Refuse(error);
    }
    return ok;
}

} // namespace rr::game
