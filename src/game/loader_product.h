#pragma once
// The race loader in the product: the race loader's set-up path PORTED in
// src\game\sim\loader.{h,cpp} (bench rows tools\rrverify\rows_loader.inc), run on the session's arena with the
// product's callees. What stays the host's, named: every malloc is a block of the session's bump region (the
// original's heap rule for the pointer it returns, OURS where); every CD read is the disc image read at once into
// such a block; the GPU / SPU library is the renderer's and the SPU model's; the callees the session performs
// at its own point of the set-up are answered as DEFERRED (named in the race log by address) - they run where
// the session runs them, most of them PORTED on their own rows.
//
// RRJB_LOADER=off is the negative control: the session's earlier transcriptions stand, nothing here runs.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "game/sim/loader.h"
#include "game/sim/loader2.h"
#include "rrvfs/disc_image.h"

namespace rr::game {

bool LoaderPorted();
constexpr uint32_t kLoaderSp = 0x801FF000u; // OURS: the stack the ported loader runs on (the world pass's, race_session.cpp)

// What the ported loader did in this process (the race log's LOADER line).
struct LoaderCounts {
    size_t enterRace = 0, raceReset = 0, setUpRace = 0, finishInit = 0, blockCopy = 0, pieceList = 0, skyInit = 0,
           resTable = 0, modelTables = 0, geoLoads = 0, carModels = 0, popReset = 0, effectSheet = 0, soundInit = 0,
           soundLoad = 0, loadBank = 0, spuAttr = 0, modelBinds = 0;
    size_t hostBlocks = 0, hostReads = 0, deferred = 0, refused = 0;
    std::vector<uint32_t> deferredFns; // each once
    std::string firstRefusal;
    int32_t hazardPick = 0;  // the session's HazardPick v0 (SetUpRace's 0x8006AD4C answered with it)
    int32_t animCount = -1;  // SetUpRace's count argument of 0x8005D1A0 (0x80063844)
    bool tablesReady = false; // ModelTablesInit ran before the bank (the traffic arena then does not run it again)
    size_t bikeBank = 0, texFiles = 0, rigLoads = 0, hazardModels = 0, renderCams = 0, texSetUp = 0, vramUploads = 0;
};
LoaderCounts& LoaderTotals();
std::string LoaderTotalsLine();

// RASHCDI.BIN over 0x8005B5E8 for the scope (the console has the loader overlay resident while the race loads);
// the arena's own bytes there come back at the end. Nested scopes are no-ops.
void LoaderOverlayReset(const DiscImage& disc);
const std::vector<uint8_t>& LoaderOverlayImage(); // RASHCDI with the loader's writes (the grid lays this one)
class LoaderOverlay {
public:
    LoaderOverlay(rr::sim::GuestRam& g, const DiscImage& disc);
    ~LoaderOverlay();
    bool ok() const { return ok_; }
    LoaderOverlay(const LoaderOverlay&) = delete;
    LoaderOverlay& operator=(const LoaderOverlay&) = delete;

private:
    rr::sim::GuestRam& g_;
    std::vector<uint8_t> saved_;
    bool ok_ = false, laid_ = false;
};

// The product's callees. `site` is asked first (handled = true when it answered); then the host's blocks and
// reads and the ported leaves; anything else is DEFERRED (v0 = 0, counted and named).
class ProductLoaderCallees final : public rr::sim::LoaderCallees {
public:
    using Site = std::function<bool(uint32_t fn, const uint32_t* a, int n, uint32_t sp, uint32_t& v0, bool& handled)>;
    ProductLoaderCallees(rr::sim::GuestRam& g, uint8_t* ram, const DiscImage* disc, uint32_t* from, uint32_t limit,
                         Site site = {})
        : g_(g), ram_(ram), disc_(disc), from_(from), limit_(limit), site_(std::move(site)) {}
    bool Call(uint32_t fn, const uint32_t* a, int n, uint32_t sp, uint32_t& v0) override;
    // A malloc of the host's (SLUS 0x8001447C's user-pointer rule: size (n + 11) & ~7, user = block + 4).
    uint32_t Block(uint32_t n);
    // libgs OpenTIM + ReadTIM (SLUS 0x80014044) into the 20-byte record at `out`: the host's (the caller's frame)
    bool HostReadTim(uint32_t out, uint32_t tim);
    std::string error;
    uint32_t lastFile = 0; // the block of the last CD read the defaults served

private:
    rr::sim::GuestRam& g_;
    uint8_t* ram_;
    const DiscImage* disc_;
    uint32_t* from_;
    uint32_t limit_;
    uint32_t lastBlock_ = 0; // the top block of the bump region (a free of it gives it back)
    Site site_;
};

std::string LoaderFnName(uint32_t fn);

// ---- BuildRace (sim\loader2.h, rows_loader2.inc): BuildRace RASHCDI 0x8006982C run as the
// grid's orchestrator (grid_session.cpp), its children PORTED or answered at the session's own points; the rest of
// SetUpRace's children in its site (race_modes.cpp); AnimNoise, FightLoad, WorldPoolsInit / PoolTableInit where the
// session builds those arenas. RRJB_LOADER2=off is the negative control (the transcriptions stand).
bool Loader2On();
struct Loader2Counts {
    size_t buildRace = 0, worldPools = 0, poolTable = 0, trialLimits = 0, popBlock = 0, pedTables = 0, partSlots = 0,
           census = 0, speedClass = 0, escapeLoad = 0, animNoise = 0, fightLoad = 0, sirenSet = 0, fxGlobals = 0,
           fxReset = 0;
    int32_t siren = -2;          // the index SirenSet 0x80018DC8 left at gp+1908 (-2: not run)
    uint32_t speedClasses[3] = {0, 0, 0};
    uint32_t jailRecords[2] = {0, 0};
    uint32_t noiseRecords = 0;   // ANIMNOIZ.DAT's records (gp+1916 .. the event table)
    std::vector<std::string> answered; // the children answered at the session's own points (each once)
    bool buildRaceRan = false;   // this race's BuildRace ran PORTED (the session then leaves its census, STARTJBA, parts)
    uint32_t heapNext = 0, heapEnd = 0; // BuildRace's mallocs at the grid (AnimNoise's file, PartSlotsInit, EscapeLoad):
                                        // OURS, a reserve of the session's bump region made before the cell buffers
    bool gridPlanned = false;    // the session will run the ported grid (BuildRace): the arenas before it leave their
                                 // part arrays / pedestrian tables to PartSlotsInit / PedTablesInit
    std::string refused;
};
Loader2Counts& Loader2Totals();
void Loader2Answered(const std::string& what);
std::string Loader2Line();

// ---- the session's hooks (each returns the race log's line)
// EnterRace RASHCDI 0x80063B90 (RaceReset 0x80063500 run PORTED under it, their loaders DEFERRED to the session's own
// points) and the finish table 0x8006A7C0.
std::string LoaderEnterRace(rr::sim::GuestRam& g, uint8_t* ram, const DiscImage& disc, uint32_t sp, uint32_t from,
                            uint32_t limit);
// ModelTablesInit RASHCDI 0x8005BE40 (RaceReset's call, run where the session builds the model arenas).
bool LoaderModelTables(rr::sim::GuestRam& g, uint8_t* ram, const DiscImage& disc, uint32_t sp, std::string& error);
// GeoLoad RASHCDI 0x8005CA10 on `file` (already read), placed at the bump region.
bool LoaderGeo(rr::sim::GuestRam& g, uint8_t* ram, const DiscImage* disc, const std::vector<uint8_t>& file, uint32_t& from,
               uint32_t limit, uint32_t sp, std::vector<uint32_t>& models, uint32_t* fileAt, std::string& error);
// CarModels RASHCDI 0x8005C630(raceId): the pedestrians' and the race's cars .GEO off the disc.
bool LoaderCarModels(rr::sim::GuestRam& g, uint8_t* ram, const DiscImage& disc, int raceId, uint32_t& from, uint32_t limit,
                     uint32_t sp, std::vector<uint32_t>& models, std::string& files, uint32_t* fileAt, std::string& error);
// RaceReset's ModelTablesInit, then LoadBikeBank RASHCDI 0x8005C45C(bank): BBLEVEL<n>.GEO / .TEX (and a player's rig
// .MRO) through the PORTED GeoLoad / TexFile / RigLoad, before the cars, as the loader orders them.
std::string LoaderBikeBank(rr::sim::GuestRam& g, const DiscImage& disc, int bank, uint32_t& from, uint32_t limit);
// TexFile RASHCDI 0x8005C920 on a level-bundle section (types 8 / 9, 0x8006270C / 0x8006275C), placed transiently at
// `at`: its CTKP lists PORTED, LECT / KNBP / TSLP the renderer's (VRAM). Returns false when it refused.
bool LoaderTexSection(rr::sim::GuestRam& g, const DiscImage& disc, const uint8_t* bytes, uint32_t n, uint32_t at,
                      int& ctkp, int& skipped, std::string& error);
// HazardModels RASHCDI 0x8005C7F0(set) (SetUpRace's call): DATA\HAZARD<n>.GEO / .TEX off the disc through the PORTED
// GeoLoad / TexFile; `fileAt` the .GEO's block, `models` the ids it registered.
bool LoaderHazardModels(rr::sim::GuestRam& g, const DiscImage& disc, int set, uint32_t& from, uint32_t limit,
                        std::vector<uint32_t>& models, uint32_t* fileAt, std::string& error);
// PopulationReset RASHCDI 0x80068D54.
bool LoaderPopReset(rr::sim::GuestRam& g, uint8_t* ram, const DiscImage& disc, uint32_t sp, std::string& error);
// RaceBlockCopy RASHCDI 0x80068470 on the race's grid block (292 bytes, laid at `blockAt`): the hazard class table
// 0x8005B328 and the event count / templates (read back out of the overlay's data before it goes).
bool LoaderBlockCopy(rr::sim::GuestRam& g, uint8_t* ram, const DiscImage& disc, const uint8_t* block, uint32_t blockAt,
                     uint32_t sp, int32_t& eventCount, uint8_t (&events)[48], std::string& error);
// SetUpRace RASHCDI 0x80063670 with the session's `site` (its children the session runs itself).
std::string LoaderSetUpRace(rr::sim::GuestRam& g, uint8_t* ram, const DiscImage* disc, uint32_t sp,
                            const ProductLoaderCallees::Site& site);
// The stream set-up: PieceListInit 0x8006A8FC and ResTableInit 0x8005D410 (its 0x8005D338 block: `buffers`; 0 skips) and
// SkyInit 0x800609B0 (its malloc: `sky`; 0 skips).
bool LoaderStreamSetUp(rr::sim::GuestRam& g, uint8_t* ram, const DiscImage* disc, uint32_t buffers, uint32_t sky,
                       uint32_t sp, std::string& error);
// EffectSheet RASHCDI 0x80061FAC on the level bundle's type-5 section, placed (transiently) at `at`; each
// LoadImage goes to `upload(x, y, w, h, pixels)`.
using LoaderUpload = std::function<void(int x, int y, int w, int h, uint32_t pixels)>;
bool LoaderEffectSheet(rr::sim::GuestRam& g, uint8_t* ram, const DiscImage& disc, const std::vector<uint8_t>& section,
                       uint32_t at, uint32_t sp, const LoaderUpload& upload, std::string& error);

} // namespace rr::game
