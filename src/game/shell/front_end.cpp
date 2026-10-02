// The front end as the product runs it (front_end.h).
#include "game/shell/front_end.h"

#include "game/shell/shell_card.h"
#include "game/shell/shell_career.h"
#include "game/shell/shell_screens.h"
#include "game/shell/shell_sound.h"
#include "game/shell/shell_text.h"
#include "game/shell/shell_widgets.h"
#include "game/shell/shell_panel.h"
#include "game/sim/split_view.h" // the commit's ViewModeSet SLUS 0x8001B67C (two players)

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace rr::shell {

namespace {

// The widget handlers of the tables 0x8009CF28 / 0x8009CF78 (frontend.md 4.2) whose drawing is
// ShellView's own (the layout is ported, not the primitives). The text block 0x8006FAC8 is
// not among them: its handler is PORTED (shell_text.h) and runs.
bool ViewDrawnHandler(uint32_t a) {
    static const uint32_t kHandlers[] = {0x8006E400u, 0x8006E3BCu, 0x8006E4D8u, 0x8006E894u, 0x8006E8FCu,
                                         0x8006E7A4u, 0x8006EDE4u, 0x8006ECA0u, 0x8006F764u, 0x8006EF30u,
                                         0x8006F9A4u, 0x8006FA38u, 0x8006F4A0u, 0x8006F0B8u, 0x80070580u};
    return std::find(std::begin(kHandlers), std::end(kHandlers), a) != std::end(kHandlers);
}

// The widget draw handlers PORTED in shell_widgets.h (types 7, 9, 12, 13, 17, 18) run and their packets
// reach the view; RRJB_MENU_WIDGETS=off (the negative control) leaves them to the view's own layout.
bool PortedWidgets() {
    static const bool on = [] {
        const char* e = std::getenv("RRJB_MENU_WIDGETS");
        return !(e != nullptr && std::strcmp(e, "off") == 0);
    }();
    return on;
}

// The panel films and logos (widget types 2..6 and 8) PORTED in shell_panel.h; RRJB_MENU_FILMS=off (their
// negative control) leaves them to the view's own layout (a film always looping, no idle gate).
bool PortedPanels() {
    static const bool on = [] {
        const char* e = std::getenv("RRJB_MENU_FILMS");
        return PortedWidgets() && !(e != nullptr && std::strcmp(e, "off") == 0);
    }();
    return on;
}

// The sprite emitter 0x800700F0 (OURS: the view draws its picture) links the sprite where the original's
// does by the record's flags (+0): a shared-slot picture (0x100000) into the caller's entry; a backdrop
// (0x200000) into the backdrop table 0x8009CCE4 + screen+0x0C * 4 + 0x1C, a title (0x800000) at + 0x18, a
// logo or bike picture (0x400000) at + 0x14 while a film plays (fe+0x0A bit 0), else the caller's.
uint32_t EmitterOt(GuestRam& g, uint32_t screen, uint32_t id, uint32_t ot) {
    for (uint32_t i = 0; i < 300u; ++i) {
        if (g.U32(kSpriteIds + 4u * i) != id) continue;
        const uint32_t f = g.U32(kSpriteRecs + 0x24u * i);
        const uint32_t back = g.U32(0x8009CCE4u) + static_cast<uint32_t>(g.S8(screen + 0x0Cu) * 4);
        if (f & 0x100000u) return ot;
        if (f & 0x200000u) return back + 0x1Cu;
        if (f & 0x400000u) return (g.U16(kFeMedia) & 1u) ? back + 0x14u : ot;
        if (f & 0x800000u) return back + 0x18u;
        return ot;
    }
    return ot;
}

std::string CString(GuestRam& g, uint32_t p, size_t limit = 512) {
    std::string out;
    for (size_t i = 0; i < limit; ++i) {
        const char c = static_cast<char>(g.U8(p + static_cast<uint32_t>(i)));
        if (c == 0 || g.Faulted()) break;
        out.push_back(c);
    }
    return out;
}

// SLUS 0x80043FD4 (sprintf) for the formats the shell passes: %d %i %u %x %X %c %s %%, with the '-'
// and '0' flags, a width and a precision. OURS: the C library's formatter on the guest's own format
// string and arguments; %s arguments are guest pointers.
std::string GuestSprintf(GuestRam& g, const std::string& fmt, const uint32_t* args, int count) {
    std::string out;
    int next = 0;
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%') {
            out.push_back(fmt[i]);
            continue;
        }
        size_t j = i + 1;
        std::string spec = "%";
        while (j < fmt.size() && std::strchr("-0+ #", fmt[j]) != nullptr && fmt[j] != 0) spec.push_back(fmt[j++]);
        while (j < fmt.size() && (std::isdigit(static_cast<unsigned char>(fmt[j])) || fmt[j] == '.')) spec.push_back(fmt[j++]);
        while (j < fmt.size() && (fmt[j] == 'l' || fmt[j] == 'h')) ++j;
        if (j >= fmt.size()) break;
        const char conv = fmt[j];
        const uint32_t a = next < count ? args[next] : 0u;
        char buf[256] = {};
        switch (conv) {
        case '%': out.push_back('%'); i = j; continue;
        case 'd': case 'i': ++next; spec += 'd'; std::snprintf(buf, sizeof(buf), spec.c_str(), static_cast<int32_t>(a)); break;
        case 'u': ++next; spec += 'u'; std::snprintf(buf, sizeof(buf), spec.c_str(), a); break;
        case 'x': case 'X': ++next; spec += conv; std::snprintf(buf, sizeof(buf), spec.c_str(), a); break;
        case 'c': ++next; spec += 'c'; std::snprintf(buf, sizeof(buf), spec.c_str(), static_cast<int>(a & 0xFFu)); break;
        case 's': {
            ++next;
            spec += 's';
            const std::string str = CString(g, a);
            std::snprintf(buf, sizeof(buf), spec.c_str(), str.c_str());
            break;
        }
        default: out.push_back(conv); i = j; continue;
        }
        out += buf;
        i = j;
    }
    return out;
}

} // namespace

