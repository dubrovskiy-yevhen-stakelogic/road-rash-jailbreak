#pragma once
// Wheelies. OURS: nothing here is the original's and nothing here edits a ported function.
//
// WHAT THE ORIGINAL HAS (read out of our own ports of RASHCDG): the bike's pitch
// +0x268 (16.16 radians) is a real "pitch move" - E3 (0x80077E98) starts a POSITIVE move (a wheelie pop) when the
// longitudinal load s5 passes 1.0 (a hard start), or when flagsA bits 11 + 12 (0x1800) are set and the speed +0x240 is
// above 0x23C36 (2.24 units/s) - and BikeCrashTimer 0x80074E6C (0x80074F50) sets 0x1800 when the throttle is held with
// the pad reader's press-window bit flagsA 0x0004 and no nitro is taken (+0x350 > 0 with the Down control's 0x0008 takes
// the nitro instead): measured, a DOUBLE-TAP of the throttle (released and pressed again within a few frames) - a single
// re-press does not; its target is stats[+0x10C] (that tap) or stats[+0x108] (the load), scaled down on a slope (+0x212), and it
// fires the rider's stance event 22 (0x80078058). E4 (0x80078390) runs it as a second-order move toward the target
// (+0x26C +-2.0 rad/s, +0x270 its acceleration), the integrator (0x8007C450) lets it fall back at stats[+0x1A8] and clamps
// it to +-1.2217 rad; the heading writer 0x8007AC04 puts it into the drawn rows +0x1B0 = RotMatrix(-pitch, 0, -roll) x
// +0x204 and lifts the box centre by it. A NEGATIVE move (a stoppie) starts on hard braking (s5 <= -1.0). So the original
// pops a short automatic wheelie - off the line (the AI too) and on a throttle double-tap (1/20, 40 units/s: up to
// 39..41 deg in ~0.7 s and down again in ~0.7 s) - but no wheelie the player holds, balances or steers: no
// input keeps +0x268 up. This layer's pitch adds only what it holds above +0x268 (never drawn twice), and a car hit in
// the original's own pop counts as "in a wheelie" too.

//
// WHAT THIS ADDS (after GTA SA's CBike, as in the re3-miami-vr project's src\vehicles\Bike.cpp: m_fLeanInput lagged 0.2 per
// 1/50 s, the lean-back force x (0.5 gas + 0.5), the wheelie stabilised about fWheelieAng (PCJ-600 35 deg) inside a band
// and hardly at all past it; the gta-sa-vr-quest project's native\src\Driving.cpp UpdateBikeLeanLocked: both hands raised
// from their height at the grab = lean back). A held, balanced wheelie as OUR layer:
//   * the input: "lean back" 0..1 - the Down action (key F, the d-pad) or a stick held near full travel back (desktop,
//     gamepad, VR Stick mode), or in the VR Handlebars mode both hands raised over the drawn grips they hold
//     (tools\rrgame\vr_wheelie.h; the stick and Down do not lean back there). A deliberate start: the request held
//     WheelieSettings::holdMs (200 ms) with the throttle open and the bike under way before the front rises; let go
//     (the request under its release point, the throttle shut) it ends. SA's lag (tau 0.09 s).
//   * the state: a drawn pitch about the rear wheel's contact line, a spring toward lean x throttle x the hold angle
//     (35 deg, SA's fWheelieAng) with SA's weight (a slight overshoot), sustained only by the throttle: off the throttle, a
//     brake, a crawl (< 3 units/s) or leaving the bike brings the front down. The balance point is 20 deg past the hold
//     angle: with the loop-over option ON a wheelie held at full pull and full throttle creeps past it and loops over -
//     the game's own fall (the loop-out trigger's writes of region C 0x800762D8..0x800763FC: flagsC |= 0x820); OFF (the
//     default) the pitch stops short of it.
//   * render only: the drawn bike, the rider on it, the head camera and the VR grips / hands / eye follow the pitch
//     (vr_visual_lean.h's second turn); the guest's +0x1B0 and +0x268 are never written. Steering with the front wheel in
//     the air is half as strong (the analogue pad's X byte, as SA's front wheel off the ground); the speed is the
//     original's (no drive or drag of ours).
//   * a CAR hit in a wheelie (overCars): the ORIGINAL decides whether the bike touches the car (BikeVsTraffic 0x800AC5BC
//     runs as always and reports its contact code); on a contact the product undoes that call (the guest RAM as it was
//     before it) and runs the game's own launch 0x80084BE8 (Launch(bike, 1): the angle and lift the game gives a thing
//     thrown off a car - 170/4096 of a turn, 0xB1333 - flagsC |= 0xC00) instead: the bike flies over the car on the
//     game's airborne path and lands through its own AirContact / TouchDown; contacts with cars during that flight are
//     undone the same way. The clearly marked alternative outcome: WheelieServeBikeVsTraffic (coll_product.cpp).
// Off (the option off, or "with Modern" under Original handling): every entry returns at once, the pad and the guest are
// untouched, nothing is printed.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "game/race_session.h" // PadState

