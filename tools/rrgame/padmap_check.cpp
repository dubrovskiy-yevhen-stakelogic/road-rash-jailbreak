// rrgame --padmapcheck [--mutate]: the controllers' decoding and the bindings, end to end, on SYNTHETIC device
// states - a scripted run cannot press a real controller. For every physical button of every source (an XInput
// state, a WinMM JOYINFOEX, a DualSense's USB report, its Bluetooth 0x31 report with the CRC, its simple Bluetooth
// 0x01 report) the state is built byte by byte from the device's own layout (written here, independently of the
// decoders), decoded, mapped with the DEFAULT bindings, and the resulting pad word is compared with the
// original's bits this table names. Also: the analogue fold, the stick
// dominance, the Bluetooth CRC (a corrupted report must be refused), the DualSense output report (motors, trigger
// effects, CRC), the ini round trip, the rebinding capture and the keyboard. `--padmapcheck-mutate` swaps R1 / L1
// in the bindings: the check must FAIL.
//
// rrgame --dualsenseprobe: lists the connected DualSense controllers (HID 054C:0CE6 / 0DF2) and reads their live
// report for a second without writing anything to them (nothing to press).
#include "platform/dualsense.h"
#include "platform/gamepad_win32.h"
#include "platform/input_bindings.h"
#include "platform/pad_decode.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {

using rr::platform::PadButton;
namespace ps1 = rr::platform::ps1;

struct Checker {
    int checks = 0, failures = 0;
    void Expect(bool ok, const std::string& what) {
        ++checks;
        if (!ok) {
            ++failures;
            if (failures <= 40) std::printf("  FAIL %s\n", what.c_str());
        }
    }
};

// The default layout, stated independently of kActions: physical button -> the original's pad bits.
struct Expected {
    PadButton b;
    uint16_t word;
};
const Expected kExpected[] = {
    {PadButton::Cross, ps1::kCross},     {PadButton::R2, ps1::kCross},         {PadButton::Square, ps1::kSquare},
    {PadButton::L2, ps1::kSquare},       {PadButton::DLeft, ps1::kLeft},       {PadButton::LsLeft, ps1::kLeft},
    {PadButton::DRight, ps1::kRight},    {PadButton::LsRight, ps1::kRight},    {PadButton::R1, ps1::kR1},
    {PadButton::L1, ps1::kL1},           {PadButton::Triangle, ps1::kR2 | ps1::kTriangle}, {PadButton::DUp, ps1::kUp},
    {PadButton::LsUp, ps1::kUp},         {PadButton::DDown, ps1::kDown},       {PadButton::LsDown, ps1::kDown},
    {PadButton::Circle, ps1::kCircle},   {PadButton::Select, ps1::kSelect},    {PadButton::Touchpad, ps1::kSelect},
    {PadButton::L3, ps1::kL2},           {PadButton::Start, ps1::kStart},      {PadButton::R3, 0},
    {PadButton::Home, 0},
};