constexpr uint32_t kPacketBase = 0x000F0000; // OURS: the packet cursor's start (front_end.cpp constructor)

bool ProductCallees::Call(uint32_t address, const uint32_t* args, int count, uint32_t* v0) {
    if (v0 != nullptr) *v0 = 0;
    const uint32_t a0 = (count > 0 && args != nullptr) ? args[0] : 0u;
    if (g != nullptr) { // the card layer (shell_memcard.h CardDevice)
        bool handled = false;
        const bool ok = card.Call(*g, address, args, count, v0, &handled);
        if (handled) return ok;
    }
    switch (address) {
    case 0x8007F20C: // RASHCDF VolumeSet (channel, value): PORTED (shell_arena.h) - the sliders the sound reads
        if (g != nullptr && count >= 2) VolumeSet(*g, static_cast<int32_t>(a0), static_cast<int32_t>(args[1]));
        return true;
    case kJukeboxSet: // SLUS 0x8002490C, PORTED (shell_sound.h)
        if (g != nullptr && count >= 2) JukeboxSet(*g, a0, static_cast<int32_t>(args[1]));
        return true;
    case kSoundPreview: // RASHCDF 0x8007EB1C / 0x8007ECFC / 0x8007ED34: queued for ShellSound, PORTED there,
                        // in order with the clicks (shell_sound.h Frame)
        uiSounds.push_back(address);
        uiSounds.push_back(a0);
        uiSounds.push_back(count > 1 ? args[1] : 0u);
        return true;
    case kSoundPreviewStop:
        uiSounds.push_back(address);
        return true;
    case kSoundModeUi:
        uiSounds.push_back(address);
        uiSounds.push_back(a0);
        return true;
    case kSoundMode: // SLUS 0x8001EFDC, PORTED (shell_sound.h)
        if (g != nullptr) SoundModeSet(*g, a0);
        return true;
    case 0x8001B67C: // SLUS ViewModeSet(session+0x17), the commit's split layout: PORTED (sim\split_view.h;
                     // not a pad install as shell_logic.h's kPadInstall names it)
        if (g != nullptr) rr::sim::ViewModeSet(*g, a0);
        return true;
    case 0x8001C304: { // SLUS: a draw-area primitive (x, y, w, h, ...) - the credits' clip (text arm 0x2F)
        if (g == nullptr || count < 4) return true;
        const uint32_t env = g->U32(0x8005B470u);
        const uint32_t b = env + 0x70u * g->U8(env + 6u);
        TextCall t;
        t.widget = widget;
        t.clip = true;
        t.x = static_cast<int32_t>(args[0]) - g->S16(b + 24u);
        t.y = static_cast<int32_t>(args[1]) - g->S16(b + 26u);
        t.w = static_cast<int32_t>(args[2]);
        t.h = static_cast<int32_t>(args[3]);
        texts.push_back(std::move(t));
        return true;
    }
    case 0x8004CDE4: // SLUS SetSemiTrans(prim, on)
        if (g != nullptr && count >= 2)
            g->W8(a0 + 7u, static_cast<uint8_t>(args[1] ? (g->U8(a0 + 7u) | 2u) : (g->U8(a0 + 7u) & 0xFDu)));
        return true;
    case 0x8004CDA4: { // SLUS AddPrim(ot, prim): linked as the original links it, and recorded for the view
        if (g == nullptr || count < 2) return true;
        const uint32_t p = args[1];
        g->W32(p, (g->U32(p) & 0xFF000000u) | (g->U32(a0) & 0xFFFFFFu));
        g->W32(a0, (g->U32(a0) & 0xFF000000u) | (p & 0xFFFFFFu));
        TextCall t;
        t.widget = widget;
        t.quad = true;
        t.ot = a0; // the entry it is linked into (the view draws a text block's calls in table order)
        t.semi = (g->U8(p + 7u) & 2u) != 0;
        t.rgb = g->U8(p + 4u) | (g->U8(p + 5u) << 8) | (g->U8(p + 6u) << 16);
        for (uint32_t i = 0; i < 4u; ++i) {
            t.qx[i] = g->S16(p + 8u + 4u * i);
            t.qy[i] = g->S16(p + 10u + 4u * i);
        }
        texts.push_back(std::move(t));
        return true;
    }
    case 0x80021C98: // SLUS: the packet buffer's wrap - OURS: the product's buffer never fills
        if (v0 != nullptr) *v0 = a0;
        return true;
    case kUiSound: uiSounds.push_back(a0); return true;
    case kMusicPlay: musicRequest = static_cast<int>(a0); return true;
    case kMusicStop: musicStop = true; return true;
    case 0x80043F00: // GetRCnt SLUS: the root counter the dispatchers seed the LCG from - OURS, a count
        if (v0 != nullptr) *v0 = (rootCounter += 0x3A5u) & 0xFFFFu;
        return true;
    case 0x8006E894: // the widget draw handlers PORTED in shell_widgets.h, run with the widget named
    case 0x8006E7A4:
    case 0x8006F764:
    case 0x8006EF30:
    case 0x8006F4A0:
    case 0x8006F0B8:
    case 0x8006ECA0:
    case 0x8006F9A4:
    case 0x8006FA38: {
        if (!PortedWidgets()) break;
        if (g == nullptr || count < 2) return true;
        const uint32_t prev = widget;
        widget = args[1];
        uint32_t r = 1;
        bool ok = true;
        switch (address) {
        case 0x8006E894: ok = SpriteWidget(*g, *this, args[0], args[1], &r); break;
        case 0x8006E7A4: ok = PadHint(*g, *this, args[0], args[1], &r); break;
        case 0x8006F764: ok = ButtonDraw(*g, *this, args[0], args[1], &r); break;
        case 0x8006EF30: ok = ChooserDraw(*g, *this, args[0], args[1], &r); break;
        case 0x8006F4A0: ok = SliderDraw(*g, *this, args[0], args[1], &r); break;
        case 0x8006ECA0: ok = PanelWidget(*g, *this, args[0], args[1], &r); break;
        case 0x8006F9A4: ok = HeaderText(*g, *this, args[0], args[1], &r); break;
        case 0x8006FA38: ok = TextLine(*g, *this, args[0], args[1], &r); break;
        default: ok = BikeStatsDraw(*g, *this, args[0], args[1], &r); break;
        }
        widget = prev;
        if (v0 != nullptr) *v0 = r;
        return ok;
    }
    case kSpriteUpload: // RASHCDF LoadImage of a sprite record: OURS, the view draws the picture from the disc
        return true;
    case 0x8006E4D8: // widget types 2..6 and 8: PORTED (shell_panel.h), run at the product's stack
    case 0x8006E8FC: {
        if (!PortedPanels()) break;
        if (g == nullptr || count < 2) return true;
        const uint32_t prev = widget;
        widget = args[1];
        uint32_t r = 1;
        bool ok;
        if (address == 0x8006E4D8u) {
            ++filmGates;
            ok = FilmWidget(*g, *this, args[0], args[1], kProductTextSp, &r);
        } else {
            ++logoCalls;
            ok = LogoWidget(*g, *this, args[0], args[1], kProductTextSp, &r);
        }
        widget = prev;
        if (v0 != nullptr) *v0 = r;
        return ok;
    }
    case kSkipMovie: { // RASHCDF 0x8006FED4 = FilmClose(0): PORTED (shell_panel.h)
        if (g == nullptr) return true;
        uint32_t r = 1;
        const bool ok = FilmClose(*g, *this, &r);
        if (v0 != nullptr) *v0 = r;
        return ok;
    }
    case kSplashFade: // SLUS 0x80022A78(n): *(0x80053464), the CD layer's busy word - OURS: the shell's CD is idle
        if (v0 != nullptr) *v0 = 0;
        return true;
    case kFileOpen: // SLUS 0x8001458C / 0x8001460C and the film library: the product's (panel_films.h, OURS)
        if (v0 != nullptr) *v0 = films != nullptr && g != nullptr ? static_cast<uint32_t>(films->OpenFile(CString(*g, a0, 64)))
                                                                  : 0xFFFFFFFFu;
        return true;
    case kFileClose:
        if (films != nullptr) films->CloseFile(static_cast<int32_t>(a0));
        return true;
    case kFilmStart:
        if (v0 != nullptr)
            *v0 = films != nullptr && count >= 5 ? films->Start(static_cast<int32_t>(a0), args[1], static_cast<int32_t>(args[2]),
                                                                static_cast<int32_t>(args[3]), static_cast<int32_t>(args[4]))
                                                 : 0u;
        return true;
    case kFilmPicture:
        if (v0 != nullptr)
            *v0 = films != nullptr && count >= 5 ? films->Picture(static_cast<int32_t>(a0), static_cast<int32_t>(args[1]),
                                                                  static_cast<int32_t>(args[2]), args[3], args[4])
                                                 : 0u;
        return true;
    case kFilmStop:
        if (films != nullptr) films->Stop(static_cast<int32_t>(a0));
        return true;
    case 0x8006FAC8: { // the text-block handler: PORTED (shell_text.h), run at the product's stack
        if (g == nullptr || count < 2) return true;
        const uint32_t prev = widget;
        widget = args[1];
        const bool ok = TextBlock(*g, *this, args[0], args[1], kProductTextSp);
        widget = prev;
        if (v0 != nullptr) *v0 = 1;
        return ok;
    }
    case kTextId:
    case kTextStr:
    case kTextAt: { // the SLUS text calls: recorded for the view (shell_view.h TextCall)
        if (g == nullptr || count < 6) return true;
        TextCall t;
        t.widget = widget;
        t.font = static_cast<int32_t>(args[0]);
        if (address == kTextStr) {
            t.text = CString(*g, args[1]);
        } else {
            const uint32_t table = g->U32(kStringTable);
            const int32_t id = static_cast<int32_t>(args[1]);
            if (table != 0 && id >= 0 && id < 1945) t.text = CString(*g, g->U32(table + 4u * static_cast<uint32_t>(id)));
        }
        t.ot = args[address == kTextAt ? 4 : 3];
        if (address == kTextAt) {
            t.x = static_cast<int16_t>(args[2]);
            t.y = static_cast<int16_t>(args[3]);
            t.rgb = args[5];
            t.just = -1;
        } else {
            t.x = g->S16(args[2]);
            t.y = g->S16(args[2] + 2u);
            t.w = g->S16(args[2] + 4u);
            t.h = g->S16(args[2] + 6u);
            t.rgb = args[4];
            t.just = static_cast<int32_t>(args[5]);
            // Mode 3 (the wrapped box) only while RASHCDF is resident (0x8002CB08 tests bit 0x10 of the
            // overlay mask); 0x8002CC74 has no mode 3.
            if (t.just == 3 && (address == kTextStr || !(g->U32(kOverlayMask) & 0x10u))) return true;
            if (t.just < 0 || t.just > 3) return true;
        }
        texts.push_back(std::move(t));
        return true;
    }
    case kSpriteEmit:
    case kPanelEmit: { // the sprite / panel emitters (screen, record, ot): recorded for the view
        if (g == nullptr || count < 3) return true;
        if (address == kPanelEmit && PortedWidgets()) { // PORTED (shell_widgets.h): its quads reach the view
            uint32_t r = 1;
            const bool ok = PanelEmit(*g, *this, args[0], args[1], args[2], &r);
            if (v0 != nullptr) *v0 = r;
            return ok;
        }
        TextCall t;
        t.widget = widget;
        const uint32_t r = args[1];
        t.ot = (address == kSpriteEmit && PortedPanels()) ? EmitterOt(*g, args[0], g->U32(r), args[2]) : args[2];
        t.rgb = g->U32(r + 4u) & 0xFFFFFFu;
        t.x = g->S16(r + 8u);
        t.y = g->S16(r + 10u);
        if (address == kSpriteEmit) {
            t.sprite = g->U32(r);
            // 0x800700F0 answers 1 when it emitted the sprite (the choosers' arrows follow only then);
            // OURS: the view draws every picture from the disc, so the emitter always has.
            if (v0 != nullptr && t.sprite != 0) *v0 = 1;
        } else {
            t.panel = true;
            t.w = g->S16(r + 12u);
            t.h = g->S16(r + 14u);
            t.ix0 = g->S16(r + 24u);
            t.iy0 = g->S16(r + 26u);
            t.ix1 = g->S16(r + 28u);
            t.iy1 = g->S16(r + 30u);
        }
        texts.push_back(std::move(t));
        return true;
    }
    case kSprintf: { // SLUS sprintf, formatted natively into the arena
        if (g == nullptr || count < 2) return true;
        const std::string text = GuestSprintf(*g, CString(*g, args[1]), args + 2, count - 2);
        g->WriteBlock(args[0], reinterpret_cast<const uint8_t*>(text.c_str()), static_cast<uint32_t>(text.size() + 1));
        if (v0 != nullptr) *v0 = static_cast<uint32_t>(text.size());
        return true;
    }
    default:
        break;
    }
    if (ViewDrawnHandler(address)) return true;
    // A text arm not ported yet (shell_text.cpp TextBlock names them): nothing drawn, noted once.
    static const uint32_t kTextArms[] = {0u};
    if (std::find(std::begin(kTextArms), std::end(kTextArms), address) != std::end(kTextArms)) {
        if (std::find(textMissing.begin(), textMissing.end(), address) == textMissing.end()) textMissing.push_back(address);
        return true;
    }
    unported.push_back({address, a0});
    ++totalUnported;
    // A per-screen input handler the port does not contain yet (it is in the table 0x8009C8C0): OURS,
    // the generic menu handler MenuInput runs in its place, so the screen still answers the cursor and
    // Back instead of trapping the player. Logged as a stand-in, never counted as the original.
    if (g != nullptr && count == 1) {
        bool screenHandler = false;
        for (uint32_t i = 0; i < static_cast<uint32_t>(kScreenCount); ++i)
            screenHandler = screenHandler || g->U32(kScreenInput + 4u * i) == address;
        if (screenHandler) {
            int32_t r = 0;
            const int32_t first = g->S8(a0 + 14u);
            MenuInput(*g, *this, a0, first, first + g->S8(a0 + 15u), &r);
            if (v0 != nullptr) *v0 = static_cast<uint32_t>(r);
            ++standIns;
        }
    }
    return true;
}

