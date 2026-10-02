// The GTE projection (gte_proj.h): the world's vertices on the screen by the GTE's own
// fixed-point arithmetic, and the RaceScene side of it (the static soup's screen buffer, the uniforms).
#include "render/gte_proj.h"
#include "render/edge_rule.h" // the console's fill rule

#include "game/sim/subdiv.h"
#include "render/race_scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <map>
#include <string>
#include <tuple>

namespace rr::render {

namespace {

namespace SD = rr::sim::subdiv;
using rr::sim::model::ModelGte;

// Entry points the shared loader (gl_api.cpp) does not carry.
using PfnBufferSubData = void(APIENTRY*)(GLenum, ptrdiff_t, ptrdiff_t, const void*);
using PfnVertexAttrib4f = void(APIENTRY*)(GLuint, GLfloat, GLfloat, GLfloat, GLfloat);
PfnBufferSubData g_bufferSubData = nullptr;
PfnVertexAttrib4f g_vertexAttrib4f = nullptr;
void LoadGteGl() {
    if (g_bufferSubData) return;
    const auto get = [](const char* name) {
        void* p = reinterpret_cast<void*>(wglGetProcAddress(name));
        if (p == nullptr || p == reinterpret_cast<void*>(1) || p == reinterpret_cast<void*>(2) ||
            p == reinterpret_cast<void*>(3) || p == reinterpret_cast<void*>(-1)) {
            HMODULE module = GetModuleHandleA("opengl32.dll");
            p = module ? reinterpret_cast<void*>(GetProcAddress(module, name)) : nullptr;
        }
        return p;
    };
    g_bufferSubData = reinterpret_cast<PfnBufferSubData>(get("glBufferSubData"));
    g_vertexAttrib4f = reinterpret_cast<PfnVertexAttrib4f>(get("glVertexAttrib4f"));
}

constexpr GLint kOtScreenUnit = 6; // uOtScreen (uOtVerts is on 5)
constexpr uint16_t kNoIndex = 0xFFFFu;
constexpr double kRecordsPerCellUnit = 256.0;         // the near records: cell units << 8
constexpr double kRecordsPerWorldUnit = 64.0 * 256.0; // ... and world units
constexpr double kGteRowAspect = 3412.0 / 4096.0;

int32_t ShlWrap(int32_t v, int s) { return static_cast<int32_t>(static_cast<uint32_t>(v) << s); }
int32_t AddWrap(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b)); }
int32_t SubWrap(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b)); }

// MVMVA 0x4A2012 with LLM / BK = the slot's RT / TR (0x8001064C moved them): (BK << 12 + LLM x V) >> 12 per row,
// then << 8 - the near records' x, y, z (0x8006A630 / 0x8006E474).
void ViewRecord(const ModelGte& g, int16_t x, int16_t y, int16_t z, int32_t out[3]) {
    for (int n = 0; n < 3; ++n) {
        const int64_t acc = (static_cast<int64_t>(g.tr[n]) << 12) + static_cast<int64_t>(g.rt[3 * n + 0]) * x +
                            static_cast<int64_t>(g.rt[3 * n + 1]) * y + static_cast<int64_t>(g.rt[3 * n + 2]) * z;
        out[n] = ShlWrap(static_cast<int32_t>(acc >> 12), 8);
    }
}

// SLUS 0x8001034C's byte from an SXY (disassembly 0x800103DC..0x80010420; bit 2 is `slti t6, 0` on 240 - never set).
uint8_t VertexByte(uint32_t sxy, int32_t z) {
    const int32_t sy = static_cast<int32_t>(sxy) >> 16, sx = static_cast<int16_t>(sxy & 0xFFFFu);
    uint32_t f = (z < 1024 ? 0x20u : 0u) | (z < 40 ? 0x10u : 0u) | (sy > 240 ? 8u : 0u);
    f += (static_cast<uint32_t>(sx) > 384u ? 1u : 0u) + (static_cast<uint32_t>(sx) >> 31);
    return static_cast<uint8_t>(f);
}
// The depth bit 0x8006A630 / 0x8006E474 OR into a corner's byte: 0x80 below 0xC800, 0x40 below 0x32000, else 0x20.
uint8_t CornerDepthBit(int32_t z) { return static_cast<uint8_t>(z < 0x32000 ? (z < 0xC800 ? 0x80u : 0x40u) : 0x20u); }

// A window wider than the console's 384 columns: the screen outcodes are taken with SX pulled toward the centre (ours,
// as the float path's `sideSqueeze`), the vertex keeps its own SX.
uint32_t Squeeze(uint32_t sxy, double squeeze) {
    if (squeeze <= 1.0) return sxy;
    const int32_t sx = static_cast<int16_t>(sxy & 0xFFFFu);
    const int32_t pulled = 192 + static_cast<int32_t>(std::lround((sx - 192) / squeeze));
    return (sxy & 0xFFFF0000u) | (static_cast<uint32_t>(pulled) & 0xFFFFu);
}

// The float camera's frame for a record's world point (the fallback of a vertex the GTE cannot place, and w of the
// clip planes): the screen frame of mat4.cpp LookAt from the same eye / forward / up, as scene_geometry.cpp's
// NearCamera builds it.
struct WorldOf {
    CellView v;
    explicit WorldOf(const CellView& view) : v(view) {
        const double f[3] = {v.forward[0], v.forward[1], v.forward[2]};
        double s[3] = {f[1] * v.up[2] - f[2] * v.up[1], f[2] * v.up[0] - f[0] * v.up[2], f[0] * v.up[1] - f[1] * v.up[0]};
        const double sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
        if (sl > 0.0) {
            for (double& c : s) c /= sl;
            const double u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
            for (int k = 0; k < 3; ++k) {
                v.right[k] = static_cast<float>(s[k]);
                v.up[k] = static_cast<float>(u[k]);
            }
        }
    }
    void Record(const SD::Rec& r, float out[3]) const {
        const double x = r.x / kRecordsPerWorldUnit, u = -(r.y / kGteRowAspect) / kRecordsPerWorldUnit,
                     z = r.z / kRecordsPerWorldUnit;
        for (int k = 0; k < 3; ++k) out[k] = static_cast<float>(v.eye[k] + x * v.right[k] + u * v.up[k] + z * v.forward[k]);
    }
};

// RTPS with RT = 0 and TR = (tx, ty, tz) (the near records' projection): SZ3 = tz.
struct RecordProjector {
    ModelGte gte;
    RecordProjector(int32_t ofx, int32_t ofy) {
        gte.h = 237;
        gte.ofx = ofx << 16;
        gte.ofy = ofy << 16;
    }
    uint32_t Sxy(int32_t tx, int32_t ty, int32_t tz) {
        gte.tr[0] = tx;
        gte.tr[1] = ty;
        gte.tr[2] = tz;
        const int16_t zero[3] = {0, 0, 0};
        int32_t mac[3];
        return gte.Rtps(zero, mac);
    }
};