namespace rr::sim {
class GuestRam;
struct CollisionCallees;
struct BikeTables;
} // namespace rr::sim

namespace rr::game {

enum class WheelieMode : int { kModern = 0, kAlways = 1, kOff = 2 };
const char* WheelieModeName(WheelieMode m); // "modern" / "on" / "off"

// [handling] wheelie=modern|on|off, wheelie_angle=20..50, wheelie_loop=on|off, wheelie_cars=on|off,
// wheelie_view_pitch=0..100 (a member of HandlingSettings: the file, the flags and both menus' page).
struct WheelieSettings {
    WheelieMode mode = WheelieMode::kModern; // "with Modern handling" (the default) / always / never
    int holdDeg = 35;                        // the balanced angle (SA fWheelieAng, PCJ-600 35)
    bool loopOver = false;                   // past the balance point the bike loops over (the game's own fall)
    bool overCars = true;                    // a car hit in a wheelie: over it (the game's launch) instead of knocked down
    int viewPitchPct = 30;                   // the VR head view keeps this % of the drawn pitch (the eye rides it whole)
    // The deliberate start (every input): the lean back asked for this long, with the throttle open and the bike under
    // way, before the front rises; let go (the input under its release point, or the throttle shut) it ends.
    int holdMs = 200;                        // wheelie_hold_ms 0..1000
    // The VR Handlebars gesture (vr_wheelie.h): BOTH hands raised this far above where they hold the drawn grips
    // start it (each at least three quarters of it); raised to liftFullCm is full lean back; under half of liftCm it
    // lets go.
    int liftCm = 14;                         // wheelie_lift_cm 5..40
    int liftFullCm = 30;                     // wheelie_full_cm 10..60 (at least liftCm + 5 in use)
    float LiftStart() const { return static_cast<float>(liftCm) / 100.0f; }
    float LiftFull() const { return static_cast<float>(std::max(liftFullCm, liftCm + 5)) / 100.0f; }
    float LiftRelease() const { return 0.5f * LiftStart(); }
    bool Parse(const std::string& key, const std::string& value);
    std::string Serialize() const; // the section's lines
    std::string Describe() const;  // one log line
    bool On(bool modernHandling) const {
        return mode == WheelieMode::kAlways || (mode == WheelieMode::kModern && modernHandling);
    }
};

// A scripted run's wheelie input (a test input, like --steer-script; --wheelie-script "F v; ..."): from frame F on,
// v is held: W (full lean back), a number 0..1 (a stick pulled back that far), 0 (nothing), N (nothing and the throttle
// released), B (the brake, the throttle released).
struct WheelieScript {
    struct Step {
        long frame;
        float pull;
        char throttle; // 'k' keep the pad's, 'N' released, 'B' brake
    };
    std::vector<Step> steps;
    bool Parse(const std::string& text);
    const Step* At(long frame) const;
};

// What main.cpp hands the layer before the frame.
struct WheelieIn {
    const uint8_t* ram = nullptr;
    uint32_t bike = 0;
    bool vrRun = false;
    bool modern = false;      // the handling this frame is Modern (handling_modern.h)
    bool paused = false;      // game state 3 / 4
    bool twoPlayers = false;  // never in split screen
    // A stick pulled back, 0..1 of its travel with no dead zone (the desktop controller's / the Touch left stick's Y;
    // 0 without one). The Down action (the key F, the d-pad; the stick past a third through the bindings) is the pad's
    // padDown - taken as a digital lean back only when the stick is not what pressed it.
    float stickBack = 0.0f;
    // The VR Handlebars mode: the bars' gesture is the only lean back (the stick and Down do not lean back there).
    bool vrHandlebars = false;
    // The VR Handlebars gesture (vr_wheelie.h WheelieBarsInput): barsKeep < 0 - not both hands on the bars; barsWant -
    // this frame meets the start (both hands high enough); barsKeep - the lean back while a wheelie is under way.
    bool barsWant = false;
    float barsKeep = -1.0f;
    float barsLow = 0.0f, barsMean = 0.0f; // the gesture's lifts this frame (metres; the CSV only)
    float barsLegacy = -1.0f;              // the earlier gesture's pull (RRJB_WHEELIE_INPUT=legacy only); < 0 none
    long frame = 0;
};

class Wheelie {
public:
    Wheelie() = default;
    Wheelie(const Wheelie&) = delete;
    Wheelie& operator=(const Wheelie&) = delete;
    ~Wheelie();
    void BeginRace();
    // Before the handling layer (handling_modern.h ApplyToPad): the script's throttle / brake, the test aim at a car
    // (--wheelie-aim-car), the lean-back input read.
    void BeforeHandling(const WheelieIn& in, PadState& pad);
    // After it: the steering with the front wheel up (the analogue X byte toward the centre).
    void AfterHandling(PadState& pad);
    // After the frame: the state, the loop-over, the flight after a launch, the metrics, the CSV (--wheelie-log).
    void AfterFrame(uint8_t* ram, uint32_t bike, int32_t dt, double routeDistance, long frame);
    // The drawn pitch this frame (radians, nose up > 0); false: none (off, or the front wheel down).
    bool DrawnPitch(float& radians) const;
    // the whole held pitch (radians, > 0) - DrawnPitch plus the original's own pop under it; false: none.
    // The VR head view's ViewPitch (tools\rrgame\vr_horizon.h) leaves this much of the game's own pitch to the layer.
    bool HeldPitch(float& radians) const;
    // The VR head view's kept share of the pitch (WheelieSettings::viewPitchPct as 0..1).
    float ViewPitchKeep() const;
    // The car hook (coll_product.cpp): whether BikeVsTraffic(bike, car) of the player is ours to judge this frame.
    bool CarHookArmed(uint32_t bike) const;
    std::string Summary() const;
    bool Engaged() const { return engaged_ || !script.steps.empty() || !csvPath.empty(); }
    // The deliberate start's gate is open: the lean back was held (holdMs, the throttle, the speed) and not let go.
    bool InputEngaged() const { return gate_; }
    // The rider seated with the wheels on the road, as the last frame left it (the gesture's meters count only those).
    bool RidingOnGround() const { return seated_ && ground_; }

