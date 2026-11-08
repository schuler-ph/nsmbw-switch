#pragma once

// Development link (Switch bring-up): the runtime connects to a harness on the PC over TCP, streams
// its stdout/stderr lines and per-frame telemetry there, and takes commands back (screenshot, quit,
// button presses). Implemented in src/platform/switch/dev_link.cpp; on every other platform the
// DevLink calls are empty, so shared code (vi.cpp, switch_platform.cpp) calls them unconditionally.
//
// Switch: on only when sdmc:/switch/nsmbw/devhost.txt exists, holding "<PC address>[:<port>]"
// (default port 28772). The PC side is scripts/switch/devlink.py in the nsmbwii2switch repository.
// Nothing the game does waits for the network: lines and frames go into a bounded queue that one
// background thread sends; with no PC listening, it retries the connection every few seconds.
//
// Wire format, Switch -> PC, one record per line ('\n'):
//   H <json>                hello after every (re)connect: start_ms (same for one process), frame
//   L1 <text> / L2 <text>   one stdout / stderr line
//   F <json>                one presented frame (only while connected)
//   S <json>                one-second summary (queued even while disconnected)
//   A <text>                answer to a command
//   I <id> <w> <h> <bytes>  followed by <bytes> of tightly packed RGBA8, row 0 at the top
// PC -> Switch, one command per line: ping, status, screenshot [count], quit,
//   press <BUTTONS> [ms] (Wii Remote names joined by '+', as in NSMBW_INPUT_SCRIPT), mark <text>.

#include <cstddef>
#include <cstdint>

namespace DevLink {

// What vi.cpp measured for one presented frame, in milliseconds.
struct FrameSample {
    uint32_t frame = 0;    // presented XFB frames so far
    uint32_t scene = 0;    // dScene_c::m_nowScene
    double frameMs = 0;    // end of the previous present to the end of this one
    double gameMs = 0;     // game thread until the present started
    double gxMs = 0;       // of which GX command processing
    double discMs = 0;     // of which disc reads
    uint32_t discReads = 0;
    double presentMs = 0;
    double endFrameMs = 0;
    double paceMs = 0;
    double cpuMs = 0;      // the game thread's own CPU time over the frame
};

#if defined(__SWITCH__)
// Reads devhost.txt and starts the link thread (after the log files are open). Idempotent.
void Start() noexcept;
// One chunk of stdout (stream 1) or stderr (stream 2), as the log tee writes it. Never blocks for
// long; safe from the crash handler (drops the chunk if the queue lock stays busy).
void LogChunk(int stream, const char* data, size_t length) noexcept;
// From vi.cpp, once per presented frame.
void OnFrame(const FrameSample& sample) noexcept;
// From the crash handler: gives the link thread up to `timeoutMs` to send what is queued.
void FlushForCrash(uint32_t timeoutMs) noexcept;
#else
inline void Start() noexcept {}
inline void LogChunk(int, const char*, size_t) noexcept {}
inline void OnFrame(const FrameSample&) noexcept {}
inline void FlushForCrash(uint32_t) noexcept {}
#endif

} // namespace DevLink

// Button presses injected from outside (DevLink's "press" command), on every platform: OR-ed into
// the Wii Remote sample the game reads while they last (wii_remote_input.cpp).
namespace DevInput {
// `buttons` as in NSMBW_INPUT_SCRIPT ("A", "A+TWO", "RIGHT"); false for an unknown name.
bool Press(const char* buttons, uint32_t durationMs);
// The WPAD bits currently injected for `chan` (0 when nothing is held).
uint32_t HeldBits(uint32_t chan);
} // namespace DevInput
