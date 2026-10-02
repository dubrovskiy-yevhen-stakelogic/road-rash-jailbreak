// Bench rows of the route block parser and the road-map loader of RoadLoad (src\game\sim\route_parse.
// {h,cpp}), RASHCDI SHA-1 9a8b79d8739713c12b0a2dc4d75ef8f0a2cc0b06 and SLUS_010.53,
// and the host's kernel string calls against SCPH1001's own. Included LAST by verify_physics.cpp inside its anonymous
// namespace (rows_loader.inc's ldr:: and rows_loader2.inc's ldr2:: are visible).
//
// The machine is a race capture (rr-race by default): its gp+472 holds STREAM1.GRF relocated, the race graph LinkFill
// reads. Every case lays the WHOLE RASHCDI image over 0x8005B5E8 first (the keys and formats are its data).
//
// INPUTS: the player's own DATA\ROADGRF1/2.TXT and ROAD1/2.MAP (work\disc_us) planted in the stream records' buffers
// 28..31 (ldr::Area), whole or a window of race blocks; synthetic texts built from the file's own keys with random
// numbers, blanks, line ends (\r\n, \n, \r) and junk bytes; random byte strings for the leaves.
//
// NAMED BOUNDS (the same on both machines):
//   * the kernel's string calls run the kernel's own code on both sides (allowBiosCalls); the host's Bios* are
//     benched on their own rows `rp_bios_*` against the kernel, where atoi's input bytes are < 0x80 (the kernel's
//     ctype table's upper half is not ctype data; the host refuses such a byte);
//   * texts end in a NUL inside the planted area; RouteParse's texts end their lines in CR LF (the console's do); a record's roads are -1..63 (LinkFill's graph index stays in
//     RAM); [RACEINTS] >= 0 where a [START] follows (the console's texts: -1 puts the start record at 0xFFFFFF88);
//   * LoadFile of MapLoad is the planted copy with v0 = its length (0x80014B40's return), never -1 (a `break`);
//   * the map's block chain has no zero-sized block (the walk would not return).
namespace rte {

namespace s = rr::sim;
using ldr::Area;
using ldr::DiscFile;
using ldr::Gs;
using ldr::Plant;
using pop::Chance;
using pop::Put8;

void PutBytes(Memory& m, uint32_t at, const std::string& str) {
    if (!str.empty()) m.WriteBlock(at, str.data(), static_cast<uint32_t>(str.size()));
    Put8(m, at + static_cast<uint32_t>(str.size()), 0);
}

std::string Int(Rng& r) {
    static const int32_t kVals[] = {-1, 0, 1, 7, 10, 64, 120, 945995209, 2147483647, -4096, 33554538};
    if (Chance(r, 3)) return std::to_string(kVals[r.U32() % (sizeof(kVals) / sizeof(kVals[0]))]);
    return std::to_string(static_cast<int32_t>(r.U32() % 20000u) - 100);
}
std::string Road(Rng& r) { return Chance(r, 3) ? "-1" : std::to_string(r.U32() % 64u); }
std::string Blank(Rng& r) {
    static const char* kB[] = {"", "", "", " ", "\t", "  ", " \t"};
    return kB[r.U32() % 7u];
}
bool g_crlf = false; // RouteParse's texts: CR LF only (a lone LF / CR makes ReadLine's caller skip the next line's first
                     // byte - "[START]" read as a record puts a number into a road field, LinkFill then leaves RAM)
std::string Eol(Rng& r) {
    if (g_crlf) return "\r\n";
    const uint32_t k = r.U32() % 10u;
    return k < 7u ? "\r\n" : (k < 9u ? "\n" : "\r");
}
// A numeric-ish string: blanks, signs, a 0 / 0x / 0b prefix, digits, hex letters, a tail (bytes < 0x80).
std::string Number(Rng& r) {
    std::string s;
    static const char* kLead[] = {"", "", " ", "\t", "\n", "\v", "\f", "\r", "  "};
    s += kLead[r.U32() % 9u];
    const uint32_t k = r.U32() % 12u;
    if (k == 0u) s += "-";
    else if (k == 1u) s += "--";
    else if (k == 2u) s += "+";
    else if (k == 3u) s += "---";
    static const char* kPre[] = {"", "", "", "", "0", "0x", "0X", "0b", "0B", "00"};
    s += kPre[r.U32() % 10u];
    static const char kDig[] = "0123456789abcdefABCDEFxz";
    const uint32_t n = r.U32() % 12u;
    for (uint32_t i = 0; i < n; ++i) s += kDig[Chance(r, 4) ? r.U32() % 24u : r.U32() % 10u];
    static const char* kTail[] = {"", " 5", "=3", "\r\n", " -1", "x", " 0"};
    s += kTail[r.U32() % 7u];
    return s;
}
// Random bytes (optionally ASCII only), NUL-free.
std::string Junk(Rng& r, uint32_t n, bool ascii) {
    std::string s;
    static const char kA[] = " \t\r\n=-0123456789[]ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcxyz";
    for (uint32_t i = 0; i < n; ++i) {
        if (!ascii && Chance(r, 8)) s += static_cast<char>(0x80u + r.U32() % 0x80u);
        else if (Chance(r, 6)) s += static_cast<char>(1u + r.U32() % 0x7Fu);
        else s += kA[r.U32() % (sizeof(kA) - 1u)];
    }
    return s;
}

// A synthetic route text: [NUM_ENTRIES], race blocks with the file's keys, records after [RACEINTS], junk lines.
std::string Synth(Rng& r, int blocks, int& firstRace) {
    std::string t;
    if (!Chance(r, 8)) t += Blank(r) + "[NUM_ENTRIES]=" + std::to_string(blocks + static_cast<int>(r.U32() % 3u)) + Eol(r);
    firstRace = static_cast<int>(1u + r.U32() % 70u);
    for (int b = 0; b < blocks; ++b) {
        const int raceInts = static_cast<int>(r.U32() % 6u);
        t += "[BEGIN]" + Eol(r) + Eol(r);
        t += "[RACEID]=" + std::to_string(firstRace + b) + Eol(r);
        std::vector<std::string> lines = {
            "[START]=" + Road(r) + " " + Int(r) + " " + Int(r) + " " + Int(r),
            "[FINISH]=" + Road(r) + " " + Int(r) + " " + Int(r) + " " + Int(r),
            "[GMAGIC]=" + Int(r),
            "[RMAGIC]=" + Int(r),
            "[START_CHECKER]=" + Int(r) + " " + Int(r) + " " + Int(r),
            "[FINISH_CHECKER]=" + Int(r) + " " + Int(r) + " " + Int(r),
            "[VEHICLE_DEFAULT_DENSITY]=" + Int(r),
            "[VEHICLE_DEFAULT_RATE]=" + Int(r),
            "[REACTIVE_DEFAULT_DENSITY]=" + Int(r),
            "[REACTIVE_DEFAULT_RATE]=" + Int(r),
        };
        for (const std::string& l : lines)
            if (!Chance(r, 6)) t += Blank(r) + l + Eol(r) + (Chance(r, 3) ? Eol(r) : "");
        if (Chance(r, 5)) t += Junk(r, r.U32() % 60u, false) + Eol(r);
        t += "[RACEINTS]=" + std::to_string(raceInts) + Eol(r);
        const int rows = raceInts + static_cast<int>(r.U32() % 3u) - 1;
        for (int k = 0; k < rows; ++k) {
            std::string l = Chance(r, 10) ? "-1" : std::to_string(r.U32() % 60u);
            l += " " + Int(r) + " " + Int(r);
            for (int j = 0; j < 4; ++j) l += " " + Road(r) + " " + (Chance(r, 2) ? "1" : "-1");
            for (int j = 0; j < 8; ++j) l += " " + (Chance(r, 2) ? std::string("-1") : std::to_string(r.U32() % 60u));
            t += (Chance(r, 4) ? "	" : "") + l + Eol(r); // a leading ' ' would shift the record's fields (NAMED BOUND)
        }
        t += Eol(r) + "[END]" + Eol(r);
    }
    return t;
}

// The player's route text, whole (sample) or a window of its blocks after its [NUM_ENTRIES] line.
std::string RealText(Rng& r, bool whole, int set, int& raceHint) {
    const std::vector<uint8_t>& f = DiscFile(set == 1 ? "DATA\\ROADGRF1.TXT" : "DATA\\ROADGRF2.TXT");
    std::string t(f.begin(), f.end());
    raceHint = static_cast<int>(1u + r.U32() % (set == 1 ? 64u : 55u));
    if (whole || t.empty()) return t;
    const size_t head = t.find("[BEGIN]");
    if (head == std::string::npos) return t;
    size_t from = head;
    const uint32_t skip = r.U32() % 60u;
    for (uint32_t k = 0; k < skip; ++k) {
        const size_t nx = t.find("[BEGIN]", from + 1);
        if (nx == std::string::npos) break;
        from = nx;
    }
    size_t to = from;
    const uint32_t take = 1u + r.U32() % 4u;
    for (uint32_t k = 0; k < take; ++k) {
        const size_t nx = t.find("[BEGIN]", to + 1);
        to = nx == std::string::npos ? t.size() : nx;
        if (nx == std::string::npos) break;
    }
    // the race id of the window's first block
    const size_t id = t.find("[RACEID]=", from);
    if (id != std::string::npos) raceHint = std::atoi(t.c_str() + id + 9);
    return t.substr(0, head) + t.substr(from, to - from);
}

// Plants a text for the text-walking rows; a[0] = text, a[1] = its length (or a variant); the race hint for FindRace.
uint32_t PlantText(CaseContext& c, Args& a, int& race) {
    Memory& m = *c.mem;
    Rng& r = *c.rng;
    std::string t;
    if (c.sample) {
        t = RealText(r, true, 1, race);
        race = 4;
    } else if (Chance(r, 2)) {
        t = RealText(r, Chance(r, 6), Chance(r, 2) ? 1 : 2, race);
    } else {
        t = Synth(r, 1 + static_cast<int>(r.U32() % 4u), race);
        race += static_cast<int>(r.U32() % 4u);
    }
    if (t.size() > ldr::kAreaBytes - 16u) t.resize(ldr::kAreaBytes - 16u);
    PutBytes(m, Area(m), t);
    a.a[0] = Area(m);
    uint32_t len = static_cast<uint32_t>(t.size());
    if (!c.sample) {
        const uint32_t k = r.U32() % 10u;
        if (k == 0u) len = r.U32() % (len + 1u);
        else if (k == 1u) len = 0;
        else if (k == 2u) len = static_cast<uint32_t>(-static_cast<int32_t>(r.U32() % 100u));
        else if (k == 3u) len += r.U32() % 64u;
    }
    a.a[1] = len;
    return len;
}

Row Make(const char* name, const char* sig, uint32_t entry, std::function<void(CaseContext&, Args&)> setup,
         std::function<uint32_t(NativeEnv&, const Args&)> native, bool v0,
         std::initializer_list<std::pair<uint32_t, int>> callees, uint64_t steps = 40'000'000) {
    std::vector<OracleCallee> k;
    for (const auto& [fn, n] : callees) {
        OracleCallee o{fn, n};
        if (fn == s::kLdLoadFile || fn == s::kRpFindRaceFn) o.stackArgs = 1;
        k.push_back(o);
    }
    Row r = ldr::Make(name, sig, entry, std::move(setup), std::move(native), v0, std::move(k));
    r.allowBiosCalls = true;
    r.maxSteps = steps;
    return r;
}

const std::initializer_list<std::pair<uint32_t, int>> kLineKids = {
    {s::kRpReadLineFn, 3}, {s::kRpNextTokenFn, 2}, {s::kRpStrNCmp, 3}, {s::kRpStrChr, 2},
    {s::kRpParseIntFn, 2}, {s::kRpStrLen, 1},      {s::kRpTokenIntFn, 4}};

} // namespace rte

