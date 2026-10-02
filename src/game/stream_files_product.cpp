// The stream's files in the product (stream_files_product.h).
#include "game/stream_files_product.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>

#include "game/route_product.h"
#include "game/sim/loader.h"   // LoaderCallees (StreamSetUp / StreamStart take it)
#include "game/sim/loader2.h"  // StreamSetUp 0x80022F78 / StreamStart 0x80023020 (PORTED, loader2)
#include "game/sim/stream.h"
#include "game/sim/stream_files.h"

namespace rr::game {

namespace s = rr::sim;
using s::GuestRam;

namespace {

StreamFilesCounts g_sf;

std::string Str(GuestRam& g, uint32_t a, size_t max = 256) {
    std::string out;
    for (size_t k = 0; k < max && a != 0u; ++k) {
        const uint8_t ch = g.U8(a + static_cast<uint32_t>(k));
        if (ch == 0 || g.Faulted()) break;
        out.push_back(static_cast<char>(ch));
    }
    return out;
}
std::string Upper(std::string v) {
    for (char& ch : v) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return v;
}

// The host's callees of the ported set-up and start; the rest forwarded.
class Host final : public s::RecoverCallees, public s::LoaderCallees {
public:
    Host(GuestRam& g, StreamFilesEnv& env) : g_(g), env_(env), next_(env.heapFrom) {}
    std::string error;
    uint32_t next() const { return next_; }

    bool Call(uint32_t fn, const uint32_t* a, int n, uint32_t sp, uint32_t& v0) override {
        v0 = 0;
        StreamFilesCounts& t = g_sf;
        switch (fn) {
        // ---- the ported children (stream_files.h)
        case s::kSfAlbumFn: ++t.albums; return s::AlbumOpen(g_, sp, *this) && Ok();
        case s::kSfFilesFn: ++t.files; return s::StreamFiles(g_, sp, *this) && Ok();
        case s::kSfCdFlushFn: ++t.flushes; return s::CdFlush(g_, a[0], sp, *this, v0) && Ok();
        case s::kStPlaceFn: // StreamPlace 0x800235C8 (PORTED, stream.h)
            s::StreamPlace(g_, a[0], static_cast<int32_t>(a[1]), static_cast<int32_t>(a[2]));
            ++t.places;
            ++RouteTotals().places;
            return !g_.Faulted();
        case s::kSfStpFn: ++t.stpPlayers; ++RouteTotals().perPlayer; return s::StpPlayers(g_, sp, *this) && Ok();
        // ---- the host's
        case s::kSfSprintf: return Sprintf(a, n, v0);
        case s::kSfOpen: return Open(Str(g_, g_.U32(g_.gp() + 0x1A0u)) + Str(g_, a[0]), v0);
        case s::kSfSize: {
            const auto it = open_.find(a[0]);
            v0 = it != open_.end() ? it->second.file.size : 0xFFFFFFFFu;
            return true;
        }
        case s::kSfMalloc: v0 = a[1] < 2u ? Block(a[0]) : 0u; return error.empty();
        case s::kSfRead: {
            const auto it = open_.find(a[0]);
            if (it == open_.end() || env_.disc == nullptr) {
                v0 = 0xFFFFFFFFu;
                return true;
            }
            File& f = it->second;
            const uint32_t want = static_cast<int32_t>(a[2]) < 0 ? 0u : a[2];
            const uint32_t count = std::min<uint32_t>(want, f.file.size - std::min(f.file.size, f.pos));
            std::vector<uint8_t> bytes(count, 0);
            if (count) env_.disc->ReadForm1(f.file.lba, f.pos, bytes.data(), count);
            if (count) g_.WriteBlock(a[1], bytes.data(), count);
            f.pos += count;
            v0 = count;
            ++t.reads;
            return !g_.Faulted();
        }
        case s::kSfClose: open_.erase(a[0]); return true;
        case s::kSfVSync:
            ++t.fields;
            if (env_.field) env_.field();
            return !g_.Faulted();
        case s::kSfMusicSetUp: ++t.musicSetUps; v0 = 0; return true; // the sound runtime's (StartMusic)
        default:
            if (env_.rest == nullptr) {
                error = "unanswered callee 0x" + Hex(fn);
                return false;
            }
            ++t.forwarded;
            return env_.rest->Call(fn, a, n, sp, v0);
        }
    }

private:
    struct File {
        rr::DiscFile file;
        uint32_t pos = 0;
    };
    GuestRam& g_;
    StreamFilesEnv& env_;
    uint32_t next_;
    std::map<uint32_t, File> open_;