// Turns the subdividers' leaves into soup vertices at their records' own SXY.
struct GteEnv final : SD::Env {
    RecordProjector proj;
    double squeeze;
    const CellLook& look;
    const WorldOf& world;
    rr::TriangleSoup* out = nullptr;
    GteScreens* screens = nullptr;
    rr::TriangleSoup::Vertex proto;
    uint32_t trueSxy[SD::kMaxRecords] = {};
    int32_t trueSz[SD::kMaxRecords] = {};
    uint32_t lastSxy = 0;
    int32_t lastSz = 0;
    size_t pieces = 0;
    bool flat = false; // F4 leaves: shade mode 2 in `flatColour`
    uint32_t flatColour = 0;
    GteEnv(int32_t ofx, int32_t ofy, double sq, const CellLook& l, const WorldOf& w)
        : proj(ofx, ofy), squeeze(sq), look(l), world(w) {}
    uint32_t Project(int32_t tx, int32_t ty, int32_t tz) override {
        lastSxy = proj.Sxy(tx, ty, tz);
        lastSz = tz;
        return Squeeze(lastSxy, squeeze);
    }
    uint32_t Colour(uint32_t index) override { return look.haveColours ? look.colours[index & 0xFFu] : 0x808080u; }
    bool Stored(int i, const SD::Rec&, bool) override {
        trueSxy[i] = lastSxy;
        trueSz[i] = lastSz;
        return true;
    }
    // Record i as a corner the caller projected itself.
    void Corner(int i, SD::Rec& r) {
        trueSxy[i] = proj.Sxy(r.x >> 5, r.y >> 5, r.z >> 5);
        trueSz[i] = r.z >> 5;
        r.sxy = Squeeze(trueSxy[i], squeeze);
    }
    void Put(const SD::Rec* recs, int i) {
        const SD::Rec& r = recs[i];
        rr::TriangleSoup::Vertex v = proto;
        float p[3];
        world.Record(r, p);
        v.x = p[0];
        v.y = p[1];
        v.z = p[2];
        if (flat) {
            v.shade[0] = static_cast<uint8_t>(flatColour & 0xFFu);
            v.shade[1] = static_cast<uint8_t>((flatColour >> 8) & 0xFFu);
            v.shade[2] = static_cast<uint8_t>((flatColour >> 16) & 0xFFu);
            v.shade[3] = 2;
        } else {
            v.u = static_cast<float>(r.uv & 0xFFu);
            v.v = static_cast<float>((r.uv >> 8) & 0xFFu);
            v.shade[0] = static_cast<uint8_t>(r.rgb & 0xFFu);
            v.shade[1] = static_cast<uint8_t>((r.rgb >> 8) & 0xFFu);
            v.shade[2] = static_cast<uint8_t>((r.rgb >> 16) & 0xFFu);
        }
        out->vertices.push_back(v);
        screens->push_back(GteScreenOf(trueSxy[i], r.z / kRecordsPerCellUnit, trueSz[i]));
    }
    void Trace(const char* tag, const SD::Rec* r, const int* v, int n) const {
        static const bool on = std::getenv("RRJB_GTE_TRACE") != nullptr; // DEVELOPMENT: the leaves, as psxgpu --prims
        if (!on) return;
        std::printf("gtetrace: %s", tag);
        for (int k = 0; k < n; ++k)
            std::printf(" %d,%d uv %u,%u", static_cast<int16_t>(trueSxy[v[k]] & 0xFFFFu), static_cast<int16_t>(trueSxy[v[k]] >> 16),
                        r[v[k]].uv & 0xFFu, (r[v[k]].uv >> 8) & 0xFFu);
        std::printf("\n");
    }
    bool Gt4(const SD::Rec* r, const int v[4]) override {
        Trace("gt4", r, v, 4);
        for (int k : {0, 1, 2, 1, 3, 2}) Put(r, v[k]);
        ++pieces;
        return true;
    }
    bool Gt3(const SD::Rec* r, const int v[3]) override {
        for (int k = 0; k < 3; ++k) Put(r, v[k]);
        ++pieces;
        return true;
    }
    bool F4(const SD::Rec* r, const int v[4], uint32_t colour) override {
        flat = true;
        flatColour = colour;
        for (int k : {0, 1, 2, 1, 3, 2}) Put(r, v[k]);
        flat = false;
        ++pieces;
        return true;
    }
};

SD::Kids KidsOf(const CellLook& look) {
    SD::Kids kids;
    for (size_t k = 0; k < 4; ++k) kids.tri[k] = look.tables.subTri[k];
    for (size_t k = 0; k < 4; ++k) kids.quad[k] = look.tables.subQuad[k];
    for (size_t k = 0; k < 2; ++k) kids.line[k] = look.tables.subLine[k];
    return kids;
}

// A whole polygon's corner: the vertex pass's own SXY and MAC3, the cell's world point.
void PutPassVertex(const rr::CellData& cell, const GteCell& gc, uint16_t index, rr::TriangleSoup::Vertex v,
                   rr::TriangleSoup& out, GteScreens& screens) {
    v.x = rr::CellWorldX(cell, index);
    v.y = rr::CellWorldY(cell, index);
    v.z = rr::CellWorldZ(cell, index);
    out.vertices.push_back(v);
    screens.push_back(GteScreenOf(gc.sxy[index], gc.z[index]));
}

} // namespace

bool GteProjOn() {
    static const bool off = [] {
        const char* v = std::getenv("RRJB_PROJ");
        return v != nullptr && std::string(v) == "float";
    }();
    return !off;
}

namespace {
size_t g_rejected[2] = {}, g_saturated = 0, g_saturatedNear = 0, g_nearDropped = 0;
} // namespace

size_t& GteRejected(int path) { return g_rejected[path & 1]; }

bool GpuRejectOn() {
    static const bool off = std::getenv("RRJB_GTE_BIG") != nullptr && std::string(std::getenv("RRJB_GTE_BIG")) == "draw";
    return !off;
}

bool GteSaturatedOn() {
    static const bool off = std::getenv("RRJB_GTE_SAT") != nullptr && std::string(std::getenv("RRJB_GTE_SAT")) == "float";
    return !off;
}

bool GpuRejects(uint32_t a, uint32_t b, uint32_t c) {
    const int32_t ax = static_cast<int16_t>(a & 0xFFFFu), ay = static_cast<int16_t>(a >> 16);
    const int32_t bx = static_cast<int16_t>(b & 0xFFFFu), by = static_cast<int16_t>(b >> 16);
    const int32_t cx = static_cast<int16_t>(c & 0xFFFFu), cy = static_cast<int16_t>(c >> 16);
    return std::max({ax, bx, cx}) - std::min({ax, bx, cx}) >= 1024 || std::max({ay, by, cy}) - std::min({ay, by, cy}) >= 512;
}

void CollapseTriangle(rr::TriangleSoup::Vertex* v) {
    for (int k = 1; k < 3; ++k) {
        v[k].x = v[0].x;
        v[k].y = v[0].y;
        v[k].z = v[0].z;
    }
}

