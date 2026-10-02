#pragma once
// The memory-card screens 43..46 (Load Game, Load Records, Save Game, Save Records), PORTED from
// RASHCDF.BIN (sha1 a3fec4b4e9292c358d0f6dc529843f5d8f25924a, base 0x8005B5E8) by reading Ghidra's
// pseudo-C (work\ghidra\decomp_shell) against our objdump.
//
// What is here, each with its original's address:
//   * the four screen handlers 0x8006C06C / 0x8006C124 / 0x8006C1DC / 0x8006C290 (entry: the card
//     layer's enter, a 120-frame hold, state 21; then the machine every frame);
//   * the card screens' state machine 0x8006C770 (pad events -> the 27 x 3 transition table
//     0x80080D48 -> the 27 arms of jump table 0x8005BE24) and its helper 0x8006CD48;
//   * the slot copies SaveSlot 0x8006CDA4, LoadSlot 0x8006CE60 (with the re-dispatch of the loaded
//     career into the choosers, the sound options and game_state), SeedRecords 0x8006CE24 and
//     RestoreRecords 0x8006D10C;
//   * the card screens' text: the status panel (text-block kind 0x2D, 0x80071064) and the slot list
//     (kind 0x2E, 0x80071AF0) with its mode label 0x80071F70.
//
// THE CARD LAYER is a seam: enter 0x8005F158, leave 0x8005F180, the status poll 0x8005F21C, format
// 0x8005ED6C and write 0x8005EC28 (libcard and the game's driver over it).
// In the bench the oracle runs the original's; in the product CardDevice below answers them over a
// raw card image file, writing the card record 0x80099550 the way the original's leave it.
#include "game/shell/shell_card.h"
#include "game/shell/shell_logic.h"

#include <cstdint>
#include <string>

namespace rr::shell {

// ---------------------------------------------------------------------------- the card record
constexpr uint32_t kCardPtr = 0x8009954C;         // Card* (set by 0x8005EB80 to kCardRecord)
constexpr uint32_t kCardRecord = 0x80099550;      // 0x2D7C bytes, cleared by 0x8005EB80
constexpr uint32_t kCardRecordSize = 0x2D7C;
// +0x00 u8 presence (0 none, 1 present, 2 busy)  +0x01 file read  +0x02 formatted  +0x03 loaded
// +0x04 u8 card full  +0x05 u8 room / file  +0x06 u8 a slot failed its checksum  +0x07 u8 flag
// +0x08 s16 state (0..26)  +0x0A s16 previous state  +0x0E s16 the selected slot 0..9
// +0x10 s32 the driver status last seen  +0x20.. the save block (6948 bytes: +0x220 slot, +0x221 u8
// "records present", +0x224 the records table 0x630, +0x854 ten 484-byte career records)
// +0x1BC4.. the card driver's own data (*(0x800548CC)).
constexpr uint32_t kCardBlockAt = 0x20;           // offset of the save block in the record
constexpr uint32_t kCardDriver = 0x1BC4;          // offset of the driver's data
constexpr uint32_t kCardHold = 0x80088C48;        // s32 the entry hold, counted down by the vsync 0x80064C30
constexpr uint32_t kCardClock = 0x80088C4C;       // s32 frames since the last card event (same vsync, ++)
constexpr uint32_t kCardTransitions = 0x80080D48; // s32[27][3] next state by event (0 = none)
constexpr uint32_t kCardOp = 0x80099548;          // s32 the write in flight (0x11 create, 0x13 save)
constexpr uint32_t kCardRecordsSaved = kFe + 0x20; // s8 screen 46: "records written"
constexpr uint32_t kCardRecordsAsked = kFe + 0x21; // s8 screen 46: "overwrite asked"

// ---------------------------------------------------------------------------- the card layer (seams)
constexpr uint32_t kCardEnter = 0x8005F158;       // ()      0x8005EB80 record init + 0x8005E068 driver reset
constexpr uint32_t kCardLeave = 0x8005F180;       // ()
constexpr uint32_t kCardPoll = 0x8005F21C;        // ()      status 0x8005EEF8, then the state it implies
constexpr uint32_t kCardFormat = 0x8005ED6C;      // (10 start / 0 poll)
constexpr uint32_t kCardWrite = 0x8005EC28;       // (0x11 create / 0x13 save start, 0 poll)

// ---------------------------------------------------------------------------- the ported functions
bool CardMachine(GuestRam& g, ShellCallees& k, uint32_t screen, int32_t* result);     // 0x8006C770
void CardPrevState(GuestRam& g);                                                        // 0x8006CD48
void CardSaveSlot(GuestRam& g, int32_t slot);                                           // 0x8006CDA4
void CardSeedRecords(GuestRam& g);                                                      // 0x8006CE24
bool CardLoadSlot(GuestRam& g, ShellCallees& k, int32_t slot, int32_t* result);        // 0x8006CE60
int32_t CardRestoreRecords(GuestRam& g);                                                // 0x8006D10C
int32_t CardModeLabel(uint32_t mode);                                                   // 0x80071F70
// The four screen handlers; `handler` is one of 0x8006C06C, 0x8006C124, 0x8006C1DC, 0x8006C290.
bool CardScreenInput(GuestRam& g, ShellCallees& k, uint32_t handler, uint32_t screen, int32_t* result);
bool IsCardScreenHandler(uint32_t handler);
// Text-block arms (shell_text.cpp TextBlock calls them). `sp` is the arm's entry stack pointer.
bool CardStatusText(GuestRam& g, ShellCallees& k, uint32_t screen, uint32_t rect, int32_t state, uint32_t sp); // 0x80071064
bool CardSlotsText(GuestRam& g, ShellCallees& k, uint32_t rect, int32_t state, uint32_t sp);                  // 0x80071AF0

// ---------------------------------------------------------------------------- the product's card
// OURS: the card layer's five entry points answered over a raw 128 KiB card image (shell_card.h),
// the file next to rrgame.exe. The effects on the card record are the original's (read from
// 0x8005EB80, 0x8005EEF8, 0x8005F21C, 0x8005ED6C, 0x8005EC28 and the slot checks 0x8005EAB4); the
// timing is not: an operation the console needs frames for completes at the next poll. A missing file
// is a freshly formatted card (an empty .mcr) that is written on the first save; a file that
// is not a card is an unformatted card, which the screens offer to format.
class CardDevice {
public:
    void SetPath(const std::string& path) { path_ = path; }
    const std::string& Path() const { return path_; }
    // Handles `address` when it is one of the five; *handled = false otherwise.
    bool Call(GuestRam& g, uint32_t address, const uint32_t* args, int count, uint32_t* v0, bool* handled);
    std::string TakeNote() {
        std::string s;
        s.swap(note_);
        return s;
    }

private:
    void Enter(GuestRam& g);
    void Poll(GuestRam& g);          // 0x8005F21C
    uint8_t Status(GuestRam& g);     // 0x8005EEF8
    void Format(GuestRam& g, uint32_t op);
    void Write(GuestRam& g, uint32_t op);
    bool Ensure();
    std::string path_;
    CardImage image_;
    bool have_ = false, formatted_ = false, failed_ = false;
    int32_t status_ = 0;             // the driver status the poll reports
    bool formatting_ = false, writing_ = false;
    std::string note_;
};

} // namespace rr::shell
