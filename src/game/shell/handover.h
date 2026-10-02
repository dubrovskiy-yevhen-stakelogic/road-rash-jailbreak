#pragma once
// What the front end hands the race and takes back. Header-only on
// purpose: the race side (race_session.cpp, tools\rrgame) reads it without linking the shell.
//
// On the console the shell and the race share one RAM, so the handover is simply that memory: the
// shell's commit RASHCDF 0x8007F37C writes game_state, and the session record 0x800D80D8 with the six
// player records 0x800D81D8 stay where they are while the race runs. The product has one arena per
// side, so these bytes are copied across at the two doors - exactly the regions named below, nothing
// else.
#include <cstdint>
#include <string>
#include <vector>

namespace rr::shell {

struct Handover {
    bool active = false;             // a race started from the front end (rrgame without --race)
    // game_state 0x800D5D38 after the commit, bytes 0x00..0x67.
    uint8_t gameState[0x68] = {};
    // session 0x800D80D8 .. player records end 0x800D82F7 (0x220 bytes), both directions.
    uint8_t sessionAndPlayers[0x220] = {};
    // The player's bike .PH (the resource-name array RASHCDF 0x8008973C entry 21 + bike index,
    // rules.md 1.3), e.g. "DATA/CRUISEA1.PH".
    std::string playerBikePh;
    std::string player2BikePh;       // the same for player 2 (game_state+0x4C), a two-player race only
    int set = 1;                     // the road set: game_state+0x30 (player count)
    int raceId = 0;                  // game_state+0x40

