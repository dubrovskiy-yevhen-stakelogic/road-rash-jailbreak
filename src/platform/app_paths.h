#pragma once
// Where the game finds the player's disc image and keeps its own files, per platform.
//
// Windows: the tools take the disc path on their command line (rrgame also finds an installed one next to the exe,
// rrvfs/disc_identity.h); the saves and settings live in `saves\` next to rrgame.exe (the installed layout,
// scripts\install.ps1) - SavesDir(). DataRoot() is the current directory.
// Android: an application has no working directory worth the name. The host (the NativeActivity's android_main) hands
// over the app-specific EXTERNAL files directory - /sdcard/Android/data/<package>/files, readable and writable over
// adb without any permission, where the installer copies the player's own disc image (docs\QUEST.md "Headset layout")
// - and the INTERNAL one (/data/data/<package>/files, invisible to the shell, reached by the save transfer with
// `run-as`), whose saves/ holds the memory card, the settings and the controls.
#include <filesystem>
#include <string>

namespace rr::platform {

void SetAppDirs(const std::string& external, const std::string& internal);

std::filesystem::path DataRoot();      // the external files dir on Android; "." elsewhere
std::filesystem::path SavesDir();      // <internal>/saves on Android, <exe dir>\saves on Windows (created)
std::filesystem::path ExecutableDir(); // the folder of the running executable (Windows); DataRoot() elsewhere
// The player's disc image in DataRoot(): `disc.bin` if present, else the first *.bin / *.img there by name. Empty when
// there is none (the caller says where to push one).
std::filesystem::path FindDiscImage();

} // namespace rr::platform
