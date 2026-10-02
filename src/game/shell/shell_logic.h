#pragma once
// The front end's logic, PORTED from the shell overlay RASHCDF.BIN (sha1 a3fec4b4e9292c358d0f6dc529843f5d8f25924a,
// base 0x8005B5E8) and the few SLUS_010.53 (sha1 67ed165a2c517d4e6106fb0dfa324d66dd9a76f1) leaves it
// reaches. Each function is proven by its bench row (`rrverify phys --only shell_*`,
// tools\rrverify\rows_shell.inc).
//
// Every function works on the guest's own memory through rr::sim::GuestRam, at the original's
// addresses, exactly as the race ports do: the product keeps one 2 MiB arena for the shell
// (shell_arena.h) and the bench runs the same code on a clone of the captured machine.
//
// THE SEAM. A callee the port does not contain is handed to ShellCallees::Call with its guest
// address and its o32 arguments, at the point the original calls it. In the bench the oracle executes
// the original there and the two call sequences must match; in the product the front end's host does
// what that callee is for (the UI click, the music) or records that it was asked.
#include "game/sim/road_query.h"

#include <cstdint>

namespace rr::shell {

using rr::sim::GuestRam;

// ---------------------------------------------------------------------------- the shell's globals
// All read out of the functions' own lui/addiu pairs.
constexpr uint32_t kFe = 0x8009C5D0;          // the front-end context
constexpr uint32_t kFeCur = kFe + 0x00;       // s16 current screen
constexpr uint32_t kFeNext = kFe + 0x02;      // s16 requested screen
constexpr uint32_t kFeHub = kFe + 0x04;       // s16 hub after a race / modal
constexpr uint32_t kFeSweep = kFe + 0x08;     // s16 sweep / parent-chain cursor
constexpr uint32_t kFeMedia = kFe + 0x0A;     // u16 media handshake (bit 0 skippable, bit 2 skip asked)
constexpr uint32_t kFeStamp = kFe + 0x0C;     // s16 the splash's start frame
constexpr uint32_t kFeTrack = kFe + 0x0E;     // u8 music track
constexpr uint32_t kFeMusicOk = kFe + 0x0F;   // u8 music allowed
constexpr uint32_t kFeMusicOn = kFe + 0x10;   // u8 music playing
constexpr uint32_t kFeHold = kFe + 0x12;      // s8 frame countdown / screen 57's hold
constexpr uint32_t kFeEdges = kFe + 0x13;     // u8 THE SHELL'S EDGE MASK
constexpr uint32_t kFeOptMask = kFe + 0x14;   // u8 chooser option mask
constexpr uint32_t kFeObjArm = kFe + 0x15;    // s8 "the object arm is running"
constexpr uint32_t kFePort = kFe + 0x16;      // s8 the port the shell edits for
constexpr uint32_t kFeValid = kFe + 0x17;     // u8 the bound chooser's selectable option count
constexpr uint32_t kFeDeferred = kFe + 0x18;  // u8 the deferred cancel target under the abort modal
constexpr uint32_t kFeMusicRestart = kFe + 0x1E; // s8 gates the Square arm's music restart
constexpr uint32_t kFeSlider = kFe + 0x7C;    // Slider* bound slider descriptor (0x8009C64C)
constexpr uint32_t kFeSliderDst = kFe + 0x80; // u32* where its value is stored (0x8009C650)
constexpr uint32_t kFeItem = kFe + 0x84;      // Widget* selected widget (0x8009C654)
constexpr uint32_t kFeLastItem = kFe + 0x88;  // Widget* last frame's (0x8009C658)
constexpr uint32_t kFeChooser = kFe + 0x94;   // Chooser* bound chooser (0x8009C664)
constexpr uint32_t kFeOptPrev = kFe + 0xA4;   // Option* last frame's option (0x8009C674)
constexpr uint32_t kFeOpt = kFe + 0xA8;       // Option* the bound chooser's current option (0x8009C678)

constexpr uint32_t kCurScreen = 0x8009C5C8;   // Screen* the screen being handled
constexpr uint32_t kScreenTablePtr = 0x8009C68C; // -> the 59-entry screen pointer table
constexpr uint32_t kScreenTable = 0x800A0880;
constexpr int kScreenCount = 59;
constexpr uint32_t kNavAdvance = 0x8009C6C8;  // s32[59]
constexpr uint32_t kNavBack = 0x8009C7B8;     // s32[59]
constexpr uint32_t kSliderCommit = 0x8009C8A8; // s32[6]
constexpr uint32_t kScreenInput = 0x8009C8C0; // handler*[59]
constexpr uint32_t kCodeGroup = 0x8009C9B0;   // s32[81]
constexpr uint32_t kCodeScreen = 0x8009CAF8;  // s32[81]
constexpr uint32_t kObjectInput = 0x8009CC40; // handler*[37]
constexpr uint32_t kChoosers = 0x8009C4B0;    // Chooser*[37]
constexpr uint32_t kSliderDescs = 0x8009C548; // 16-byte slider descriptors
constexpr uint32_t kIdle = 0x8005ACAC;        // s32 the attract idle counter
constexpr uint32_t kVblankCount = 0x80088C40; // u32 the counter the splash times against
constexpr uint32_t kGameStatePtr = 0x8005B2F8;
constexpr uint32_t kGameState = 0x800D5D38;
constexpr uint32_t kDemo = 0x8005B220;        // the attract path's flag
constexpr uint32_t kDebugFlags = 0x8005AE7C;

constexpr uint32_t kPads = 0x800D7128;        // PollPads' snapshot, 192 bytes per player
constexpr uint32_t kPadStride = 192;
constexpr uint32_t kPadLive = 0x800D6DE0;     // the live records the snapshot is taken of
constexpr uint32_t kMultitapByte = 0x800D70E1; // the device nibble of the SIO0 receive buffer
constexpr uint32_t kPorts = 0x800D7428;       // four 24-byte per-port records
constexpr uint32_t kSession = 0x800D80D8;     // the career/session record (rules.md 1.2)
constexpr uint32_t kPlayers = 0x800D81D8;     // six 0x24-byte player records (rules.md 1.3)
constexpr uint32_t kProgress = 0x800D81C8;    // session+0xF0, 65-bit race bitmap
constexpr uint32_t kMissions = 0x800D81D4;    // session+0xFC, 18-bit bitmap
constexpr uint32_t kRecords = 0x80053A88;     // SLUS: the records table the card's region is seeded from

// Pad slot press codes: record + 0x14 + 8*slot + 6 = record + 0x1A + 8*slot.
constexpr uint32_t PadSlot(uint32_t pad, int slot) { return kPads + kPadStride * pad + 0x1Au + 8u * static_cast<uint32_t>(slot); }
enum Slot : int { kLeft = 0, kRight = 1, kUp = 2, kDown = 3, kSquare = 4, kCircle = 5, kTriangle = 6, kCross = 7 };

// Callees the port reaches but does not contain.
constexpr uint32_t kUiSound = 0x8007EAC0;     // PlayUiSound(n)        RASHCDF
constexpr uint32_t kMusicPlay = 0x8007EDE0;   // MusicPlay(track)      RASHCDF
constexpr uint32_t kMusicStop = 0x8007F158;   // MusicStop(flag)       RASHCDF
constexpr uint32_t kSoundMode = 0x8001EFDC;   // stereo/mono switch    SLUS
constexpr uint32_t kSoundModeUi = 0x8007ED34; // RASHCDF
constexpr uint32_t kJukeboxSet = 0x8002490C;  // SLUS
constexpr uint32_t kSplashFade = 0x80022A78;  // SLUS
constexpr uint32_t kSkipMovie = 0x8006FED4;   // RASHCDF
constexpr uint32_t kChooserPass = 0x8006738C; // RASHCDF, the per-frame chooser validation pass
constexpr uint32_t kPadInstall = 0x8001B67C;  // SLUS, fn(session+0x17)

class ShellCallees {
public:
    virtual ~ShellCallees() = default;
    // One call of an unported callee. `v0` may be null when the caller ignores the result. Returns
    // false when the call could not be made (the bench then fails the case).
    virtual bool Call(uint32_t address, const uint32_t* args, int count, uint32_t* v0) = 0;
    bool Call1(uint32_t address, uint32_t a0, uint32_t* v0 = nullptr) {
        const uint32_t a[1] = {a0};
        return Call(address, a, 1, v0);
    }
    // A primitive a PORTED widget handler has just written into the packet buffer and linked into the
    // ordering table (shell_widgets.h), told to the host in its own terms so the product's view can draw
    // it without a GPU. The bench ignores it (the packet itself is compared as RAM).
    struct DrawNote {
        enum Kind { Sprite, Tile, Box } kind = Sprite;
        uint32_t fourcc = 0;      // Sprite: the sprite record's FourCC
        int32_t srcX = 0, srcY = 0; // Sprite: the frame's offset inside the picture
        int32_t x = 0, y = 0, w = 0, h = 0;
        uint32_t rgb = 0;         // 0x00BBGGRR as the packet carries it
        bool semi = false;        // Tile / Box: semi-transparent (the packet's code bit 1)
        uint32_t ot = 0;          // the ordering-table entry it was linked into
    };
    virtual void Drew(const DrawNote&) {}
};

// ---------------------------------------------------------------------------- the ported functions
// Each carries its original's address; `sp` where given is the original's stack pointer at entry (a
// port that reaches a callee through the seam calls it at the depth the original would have).

uint32_t Rand(GuestRam& g);                                        // SLUS 0x8001FC58
void ClearProgress(GuestRam& g);                                    // SLUS 0x8002D250
void ClearMissions(GuestRam& g);                                    // SLUS 0x8002D298
void MemSet(GuestRam& g, uint32_t dst, uint8_t value, uint32_t n);  // SLUS 0x8001E100
void PadConfig(GuestRam& g, uint32_t record, int32_t index);        // SLUS 0x8001C4A8

int32_t FindByCode(GuestRam& g, uint32_t screen, int32_t code);    // RASHCDF 0x8006C700
bool MoveNext(GuestRam& g, ShellCallees& k, uint32_t screen);       // RASHCDF 0x8006C3A4
bool MovePrev(GuestRam& g, ShellCallees& k, uint32_t screen);       // RASHCDF 0x8006C558
void BindSlider(GuestRam& g, uint32_t screen, uint32_t widget);     // RASHCDF 0x80064B30
bool MenuInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t padFirst, int32_t padEnd,
               int32_t* result);                                    // RASHCDF 0x8006B03C
