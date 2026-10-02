#pragma once
// The race camera - ViewUpdate and the fourteen functions under it - ported from the RASHCDG overlay
// (SHA-1 cfe43a7786759f2cb9c57751cf99e84d1074782c, loaded at 0x8005B5E8) and one leaf of the
// resident executable SLUS_010.53 (SHA-1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1).
// This file carries the addresses.
//
//   RASHCDG 0x800881B4  ViewUpdate        10212 bytes  the per-player camera step
//   RASHCDG 0x80087420  CameraAim          3360 bytes  look point, aim angles, the aim frame +0x24C
//   RASHCDG 0x80086E1C  CameraOrient        908 bytes  the render frame +0x1B0 (yaw +0x2E8, pitch +0x2EC)
//   RASHCDG 0x8008676C  CameraTransition    908 bytes  the eased cut between two camera placements
//   RASHCDG 0x800871A8  CameraLead          632 bytes  a point `lead` units from b toward a (+0x464)
//   RASHCDG 0x80086584  CameraToFrame       488 bytes  a point in the aim frame
//   RASHCDG 0x800863EC  CameraToPolar       408 bytes  (x, y, z) -> (angle in radians 16.16, y, r)
//   RASHCDG 0x80086C00  CameraAngleChase    340 bytes  angle follower with a rate and a 2-unit floor
//   RASHCDG 0x8008A998  CameraSetMode       280 bytes  select a camera (+0x21C / +0x220 / +0x304)
//   RASHCDG 0x80086B1C  CameraChaseSpring   228 bytes  the along-track distance spring (+0x2DC)
//   RASHCDG 0x80086D54  CameraAngleSpring   200 bytes  angle spring with rate (+0x26C / +0x268)
//   SLUS    0x8002FA28  Hermite             172 bytes  the cubic Hermite basis of t
//   RASHCDG 0x80088140  CameraZones         116 bytes  RoadsideZones with the lateral mirrored
//   RASHCDG 0x8008AAB0  CameraResetFlags     80 bytes  +0x224: 0x8000 -> 0x10006, 0x40000 -> 6
//   RASHCDG 0x80086AF8  CameraSpringReset    36 bytes  zero the springs, speed follower = speed
//
// Each is accepted by its own row of `rrverify phys` (tools\rrverify\rows_camera.inc): 0 mismatches
// over the whole guest RAM outside the stack window and the whole scratchpad, on dump-derived and
// randomised inputs.
//
// NOT PORTED - reached through `CameraSeams` and executed by the oracle in the bench:
//   RASHCDG 0x800853E4 (4104 bytes, the director's shot-script reader), SLUS 0x8002F634 (864 bytes,
//   the spline slope solver), SLUS 0x80018C1C (the race-over signal) and SLUS 0x80043F00 (GetRCnt,
//   a hardware root counter). The first three are reached only when the view record's +0x304 is set
//   (the director / replay camera, never in a race capture); GetRCnt is also read by the off-road
//   shake of the race camera.
//
// MEMORY MODEL. Everything is over road_query.h's guest-address view (`GuestRam`): the view record
// (0x800CD898 + 1132 p), the followed bike and its rider, CAMERA.CA at 0x800CD7B8, the director's
// shot table at 0x800D83B0 and the road the view record is itself bound to (ViewUpdate re-binds the
// view record to the road like an entity, through the ported SLUS 0x800374D4).
//
// STACK. Every function takes `sp`, the stack pointer at its entry, and computes its frame as the
// original's prologue does. Locals whose address the original hands to a callee, and the ones it may
// read before this call wrote them, live at the original's frame offsets in guest memory; a read of
// such a slot that THIS call has not written is a stale-stack read whose value a port cannot know,
// and the port DECLINES it (`CameraPort::Failed`) instead of inventing it. Three such reads exist;
// none is reached by a race frame.
#include <cstdint>

#include "game/sim/integrator.h"
#include "game/sim/road_query.h"
#include "game/sim/road_runtime.h"