FrontEnd::FrontEnd(const DiscImage& disc, uint32_t seed) : disc_(disc) {
    BuildShellArena(disc, arena_, callees_, seed, &arenaReport_);
    g_ = std::make_unique<GuestRam>(arena_.ram.data(), kArenaGp);
    g_->SetScratchpad(arena_.scratchpad.data());
    callees_.g = g_.get();
    view_ = std::make_unique<ShellView>(disc);
    // PollPads' snapshot has four records; the product has one pad: port 0 is present (+0x10, the
    // "pad connected" word the option gates 0x40/0x20 read), the others are not.
    g_->W32(kPads + 0x10u, 1);
    // OURS: the display environment SLUS's display init leaves at *(0x8005B470) (not ported): the record
    // at the capture's address 0x800D6CB8 with draw buffer 0 at offset (0, 0), and the primitive packet
    // buffer whose cursor +0x10C the widget handlers allocate from (the keyboard's highlights, text arm
    // 0x37) at the capture's region below its limit *(0x8005B4D0) = 0x000F88A0. The console's frame swap
    // puts the cursor back every frame; Frame does (kPacketBase).
    if (g_->U32(0x8005B470u) == 0) {
        g_->W32(0x8005B470u, 0x800D6CB8u);
        g_->W32(0x8005B4D0u, 0x000F88A0u);
    }
    g_->W32(g_->U32(0x8005B470u) + 268u, kPacketBase);
    // OURS: the two ordering tables 0x80080400 points the frame at (the capture's first buffer), and the
    // sprite records of FEMISC.PSH's shapes (the arrows' sheets among them): the product's view draws them
    // from the disc, so each reads "loaded and in VRAM" (0x30000000) with the shape's size at +0x14 / +0x16
    // - the width is what the ported AnimSprite 0x800705DC wraps a sheet's frames at.
    g_->W32(kOtTable, 0x8009CCE8u);
    g_->W32(0x8009CCE4u, 0x8009D1B0u);
    for (uint32_t i = 0; i < 300u; ++i) {
        const uint32_t rec = kSpriteRecs + 0x24u * i;
        int sw = 0, sh = 0;
        if (g_->U32(rec) != 0 || !view_->ShapeSize(g_->U32(kSpriteIds + 4u * i), &sw, &sh)) continue;
        g_->W32(rec, 0x30000000u);
        g_->W16(rec + 0x14u, static_cast<uint16_t>(sw));
        g_->W16(rec + 0x16u, static_cast<uint16_t>(sh));
    }
    view_->SetPortedWidgets(PortedWidgets());
    view_->SetPortedPanels(PortedPanels());
    films_ = std::make_unique<PanelFilms>(disc_);
    callees_.films = films_.get();
    view_->SetFilmShown(&films_->Shown());
    // ... and port 1: the product's second player - the keyboard's right-hand layout and the second
    // controller (game\mp_input.h) - is always there, so the two-player modes' gates see a second pad.
    g_->W32(kPads + 192u + 0x10u, 1);
}

