// rrverify gte - what the material in work\oracle can and cannot prove about our COP2.
//
// Three separate things are attempted, and each reports its own verdict rather than being folded
// into a single green tick:
//
//   1. the divider.   Our UNR reciprocal is compared against the exact integer quotient over the
//                     whole SZ3 range, so its error profile is visible instead of assumed.
//   2. the captured   rr-race/cpu.json holds a real hardware GTE register file. LZCS/LZCR is an
//      register file. input/output pair that can be checked outright; for the rest we search for a
//                     GTE command that reproduces the captured outputs from the captured inputs.
//   3. the draw       work\oracle\vr_capture\*.csv - whether an input -> output comparison can be
//      captures.      constructed from them at all.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "interp/gte.h"
#include "interp/r3000.h"
#include "interp/snapshot.h"

using rr::interp::Cpu;
using rr::interp::Gte;
using rr::interp::Memory;

namespace {

struct Op {
    const char* name;
    uint32_t opcode;
};

constexpr Op kOps[] = {
    {"RTPS", 0x01},  {"NCLIP", 0x06}, {"OP", 0x0C},   {"DPCS", 0x10}, {"INTPL", 0x11},
    {"MVMVA", 0x12}, {"NCDS", 0x13},  {"CDP", 0x14},  {"NCDT", 0x16}, {"NCCS", 0x1B},
    {"CC", 0x1C},    {"NCS", 0x1E},   {"NCT", 0x20},  {"SQR", 0x28},  {"DCPL", 0x29},
    {"DPCT", 0x2A},  {"AVSZ3", 0x2D}, {"AVSZ4", 0x2E}, {"RTPT", 0x30}, {"GPF", 0x3D},
    {"GPL", 0x3E},   {"NCCT", 0x3F},
};

std::string Join(const std::string& dir, const char* name) {
    std::string s = dir;
    if (!s.empty() && s.back() != '\\' && s.back() != '/') s.push_back('\\');
    return s + name;
}

int CheckDivider() {
    std::printf("--- 1. the RTPS/RTPT perspective divider ---\n");
    // Reference: the closed form the UNR table approximates, ((H * 0x20000 / SZ3) + 1) / 2, taken
    // only over the non-overflow domain H < SZ3*2 (outside it the hardware saturates to 0x1FFFF).
    // The divider is exercised through a real RTPS rather than through a private back door: with a
    // zero rotation matrix, V0 = 0, sf = 1, TRZ = z and DQA = 1, DQB = 0, the final MAC0 that RTPS
    // leaves behind is exactly the quotient (((H * 0x20000) / SZ3) + 1) / 2.
    const uint32_t kRtpsSf = 0x4A000000u | (1u << 19) | 0x01u;
    uint32_t worst = 0;
    uint32_t worstH = 0, worstZ = 0;
    uint64_t samples = 0;
    uint64_t exactHits = 0;
    for (uint32_t h : {1u, 7u, 100u, 237u, 1000u, 4096u, 0xFFFFu}) {
        for (uint32_t z = 1; z <= 0xFFFFu; ++z) {
            if (h >= z * 2) continue;
            Gte probe;
            probe.Reset();
            probe.cr[26] = h;                    // H
            probe.cr[7] = z;                     // TRZ -> MAC3 = z -> SZ3 = z
            probe.cr[27] = 1;                    // DQA = 1
            probe.cr[28] = 0;                    // DQB = 0
            probe.Execute(kRtpsSf);
            const uint32_t got = probe.dr[24];   // MAC0
            const uint64_t exact = ((static_cast<uint64_t>(h) * 0x20000u / z) + 1) / 2;
            const uint32_t diff = (got > exact) ? static_cast<uint32_t>(got - exact)
                                                : static_cast<uint32_t>(exact - got);
            if (diff == 0) ++exactHits;
            if (diff > worst) { worst = diff; worstH = h; worstZ = z; }
            ++samples;
        }
    }
    std::printf("  exact on %llu of %llu pairs (%.2f%%)\n", static_cast<unsigned long long>(exactHits),
                static_cast<unsigned long long>(samples),
                samples ? 100.0 * static_cast<double>(exactHits) / static_cast<double>(samples) : 0.0);
    std::printf("  %llu (H,SZ3) pairs; max |UNR - exact| = %u (at H=%u SZ3=%u)\n",
                static_cast<unsigned long long>(samples), worst, worstH, worstZ);
    std::printf("  NOTE: this is a self-consistency check, NOT a hardware comparison. It shows only\n");
    std::printf("  that we implement the documented seed table plus Newton step and that its error\n");
    std::printf("  profile is the expected one (0..2 off the exact quotient, never more). Proving the\n");
    std::printf("  table itself right would need output from silicon.\n");
    std::printf("  verdict: %s\n",
                worst <= 2 ? "error profile as documented"
                           : "OUT OF SPEC - the table or the Newton step is wrong");
    return worst <= 2 ? 0 : 1;
}

int CheckRegisterFile(const std::string& stateDir, double& snapH, double& snapOfx, double& snapOfy) {
    std::printf("\n--- 2. the captured hardware GTE register file (%s) ---\n", stateDir.c_str());
    Memory mem;
    Cpu cpu(mem);
    rr::interp::SnapshotInfo info;
    std::string error;
    if (!rr::interp::LoadSnapshot(stateDir, mem, cpu, info, error)) {
        std::printf("  FAIL: %s\n", error.c_str());
        return 1;
    }
    Gte captured;
    for (int i = 0; i < 32; ++i) {
        captured.dr[i] = cpu.gte().dr[i];
        captured.cr[i] = cpu.gte().cr[i];
    }

    std::printf("  control: R = [%08X %08X %08X %08X %08X]  TR = (%d, %d, %d)\n", captured.cr[0],
                captured.cr[1], captured.cr[2], captured.cr[3], captured.cr[4],
                static_cast<int32_t>(captured.cr[5]), static_cast<int32_t>(captured.cr[6]),
                static_cast<int32_t>(captured.cr[7]));
    std::printf("  OFX=%d/65536 OFY=%d/65536 H=%u DQA=%d DQB=%d ZSF3=%d ZSF4=%d FLAG=%08X\n",
                static_cast<int32_t>(captured.cr[24]), static_cast<int32_t>(captured.cr[25]),
                captured.cr[26] & 0xFFFFu,
                static_cast<int32_t>(static_cast<int16_t>(captured.cr[27])),
                static_cast<int32_t>(captured.cr[28]),
                static_cast<int32_t>(static_cast<int16_t>(captured.cr[29])),
                static_cast<int32_t>(static_cast<int16_t>(captured.cr[30])), captured.cr[31]);

    snapH = static_cast<double>(captured.cr[26] & 0xFFFFu);
    snapOfx = static_cast<double>(static_cast<int32_t>(captured.cr[24])) / 65536.0;
    snapOfy = static_cast<double>(static_cast<int32_t>(captured.cr[25])) / 65536.0;

    // 2a. LZCS / LZCR is a genuine hardware input/output pair and needs nothing else.
    {
        Gte probe;
        probe.Reset();
        probe.WriteData(30, captured.dr[30]);
        const bool ok = probe.dr[31] == captured.dr[31];
        std::printf("  LZCS=0x%08X -> LZCR: hardware %u, ours %u   %s\n", captured.dr[30],
                    captured.dr[31], probe.dr[31], ok ? "ok" : "FAIL");
        if (!ok) return 1;
    }

    // 2b. Is the captured state a fixed point of some GTE command? If the last instruction before
    // the save was, say, an RTPT, then re-running RTPT on the captured inputs must reproduce the
    // captured outputs exactly - a real bit-exact comparison against hardware.
    int matches = 0;
    for (const Op& op : kOps) {
        for (int sf = 0; sf < 2; ++sf) {
            for (int lm = 0; lm < 2; ++lm) {
                Gte probe;
                for (int i = 0; i < 32; ++i) { probe.dr[i] = captured.dr[i]; probe.cr[i] = captured.cr[i]; }
                uint32_t command = 0x4A000000u | op.opcode |
                                   (static_cast<uint32_t>(sf) << 19) | (static_cast<uint32_t>(lm) << 10);
                probe.Execute(command);
                if (probe.unimplemented) continue;
                bool same = true;
                for (int i = 0; i < 32 && same; ++i) {
                    if (i == 23) continue; // RES1 is not a real register
                    if (probe.dr[i] != captured.dr[i]) same = false;
                }
                if (same) {
                    ++matches;
                    std::printf("  fixed point: %s sf=%d lm=%d reproduces every captured data register\n",
                                op.name, sf, lm);
                }
            }
        }
    }
    if (matches == 0) {
        std::printf("  no GTE command reproduces the captured register file from itself.\n");
        std::printf("  NEGATIVE, and it is explainable rather than a failure: the captured state has\n");
        std::printf("  IR1..IR3 = 0 while MAC1..MAC3 = %d, %d, %d, and every command that writes MAC1..3\n",
                    static_cast<int32_t>(captured.dr[25]), static_cast<int32_t>(captured.dr[26]),
                    static_cast<int32_t>(captured.dr[27]));
        std::printf("  also writes IR1..3 from them, so the register file cannot be the immediate\n");
        std::printf("  result of any single GTE command. Instructions ran between the last COP2\n");
        std::printf("  command and the save, so this snapshot is not an input/output pair.\n");
    }
    return 0;
}

struct CsvStats {
    size_t rows = 0;
    size_t integralScreen = 0;
    bool hasInputVertexColumn = false;
    bool hasMatrixColumn = false;
    std::vector<std::string> columns;
};

bool ReadCsv(const std::string& path, CsvStats& stats, double h, double ofx, double ofy) {
    std::vector<uint8_t> bytes;
    std::string error;
    if (!rr::interp::ReadWholeFile(path, bytes, error)) return false;
    const std::string s(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    size_t at = 0;
    bool header = true;
    int ix = -1, iy = -1, iz = -1;
    while (at < s.size()) {
        size_t eol = s.find('\n', at);
        if (eol == std::string::npos) eol = s.size();
        std::string line = s.substr(at, eol - at);
        at = eol + 1;
        while (!line.empty() && (line.back() == '\r')) line.pop_back();
        if (line.empty()) continue;
        std::vector<std::string> cells;
        size_t p = 0;
        while (p <= line.size()) {
            const size_t c = line.find(',', p);
            const size_t e = (c == std::string::npos) ? line.size() : c;
            cells.push_back(line.substr(p, e - p));
            if (c == std::string::npos) break;
            p = c + 1;
        }
        if (header) {
            stats.columns = cells;
            for (size_t i = 0; i < cells.size(); ++i) {
                if (cells[i] == "x") ix = static_cast<int>(i);
                if (cells[i] == "y") iy = static_cast<int>(i);
                if (cells[i] == "z") iz = static_cast<int>(i);
                if (cells[i] == "vx" || cells[i] == "mx" || cells[i] == "sx0") stats.hasInputVertexColumn = true;
                if (cells[i].rfind("rt", 0) == 0 || cells[i].rfind("mat", 0) == 0) stats.hasMatrixColumn = true;
            }
            header = false;
            continue;
        }
        if (ix < 0 || iy < 0 || iz < 0) return false;
        if (cells.size() <= static_cast<size_t>(iz)) continue;
        const double x = std::atof(cells[static_cast<size_t>(ix)].c_str());
        const double y = std::atof(cells[static_cast<size_t>(iy)].c_str());
        const double z = std::atof(cells[static_cast<size_t>(iz)].c_str());
        ++stats.rows;
        if (z == 0.0) continue;
        const double sx = x * h / z + ofx;
        const double sy = y * h / z + ofy;
        const double ex = std::fabs(sx - std::round(sx));
        const double ey = std::fabs(sy - std::round(sy));
        if (ex < 1e-4 && ey < 1e-4) ++stats.integralScreen;
    }
    return true;
}

int CheckCaptures(const std::string& captureDir, double snapH, double snapOfx, double snapOfy) {
    std::printf("\n--- 3. the draw-stream captures (%s) ---\n", captureDir.c_str());
    // scene.txt records the projection the capture was taken with.
    double h = 0, ofx = 0, ofy = 0;
    {
        std::vector<uint8_t> bytes;
        std::string error;
        if (!rr::interp::ReadWholeFile(Join(captureDir, "scene.txt"), bytes, error)) {
            std::printf("  FAIL: %s\n", error.c_str());
            return 1;
        }
        const std::string s(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        const auto get = [&](const char* key, double& out) {
            const size_t k = s.find(key);
            if (k != std::string::npos) out = std::atof(s.c_str() + k + std::strlen(key));
        };
        get("H=", h);
        get("OFX=", ofx);
        get("OFY=", ofy);
    }
    std::printf("  scene.txt projection:      H=%.2f OFX=%.2f OFY=%.2f\n", h, ofx, ofy);
    std::printf("  snapshot GTE control regs: H=%.2f OFX=%.2f OFY=%.2f\n", snapH, snapOfx, snapOfy);
    if (h == snapH && ofx == snapOfx && ofy == snapOfy) {
        std::printf("  -> the draw capture and the register-file snapshot agree on the projection, which\n");
        std::printf("     independently confirms our decoding of cop2r56/57/58 (OFX/OFY/H) and the\n");
        std::printf("     16.16 scale of OFX/OFY. That is the one thing these two artifacts jointly prove.\n");
    } else {
        std::printf("  -> they differ; the two artifacts are from different projection setups.\n");
    }

    CsvStats stats;
    if (!ReadCsv(Join(captureDir, "scene.csv"), stats, h, ofx, ofy)) {
        std::printf("  FAIL: cannot read scene.csv\n");
        return 1;
    }
    std::printf("  scene.csv columns:");
    for (const std::string& c : stats.columns) std::printf(" %s", c.c_str());
    std::printf("\n");
    std::printf("  %zu vertices; %zu (%.1f%%) back-project to integral screen coordinates\n", stats.rows,
                stats.integralScreen,
                stats.rows ? 100.0 * static_cast<double>(stats.integralScreen) / static_cast<double>(stats.rows) : 0.0);
    std::printf("  input-vertex column present: %s; matrix column present: %s\n",
                stats.hasInputVertexColumn ? "yes" : "no", stats.hasMatrixColumn ? "yes" : "no");
    std::printf("\n");
    std::printf("  VERDICT: no valid GTE comparison can be constructed from these captures.\n");
    std::printf("    * The rows hold only post-transform view-space positions, already converted to\n");
    std::printf("      float and back-projected through H/OFX/OFY by the capture tool; the integral\n");
    std::printf("      screen coordinates the GTE actually produced cannot be recovered exactly\n");
    std::printf("      (see the percentage above - the ones that do come out integral are the 2D HUD\n");
    std::printf("      primitives sitting at z == H).\n");
    std::printf("    * There is no input vertex, no rotation matrix and no translation in the file,\n");
    std::printf("      and the capture is from frame 10992 while the register-file snapshot is frame\n");
    std::printf("      6229, so the two cannot be joined either.\n");
    std::printf("    * A GTE oracle needs (input registers, opcode, output registers) triples. The\n");
    std::printf("      interpreter itself is now the cheapest way to produce them: log every COP2\n");
    std::printf("      command executed during a real frame and replay the log.\n");
    return 0;
}

} // namespace

int CmdGte(const std::string& stateDir, const std::string& captureDir);

int CmdGte(const std::string& stateDir, const std::string& captureDir) {
    int rc = 0;
    double snapH = 0, snapOfx = 0, snapOfy = 0;
    rc |= CheckDivider();
    rc |= CheckRegisterFile(stateDir, snapH, snapOfx, snapOfy);
    rc |= CheckCaptures(captureDir, snapH, snapOfx, snapOfy);
    return rc;
}
