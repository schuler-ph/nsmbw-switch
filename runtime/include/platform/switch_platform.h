#pragma once

// Nintendo Switch homebrew (devkitA64/libnx) process services: the SD-card folder layout, the
// startup log, the launch-mode check and the user-visible error message. Implemented in
// src/platform/switch/switch_platform.cpp, together with the crash handler and the thread-stack
// wrap. Parts are ported from DriftDroid (dorPXP/driftdroid, GPL-3.0), runtime/src/main.cpp and
// runtime/src/switch_launcher.cpp.
//
// Everything lives in one folder on the SD card:
//   sdmc:/switch/nsmbw/            nsmbw.nro, the runtime's data files (dsp_coef.bin,
//                                  initial_pipeline_cache.db, wii_bootstrap/), Config.toml, NAND/,
//                                  Cache/, Logs/
//   sdmc:/switch/nsmbw/game/       the player's own extracted disc (files/ and sys/)
//   sdmc:/switch/nsmbw/log.txt     stdout of the last run
//   sdmc:/switch/nsmbw/stderr.txt  stderr of the last run, including [switch-crash] lines
#if defined(__SWITCH__)

#include <string>

namespace SwitchPlatform {

inline constexpr const char* kDataDirectory = "sdmc:/switch/nsmbw";
inline constexpr const char* kGameDirectory = "sdmc:/switch/nsmbw/game";

// First thing in RuntimeMain: creates the data folder, sends stdout/stderr to log.txt and
// stderr.txt (unbuffered, so a crash loses nothing) and, when started through nxlink, also live to
// the PC (nxlink -s), then logs the launch facts (applet type, memory).
void EarlyInit() noexcept;

// True when the program runs as a library applet (hbmenu opened from the Album). That mode gets far
// too little memory for the game; it must be started through a title override or a forwarder.
bool InAppletMode() noexcept;

// Initializes libnx's socket service once (nxlink stdio and the development link share it).
bool EnsureSockets() noexcept;

// Shows `text` on the libnx text console until the player presses + (or the system asks the
// program to close). Only for use before aurora owns the screen.
void ShowConsoleMessage(const std::string& text) noexcept;

// Shows `text` in the system's error dialog (works while aurora owns the screen). Logged as well.
void ShowErrorDialog(const std::string& shortText, const std::string& longText) noexcept;

// Ends the process at once, without atexit handlers or static destructors (other threads such as
// the shader compiler may still be running). stdout/stderr are unbuffered, so the logs are complete.
[[noreturn]] void TerminateNow(int code) noexcept;

} // namespace SwitchPlatform

#endif