void GteRejectBig(GteScreen* s, size_t n) {
    for (size_t t = 0; t + 2 < n; t += 3) {
        if (s[t].use < 1.5f || s[t + 1].use < 1.5f || s[t + 2].use < 1.5f) continue;
        // the cell emitters' trivial reject (0x8006D350 / 0x8006A630: the corners' bytes AND 0x1F non-zero), its z < 40 bit:
        // a polygon wholly at the eye is not drawn (the screen bits are the clip rectangle's business here)
        if (s[t].use == 2.25f && s[t + 1].use == 2.25f && s[t + 2].use == 2.25f) {
            s[t].use = s[t + 1].use = s[t + 2].use = 3.0f;
            ++g_nearDropped;
            continue;
        }
        const auto word = [](const GteScreen& e) {
            return (static_cast<uint32_t>(static_cast<int32_t>(e.sy)) << 16) | (static_cast<uint32_t>(static_cast<int32_t>(e.sx)) & 0xFFFFu);
        };
        if (!GpuRejectOn() || !GpuRejects(word(s[t]), word(s[t + 1]), word(s[t + 2]))) continue;
        s[t].use = s[t + 1].use = s[t + 2].use = 3.0f;
        ++g_rejected[0];
    }
}

GteScreen GteScreenOf(uint32_t sxy, double zCell, double sz3) {
    if (sz3 < 0.0) sz3 = zCell;
    GteScreen s;
    const int32_t sx = static_cast<int16_t>(sxy & 0xFFFFu), sy = static_cast<int16_t>(sxy >> 16);
    s.sx = static_cast<float>(sx);
    s.sy = static_cast<float>(sy);
    s.z = static_cast<float>(zCell);
    // RTPS: SZ3 <= H / 2 saturates the divide; SX / SY clamp to -0x400 / 0x3FF.
    s.use = (sz3 > 118.0 && zCell > 0.0 && sx > -0x400 && sx < 0x3FF && sy > -0x400 && sy < 0x3FF) ? 2.0f : 0.0f;
    if (s.use < 1.5f && GteSaturatedOn()) { // where the console draws it, the depth held off the near plane
        s.use = 2.0f;
        s.z = static_cast<float>(std::max(zCell, kGteMinW * 64.0));
        ++g_saturated;
        if (sz3 <= 118.0) ++g_saturatedNear;
        if (zCell < 40.0) s.use = 2.25f; // 0x8001034C's byte 0x10 (z < 40): GteRejectBig drops a triangle all of whose corners have it
    }
    return s;
}

bool GteCellFromSlot(const uint8_t* ram, uint32_t slot, const rr::CellData& cell, int32_t ofx, int32_t ofy, GteCell& out) {
    out.valid = false;
    rr::sim::GuestRam g(const_cast<uint8_t*>(ram), 0x8005AC8Cu);
    ModelGte& gte = out.gte;
    gte.LoadRt(g, slot + 0x10u);
    gte.LoadTr(g, slot + 0x24u);
    gte.h = 237; // RASHCDI 0x8005CF1C's H (the view projection record 0x800D82B0 + 4; every COP2 record of the traces)
    gte.ofx = ofx << 16;
    gte.ofy = ofy << 16;
    bool any = false;
    for (int16_t e : gte.rt) any = any || e != 0;
    if (!any) return false;
    const size_t n = cell.vertexCount;
    out.sxy.resize(n);
    out.z.resize(n);
    out.flags.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const int16_t v[3] = {cell.vertexX[i], cell.vertexY[i], cell.vertexZ[i]};
        int32_t mac[3];
        out.sxy[i] = gte.Rtps(v, mac);
        out.z[i] = mac[2];
        out.flags[i] = VertexByte(out.sxy[i], mac[2]);
    }
    out.valid = true;
    return true;
}

void AppendFineNearGroupGte(const rr::CellData& cell, const GteCell& gc, int group, uint16_t texKey, const CellView& view,
                            const CellLook& look, float sideSqueeze, rr::TriangleSoup& out, GteScreens& screens,
                            SubdivStats& stats, GteStats& gstats) {
    const WorldOf world(view);
    const double squeeze = std::max(1.0, static_cast<double>(sideSqueeze));
    GteEnv env(gc.gte.ofx >> 16, gc.gte.ofy >> 16, squeeze, look, world);
    env.out = &out;
    env.screens = &screens;
    const SD::Kids kids = KidsOf(look);
    for (const rr::CellPrimitive& prim : cell.band1) {
        if (static_cast<int>(prim.group) != group || CellPrimitiveKey(cell, prim) != texKey) continue;
        ++stats.nearPrims;
        ++gstats.nearPrims;
        const int n = prim.quad ? 4 : 3;
        uint8_t andBits = 0x1F, orBits = 0;
        int32_t maxZ = INT32_MIN;
        for (int k = 0; k < n; ++k) {
            const uint16_t index = prim.index[k];
            const uint8_t byte = squeeze > 1.0 ? VertexByte(Squeeze(gc.sxy[index], squeeze), gc.z[index]) : gc.flags[index];
            andBits &= byte;
            orBits |= byte;
            maxZ = std::max(maxZ, gc.z[index]);
        }
        // 0x8006A630: every corner outside one side of the screen (the vertex pass's bytes) - not drawn.
        if ((andBits & 0x1F) != 0) {
            ++stats.culledPrims;
            continue;
        }
        // The back-face test on the vertex pass's SXY (0x8006A6D4; a quad only while no corner is nearer than 40 and
        // (i0, i3, i2) faces the other way, 0x8006AFD4..0x8006B02C).
        if (look.nearNclip && (prim.flags & 4u) == 0) {
            const auto nclip = [&](int a, int b, int c) {
                const uint32_t sa = gc.sxy[prim.index[a]], sb = gc.sxy[prim.index[b]], sc = gc.sxy[prim.index[c]];
                const int64_t ax = static_cast<int16_t>(sa & 0xFFFFu), ay = static_cast<int16_t>(sa >> 16);
                const int64_t bx = static_cast<int16_t>(sb & 0xFFFFu), by = static_cast<int16_t>(sb >> 16);
                const int64_t cx = static_cast<int16_t>(sc & 0xFFFFu), cy = static_cast<int16_t>(sc >> 16);
                return ax * by + bx * cy + cx * ay - ax * cy - bx * ay - cx * by;
            };
            const bool back = !prim.quad ? nclip(0, 1, 2) < 0
                                         : ((orBits & 0x10) == 0 && nclip(0, 1, 2) < 0 && nclip(0, 3, 2) > 0);
            if (back) {
                ++stats.backPrims;
                continue;
            }
        }
        rr::TriangleSoup::Vertex proto;
        proto.tpage = prim.pal;
        ShadeCellVertex(cell, prim, 1, prim.index[0], look, proto);
        proto.shade[3] = 1;
        proto.ot = static_cast<float>(std::max(maxZ, 0)); // the largest corner MAC3 (the vertex pass's)
        if ((orBits & 0x20) == 0) {
            // A whole GT3 / GT4 at the vertex pass's SXY (0x8006A630's own packet: (i0, i1, i2) / (i0, i1, i3, i2)).
            const int order3[3] = {0, 1, 2};
            const int order4[6] = {0, 1, 3, 1, 2, 3};
            const int* order = prim.quad ? order4 : order3;
            const int count = prim.quad ? 6 : 3;
            for (int k = 0; k < count; ++k) {
                const int c = order[k];
                rr::TriangleSoup::Vertex v = proto;
                v.u = static_cast<float>(prim.u[c]);
                v.v = static_cast<float>(prim.v[c]);
                const uint32_t colour = env.Colour(static_cast<uint16_t>(cell.vertexW[prim.index[c]]) & 0xFFu);
                if (look.shade && look.haveColours) {
                    v.shade[0] = static_cast<uint8_t>(colour & 0xFFu);
                    v.shade[1] = static_cast<uint8_t>((colour >> 8) & 0xFFu);
                    v.shade[2] = static_cast<uint8_t>((colour >> 16) & 0xFFu);
                }
                PutPassVertex(cell, gc, prim.index[c], v, out, screens);
            }
            continue;
        }
        // Subdivided: the corners' MVMVA records, RTPS'd from TR = record >> 5, the vertex pass's byte and the depth bit.
        SD::Subdivider sd(env, kids);
        for (int k = 0; k < n; ++k) {
            const uint16_t index = prim.index[k];
            SD::Rec& r = sd.rec[k];
            int32_t rec[3];
            ViewRecord(gc.gte, cell.vertexX[index], cell.vertexY[index], cell.vertexZ[index], rec);
            r.x = rec[0];
            r.y = rec[1];
            r.z = rec[2];
            env.Corner(k, r);
            const uint8_t byte = squeeze > 1.0 ? VertexByte(Squeeze(gc.sxy[index], squeeze), gc.z[index]) : gc.flags[index];
            r.uv = static_cast<uint32_t>(prim.u[k]) | static_cast<uint32_t>(prim.v[k]) << 8;
            r.shade = static_cast<uint16_t>(cell.vertexW[index]) & 0xFF;
            r.rgb = (env.Colour(static_cast<uint32_t>(r.shade)) & 0xFFFFFFu) |
                    static_cast<uint32_t>(byte | CornerDepthBit(r.z)) << 24;
        }
        env.proto = proto;
        ++stats.splitPrims;
        const size_t before = env.pieces;
        const int32_t level = 5 - static_cast<int32_t>(prim.attr & 0xFu);
        const bool ok = prim.quad ? sd.Quad(4, 0x03020100u, level) : sd.Tri(3, 0x00020100u, level);
        if (!ok) ++stats.refused;
        stats.pieces += env.pieces - before;
    }
}

