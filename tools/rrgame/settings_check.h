#pragma once
// rrgame --settings-roundtrip-check <ini> [start]: the settings file read as an interactive start reads it (the one-time
// migrations and their markers), one value changed on every menu that saves to it, then read back and compared key by
// key; the file is changed in place. Returns the process's exit code (0: every section kept).
#include <string>

namespace rrgame {

// `startOnly`: only the start (the reads, the migrations and their markers) - a player who changes nothing.
int SettingsRoundTripCheck(const std::string& path, bool startOnly = false);

} // namespace rrgame
