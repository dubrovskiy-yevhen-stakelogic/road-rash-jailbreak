// rrtool - command line access to the assets of Road Rash: Jailbreak (USA, SLUS_01053).
// Reads the player's own disc image; writes nothing back to it.
#include "game/world.h"
#include "rrformats/camera_ca.h"
#include "rrformats/cell.h"
#include "rrformats/chunk.h"
#include "rrformats/dash.h"
#include "rrformats/rmd3.h"
#include "rrformats/road.h"
#include "rrformats/texture.h"
#include "platform/png.h"
#include "rrvfs/disc_identity.h"
#include "rrvfs/disc_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <map>
#include <string>

namespace {

int Usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  rrtool identify <disc.bin|disc.cue> [--json]      supported release? (EXE + overlay SHA-1)\n"
                 "  rrtool list <disc.bin>\n"
                 "  rrtool extract <disc.bin> <outdir>\n"
                 "  rrtool cat <disc.bin> <path-on-disc> <out-file>\n"
                 "  rrtool geo <disc.bin> <path-on-disc>              model structure\n"
                 "  rrtool geoobj <disc.bin> <path-on-disc> <out.obj> [group]\n"
                 "  rrtool geoscan <disc.bin>                          parse every *.GEO\n");
    return 2;
}

void PrintModels(const std::vector<rr::Model>& models) {
    for (size_t m = 0; m < models.size(); ++m) {
        const rr::Model& model = models[m];
        std::printf("object %zu: id %u, %zu group(s)\n", m, model.id, model.groups.size());
        for (size_t g = 0; g < model.groups.size(); ++g) {
            const rr::ModelGroup& group = model.groups[g];
            size_t prims = 0;
            for (const rr::SubMesh& s : group.subMeshes) prims += s.prims.size();
            std::printf("  group %zu: flags %08X scale %u slot %u  verts %zu normals %zu "
                        "quadsD %zu sub %zu prims %zu  bbox half (%d,%d,%d) r %d\n",
                        g, group.flags, group.scale, group.slot, group.verts.size(), group.normals.size(),
                        group.quadsD.size(), group.subMeshes.size(), prims, group.bbox.half[0],
                        group.bbox.half[1], group.bbox.half[2], group.bbox.radius);
        }
    }
}

// Wavefront OBJ of one group, for eyeballing and for diffing against the Python probe.
void WriteObj(const std::filesystem::path& path, const rr::ModelGroup& group) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    out << "# Road Rash: Jailbreak model group, exported by rrtool\n";
    for (const rr::SVector& v : group.verts) out << "v " << v.x << " " << v.y << " " << v.z << "\n";
    for (const rr::SubMesh& sub : group.subMeshes)
        for (const rr::Primitive& p : sub.prims) {
            // Perimeter order, not the PS1 strip order - see the note in `BuildTriangleSoup` and
            // `rrtool quadwind` for the measurement that settles it. The soup's corners (rmd3.h SoupCorners):
            // a quad on the emitter's i1-i3 diagonal, a 3-corner primitive as its one triangle.
            const int* c = rr::SoupCorners(p);
            for (int t = 0; t < 2; ++t) {
                if (!rr::SoupTriangleDrawn(p, t)) continue;
                out << "f " << p.index[static_cast<size_t>(c[3 * t])] + 1 << " " << p.index[static_cast<size_t>(c[3 * t + 1])] + 1
                    << " " << p.index[static_cast<size_t>(c[3 * t + 2])] + 1 << "\n";
            }
        }
}

void WriteFile(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write " + path.string());
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

} // namespace