void AppendBand2Gte(const rr::CellData& cell, size_t cellIndex, size_t fineGroup, const GteCell& gc, const CellView& view,
                    const CellLook& look, float sideSqueeze, rr::TriangleSoup& road, GteScreens& roadScreens,
                    rr::TriangleSoup& lines, GteScreens& lineScreens, SubdivStats& stats, GteStats& gstats) {
    (void)cellIndex;
    if (!cell.region7Present || !look.haveTables) return;
    const size_t group = static_cast<size_t>(cell.countA) + 2u * cell.countB + fineGroup;
    const rr::CellDrawTables& t = look.tables;
    const WorldOf world(view);
    const double squeeze = std::max(1.0, static_cast<double>(sideSqueeze));
    const int32_t ofx = gc.gte.ofx >> 16, ofy = gc.gte.ofy >> 16;
    GteEnv env(ofx, ofy, squeeze, look, world);
    const SD::Kids kids = KidsOf(look);
    const auto colourOf = [&](int32_t shade) { return env.Colour(static_cast<uint32_t>(shade) & 0xFFu) & 0xFFFFFFu; };
    for (const rr::CellPrimitive& prim : cell.band2) {
        if (prim.group != group || !prim.quad) continue; // 0x8006E474 reads the group's QUAD count
        int32_t z[4];
        for (int c = 0; c < 4; ++c) z[c] = gc.z[prim.index[c]];
        const int32_t maxZ = std::max(std::max(z[0], z[1]), std::max(z[2], z[3])); // the vertex pass's MAC3
        const int group4 = static_cast<int>((prim.attr >> 2) & 0xC);
        const float otKey = static_cast<float>(std::clamp<int64_t>(maxZ, 0, 1 << 24));
        if (maxZ >= 0x1000) {
            // The far path (0x8006E7C0..0x8006E918): one GT4 (i0, i1, i3, i2) at the vertex pass's SXY, the record's
            // own UVs, palette row (attr >> 2) & 0xC, Gouraud from each corner's colour-table entry.
            ++gstats.band2Far;
            const int order[6] = {0, 1, 3, 1, 2, 3};
            for (int c : order) {
                rr::TriangleSoup::Vertex v;
                v.u = prim.u[c];
                v.v = prim.v[c];
                v.tpage = static_cast<uint16_t>(std::clamp(group4, 0, 11));
                const uint32_t col = colourOf(static_cast<uint16_t>(cell.vertexW[prim.index[c]]) & 0xFF);
                v.shade[0] = static_cast<uint8_t>(col & 0xFFu);
                v.shade[1] = static_cast<uint8_t>((col >> 8) & 0xFFu);
                v.shade[2] = static_cast<uint8_t>((col >> 16) & 0xFFu);
                v.shade[3] = 1;
                v.ot = otKey;
                PutPassVertex(cell, gc, prim.index[c], v, road, roadScreens);
            }
            continue;
        }
        ++gstats.band2Near;
        const unsigned templ = prim.attr & 0xFu;
        const uint32_t kinds = t.stripKinds[templ];
        const uint32_t laneBits = t.stripLines[templ];
        const int n = static_cast<int>(laneBits >> 28);
        if (n <= 0) continue;
        const int32_t recip = std::array<int32_t, 8>{0, 4096, 2048, 1365, 1024, 819, 682, 585}[n < 8 ? n : 7]; // 0x800CCA10
        const int32_t absZ = maxZ < 0 ? -maxZ : maxZ;
        const size_t depthIndex = static_cast<size_t>(absZ >> 9);
        const int nearTexture = depthIndex < t.nearByDepth.size() ? t.nearByDepth[depthIndex] : 0;
        // The records: corner c into slot {0, 2n, 2n + 1, 1}[c] (0x8006E6E8..), P_j = slot 2j on i0 -> i1, Q_j = 2j + 1
        // on i3 -> i2.
        std::vector<SD::Rec> rec(static_cast<size_t>(2 * n + 2));
        std::vector<uint32_t> rsxy(rec.size());
        std::vector<int32_t> rsz(rec.size());
        const int slotOf[4] = {0, 2 * n, 2 * n + 1, 1};
        RecordProjector proj(ofx, ofy);
        for (int c = 0; c < 4; ++c) {
            const uint16_t index = prim.index[c];
            SD::Rec& r = rec[static_cast<size_t>(slotOf[c])];
            int32_t v3[3];
            ViewRecord(gc.gte, cell.vertexX[index], cell.vertexY[index], cell.vertexZ[index], v3);
            r.x = v3[0];
            r.y = v3[1];
            r.z = v3[2];
            rsxy[static_cast<size_t>(slotOf[c])] = proj.Sxy(r.x >> 5, r.y >> 5, r.z >> 5);
            rsz[static_cast<size_t>(slotOf[c])] = r.z >> 5;
            r.sxy = Squeeze(rsxy[static_cast<size_t>(slotOf[c])], squeeze);
            r.shade = static_cast<uint16_t>(cell.vertexW[index]) & 0xFF;
            const uint8_t byte = squeeze > 1.0 ? VertexByte(Squeeze(gc.sxy[index], squeeze), gc.z[index]) : gc.flags[index];
            r.rgb = colourOf(r.shade) | static_cast<uint32_t>(byte | CornerDepthBit(r.z)) << 24;
        }
        // The steps (0x8006EC44..): (far - near) * recip >> 12 in 32 bits, the shade's as a halfword.
        int32_t step[2][3];
        int32_t shadeStep[2];
        for (int k = 0; k < 2; ++k) {
            const SD::Rec& a = rec[static_cast<size_t>(k)];
            const SD::Rec& b = rec[static_cast<size_t>(2 * n + k)];
            const auto mul = [recip](int32_t d) {
                return static_cast<int32_t>(static_cast<uint32_t>(d) * static_cast<uint32_t>(recip)) >> 12;
            };
            step[k][0] = mul(SubWrap(b.x, a.x));
            step[k][1] = mul(SubWrap(b.y, a.y));
            step[k][2] = mul(SubWrap(b.z, a.z));
            shadeStep[k] = static_cast<int16_t>(mul(SubWrap(b.shade, a.shade)));
        }
        for (int j = 1; j < n; ++j)
            for (int k = 0; k < 2; ++k) {
                const SD::Rec& p = rec[static_cast<size_t>(2 * j - 2 + k)];
                SD::Rec& r = rec[static_cast<size_t>(2 * j + k)];
                r.x = AddWrap(p.x, step[k][0]);
                r.y = AddWrap(p.y, step[k][1]);
                r.z = AddWrap(p.z, step[k][2]);
                const size_t s = static_cast<size_t>(2 * j + k);
                rsxy[s] = proj.Sxy(r.x >> 5, r.y >> 5, r.z >> 5);
                rsz[s] = r.z >> 5;
                r.sxy = Squeeze(rsxy[s], squeeze);
                r.shade = AddWrap(p.shade, shadeStep[k]);
                r.rgb = colourOf(r.shade) | static_cast<uint32_t>(SD::Outcode(r.sxy, r.z)) << 24;
            }
        // The lane lines (0x8006EEC4..0x8006F424), chained before the strips: per strip j and side, four records
        // (step >> 6) and a further (step >> 5) in from the strip edge; SubLine below 0x640, else one F4.
        env.out = &lines;
        env.screens = &lineScreens;
        for (int j = 0; j < n; ++j)
            for (int side = 0; side < 2; ++side) {
                const unsigned colourIndex = (laneBits >> (4 * j + 2 * side)) & 3u;
                if (colourIndex == 0) continue;
                SD::Subdivider sd(env, kids);
                for (int k = 0; k < 2; ++k) {
                    const SD::Rec& base = rec[static_cast<size_t>(side == 0 ? 2 * j + k : 2 * j + 2 + k)];
                    SD::Rec inner, outer;
                    const int sign = side == 0 ? 1 : -1;
                    for (int a = 0; a < 3; ++a) {
                        const int32_t b = a == 0 ? base.x : (a == 1 ? base.y : base.z);
                        const int32_t in = sign > 0 ? AddWrap(b, step[k][a] >> 6) : SubWrap(b, step[k][a] >> 6);
                        const int32_t outv = sign > 0 ? AddWrap(in, step[k][a] >> 5) : SubWrap(in, step[k][a] >> 5);
                        (a == 0 ? inner.x : (a == 1 ? inner.y : inner.z)) = in;
                        (a == 0 ? outer.x : (a == 1 ? outer.y : outer.z)) = outv;
                    }
                    // side 0: slots 0, 1 inner and 2, 3 outer; side 1: 2, 3 inner and 0, 1 outer
                    sd.rec[side == 0 ? k : 2 + k] = inner;
                    sd.rec[side == 0 ? 2 + k : k] = outer;
                }
                for (int s = 0; s < 4; ++s) {
                    SD::Rec& r = sd.rec[s];
                    env.Corner(s, r);
                    r.rgb = static_cast<uint32_t>(SD::Outcode(r.sxy, r.z)) << 24;
                }
                env.proto = rr::TriangleSoup::Vertex{};
                env.proto.ot = otKey;
                const uint32_t colour = t.lineColour[colourIndex];
                ++gstats.lines;
                if (maxZ < 0x640) {
                    if (!sd.Line(4, 0x01030200u, colour)) ++stats.refused;
                } else {
                    const int v4[4] = {0, 1, 2, 3};
                    env.F4(sd.rec, v4, colour);
                }
            }
        // The strips (0x8006F434..0x8006F548): records 2j .. 2j + 3 with template 2 * kind + near's texels, palette
        // row g + 1 + clutsel, through SubRoad(4, 0x01030200, 5).
        env.out = &road;
        env.screens = &roadScreens;
        for (int j = 0; j < n; ++j) {
            const unsigned kind = (kinds >> (5 * j)) & 3u;
            const unsigned clutsel = (kinds >> (5 * j + 2)) & 3u;
            const std::array<uint16_t, 4>& uv = t.stripUv[2 * kind + static_cast<unsigned>(nearTexture)];
            SD::Subdivider sd(env, kids);
            for (int k = 0; k < 4; ++k) {
                const size_t s = static_cast<size_t>(2 * j + k);
                sd.rec[k] = rec[s];
                sd.rec[k].uv = uv[static_cast<size_t>(k)];
                env.trueSxy[k] = rsxy[s];
                env.trueSz[k] = rsz[s];
            }
            env.proto = rr::TriangleSoup::Vertex{};
            env.proto.tpage = static_cast<uint16_t>(std::clamp(group4 + 1 + static_cast<int>(clutsel), 0, 11));
            env.proto.shade[3] = 1;
            env.proto.ot = otKey;
            const size_t before = env.pieces;
            if (!sd.Road(4, 0x01030200u, 5)) ++stats.refused;
            ++stats.roadStrips;
            stats.roadPieces += env.pieces - before;
        }
    }
}