// The pad snapshot 0x800D7128 PollPads SLUS 0x8001CB3C leaves for the shell: slot press codes at
// record + 0x1A + 8*slot. OURS: the value and the auto-repeat cadence - the
// fold SLUS 0x8001C8E0 writes 2 on a fresh press; its repeat cadence is unknown, so a held key
// repeats after 20 frames every 6 frames here.
void FrontEnd::WritePads(const ShellKeys& k, uint32_t pad) {
    const bool down[8] = {k.left, k.right, k.up, k.down, k.square, k.circle, k.triangle, k.cross};
    int* held = pad == 0 ? held_ : held2_; // pad 1: player 2's keys (the two-player screens' pad windows)
    bool any = false;
    for (int s = 0; s < 19; ++s) g_->W8(PadSlot(pad, s), 0);
    for (int s = 0; s < 8; ++s) {
        if (!down[s]) {
            held[s] = 0;
            continue;
        }
        any = true;
        ++held[s];
        const bool edge = held[s] == 1;
        const bool repeat = s <= kDown && held[s] > 20 && (held[s] - 20) % 6 == 0;
        if (edge || repeat) g_->W8(PadSlot(pad, s), 2);
    }
    // record +0x04: the activity word InputPass tests for the attract idle reset (0x80066808).
    g_->W32(kPads + kPadStride * pad + 4u, any ? 1u : 0u);
}

