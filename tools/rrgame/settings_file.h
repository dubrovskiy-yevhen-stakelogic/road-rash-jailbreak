#pragma once
// rrgame_settings.ini as sections: one reader and one writer for every section the game keeps there ([graphics],
// [handling], [cheats], [vr]).
//   * Reading: a UTF-8 byte-order mark and CR line ends are ignored (a file edited in a Windows editor or copied through
//     a PowerShell pipe carries both); every block of the section counts, in order, when the file holds it twice.
//   * Writing a section: the file is read again, every line outside the section is kept as it was, the section is
//     written ONCE (duplicates merged into it) with the caller's complete key list, and any key the old section carried
//     that the caller does not write is kept after it (a key of another build of the game survives this build's save);
//     `retired` names keys that are dropped instead. The file is replaced through a temporary (never half written).
#include <string>
#include <utility>
#include <vector>

namespace rrgame {

using IniPairs = std::vector<std::pair<std::string, std::string>>;

// The lines of the file (the byte-order mark and CRs removed); false when it cannot be opened.
bool ReadIniLines(const std::string& path, std::vector<std::string>& lines);
// The section's key / value pairs (trimmed), in file order. `present`: the file has a [name] header at all.
// False when the file cannot be opened.
bool ReadIniSection(const std::string& path, const std::string& name, IniPairs& out, bool* present = nullptr);
// `body` - the section's key=value lines (without the header). See the top for what is kept.
bool WriteIniSection(const std::string& path, const std::string& name, const std::string& body,
                     const std::vector<std::string>& retired = {});
// DEVELOPMENT RRJB_SETTINGS_MARK=off: a one-time migration does not write its marker back at the start (the control of
// the round trip's "runs once"); RRJB_INI_BOM=keep: the reader keeps a UTF-8 byte-order mark (the control of the
// reader's strip).
bool SettingsMarkersOff();

} // namespace rrgame