void GteAttachScreens(GLuint vao, GLuint& vbo, GteScreens& screens) {
    GteRejectBig(screens.data(), screens.size()); // the GPU's large-polygon rule
    gl.BindVertexArray(vao);
    if (vbo == 0) gl.GenBuffers(1, &vbo);
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
    gl.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(screens.size() * sizeof(GteScreen)),
                  screens.empty() ? nullptr : screens.data(), GL_DYNAMIC_DRAW);
    gl.VertexAttribPointer(7, 4, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(sizeof(GteScreen)), nullptr);
    gl.EnableVertexAttribArray(7);
}

// ============================================================================ RaceScene's side
void RaceScene::GtePrograms() {
    LoadGteGl();
    gteLocation_ = gl.GetUniformLocation(program_, "uGte");
    gteMapLocation_ = gl.GetUniformLocation(program_, "uGteMap");
    gteDepthLocation_ = gl.GetUniformLocation(program_, "uGteDepth");
    gtePixLocation_ = gl.GetUniformLocation(program_, "uGtePix");
    otScreenLocation_ = gl.GetUniformLocation(program_, "uOtScreen");
    otScreenOnLocation_ = gl.GetUniformLocation(program_, "uOtScreenOn");
    gl.UseProgram(program_);
    gl.Uniform1i(gteLocation_, 0);
    gl.Uniform1i(otScreenLocation_, kOtScreenUnit);
    gl.Uniform1i(otScreenOnLocation_, 0);
    // A vertex array without attribute 7 reads this: not a GTE point.
    if (g_vertexAttrib4f) g_vertexAttrib4f(7, 0.0f, 0.0f, 0.0f, 0.0f);
    const char* pix = std::getenv("RRJB_GTE_PIX");
    gtePix_ = pix ? static_cast<float>(std::atof(pix)) : 0.5f;
    // The texel: the GPU steps u / v in fixed point from the vertices' exact integers and TRUNCATES (the near wall of
    // rr-pack, 11 pixels a texel: floor matches the original's texel edges, round-to-nearest puts them half a texel
    // off; the machines' pages 82..85 % -> 90 % close with it alone); 1/256 lifts GL's float interpolation that lands a
    // hair under an exact integer (quick's far facade: 6 minified primitives flip without it). For every textured
    // primitive of the program while the GTE projection is on (GteBegin); RRJB_GTE_ROUND=<bias>: another (DEVELOPMENT).
    const char* round = std::getenv("RRJB_GTE_ROUND");
    gteRound_ = round ? static_cast<float>(std::atof(round)) : 1.0f / 256.0f;
    texRoundLocation_ = gl.GetUniformLocation(program_, "uTexRound");
    gl.Uniform1f(texRoundLocation_, 0.5f);
}

