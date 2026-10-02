// The ported route loaders in the product (route_product.h).
#include "game/route_product.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "game/loader_product.h"
#include "game/mp_arena.h" // the two-player stat array below pool 0
#include "game/sim/loader2.h"
#include "game/sim/stream.h"
#include "game/sim/stream_cd.h"

namespace rr::game {

namespace s = rr::sim;
using s::GuestRam;

namespace {

RouteCounts g_route;

constexpr uint32_t kHeapFrom = 0x801A7BC8u; // rr-race: the block of STREAM1.GRF (user 0x801A7BCC), then the route
constexpr uint32_t kHeapTo = 0x801B5ECCu;   // block and ROAD1.MAP; up to the session's stat array (race_session.cpp)
constexpr uint32_t kRoadName = 0x80052400u; // SLUS: "DATA\ROAD" (SetUpRace's argument of RoadLoad)

std::string Str(GuestRam& g, uint32_t a, size_t max = 256) {
    std::string out;
    for (size_t k = 0; k < max && a != 0u; ++k) {
        const uint8_t ch = g.U8(a + static_cast<uint32_t>(k));
        if (ch == 0 || g.Faulted()) break;
        out.push_back(static_cast<char>(ch));
    }
    return out;
}

// SLUS 0x80043FD4 sprintf, the host's: the conversions the loaders use (%s, %d with a width, %%).
bool Sprintf(GuestRam& g, const uint32_t* a, int n, uint32_t& v0, std::string& error) {
    const std::string fmt = Str(g, a[1]);
    std::string out;
    int arg = 2;
    for (size_t k = 0; k < fmt.size(); ++k) {
        if (fmt[k] != '%') {
            out.push_back(fmt[k]);
            continue;
        }
        size_t width = 0;
        while (k + 1 < fmt.size() && fmt[k + 1] >= '0' && fmt[k + 1] <= '9') width = width * 10 + static_cast<size_t>(fmt[++k] - '0');
        const char c = k + 1 < fmt.size() ? fmt[++k] : '\0';
        std::string piece;
        if (c == '%') {
            piece = "%";
        } else if (c == 's' && arg < n) {
            piece = Str(g, a[arg++]);
        } else if (c == 'd' && arg < n) {
            piece = std::to_string(static_cast<int32_t>(a[arg++]));
        } else {
            error = "sprintf format '" + fmt + "' has a conversion the host does not serve";
            return false;
        }
        while (piece.size() < width) piece.insert(piece.begin(), ' ');
        out += piece;
    }
    for (size_t k = 0; k <= out.size(); ++k)
        g.W8(a[0] + static_cast<uint32_t>(k), k < out.size() ? static_cast<uint8_t>(out[k]) : 0u);
    v0 = static_cast<uint32_t>(out.size());
    return !g.Faulted();
}

// A callee set answered by one function (the stream set-up / start sites).
struct FnCallees final : s::LoaderCallees {
    std::function<bool(uint32_t, const uint32_t*, int, uint32_t, uint32_t&)> f;
    bool Call(uint32_t fn, const uint32_t* a, int n, uint32_t sp, uint32_t& v0) override { return f(fn, a, n, sp, v0); }
};

} // namespace

bool RoutePorted() {
    const char* v = std::getenv("RRJB_ROUTE");
    return !(v != nullptr && std::strcmp(v, "ours") == 0);
}

RouteCounts& RouteTotals() { return g_route; }

std::string RouteLine() {
    const RouteCounts& t = g_route;
    if (!RoutePorted()) return "route: RRJB_ROUTE=ours - the session's transcription of the route parser's rules and "
                               "the host's ROAD<n>.MAP loader stand (the negative control)";
    if (t.loads == 0) return "route: the ported route loaders did NOT run" + (t.refused.empty() ? std::string() : ": " + t.refused);
    char b[900];
    std::snprintf(b, sizeof(b),
                  "route: GrfLoad SLUS 0x800244E0 + RoadLoad RASHCDI 0x8006AC6C (the parser 0x8006A0C8, the map 0x8006ABC8) "
                  "PORTED: %zu ported call(s), %zu kernel string call(s) (the host's), %zu host callee(s) (%zu open(s), %zu "
                  "read(s), %zu block(s), %zu LoadFile); STREAM.GRF 0x%08X, route block 0x%08X ([RACEINTS] %d), ROAD.MAP "
                  "0x%08X (G 0x%08X), text 0x%08X + %u; against the transcription: route block %zu, header %zu, graph %zu, "
                  "map %zu of %zu bytes differ; StreamSetUp 0x80022F78 %zu, StreamStart 0x80023020 %zu (StreamPlace %zu, "
                  "0x80023714 %zu) PORTED%s%s",
                  t.ported, t.bios, t.forwarded, t.opens, t.reads, t.blocks, t.loadFiles, t.grf, t.alloc, t.raceInts, t.map,
                  t.graph, t.text, t.textLen, t.routeDiffer, t.headerDiffer, t.graphDiffer, t.mapDiffer, t.compared,
                  t.setUps, t.starts, t.places, t.perPlayer, t.note.empty() ? "" : "; OURS: ", t.note.c_str());
    return b;
}

bool RouteLoadPorted(GuestRam& g, const DiscImage& disc, int set, int32_t raceId, uint32_t sp, std::string& why) {
    RouteCounts& t = g_route;
    LoaderOverlay ov(g, disc); // RASHCDI's keys and formats (0x8005B958..0x8005BA90)
    if (!ov.ok()) {
        why = "RASHCDI.BIN is not on the disc";
        t.refused = why;
        return false;
    }
    // the race the loaders name: game_state +0x30 (the players = the road set) and +0x40 (the race id)
    const uint32_t gs = g.U32(s::kLdGameStatePtr);
    const uint32_t keep30 = g.U32(gs + 0x30u), keep40 = g.U32(gs + 0x40u);
    t.note.clear();
    if (keep30 != static_cast<uint32_t>(set)) {
        t.note += "game_state+0x30 = " + std::to_string(keep30) + " set to road set " + std::to_string(set) +
                  " for the loaders (a development start); ";
        g.W32(gs + 0x30u, static_cast<uint32_t>(set));
    }
    if (static_cast<int32_t>(keep40) != raceId) {
        t.note += "game_state+0x40 = " + std::to_string(static_cast<int32_t>(keep40)) + " set to race " +
                  std::to_string(raceId) + " for the parser's FindRace (a direct start); ";
        g.W32(gs + 0x40u, static_cast<uint32_t>(raceId));
    }
    // the host's CD file layer: handles 1.., each {bytes, position}
    struct File {
        std::vector<uint8_t> bytes;
        size_t pos = 0;
    };
    std::vector<File> files;
    auto open = [&](const std::string& name, uint32_t& v0) {
        std::string n = name;
        for (char& ch : n)
            if (ch == '\\') ch = '/';
        const auto f = disc.Find(n);
        ++t.opens;
        if (!f) {
            v0 = 0xFFFFFFFFu;
            return true;
        }
        files.push_back(File{disc.ReadFile(*f), 0});
        v0 = static_cast<uint32_t>(files.size());
        return true;
    };
    uint32_t next = kHeapFrom;
    // two players: the heap ends at the five-block stat array's block, which ends at pool 0's (mp_arena.h)
    const uint32_t heapTo = (keep30 == 2u && MpArenaOriginal()) ? StatArrayAt(2u) : kHeapTo;
    std::string hostError;
    ProductLoaderCallees* base = nullptr;
    ProductLoaderCallees host(g, nullptr, &disc, &next, heapTo,
                              [&](uint32_t fn, const uint32_t* a, int n, uint32_t, uint32_t& v0, bool& handled) {
                                  handled = true;
                                  switch (fn) {
                                  case s::kLdSprintf: return Sprintf(g, a, n, v0, hostError);
                                  case s::kL2OpenStream: // "<gp+0x1A0's prefix><name>", mode 0
                                      return open(Str(g, g.U32(g.gp() + 0x1A0u)) + Str(g, a[0]), v0);
                                  case s::kL2Open: return open(Str(g, a[0]), v0);
                                  case s::kL2Size:
                                      v0 = (a[0] >= 1u && a[0] <= files.size()) ? static_cast<uint32_t>(files[a[0] - 1u].bytes.size())
                                                                                 : 0xFFFFFFFFu;
                                      return true;
                                  case s::kL2Read: {
                                      if (!(a[0] >= 1u && a[0] <= files.size())) {
                                          v0 = 0xFFFFFFFFu;
                                          return true;
                                      }
                                      File& f = files[a[0] - 1u];
                                      const size_t want = static_cast<size_t>(static_cast<int32_t>(a[2]) < 0 ? 0 : a[2]);
                                      const size_t count = std::min(want, f.bytes.size() - f.pos);
                                      if (count) g.WriteBlock(a[1], f.bytes.data() + f.pos, static_cast<uint32_t>(count));
                                      f.pos += count;
                                      v0 = static_cast<uint32_t>(count);
                                      ++t.reads;
                                      return !g.Faulted();
                                  }
                                  case s::kL2Close: v0 = 0; return true;
                                  case s::kLdMalloc: ++t.blocks; handled = false; return true;   // the block rule (loader_product)
                                  case s::kLdLoadFile: ++t.loadFiles; handled = false; return true;
                                  case s::kLdMemset: handled = false; return true;
                                  default:
                                      hostError = "unanswered callee " + LoaderFnName(fn);
                                      return false;
                                  }
                              });
    base = &host;
    (void)base;
    s::RouteCallees rc(g, host);
    g.W32(g.gp() + s::kRpGraphGp, 0);            // GrfLoad mallocs its buffer once per boot (OURS: once per session)
    bool ok = s::GrfLoad(g, sp, rc);
    uint32_t v0 = 0;
    if (ok) ok = s::RoadLoad(g, kRoadName, sp, rc, v0);
    g.W32(gs + 0x30u, keep30);
    g.W32(gs + 0x40u, keep40);
    t.ported += rc.ported;
    t.bios += rc.bios;
    t.forwarded += rc.forwarded;
    if (!ok || g.Faulted()) {
        g.ClearFault();
        why = std::string("the ported route loaders refused") + (rc.error ? std::string(": ") + rc.error : "") +
              (hostError.empty() ? "" : ": " + hostError) + (host.error.empty() ? "" : ": " + host.error);
        t.refused = why;
        return false;
    }
    ++t.loads;
    t.heapNext = next;  // the stream's files follow (stream_files_product.h)
    t.heapTo = heapTo;
    t.grf = g.U32(g.gp() + s::kRpGraphGp);
    t.alloc = g.U32(s::kRpHeader);
    t.raceInts = g.S16(s::kRpHeader + 18u);
    t.map = g.U32(s::kRpMapFile);
    t.graph = g.U32(s::kRpMapGraph);
    t.text = g.U32(s::kRpHeader + 28u);
    t.textLen = g.U32(s::kRpHeader + 32u);
    return true;
}

bool RouteStreamSetUp(GuestRam& g, uint32_t sp, const std::function<void()>& files, std::string& why) {
    FnCallees c;
    c.f = [&](uint32_t fn, const uint32_t* a, int, uint32_t, uint32_t& v0) {
        v0 = 0;
        switch (fn) {
        case 0x80024630u: // the album (one player): the sound runtime opens and streams it (sound_runtime.cpp)
            return true;
        case 0x80023498u: // the stream's files: the session's (stream_session.cpp)
            files();
            return true;
        case 0x80022A1Cu: // the CD flush: the current request's callback cleared, the ring emptied (CdReset PORTED);
                          // its critical section and the wait 0x80022A78(a0) for the idle drive are the host's
            if (g.U32(s::kCdBusy) != 0u) g.W32(g.U32(g.gp() + s::kGpCdCur) + 0x18u, 0);
            s::CdReset(g);
            (void)a;
            return true;
        default:
            why = "StreamSetUp: unanswered callee " + LoaderFnName(fn);
            return false;
        }
    };
    const bool ok = s::StreamSetUp(g, sp, c);
    if (ok) ++g_route.setUps;
    return ok && why.empty();
}

bool RouteStreamStart(GuestRam& g, uint32_t sp, const std::function<void()>& perPlayer, std::string& why) {
    FnCallees c;
    c.f = [&](uint32_t fn, const uint32_t* a, int, uint32_t, uint32_t& v0) {
        v0 = 0;
        switch (fn) {
        case s::kStPlaceFn: // StreamPlace 0x800235C8 (PORTED)
            s::StreamPlace(g, a[0], static_cast<int32_t>(a[1]), static_cast<int32_t>(a[2]));
            ++g_route.places;
            return !g.Faulted();
        case 0x80023714u:   // each player's .STP start: the session's (stream_session.cpp)
            perPlayer();
            ++g_route.perPlayer;
            return true;
        default:
            why = "StreamStart: unanswered callee " + LoaderFnName(fn);
            return false;
        }
    };
    const bool ok = s::StreamStart(g, sp, c);
    if (ok) ++g_route.starts;
    return ok && why.empty();
}

} // namespace rr::game