void ProductCallees::Drew(const DrawNote& n) {
    TextCall t;
    t.widget = widget;
    t.ot = n.ot;
    t.rgb = n.rgb;
    t.x = n.x, t.y = n.y, t.w = n.w, t.h = n.h;
    t.semi = n.semi;
    if (n.kind == DrawNote::Sprite) {
        t.sprite = n.fourcc;
        t.sx = n.srcX, t.sy = n.srcY, t.sw = n.w, t.sh = n.h;
    } else if (n.kind == DrawNote::Tile) {
        t.tile = true;
    } else {
        t.box = true;
    }
    texts.push_back(std::move(t));
}

// ScreenTick RASHCDF 0x80066C34: every screen but the current one, then the current one, each through
// its transition handler while screen+0x02 != 0 and its tick handler while it is live (flags & 2), with
// fe+0x08 naming the screen being handled. The default handlers are PORTED for their state and their
// widget pass: the tick 0x8006D630 (fe+0x11 = screen+0x0C, then WidgetPass 0x8006D3E0) and the
// transition 0x8006D5B0 (a live screen: fe+0x11 = +0x0C, the pass, f02 = 0; a leave: 32766 -> 1 with
// fe+0x11 = +0x0D and the pass; else f02 = 0). The overlays' 0x8006D8B4 / 0x8006D658 are given the
// same effect (OURS: their slide is presentation); the movies' 0x8006DB5C / 0x8006DE5C and screen 57's
// 0x8006E008 run no pass. The pass reaches the widget handlers through the seam: the text block's is
// PORTED and runs (ProductCallees), the others' drawing is ShellView's.
void FrontEnd::ScreenTick() {
    GuestRam& g = *g_;
    const int16_t cur = g.S16(kFeCur);
    auto tick = [&](int32_t i) {
        g.W16(kFeTicked, static_cast<uint16_t>(i));
        const uint32_t s = g.U32(g.U32(kScreenTablePtr) + 4u * static_cast<uint32_t>(i));
        if (s == 0) return;
        const int16_t t = g.S16(s + 2u);
        const bool live = (g.U16(s) & 2u) != 0;
        const uint32_t tickHandler = g.U32(0x8009CFD0u + 4u * static_cast<uint32_t>(i));
        const uint32_t transHandler = g.U32(0x8009D0C0u + 4u * static_cast<uint32_t>(i));
        const bool movie = transHandler == 0x8006DB5Cu || tickHandler == 0x8006DE5Cu || tickHandler == 0x8006E008u;
        if (t == 0) {
            if (!live || tickHandler != 0x8006D630u) return;
            g.W8(kFeLayer, g.U8(s + 0x0Cu));
            WidgetPass(g, callees_, s);
            return;
        }
        if (transHandler == 0) {
            g.W16(s + 2u, 0);
            return;
        }
        if (live) {
            if (!movie) {
                g.W8(kFeLayer, g.U8(s + 0x0Cu));
                WidgetPass(g, callees_, s);
            }
            g.W16(s + 2u, 0);
        } else if (t == 32766) {
            g.W16(s + 2u, 1);
            if (!movie) {
                g.W8(kFeLayer, g.U8(s + 0x0Du));
                WidgetPass(g, callees_, s);
            }
        } else {
            g.W16(s + 2u, 0);
        }
    };
    for (int32_t i = 0; i < kScreenCount; ++i)
        if (i != cur) tick(i);
    // 0x80066D34..: a current screen flagged 0x1000 loads the shell's shared resource list 0x80089C84
    // once (0x8008972C). Its first entry is record 17, class 5 - res_show 0x800782B0 runs the sound
    // system's init 0x8007E824 for it and sets fe+0x0F, "the music may play", which the music driver of
    // the input pass waits for. PORTED for that record; the list's pictures,
    // fonts and strings are the view's own (shell_view.h) and the sound init is ShellSound's.
    if (cur >= 0 && cur < kScreenCount && g.U32(0x8008972Cu) == 0) {
        const uint32_t s = g.U32(g.U32(kScreenTablePtr) + 4u * static_cast<uint32_t>(cur));
        if (s != 0 && (g.U16(s) & 0x1000u)) {
            const uint32_t rec = 0x800897E4u + 32u * 17u;
            if ((g.U16(rec) & 0xEu) == 0 && g.U8(rec + 2u) == 5) {
                g.W8(kFeMusicOk, 1);
                g.W16(rec, static_cast<uint16_t>(g.U16(rec) | 4u));
            }
            g.W32(0x8008972Cu, 1);
        }
    }
    // The movie screens' transition 0x8006DB5C (entry arm, MovieEntry below) runs where ScreenTick runs the
    // current screen's handler: after every other screen's pass - a leaving screen's panel film gate closes
    // what plays before the new screen's film is opened (0x8006E6F4 / 0x8006FE6C share fe+0x0A with it).
    MovieEntry();
    if (cur >= 0 && cur < kScreenCount) tick(cur);
    g.W16(kFeTicked, static_cast<uint16_t>(cur));
}