void RaceScene::GteKeepSoup(const rr::TriangleSoup& soup) {
    gteSoupPos_.resize(soup.vertices.size() * 3);
    for (size_t i = 0; i < soup.vertices.size(); ++i) {
        gteSoupPos_[3 * i + 0] = soup.vertices[i].x;
        gteSoupPos_[3 * i + 1] = soup.vertices[i].y;
        gteSoupPos_[3 * i + 2] = soup.vertices[i].z;
    }
    gteSoupIndex_.clear();
    gteScreenStale_ = true; // a new soup (SetCells makes a new vertex array): its screen buffer is attached again
}

// Each static soup vertex's cell vertex: the one whose world point it was built from (BuildCellSoup copies
// CellWorldX/Y/Z of the index, so the floats are equal; two vertices at one point project to one point).
void RaceScene::GteIndexSoup() {
    if (!cellData_ || gteSoupPos_.empty() || !gteSoupIndex_.empty()) return;
    gteSoupIndex_.assign(gteSoupPos_.size() / 3, kNoIndex);
    std::vector<char> done(cellData_->size(), 0);
    size_t missing = 0;
    for (const CellRange& range : cellRanges_) {
        if (range.cell >= cellData_->size()) continue;
        const rr::CellData& cell = (*cellData_)[range.cell];
        std::map<std::tuple<float, float, float>, uint16_t> at;
        for (size_t i = 0; i < cell.vertexCount && i < kNoIndex; ++i)
            at.emplace(std::make_tuple(rr::CellWorldX(cell, i), rr::CellWorldY(cell, i), rr::CellWorldZ(cell, i)),
                       static_cast<uint16_t>(i));
        for (GLint k = range.first; k < range.first + range.count; ++k) {
            const size_t s = static_cast<size_t>(k);
            if (3 * s + 2 >= gteSoupPos_.size()) break;
            const auto it = at.find(std::make_tuple(gteSoupPos_[3 * s], gteSoupPos_[3 * s + 1], gteSoupPos_[3 * s + 2]));
            if (it != at.end()) gteSoupIndex_[s] = it->second;
            else ++missing;
        }
    }
    gteSoupMissing_ = missing;
    gteSoupPos_.clear();
    gteSoupPos_.shrink_to_fit();
    gteScreenFill_.assign(gteSoupIndex_.size(), GteScreen{});
}

void RaceScene::GteBegin(const DrawRequest& request) {
    gteOn_ = request.gteProj && GteProjOn() && otRam_ != nullptr && otCellIds_ != nullptr && cellData_ != nullptr;
    gl.Uniform1i(gteLocation_, gteOn_ ? 1 : 0);
    gl.Uniform1i(otScreenOnLocation_, 0);
    gl.Uniform1f(texRoundLocation_, gteOn_ ? gteRound_ : 0.5f); // the frame's texels: truncated (GtePrograms)
    // the fill rule (edge_rule.h): the frame's later draws (machines, objects) find their console points through uGteMap
    gl.Uniform1i(gl.GetUniformLocation(program_, "uEdgeMapOn"), gteOn_ ? 1 : 0);
    EdgeViewport(program_);
    if (!gteOn_) return;
    ++gteStats_.frames;
    gteViewProj_ = request.viewProj;
    {   // its inverse, for the shadow's packet points (GteScreenToWorld)
        const float* a = gteViewProj_.m;
        double* inv = gteInv_;
        inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
        inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
        inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
        inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
        inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
        inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
        inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
        inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
        inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
        inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
        inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
        inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
        inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
        inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
        inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
        inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
        const double det = static_cast<double>(a[0]) * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
        gteInvOk_ = det > 1e-30 || det < -1e-30;
        if (gteInvOk_)
            for (int k = 0; k < 16; ++k) inv[k] /= det;
    }
    for (int k = 0; k < 4; ++k) gteMap_[k] = request.gteMap[k];
    for (int k = 0; k < 2; ++k) gteDepth_[k] = request.gteDepth[k];
    gl.Uniform4f(gteMapLocation_, request.gteMap[0], request.gteMap[1], request.gteMap[2], request.gteMap[3]);
    gl.Uniform2f(gteDepthLocation_, request.gteDepth[0], request.gteDepth[1]);
    gl.Uniform1f(gtePixLocation_, gtePix_);
    gteOfx_ = request.gteOfx;
    gteOfy_ = request.gteOfy;
    // The slots of this view (0x800D87E8 + 0x70 i, i = 12 v .. 12 v + 11) by the cell they hold.
    gteSlotOf_.assign(cellData_->size(), 0u);
    gteCells_.resize(cellData_->size());
    gteCellDone_.assign(cellData_->size(), 0);
    const auto word = [this](uint32_t a) {
        uint32_t w = 0;
        std::memcpy(&w, otRam_ + (a & 0x1FFFFFu), 4);
        return w;
    };
    const uint32_t v = static_cast<uint32_t>(otView_ & 1);
    for (uint32_t i = 12u * v; i < 12u * v + 12u; ++i) {
        const uint32_t slot = 0x800D87E8u + 0x70u * i;
        const auto it = otCellIds_->find(word(slot));
        if (it != otCellIds_->end() && it->second < gteSlotOf_.size()) gteSlotOf_[it->second] = slot;
    }
    GteIndexSoup();
    // The static soup's screen buffer on the cell array, and its buffer texture for the ordering-table keys.
    if (gteScreenStale_ && cellVao_ != 0) {
        gteScreenStale_ = false;
        GteScreens zeros(static_cast<size_t>(cellVertexCount_));
        GteAttachScreens(cellVao_, gteCellScreenVbo_, zeros);
        if (gteCellScreenTex_ == 0) glGenTextures(1, &gteCellScreenTex_);
        gl.ActiveTexture(GL_TEXTURE0 + kOtScreenUnit);
        glBindTexture(GL_TEXTURE_BUFFER, gteCellScreenTex_);
        gl.TexBuffer(GL_TEXTURE_BUFFER, 0x8814 /* GL_RGBA32F */, gteCellScreenVbo_);
        gl.ActiveTexture(GL_TEXTURE0);
    }
}