    // Filled on the way back by the race (tools\rrgame): the arena's session and player records after
    // the race, game_state+0x00 as the race left it, and the player's rider record +0x27 / +0x28.
    bool returned = false;
    uint8_t gameStateByte = 0;
    uint8_t resultCode = 0;          // riderDef[0]+0x27: a place 1..n or a result code >= 248 (rules.md 7.1)
    uint32_t resultTicks = 0;        // riderDef[0]+0x28: the finish/elimination stamp in ticks
    // The twenty 0x48-byte rider records 0x800D5758, both directions: the commit writes the career
    // identities (+0x2C..+0x33) into them and the race's loader keeps those it does not reset
    // (grid_loader.h); on the way back the career dispatcher RASHCDF 0x8007BB34 copies the player's identity
    // from rider 0 and the opponents' into session+0x40.
    uint8_t riders[20 * 0x48] = {};
    bool quit = false;               // the window was closed during the race
    // The whole race arena as the race left it (2 MiB), for the race's own results scene
    // (race_results.h), which the front end runs before its post-race dispatch.
    std::vector<uint8_t> raceRam;
    // SLUS_010.53's data is resident under both overlays, so what the shell writes there the race reads and
    // the reverse (the product has two arenas - OURS: these two regions are carried both ways, named):
    //   * the album table's flag words 0x80053588 + 12 t, t < 18 (bit 0: the jukebox's "play this track",
    //     JukeboxSet SLUS 0x8002490C from screen 47 / LoadSlot; bit 1: "played this shuffle cycle",
    //     MusicPickShuffle SLUS 0x80024B20 in the race);
    //   * the Time Trial records 0x80053A88, 176 bytes for each of the races 56..64 (the new-record keyboard
    //     0x80069BF0, the card's RestoreRecords 0x8006D10C; the race's HUD, split times and results read it).
    bool resident = false;
    uint32_t albumFlags[18] = {};
    uint8_t records[176 * 9] = {};
    //   * the seven volume sliders 0x800D6C00 and their saved copy 0x800D6C20 (the options' VolumeSet RASHCDF
    //     0x8007F20C, and SessionInit / the card's load through it, write both; the race's sound reads them -
    //     EngineNote [0], RoadNote [3], the music [5] - and RaceOverSignal SLUS 0x80018C1C saves / restores).
    bool sliders = false;
    uint32_t volume[7] = {}, volumeSaved[7] = {};
};

constexpr uint32_t kResidentAlbumFlags = 0x80053588u, kResidentAlbumStride = 12u, kResidentAlbumTracks = 18u;
constexpr uint32_t kResidentRecords = 0x80053A88u, kResidentRecordBytes = 176u * 9u;
constexpr uint32_t kResidentSliders = 0x800D6C00u, kResidentSlidersSaved = 0x800D6C20u, kResidentSliderCount = 7u;

// The resident regions out of / into a 2 MiB guest image (either arena).
inline void ReadResident(Handover& h, const uint8_t* ram) {
    const uint32_t mask = 0x1FFFFFu;
    for (uint32_t t = 0; t < kResidentAlbumTracks; ++t) {
        const uint32_t o = (kResidentAlbumFlags + kResidentAlbumStride * t) & mask;
        h.albumFlags[t] = static_cast<uint32_t>(ram[o]) | (static_cast<uint32_t>(ram[o + 1]) << 8) |
                          (static_cast<uint32_t>(ram[o + 2]) << 16) | (static_cast<uint32_t>(ram[o + 3]) << 24);
    }
    for (uint32_t i = 0; i < kResidentRecordBytes; ++i) h.records[i] = ram[(kResidentRecords + i) & mask];
    h.resident = true;
    auto word = [&](uint32_t a) {
        const uint32_t o = a & mask;
        return static_cast<uint32_t>(ram[o]) | (static_cast<uint32_t>(ram[o + 1]) << 8) |
               (static_cast<uint32_t>(ram[o + 2]) << 16) | (static_cast<uint32_t>(ram[o + 3]) << 24);
    };
    for (uint32_t k = 0; k < kResidentSliderCount; ++k) {
        h.volume[k] = word(kResidentSliders + 4u * k);
        h.volumeSaved[k] = word(kResidentSlidersSaved + 4u * k);
    }
    h.sliders = true;
}
inline void WriteResident(const Handover& h, uint8_t* ram) {
    if (!h.resident) return;
    const uint32_t mask = 0x1FFFFFu;
    for (uint32_t t = 0; t < kResidentAlbumTracks; ++t) {
        const uint32_t o = (kResidentAlbumFlags + kResidentAlbumStride * t) & mask;
        for (uint32_t k = 0; k < 4; ++k) ram[o + k] = static_cast<uint8_t>(h.albumFlags[t] >> (8 * k));
    }
    for (uint32_t i = 0; i < kResidentRecordBytes; ++i) ram[(kResidentRecords + i) & mask] = h.records[i];
    for (uint32_t k = 0; h.sliders && k < kResidentSliderCount; ++k)
        for (uint32_t b = 0; b < 4; ++b) {
            ram[(kResidentSliders + 4u * k + b) & mask] = static_cast<uint8_t>(h.volume[k] >> (8 * b));
            ram[(kResidentSlidersSaved + 4u * k + b) & mask] = static_cast<uint8_t>(h.volumeSaved[k] >> (8 * b));
        }
}

inline Handover& PendingHandover() {
    static Handover h;
    return h;
}

// The race loop's door back to the front end (tools\rrgame\main.cpp): only for a race the front end
// started - it ends when the race does (RaceDirector writes game_state+0x00 = 6 or 2) or on Esc, the original's pause-and-quit (which forces result code 255, rules.md 7.1).
inline bool RaceLeaves(bool raceOver, bool escape) {
    Handover& h = PendingHandover();
    if (!h.active) return false;
    if (escape && !raceOver) {
        h.quit = true;
        return true;
    }
    return raceOver;
}

// After the race loop: the race arena's session and player records, game_state+0x00 and the
// player's rider record +0x27 (place or result code) / +0x28 (stamp) for the front end.
inline void NoteRaceReturn(const uint8_t* arenaRam, uint32_t playerRiderDef, uint8_t gameStateByte) {
    Handover& h = PendingHandover();
    if (!h.active || arenaRam == nullptr) return;
    const uint32_t mask = 0x1FFFFFu;
    for (uint32_t i = 0; i < sizeof(h.sessionAndPlayers); ++i) h.sessionAndPlayers[i] = arenaRam[(0x800D80D8u + i) & mask];
    for (uint32_t i = 0; i < sizeof(h.riders); ++i) h.riders[i] = arenaRam[(0x800D5758u + i) & mask];
    h.gameStateByte = gameStateByte;
    h.resultCode = h.quit ? 255 : arenaRam[(playerRiderDef + 0x27u) & mask];
    const uint32_t o = (playerRiderDef + 0x28u) & mask;
    h.resultTicks = static_cast<uint32_t>(arenaRam[o]) | (static_cast<uint32_t>(arenaRam[o + 1]) << 8) |
                    (static_cast<uint32_t>(arenaRam[o + 2]) << 16) | (static_cast<uint32_t>(arenaRam[o + 3]) << 24);
    h.raceRam.assign(arenaRam, arenaRam + 0x200000u);
    ReadResident(h, arenaRam); // the album's played bits and the records, back to the shell
    h.returned = true;
}

} // namespace rr::shell