// ---- synthetic device states, from each device's own layout
bool XInputState(const std::vector<PadButton>& down, rr::platform::XInputRaw& s) {
    namespace xi = rr::platform::xinput;
    s = {};
    for (PadButton b : down) switch (b) {
        case PadButton::Cross: s.buttons |= xi::kA; break;
        case PadButton::Circle: s.buttons |= xi::kB; break;
        case PadButton::Square: s.buttons |= xi::kX; break;
        case PadButton::Triangle: s.buttons |= xi::kY; break;
        case PadButton::L1: s.buttons |= xi::kLShoulder; break;
        case PadButton::R1: s.buttons |= xi::kRShoulder; break;
        case PadButton::L2: s.leftTrigger = 255; break;
        case PadButton::R2: s.rightTrigger = 255; break;
        case PadButton::Select: s.buttons |= xi::kBack; break;
        case PadButton::Start: s.buttons |= xi::kStart; break;
        case PadButton::L3: s.buttons |= xi::kLThumb; break;
        case PadButton::R3: s.buttons |= xi::kRThumb; break;
        case PadButton::DUp: s.buttons |= xi::kUp; break;
        case PadButton::DDown: s.buttons |= xi::kDown; break;
        case PadButton::DLeft: s.buttons |= xi::kLeft; break;
        case PadButton::DRight: s.buttons |= xi::kRight; break;
        case PadButton::LsUp: s.ly = 32767; break;
        case PadButton::LsDown: s.ly = -32768; break;
        case PadButton::LsLeft: s.lx = -32768; break;
        case PadButton::LsRight: s.lx = 32767; break;
        default: return false; // no touchpad / guide through XInput
        }
    return true;
}
bool WinMMState(const std::vector<PadButton>& down, rr::platform::WinMMRaw& s) {
    s = {};
    for (PadButton b : down) switch (b) {
        case PadButton::Square: s.buttons |= 1u << 0; break;
        case PadButton::Cross: s.buttons |= 1u << 1; break;
        case PadButton::Circle: s.buttons |= 1u << 2; break;
        case PadButton::Triangle: s.buttons |= 1u << 3; break;
        case PadButton::L1: s.buttons |= 1u << 4; break;
        case PadButton::R1: s.buttons |= 1u << 5; break;
        case PadButton::L2: s.buttons |= 1u << 6; break;
        case PadButton::R2: s.buttons |= 1u << 7; break;
        case PadButton::Select: s.buttons |= 1u << 8; break;
        case PadButton::Start: s.buttons |= 1u << 9; break;
        case PadButton::L3: s.buttons |= 1u << 10; break;
        case PadButton::R3: s.buttons |= 1u << 11; break;
        case PadButton::Home: s.buttons |= 1u << 12; break;
        case PadButton::Touchpad: s.buttons |= 1u << 13; break;
        case PadButton::DUp: s.pov = 0; break;
        case PadButton::DRight: s.pov = 9000; break;
        case PadButton::DDown: s.pov = 18000; break;
        case PadButton::DLeft: s.pov = 27000; break;
        case PadButton::LsUp: s.y = 0; break;
        case PadButton::LsDown: s.y = 0xFFFF; break;
        case PadButton::LsLeft: s.x = 0; break;
        case PadButton::LsRight: s.x = 0xFFFF; break;
        default: return false;
        }
    return true;
}
// The DualSense's common input block: LX LY RX RY L2 R2 counter hat+faces shoulders misc (the USB order).
struct DsBlock {
    uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80, l2 = 0, r2 = 0, hat = 8, faces = 0, shoulders = 0, misc = 0;
};
bool DsState(const std::vector<PadButton>& down, DsBlock& d) {
    d = {};
    for (PadButton b : down) switch (b) {
        case PadButton::Square: d.faces |= 0x10; break;
        case PadButton::Cross: d.faces |= 0x20; break;
        case PadButton::Circle: d.faces |= 0x40; break;
        case PadButton::Triangle: d.faces |= 0x80; break;
        case PadButton::L1: d.shoulders |= 0x01; break;
        case PadButton::R1: d.shoulders |= 0x02; break;
        case PadButton::L2: d.shoulders |= 0x04; d.l2 = 255; break;
        case PadButton::R2: d.shoulders |= 0x08; d.r2 = 255; break;
        case PadButton::Select: d.shoulders |= 0x10; break;
        case PadButton::Start: d.shoulders |= 0x20; break;
        case PadButton::L3: d.shoulders |= 0x40; break;
        case PadButton::R3: d.shoulders |= 0x80; break;
        case PadButton::Home: d.misc |= 0x01; break;
        case PadButton::Touchpad: d.misc |= 0x02; break;
        case PadButton::DUp: d.hat = 0; break;
        case PadButton::DRight: d.hat = 2; break;
        case PadButton::DDown: d.hat = 4; break;
        case PadButton::DLeft: d.hat = 6; break;
        case PadButton::LsUp: d.ly = 0x00; break;
        case PadButton::LsDown: d.ly = 0xFF; break;
        case PadButton::LsLeft: d.lx = 0x00; break;
        case PadButton::LsRight: d.lx = 0xFF; break;
        default: return false;
        }
    // two held d-pad directions: the hat's diagonals (only Up+Down / Left+Right are impossible)
    return true;
}
// A standard reflected CRC-32 (poly 0xEDB88320), table-driven: independent of DualSenseCrc's bit loop.
uint32_t Crc32(const uint8_t* p, size_t n) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}
std::vector<uint8_t> DsUsb(const DsBlock& d) {
    std::vector<uint8_t> r(64, 0);
    r[0] = 0x01;
    const uint8_t block[10] = {d.lx, d.ly, d.rx, d.ry, d.l2, d.r2, 0x5A, uint8_t(d.hat | d.faces), d.shoulders, d.misc};
    std::memcpy(&r[1], block, 10);
    return r;
}
std::vector<uint8_t> DsBt31(const DsBlock& d) {
    std::vector<uint8_t> r(78, 0);
    r[0] = 0x31;
    r[1] = 0x10;
    const uint8_t block[10] = {d.lx, d.ly, d.rx, d.ry, d.l2, d.r2, 0x5A, uint8_t(d.hat | d.faces), d.shoulders, d.misc};
    std::memcpy(&r[2], block, 10);
    std::vector<uint8_t> seeded(75);
    seeded[0] = 0xA1;
    std::memcpy(&seeded[1], r.data(), 74);
    const uint32_t crc = Crc32(seeded.data(), seeded.size());
    for (int i = 0; i < 4; ++i) r[74 + i] = uint8_t(crc >> (8 * i));
    return r;
}
std::vector<uint8_t> DsBtSimple(const DsBlock& d) {
    std::vector<uint8_t> r(10, 0);
    r[0] = 0x01;
    const uint8_t block[9] = {d.lx, d.ly, d.rx, d.ry, uint8_t(d.hat | d.faces), d.shoulders, d.misc, d.l2, d.r2};
    std::memcpy(&r[1], block, 9);
    return r;
}