const GteCell* RaceScene::GteCellOf(size_t cell) {
    if (!gteOn_ || cell >= gteCells_.size()) return nullptr;
    if (!gteCellDone_[cell]) {
        gteCellDone_[cell] = 1;
        gteCells_[cell].valid = false;
        if (gteSlotOf_[cell] != 0 &&
            GteCellFromSlot(otRam_, gteSlotOf_[cell], (*cellData_)[cell], gteOfx_, gteOfy_, gteCells_[cell])) {
            ++gteStats_.cells;
            gteStats_.vertices += gteCells_[cell].sxy.size();
        } else {
            ++gteStats_.cellsNoSlot;
        }
    }
    return gteCells_[cell].valid ? &gteCells_[cell] : nullptr;
}

void RaceScene::GteFillRange(const CellRange& range) {
    if (!gteOn_ || gteCellScreenVbo_ == 0 || !g_bufferSubData || range.count <= 0) return;
    const size_t first = static_cast<size_t>(range.first), count = static_cast<size_t>(range.count);
    if (first + count > gteScreenFill_.size()) return;
    const GteCell* gc = GteCellOf(range.cell);
    for (size_t k = 0; k < count; ++k) {
        const uint16_t index = gteSoupIndex_[first + k];
        GteScreen& s = gteScreenFill_[first + k];
        if (gc && index != kNoIndex && index < gc->sxy.size()) s = GteScreenOf(gc->sxy[index], gc->z[index]);
        else s = GteScreen{};
        if (s.use < 1.5f) ++gteStats_.floatVertices;
    }
    {   // the GPU's large-polygon rule on the range's whole triangles (the soup is triangles from vertex 0)
        const size_t t0 = (first + 2) / 3 * 3;
        if (t0 < first + count) GteRejectBig(gteScreenFill_.data() + t0, first + count - t0);
    }
    gl.BindBuffer(GL_ARRAY_BUFFER, gteCellScreenVbo_);
    g_bufferSubData(GL_ARRAY_BUFFER, static_cast<ptrdiff_t>(first * sizeof(GteScreen)),
                    static_cast<ptrdiff_t>(count * sizeof(GteScreen)), gteScreenFill_.data() + first);
}

void RaceScene::GteStaticSource(bool on) {
    if (!gteOn_) return;
    gl.Uniform1i(otScreenOnLocation_, on && gteCellScreenTex_ != 0 ? 1 : 0);
    if (!on || gteCellScreenTex_ == 0) return;
    gl.ActiveTexture(GL_TEXTURE0 + kOtScreenUnit);
    glBindTexture(GL_TEXTURE_BUFFER, gteCellScreenTex_);
    gl.ActiveTexture(GL_TEXTURE0);
}

void RaceScene::GteModelPoints(const CapturedVerts& c, const Mat4& m, std::vector<float>& pos) const {
    static const bool off = [] {
        const char* v = std::getenv("RRJB_GTE_MODELS");
        return v != nullptr && std::string(v) == "off";
    }();
    if (!gteOn_ || off || c.sxy.size() * 3 != pos.size() || c.mac3.size() != c.sxy.size()) return;
    // inverse(viewProj) and the model matrix's inverse, in double
    double a[16], inv[16];
    for (int k = 0; k < 16; ++k) a[k] = gteViewProj_.m[k];
    {   // cofactor expansion (column-major, as Mat4)
        inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
        inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
        inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
        inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
        inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
        inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
        inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
        inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
        inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
        inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
        inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
        inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
        inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
        inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
        inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
        inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
        const double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
        if (det > -1e-30 && det < 1e-30) return;
        for (double& e : inv) e /= det;
    }
    double r[9];
    for (int col = 0; col < 3; ++col)
        for (int row = 0; row < 3; ++row) r[3 * row + col] = m.m[4 * col + row];
    const double det = r[0] * (r[4] * r[8] - r[5] * r[7]) - r[1] * (r[3] * r[8] - r[5] * r[6]) + r[2] * (r[3] * r[7] - r[4] * r[6]);
    if (det > -1e-20 && det < 1e-20) return;
    const double ri[9] = {(r[4] * r[8] - r[5] * r[7]) / det, (r[2] * r[7] - r[1] * r[8]) / det, (r[1] * r[5] - r[2] * r[4]) / det,
                          (r[5] * r[6] - r[3] * r[8]) / det, (r[0] * r[8] - r[2] * r[6]) / det, (r[2] * r[3] - r[0] * r[5]) / det,
                          (r[3] * r[7] - r[4] * r[6]) / det, (r[1] * r[6] - r[0] * r[7]) / det, (r[0] * r[4] - r[1] * r[3]) / det};
    const double unit = static_cast<double>(64 << std::clamp(c.exp, 0, 15));
    for (size_t v = 0; v < c.sxy.size(); ++v) {
        const double zCell = c.mac3[v] * 64.0 / unit; // the view depth in cell units
        const GteScreen s = GteScreenOf(c.sxy[v], zCell, static_cast<double>(c.mac3[v]));
        ++gteModelVertices_;
        if (s.use < 1.5f) {
            ++gteModelFloat_;
            continue;
        }
        if (s.z != static_cast<float>(zCell)) ++gteSaturated_; // a saturated SXY, drawn where the console draws it
        const double w = static_cast<double>(s.z) / 64.0;
        const double clip[4] = {((s.sx + gtePix_) * gteMap_[0] + gteMap_[1]) * w, ((s.sy + gtePix_) * gteMap_[2] + gteMap_[3]) * w,
                                gteDepth_[0] * w + gteDepth_[1], w};
        double p[4];
        for (int row = 0; row < 4; ++row)
            p[row] = inv[row] * clip[0] + inv[4 + row] * clip[1] + inv[8 + row] * clip[2] + inv[12 + row] * clip[3];
        if (p[3] > -1e-12 && p[3] < 1e-12) continue;
        const double wp[3] = {p[0] / p[3] - m.m[12], p[1] / p[3] - m.m[13], p[2] / p[3] - m.m[14]};
        for (int row = 0; row < 3; ++row)
            pos[3 * v + static_cast<size_t>(row)] = static_cast<float>(ri[3 * row] * wp[0] + ri[3 * row + 1] * wp[1] + ri[3 * row + 2] * wp[2]);
    }
}