    WheelieScript script;
    std::string csvPath;  // --wheelie-log
    bool aimCar = false;  // --wheelie-aim-car: a TEST input - steer at the nearest car ahead (the car hit's run)
    long aimFrom = 0;     // ... from this frame on

    // the hook's report (WheelieServeBikeVsTraffic)
    void NoteContact(uint32_t car, bool launched, bool launchOk, long frameHint);

private:
    bool on_ = false, engaged_ = false, vr_ = false;
    // the deliberate start (WheelieSettings::holdMs): the request held so far (s), the gate, the last frame seen
    bool gate_ = false;
    float armed_ = 0.0f;
    long gateFrame_ = -1;
    long starts_ = 0, wantFrames_ = 0, shortWants_ = 0; // the metrics: gates opened; frames asked; asks let go too early
    bool wanting_ = false, want_ = false;
    float barsLow_ = 0.0f, barsMean_ = 0.0f, stick_ = 0.0f; // this frame's inputs (the CSV)
    uint32_t bike_ = 0;
    float pull_ = 0.0f, throttle_ = 0.0f, brake_ = 0.0f;
    float lean_ = 0.0f;         // SA's m_fLeanInput (lean back, 0..1)
    float pitch_ = 0.0f, rate_ = 0.0f, target_ = 0.0f, creep_ = 0.0f;
    bool ground_ = false, seated_ = false;
    bool looped_ = false;       // the loop-over fired (once until the rider is back on)
    // the flight after a car launch
    bool flying_ = false;
    long flightFrames_ = 0;
    uint32_t flightCar_ = 0;
    long frame_ = 0;
    // the metrics
    struct Metrics {
        long frames = 0, up = 0, lifts = 0, drops = 0, longest = 0, current = 0, loops = 0;
        double maxPitch = 0, sumPitch = 0, holdTime = 0;
        long contactsJudged = 0, launches = 0, launchFails = 0, flightContactsUndone = 0, landedSeated = 0,
             landedFallen = 0, loopFalls = 0;
        double distance = 0, firstDistance = -1;
        long firstLiftFrame = -1, firstLaunchFrame = -1, landFrame = -1;
        double maxFlightHeight = 0;
        double riseMs = -1, dropMs = -1; // the first lift: lean back to 30 deg; the first release: 20+ deg to the ground
        long origPops = 0; // the original's own pitch moves (+0x268 leaving 0) seen
        long falls = 0, firstFall = -1; // the rider off the bike (seated -> not), whatever the option
        double origMaxPitch = 0;
    } m_;
    int32_t lastOrigPitch_ = 0;
    bool wasSeated_ = false;
    double riseClock_ = -1, dropClock_ = -1; // the timings' running clocks (s), < 0 idle


    float origPitch_ = 0.0f; // the original's own pitch +0x268 this frame (radians)

    double flightBase_ = 0;
    std::FILE* csv_ = nullptr;
    std::string event_;
    void Crash(uint8_t* ram, uint32_t bike);
};

Wheelie& PlayerWheelie();

// The clearly marked alternative outcome of the player's bike against a car (the header's top):
// called by coll_product.cpp for BikeVsTraffic 0x800AC5BC before it serves the port. False: not armed (the port runs
// as always). True: handled here - the port ran, and on a contact its effects were undone and the game's launch run
// (or, in the flight, just undone); `ok` / `v0` as ServeCollNative's.
bool WheelieServeBikeVsTraffic(rr::sim::GuestRam& g, rr::sim::CollisionCallees& c, const rr::sim::BikeTables& t,
                               const uint32_t* a, int n, uint32_t sp, uint32_t& v0, bool& ok);

} // namespace rr::game
