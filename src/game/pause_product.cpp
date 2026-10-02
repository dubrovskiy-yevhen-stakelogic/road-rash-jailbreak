// The in-race pause in the product - see pause_product.h.
#include "game/pause_product.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#include "game/race_session.h"
#include "game/hud_view.h"
#include "game/sim/passes.h"
#include "game/sim/pause.h"
#include "game/sim/race.h"
#include "game/strike_product.h"

namespace rr::game {

namespace {
constexpr uint32_t kGp = 0x8005AC8C;
constexpr uint32_t kGsPtr = 0x8005B2F8;
constexpr uint32_t kFrameOt = 0x800D9C80;        // OURS: every capture's *(0x8005B5AC)
constexpr uint32_t kFinishOrder = 0x800D5D98, kFinishOrderBytes = 16u * 20u; // race_session.cpp's
constexpr uint32_t kPauseSp = 0x801FF000;         // the session's frame stack (race_session.cpp kArenaStepSp)
} // namespace

bool PauseOn() {
    static const bool on = [] {
        const char* v = std::getenv("RRJB_PAUSE");
        return !(v != nullptr && std::strcmp(v, "off") == 0);
    }();
    return on;
}

PauseCounts& PauseCounters() {
    static PauseCounts c;
    return c;
}

std::string PauseTotals() {
    const PauseCounts& c = PauseCounters();
    char b[640];
    std::snprintf(b, sizeof(b),
                  "the pause port%s: the pause test 0x8001CBEC paused %llu time(s), %llu frame(s) in "
                  "state 3 / 4, PauseMenu 0x8002D2F4 %llu call(s) (%llu refused), %llu menu packet(s) walked; resumed %llu, "
                  "quit %llu, restarted %llu; the stall switch in %llu / out %llu; SoundHold 0x80020E30 %llu (%llu refused), "
                  "RaceOverSignal %llu; results-state presses %llu\n",
                  PauseOn() ? "" : " OFF (RRJB_PAUSE=off)", static_cast<unsigned long long>(c.pauses),
                  static_cast<unsigned long long>(c.pausedFrames), static_cast<unsigned long long>(c.menuFrames),
                  static_cast<unsigned long long>(c.menuRefused), static_cast<unsigned long long>(c.drawnPackets),
                  static_cast<unsigned long long>(c.resumes), static_cast<unsigned long long>(c.quits),
                  static_cast<unsigned long long>(c.restarts), static_cast<unsigned long long>(c.stallIn),
                  static_cast<unsigned long long>(c.stallOut), static_cast<unsigned long long>(c.soundHolds),
                  static_cast<unsigned long long>(c.soundHoldRefused), static_cast<unsigned long long>(c.overSignals),
                  static_cast<unsigned long long>(c.resultsKeys));
    return b;
}

// The menu's callees on the session: the PORTED PlaySound3D, RaceOverSignal, SoundHold (on the sound
// runtime), ResultsPrepare with RecordFinish over the session's bikes; the heap manager 0x80021C98 and
// 0x800662CC are refused (named).
class ProductPauseCallees final : public rr::sim::PauseCallees {
public:
    explicit ProductPauseCallees(RaceSession* s) : s_(s) {}
    bool PlaySound(uint32_t, int32_t id) override {
        s_->sounds_.PlaySound3D(0, 0, id, 0); // SLUS 0x80017BA0, PORTED
        return true;
    }
    bool StopCountdownVoice(uint32_t) override { return true; }
    bool ComputePlace(uint32_t, uint32_t, int32_t, int32_t*) override { return false; } // not reached by the menu
    bool HeapOverflow(uint32_t, uint32_t, uint32_t, uint32_t*) override {
        s_->NoteSeam("pause: the packet heap filled (SLUS 0x80021C98, the heap manager, is not ported) - the menu "
                     "frame was refused");
        return false;
    }
    bool Unported(uint32_t, uint32_t address, const uint32_t*, int) override {
        char b[120];
        std::snprintf(b, sizeof(b), "pause: 0x%08X is not ported (the front end's word wrap) - refused", address);
        s_->NoteSeam(b);
        return false;
    }
    bool SoundHold(uint32_t, uint32_t v) override { // SLUS 0x80020E30, PORTED (pause.h)
        ++PauseCounters().soundHolds;
        if (!s_->sounds_.SoundHold(v)) {
            ++PauseCounters().soundHoldRefused;
            s_->NoteSeam("pause: SLUS 0x80020E30 SoundHold (PORTED) was not run: no sound world attached, or it faulted");
        }
        return true;
    }
    bool RaceOverSignal(uint32_t, uint32_t v) override { // SLUS 0x80018C1C, PORTED (race_over.h)
        ++PauseCounters().overSignals;
        ProductRaceOverSignal(s_->sounds_, v, [this](const std::string& w) { s_->NoteSeam(w); },
                              "SLUS 0x80018C1C the pause's audio pause / resume is not run (RRJB_STRIKE=off)");
        return true;
    }
    bool ResultsPrepare(uint32_t) override { // SLUS 0x8003F708, PORTED (passes.h); RecordFinish PORTED (race.h)
        struct Rec final : rr::sim::ResultsCallees {
            RaceSession* s;
            explicit Rec(RaceSession* ss) : s(ss) {}
            bool RecordFinish(uint32_t handle, int32_t flag) override {
                if (handle >= s->bikes_.size()) return false;
                rr::sim::FinishOrderEnv fo;
                fo.entity = s->bikes_[handle].entity.data();
                fo.riderDef = s->bikes_[handle].riderDef.data();
                fo.table = s->At(kFinishOrder);
                fo.tableBytes = static_cast<int32_t>(kFinishOrderBytes);
                return rr::sim::RecordFinish(flag, fo);
            }
        } rec(s_);
        rr::sim::GuestRam g(s_->arena_.Ram(), kGp);
        if (!rr::sim::ResultsPrepare(g, rec) || g.Faulted()) {
            g.ClearFault();
            s_->NoteSeam("pause: SLUS 0x8003F708 ResultsPrepare (PORTED) refused on the quit");
        }
        return true;
    }

private:
    RaceSession* s_;
};

uint32_t RaceSession::PausePoll() {
    if (!PauseOn()) return 0;
    rr::sim::GuestRam g(arena_.Ram(), kGp);
    const uint32_t gs = g.U32(kGsPtr);
    const uint8_t before = g.U8(gs);
    const uint32_t key = g.U32(rr::sim::kPauseResultsKey);
    const uint32_t s0 = rr::sim::PadPauseTest(g); // PORTED region 0x8001CBEC..0x8001CD00
    if (g.Faulted()) {
        g.ClearFault();
        NoteSeam("pause: the PORTED pause test met an address the console faults on");
        return 0;
    }
    if (before == 1 && g.U8(gs) == 3) ++PauseCounters().pauses;
    if (key == 0 && g.U32(rr::sim::kPauseResultsKey) != 0) ++PauseCounters().resultsKeys;
    return s0;
}

void RaceSession::PauseStall() {
    if (!PauseOn()) return;
    rr::sim::GuestRam g(arena_.Ram(), kGp);
    const uint32_t gs = g.U32(kGsPtr);
    const uint8_t before = g.U8(gs);
    ProductPauseCallees c(this);
    if (!rr::sim::GameFrameStall(g, c, kPauseSp) || g.Faulted()) { // PORTED region 0x80011C70..0x80011D88
        g.ClearFault();
        NoteSeam("pause: GameFrame's stall switch (PORTED) refused a frame");
    }
    const uint8_t after = g.U8(gs);
    if (before != 4 && after == 4) ++PauseCounters().stallIn;
    if (before == 4 && after != 4) ++PauseCounters().stallOut;
}

void RaceSession::PauseMenuFrame() {
    if (!PauseOn()) return;
    rr::sim::GuestRam g(arena_.Ram(), kGp);
    if (g.U32(rr::sim::kPauseFrameOtPtr) == 0) g.W32(rr::sim::kPauseFrameOtPtr, kFrameOt);
    const uint32_t ot = g.U32(rr::sim::kPauseFrameOtPtr);
    for (uint32_t k = 2; k <= 6; ++k) g.W32(ot + 4u * k, 0x00FFFFFFu); // OURS: the menu's slots, each a list
    const uint32_t gs = g.U32(kGsPtr);
    const uint8_t st = g.U8(gs);
    if (static_cast<uint8_t>(st - 3u) >= 2u) return;                       // GameFrame 0x80011EB0: 3 or 4 only
    ++PauseCounters().pausedFrames;
    if (!hudReady_) {
        NoteSeam("pause: the menu is not drawn - the HUD arena (its font, strings and packet heap) was not built");
        return;
    }
    ++PauseCounters().menuFrames;
    ProductPauseCallees c(this);
    if (!rr::sim::PauseMenu(g, c, kPauseSp) || g.Faulted()) {             // PORTED, SLUS 0x8002D2F4
        g.ClearFault();
        ++PauseCounters().menuRefused;
        NoteSeam("pause: PauseMenu 0x8002D2F4 (PORTED) refused a frame");
    }
    const uint8_t after = g.U8(gs);
    if (st == 3 && after == 1) ++PauseCounters().resumes;
    if (after == 6) ++PauseCounters().quits;
    if (after == 5) ++PauseCounters().restarts;
    for (const uint32_t h : PauseListHeads()) PauseCounters().drawnPackets += WalkHudList(arena_.Ram(), h).size();
}

void PauseKeys::Apply(PadState& pad, const PauseKeyInput& in, bool paused) {
    if (!PauseOn()) return;
    if (!in.enter) enterRole_ = 0;
    else if (enterRole_ == 0) enterRole_ = paused ? 2 : 1;
    if (!in.escape) escRole_ = 0;
    else if (escRole_ == 0) escRole_ = paused ? 2 : 1;
    const bool start = enterRole_ == 1 || escRole_ == 1 || in.p || in.padStart;
    if (!paused) {
        pad.start = pad.start || start;
        return;
    }
    const PadDevice device = pad.device;
    pad = PadState{};
    pad.device = device;
    pad.start = start;
    pad.padUp = in.up || in.padUp;
    pad.padDown = in.down || in.padDown;
    pad.left = in.left || in.padLeft;
    pad.right = in.right || in.padRight;
    pad.throttle = enterRole_ == 2 || in.space || in.padCross; // Cross
    pad.triangle = escRole_ == 2 || in.back || in.padTriangle;
}

void PauseScript::Parse(const std::string& s) {
    for (size_t at = 0; at < s.size();) {
        const size_t semi = s.find(';', at);
        const std::string e = s.substr(at, semi == std::string::npos ? std::string::npos : semi - at);
        const size_t colon = e.find(':');
        if (colon != std::string::npos) events_.push_back({std::atol(e.substr(0, colon).c_str()), e.substr(colon + 1)});
        if (semi == std::string::npos) break;
        at = semi + 1;
    }
}
void PauseScript::At(long frame, const char* key) { events_.push_back({frame, key}); }
bool PauseScript::Has(long frame, const char* key) const {
    if (RaceAttempt() != 0) return false;
    for (const auto& e : events_)
        if (e.first == frame && ("," + e.second + ",").find(std::string(",") + key + ",") != std::string::npos) return true;
    return false;
}
int& RaceAttempt() {
    static int n = 0;
    return n;
}

namespace {
// GP0 words per command, as the GPU splits a packet (hud_view.cpp's CheckHudAgainstTrace).
uint32_t CommandWords(uint32_t w0) {
    const uint32_t cmd = w0 >> 24;
    if (cmd >= 0x20 && cmd < 0x40) {
        const bool gouraud = cmd & 0x10u, quad = cmd & 8u, tex = cmd & 4u;
        const uint32_t n = quad ? 4u : 3u;
        return 1u + n * (tex ? 2u : 1u) + (gouraud ? n - 1u : 0u);
    }
    if (cmd >= 0x40 && cmd < 0x60) return (cmd & 0x10u) ? 4u : 3u;
    if (cmd >= 0x60 && cmd < 0x80) return 2u + ((cmd & 4u) ? 1u : 0u) + (((cmd >> 3) & 3u) == 0 ? 1u : 0u);
    return 1u;
}
} // namespace

std::string PausePacketCheck(const RaceSession& s, const std::string& primsCsv, bool mutate, bool* pass) {
    *pass = false;
    std::vector<std::vector<uint32_t>> ours;
    // One command per list element, as the renderer draws it: the product's packet heap is KSEG0 (0x80......,
    // shadow_product.h), so HudDrawString's `*ot | 0x09000000` gives a glyph after the first a tag length of
    // 0x89 where the original's physical heap pointers (rr-race +0x10C = 0x000E26E8) give 9.
    for (const uint32_t h : s.PauseListHeads())
        for (const HudPacket& p : WalkHudList(s.ArenaRam(), h))
            if (!p.words.empty()) {
                const size_t n = std::min<size_t>(CommandWords(p.words[0]), p.words.size());
                ours.emplace_back(p.words.begin(), p.words.begin() + static_cast<std::ptrdiff_t>(n));
            }
    if (mutate && ours.size() > 6) ours[6][0] ^= 0x000100u; // the first glyph's colour, one bit
    // The original's: the rows of the last transfer (seq) that holds the box packet 0x800540E8.
    std::vector<std::vector<std::string>> rows;
    if (FILE* f = std::fopen(primsCsv.c_str(), "rb")) {
        std::string line;
        int c;
        while ((c = std::fgetc(f)) != EOF || !line.empty()) {
            if (c == '\n' || c == EOF) {
                std::vector<std::string> r;
                size_t at = 0;
                for (;;) {
                    const size_t comma = line.find(',', at);
                    r.push_back(line.substr(at, comma == std::string::npos ? std::string::npos : comma - at));
                    if (comma == std::string::npos) break;
                    at = comma + 1;
                }
                rows.push_back(std::move(r));
                line.clear();
                if (c == EOF) break;
            } else if (c != '\r') {
                line.push_back(static_cast<char>(c));
            }
        }
        std::fclose(f);
    }
    size_t start = rows.size();
    for (size_t i = 1; i < rows.size(); ++i)
        if (rows[i].size() > 4 && std::strtoul(rows[i][4].c_str(), nullptr, 0) == 0x800540E8u) start = i;
    std::vector<std::vector<uint32_t>> original;
    for (size_t i = start; i < rows.size() && rows[i].size() > 9; ++i) {
        if (rows[i][1] != rows[start][1]) break;
        const uint32_t n = static_cast<uint32_t>(std::strtoul(rows[i][3].c_str(), nullptr, 0));
        std::vector<uint32_t> w;
        for (uint32_t k = 0; k < n && 9u + k < rows[i].size(); ++k)
            w.push_back(static_cast<uint32_t>(std::strtoul(rows[i][9 + k].c_str(), nullptr, 0)));
        if (w.empty() || (w[0] >> 24) == 0x80u) break; // the frame's VRAM copy follows the menu
        original.push_back(std::move(w));
    }
    size_t same = 0, differ = 0;
    std::string details;
    const size_t common = std::min(ours.size(), original.size());
    for (size_t k = 0; k < common; ++k) {
        if (ours[k] == original[k]) {
            ++same;
            continue;
        }
        if (++differ <= 6) {
            char b[160];
            std::snprintf(b, sizeof(b), "    DIFFER #%zu: original w0 0x%08X w1 0x%08X / ours w0 0x%08X w1 0x%08X\n", k,
                          original[k][0], original[k].size() > 1 ? original[k][1] : 0u, ours[k][0],
                          ours[k].size() > 1 ? ours[k][1] : 0u);
            details += b;
        }
    }
    *pass = !original.empty() && differ == 0 && ours.size() == original.size();
    char b[512];
    std::snprintf(b, sizeof(b),
                  "pause check%s: the menu's GP0 commands - original (%s, transfer %s) %zu, ours %zu: %zu identical, %zu "
                  "differ, %zu only in the original, %zu only in ours -> %s\n",
                  mutate ? " [NEGATIVE CONTROL: one colour bit flipped]" : "", primsCsv.c_str(),
                  start < rows.size() ? rows[start][1].c_str() : "none", original.size(), ours.size(), same, differ,
                  original.size() - common, ours.size() - common, *pass ? "PASS" : "FAIL");
    return b + details;
}

std::vector<uint32_t> RaceSession::PauseListHeads() const {
    if (!PauseOn()) return {};
    const uint32_t ot = ArenaWord(rr::sim::kPauseFrameOtPtr);
    if (ot < 0x80000000u || ot >= 0x80200000u) return {};
    std::vector<uint32_t> heads;
    for (uint32_t k = 6; k >= 2; --k) heads.push_back(ArenaWord(ot + 4u * k)); // the reverse table: 6 first
    return heads;
}

} // namespace rr::game
