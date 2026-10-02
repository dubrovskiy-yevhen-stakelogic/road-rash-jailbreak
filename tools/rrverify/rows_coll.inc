// Bench rows for the contact and impact ports. Included by
// verify_physics.cpp inside its anonymous namespace, AFTER rows_collision.inc (whose helpers in `co`
// - CloneCallees, FaithfulOracle, BenchCaller, Refuse, MoveNear, Pool0, PlantVolumes - these rows use).
//
// One file per group, each with its own Add function; the order below is fixed (a row's random cases
// are seeded by its index, so a group appends its rows at the END of its own function only).
#include "rows_coll_pair.inc"
#include "rows_coll_solve.inc"
#include "rows_coll_speed.inc"
#include "rows_coll_react.inc"
#include "rows_coll_resolve.inc"

void AddCollRows(std::vector<Row>& rows) {
    AddCollPairRows(rows);    // rows_coll_pair.inc    (src\game\sim\contact.cpp)
    AddCollSolveRows(rows);   // rows_coll_solve.inc   (src\game\sim\impact_solve.cpp)
    AddCollSpeedRows(rows);   // rows_coll_speed.inc   (src\game\sim\hit_speed.cpp)
    AddCollReactRows(rows);   // rows_coll_react.inc   (src\game\sim\bike_react.cpp)
    AddCollResolveRows(rows); // rows_coll_resolve.inc (src\game\sim\resolvers.cpp)
}

// The extent [entry, end) of each function these rows accept, for `--coverage` (from our own
// disassembly of RASHCDG: the next function's entry).
const CoverageRange* CollCoverage(const char* rowName) {
    struct Named {
        const char* name;
        CoverageRange range;
    };
    static const Named kRanges[] = {
        {"pair_classify", {0x800ABE78, 0x800AC130}},       {"pair_view_extent", {0x800AA34C, 0x800AA474}},
        {"pair_response_far", {0x800AA140, 0x800AA34C}},   {"pair_push_dir", {0x800B5B48, 0x800B5EB4}},
        {"pair_heavy_response", {0x800AA474, 0x800AAD30}}, {"pair_response", {0x800AAD30, 0x800AB7A0}},
        {"bike_vs_bike", {0x800AB7A0, 0x800ABE78}},
    };
    for (const Named& n : kRanges)
        if (std::strcmp(n.name, rowName) == 0) return &n.range;
    return nullptr;
}