// rrtool identify <image> [--json]: which disc this is, by the SHA-1 of its executable and overlays.
// Exit 0 = the supported release, 3 = a readable image of something else, 1 = not a readable image.
int Identify(const std::string& input, bool json) {
    auto escape = [](const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '\\' || c == '"') o.push_back('\\');
            if (static_cast<unsigned char>(c) < 0x20) continue;
            o.push_back(c);
        }
        return o;
    };
    std::string image = input, error;
    rr::DiscIdentity id;
    bool readable = false;
    try {
        image = rr::ResolveImagePath(input);
        rr::DiscImage disc(image);
        id = rr::IdentifyDisc(disc);
        readable = true;
    } catch (const std::exception& e) {
        error = e.what();
        id.reason = "not a readable raw MODE2/2352 PS1 image (" + error +
                    "). Use a single-track BIN/CUE dump; a 2048-byte ISO lacks the XA sectors the game streams";
    }
    const int rc = id.supported ? 0 : (readable ? 3 : 1);
    if (json) {
        std::printf("{\n  \"image\": \"%s\",\n  \"readable\": %s,\n  \"volume\": \"%s\",\n  \"sectors\": %u,\n  \"files\": %u,\n",
                    escape(image).c_str(), readable ? "true" : "false", escape(id.volume).c_str(), id.sectors, id.files);
        std::printf("  \"exeSha1\": \"%s\",\n  \"parts\": [", id.parts.empty() ? "" : id.parts[0].sha1.c_str());
        for (size_t i = 0; i < id.parts.size(); ++i) {
            const rr::DiscPart& p = id.parts[i];
            std::printf("%s\n    {\"name\": \"%s\", \"size\": %u, \"sha1\": \"%s\", \"expected\": \"%s\", \"matches\": %s}",
                        i ? "," : "", p.name.c_str(), p.size, p.sha1.c_str(), p.expected.c_str(), p.matches ? "true" : "false");
        }
        std::printf("\n  ],\n  \"supported\": %s,\n  \"release\": \"%s\",\n  \"reason\": \"%s\"\n}\n",
                    id.supported ? "true" : "false", escape(id.release).c_str(), escape(id.reason).c_str());
        return rc;
    }
    std::printf("image    %s\n", image.c_str());
    if (readable) {
        std::printf("format   raw MODE2/2352, %u sectors, %u files\n", id.sectors, id.files);
        std::printf("volume   %s\n", id.volume.c_str());
        for (size_t i = 0; i < id.parts.size(); ++i) {
            const rr::DiscPart& p = id.parts[i];
            if (p.present)
                std::printf("%-8s %-12s %7u bytes  sha1 %s  %s\n", i == 0 ? "exe" : "overlay", p.name.c_str(), p.size,
                            p.sha1.c_str(), p.matches ? "ok" : "DIFFERS");
            else std::printf("%-8s %-12s missing\n", i == 0 ? "exe" : "overlay", p.name.c_str());
        }
    }
    if (id.supported) std::printf("verdict  SUPPORTED: %s\n", id.release.c_str());
    else std::printf("verdict  UNSUPPORTED: %s\n", id.reason.c_str());
    return rc;
}

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "identify")
        return Identify(argv[2], argc >= 4 && std::string(argv[3]) == "--json");
    if (argc < 3) return Usage();
    const std::string command = argv[1];
    try {
        rr::DiscImage disc(argv[2]);
        if (command == "list") {
            std::printf("volume %s, %u sectors, %zu files\n", disc.VolumeId().c_str(), disc.SectorCount(),
                        disc.Files().size());
            for (const rr::DiscFile& f : disc.Files())
                std::printf("%8u %10u  %s\n", f.lba, f.size, f.path.c_str());
            return 0;
        }
        if (command == "extract") {
            if (argc < 4) return Usage();
            const std::filesystem::path root = argv[3];
            for (const rr::DiscFile& f : disc.Files()) {
                std::filesystem::path out = root;
                for (const std::string& part : [&] {
                         std::vector<std::string> parts;
                         std::string current;
                         for (char c : f.path) {
                             if (c == '/') {
                                 if (!current.empty()) parts.push_back(current);
                                 current.clear();
                             } else {
                                 current.push_back(c);
                             }
                         }
                         if (!current.empty()) parts.push_back(current);
                         return parts;
                     }())
                    out /= part;
                WriteFile(out, disc.ReadFile(f));
            }
            std::printf("extracted %zu files to %s\n", disc.Files().size(), argv[3]);
            return 0;
        }
        if (command == "cat") {
            if (argc < 5) return Usage();
            const auto found = disc.Find(argv[3]);
            if (!found) {
                std::fprintf(stderr, "not on disc: %s\n", argv[3]);
                return 1;
            }
            WriteFile(argv[4], disc.ReadFile(*found));
            std::printf("%s -> %s (%u bytes, lba %u)\n", found->path.c_str(), argv[4], found->size, found->lba);
            return 0;
        }
        if (command == "geo" || command == "geoobj") {
            if (argc < 4) return Usage();
            const auto found = disc.Find(argv[3]);
            if (!found) {
                std::fprintf(stderr, "not on disc: %s\n", argv[3]);
                return 1;
            }
            const std::vector<uint8_t> bytes = disc.ReadFile(*found);
            const std::string p = found->path;
            const bool mro = p.size() >= 4 && p.compare(p.size() - 4, 4, ".MRO") == 0; // the sidecar machines
            const std::vector<rr::Model> models = mro ? rr::ParseMro(bytes) : rr::ParseGeo(bytes);
            if (command == "geo") {
                std::printf("%s: %u bytes, %zu object(s)\n", found->path.c_str(), found->size, models.size());
                PrintModels(models);
                return 0;
            }
            if (argc < 5) return Usage();
            const size_t groupIndex = argc > 5 ? static_cast<size_t>(std::stoul(argv[5])) : 0;
            if (groupIndex >= models.front().groups.size()) {
                std::fprintf(stderr, "object 0 has only %zu group(s)\n", models.front().groups.size());
                return 1;
            }
            WriteObj(argv[4], models.front().groups[groupIndex]);
            std::printf("%s object 0 group %zu -> %s\n", found->path.c_str(), groupIndex, argv[4]);
            return 0;
        }
        if (command == "quadwind") {
            // Which way round are a model quad's four corners written?
            //
            // The scene cells turned out to store them along the PERIMETER (corner 2 opposite
            // corner 0), not in the PS1 `POLY_FT4` strip order (corner 3 opposite corner 0), and
            // splitting them the strip way folded every quad into a bowtie that lost a quarter of
            // its area (docs\formats\scene_cell.md 4.1.1). Models are a different format, so the
            // same question has to be asked of them separately rather than assumed either way.
            //
            // The test needs no rendering: split each quad both ways and ask which split gives two
            // triangles wound the SAME way. A bowtie gives opposite windings.
            size_t quads = 0, stripOk = 0, perimeterOk = 0, degenerate = 0, neither = 0;
            for (const rr::DiscFile& f : disc.Files()) {
                if (f.path.size() < 4 || f.path.compare(f.path.size() - 4, 4, ".GEO") != 0) continue;
                for (const rr::Model& model : rr::ParseGeo(disc.ReadFile(f)))
                    for (const rr::ModelGroup& group : model.groups)
                        for (const rr::SubMesh& sub : group.subMeshes)
                            for (const rr::Primitive& prim : sub.prims) {
                                bool inRange = true;
                                for (int k = 0; k < 4; ++k)
                                    if (prim.index[k] >= group.verts.size()) inRange = false;
                                if (!inRange) continue;
                                ++quads;
                                const auto cross = [&](int a, int b, int c) {
                                    const rr::SVector& p = group.verts[prim.index[a]];
                                    const rr::SVector& q = group.verts[prim.index[b]];
                                    const rr::SVector& r = group.verts[prim.index[c]];
                                    const double ux = q.x - p.x, uy = q.y - p.y, uz = q.z - p.z;
                                    const double vx = r.x - p.x, vy = r.y - p.y, vz = r.z - p.z;
                                    return std::array<double, 3>{uy * vz - uz * vy, uz * vx - ux * vz,
                                                                 ux * vy - uy * vx};
                                };
                                const auto agree = [&](std::array<double, 3> a, std::array<double, 3> b) {
                                    const double dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
                                    const double la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
                                    const double lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
                                    if (la <= 0.0 || lb <= 0.0) return 0; // a degenerate half
                                    return dot > 0.0 ? 1 : -1;
                                };
                                const int strip = agree(cross(0, 1, 2), cross(1, 3, 2));
                                const int perimeter = agree(cross(0, 1, 2), cross(0, 2, 3));
                                if (strip == 0 || perimeter == 0) {
                                    ++degenerate;
                                } else if (strip > 0 && perimeter <= 0) {
                                    ++stripOk;
                                } else if (perimeter > 0 && strip <= 0) {
                                    ++perimeterOk;
                                } else if (strip > 0 && perimeter > 0) {
                                    ++stripOk; // planar and convex: both splits agree, no evidence
                                    ++perimeterOk;
                                } else {
                                    ++neither;
                                }
                            }
            }
            std::printf("model quads %zu: strip order consistent %zu, perimeter order consistent %zu, "
                        "degenerate %zu, neither %zu\n",
                        quads, stripOk, perimeterOk, degenerate, neither);
            return 0;
        }
        if (command == "geoscan") {
            size_t files = 0, objects = 0, groups = 0, subMeshes = 0, verts = 0, prims = 0, failures = 0;
            for (const rr::DiscFile& f : disc.Files()) {
                if (f.path.size() < 4 || f.path.compare(f.path.size() - 4, 4, ".GEO") != 0) continue;
                ++files;
                try {
                    const std::vector<rr::Model> models = rr::ParseGeo(disc.ReadFile(f));
                    objects += models.size();
                    for (const rr::Model& m : models)
                        for (const rr::ModelGroup& g : m.groups) {
                            ++groups;
                            subMeshes += g.subMeshes.size();
                            verts += g.verts.size();
                            for (const rr::SubMesh& s : g.subMeshes) prims += s.prims.size();
                        }
                } catch (const std::exception& e) {
                    ++failures;
                    std::printf("FAIL %s: %s\n", f.path.c_str(), e.what());
                }
            }
            std::printf("%zu .GEO files, %zu failed\n", files, failures);
            std::printf("objects %zu groups %zu submeshes %zu verts %zu prims %zu\n", objects, groups, subMeshes,
                        verts, prims);
            return failures == 0 ? 0 : 1;
        }
        if (command == "races") {
            // The race ids of each set, as the race graph DATA/ROADGRF<set>.TXT declares them. They are
            // not 1..count: set 2's 36 ids are not contiguous, so anything that runs "every race"
            // (the race sweep gate) enumerates them here. One line per set: `set <n>: <id> <id> ...`.
            for (int set = 1; set <= 2; ++set) {
                const auto textFile = disc.Find("DATA/ROADGRF" + std::to_string(set) + ".TXT");
                if (!textFile) {
                    std::fprintf(stderr, "races: DATA/ROADGRF%d.TXT not found\n", set);
                    return 1;
                }
                const std::vector<uint8_t> textBytes = disc.ReadFile(*textFile);
                const rr::RaceGraph graph = rr::ParseRaceGraph(
                    std::string_view(reinterpret_cast<const char*>(textBytes.data()), textBytes.size()));
                std::printf("set %d:", set);
                for (const rr::Race& race : graph.races) std::printf(" %d", race.raceId);
                std::printf("\n");
            }
            return 0;
        }
        if (command == "raceplan") {
            // Builds the ordered roads of every race and reports the plan.
            //
            // WHY THERE IS NO CHECK AGAINST THE GAME'S OWN PRELOAD HERE: the obvious test - find the
            // .STP's chunks in STREAM<n>.STR and see which road/direction range they land in - cannot
            // be made to work by content, because chunk contents are not unique. STREAM1.STR holds
            // 7218 chunks but only 3134 distinct ones (measured), with some content appearing three
            // times, so a chunk cannot be attributed to a stream by its bytes. Two further premises
            // also failed when tried: the .STP does not begin with the start road's chunks, and a
            // race starts part way along its road rather than at the road's first chunk. Verifying
            // the plan therefore waits on the chunk format, which gives a position inside the road.
            int problems = 0;
            for (int set = 1; set <= 2; ++set) {
                const std::string suffix = std::to_string(set);
                const auto textFile = disc.Find("DATA/ROADGRF" + suffix + ".TXT");
                const auto grfFile = disc.Find("DATA/STREAM" + suffix + ".GRF");
                const auto tocFile = disc.Find("DATA/STREAM" + suffix + ".TOC");
                if (!textFile || !grfFile || !tocFile) { ++problems; continue; }
                const std::vector<uint8_t> textBytes = disc.ReadFile(*textFile);
                const rr::RaceGraph graph = rr::ParseRaceGraph(
                    std::string_view(reinterpret_cast<const char*>(textBytes.data()), textBytes.size()));
                const rr::RoadNetwork net = rr::ParseRoadNetwork(disc.ReadFile(*grfFile));
                const rr::StreamToc toc = rr::ParseStreamToc(disc.ReadFile(*tocFile));

                size_t checked = 0, legTotal = 0, chunksPlanned = 0;
                for (const rr::Race& race : graph.races) {
                    const std::vector<rr::RouteLeg> legs = rr::BuildRouteLegs(race, net);
                    legTotal += legs.size();
                    if (legs.empty()) { ++problems; continue; }

                    // Every leg must resolve to a stream range, and the legs must chain: each leg
                    // starts at the junction the previous one ended at.
                    for (size_t i = 0; i < legs.size(); ++i) {
                        const rr::StreamRange range = rr::StreamRangeFor(toc, legs[i].road, legs[i].direction);
                        if (range.size == 0 || range.size % 0x4000 != 0) {
                            std::printf("  FAIL set %d race %d leg %zu: road %d dir %d has a bad stream range\n", set,
                                        race.raceId, i, legs[i].road, legs[i].direction);
                            ++problems;
                        }
                        chunksPlanned += range.size / 0x4000;
                        if (i > 0 && legs[i].fromNode != legs[i - 1].toNode) {
                            std::printf("  FAIL set %d race %d leg %zu: starts at node %d but the previous leg ended "
                                        "at %d\n",
                                        set, race.raceId, i, legs[i].fromNode, legs[i - 1].toNode);
                            ++problems;
                        }
                    }
                    ++checked;
                }
                std::printf("set %d: %zu races, %zu legs, %zu chunks of road planned; legs chain and resolve to a "
                            "stream range in %zu races\n",
                            set, graph.races.size(), legTotal, chunksPlanned, checked);
            }
            std::printf("%s\n", problems == 0
                                    ? "raceplan: every leg chains and resolves to a stream range (the plan itself is "
                                      "not yet verified against the original - see the comment above)"
                                    : "raceplan: FAILURES above");
            return problems == 0 ? 0 : 1;
        }
        if (command == "props") {
            // The kind-4 placements (the roadside props) a race's cells carry, one line each, and the
            // pairs that stand in the same place: `props <set> <race>`. A diagnostic of the loader,
            // not a claim about what the original spawns (that is RASHCDG 0x8009C654's rule).
            if (argc < 5) return Usage();
            const rr::RaceWorld world = rr::LoadRaceWorld(disc, std::atoi(argv[3]), std::atoi(argv[4]), 8000);
            struct P { float x, y, z; uint16_t cls; uint32_t cell; size_t rec; };
            std::vector<P> props;
            for (const rr::CellData& cell : world.cells)
                for (size_t r = 0; r < cell.placements.size(); ++r) {
                    const rr::CellPlacement& p = cell.placements[r];
                    if (p.kind != 4) continue;
                    props.push_back({rr::PlacementWorldX(p), rr::PlacementWorldY(p), rr::PlacementWorldZ(p), p.cls,
                                     cell.header.key, r});
                }
            size_t same = 0;
            for (size_t i = 0; i < props.size(); ++i) {
                const P& a = props[i];
                std::printf("  prop cls %2u at (%9.2f %8.2f %9.2f) cell %08X rec %zu\n", a.cls, a.x, a.y, a.z, a.cell,
                            a.rec);
                for (size_t j = i + 1; j < props.size(); ++j) {
                    const P& b = props[j];
                    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
                    if (dx * dx + dy * dy + dz * dz < 1.0f) {
                        ++same;
                        std::printf("    SAME PLACE as cls %u cell %08X rec %zu\n", b.cls, b.cell, b.rec);
                    }
                }
            }
            std::printf("props: %zu kind-4 placements in %zu cells, %zu pairs within 1 unit\n", props.size(),
                        world.cells.size(), same);
            return 0;
        }
        if (command == "raceworld") {
            // Diagnostics for the assembled route: where the centre line jumps, and how big the
            // jumps are. A well-assembled route has slice spacing in the 19..32 world unit band
            // that docs\formats\road_chunk.md measured.
            if (argc < 5) return Usage();
            const int set = std::atoi(argv[3]);
            const int raceId = std::atoi(argv[4]);
            const rr::RaceWorld world = rr::LoadRaceWorld(disc, set, raceId, 8000);
            std::printf("set %d race %d: %zu legs, %zu road chunks, %zu cells, %zu slices\n", set, raceId,
                        world.legs.size(), world.roadChunksRead, world.cells.size(), world.path.size());
            std::printf("  junction cores found %zu, junction arms stitched in %zu, chunks scanned %zu\n",
                        world.junctionCoresFound, world.junctionsStitched, world.chunksScanned);
            for (const rr::JunctionReport& r : world.junctionReports)
                std::printf("  junction node %d: road %d -> %d, turn-table arm %d, %zu arms available, gap %.0f, "
                            "crossed by %d arms over %.0f units, worst seam %.0f\n",
                            r.node, r.roadFrom, r.roadTo, r.armIndex, r.armCount, r.directGap, r.bestArm,
                            r.chainLength, r.bestSeam);
            for (size_t i = 0; i < world.legs.size(); ++i)
                std::printf("  leg %zu: road %d dir %+d  node %d -> %d\n", i, world.legs[i].road,
                            world.legs[i].direction, world.legs[i].fromNode, world.legs[i].toNode);

            for (const rr::RoadObjectPlacement& p : world.placements)
                std::printf("  leg %d road %d dir %+d: owner %u, along %u..%u (%u long), %zu slices, %zu runs\n",
                            p.leg, p.road, p.direction, p.ownerRoad, p.start, p.end, p.end - p.start, p.slices,
                            p.runs);

            std::vector<double> gaps;
            for (size_t i = 0; i + 1 < world.path.size(); ++i) {
                const double dx = rr::WorldX(world.path[i + 1]) - rr::WorldX(world.path[i]);
                const double dy = rr::WorldY(world.path[i + 1]) - rr::WorldY(world.path[i]);
                const double dz = rr::WorldZ(world.path[i + 1]) - rr::WorldZ(world.path[i]);
                gaps.push_back(std::sqrt(dx * dx + dy * dy + dz * dz));
            }
            if (gaps.empty()) {
                std::printf("  path is empty\n");
                return 1;
            }
            std::vector<double> sorted = gaps;
            std::sort(sorted.begin(), sorted.end());
            size_t big = 0;
            for (double g : gaps)
                if (g > 60.0) ++big;
            std::printf("  slice spacing: min %.1f  median %.1f  p90 %.1f  max %.1f\n", sorted.front(),
                        sorted[sorted.size() / 2], sorted[sorted.size() * 9 / 10], sorted.back());
            std::printf("  jumps over 60 units: %zu of %zu steps\n", big, gaps.size());
            // The assembled length against what the race graph declares, which is an independent
            // check on the whole assembly: the graph's own unit turns out to be the world unit, so
            // these two numbers are directly comparable and a missing stretch of road shows up here
            // as a shortfall. The declared figure is the distance-to-finish of the first node on the
            // route plus the run-in from the start point, both in graph units.
            // Does the scenery actually line the route? The road had exactly one hole per junction
            // until the junction fan was crossed properly; the cells are collected by the same
            // per-leg walk, so they can have a hole of their own and nothing else would show it.
            // Measured per path slice: the distance to the nearest placed object in any cell.
            {
                std::vector<float> nearest;
                nearest.reserve(world.path.size());
                for (const rr::RoadSlice& slice : world.path) {
                    const float sx = rr::WorldX(slice), sy = rr::WorldY(slice), sz = rr::WorldZ(slice);
                    float best = 1e30f;
                    for (const rr::CellData& cell : world.cells)
                        for (const rr::CellPlacement& placement : cell.placements) {
                            const float dx = rr::PlacementWorldX(placement) - sx;
                            const float dy = rr::PlacementWorldY(placement) - sy;
                            const float dz = rr::PlacementWorldZ(placement) - sz;
                            best = std::min(best, dx * dx + dy * dy + dz * dz);
                        }
                    nearest.push_back(std::sqrt(best));
                }
                std::vector<float> ranked = nearest;
                std::sort(ranked.begin(), ranked.end());
                size_t bare = 0;
                for (float d : nearest)
                    if (d > 300.0f) ++bare;
                if (!ranked.empty())
                    std::printf("  scenery beside the route: nearest placed object median %.0f, p90 %.0f, max %.0f; "
                                "%zu of %zu slices with nothing within 300 units\n",
                                ranked[ranked.size() / 2], ranked[ranked.size() * 9 / 10], ranked.back(), bare,
                                nearest.size());
            }
            std::printf("  route length: assembled %.0f, declared by the race graph %.0f world units (%+.1f%%)\n",
                        world.assembledLength, world.declaredLength,
                        world.declaredLength > 0.0
                            ? 100.0 * (world.assembledLength - world.declaredLength) / world.declaredLength
                            : 0.0);
            for (size_t i = 0, shown = 0; i < gaps.size() && shown < 12; ++i)
                if (gaps[i] > 60.0) {
                    std::printf("    step %zu: %.0f units, from (%.0f,%.0f,%.0f) to (%.0f,%.0f,%.0f)\n", i, gaps[i],
                                rr::WorldX(world.path[i]), rr::WorldY(world.path[i]), rr::WorldZ(world.path[i]),
                                rr::WorldX(world.path[i + 1]), rr::WorldY(world.path[i + 1]),
                                rr::WorldZ(world.path[i + 1]));
                    ++shown;
                }
            return 0;
        }
        if (command == "raceworldall") {
            // The whole game's worth of routes, as one number. A race is assembled correctly when
            // its centre line has no jump: every junction on it was crossed on real tarmac rather
            // than skipped over. The command fails unless the jump count is zero.
            size_t races = 0, junctions = 0, stitched = 0, withJumps = 0, jumps = 0;
            double worstSeam = 0.0;
            // The assembled length against what the race graph declares, over every race. This is
            // the end-to-end number: legs, junction crossings and start/finish trimming all land in
            // it, and a route that quietly ran the wrong way or kept a whole road it should have
            // trimmed shows up here even when it has no jump.
            double worstSigned = 0.0, worstUnits = 0.0, sumError = 0.0;
            size_t placed = 0, inFan = 0;
            double fanError = 0.0, worstFan = 0.0;
            size_t sampled = 0, bareSlices = 0, bareInJunction = 0;
            float worstScenery = 0.0f;
            std::string worstWhere = "none";
            for (int set = 1; set <= 2; ++set) {
                const std::vector<uint8_t> textBytes = disc.ReadFile(
                    *disc.Find("DATA/ROADGRF" + std::to_string(set) + ".TXT"));
                const rr::RaceGraph graph = rr::ParseRaceGraph(
                    std::string_view(reinterpret_cast<const char*>(textBytes.data()), textBytes.size()));
                for (const rr::Race& race : graph.races) {
                    const rr::RaceWorld world = rr::LoadRaceWorld(disc, set, race.raceId, 8000);
                    ++races;
                    junctions += world.junctionReports.size();
                    stitched += world.junctionsStitched;
                    for (const rr::JunctionReport& r : world.junctionReports)
                        worstSeam = std::max(worstSeam, r.bestSeam);
                    size_t big = 0;
                    for (size_t i = 0; i + 1 < world.path.size(); ++i) {
                        const double dx = rr::WorldX(world.path[i + 1]) - rr::WorldX(world.path[i]);
                        const double dy = rr::WorldY(world.path[i + 1]) - rr::WorldY(world.path[i]);
                        const double dz = rr::WorldZ(world.path[i + 1]) - rr::WorldZ(world.path[i]);
                        if (std::sqrt(dx * dx + dy * dy + dz * dz) > 60.0) ++big;
                    }
                    // Does the scenery line every route, or only the one the single-race command
                    // looks at? The cells are collected by the same per-leg walk the road was, so
                    // they could have a hole of their own and nothing else would show it. Sampled
                    // every 8th slice to keep a whole-game sweep to seconds rather than minutes;
                    // the sampling is stated here because it bounds what this can catch.
                    // Cell GEOMETRY, not placed objects. Those are two different questions, and
                    // measuring the wrong one gives a wrong answer: a stretch of road with no sign
                    // or obstacle beside it is perfectly normal, while a stretch with no terrain is
                    // a hole in the assembled world. The box of each cell is computed once and the
                    // slice is measured against the box, which keeps a whole-game sweep cheap.
                    // Every 16th vertex, not the cells' bounding boxes: a box test says only that the
                    // slice is somewhere inside the sprawl of a cell, and on this data it returns 0
                    // for every slice of every race - a check that cannot fail. Real vertices give a
                    // number that moves, and a hole in the assembled world shows up in it.
                    constexpr size_t kVertexStride = 16;
                    // Measured today: the farthest any sampled slice sits from sampled terrain is 42
                    // world units over all 100 races. The limit is set well above that so it catches
                    // a real hole rather than restating the current number.
                    constexpr float kSceneryLimit = 150.0f;
                    std::vector<std::array<float, 3>> near;
                    for (const rr::CellData& cell : world.cells)
                        for (size_t v = 0; v < cell.vertexX.size(); v += kVertexStride)
                            near.push_back({rr::CellWorldX(cell, v), rr::CellWorldY(cell, v),
                                            rr::CellWorldZ(cell, v)});
                    size_t bareHere = 0;
                    float farthestHere = 0.0f;
                    for (size_t i = 0; i < world.path.size(); i += 8) {
                        const float s[3] = {rr::WorldX(world.path[i]), rr::WorldY(world.path[i]),
                                            rr::WorldZ(world.path[i])};
                        float best = 1e30f;
                        for (const std::array<float, 3>& p : near) {
                            const float dx = p[0] - s[0], dy = p[1] - s[1], dz = p[2] - s[2];
                            best = std::min(best, dx * dx + dy * dy + dz * dz);
                        }
                        ++sampled;
                        const float d = std::sqrt(best);
                        worstScenery = std::max(worstScenery, d);
                        if (d > kSceneryLimit) {
                            ++bareSlices;
                            ++bareHere;
                            farthestHere = std::max(farthestHere, d);
                            // Is the bare stretch inside a junction, or on open road? That decides
                            // whether this is the same class of bug as the road gap was - cells of a
                            // junction filed under a road the leg does not name - or simply a piece
                            // of the map with nothing placed beside it.
                            if (i < world.pathIsJunction.size() && world.pathIsJunction[i]) ++bareInJunction;
                        }
                    }
                    jumps += big;
                    if (big) {
                        ++withJumps;
                        std::printf("  set %d race %d: %zu jumps\n", set, race.raceId, big);
                    }
                    if (bareHere)
                        std::printf("  set %d race %d: %zu sampled slices with no scenery within 300 units, "
                                    "farthest %.0f\n",
                                    set, race.raceId, bareHere, farthestHere);
                    if (world.declaredLength > 0.0) {
                        const double error =
                            100.0 * (world.assembledLength - world.declaredLength) / world.declaredLength;
                        // Races whose start or finish falls inside a junction fan are counted apart.
                        // Such a point has no distance along any road to be placed by, so the leg is
                        // dropped whole and the length is off by whatever part of the fan the race
                        // does not run. Lumping them in would hide how well the rest agrees; making
                        // them agree by using the declared number would make this check circular.
                        if (world.startInsideJunction || world.finishInsideJunction) {
                            std::printf("  set %d race %d: %s inside a junction fan (%+.1f%%)\n", set, race.raceId,
                                        world.startInsideJunction && world.finishInsideJunction ? "starts and finishes"
                                        : world.startInsideJunction                             ? "starts"
                                                                                                : "finishes",
                                        error);
                            ++inFan;
                            fanError += std::abs(error);
                            worstFan = std::max(worstFan, std::abs(error));
                        } else {
                            ++placed;
                            sumError += std::abs(error);
                            if (std::abs(error) > std::abs(worstSigned)) {
                                worstSigned = error;
                                worstUnits = world.assembledLength - world.declaredLength;
                                worstWhere = "set " + std::to_string(set) + " race " + std::to_string(race.raceId);
                            }
                        }
                    }
                }
            }
            const double meanError = placed ? sumError / static_cast<double>(placed) : 0.0;
            std::printf("races assembled %zu, junctions crossed %zu of %zu, worst seam %.0f world units\n", races,
                        stitched, junctions, worstSeam);
            std::printf("races with a jump over 60 units: %zu (%zu jumps in total)\n", withJumps, jumps);
            std::printf("route length against the race graph, %zu races with both ends placed on a road:\n"
                        "  worst %+.1f%% (%s, %+.0f units), mean |error| %.2f%%\n",
                        placed, worstSigned, worstWhere.c_str(), worstUnits, meanError);
            std::printf("  %zu races start or finish inside a junction fan (no road distance to place them by): "
                        "mean |error| %.1f%%, worst %.1f%%\n",
                        inFan, inFan ? fanError / static_cast<double>(inFan) : 0.0, worstFan);
            std::printf("scenery beside every route: %zu sampled slices, %zu with no terrain within 150 units "
                        "(%zu of those inside a junction), farthest %.0f\n",
                        sampled, bareSlices, bareInJunction, worstScenery);
            // The gate. Thresholds sit above what is measured today (0.70 % mean, 7.7 % worst over
            // the 96 placeable races) so that this catches a regression rather than restating the
            // current number, and the jump count has to be exactly zero because a jump means a
            // junction was skipped over rather than driven through.
            const bool ok = withJumps == 0 && meanError < 1.5 && std::abs(worstSigned) < 10.0 && bareSlices == 0;
            std::printf("race world verdict %s\n", ok ? "PASS" : "FAIL");
            return ok ? 0 : 1;
        }
        if (command == "cellcheck") {
            // Independent C++ check of the scene cells in a .STP: every cell must parse, its
            // primitive indices must stay inside its own vertex array, and its geometry must land
            // somewhere sane in world space.
            //
            // The parser also reads regions 3 and 4 (the ground query's sub-area
            // polygons and walk lists), so this also prints their census in the
            // SAME terms `python tools\scout\ground.py cells` prints for the two stream files:
            //   rrtool cellcheck <disc> --streams
            // runs over every distinct cell of STREAM1.STR and STREAM2.STR, as ground.py does, and
            // its census line must equal ground.py's field for field.
            if (argc < 4) return Usage();
            std::vector<std::pair<std::string, size_t>> files; // path, header bytes
            const bool streams = std::string(argv[3]) == "--streams";
            if (streams) files = {{"DATA/STREAM1.STR", 0}, {"DATA/STREAM2.STR", 0}};
            else files = {{argv[3], 0x800}};
            size_t cells = 0, type8 = 0, verts = 0, prims = 0, failures = 0;
            std::map<std::string, size_t> census;
            std::vector<uint8_t> chunk(rr::kChunkSize);
            for (const auto& [path, header] : files) {
                const auto file = disc.Find(path);
                if (!file) {
                    std::fprintf(stderr, "not on disc: %s\n", path.c_str());
                    return 1;
                }
                const size_t chunkCount = (file->size - header) / rr::kChunkSize;
                std::set<std::pair<uint8_t, uint32_t>> seen; // ground.py: the first of each (type, id)
                for (size_t i = 0; i < chunkCount; ++i) {
                    disc.ReadForm1(file->lba, header + i * rr::kChunkSize, chunk.data(), chunk.size());
                    const rr::ChunkHeader h = rr::ParseChunkHeader(chunk);
                    if (h.type != 0 && h.type != 8) continue;
                    if (streams && !seen.insert({h.type, h.id}).second) continue;
                    try {
                        const rr::CellData cell = rr::ParseCellChunk(chunk);
                        ++cells;
                        if (h.type == 8) ++type8;
                        verts += cell.vertexX.size();
                        prims += cell.band0.size() + cell.band1.size();
                        // ---- regions 3 and 4, in ground.py's terms
                        const size_t A = cell.countA, B = cell.countB;
                        bool r3ok = true;
                        for (size_t sub = 0; sub < B; ++sub) {
                            const size_t n = rr::CellSubAreaPolygon(cell, sub).size();
                            ++census["poly" + std::to_string(n)];
                            if (n != 4 && n != 6) r3ok = false;
                        }
                        census["region3 ok"] += r3ok ? 1u : 0u;
                        bool r4ok = true;
                        std::set<uint32_t> keys;
                        auto groupSize = [&](bool fine, size_t sub) -> size_t {
                            const size_t g = fine ? A + B + sub : A + sub;
                            return g < cell.groups.size() ? cell.groups[g].triCount + cell.groups[g].quadCount : 0;
                        };
                        for (size_t k = 0; k < cell.walk.size(); ++k) {
                            const uint32_t key = cell.walk[k].tag >> 2;
                            if (!keys.insert(key).second) continue;
                            const bool fine = (cell.walk[k].tag >> 7) != 0;
                            const size_t sub = (cell.walk[k].tag >> 4) & 7u;
                            const std::vector<uint32_t> pat = fine ? std::vector<uint32_t>{1, 2, 3}
                                                                   : std::vector<uint32_t>{1, 3};
                            if (k + pat.size() > cell.walk.size()) { r4ok = false; continue; }
                            if (sub >= B) r4ok = false;
                            size_t span = 0;
                            for (size_t q = 0; q < pat.size(); ++q) {
                                const rr::CellWalkRecord& rec = cell.walk[k + q];
                                if ((rec.tag & 3u) != pat[q] || static_cast<uint32_t>(rec.tag >> 2u) != key) r4ok = false;
                                for (uint8_t entry : rec.entries)
                                    if (entry >= groupSize(fine, sub)) r4ok = false;
                                census["list entries"] += rec.entries.size();
                                census["empty lists"] += rec.entries.empty() ? 1u : 0u;
                                span += 2 + rec.entries.size();
                            }
                            census["max trio span"] = std::max(census["max trio span"], span);
                            if (fine && cell.walk[k + 1].entries.empty() && !cell.walk[k + 2].entries.empty())
                                ++census["runaway shape"];
                            if (cell.walk[k].entries.empty()) ++census["list 0 empty (0xFF000000 return)"];
                        }
                        census["region4 ok"] += r4ok ? 1u : 0u;
                        census["records"] += cell.walk.size();
                        census["keys"] += keys.size();
                        ++census["cells"];
                    } catch (const std::exception& e) {
                        ++failures;
                        std::printf("  FAIL %s chunk %zu: %s\n", path.c_str(), i, e.what());
                    }
                }
            }
            std::printf("%s: %zu scene cells (%zu of type 8), %zu vertices, %zu primitives, %zu failed\n",
                        streams ? "DATA/STREAM1.STR + DATA/STREAM2.STR (distinct cells)" : files[0].first.c_str(),
                        cells, type8, verts, prims, failures);
            // ground.py's own line: every field, sorted by name, "name value" joined by ", "
            std::string line = "  ";
            for (const auto& [name, value] : census) {
                if (line.size() > 2) line += ", ";
                line += name + " " + std::to_string(value);
            }
            std::printf("regions 3/4 census:\n%s\n", line.c_str());
            return failures == 0 ? 0 : 1;
        }
        if (command == "camera") {
            // DATA\CAMERA*.CA: the four 56-byte records the race loader reads (the first 224 bytes of
            // the file), word by word, for the probe tools\scout\camera.py.
            for (const char* name : {"DATA/CAMERA.CA", "DATA/CAMERAS.CA", "DATA/CAMERA2.CA", "DATA/CAMERA2S.CA"}) {
                const auto f = disc.Find(name);
                if (!f) {
                    std::printf("%s: not on this disc\n", name);
                    continue;
                }
                const std::vector<uint8_t> bytes = disc.ReadFile(*f);
                const rr::CameraSet set = rr::ParseCameraCa(bytes);
                std::printf("%s %zu bytes\n", name, bytes.size());
                for (size_t r = 0; r < rr::kCameraRecords; ++r) {
                    const rr::CameraRecord& c = set.records[r];
                    std::printf("  record %zu yawRate %d eyeZ %d %d eyeY %d %d lookZ %d %d lagK %d %d lagC %d %d "
                                "springC %d springK %d shake %d\n",
                                r, c.yawRate, c.eyeZ[0], c.eyeZ[1], c.eyeY[0], c.eyeY[1], c.lookZ[0], c.lookZ[1],
                                c.lagK[0], c.lagK[1], c.lagC[0], c.lagC[1], c.springC, c.springK, c.shake);
                }
            }
            for (int players = 1; players <= 2; ++players)
                for (uint32_t kind : {0u, 6u, 8u, 9u, 15u, 17u, 18u})
                    std::printf("  file for players %d kind %u: %s\n", players, kind, rr::CameraFileName(players, kind).c_str());
            return 0;
        }
        if (command == "dash") {
            // The HUD page and the layout that ships beside it. Writes the page under each CLUT slot,
            // so the decode can be checked against the composite in docs\formats\textures.md 3.1.
            if (argc < 4) return Usage();
            const auto csvFile = disc.Find("DATA/DASH1P.CSV");
            const auto texFile = disc.Find("DATA/DASH1P.TEX");
            if (!csvFile || !texFile) {
                std::fprintf(stderr, "DASH1P.CSV or DASH1P.TEX is not on this disc\n");
                return 1;
            }
            const std::vector<uint8_t> csvBytes = disc.ReadFile(*csvFile);
            const rr::DashLayout layout = rr::ParseDashCsv(
                std::string_view(reinterpret_cast<const char*>(csvBytes.data()), csvBytes.size()));
            std::printf("DASH1P: CLUT table at (%d,%d); %d textures, %d art rectangles, %d screen items\n",
                        layout.clutTableX, layout.clutTableY, layout.textureCount, layout.artCount,
                        layout.itemCount);
            const bool consistent = layout.textures.size() == static_cast<size_t>(layout.textureCount) &&
                                    layout.art.size() == static_cast<size_t>(layout.artCount) &&
                                    layout.items.size() == static_cast<size_t>(layout.itemCount);
            std::printf("  rows read: %zu / %zu / %zu -> %s\n", layout.textures.size(), layout.art.size(),
                        layout.items.size(), consistent ? "match the declared counts" : "DO NOT MATCH");

            const std::vector<uint8_t> page = disc.ReadFile(*texFile);
            const std::vector<rr::Image> images = rr::DecodeDashTextures(page, layout);
            for (size_t i = 0; i < images.size(); ++i) {
                char name[512];
                std::snprintf(name, sizeof(name), "%s/dash1p.clut%02zu.png", argv[3], i);
                rr::WritePng(name, images[i].width, images[i].height, images[i].rgba);
            }
            std::printf("  wrote %zu page decodes to %s\n", images.size(), argv[3]);
            return consistent ? 0 : 1;
        }
        if (command == "tex") {
            // Decodes a texture container straight off the disc and writes one PNG per LECT image.
            // Kinds 5 and 6 carry no palette in the file, so they come out as an index preview -
            // their real CLUT lives in VRAM (docs\formats\textures.md section 5a).
            if (argc < 5) return Usage();
            const auto found = disc.Find(argv[3]);
            if (!found) {
                std::fprintf(stderr, "not on disc: %s\n", argv[3]);
                return 1;
            }
            const std::vector<rr::TextureChunk> chunks = rr::ParseTextureContainer(disc.ReadFile(*found));
            std::printf("%s: %zu LECT image(s)\n", found->path.c_str(), chunks.size());
            for (size_t i = 0; i < chunks.size(); ++i) {
                const rr::TextureChunk& c = chunks[i];
                std::printf("  %2zu id %04X kind %u bpp %u slot %2u  %dx%d  palette %zu  %s\n", i, c.id, c.kind,
                            c.bpp, c.slot, c.width, c.height, c.palette.size(),
                            c.decoded.Empty() ? "needs an external CLUT" : "decoded");
                char name[512];
                std::snprintf(name, sizeof(name), "%s/chunk%02zu.id%04X.k%u.png", argv[4], i, c.id, c.kind);
                if (!c.decoded.Empty()) {
                    rr::WritePng(name, c.decoded.width, c.decoded.height, c.decoded.rgba);
                } else {
                    // Index preview: a neutral ramp, so the shapes can be checked without the palette.
                    std::vector<uint32_t> ramp(static_cast<size_t>(1) << c.bpp);
                    for (size_t k = 0; k < ramp.size(); ++k) {
                        const uint32_t level = static_cast<uint32_t>(k * 255 / (ramp.size() - 1));
                        ramp[k] = level | (level << 8) | (level << 16) | (0xFFu << 24);
                    }
                    const rr::Image preview = rr::ApplyPalette(c, ramp);
                    rr::WritePng(name, preview.width, preview.height, preview.rgba);
                }
            }
            return 0;
        }
        if (command == "roadgeo") {
            // Independent C++ check of the road geometry: walk every chunk of a .STP, and for the
            // road chunks re-derive the two identities that make the slice frame trustworthy -
            // row 2 of the matrix is the unit tangent, and `chord` is the distance to the next slice.
            if (argc < 4) return Usage();
            const auto stp = disc.Find(argv[3]);
            if (!stp) {
                std::fprintf(stderr, "not on disc: %s\n", argv[3]);
                return 1;
            }
            const size_t chunkCount = (stp->size - 0x800) / rr::kChunkSize;
            size_t roadChunks = 0, slices = 0, runs = 0, pairs = 0;
            double worstTangent = 0.0, worstChord = 0.0;
            size_t typeCount[16] = {};
            std::vector<uint8_t> chunk(rr::kChunkSize);
            for (size_t i = 0; i < chunkCount; ++i) {
                disc.ReadForm1(stp->lba, 0x800 + i * rr::kChunkSize, chunk.data(), chunk.size());
                const rr::ChunkHeader header = rr::ParseChunkHeader(chunk);
                ++typeCount[header.type & 0x0F];
                if (header.type != static_cast<uint8_t>(rr::ChunkType::Road)) continue;
                ++roadChunks;
                const rr::RoadObject object = rr::ParseRoadChunk(chunk);
                slices += object.slices.size();
                runs += object.runs.size();
                for (const rr::SliceRun& run : object.runs)
                    for (size_t k = run.first; k + 1 < run.first + run.count; ++k) {
                        const rr::RoadSlice& a = object.slices[k];
                        const rr::RoadSlice& b = object.slices[k + 1];
                        const double d[3] = {rr::WorldX(b) - rr::WorldX(a), rr::WorldY(b) - rr::WorldY(a),
                                             rr::WorldZ(b) - rr::WorldZ(a)};
                        const double length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                        if (length <= 0.0) continue;
                        ++pairs;
                        // Row 2 of the matrix should be the unit tangent along the chord.
                        const double t[3] = {a.m[6] / 4096.0, a.m[7] / 4096.0, a.m[8] / 4096.0};
                        const double dot = (t[0] * d[0] + t[1] * d[1] + t[2] * d[2]) / length;
                        worstTangent = std::max(worstTangent, std::abs(1.0 - dot));
                        // `chord` is that distance, in 16.16 world units.
                        const double chord = a.chord / 65536.0;
                        if (chord > 0.0) worstChord = std::max(worstChord, std::abs(chord - length) / chord);
                    }
            }
            std::printf("%s: %zu chunks\n", stp->path.c_str(), chunkCount);
            std::printf("  by type:");
            for (size_t t = 0; t < 16; ++t)
                if (typeCount[t]) std::printf(" %zu:%zu", t, typeCount[t]);
            std::printf("\n  road chunks %zu, slices %zu, runs %zu\n", roadChunks, slices, runs);
            std::printf("  over %zu slice pairs: worst |1 - tangent.chord| %.5f, worst chord error %.4f%%\n", pairs,
                        worstTangent, worstChord * 100.0);
            const bool ok = pairs > 0 && worstTangent < 0.01 && worstChord < 0.01;
            std::printf("%s\n", ok ? "roadgeo: the slice frame and the chord agree with the positions"
                                   : "roadgeo: IDENTITIES DO NOT HOLD");
            return ok ? 0 : 1;
        }
        if (command == "asmcheck") {
            // Independent C++ check of part assembly: the assembled bounding box must reproduce the
            // authored BBD3 box. A wrong assembly is off by hundreds of units, so this is a real test.
            const auto overlay = disc.Find("RASHCDG.BIN");
            if (!overlay) {
                std::fprintf(stderr, "RASHCDG.BIN is not on this disc\n");
                return 1;
            }
            const rr::SkeletonTable skeleton = rr::SkeletonTable::FromOverlay(disc.ReadFile(*overlay));
            std::printf("attachment programs read from RASHCDG.BIN:");
            for (const rr::AttachProgram& p : skeleton.Programs()) std::printf(" %zu", p.PartCount());
            std::printf(" parts\n");

            size_t multiPart = 0, fitted = 0, unfitted = 0;
            int32_t worst = 0;
            std::string worstWhere;
            for (const rr::DiscFile& f : disc.Files()) {
                if (f.path.size() < 4 || f.path.compare(f.path.size() - 4, 4, ".GEO") != 0) continue;
                for (const rr::Model& model : rr::ParseGeo(disc.ReadFile(f)))
                    for (size_t g = 0; g < model.groups.size(); ++g) {
                        const rr::ModelGroup& group = model.groups[g];
                        if (group.subMeshes.size() <= 1) continue;
                        ++multiPart;
                        const rr::Assembly assembly = rr::AssembleGroup(group, skeleton);
                        if (!assembly.assembled) {
                            ++unfitted;
                            std::printf("  no program fits %s id %u group %zu (%zu parts)\n", f.path.c_str(), model.id,
                                        g, group.subMeshes.size());
                            continue;
                        }
                        ++fitted;
                        if (assembly.boxError > worst) {
                            worst = assembly.boxError;
                            worstWhere = f.path + " id " + std::to_string(model.id) + " group " + std::to_string(g);
                        }
                    }
            }
            std::printf("multi-part groups %zu: %zu assembled, %zu without a program\n", multiPart, fitted, unfitted);
            std::printf("worst bounding-box error %d units (%s)\n", worst, worstWhere.c_str());
            return unfitted == 0 ? 0 : 1;
        }
        if (command == "lodcheck") {
            // Independent measurement of the claim that bit 30 of DOD3+0x0C marks the groups
            // authored in the large coordinate unit, and of the size factor between the two units.
            std::vector<double> ratios;
            size_t objectsWithBoth = 0, objectsAllSet = 0, objectsAllClear = 0;
            for (const rr::DiscFile& f : disc.Files()) {
                if (f.path.size() < 4 || f.path.compare(f.path.size() - 4, 4, ".GEO") != 0) continue;
                for (const rr::Model& model : rr::ParseGeo(disc.ReadFile(f))) {
                    double sumSet = 0, sumClear = 0;
                    size_t countSet = 0, countClear = 0;
                    for (const rr::ModelGroup& g : model.groups) {
                        if (g.bbox.radius <= 0) continue;
                        if (g.IsLargeUnit()) { sumSet += g.bbox.radius; ++countSet; }
                        else { sumClear += g.bbox.radius; ++countClear; }
                    }
                    if (countSet && countClear) {
                        ++objectsWithBoth;
                        ratios.push_back((sumSet / static_cast<double>(countSet)) /
                                         (sumClear / static_cast<double>(countClear)));
                    } else if (countSet) {
                        ++objectsAllSet;
                    } else if (countClear) {
                        ++objectsAllClear;
                    }
                }
            }
            std::sort(ratios.begin(), ratios.end());
            if (ratios.empty()) {
                std::printf("lodcheck: no object carries both unit flags\n");
                return 1;
            }
            double sum = 0;
            for (double r : ratios) sum += r;
            std::printf("objects with both flags %zu, all-large %zu, all-small %zu\n", objectsWithBoth, objectsAllSet,
                        objectsAllClear);
            std::printf("radius ratio large/small: n %zu, min %.2f, median %.2f, mean %.2f, max %.2f\n", ratios.size(),
                        ratios.front(), ratios[ratios.size() / 2], sum / static_cast<double>(ratios.size()),
                        ratios.back());
            return 0;
        }
        if (command == "roadcheck") {
            // Re-derives the invariants of docs\formats\road.md with our own C++ parsers, so the
            // C++ layer is verified rather than trusted.
            int problems = 0;
            for (int set = 1; set <= 2; ++set) {
                const std::string suffix = std::to_string(set);
                const auto textFile = disc.Find("DATA/ROADGRF" + suffix + ".TXT");
                const auto grfFile = disc.Find("DATA/STREAM" + suffix + ".GRF");
                const auto tocFile = disc.Find("DATA/STREAM" + suffix + ".TOC");
                const auto strFile = disc.Find("DATA/STREAM" + suffix + ".STR");
                if (!textFile || !grfFile || !tocFile || !strFile) {
                    std::printf("set %d: files missing from the disc\n", set);
                    ++problems;
                    continue;
                }
                const std::vector<uint8_t> textBytes = disc.ReadFile(*textFile);
                const rr::RaceGraph graph = rr::ParseRaceGraph(
                    std::string_view(reinterpret_cast<const char*>(textBytes.data()), textBytes.size()));
                const rr::RoadNetwork net = rr::ParseRoadNetwork(disc.ReadFile(*grfFile));
                const rr::StreamToc toc = rr::ParseStreamToc(disc.ReadFile(*tocFile));

                std::printf("set %d: %zu races (declared %d), %zu nodes, %zu roads, gmagic %08X\n", set,
                            graph.races.size(), graph.declaredEntries, net.nodes.size(), net.roads.size(), net.gmagic);
                if (graph.races.empty() || net.gmagic != toc.gmagic) {
                    std::printf("  FAIL gmagic differs between .GRF and .TOC\n");
                    ++problems;
                }
                for (const rr::Race& race : graph.races)
                    if (race.gmagic != net.gmagic) {
                        std::printf("  FAIL race %d gmagic %08X != network %08X\n", race.raceId, race.gmagic,
                                    net.gmagic);
                        ++problems;
                    }

                // Link direction: +1 when the node is road.nodeA, -1 when it is road.nodeB.
                int badLinks = 0;
                for (const rr::NetworkNode& n : net.nodes)
                    for (int k = 0; k < n.linkCount; ++k) {
                        const int32_t roadId = n.linkRoad[static_cast<size_t>(k)];
                        if (roadId < 0 || roadId >= static_cast<int32_t>(net.roads.size())) { ++badLinks; continue; }
                        const rr::NetworkRoad& road = net.roads[static_cast<size_t>(roadId)];
                        const int32_t expected = (road.nodeA == n.id) ? 1 : -1;
                        if (n.linkDir[static_cast<size_t>(k)] != expected) ++badLinks;
                    }
                std::printf("  links with the outgoing direction convention: %d bad\n", badLinks);
                problems += badLinks;

                // Routes close, and the distance identity holds.
                size_t routesWalked = 0, distanceChecks = 0;
                std::vector<std::string> failures;
                for (const rr::Race& race : graph.races) {
                    if (race.intersections.empty()) continue;
                    rr::WalkRoute(race);
                    ++routesWalked;
                    distanceChecks += rr::CheckRouteDistances(race, net, failures);
                }
                std::printf("  routes that close: %zu; distance identity: %zu checks, %zu failures\n", routesWalked,
                            distanceChecks, failures.size());
                for (const std::string& f : failures) std::printf("    FAIL %s\n", f.c_str());
                problems += static_cast<int>(failures.size());

                // The TOC tiles the whole .STR with 0x4000 chunks, gapless.
                uint64_t cursor = 0;
                int tilingProblems = 0;
                for (const rr::StreamRoad& r : toc.roads) {
                    const uint32_t ranges[2][2] = {{r.forwardOffset, r.forwardSize}, {r.reverseOffset, r.reverseSize}};
                    for (const auto& range : ranges) {
                        if (range[0] % 0x4000 != 0 || range[1] % 0x4000 != 0) ++tilingProblems;
                        if (range[0] != cursor) ++tilingProblems;
                        cursor = static_cast<uint64_t>(range[0]) + range[1];
                    }
                }
                const bool endsAtFileEnd = cursor == strFile->size;
                std::printf("  TOC tiling of STREAM%d.STR: %d problems, ends at %llu vs file size %u -> %s\n", set,
                            tilingProblems, static_cast<unsigned long long>(cursor), strFile->size,
                            endsAtFileEnd ? "exact" : "MISMATCH");
                problems += tilingProblems + (endsAtFileEnd ? 0 : 1);
            }
            std::printf("%s\n", problems == 0 ? "roadcheck: all invariants hold" : "roadcheck: FAILURES above");
            return problems == 0 ? 0 : 1;
        }
        return Usage();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