enum class Source { XInput, WinMM, DsUsb, DsBt31, DsBtSimple };
const char* SourceName(Source s) {
    switch (s) {
    case Source::XInput: return "XInput";
    case Source::WinMM: return "WinMM";
    case Source::DsUsb: return "DualSense USB 0x01";
    case Source::DsBt31: return "DualSense BT 0x31";
    default: return "DualSense BT 0x01";
    }
}
// Builds and decodes one state; false when the source has no such button.
bool Decode(Source src, const std::vector<PadButton>& down, rr::platform::PhysicalPad& p, bool& decoded) {
    decoded = false;
    if (src == Source::XInput) {
        rr::platform::XInputRaw s;
        if (!XInputState(down, s)) return false;
        p = rr::platform::DecodeXInput(s);
        decoded = true;
        return true;
    }
    if (src == Source::WinMM) {
        rr::platform::WinMMRaw s;
        if (!WinMMState(down, s)) return false;
        p = rr::platform::DecodeWinMM(s, true);
        decoded = true;
        return true;
    }
    DsBlock d;
    if (!DsState(down, d)) return false;
    const std::vector<uint8_t> r = src == Source::DsUsb ? DsUsb(d) : src == Source::DsBt31 ? DsBt31(d) : DsBtSimple(d);
    decoded = rr::platform::DecodeDualSenseInput(r.data(), r.size(), src != Source::DsUsb, p);
    return true;
}

std::string Hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%04X", v);
    return b;
}

} // namespace