void FrontEnd::Frame(const ShellKeys& keys, const ShellKeys* keys2) {
    GuestRam& g = *g_;
    if (results_.Active()) { // the race's results scene: one frame of it, until Cross / Start
        ++frames_;
        resultsTexts_.clear();
        results_.Frame(resultsTexts_);
        const bool edge = keys.cross && !prevCross_;
        prevCross_ = keys.cross;
        if (edge) FinishRace();
        return;
    }
    callees_.uiSounds.clear();
    callees_.musicRequest = -1;
    callees_.musicStop = false;
    callees_.unported.clear();
    callees_.texts.clear();
    g.W32(g.U32(0x8005B470u) + 268u, kPacketBase); // the frame swap's packet cursor (constructor, OURS)
    films_->Frame(); // the panel films: the drive's bytes, last frame's pictures gone (panel_films.h)
    ++frames_;
    // The vblank counter the splash times against (60 Hz; the shell's frame is one vblank here).
    g.W32(kVblankCount, g.U32(kVblankCount) + 1u);
    WritePads(keys);
    if (keys2 != nullptr) WritePads(*keys2, 1); // player 2's pad (game\mp_input.h's player)
    if (vib_[0] || vib_[1]) { // the pad rumble: the driver's actuator service per pad (SLUS 0x8001CA04's call)
        if (!padsBooted_) {
            rr::game::PadPortsBoot(g); // SLUS 0x8001C590's table (pad_device.h)
            padsBooted_ = true;
        }
        for (uint32_t p = 0; p < 2; ++p) {
            padModel_.kind[p] = vib_[p] ? rr::game::PadKind::DualShock : rr::game::PadKind::FirstType;
            rr::sim::PadActuatorService(g, p, 0, padModel_); // PORTED (sim\pad_motor.h)
        }
    }
    // The idle counter the attract timeout reads (InputPass 0x80066BF8) advances with the frame, and so
    // do the other counters of the shell's vsync callback RASHCDF 0x80064C30: 0x80088C44, 0x80088C50, the
    // card screens' clock 0x80088C4C, and their entry hold 0x80088C48 counts down to 0.
    g.W32(kIdle, g.U32(kIdle) + 1u);
    g.W32(0x80088C44u, g.U32(0x80088C44u) + 1u);
    g.W32(0x80088C50u, g.U32(0x80088C50u) + 1u);
    g.W32(kCardClock, g.U32(kCardClock) + 1u);
    if (g.U32(kCardHold) != 0) g.W32(kCardHold, g.U32(kCardHold) - 1u);
    InputPass(g, callees_);       // RASHCDF 0x800667E4
    // (the movie screens' transition 0x8006DB5C, entry arm: inside ScreenTick, before the current screen)
    ScreenTick();                 // RASHCDF 0x80066C34 (logic effect, above)
    MovieTick();                  // ... their tick 0x8006DE5C and the transition's leave arm
    if (g.S8(kFeHold) > 0) g.W8(kFeHold, static_cast<uint8_t>(g.S8(kFeHold) - 1)); // 0x80080324
    CommitScreen(g, callees_);    // RASHCDF 0x8006711C
}

bool FrontEnd::RaceRequested() { return !results_.Active() && g_->U8(g_->U32(kGameStatePtr)) == 3; }

void FrontEnd::BeginRace(Handover& h) {
    GuestRam& g = *g_;
    CommitSelection(g, callees_); // RASHCDF 0x8007F37C
    h = Handover{};
    h.active = true;
    g.ReadBlock(kGameState, h.gameState, sizeof(h.gameState));
    g.ReadBlock(kSession, h.sessionAndPlayers, sizeof(h.sessionAndPlayers));
    g.ReadBlock(kRiders, h.riders, sizeof(h.riders)); // the commit's career identities (+0x2C..+0x33)
    // The resident SLUS data (handover.h): the jukebox's flags and the Time Trial records, to the race.
    for (uint32_t t = 0; t < kResidentAlbumTracks; ++t) h.albumFlags[t] = g.U32(kResidentAlbumFlags + kResidentAlbumStride * t);
    g.ReadBlock(kResidentRecords, h.records, kResidentRecordBytes);
    h.resident = true;
    for (uint32_t k = 0; k < kResidentSliderCount; ++k) { // the options' volume sliders
        h.volume[k] = g.U32(kResidentSliders + 4u * k);
        h.volumeSaved[k] = g.U32(kResidentSlidersSaved + 4u * k);
    }
    h.sliders = true;
    const int32_t players = g.S32(kGameState + 0x30u);
    h.set = players >= 2 ? 2 : 1;
    h.raceId = g.S32(kGameState + 0x40u);
    // The bike's .PH: the resource-name array 0x8008973C entry 21 + bike index (rules.md 1.3); player 2's
    // bike is game_state+0x4C (the commit's player[1]+0x03), read the same way.
    auto bikePh = [&g](int32_t bike) -> std::string {
        if (bike < 0 || bike >= 21) return {};
        std::string name;
        const uint32_t p = g.U32(0x8008973Cu + 4u * static_cast<uint32_t>(21 + bike));
        for (uint32_t k = 0; k < 16u; ++k) {
            const char c = static_cast<char>(g.U8(p + k));
            if (c == 0) break;
            name.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
        }
        const size_t dot = name.find('.');
        if (dot != std::string::npos) name.resize(dot);
        return name.empty() ? std::string() : "DATA/" + name + ".PH";
    };
    h.playerBikePh = bikePh(g.S32(kGameState + 0x48u));
    if (players >= 2) h.player2BikePh = bikePh(g.S32(kGameState + 0x4Cu));
}

void FrontEnd::EndRace(const Handover& h) {
    pending_ = h;
    pending_.raceRam.clear();
    if (h.returned && h.raceRam.size() >= 0x200000u) {
        std::vector<uint8_t> ram = h.raceRam;
        // Leaving the race from the pause menu is the original's 0x8005AF4C flag; the scene turns it
        // into result code 0xFF itself (0x800C6634 / 0x800C84F8 / 0x800C7510).
        if (h.quit) ram[0x8005AF4Cu & 0x1FFFFFu] = 1;
        results_.Begin(std::move(ram), &view_->GameStrings());
        prevCross_ = true; // the key that ended the race does not also end the scene
        resultsTexts_.clear();
        results_.Frame(resultsTexts_);
        cardNote_ = "results: the race's results scene (RASHCDG 0x800C5918) - Cross / Enter continues";
        if (!results_.Gap().empty()) cardNote_ += "; " + results_.Gap();
        return;
    }
    FinishRace();
}

