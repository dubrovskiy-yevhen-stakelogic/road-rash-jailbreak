// The original's streamer (stream.h). Every function transcribed from our own disassembly of SLUS_010.53
// (SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1) and RASHCDI.BIN, the Ghidra pseudo-C of
// work\ghidra as the reading aid; each accepted by its rrverify row.
#include "game/sim/stream.h"

#include "game/sim/cell_draw.h"

namespace rr::sim {

namespace {

using rc::S;
using rc::U;

uint32_t Cur(GuestRam& g) { return g.U32(g.gp() + kGpStCur); }
uint32_t Res(GuestRam& g) { return g.U32(kStResList); }
uint32_t Gs(GuestRam& g) { return g.U32(kStGameState); }
uint32_t Players(GuestRam& g) { return g.U32(Gs(g) + 0x30u); }
uint32_t Rec(GuestRam& g, uint32_t i) { return Res(g) + 0x2Cu + 36u * i; }
// SLUS 0x800245DC / 0x800245F4: a road record (16 bytes) / a node record (40 bytes).
uint32_t RoadRec(GuestRam& g, uint32_t road) { return g.U32(g.U32(kStRoadTables) + 0x18u) + road * 16u; }
uint32_t NodeRec(GuestRam& g, uint32_t node) { return g.U32(g.U32(kStRoadTables) + 0x14u) + node * 40u; }
// SLUS 0x80023DB8(dst): the snapshot {road, along, dir}.
void Snap(GuestRam& g, uint32_t dst) {
    const uint32_t c = Cur(g);
    g.W32(dst + 8u, g.U32(c + 0x40u));
    g.W32(dst + 0u, g.U32(c + 0x48u));
    g.W32(dst + 4u, g.U32(c + 0x44u));
}
bool Call(RecoverCallees& c, uint32_t fn, std::initializer_list<uint32_t> a, uint32_t sp, uint32_t* v0 = nullptr) {
    return rc::Call(c, fn, a, sp, v0);
}

} // namespace

// ============================================================================ the stream record
void StreamSelect(GuestRam& g, uint32_t p) { g.W32(g.gp() + kGpStCur, kStStates + kStStateBytes * p); }

void StreamTrack(GuestRam& g) {
    uint32_t c = Cur(g);
    const uint32_t e = g.U32(c + 4u);
    if (g.U16(e + 0x16Au) != 0) return;
    g.W32(c + 0x44u, U(g.S16(e + 0x172u)));
    g.W32(c + 0x48u, g.U16(e + 0x168u));
    const int32_t d = g.S32(g.U32(c + 4u) + 0x16Cu);
    g.W32(c + 0x40u, d < 0 ? 0xFFFFFFFFu : (d > 0 ? 1u : 0u));
    c = Cur(g);
    const int32_t run = g.S32(c + 0x40u) > 0 ? g.S32(c + 0x68u) - g.S32(c + 0x44u) : g.S32(c + 0x44u) - g.S32(c + 0x68u);
    g.W32(c + 0x64u, U(run));
    const uint32_t roadChanged = g.U32(c + 0x48u) != g.U32(c + 0x50u) ? 1u : 0u;
    g.W32(c + 0x2Cu, roadChanged);
    const uint32_t dirChanged = g.U32(c + 0x58u) != g.U32(c + 0x40u) ? 1u : 0u;
    g.W32(c + 0x30u, dirChanged);
    if (roadChanged != 0 || dirChanged != 0) {
        const uint32_t r = RoadRec(g, g.U32(c + 0x48u));
        g.W32(c + 0x5Cu, r);
        g.W32(c + 0x60u, g.U32(r + 4u) >> 6);
        g.W32(c + 0x34u, g.S32(c + 0x40u) > 0 ? g.U32(g.U32(c + 0x5Cu) + 0x0Cu) : g.U32(g.U32(c + 0x5Cu) + 8u));
    }
    c = Cur(g);
    const int32_t along = g.S32(c + 0x44u);
    if (g.U32(c + 0x3Cu) != 0) {
        if (along >= 41 && along < g.S32(c + 0x60u) - 40) {
            g.W32(c + 0x3Cu, 0);
            g.W32(c + 0x24u, 1);
        }
    } else if (along < 41 || !(along < g.S32(c + 0x60u) - 40)) {
        if (g.U32(c + 0x2Cu) != 0 || g.U32(c + 0x30u) != 0 ||
            static_cast<int32_t>(static_cast<uint32_t>(along - g.S32(c + 0x54u)) * g.U32(c + 0x40u)) >= 0) {
            g.W32(c + 0x3Cu, 1);
            g.W32(c + 0x28u, 1);
        }
    }
    Snap(g, Cur(g) + 0x50u);
}

void StreamCursor(GuestRam& g, uint32_t cursor, uint32_t file, uint32_t start, uint32_t len) {
    g.W32(cursor + 4u, start);
    g.W32(cursor + 12u, start);
    g.W32(cursor + 8u, 0x10u);
    g.W32(cursor + 0u, file);
    g.W32(cursor + 16u, start + len);
    g.W32(cursor + 20u, 1u);
}

void StreamRange(GuestRam& g) {
    const uint32_t c = Cur(g);
    const uint32_t mode = g.U32(c + 0x38u);
    if (mode == 0) {
        const uint32_t toc = g.U32(g.gp() + kGpStToc);
        uint32_t i = 0;
        if (g.S32(toc + 12u) > 0) {
            const uint32_t road = g.U32(c + 0x48u);
            uint32_t off = 0;
            for (;;) {
                if (g.U32(off + g.U32(toc + 20u)) == road) break;
                ++i;
                off += 20u;
                if (!(S(i) < g.S32(toc + 12u))) break;
            }
        }
        const uint32_t c2 = Cur(g);
        const uint32_t ent = 20u * i + g.U32(g.U32(g.gp() + kGpStToc) + 20u);
        const uint32_t file = g.U32(g.gp() + kGpStStr);
        if (g.S32(c2 + 0x40u) > 0) StreamCursor(g, c2 + 8u, file, g.U32(ent + 4u), g.U32(ent + 8u));
        else StreamCursor(g, c2 + 8u, file, g.U32(ent + 12u), g.U32(ent + 16u));
    } else if (mode == 1) {
        StreamCursor(g, c + 8u, g.U32(g.gp() + kGpStStr), 0, 0);
    }
    const uint32_t c3 = Cur(g);
    g.W32(c3 + 0x64u, 0);
    g.W32(c3 + 0x68u, g.S32(c3 + 0x40u) < 1 ? 0x10000u : 0u);
}

void StreamLimit(GuestRam& g, uint32_t off) {
    const uint32_t c = Cur(g);
    const uint32_t pos = g.U32(c + 0x14u) + off;
    g.W32(c + 0x0Cu, pos);
    if (g.U32(c + 0x18u) < pos) g.W32(c + 0x0Cu, g.U32(c + 0x18u));
}

void StreamSeek(GuestRam& g) {
    uint32_t c = Cur(g);
    if (g.U32(c + 0x2Cu) == 0 && g.U32(c + 0x30u) == 0 && g.U32(c + 0x28u) == 0 && g.U32(c + 0x24u) == 0) return;
    c = Cur(g);
    const uint32_t mode = g.U32(c + 0x38u);
    if (mode == 0) {
        if (g.U32(c + 0x30u) != 0) {
            StreamRange(g);
            const uint32_t c2 = Cur(g);
            const uint32_t rls = g.U32(g.gp() + kGpStRls + 4u * g.U32(c2));
            StreamLimit(g, g.S32(c2 + 0x40u) > 0 ? g.U32(rls + 4u) : g.U32(rls + 8u));
        } else if (g.U32(c + 0x28u) != 0) {
            g.W32(c + 0x38u, 1);
        } else if (g.U32(c + 0x2Cu) != 0) {
            g.W32(c + 0x38u, 1);
            g.W32(c + 0x3Cu, 1);
        }
    } else if (mode == 1) {
        if (g.U32(c + 0x28u) != 0) {
            g.W32(c + 0x38u, 0);
            StreamRange(g);
            return;
        }
        if (g.U32(c + 0x24u) != 0) {
            if ((g.U32(c + 0x1Cu) & 2u) == 0) {
                g.W32(c + 0x3Cu, mode);
                return;
            }
            g.W32(c + 0x38u, 0);
            StreamRange(g);
        }
    }
    c = Cur(g);
    g.W32(c + 0x24u, 0);
    g.W32(c + 0x28u, 0);
    g.W32(c + 0x30u, 0);
    g.W32(c + 0x2Cu, 0);
}

uint32_t StreamAtEnd(GuestRam& g, uint32_t cursor) {
    const uint32_t file = kStFileTable + 24u * g.U32(cursor);
    if (g.U32(cursor + 4u) < g.U32(cursor + 16u)) g.W32(cursor + 20u, g.U32(cursor + 20u) & ~2u); // 0x800233F4
    else g.W32(cursor + 20u, g.U32(cursor + 20u) | 2u);                                          // 0x800233B4
    g.W32(file, g.U32(file) & ~0x100u);
    return g.U32(cursor + 20u) & 2u;
}

void StreamReset(GuestRam& g) {
    uint32_t c = Cur(g);
    g.W32(c + 4u, 0);
    g.W32(c + 0x2Cu, 0);
    g.W32(c + 0x3Cu, 0);
    g.W32(c + 0x20u, 0);
    g.W32(c + 0x48u, 0xFFFFFFFFu);
    g.W32(c + 0x34u, 0xFFFFFFFFu);
    g.W32(c + 0x5Cu, 0);
    g.W32(c + 0x60u, 0);
    g.W32(c + 0x4Cu, 0);
    g.W32(c + 0x28u, 0);
    g.W32(c + 0x24u, 0);
    g.W32(c + 0x58u, 0);          // 0x80023DA4(+0x50)
    g.W32(c + 0x50u, 0xFFFFFFFFu);
    g.W32(c + 0x54u, 0xFFFFFFFFu);
    c = Cur(g);
    g.W32(c + 0x64u, 0);
    g.W32(c + 0x68u, 0);
    g.W32(c + 0x6Cu, 0);
}

void StreamPlace(GuestRam& g, uint32_t road, int32_t along, int32_t dir) {
    for (uint32_t p = 0; p < Players(g); ++p) {
        StreamSelect(g, p);
        uint32_t c = Cur(g);
        g.W32(c + 0x48u, road);
        g.W32(c + 0x44u, U(along));
        g.W32(c + 0x40u, U(dir));
        const uint32_t r = RoadRec(g, road);
        c = Cur(g);
        g.W32(c + 0x5Cu, r);
        g.W32(c + 0x60u, g.U32(r + 4u) >> 6);
        g.W32(c + 0x34u, g.S32(c + 0x40u) > 0 ? g.U32(g.U32(c + 0x5Cu) + 0x0Cu) : g.U32(g.U32(c + 0x5Cu) + 8u));
        Snap(g, Cur(g) + 0x50u);
    }
}

// ============================================================================ the resource table
void ResCount(GuestRam& g, int32_t from, int32_t to, uint32_t sp, RecoverCallees& c) {
    if (g.U32(kStMusicBusy) == 0) Call(c, kStCritEnterFn, {}, sp - 32u);
    const uint32_t r = Res(g);
    auto bump = [&](int32_t which, int32_t d) {
        uint32_t at = 0;
        if (which == 0) at = 0x10u;
        else if (which == 1) at = 0x00u;
        else if (which == 2) at = 0x14u;
        else if (which == 3) at = 0x0Cu;
        else return;
        g.W32(r + at, g.U32(r + at) + U(d));
    };
    bump(from, -1); // 0x80032190
    bump(to, 1);
    if (g.U32(kStMusicBusy) == 0) Call(c, kStCritLeaveFn, {}, sp - 32u);
}

// 0x80032190(from, to): the counter bump itself (ResCount's body, called bare inside a critical section).
void ResBump(GuestRam& g, int32_t from, int32_t to) {
    const uint32_t r = Res(g);
    auto bump = [&](int32_t which, int32_t d) {
        uint32_t at = 0;
        if (which == 0) at = 0x10u;
        else if (which == 1) at = 0x00u;
        else if (which == 2) at = 0x14u;
        else if (which == 3) at = 0x0Cu;
        else return;
        g.W32(r + at, g.U32(r + at) + U(d));
    };
    bump(from, -1);
    bump(to, 1);
}

void ResFree(GuestRam& g, uint32_t rec, uint32_t sp, RecoverCallees& c) {
    const uint32_t f = g.U32(rec);
    const bool more = ((S(f) >> 12) & 0xF) != 0;
    const uint32_t csp = sp - 32u;
    if ((f & 1u) != 0) {
        g.W32(rec, f & 0xFFFFFFDCu);
        ResCount(g, 1, 0, csp, c);
        const uint32_t f2 = g.U32(rec);
        const uint32_t r = Res(g);
        if ((f2 & 0x10u) != 0) g.W32(r + 4u, g.U32(r + 4u) - 1u);
        else if ((f2 & 8u) != 0) g.W32(r + 8u, g.U32(r + 8u) - 1u);
    } else {
        ResCount(g, 2, 0, csp, c);
        const uint32_t f2 = g.U32(rec);
        const uint32_t r = Res(g);
        if ((f2 & 0x10u) != 0) g.W32(r + 0x18u, g.U32(r + 0x18u) - 1u);
        else if ((f2 & 8u) != 0) g.W32(r + 0x1Cu, g.U32(r + 0x1Cu) - 1u);
    }
    if (!more) {
        g.W32(rec + 8u, 0);
        g.W32(rec, 0);
        g.W32(rec + 16u, 0);
        g.W32(rec + 20u, 0);
    } else {
        g.W32(rec, (g.U32(rec) - 0x1000u) | 4u);
        ResCount(g, 0, 2, csp, c);
    }
}

void ResFreeByBuffer(GuestRam& g, uint32_t buf, uint32_t sp, RecoverCallees& c) {
    uint32_t rec = Res(g) + 0x2Cu;
    int32_t i = 0;
    if (g.S32(Res(g) + 0xA58u) > 0) {
        do {
            ++i;
            if (g.U32(rec + 0x10u) == buf) break;
            rec += 36u;
        } while (i < g.S32(Res(g) + 0xA58u));
    }
    ResFree(g, rec, sp - 24u, c);
}

int32_t ResTakeMusic(GuestRam& g, uint32_t list, int32_t n, uint32_t sp, RecoverCallees& c) {
    uint32_t i = g.U32(Res(g) + 0xA50u);
    const uint32_t last = g.U32(Res(g) + 0xA54u);
    const uint32_t csp = sp - 48u; // 0x80031064's frame
    if (g.U32(kStMusicBusy) == 0) Call(c, kStCritEnterFn, {}, csp);
    uint32_t rec = Rec(g, i);
    int32_t got = 0;
    while (i <= last && got < n) {
        if (g.U32(rec) == 0) {
            g.W32(list, rec);
            list += 4u;
            ++got;
            g.W32(rec, 0x100u);
            ResBump(g, 0, 3);
        }
        ++i;
        rec += 36u;
    }
    if (g.U32(kStMusicBusy) == 0) Call(c, kStCritLeaveFn, {}, csp);
    return got;
}

int32_t ResTakeStream(GuestRam& g, uint32_t list, int32_t n, int32_t all, int32_t p, uint32_t sp, RecoverCallees& c) {
    uint32_t i = g.U32(Res(g) + 8u * U(p) + 0xA40u);
    const uint32_t last = g.U32(Res(g) + 8u * U(p) + 0xA44u);
    const uint32_t csp = sp - 56u;
    if (g.U32(kStMusicBusy) == 0) Call(c, kStCritEnterFn, {}, csp);
    int32_t kept = 0, got = 0;
    uint32_t rec = Rec(g, i);
    while (i <= last && got < n) {
        if (g.U32(rec) == 0) {
            if (all == 0 && kept < g.S32(Res(g) + 0xA5Cu)) {
                ++kept;
            } else {
                g.W32(list, rec);
                list += 4u;
                ++got;
                g.W32(rec, 0x100u);
                ResBump(g, 0, 3);
            }
        }
        ++i;
        rec += 36u;
    }
    if (g.U32(kStMusicBusy) == 0) Call(c, kStCritLeaveFn, {}, csp);
    return got;
}

void ResArm(GuestRam& g, uint32_t rec, uint32_t kind, uint32_t sp, RecoverCallees& c) {
    g.W32(rec, kind | 4u);
    ResCount(g, 3, 2, sp - 24u, c);
    const uint32_t r = Res(g);
    if (kind == 8) g.W32(r + 0x1Cu, g.U32(r + 0x1Cu) + 1u);
    else if (kind == 0x10) g.W32(r + 0x18u, g.U32(r + 0x18u) + 1u);
}

int32_t ResTake(GuestRam& g, uint32_t list, uint32_t n, uint32_t kind, uint32_t contiguous, uint32_t arg,
                uint32_t sp, RecoverCallees& c) {
    const uint32_t csp = sp - 48u;
    uint32_t got = 0;
    if (n == 0) return 0;
    const int32_t p = static_cast<int32_t>((arg >> 16) & 0xFFu); // the fifth argument's byte 2
    if (kind == 8) {
        got = U(ResTakeMusic(g, list, S(n), csp, c));
        if (contiguous == 0) got += U(ResTakeStream(g, list + 4u * got, S(n - got), 0, p, csp, c));
    } else {
        got = U(ResTakeStream(g, list, S(n), 0, p, csp, c));
        if (contiguous != 0) got += U(ResTakeMusic(g, list + 4u * got, S(n - got), csp, c));
    }
    for (uint32_t k = 0; k < got; ++k) ResArm(g, g.U32(list + 4u * k), kind, csp, c);
    uint32_t ret = got;
    if (contiguous != 0 && 1u < got && got < n) {
        const uint32_t r = Res(g);
        ret = n;
        uint32_t src = list;
        for (uint32_t k = 0; k != n - got; ++k) {
            g.W32(list + 4u * (k + got), g.U32(src));
            g.W32(g.U32(src), g.U32(g.U32(src)) + 0x1000u);
            g.W32(r + 0x1Cu, g.U32(r + 0x1Cu) + 1u);
            src += 4u;
        }
    }
    return S(ret);
}

void StreamUnwind(GuestRam& g, uint32_t list, int32_t n, uint32_t sp, RecoverCallees& c) {
    for (int32_t k = 0; k < n; ++k) {
        const uint32_t idx = g.U32(g.U32(list) + 4u);
        ResFree(g, Rec(g, idx), sp - 32u - 24u, c); // 0x80030FD0 (frame 24) from 0x80023358 (frame 32)
        list += 4u;
    }
}

int32_t StreamRequest(GuestRam& g, uint32_t cursor, uint32_t n, uint32_t contiguous, uint32_t arg, uint32_t sp,
                      RecoverCallees& c) {
    const uint32_t csp = sp - 88u;
    if ((g.U32(cursor + 20u) & 2u) != 0) return -1;
    const int32_t got = ResTake(g, kStReqList, n, g.U32(cursor + 8u), contiguous, arg, csp, c);
    uint32_t at = kStReqList;
    for (int32_t k = 0; k < got; ++k, at += 4u) {
        if (StreamAtEnd(g, cursor) != 0) {
            StreamUnwind(g, kStReqList + 4u * U(k), got - k, csp, c);
            return -1;
        }
        const uint32_t req = csp + 24u; // the request, in 0x80023148's frame
        const uint32_t rec = g.U32(at);
        g.W32(req + 0u, g.U32(cursor + 8u));
        g.W32(req + 4u, g.U32(cursor + 0u));
        g.W32(req + 8u, g.U32(cursor + 4u));
        g.W32(req + 12u, g.U32(rec + 4u));
        g.W32(req + 20u, arg);
        g.W32(req + 24u, kStDoneFn);
        g.W32(req + 28u, 0x4000u);
        g.W32(req + 16u, g.U32(rec + 12u));
        uint32_t v = 0;
        if (!Call(c, kStCdQueueFn, {req}, csp, &v)) return -1;
        if (v == 0xFFFFFF9Cu) {
            StreamUnwind(g, at, got - k, csp, c);
            return -1;
        }
        g.W32(cursor + 4u, g.U32(cursor + 4u) + 0x4000u);
    }
    return 1;
}

int32_t ResFreeCount(GuestRam& g, int32_t p) {
    const uint32_t r = Res(g);
    uint32_t i = g.U32(r + 8u * U(p) + 0xA40u);
    uint32_t rec = r + 36u * i + 0x2Cu;
    int32_t spare = 0, n = 0;
    for (; i <= g.U32(Res(g) + 8u * U(p) + 0xA44u); ++i, rec += 36u) {
        if (g.U32(rec) != 0) continue;
        if (Players(g) == 1 && spare < 2) ++spare;
        else ++n;
    }
    return n;
}

void StreamFreeCount(GuestRam& g) {
    const int32_t n = ResFreeCount(g, S(g.U32(Cur(g))));
    const uint32_t c = Cur(g);
    g.W32(c + 0x6Cu, (n == 0 || (g.U32(c + 0x1Cu) & 2u) != 0) ? 0u : 1u);
}

void StreamRead(GuestRam& g, uint32_t sp, RecoverCallees& c) {
    const uint32_t csp = sp - 24u;
    const uint32_t turn = g.U32(g.gp() + kGpStTurn);
    const uint32_t other = turn ^ 1u;
    if (g.U32(kStStates + 0x6Cu + 128u * turn) == 0 && g.U32(kStStates + 0x6Cu + 128u * other) != 0)
        g.W32(g.gp() + kGpStTurn, other);
    const uint32_t t = g.U32(g.gp() + kGpStTurn);
    const int32_t x = g.S32(Res(g) + 0x18u);
    const int32_t v = 5 - x;
    const int32_t a2 = v >= 0 ? v : 0;
    const int32_t a1 = x < 0 ? x : 0;
    StreamRequest(g, kStStates + 8u + 128u * t, U(a2 + a1), 0, t << 16, csp, c);
    const uint32_t pos = g.U32(kStStates + 12u + 128u * g.U32(g.gp() + kGpStTurn));
    if (pos != 0) g.W32(g.gp() + kGpStLastPos, pos);
}

int32_t ResWindow(GuestRam& g, uint32_t, uint32_t w, uint32_t p) {
    const uint32_t keep = g.U32(Cur(g));
    StreamSelect(g, p);
    // 0x800243EC(type, windows)
    uint32_t i = 0;
    int32_t r = -1;
    if (g.S16(w) != -1) {
        for (;;) {
            if (g.S32(Cur(g) + 0x48u) == g.S16(w)) break;
            w += 6u;
            ++i;
            if (g.S16(w) == -1) break;
            if (!(i < 4u)) break;
        }
    }
    if (i < 4u) {
        const uint32_t c = Cur(g);
        if (g.S32(c + 0x48u) == g.S16(w)) {
            const int32_t along = g.S32(c + 0x44u);
            if (along < g.S16(w + 2u) || g.S16(w + 4u) < along) {
                if (g.S32(c + 0x40u) > 0) r = along < g.S16(w + 2u) ? 1 : -1;
                else r = g.S16(w + 4u) < along ? 1 : -1;
            } else {
                r = 0;
            }
        }
    }
    StreamSelect(g, keep);
    return r;
}

void ResRelease(GuestRam& g, uint32_t p, uint32_t sp, RecoverCallees& c) {
    const uint32_t csp = sp - 40u;
    const uint32_t r = Res(g);
    uint32_t i = g.U32(r + 8u * p + 0xA40u);
    const uint32_t last = g.U32(r + 8u * p + 0xA44u);
    g.W32(r + 0x20u, 0);
    g.W32(r + 0x24u, 0);
    g.W32(r + 0x28u, 0);
    uint32_t rec = r + 36u * i + 0x2Cu;
    for (; i <= last; ++i, rec += 36u) {
        if ((g.U32(rec) & 0x91u) != 0x11u) continue;
        const uint32_t hdr = g.U32(rec + 20u);
        const int32_t w = ResWindow(g, g.U32(hdr) >> 28, hdr + 4u, p);
        if (w < 0) ResUnload(g, rec, p, csp, c);
        const uint32_t rr = Res(g);
        const uint32_t at = w == 0 ? 0x24u : (w < 0 ? 0x20u : 0x28u);
        g.W32(rr + at, g.U32(rr + at) + 1u);
    }
    TexSweep(g, p, csp, c);
}

void StreamPlayers(GuestRam& g, uint32_t sp, RecoverCallees& c) {
    const uint32_t csp = sp - 24u;
    for (uint32_t p = 0; p < Players(g);) {
        StreamSelect(g, p);
        StreamTrack(g);
        if (g.U32(Cur(g) + 0x2Cu) != 0) ResEntryTag(g, g.U32(Cur(g) + 0x48u), 0, p);
        ++p;
        StreamSeek(g);
        ResRelease(g, g.U32(Cur(g)), csp - 24u, c); // 0x80023D7C (frame 24)
        StreamFreeCount(g);
    }
    StreamRead(g, csp, c);
}

void ResReadDone(GuestRam& g, uint32_t index, uint32_t kind, uint32_t arg, uint32_t sp, RecoverCallees& c) {
    const uint32_t csp = sp - 32u;
    const uint32_t rec = Rec(g, index);
    if ((g.U32(rec) & 9u) == 9u) Call(c, kStPrintFn, {0x80010D00u}, csp); // a diagnostic print, then on
    g.W32(rec, (g.U32(rec) | 1u) & ~4u);
    ResBump(g, 2, 1);
    const uint32_t f = g.U32(rec);
    const uint32_t r = Res(g);
    if ((f & 0x10u) != 0) {
        g.W32(r + 4u, g.U32(r + 4u) + 1u);
        g.W32(r + 0x18u, g.U32(r + 0x18u) - 1u);
    } else if ((f & 8u) != 0) {
        g.W32(r + 8u, g.U32(r + 8u) + 1u);
        g.W32(r + 0x1Cu, g.U32(r + 0x1Cu) - 1u);
    }
    g.W32(rec + 0x20u, arg);
    g.W32(rec, g.U32(rec) | kind);
    if (kind == 8) {
        g.W32(rec, g.U32(rec) | 0x20u);
        g.W32(rec + 8u, 0xFFFFFFFFu); // 0x800315E8
        g.W32(rec + 0x14u, 0);
        g.W32(rec + 0x18u, 0);
        g.W32(rec + 0x10u, g.U32(rec + 0x0Cu));
        ResLoad(g, rec, 0, csp, c);
    } else if (kind == 0x10) {
        // 0x80031540: the header and the payload by the key type
        const uint32_t buf = g.U32(rec + 0x0Cu);
        g.W32(rec + 0x14u, buf);
        g.W32(rec + 8u, g.U32(buf));
        const uint32_t type = g.U32(g.U32(rec + 0x14u)) >> 28;
        if (type < 3u && type != 0u) {
            g.W32(rec + 0x18u, 0);
            g.W32(rec + 0x10u, g.U32(rec + 0x0Cu));
        } else if (type >= 3u && type != 8u) {
            g.W32(rec + 0x18u, 0);
            g.W32(rec + 0x10u, g.U32(rec + 0x0Cu) + 0x20u);
        } else {
            const uint32_t pay = g.U32(rec + 0x0Cu) + 0x20u;
            g.W32(rec + 0x10u, pay);
            const int32_t len = S(g.U32(g.U32(rec + 0x0Cu) + 0x20u) * 4u) - 48;
            if (len > 0) g.W32(rec + 0x18u, pay + U(len));
        }
        // 0x80031CD4: onto the new-block queue (the console prints a diagnostic when it is full)
        const uint32_t q = Res(g);
        if (g.U32(q + 0x934u) == 0x40u) {
            Call(c, kStPrintFn, {0x80010D2Cu}, csp - 24u); // "New Block List Full"
        } else {
            g.W32(rec, g.U32(rec) | 0x80u);
            g.W32(q + 4u * g.U32(q + 0x92Cu) + 0x938u, rec);
            const int32_t nx = g.S32(q + 0x92Cu) + 1;
            g.W32(q + 0x92Cu, U(nx - ((nx < 0 ? nx + 63 : nx) >> 6) * 64));
            g.W32(q + 0x934u, g.U32(q + 0x934u) + 1u);
        }
    }
}

namespace {

// 0x80031D70: the oldest queued chunk, or 0.
uint32_t ResPop(GuestRam& g, uint32_t sp, RecoverCallees& c) {
    Call(c, kStCritEnterFn, {}, sp - 24u);
    const uint32_t q = Res(g);
    uint32_t rec = 0;
    if (g.U32(q + 0x934u) != 0) {
        rec = g.U32(q + 4u * g.U32(q + 0x930u) + 0x938u);
        g.W32(rec, g.U32(rec) & ~0x80u);
        const int32_t nx = g.S32(q + 0x930u) + 1;
        const int32_t t = nx < 0 ? g.S32(q + 0x930u) + 0x40 : nx;
        g.W32(q + 0x930u, U(nx - (t >> 6) * 64));
        g.W32(q + 0x934u, g.U32(q + 0x934u) - 1u);
    }
    Call(c, kStCritLeaveFn, {}, sp - 24u);
    return rec;
}

// 0x80031BF8(rec, p): the index of a loaded twin of `rec` (same key; for a type-1 texture, the same half),
// or -1.
int32_t ResDup(GuestRam& g, uint32_t rec, uint32_t p) {
    const uint32_t key = g.U32(g.U32(rec + 20u));
    uint32_t i = g.U32(Res(g) + 8u * p + 0xA40u);
    uint32_t o = Res(g) + 36u * i + 0x2Cu;
    for (;; ++i, o += 36u) {
        if (g.U32(Res(g) + 8u * p + 0xA44u) < i) return -1;
        if (rec != o && (g.U32(o) & 0x91u) == 0x11u && g.U32(g.U32(o + 20u)) == key &&
            ((key >> 28) != 1u || (g.U32(g.U32(o + 20u) + 28u) & 1u) == (g.U32(g.U32(rec + 20u) + 28u) & 1u)))
            return S(i);
    }
}

// 0x80031B98(a, b): 32 bytes swapped.
void Swap32(GuestRam& g, uint32_t a, uint32_t b) {
    for (uint32_t k = 0; k < 32u; ++k) {
        const uint8_t x = g.U8(a + k);
        g.W8(a + k, g.U8(b + k));
        g.W8(b + k, x);
    }
}

// 0x800319F8(rec, p): a texture half paired with its loaded twin: the two headers moved to the ends of
// the half with bit 0 set (+0x3F60 / +0x3FE0), both marked.
void ResPair(GuestRam& g, uint32_t rec, uint32_t p) {
    if ((g.U32(rec) & 0x20u) != 0) return;
    uint32_t i = g.U32(Res(g) + 8u * p + 0xA40u);
    uint32_t o = Res(g) + 36u * i + 0x2Cu;
    uint32_t twin = 0;
    for (; i <= g.U32(Res(g) + 8u * p + 0xA44u); ++i, o += 36u) {
        if (o == rec || (g.U32(o) & 0x91u) != 0x11u) continue;
        if (g.U32(g.U32(o + 20u)) == g.U32(g.U32(rec + 20u))) {
            twin = o;
            break;
        }
    }
    if (twin == 0) return;
    uint32_t a = rec, b = twin;
    if ((g.U32(g.U32(rec + 20u) + 28u) & 1u) != 0) {
        a = twin;
        b = rec;
    }
    Swap32(g, g.U32(a + 16u), g.U32(b + 16u) + 0x3F60u);
    Swap32(g, g.U32(b + 16u), g.U32(b + 16u) + 0x3FE0u);
    g.W32(a + 20u, g.U32(b + 16u) + 0x3F60u);
    g.W32(b + 20u, g.U32(b + 16u) + 0x3FE0u);
    g.W32(a, g.U32(a) | 0x20u);
    g.W32(b, g.U32(b) | 0x20u);
}

} // namespace

void ResPump(GuestRam& g, uint32_t sp, RecoverCallees& c) {
    const uint32_t csp = sp - 296u;
    const uint32_t local = csp + 16u;
    int32_t n = 0;
    for (;;) {
        const uint32_t rec = ResPop(g, csp, c);
        if (rec == 0) break;
        const int32_t twin = ResDup(g, rec, g.U8(rec + 34u));
        if (twin < 0) {
            g.W32(local + 4u * U(n), rec);
            ++n;
            continue;
        }
        const uint32_t bits = g.U32(g.U32(rec + 20u) + 28u) >> 1;
        const uint32_t arg = g.U32(rec + 32u);
        ResFree(g, rec, csp, c);
        const uint32_t o = Rec(g, U(twin));
        const uint32_t h = g.U32(o + 20u);
        g.W32(h + 28u, (g.U32(h + 28u) & 1u) | (bits << 1));
        g.W32(o + 32u, arg);
    }
    for (int32_t k = 0; k < n; ++k) {
        const uint32_t rec = g.U32(local + 4u * U(k));
        if ((g.U32(rec) & 0x20u) != 0) continue;
        const uint32_t type = g.U32(g.U32(rec + 20u)) >> 28;
        if (type == 1) ResPair(g, rec, g.U8(rec + 34u));
        else if (type == 2) { // 0x80031B4C: the header to the end of the chunk
            const uint32_t h = g.U32(rec + 16u) + 0x3FE0u;
            Swap32(g, g.U32(rec + 16u), h);
            g.W32(rec + 20u, h);
            g.W32(rec, g.U32(rec) | 0x20u);
        } else {
            g.W32(rec, g.U32(rec) | 0x20u);
        }
    }
    uint32_t rec = Res(g) + 0x2Cu;
    for (int32_t k = 0; k < g.S32(Res(g) + 0xA58u); ++k, rec += 36u)
        if ((g.U32(rec) & 0x22u) == 0x20u) ResLoad(g, rec, g.U8(rec + 34u), csp, c);
    // 0x800320CC(5) / 0x800320AC: counter reads, no effect
    if (g.U32(g.gp() + 0x24Cu) != 0) g.W32(g.gp() + 0x24Cu, 0);
}

void ResLoad(GuestRam& g, uint32_t rec, uint32_t p, uint32_t sp, RecoverCallees& c) {
    const uint32_t csp = sp - 56u;
    uint32_t id = 0, type = 0, v = 0;
    const uint32_t pay = g.U32(rec + 16u);
    if ((g.U32(rec) & 8u) != 0) {
        Call(c, kStMusicDoneFn, {g.U32(rec + 4u), pay}, csp);
        v = 1;
    } else {
        const uint32_t h = g.U32(rec + 20u);
        const uint32_t ext = g.U32(rec + 24u);
        const uint32_t key = g.U32(h);
        type = key >> 28;
        id = key & 0x0FFFFFFFu;
        const uint32_t w7 = (g.U32(h + 28u) & 1u) | ((g.U32(h + 28u) >> 1) << 1); // 0x80023860, the identity
        g.W32(h + 28u, w7);
        switch (type) {
        case 0:
        case 8:
            ResEntryTag(g, g.U32(Cur(g) + 0x48u), rec, p);
            v = CellLoad(g, pay, id, type, ext, rec + 28u, p, csp, c);
            break;
        case 9:
            v = CellLoad(g, pay, id, type, 0, 0, p, csp, c);
            break;
        case 1:
            if ((g.U32(rec) & 0x20u) == 0) { v = 0; break; }
            // 0x80032CC8(buf, id, half, w7, windows, p)
            v = TexLoad(g, ((id >> 16) & 0x8000u) | ((id >> 13) & 0x7C00u) | (id & 0x3FFu), pay, 0, (w7 & 1u) != 0 ? 1u : 0u,
                        w7, h + 4u, p, csp - 40u, c);
            break;
        case 2:
            if ((g.U32(rec) & 0x20u) == 0) { v = 0; break; }
            // 0x80032C6C(buf, id, w7, windows, p)
            v = TexLoad(g, ((id >> 16) & 0x8000u) | ((id >> 13) & 0x7C00u) | (id & 0x3FFu) | 0x8000u, pay, 1, 0, w7,
                        h + 4u, p, csp - 40u, c);
            break;
        case 3:
            v = PieceLoad(g, pay, id & 0xFFFFu, p, csp, c);
            if (v == 3) v = 1;
            break;
        case 4:
            v = 2;
            if (Players(g) == 1) v = PanoLoad(g, pay, h + 4u, g.U32(h));
            break;
        case 10:
            if (!Call(c, kStSpeechLoadFn, {pay, id, g.U32(rec + 4u)}, csp, &v)) v = 0;
            break;
        default:
            v = 2;
            break;
        }
    }
    if (v == 1) {
        const uint32_t f = g.U32(rec);
        if (f == 0) return;
        g.W32(rec, f | 2u);
        if (((f | 2u) & 0x10u) == 0) return;
        StreamEntry(g, g.U32(rec + 20u), p);
        if (type != 0 && type != 8) return;
        const uint32_t start = g.U32(kStStartRec);
        if (id == g.U32(start + 16u)) {
            g.W32(kStFinishCell, start);
            return;
        }
        const uint32_t fin = g.U32(kStFinishRec);
        if (id == g.U32(fin + 16u)) g.W32(kStFinishCell, fin);
    } else if (v == 2 || v == 3) {
        ResFree(g, rec, csp, c);
    }
}

void ResUnload(GuestRam& g, uint32_t rec, uint32_t p, uint32_t sp, RecoverCallees& c) {
    const uint32_t csp = sp - 32u;
    const uint32_t h = g.U32(rec + 20u);
    const uint32_t key = g.U32(h);
    const uint32_t id = key & 0x0FFFFFFFu;
    uint32_t v = 1;
    if ((g.U32(rec) & 2u) != 0) {
        switch (key >> 28) {
        case 0:
        case 8:
        case 9: CellUnload(g, id, key >> 28, p, csp, c); v = 1; break;
        case 1: TexFree(g, ((id >> 16) & 0x8000u) | ((id >> 13) & 0x7C00u) | (id & 0x3FFu), 0, p, csp - 24u, c); v = 1; break;
        case 2: TexFree(g, ((id >> 13) & 0x7C00u) | 0x8000u | (id & 0x3FFu), 1, p, csp - 24u, c); v = 1; break;
        case 3: v = PieceUnload(g, key & 0xFFFFu, p, csp, c); break;
        case 4: v = PanoFree(g, g.U32(rec + 16u)); break;
        default: v = 1; break;
        }
    }
    if (v != 1) return;
    // (flags & 0x10): 0x80023868, a no-op
    ResFree(g, rec, csp, c);
}

void ResEntryTag(GuestRam& g, uint32_t road, uint32_t rec, uint32_t p) {
    uint32_t i = 0, last = 0;
    if (rec == 0) {
        i = g.U32(Res(g) + 8u * p + 0xA40u);
        last = g.U32(Res(g) + 8u * p + 0xA44u);
        rec = Res(g) + 36u * i + 0x2Cu;
    }
    const uint32_t roadRec = RoadRec(g, road);
    for (; !(last < i); ++i, rec += 36u) {
        if ((g.U32(rec) & 0x11u) != 0x11u) continue;
        const uint32_t type = g.U32(g.U32(rec + 20u)) >> 28;
        if (type != 0 && type != 8) continue;
        uint32_t ext = g.U32(rec + 24u);
        uint32_t k = 0;
        for (; k < 4u; ++k, ext += 12u)
            if (g.U32(ext) == road) break;
        uint32_t out = 0;
        if (k < 4u) {
            out = U(g.S32(ext + 4u) >> 6);
        } else {
            ext = g.U32(rec + 24u);
            const uint32_t r0 = g.U32(ext);
            const uint32_t r0Rec = RoadRec(g, r0);
            out = 0xFFFF0000u;
            bool found = false;
            for (int side = 0; side < 2 && !found; ++side) {
                const uint32_t node = NodeRec(g, g.U32(roadRec + (side == 0 ? 8u : 12u)));
                uint32_t j = 0;
                uint32_t at = node;
                if (g.U32(node + 4u) != 0) {
                    for (; j < g.U32(node + 4u); ++j, at += 8u)
                        if (g.U32(at + 8u) == r0) break;
                    if (j < g.U32(node + 4u)) {
                        const int32_t d = g.S32(node + 8u * j + 12u) < 1 ? g.S32(r0Rec + 4u) - g.S32(ext + 8u) : g.S32(ext + 4u);
                        out = side == 0 ? U(-(d >> 6)) : U((d >> 6) + 0x10000);
                        found = true;
                    }
                }
            }
        }
        g.W32(rec + 28u, out);
    }
}

void StreamEntry(GuestRam& g, uint32_t w, uint32_t p) {
    const uint32_t st = kStStates + kStStateBytes * p;
    int32_t k = 0;
    uint32_t e = w;
    for (; k < 4; ++k, e += 6u)
        if (g.S16(e + 4u) == g.S32(st + 0x48u)) break;
    if (k >= 4) return;
    const int32_t v = g.S32(st + 0x40u) < 1 ? g.S16(w + 6u * U(k) + 8u) : g.S16(w + 6u * U(k) + 6u);
    g.W32(st + 0x68u, U(v));
    g.W32(st + 0x64u, U(g.S32(st + 0x40u) < 1 ? g.S32(st + 0x44u) - v : v - g.S32(st + 0x44u)));
}

int32_t StreamStall(GuestRam& g) {
    if (g.U32(g.gp() + kGpStCdBusy) == 0) return 0;
    int32_t d = g.S32(Gs(g) + 12u) - g.S32(g.gp() + kGpStCdStamp);
    if (d < 0) d = -d;
    if (d > 0xF0) return d < 0xBB9 ? 1 : 2;
    return 0;
}

uint32_t StreamStallCells(GuestRam& g, uint32_t sp, RecoverCallees& c) {
    const uint32_t csp = sp - 32u;
    uint32_t a = 0, b = 0;
    Call(c, kStEntityCellFn, {g.U32(kStPlayer1)}, csp, &a);
    Call(c, kStEntityCellFn, {kStViews}, csp, &b);
    uint32_t v = a | b;
    if (Players(g) == 2) {
        Call(c, kStEntityCellFn, {g.U32(kStPlayer2)}, csp, &a);
        Call(c, kStEntityCellFn, {kStViews + 1132u}, csp, &b);
        v |= a | b;
    }
    return v >> 31;
}

void StreamFrame(GuestRam& g, uint32_t sp, RecoverCallees& c) {
    const uint32_t csp = sp - 24u;
    StreamPlayers(g, csp, c);
    ResPump(g, csp, c);
    if (Players(g) == 1) Call(c, kStMusicFrameFn, {}, csp);
    const int32_t stall = StreamStall(g);
    if (stall != 0 || g.U8(Gs(g)) == 4u) {
        const uint32_t cells = StreamStallCells(g, csp, c);
        g.W16(Gs(g) + 0x28u, static_cast<uint16_t>((cells != 0 ? 2u : 0u) | (stall == 2 ? 1u : 0u)));
    }
}

} // namespace rr::sim