void AddRouteRows(std::vector<Row>& rows) {
    namespace s = rr::sim;
    using ldr::Run;
    using rte::Make;
    using V = ldr::Seams;
    using ldr::Area;
    using ldr::DiscFile;
    using ldr::Gs;
    using ldr::Plant;
    using pop::Chance;
    using pop::Put8;
    // ---- the leaves of the text walk
    rows.push_back(Make("rp_read_line", "char *ReadLine(char *dst, s32 max, char *src)  [RASHCDI 0x80064084]", s::kRpReadLineFn,
                        [](CaseContext& c, Args& a) {
                            gridr::InjectOverlay(c);
                            Memory& m = *c.mem;
                            Rng& r = *c.rng;
                            a.a[0] = c.scratch;
                            a.a[1] = (c.sample || Chance(r, 2)) ? 127u : static_cast<uint32_t>(static_cast<int32_t>(r.U32() % 300u) - 3);
                            if (c.sample || Chance(r, 3)) {
                                int race = 0;
                                const std::string t = rte::RealText(r, true, Chance(r, 2) ? 1 : 2, race);
                                rte::PutBytes(m, Area(m), t);
                                a.a[2] = Area(m) + (c.sample ? 0u : r.U32() % static_cast<uint32_t>(t.size() + 1u));
                            } else {
                                std::string t = rte::Junk(r, r.U32() % 400u, false);
                                if (Chance(r, 3)) t = rte::Eol(r) + t;
                                rte::PutBytes(m, c.scratch + 1024u, t);
                                a.a[2] = Chance(r, 12) ? 0u : c.scratch + 1024u;
                            }
                        },
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "ReadLine", [&](s::GuestRam& g, V& c, uint32_t& v0) {
                                return s::ReadLine(g, a.a[0], a.a[1], a.a[2], env.sp, c, v0);
                            });
                        }, true, {{s::kRpStrNCpy, 3}}));
    rows.push_back(Make("rp_next_token", "char *NextToken(char *src, char *dst)  [RASHCDI 0x800643B4]", s::kRpNextTokenFn,
                        [](CaseContext& c, Args& a) {
                            gridr::InjectOverlay(c);
                            Memory& m = *c.mem;
                            Rng& r = *c.rng;
                            std::string t = c.sample ? std::string("[NUM_ENTRIES]=64\r\n") : rte::Junk(r, r.U32() % 300u, false);
                            if (!c.sample && Chance(r, 2)) t = rte::Blank(r) + (Chance(r, 2) ? "\r" : "") + t;
                            rte::PutBytes(m, c.scratch + 1024u, t);
                            a.a[0] = (!c.sample && Chance(r, 12)) ? 0u : c.scratch + 1024u;
                            a.a[1] = c.scratch;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "NextToken", [&](s::GuestRam& g, V& c, uint32_t& v0) {
                                return s::NextToken(g, a.a[0], a.a[1], env.sp, c, v0);
                            });
                        }, true, {{s::kRpStrNCpy, 3}}));
    rows.push_back(Make("rp_parse_int", "char *ParseInt(char *s, s32 *out)  [RASHCDI 0x80064144]", s::kRpParseIntFn,
                        [](CaseContext& c, Args& a) {
                            gridr::InjectOverlay(c);
                            Rng& r = *c.rng;
                            rte::PutBytes(*c.mem, c.scratch + 1024u, c.sample ? std::string("840 1 0\r\n") : rte::Number(r));
                            a.a[0] = c.scratch + 1024u;
                            a.a[1] = c.scratch + 16u;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "ParseInt", [&](s::GuestRam& g, V& c, uint32_t& v0) {
                                return s::ParseInt(g, a.a[0], a.a[1], env.sp, c, v0);
                            });
                        }, true, {{s::kRpAtoi, 1}, {s::kRpStrChr, 2}}));
    rows.push_back(Make("rp_token_int", "char *TokenInt(char *prev, char *line, char sep, s32 *out)  [RASHCDI 0x80069B10]",
                        s::kRpTokenIntFn,
                        [](CaseContext& c, Args& a) {
                            gridr::InjectOverlay(c);
                            Rng& r = *c.rng;
                            std::string t = c.sample ? std::string("[START]=3 840 1 0\r\n") : rte::Number(r);
                            if (!c.sample && Chance(r, 2)) t = rte::Junk(r, r.U32() % 12u, true) + (Chance(r, 2) ? "=" : " ") + t;
                            rte::PutBytes(*c.mem, c.scratch + 1024u, t);
                            a.a[0] = (!c.sample && Chance(r, 6)) ? 0u : (Chance(r, 2) ? c.scratch + 1030u : r.U32() | 1u);
                            a.a[1] = (!c.sample && Chance(r, 10)) ? 0u : c.scratch + 1024u;
                            static const uint32_t kSep[] = {61, 32, 0, 61, 32, 0x3D3Du, 0x120u};
                            a.a[2] = c.sample ? 61u : (Chance(r, 8) ? r.U32() : kSep[r.U32() % 7u]);
                            a.a[3] = (!c.sample && Chance(r, 24)) ? 0u : c.scratch + 16u;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "TokenInt", [&](s::GuestRam& g, V& c, uint32_t& v0) {
                                return s::TokenInt(g, a.a[0], a.a[1], a.a[2], a.a[3], env.sp, c, v0);
                            });
                        }, true, {{s::kRpStrChr, 2}, {s::kRpParseIntFn, 2}}));
    // ---- the text's walkers
    rows.push_back(Make("rp_num_entries", "s32 NumEntries(char *text, s32 len)  [RASHCDI 0x80069B8C]", s::kRpNumEntriesFn,
                        [](CaseContext& c, Args& a) {
                            gridr::InjectOverlay(c);
                            int race = 0;
                            rte::PlantText(c, a, race);
                        },
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "NumEntries", [&](s::GuestRam& g, V& c, uint32_t& v0) {
                                return s::NumEntries(g, a.a[0], a.a[1], env.sp, c, v0);
                            });
                        }, true, rte::kLineKids, 200'000'000));
    rows.push_back(Make("rp_find_race", "s32 FindRace(char *text, s32 len, s32 race, char **start, s32 *len)  [RASHCDI 0x80069C60]",
                        s::kRpFindRaceFn,
                        [](CaseContext& c, Args& a) {
                            gridr::InjectOverlay(c);
                            Rng& r = *c.rng;
                            int race = 0;
                            rte::PlantText(c, a, race);
                            a.a[2] = c.sample ? 4u : (Chance(r, 6) ? r.U32() % 200u : static_cast<uint32_t>(race));
                            for (uint32_t k = 0; k < 16u; k += 4) c.mem->PokeWord(c.scratch + k, r.U32());
                            a.a[3] = c.scratch + 8u;
                            a.a4 = c.scratch + 12u;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "FindRace", [&](s::GuestRam& g, V& c, uint32_t& v0) {
                                return s::FindRace(g, a.a[0], a.a[1], a.a[2], a.a[3], env.sp, c, v0);
                            });
                        }, true, rte::kLineKids, 200'000'000));
    rows.push_back(Make("rp_header", "void RouteHeader(char *text, s32 len)  [RASHCDI 0x80069DEC]", s::kRpHeaderFn,
                        [](CaseContext& c, Args& a) {
                            gridr::InjectOverlay(c);
                            Rng& r = *c.rng;
                            int race = 0;
                            rte::PlantText(c, a, race);
                            c.mem->PokeWord(Gs(*c.mem) + 64u, c.sample ? 4u : (Chance(r, 6) ? r.U32() % 200u : static_cast<uint32_t>(race)));
                            if (c.sample) return;
                            for (uint32_t k = 0; k < 40u; k += 4) c.mem->PokeWord(s::kRpHeader + k, r.U32());
                        },
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "RouteHeader", [&](s::GuestRam& g, V& c, uint32_t&) {
                                return s::RouteHeader(g, a.a[0], a.a[1], env.sp, c);
                            });
                        }, false,
                        {{s::kLdMemset, 3}, {s::kRpNumEntriesFn, 2}, {s::kRpFindRaceFn, 4}, {s::kRpReadLineFn, 3},
                         {s::kRpNextTokenFn, 2}, {s::kRpStrNCmp, 3}, {s::kRpTokenIntFn, 4}, {s::kRpStrLen, 1}},
                        400'000'000));
    rows.push_back(Make("rp_link_fill", "void LinkFill(s32 *link)  [RASHCDI 0x8006A014]", s::kRpLinkFillFn,
                        [](CaseContext& c, Args& a) {
                            gridr::InjectOverlay(c);
                            Memory& m = *c.mem;
                            Rng& r = *c.rng;
                            const uint32_t link = c.scratch + 64u;
                            for (uint32_t k = 0; k < 16u; k += 4) m.PokeWord(link + k, r.U32());
                            m.PokeWord(link, c.sample ? 3u : (Chance(r, 5) ? 0xFFFFFFFFu : r.U32() % 64u));
                            WriteU16Bench(m, 0x800D6182u, c.sample ? 4u : (Chance(r, 5) ? 0xFFFFu : static_cast<uint16_t>(r.U32())));
                            a.a[0] = link;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "LinkFill", [&](s::GuestRam& g, V& c, uint32_t&) { return s::LinkFill(g, a.a[0], env.sp, c); });
                        }, false, {}));
    rows.push_back(Make("rp_parse", "void RouteParse(char *text, s32 len)  [RASHCDI 0x8006A0C8]", s::kRpParseFn,
                        [](CaseContext& c, Args& a) {
                            gridr::InjectOverlay(c);
                            Rng& r = *c.rng;
                            int race = 0;
                            rte::g_crlf = true;                                    // NAMED BOUND (the console's texts)
                            rte::PlantText(c, a, race);
                            rte::g_crlf = false;
                            if (!c.sample && Chance(r, 16)) a.a[0] = 0;
                            c.mem->PokeWord(Gs(*c.mem) + 64u, c.sample ? 4u : (Chance(r, 8) ? r.U32() % 200u : static_cast<uint32_t>(race)));
                            if (c.sample) return;
                            for (uint32_t k = 0; k < 40u; k += 4) c.mem->PokeWord(s::kRpHeader + k, r.U32());
                        },
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "RouteParse", [&](s::GuestRam& g, V& c, uint32_t&) {
                                return s::RouteParse(g, a.a[0], a.a[1], env.sp, c);
                            });
                        }, false,
                        {{s::kRpHeaderFn, 2}, {s::kLdMalloc, 2}, {s::kLdMemset, 3}, {s::kRpReadLineFn, 3},
                         {s::kRpNextTokenFn, 2}, {s::kRpStrNCmp, 3}, {s::kRpTokenIntFn, 4}, {s::kRpLinkFillFn, 1},
                         {s::kRpStrLen, 1}},
                        400'000'000));
    // ---- the road map
    rows.push_back(Make("rp_map_blocks", "G *MapBlocks(u8 *map, s32 len)  [RASHCDI 0x8006A9F4]", s::kRpMapBlocksFn,
                        [](CaseContext& c, Args& a) {
                            gridr::InjectOverlay(c);
                            Memory& m = *c.mem;
                            Rng& r = *c.rng;
                            const bool two = !c.sample && Chance(r, 2);
                            const std::vector<uint8_t>& f = DiscFile(two ? "DATA\\ROAD2.MAP" : "DATA\\ROAD1.MAP");
                            const uint32_t len = Plant(m, f, Area(m), ldr::kAreaBytes);
                            a.a[0] = Area(m);
                            a.a[1] = len;
                            if (c.sample) return;
                            // the chain's block offsets; a tag corrupted (that table absent), a shorter length
                            std::vector<uint32_t> blocks;
                            for (uint32_t o = 0; o + 8u <= len && blocks.size() < 64u;) {
                                blocks.push_back(o);
                                const uint32_t sz = m.PeekWord(Area(m) + o + 4u);
                                if (sz == 0u) break;
                                o += sz;
                            }
                            if (!blocks.empty() && Chance(r, 3)) Put8(m, Area(m) + blocks[r.U32() % blocks.size()] + (r.U32() % 4u), r.U32());
                            if (!blocks.empty() && Chance(r, 4)) a.a[1] = blocks[r.U32() % blocks.size()] + r.U32() % 16u;
                            if (Chance(r, 8)) a.a[1] = 0;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "MapBlocks", [&](s::GuestRam& g, V& c, uint32_t& v0) {
                                return s::MapBlocks(g, a.a[0], a.a[1], env.sp, c, v0);
                            });
                        }, true, {{s::kRpStrNCmp, 3}}));
    rows.push_back(Make("rp_map_load", "s32 MapLoad(char *name)  [RASHCDI 0x8006ABC8]", s::kRpMapLoadFn,
                        [](CaseContext& c, Args& a) {
                            gridr::InjectOverlay(c);
                            Memory& m = *c.mem;
                            Rng& r = *c.rng;
                            const bool two = !c.sample && Chance(r, 2);
                            const std::vector<uint8_t>& f = DiscFile(two ? "DATA\\ROAD2.MAP" : "DATA\\ROAD1.MAP");
                            const uint32_t len = Plant(m, f, Area(m), ldr::kAreaBytes);
                            ldr2::StubLoadFileLen(m, Area(m), c.sample ? len : (Chance(r, 4) ? r.U32() % (len + 1u) : len), false);
                            a.a[0] = 0x80052400u;                                  // "DATA\ROAD"
                            if (c.sample) return;
                            m.PokeWord(Gs(m) + 48u, Chance(r, 2) ? 2u : r.U32() % 4u);
                            m.PokeWord(0x8005AD5Cu, r.U32() % 4u);
                            for (uint32_t w : {s::kRpMapFile, s::kRpMapGraph}) m.PokeWord(w, r.U32());
                            if (Chance(r, 3)) {
                                rte::PutBytes(m, c.scratch + 256u, rte::Junk(r, r.U32() % 40u, true));
                                a.a[0] = c.scratch + 256u;
                            }
                        },
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "MapLoad", [&](s::GuestRam& g, V& c, uint32_t& v0) {
                                return s::MapLoad(g, a.a[0], env.sp, c, v0);
                            });
                        }, true, {{s::kLdSprintf, 4}, {s::kLdLoadFile, 4}, {s::kRpMapBlocksFn, 2}}));
    // ---- the host's kernel string calls against the kernel's own (the entry is the SLUS stub into the A0 table)
    rows.push_back(Make("rp_bios_strncmp", "s32 strncmp(char *a, char *b, s32 n)  [SLUS 0x80044874 -> A(18h)]", s::kRpStrNCmp,
                        [](CaseContext& c, Args& a) {
                            Memory& m = *c.mem;
                            Rng& r = *c.rng;
                            std::string x = c.sample ? std::string("[START]") : rte::Junk(r, r.U32() % 24u, false);
                            std::string y = x;
                            if (!c.sample) {
                                if (Chance(r, 2)) y = y.substr(0, r.U32() % (y.size() + 1u)) + rte::Junk(r, r.U32() % 8u, false);
                                if (Chance(r, 4)) x += rte::Junk(r, r.U32() % 4u, false);
                            }
                            rte::PutBytes(m, c.scratch, x);
                            rte::PutBytes(m, c.scratch + 512u, y);
                            a.a[0] = (!c.sample && Chance(r, 12)) ? 0u : c.scratch;
                            a.a[1] = (!c.sample && Chance(r, 12)) ? 0u : c.scratch + 512u;
                            a.a[2] = c.sample ? 7u : (Chance(r, 10) ? r.U32() : static_cast<uint32_t>(static_cast<int32_t>(r.U32() % 40u) - 3));
                        },
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "strncmp", [&](s::GuestRam& g, V&, uint32_t& v0) {
                                v0 = s::BiosStrNCmp(g, a.a[0], a.a[1], a.a[2]);
                                return true;
                            });
                        }, true, {}));
    rows.push_back(Make("rp_bios_strlen", "s32 strlen(char *a)  [SLUS 0x800448F4 -> A(1Bh)]", s::kRpStrLen,
                        [](CaseContext& c, Args& a) {
                            Rng& r = *c.rng;
                            rte::PutBytes(*c.mem, c.scratch, c.sample ? std::string("[BEGIN]\r\n") : rte::Junk(r, r.U32() % 600u, false));
                            a.a[0] = (!c.sample && Chance(r, 12)) ? 0u : c.scratch;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "strlen", [&](s::GuestRam& g, V&, uint32_t& v0) {
                                v0 = s::BiosStrLen(g, a.a[0]);
                                return true;
                            });
                        }, true, {}));
    rows.push_back(Make("rp_bios_strncpy", "char *strncpy(char *d, char *s, s32 n)  [SLUS 0x80044914 -> A(1Ah)]", s::kRpStrNCpy,
                        [](CaseContext& c, Args& a) {
                            Memory& m = *c.mem;
                            Rng& r = *c.rng;
                            for (uint32_t k = 0; k < 512u; k += 4) m.PokeWord(c.scratch + k, r.U32());
                            rte::PutBytes(m, c.scratch + 2048u, c.sample ? std::string("[RACEID]=4") : rte::Junk(r, r.U32() % 300u, false));
                            a.a[0] = (!c.sample && Chance(r, 12)) ? 0u : c.scratch;
                            a.a[1] = (!c.sample && Chance(r, 12)) ? 0u : c.scratch + 2048u;
                            a.a[2] = c.sample ? 20u : static_cast<uint32_t>(static_cast<int32_t>(r.U32() % 400u) - 3);
                        },
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "strncpy", [&](s::GuestRam& g, V&, uint32_t& v0) {
                                v0 = s::BiosStrNCpy(g, a.a[0], a.a[1], a.a[2]);
                                return true;
                            });
                        }, true, {}));
    rows.push_back(Make("rp_bios_strchr", "char *strchr(char *a, s32 ch)  [SLUS 0x80044924 -> A(1Eh)]", s::kRpStrChr,
                        [](CaseContext& c, Args& a) {
                            Rng& r = *c.rng;
                            const std::string t = c.sample ? std::string("[RMAGIC]=945995209") : rte::Junk(r, r.U32() % 200u, false);
                            rte::PutBytes(*c.mem, c.scratch, t);
                            a.a[0] = (!c.sample && Chance(r, 12)) ? 0u : c.scratch;
                            uint32_t ch = r.U32();
                            if (!t.empty() && Chance(r, 2)) ch = static_cast<uint8_t>(t[r.U32() % t.size()]) | (Chance(r, 2) ? 0u : r.U32() & 0xFFFFFF00u);
                            if (Chance(r, 8)) ch = 0;
                            a.a[1] = c.sample ? 61u : ch;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "strchr", [&](s::GuestRam& g, V&, uint32_t& v0) {
                                v0 = s::BiosStrChr(g, a.a[0], a.a[1]);
                                return true;
                            });
                        }, true, {}));
    rows.push_back(Make("rp_bios_atoi", "s32 atoi(char *a)  [SLUS 0x80044974 -> A(10h)]", s::kRpAtoi,
                        [](CaseContext& c, Args& a) {
                            Rng& r = *c.rng;
                            std::string t = c.sample ? std::string("945995209") : rte::Number(r);
                            if (!c.sample && Chance(r, 6)) t = rte::Junk(r, r.U32() % 20u, true); // NAMED BOUND: bytes < 0x80
                            rte::PutBytes(*c.mem, c.scratch, t);
                            a.a[0] = (!c.sample && Chance(r, 16)) ? 0u : c.scratch;
                        },
                        [](NativeEnv& env, const Args& a) {
                            return Run(env, "atoi", [&](s::GuestRam& g, V&, uint32_t& v0) { return s::BiosAtoi(g, a.a[0], v0); });
                        }, true, {}));
}