void FrontEnd::FinishRace() {
    GuestRam& g = *g_;
    const Handover& h = pending_;
    if (h.returned) {
        g.WriteBlock(kSession, h.sessionAndPlayers, sizeof(h.sessionAndPlayers));
        g.WriteBlock(kRiders, h.riders, sizeof(h.riders));
        if (h.resident) { // the resident SLUS data as the race left it (handover.h)
            for (uint32_t t = 0; t < kResidentAlbumTracks; ++t)
                g.W32(kResidentAlbumFlags + kResidentAlbumStride * t, h.albumFlags[t]);
            g.WriteBlock(kResidentRecords, h.records, kResidentRecordBytes);
            for (uint32_t k = 0; h.sliders && k < kResidentSliderCount; ++k) {
                g.W32(kResidentSliders + 4u * k, h.volume[k]);
                g.W32(kResidentSlidersSaved + 4u * k, h.volumeSaved[k]);
            }
        }
    }
    // player[0]+0x20 (the result code) and +0x00 (the stamp): written by the race's results scene
    // through RASHCDG 0x800C84C0 (race_results.h) when it ran; else the rider record's +0x27 / +0x28,
    // the same two fields that function copies.
    uint32_t code = h.resultCode, ticks = h.resultTicks;
    if (h.returned && results_.Ran()) {
        code = results_.PlayerWord(0, 0x20u);
        ticks = results_.PlayerWord(0, 0x00u);
    }
    results_.End();
    // The shell overlay is read from the disc again on the way back: the resource
    // list's "loaded" word and the sound record's resident flag are the file's again.
    g.W32(0x8008972Cu, 0);
    g.W16(0x800897E4u + 32u * 17u, static_cast<uint16_t>(g.U16(0x800897E4u + 32u * 17u) & ~0xEu));
    g.W32(kPlayers + 0x20u, code);
    g.W32(kPlayers + 0x00u, ticks);
    g.W8(g.U32(kGameStatePtr), 2);
    // RASHCDF 0x8007FF4C: its three init calls (the logic part of 0x80080544, FrontendInit 0x800665E8,
    // ScreenTableInit 0x800806D0), the PORTED post-race dispatch (shell_career.h) and its tail.
    g.W32(g.U32(kGameStatePtr) + 0x34u, 4);
    ChooserTableInit(g);
    PadConfig(g, kPadLive, 0);
    PadConfig(g, kPadLive + 192u, 0);
    g.W32(0x8009C2F0u, 0);
    g.W32(0x8009C2F4u, 0);
    FrontendInit(g, frames_);
    ScreenTableInit(g);
    bool ok = true;
    const int32_t hub = ResumeDispatch(g, callees_, &ok);
    ResumeShow(g, hub);
    char line[160];
    std::snprintf(line, sizeof(line), "post-race dispatch 0x8007FF4C: mode 0x%02X result %u -> hub screen %d%s",
                  g.U32(kSession), code, hub, ok ? "" : " (a seam refused)");
    cardNote_ = line;
}

std::string FrontEnd::Status() {
    GuestRam& g = *g_;
    const int cur = g.S16(kFeCur);
    const uint32_t s = g.U32(kScreenTable + 4u * static_cast<uint32_t>(cur < 0 ? 0 : cur));
    char line[256];
    std::snprintf(line, sizeof(line), "screen %2d sel %2d mode 0x%02X venue %d race %d bike %d p2 bike %d | clicks %zu texts %zu unported %zu",
                  cur, s ? g.S16(s + 4u) : -1, g.U32(kSession), g.S8(kSession + 4u), g.S8(kSession + 8u),
                  g.S8(kPlayers + 7u), g.S8(kPlayers + 0x24u + 7u), callees_.uiSounds.size(), callees_.texts.size(),
                  callees_.unported.size());
    std::string out = line;
    if (std::getenv("RRJB_SHELL_TRACE") != nullptr) { // development: the bound chooser and the pad windows
        const uint32_t obj = g.U32(kFeChooser);
        const uint32_t item = s ? g.U32(s + 16u) + 120u * static_cast<uint32_t>(g.S16(s + 4u)) : 0u;
        std::snprintf(line, sizeof(line), " | item type %d code %d obj %08X kind %d opt %d/%d pads %d+%d p1 %02X%02X p2 %02X%02X",
                      item ? g.S16(item + 8u) : -1, item ? g.S16(item + 18u) : -1, obj, obj ? g.S8(obj + 2u) : -1,
                      obj ? g.S8(obj + 3u) : -1, obj ? g.S8(obj) : -1, s ? g.S8(s + 14u) : -1, s ? g.S8(s + 15u) : -1,
                      g.U8(PadSlot(0, 0)), g.U8(PadSlot(0, 1)), g.U8(PadSlot(1, 0)), g.U8(PadSlot(1, 1)));
        out += line;
    }
    if (cur >= 43 && cur <= 46 && g.U32(kCardPtr) == kCardRecord) { // the card screens (shell_memcard.h)
        std::snprintf(line, sizeof(line), " | card state %d prev %d slot %d hold %u", g.S16(kCardRecord + 8u),
                      g.S16(kCardRecord + 10u), g.S16(kCardRecord + 14u), g.U32(kCardHold));
        out += line;
    }
    for (const auto& u : callees_.unported) {
        char one[32];
        std::snprintf(one, sizeof(one), " 0x%08X", u.first);
        out += one;
    }
    return out;
}

// ---------------------------------------------------------------------------- the films
//
// The movie screens (transition 0x8009D0C0[i] == RASHCDF 0x8006DB5C: screens 0, 1, 2, 6, 11, 12, 15, 16,
// 19, 20, 22, 58), transcribed from our own disassembly of RASHCDF.BIN; the film itself is MoviePlayer.
namespace {
constexpr uint32_t kTransitionTable = 0x8009D0C0, kMovieTransition = 0x8006DB5C;
constexpr uint32_t kResourceNames = 0x8008973C; // the 42-name array the movie widget's +0x10 indexes
constexpr uint32_t kFeMovieHandle = kFe + 0xB4, kFeMovieMode = kFe + 0xB8;
} // namespace