int PadMapCheck(bool mutate) {
    using namespace rr::platform;
    Checker c;
    Bindings b = DefaultBindings();
    if (mutate) { // the negative control: R1 and L1 swapped
        std::swap(b.pad[int(GameAction::Attack1)][0], b.pad[int(GameAction::Attack2)][0]);
        std::printf("padmapcheck: MUTATED (the bindings' R1 / L1 swapped) - the verdict must be FAIL\n");
    }
    const Source sources[] = {Source::XInput, Source::WinMM, Source::DsUsb, Source::DsBt31, Source::DsBtSimple};

    // 1. every physical button alone, every source
    for (Source src : sources) {
        int ran = 0;
        for (const Expected& e : kExpected) {
            PhysicalPad p;
            bool decoded = false;
            if (!Decode(src, {e.b}, p, decoded)) continue;
            ++ran;
            c.Expect(decoded, std::string(SourceName(src)) + " " + PadButtonId(e.b) + ": the report decodes");
            const GamepadState s = StateFromPhysical(p, b);
            c.Expect(s.word == e.word, std::string(SourceName(src)) + " " + PadButtonId(e.b) + ": word " + Hex(s.word) +
                                           ", expected " + Hex(e.word));
            c.Expect(p.Down(e.b), std::string(SourceName(src)) + " " + PadButtonId(e.b) + ": the physical button is down");
            // the bound flags the race reads agree with the word
            const bool flagsAgree = s.throttle == ((e.word & ps1::kCross) != 0) && s.brake == ((e.word & ps1::kSquare) != 0) &&
                                    s.r1 == ((e.word & ps1::kR1) != 0) && s.l1 == ((e.word & ps1::kL1) != 0) &&
                                    s.r2 == ((e.word & ps1::kR2) != 0) && s.taunt == ((e.word & ps1::kL2) != 0) &&
                                    s.padUp == ((e.word & ps1::kUp) != 0) && s.padDown == ((e.word & ps1::kDown) != 0) &&
                                    s.left == ((e.word & ps1::kLeft) != 0) && s.right == ((e.word & ps1::kRight) != 0) &&
                                    s.lookBack == ((e.word & ps1::kCircle) != 0) &&
                                    s.camera == ((e.word & ps1::kSelect) != 0) && s.start == ((e.word & ps1::kStart) != 0) &&
                                    s.toBike == ((e.word & ps1::kTriangle) != 0);
            c.Expect(flagsAgree, std::string(SourceName(src)) + " " + PadButtonId(e.b) + ": GamepadState flags = word");
        }
        // nothing held: the word is 0, the sticks centred
        PhysicalPad idle;
        bool decoded = false;
        Decode(src, {}, idle, decoded);
        const GamepadState s = StateFromPhysical(idle, b);
        c.Expect(decoded && s.connected && s.word == 0, std::string(SourceName(src)) + ": idle word " + Hex(s.word));
        c.Expect(s.lx == 0x80 || s.lx == 0x7F, std::string(SourceName(src)) + ": idle stick centred");
        // the combat modifiers: R1 + Up = action 5, Triangle (R2) + Down = action 8; Triangle also
        // presses the original's Triangle (slot 6: back to the bike off it)
        PhysicalPad m;
        Decode(src, {PadButton::R1, PadButton::DUp}, m, decoded);
        c.Expect(StateFromPhysical(m, b).word == (ps1::kR1 | ps1::kUp), std::string(SourceName(src)) + ": R1 + Up");
        Decode(src, {PadButton::Triangle, PadButton::DDown}, m, decoded);
        c.Expect(StateFromPhysical(m, b).word == (ps1::kR2 | ps1::kTriangle | ps1::kDown),
                 std::string(SourceName(src)) + ": Triangle + Down = R2 + Down");
        std::printf("  %-20s %d buttons\n", SourceName(src), ran);
    }

    // 2. the analogue fold: the throttle trigger up the right stick, the brake trigger down
    for (Source src : {Source::XInput, Source::DsUsb, Source::DsBt31, Source::DsBtSimple}) {
        PhysicalPad p;
        bool decoded = false;
        Decode(src, {PadButton::R2}, p, decoded);
        const int ryUp = StateFromPhysical(p, b).ry;
        Decode(src, {PadButton::L2}, p, decoded);
        const int ryDown = StateFromPhysical(p, b).ry;
        c.Expect(ryUp <= 0x02 && ryDown >= 0xFD, std::string(SourceName(src)) + ": analogue fold ry " +
                                                     std::to_string(ryUp) + " / " + std::to_string(ryDown));
    }
    {   // a half-pressed DualSense trigger: under the digital threshold no throttle bit, past it the bit
        DsBlock d;
        d.r2 = kTriggerDown;
        PhysicalPad p;
        auto r = DsUsb(d);
        DecodeDualSenseInput(r.data(), r.size(), false, p);
        c.Expect(StateFromPhysical(p, b).word == 0, "DualSense: R2 at the threshold is not pressed");
        d.r2 = kTriggerDown + 1;
        r = DsUsb(d);
        DecodeDualSenseInput(r.data(), r.size(), false, p);
        c.Expect(StateFromPhysical(p, b).word == ps1::kCross, "DualSense: R2 past the threshold is the throttle");
    }

    // 3. the stick: a hard left with a little up is Left only; a diagonal is both
    {
        XInputRaw s;
        s.lx = -32768;
        s.ly = 12000;
        c.Expect(StateFromPhysical(DecodeXInput(s), b).word == ps1::kLeft, "stick: hard left + a little up = Left only");
        s.ly = 30000;
        c.Expect(StateFromPhysical(DecodeXInput(s), b).word == (ps1::kLeft | ps1::kUp), "stick: the diagonal = Left + Up");
    }

    // 4. the Bluetooth CRC: a corrupted 0x31 report is refused; the CRC routine matches the standard CRC-32
    {
        DsBlock d;
        auto r = DsBt31(d);
        PhysicalPad p;
        c.Expect(DecodeDualSenseInput(r.data(), r.size(), true, p), "BT 0x31: a good CRC decodes");
        r[10] ^= 0x01;
        c.Expect(!DecodeDualSenseInput(r.data(), r.size(), true, p), "BT 0x31: a corrupted report is refused");
        const char* nine = "123456789";
        c.Expect(DualSenseCrc('1', reinterpret_cast<const uint8_t*>(nine + 1), 8) == 0xCBF43926u,
                 "CRC-32 check value 0xCBF43926");
        auto usb = DsUsb(d);
        usb[0] = 0x05;
        c.Expect(!DecodeDualSenseInput(usb.data(), usb.size(), false, p), "USB: a foreign report id is refused");
    }

    // 5. the output report: motors, trigger effects, the Bluetooth CRC
    {
        DualSenseOutput o;
        o.small = 1;
        o.large = 200;
        o.rightForce = 2;
        o.leftForce = 4;
        const auto u = DualSenseOutputReport(o, false, 0);
        c.Expect(u[0] == 0x02 && u[1] == 0x0F && u[3] == 127 && u[4] == 100, "USB output: id, flags, motors");
        c.Expect(u[11] == 0x21 && u[22] == 0x21, "USB output: both trigger effects on");
        // right trigger: zones 2..9 at force 2 -> mask 0x03FC, three-bit forces (2-1) per zone
        uint32_t forces = 0;
        for (int z = 2; z < 10; ++z) forces |= 1u << (3 * z);
        c.Expect(u[12] == 0xFC && u[13] == 0x03 && u[14] == uint8_t(forces) && u[15] == uint8_t(forces >> 8),
                 "USB output: the right trigger's zones and forces");
        const auto off = DualSenseOutputReport(DualSenseOutput{}, false, 0);
        c.Expect(off[3] == 0 && off[4] == 0 && off[11] == 0x05 && off[22] == 0x05, "USB output: everything off");
        const auto bt = DualSenseOutputReport(o, true, 3);
        std::vector<uint8_t> seeded(75);
        seeded[0] = 0xA2;
        std::memcpy(&seeded[1], bt.data(), 74);
        const uint32_t crc = Crc32(seeded.data(), seeded.size());
        c.Expect(bt[0] == 0x31 && bt[1] == 0x30 && bt[2] == 0x10 && bt[3] == 0x0F && bt[5] == 127 && bt[6] == 100,
                 "BT output: id, sequence, tag, flags, motors");
        c.Expect(bt[74] == uint8_t(crc) && bt[75] == uint8_t(crc >> 8) && bt[76] == uint8_t(crc >> 16) &&
                     bt[77] == uint8_t(crc >> 24),
                 "BT output: CRC-32 seeded 0xA2");
    }

    // 6. adaptive triggers from the bindings
    {
        uint8_t r = 0, l = 0;
        TriggerResistance(b, r, l);
        c.Expect(r == 2 && l == 4, "adaptive triggers: R2 throttle light, L2 brake firm");
        Bindings off = b;
        off.adaptiveTriggers = false;
        TriggerResistance(off, r, l);
        c.Expect(r == 0 && l == 0, "adaptive triggers: off");
    }

    // 7. the keyboard
    {
        bool keys[256] = {};
        keys['Z'] = true;
        c.Expect(KeyHeld(b, GameAction::Attack1, keys), "keyboard: Z = R1");
        keys[VK_UP] = true;
        c.Expect(KeyHeld(b, GameAction::Throttle, keys) && !KeyHeld(b, GameAction::Throttle, keys, true),
                 "keyboard: Up = throttle, not in a two-player race");
        keys['W'] = true;
        c.Expect(KeyHeld(b, GameAction::Throttle, keys, true), "keyboard: W = throttle in a two-player race");
        keys[VK_SPACE] = true;
        c.Expect(KeyHeld(b, GameAction::ToBike, keys) && Action(GameAction::ToBike).ps1 == ps1::kTriangle,
                 "keyboard: Space = Triangle, back to the bike");
    }

    // 8. the ini: a changed binding survives a save and a load; another section of the file is kept
    {
        char tmp[MAX_PATH] = {}, file[MAX_PATH] = {};
        GetTempPathA(MAX_PATH, tmp);
        GetTempFileNameA(tmp, "rrc", 0, file);
        {
            std::ofstream o(file);
            o << "[video]\nwidescreen = 1\n\n[controls]\nthrottle.pad = square\n";
        }
        Bindings changed = DefaultBindings();
        changed.pad[int(GameAction::Attack3)][0] = PadButton::R3;
        changed.pad[int(GameAction::Attack3)][1] = PadButton::Triangle;
        changed.key[int(GameAction::Taunt)][0] = VK_OEM_COMMA;
        changed.adaptiveTriggers = false;
        c.Expect(SaveBindings(file, changed), "ini: saved");
        Bindings back;
        c.Expect(LoadBindings(file, back) && back == changed, "ini: loaded back equal");
        std::ifstream in(file);
        std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        c.Expect(all.find("[video]\nwidescreen = 1") != std::string::npos && all.find("throttle.pad = square") == std::string::npos,
                 "ini: the other section kept, the old [controls] replaced");
        DeleteFileA(file);
    }

    // 9. the rebinding capture: waits for a release, takes the next press, moves it off the other action
    {
        Bindings cb = DefaultBindings();
        BindingCapture cap;
        cap.Begin(GameAction::Attack1, true, 0);
        PhysicalPad p;
        p.connected = true;
        p.buttons = Bit(PadButton::Cross); // still held from the menu: not captured
        c.Expect(cap.Feed(cb, &p, nullptr) == BindingCapture::Result::Waiting, "capture: a held button waits");
        p.buttons = 0;
        cap.Feed(cb, &p, nullptr);
        p.buttons = Bit(PadButton::Square);
        c.Expect(cap.Feed(cb, &p, nullptr) == BindingCapture::Result::Captured &&
                     cb.pad[int(GameAction::Attack1)][0] == PadButton::Square &&
                     cb.pad[int(GameAction::Brake)][0] == PadButton::Count && cap.Displaced() == GameAction::Brake,
                 "capture: Square to R1, taken off the brake");
        p.buttons = Bit(PadButton::Square);
        c.Expect(StateFromPhysical(p, cb).word == ps1::kR1, "capture: Square now presses R1");
        bool keys[256] = {};
        cap.Begin(GameAction::Taunt, false, 1);
        cap.Feed(cb, nullptr, keys);
        keys['G'] = true;
        c.Expect(cap.Feed(cb, nullptr, keys) == BindingCapture::Result::Captured && cb.key[int(GameAction::Taunt)][1] == 'G',
                 "capture: key G as the taunt's second key");
        keys['G'] = false;
        cap.Begin(GameAction::Taunt, false, 0);
        cap.Feed(cb, nullptr, keys);
        keys[VK_ESCAPE] = true;
        c.Expect(cap.Feed(cb, nullptr, keys) == BindingCapture::Result::Cancelled && cb.key[int(GameAction::Taunt)][0] == 'T',
                 "capture: Esc cancels");
        ResetToDefaults(cb);
        c.Expect(cb == DefaultBindings(), "capture: reset to defaults");
    }

    std::printf("padmapcheck: %d checks, %d failures\n", c.checks, c.failures);
    std::printf("padmapcheck verdict %s\n", c.failures == 0 ? "PASS" : "FAIL");
    return c.failures == 0 ? 0 : 1;
}