bool DefaultScreenInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result); // 0x80069418
int32_t StartRaceInput(GuestRam& g, uint32_t screen);              // RASHCDF 0x8006C354 - THE HANDOVER
void SetMode(GuestRam& g, uint32_t widget);                         // RASHCDF 0x800685BC
int32_t FirstItem(GuestRam& g, uint32_t screen);                   // RASHCDF 0x80080A70

// Choosers: {s8 count; u8 ?; s8 kind; s8 current; Option* options}, options
// 12 bytes {u8 gate; u8 value; u8 flags; u8 mask; ...}.
bool OptionAvailable(GuestRam& g, uint32_t option);                // the gate every chooser walk applies
void ChooserSet(GuestRam& g, uint32_t chooser, int32_t value);     // RASHCDF 0x800630C0
int32_t ChooserFirstValid(GuestRam& g, uint32_t chooser);          // RASHCDF 0x80062D6C
int32_t ChooserCount(GuestRam& g, uint32_t chooser);               // RASHCDF 0x800649BC
bool ChooserPrev(GuestRam& g, ShellCallees& k, uint32_t chooser);   // RASHCDF 0x8006460C
bool ChooserNext(GuestRam& g, ShellCallees& k, uint32_t chooser);   // RASHCDF 0x800647E4
bool ChooserApply(GuestRam& g, ShellCallees& k, uint32_t chooser);  // RASHCDF 0x8006310C
void BindChooser(GuestRam& g, uint32_t screen, uint32_t widget);    // RASHCDF 0x800680E8
bool ObjectInput(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result); // 0x8006B7BC (+0x8006B800's f15 arms)
void SetOptionMask(GuestRam& g, int32_t venue);                     // RASHCDF 0x80064254