// The entry arm (0x8006DB88..0x8006DD78): a screen entering (flags bit 1, +0x02 == 32767) with a movie
// widget at +0x10 and no film playing (fe+0x0A bit 0) builds "DATA\<name>" and opens it. Opened: fe+0xB4
// the handle, fe+0xB8 the widget's +0x18, the widget's frame counter +0x20 = 0 then 1 after the first
// picture, fe+0x0A |= 1. Not opened: the same exit with bit 0 clear, so the screen's handler advances at
// once. Both: fe+0x12 = 2 and +0x02 = 3 (the entry's count-down; ScreenTick's logic effect ends it).
// Not ported, named: the splash fade wait SLUS 0x80022A78 (a seam answering 0 here), the display
// reconfiguration 0x80080D08 / 0x8001BE08 / 0x8001BF1C / 0x8001C3F4 / 0x8001C408 (the picture is ours).
void FrontEnd::MovieEntry() {
    GuestRam& g = *g_;
    for (int32_t i = 0; i < kScreenCount; ++i) {
        const uint32_t s = g.U32(kScreenTable + 4u * static_cast<uint32_t>(i));
        if (s == 0 || g.U32(kTransitionTable + 4u * static_cast<uint32_t>(i)) != kMovieTransition) continue;
        const uint32_t w = g.U32(s + 0x10u);
        if (w == 0 || !(g.U16(s) & 2u) || g.S16(s + 2u) != 32767) continue;
        if (g.U16(kFeMedia) & 1u) continue;                        // a film still playing: wait
        std::string name;
        const uint32_t p = g.U32(kResourceNames + 4u * g.U32(w + 0x10u));
        for (uint32_t k = 0; k < 32u && p != 0; ++k) {
            const char c = static_cast<char>(g.U8(p + k));
            if (c == 0) break;
            name.push_back(c);
        }
        if (!movie_) movie_ = std::make_unique<MoviePlayer>();
        movie_->SetScale(hdScale_); // HD media (SetHdScale)
        if (!name.empty() && movie_->Open(disc_, name, g.S16(w + 0x1Cu), g.S16(w + 0x1Eu))) {
            g.W32(kFeMovieHandle, 1);                              // OURS: the handle value
            g.W16(kFeMovieMode, g.U16(w + 0x18u));
            g.W32(w + 0x20u, 1);
            g.W16(kFeMedia, static_cast<uint16_t>(g.U16(kFeMedia) | 1u));
            movieScreen_ = i;
            movieAudio_ = MovieAudio{};
            movieAudio_.start = true;
            movieAudio_.pcm = &movie_->Audio();
            movieAudio_.rate = movie_->AudioRate();
            movieAudio_.channels = movie_->AudioChannels();
            char line[160];
            std::snprintf(line, sizeof(line), "film: screen %d plays %s (%d pictures, %.1f s of sound)", i, name.c_str(),
                          movie_->Frames(), movie_->AudioRate() && movie_->AudioChannels()
                              ? static_cast<double>(movie_->Audio().size()) / movie_->AudioChannels() / movie_->AudioRate()
                              : 0.0);
            movieNote_ += (movieNote_.empty() ? "" : "\n") + std::string(line);
            if (movie_->PlayingHd()) movieNote_ += " - HD pictures from the HD pack";
        } else {
            movieNote_ += (movieNote_.empty() ? "" : "\n") + std::string("film: screen ") + std::to_string(i) +
                          " could not open '" + name + "': the handler advances at once";
        }
        g.W8(kFeHold, 2);
        g.W16(s + 2u, 3);
    }
}

// The tick 0x8006DE5C (with fe+0x0A bit 0): bit 2 (the PORTED MovieInput's skip) closes the film and
// clears bits 0..2; otherwise the next vertical blank of the film, and at its end the film closes, bit 0
// is cleared and fe+0x12 = 1. The leave arm of the transition (0x8006DD90..0x8006DE38): a movie screen
// left while its film plays closes it the same way. Either way the screen's handler then sees bit 0
// clear and takes its advance.
void FrontEnd::MovieTick() {
    if (!movie_ || !movie_->Playing() || movieScreen_ < 0) return;
    GuestRam& g = *g_;
    const uint32_t s = g.U32(kScreenTable + 4u * static_cast<uint32_t>(movieScreen_));
    if (s == 0 || !(g.U16(s) & 2u) || g.S16(kFeCur) != movieScreen_) { // left (or jumped away from)
        MovieClose(false);
        g.W8(kFeHold, 1);
        return;
    }
    if (g.U16(kFeMedia) & 4u) {
        MovieClose(true);
        return;
    }
    const uint32_t w = g.U32(s + 0x10u);
    if (!movie_->Tick()) {
        MovieClose(false);
        g.W8(kFeHold, 1);
    }
    if (w != 0) g.W32(w + 0x20u, g.U32(w + 0x20u) + 1u);
}

void FrontEnd::MovieClose(bool skipped) {
    GuestRam& g = *g_;
    const uint16_t media = g.U16(kFeMedia);
    g.W16(kFeMedia, static_cast<uint16_t>(skipped ? (media & 0xFFF8u) : (media & 0xFFFEu)));
    g.W32(kFeMovieHandle, 0xFFFFFFFFu);
    char line[128];
    std::snprintf(line, sizeof(line), "film: %s %s at picture %d of %d", movie_->Name().c_str(),
                  skipped ? "skipped" : "ended", movie_->Frame() + 1, movie_->Frames());
    movieNote_ += (movieNote_.empty() ? "" : "\n") + std::string(line);
    movie_->Close();
    movieScreen_ = -1;
    movieAudio_.stop = true;
}

FrontEnd::MovieAudio FrontEnd::TakeMovieAudio() {
    MovieAudio a = movieAudio_;
    movieAudio_ = MovieAudio{};
    return a;
}

bool FrontEnd::MovieDraw(std::vector<uint8_t>& rgba) const {
    if (!movie_ || !movie_->Playing()) return false;
    movie_->Draw(rgba);
    return true;
}

} // namespace rr::shell
