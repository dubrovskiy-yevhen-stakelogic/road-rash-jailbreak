// Bench rows for the rider off the bike and back on it (src\game\sim\recover*.{h,cpp}).
// Included LAST by verify_physics.cpp inside its anonymous
// namespace, so every helper of the earlier row files (bs::ExactOracle, pop::, animrows::) is visible.
//
// One row per function. Every non-leaf callee of a port goes through rr::sim::RecoverCallees (recover.h):
// here it is answered by the ORIGINAL code at the port's own call-site sp (bs::ExactOracle), and the
// row declares it, so the guest's and the port's call sequences are compared call for call. The pure
// leaves (recover.h) run natively inside the ports and are not declared.
//
// The rows are split by group, one file each:
//   rows_recover_walk.inc    the walk back (AI op 18) and the re-seat   (recover_walk.h)
//   rows_recover_fall.inc    the fall off the bike and the flight       (recover_fall.h)
//   rows_recover_ground.inc  the rider on the ground                    (recover_ground.h)

namespace rcv {

// The seam, answered by the original at exactly the port's call-site sp.
struct RecoverOracle final : rr::sim::RecoverCallees {
    NativeEnv& env;
    explicit RecoverOracle(NativeEnv& e) : env(e) {}
    bool Call(uint32_t fn, const uint32_t* a, int n, uint32_t sp, uint32_t& v0) override {
        v0 = bs::ExactOracle(env, fn, a, n, sp);
        return env.failure.empty();
    }
};

// A view over the clone's RAM with the scratchpad attached, and the engine tables out of it.
inline rr::sim::GuestRam View(NativeEnv& env) {
    rr::sim::GuestRam g(env.clone->mem.ram().data(), env.gp);
    return g;
}
inline rr::sim::BikeTables Tables(NativeEnv& env, rr::sim::GuestRam& g) {
    return rr::sim::RecoverTables(env.clone->mem.ram().data(), g);
}
// The common end of a native side: a refused port fails the case loudly, a fault too.
inline uint32_t Done(NativeEnv& env, rr::sim::GuestRam& g, bool ok, const char* what, uint32_t v0 = 0) {
    if (!ok && env.failure.empty() && !g.Faulted()) env.failure = std::string(what) + " refused";
    return RqDone(env, g, v0);
}

} // namespace rcv

void AddRecoverWalkRows(std::vector<Row>& rows);   // rows_recover_walk.inc
void AddRecoverFallRows(std::vector<Row>& rows);   // rows_recover_fall.inc
void AddRecoverGroundRows(std::vector<Row>& rows); // rows_recover_ground.inc
void AddRecoverAirRows(std::vector<Row>& rows);    // rows_recover_air.inc

#include "rows_recover_walk.inc"
#include "rows_recover_fall.inc"
#include "rows_recover_ground.inc"
#include "rows_recover_air.inc"

void AddRecoverRows(std::vector<Row>& rows) {
    AddRecoverWalkRows(rows);
    AddRecoverFallRows(rows);
    AddRecoverGroundRows(rows);
    AddRecoverAirRows(rows);
}