int DualSenseProbe() {
    using namespace rr::platform;
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<std::wstring> paths = EnumerateDualSensePaths();
    const double enumMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("dualsenseprobe: %zu DualSense game-pad collection(s) (HID 054C:0CE6 / 0DF2), enumerated in %.1f ms\n",
                paths.size(), enumMs);
    for (const std::wstring& path : paths) {
        std::printf("  path %ls\n", path.c_str());
        DualSenseDevice d(path); // opened for input only: nothing is written unless Motors / Triggers are called
        if (!d.Ok()) {
            std::printf("  could not open it for reading\n");
            continue;
        }
        const auto start = std::chrono::steady_clock::now();
        while (d.ReportsRead() == 0 && std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1500))
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const uint32_t first = d.ReportsRead();
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        const uint32_t second = d.ReportsRead();
        std::printf("  transport %s, reports: %u in the first 1.5 s, %u in the next second; last report id 0x%02X\n",
                    d.Bluetooth() ? "Bluetooth" : "USB", first, second - first, d.LastReportId());
        PhysicalPad p;
        const int r = d.ReadPad(p);
        if (r <= 0) {
            std::printf("  no report decoded (%s)\n",
                        r < 0 ? "the read failed" : "a Bluetooth pad in its simple mode reports only on a change");
            continue;
        }
        std::string held;
        for (int i = 0; i < kPadButtons; ++i)
            if (p.buttons & (1u << i)) held += std::string(" ") + PadButtonId(static_cast<PadButton>(i));
        const GamepadState s = StateFromPhysical(p, DefaultBindings());
        std::printf("  decoded: %s, sticks L %02X %02X R %02X %02X, triggers L2 %u R2 %u, buttons:%s; pad word 0x%04X\n",
                    p.source, p.lx, p.ly, p.rx, p.ry, p.l2, p.r2, held.empty() ? " none" : held.c_str(), s.word);
    }
    // what the WinMM fallback sees (a DualSense also shows up here when nothing has it open natively)
    const auto w0 = std::chrono::steady_clock::now();
    int present = 0;
    const UINT ids = joyGetNumDevs();
    for (UINT id = 0; id < ids; ++id) {
        JOYINFOEX info{};
        info.dwSize = sizeof(info);
        info.dwFlags = JOY_RETURNBUTTONS;
        if (joyGetPosEx(id, &info) != JOYERR_NOERROR) continue;
        JOYCAPSA caps{};
        joyGetDevCapsA(id, &caps, sizeof(caps));
        std::printf("  WinMM joystick %u: %04X:%04X \"%s\"\n", id, caps.wMid, caps.wPid, caps.szPname);
        ++present;
    }
    const double winmmMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count();
    const auto w1 = std::chrono::steady_clock::now(); // again: the first scan pays the joystick driver's start-up
    for (UINT id = 0; id < ids; ++id) {
        JOYINFOEX info{};
        info.dwSize = sizeof(info);
        info.dwFlags = JOY_RETURNBUTTONS;
        joyGetPosEx(id, &info);
    }
    const double againMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w1).count();
    std::printf("dualsenseprobe: WinMM %d of %u ids present, scanned in %.1f ms (again: %.1f ms)\n", present, ids,
                winmmMs, againMs);
    return 0;
}