// The career record's readers (frontend.md 7.3, 11.2).
int32_t AllDone(GuestRam& g, int32_t first, int32_t last);         // RASHCDF 0x8007E724 (+0xF0)
int32_t AllDoneMissions(GuestRam& g, int32_t first, int32_t last); // RASHCDF 0x8007E770 (+0xFC)
int32_t BeatsRecord(GuestRam& g, int32_t raceId, uint32_t value);  // RASHCDF 0x8007E7BC

// Screens (frontend.md 3.3/3.4).
void ArmScreen(GuestRam& g, int32_t id);                            // RASHCDF 0x800809A8
void GotoScreen(GuestRam& g, int32_t id);                           // RASHCDF 0x800809E0
bool ChangeScreen(GuestRam& g, ShellCallees& k);                    // RASHCDF 0x80066EF8
bool CommitScreen(GuestRam& g, ShellCallees& k);                    // RASHCDF 0x8006711C
void NewGame(GuestRam& g, int32_t screenId);                        // RASHCDF 0x80068688
void CareerSetup(GuestRam& g, int32_t screenId);                    // RASHCDF 0x8006883C

// Per-screen input handlers.
int32_t MovieInput(GuestRam& g, ShellCallees& k, uint32_t screen, bool* ok);  // RASHCDF 0x8006A8FC
int32_t SplashInput(GuestRam& g, ShellCallees& k, uint32_t screen, bool* ok); // RASHCDF 0x8006AC80

// A handler held in one of the shell's function tables, called as the original calls it: h(screen).
bool CallHandler(GuestRam& g, ShellCallees& k, uint32_t handler, uint32_t screen, int32_t* result);
bool InputPass(GuestRam& g, ShellCallees& k);                       // RASHCDF 0x800667E4
void PassChoosers(GuestRam& g);                                     // RASHCDF 0x80068448
int32_t FirstSelectable(GuestRam& g, uint32_t screen);             // RASHCDF 0x80080ACC
bool ChooserPass(GuestRam& g, ShellCallees& k, uint32_t players);   // RASHCDF 0x8006738C

// The card record checksum (frontend.md 8.5).
void CardChecksum(GuestRam& g, int32_t slot, uint32_t outA, uint32_t outB);             // RASHCDF 0x8005E9D0
void CardChecksumHost(const uint8_t* record, int32_t slot, uint32_t* a, uint32_t* b);    // the same, on a host buffer

// The commit of the menu selection into game_state (rules.md 2.3).
bool CommitSelection(GuestRam& g, ShellCallees& k);                 // RASHCDF 0x8007F37C

} // namespace rr::shell