// ============================================================================ the other objects
bool CapturedToModelPoints(const Mat4& m, const CapturedVerts& c, std::vector<float>& out); // race_scene_shadow.cpp

void GteStreamArray(GLuint& vao, GLuint& vbo) {
    if (vao != 0) return;
    gl.GenVertexArrays(1, &vao);
    gl.BindVertexArray(vao);
    gl.GenBuffers(1, &vbo);
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
    gl.BufferData(GL_ARRAY_BUFFER, 16, nullptr, GL_DYNAMIC_DRAW);
    const GLsizei stride = static_cast<GLsizei>(sizeof(rr::TriangleSoup::Vertex));
    using V = rr::TriangleSoup::Vertex;
    gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(V, x)));
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(V, nx)));
    gl.EnableVertexAttribArray(1);
    gl.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(V, u)));
    gl.EnableVertexAttribArray(2);
    gl.VertexAttribPointer(3, 1, GL_UNSIGNED_SHORT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(V, tpage)));
    gl.EnableVertexAttribArray(3);
    gl.VertexAttribPointer(4, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, reinterpret_cast<void*>(offsetof(V, shade)));
    gl.EnableVertexAttribArray(4);
    gl.VertexAttribPointer(5, 4, GL_UNSIGNED_BYTE, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(V, window)));
    gl.EnableVertexAttribArray(5);
    gl.VertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offsetof(V, ot)));
    gl.EnableVertexAttribArray(6);
}

void GteStreamUpload(GLuint vao, GLuint vbo, const std::vector<rr::TriangleSoup::Vertex>& v) {
    gl.BindVertexArray(vao);
    gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
    gl.BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(v.size() * sizeof(rr::TriangleSoup::Vertex)), v.data(), GL_DYNAMIC_DRAW);
}

bool RaceScene::GteObjectSoup(const CapturedVerts* c, uint32_t model, int lod, const Mat4& m, const rr::ModelGroup& group,
                              const rr::TriangleSoup::Vertex* rest, size_t count, std::vector<rr::TriangleSoup::Vertex>& out) const {
    if (!gteOn_ || c == nullptr) return false;
    if (c->model != model || c->lod != lod || c->sxy.size() != group.verts.size() || c->rel.size() != 3 * group.verts.size()) {
        ++gteObjMissed_;
        return false;
    }
    // each soup vertex's model vertex: six a primitive, BuildTriangleSoup's corners (lod_pose_draw.cpp CapturedSoup)
    size_t at = 0;
    for (const rr::SubMesh& sub : group.subMeshes) at += 6 * sub.prims.size();
    if (at != count) {
        ++gteObjMissed_;
        return false;
    }
    std::vector<float> pos;
    if (!CapturedToModelPoints(m, *c, pos)) return false;
    GteModelPoints(*c, m, pos);
    out.assign(rest, rest + count);
    at = 0;
    uint32_t sxy[6] = {};
    for (const rr::SubMesh& sub : group.subMeshes)
        for (const rr::Primitive& prim : sub.prims) {
            for (int k = 0; k < 6; ++k) {
                const size_t vi = prim.index[static_cast<size_t>(rr::SoupCorners(prim)[k])];
                if (vi >= group.verts.size()) return false;
                rr::TriangleSoup::Vertex& v = out[at + static_cast<size_t>(k)];
                v.x = pos[3 * vi];
                v.y = pos[3 * vi + 1];
                v.z = pos[3 * vi + 2];
                sxy[k] = c->sxy[vi];
            }
            for (int t = 0; t < 6; t += 3)
                if (GpuRejectOn() && GpuRejects(sxy[t], sxy[t + 1], sxy[t + 2])) {
                    CollapseTriangle(&out[at + static_cast<size_t>(t)]);
                    ++GteRejected(1);
                }
            at += 6;
        }
    ++gteObjDraws_;
    return true;
}

bool RaceScene::GteScreenToWorld(double sx, double sy, double w, float out[3]) const {
    if (!gteOn_ || !gteInvOk_ || !(w > 0.0)) return false;
    const double clip[4] = {((sx + gtePix_) * gteMap_[0] + gteMap_[1]) * w, ((sy + gtePix_) * gteMap_[2] + gteMap_[3]) * w,
                            gteDepth_[0] * w + gteDepth_[1], w};
    double p[4];
    for (int row = 0; row < 4; ++row)
        p[row] = gteInv_[row] * clip[0] + gteInv_[4 + row] * clip[1] + gteInv_[8 + row] * clip[2] + gteInv_[12 + row] * clip[3];
    if (p[3] > -1e-12 && p[3] < 1e-12) return false;
    for (int k = 0; k < 3; ++k) out[k] = static_cast<float>(p[k] / p[3]);
    return true;
}

void RaceScene::GteEnd() {
    gl.Uniform1i(gteLocation_, 0);
    gl.Uniform1i(otScreenOnLocation_, 0);
}

std::string RaceScene::GteTotals() const {
    char line[512];
    std::snprintf(line, sizeof(line),
                  "gteproj: the world's vertices through the GTE's own RTPS (the cells' slot RT / TR, H 237) in %zu frame(s): "
                  "%zu cell vertex passes (%zu vertices), %zu drawn cells without a transformed slot (float), %zu static "
                  "soup vertices kept on the float camera, %zu near polygons, band 2 %zu far / %zu near quads, %zu lane "
                  "lines; soup vertices not matched to a cell vertex %zu; the machines' model-draw vertices %zu, of them on the "
                  "float camera %zu",
                  gteStats_.frames, gteStats_.cells, gteStats_.vertices, gteStats_.cellsNoSlot, gteStats_.floatVertices,
                  gteStats_.nearPrims, gteStats_.band2Far, gteStats_.band2Near, gteStats_.lines, gteSoupMissing_,
                  gteModelVertices_, gteModelFloat_);
    char more[512];
    std::snprintf(more, sizeof(more),
                  "; gteproj2: the cars / props / pedestrians drawn at their model-draw SXY %zu time(s) (%zu capture(s) of "
                  "another model / LOD: float), shadow quads at their packets' SXY %zu, vertices drawn at a saturated SXY %zu "
                  "(%zu of them the divide's, SZ3 <= H / 2; the models' %zu)%s, triangles refused by the GPU's 1024 x 512 rule %zu (cells) / %zu (models, "
                  "shadow)%s, cell triangles wholly at z < 40 dropped %zu%s",
                  gteObjDraws_, gteObjMissed_, gteShadowQuads_, g_saturated, g_saturatedNear, gteSaturated_,
                  GteSaturatedOn() ? "" : " - RRJB_GTE_SAT=float", g_rejected[0], g_rejected[1],
                  GpuRejectOn() ? "" : " - RRJB_GTE_BIG=draw", g_nearDropped,
                  std::getenv("RRJB_GTE_OBJECTS") != nullptr ? " - RRJB_GTE_OBJECTS set" : "");
    return std::string(line) + more;
}

} // namespace rr::render