    bool Ok() {
        if (!error.empty() || g_.Faulted()) return false;
        return true;
    }
    static std::string Hex(uint32_t v) {
        char b[16];
        std::snprintf(b, sizeof(b), "%08X", v);
        return b;
    }
    // SLUS 0x80043FD4, the host's: %s, %d, %ld with a width, %%.
    bool Sprintf(const uint32_t* a, int n, uint32_t& v0) {
        const std::string fmt = Str(g_, a[1]);
        std::string out;
        int arg = 2;
        for (size_t k = 0; k < fmt.size(); ++k) {
            if (fmt[k] != '%') {
                out.push_back(fmt[k]);
                continue;
            }
            size_t width = 0;
            while (k + 1 < fmt.size() && fmt[k + 1] >= '0' && fmt[k + 1] <= '9') width = width * 10 + static_cast<size_t>(fmt[++k] - '0');
            if (k + 1 < fmt.size() && fmt[k + 1] == 'l') ++k;
            const char ch = k + 1 < fmt.size() ? fmt[++k] : '\0';
            std::string piece;
            if (ch == '%') piece = "%";
            else if (ch == 's' && arg < n) piece = Str(g_, a[arg++]);
            else if (ch == 'd' && arg < n) piece = std::to_string(static_cast<int32_t>(a[arg++]));
            else {
                error = "sprintf format '" + fmt + "' has a conversion the host does not serve";
                return false;
            }
            while (piece.size() < width) piece.insert(piece.begin(), ' ');
            out += piece;
        }
        for (size_t k = 0; k <= out.size(); ++k)
            g_.W8(a[0] + static_cast<uint32_t>(k), k < out.size() ? static_cast<uint8_t>(out[k]) : 0u);
        v0 = static_cast<uint32_t>(out.size());
        return !g_.Faulted();
    }
    // The name the product opens: a development start's STREAM<n> / race<n>_<id> names the product's set / race.
    std::string Product(const std::string& asked) {
        std::string u = Upper(asked);
        for (char& ch : u)
            if (ch == '\\') ch = '/';
        const size_t slash = u.rfind('/');
        const std::string dir = slash == std::string::npos ? "" : u.substr(0, slash + 1);
        const std::string base = slash == std::string::npos ? u : u.substr(slash + 1);
        std::string want = base;
        if (base.rfind("STREAM", 0) == 0 && base.size() > 7 && std::isdigit(static_cast<unsigned char>(base[6])))
            want = "STREAM" + std::to_string(env_.set) + base.substr(7);
        else if (base.rfind("RACE", 0) == 0 && base.size() > 4 && base.find(".STP") != std::string::npos)
            want = "RACE" + std::to_string(env_.set) + "_" + std::to_string(env_.raceId) + ".STP";
        if (want != base) {
            ++g_sf.renamed;
            g_sf.names += base + " opened as " + want + " (OURS: a development start); ";
        }
        return dir + want;
    }
    bool Open(const std::string& asked, uint32_t& v0) {
        const std::string name = Product(asked);
        ++g_sf.opens;
        const auto f = env_.disc != nullptr ? env_.disc->Find(name) : std::nullopt;
        if (!f) {
            ++g_sf.opensFailed;
            g_sf.names += name + " NOT ON THE DISC; ";
            v0 = 0xFFFFFFFFu;
            return true;
        }
        const std::string ext = name.size() >= 4 ? name.substr(name.size() - 4) : "";
        if (ext == ".STP") v0 = 4u;
        else if (name.find("ALBUM2.ALB") != std::string::npos) v0 = 2u;
        else if (name.find("ALBUM.ALB") != std::string::npos) v0 = 0u;
        else v0 = 3u; // STREAM<n>.TOC / .RLS / .STR
        open_[v0] = File{*f, 0};
        return true;
    }
    // SLUS 0x8001447C's block rule: size (n + 11) & ~7, user = block + 4.
    uint32_t Block(uint32_t n) {
        if (n == 0u) return 0;
        if (next_ < 0x80008000u) {
            error = "no heap region handed over for the stream's files";
            return 0;
        }
        const uint32_t size = (n + 11u) & ~7u;
        const uint32_t at = (next_ + 7u) & ~7u;
        if (at + size > env_.heapTo) {
            error = "the stream files' heap region is full (" + std::to_string(n) + " bytes asked)";
            return 0;
        }
        next_ = at + size;
        ++g_sf.blocks;
        return at + 4u;
    }
};

} // namespace

bool StreamFilesPorted() {
    const char* v = std::getenv("RRJB_STREAM_FILES");
    return !(v != nullptr && std::strcmp(v, "session") == 0);
}

StreamFilesCounts& StreamFilesTotals() { return g_sf; }

bool StreamFilesSetUp(GuestRam& g, StreamFilesEnv& env, uint32_t sp, std::string& why) {
    g_sf.heapFrom = env.heapFrom;
    Host h(g, env);
    const bool ok = s::StreamSetUp(g, sp, h) && h.error.empty() && !g.Faulted();
    if (!ok) {
        g.ClearFault();
        why = "StreamSetUp 0x80022F78 (PORTED with its children) refused" + (h.error.empty() ? std::string() : ": " + h.error);
        g_sf.refused = why;
        return false;
    }
    ++g_sf.setUps;
    ++RouteTotals().setUps;
    g_sf.heapNext = h.next();
    g_sf.toc = g.U32(g.gp() + s::kGpStToc);
    g_sf.rls = g.U32(g.gp() + 0x1A4u);
    g_sf.rlsTable = g.U32(g.gp() + 0x88Cu);
    g_sf.str = g.U32(g.gp() + s::kGpStStr);
    if (env.disc != nullptr)
        if (const auto f = env.disc->Find("DATA/STREAM" + std::to_string(env.set) + ".RLS")) g_sf.rlsBytes = f->size;
    return true;
}

bool StreamFilesStart(GuestRam& g, StreamFilesEnv& env, uint32_t sp, std::string& why) {
    Host h(g, env);
    const bool ok = s::StreamStart(g, sp, h) && h.error.empty() && !g.Faulted();
    if (!ok) {
        g.ClearFault();
        why = "StreamStart 0x80023020 (PORTED with its children) refused" + (h.error.empty() ? std::string() : ": " + h.error);
        g_sf.refused = why;
        return false;
    }
    ++g_sf.starts;
    ++RouteTotals().starts;
    return true;
}

std::string StreamFilesLine() {
    const StreamFilesCounts& t = g_sf;
    if (!StreamFilesPorted())
        return "stream files: RRJB_STREAM_FILES=session - the session's transcriptions of 0x80023498 / 0x80023714 and the "
               "release list at the cell stream's place (the negative control)";
    char b[1200];
    std::snprintf(b, sizeof(b),
                  "stream files: StreamSetUp 0x80022F78 %zu / StreamStart 0x80023020 %zu with their children PORTED - album "
                  "0x80024630 %zu, files 0x80023498 %zu, CD flush 0x80022A1C %zu, StreamPlace %zu, .STP start 0x80023714 %zu; "
                  "host: %zu open(s) (%zu failed), %zu read(s), %zu heap block(s) from 0x%08X to 0x%08X, %zu field(s) of "
                  "the CD waits 0x80022A78, music set-up 0x80020BEC %zu (the sound runtime's); STREAM.TOC 0x%08X, release "
                  "list STREAM.RLS 0x%08X (table 0x%08X, %u bytes), .STR handle %u; %zu name(s) mapped%s%s%s%s",
                  t.setUps, t.starts, t.albums, t.files, t.flushes, t.places, t.stpPlayers, t.opens, t.opensFailed, t.reads,
                  t.blocks, t.heapFrom, t.heapNext, t.fields, t.musicSetUps, t.toc, t.rls, t.rlsTable, t.rlsBytes, t.str,
                  t.renamed, t.names.empty() ? "" : "; ", t.names.c_str(), t.refused.empty() ? "" : "; REFUSED: ",
                  t.refused.c_str());
    return b;
}

} // namespace rr::game