namespace rr::sim {

// ---------------------------------------------------------------------------- the data
constexpr uint32_t kCameraFile = 0x800CD7B8;       // DATA\CAMERA.CA, its first 224 bytes (RASHCDI 0x80069618)
constexpr uint32_t kCameraRecordBytes = 56;        // 4 records, indexed by +0x21C / +0x220
constexpr uint32_t kCameraViews = 0x800CD898;      // the per-player view records
constexpr uint32_t kCameraViewBytes = 1132;
constexpr uint32_t kShotTable = 0x800D83B0;        // s32 count, 26-byte shots at +4, s16 index at +0x20C, points at +0x214
constexpr uint32_t kShotRecords = 0x800D83B4;
constexpr uint32_t kRiderKindTable = 0x800541D4;   // 8-byte records, +2 the category (8 = ...)
constexpr uint32_t kCameraGameState = 0x8005B2F8;  // -> game state (+0x04 flags, +0x39 u8)
constexpr uint32_t kCameraPlayerBikes = 0x8005B268; // Bike*[2]
constexpr uint32_t kCameraPlayer1Bike = 0x8005B38C;
constexpr uint32_t kCameraPlayer2Bike = 0x8005B21C;
constexpr uint32_t kCameraSinCos = 0x8005624C;     // {s16 sin; s16 cos}[4096]

// ---------------------------------------------------------------------------- the seams
// The unported callees. A `false` return is a refusal: the call fails, it is never guessed.
class CameraSeams {
public:
    virtual ~CameraSeams() = default;
    // SLUS 0x80043F00 GetRCnt(0xF2000002): root counter 2. Called at 0x80088AC4 and 0x80089808.
    virtual bool GetRCnt(uint32_t id, uint32_t& value) = 0;
    // RASHCDG 0x800853E4 ShotSetup(v, script), called at 0x80088B24.
    virtual bool ShotSetup(GuestRam& m, uint32_t v, uint32_t script) = 0;
    // SLUS 0x8002F634 SplineSlopes(x, y, out, n), called at 0x80088CEC, 0x80088D04, 0x80089334,
    // 0x8008A128.
    virtual bool SplineSlopes(GuestRam& m, uint32_t x, uint32_t y, uint32_t out, int32_t n) = 0;
    // SLUS 0x80018C1C(0), called at 0x80088780.
    virtual bool RaceOverSignal(uint32_t a0) = 0;
};

// ---------------------------------------------------------------------------- the port
class CameraPort {
public:
    CameraPort(GuestRam& m, const BikeTables& t, CameraSeams& seams, RoadRuntimeCallees& road)
        : m_(m), t_(t), seams_(seams), road_(road) {}

    // RASHCDG 0x800881B4 ViewUpdate(View *v, s32 dt).
    void ViewUpdate(uint32_t v, int32_t dt, uint32_t sp);
    // RASHCDG 0x80087420 CameraAim(View *v, s32 dt).
    void Aim(uint32_t v, int32_t dt, uint32_t sp);
    // RASHCDG 0x80086E1C CameraOrient(View *v).
    void Orient(uint32_t v, uint32_t sp);
    // RASHCDG 0x8008676C CameraTransition(View *v, s32 dt).
    void Transition(uint32_t v, int32_t dt, uint32_t sp);
    // RASHCDG 0x800871A8 CameraLead(const s32 a[3], const s32 b[3], s32 k, View *v, s32 out[3]);
    // `out` is the fifth o32 argument (sp+16).
    void Lead(uint32_t a, uint32_t b, int32_t k, uint32_t v, uint32_t out, uint32_t sp);
    // RASHCDG 0x80086584 CameraToFrame(View *v, const s32 origin[3], const s32 p[3], s32 out[3]).
    void ToFrame(uint32_t v, uint32_t origin, uint32_t p, uint32_t out);
    // RASHCDG 0x800863EC CameraToPolar(s32 p[3]).
    void ToPolar(uint32_t p, uint32_t sp);
    // RASHCDG 0x80086C00 CameraAngleChase(s32 *angle, s32 target, s32 dt, s32 rate).
    void AngleChase(uint32_t angle, int32_t target, int32_t dt, int32_t rate);
    // RASHCDG 0x80086D54 CameraAngleSpring(s32 *angle, s32 target, s32 dt, s32 *rate, s32 k, s32 c);
    // k and c are the fifth and sixth o32 arguments (sp+16, sp+20).
    void AngleSpring(uint32_t angle, int32_t target, int32_t dt, uint32_t rate, int32_t k, int32_t c);
    // RASHCDG 0x8008A998 CameraSetMode(View *v, u32 mode).
    void SetMode(uint32_t v, uint32_t mode);
    // RASHCDG 0x80086B1C CameraChaseSpring(View *v, s32 dt, s32 x).
    void ChaseSpring(uint32_t v, int32_t dt, int32_t x);
    // RASHCDG 0x80086AF8 CameraSpringReset(View *v).
    void SpringReset(uint32_t v);
    // RASHCDG 0x8008AAB0 CameraResetFlags(View *v).
    void ResetFlags(uint32_t v);
    // RASHCDG 0x80088140 CameraZones(View *v).
    void Zones(uint32_t v, uint32_t sp);
    // SLUS 0x8002FA28 Hermite(t, *h00, *h01, *h10, *h11); h11 is the fifth o32 argument (sp+16).
    void Hermite(int32_t t, uint32_t h00, uint32_t h01, uint32_t h10, uint32_t h11);

    // A refusal (a seam refused, a guest-memory fault, a stale-stack read, a loop past its bound):
    // everything this port wrote since is NOT what the console does and must be thrown away.
    bool Failed() const { return failed_ || m_.Faulted(); }
    uint32_t FailAddress() const { return failAt_; }
    const char* FailReason() const { return why_; }

private:
    void Fail(uint32_t at, const char* why) {
        if (!failed_) { failed_ = true; failAt_ = at; why_ = why; }
    }
    GuestRam& m_;
    const BikeTables& t_;
    CameraSeams& seams_;
    RoadRuntimeCallees& road_;
    bool failed_ = false;
    uint32_t failAt_ = 0;
    const char* why_ = "";
};

} // namespace rr::sim
