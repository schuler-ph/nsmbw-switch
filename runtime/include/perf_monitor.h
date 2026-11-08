#pragma once

// Performance monitor for tuning runs on the Switch (and any other platform, with fewer sources):
// per one-second window and per scene it measures
//   - FPS (presented game frames per second) and the 1 % low (1000 / 99th-percentile frame time),
//   - game speed in %: game frames per second against the VI's nominal rate (NSMBW advances its
//     world once per frame and does not skip, so fewer frames are slower play), plus the VI's own
//     rate (retraces per second) and the audio rate (sample frames the game produced against the
//     output rate),
//   - per-thread CPU time (game, gx_fifo, frame_worker, presenter, pipeline, pipeline_writer,
//     audio_mix, audio_out = SDL's output thread, devlink), in % of one core and in ms per frame,
//   - on the Switch, CPU/GPU/EMC clocks (clkrst), SoC temperature (ts) and docked/handheld, where
//     the services answer.
// Shown as an ImGui overlay (NSMBW_PERF_OVERLAY=1, or "overlay on" over the development link),
// sent to the PC as the development link's "S" record, and logged as a per-scene table when the
// program ends through the runtime ("[perf] scene ..." lines).
//
// Self-ending test runs: NSMBW_EXIT_AFTER_S=<seconds> ends the program that long after the first
// presented frame, NSMBW_EXIT_AFTER_SCRIPT=<ms> that long after NSMBW_INPUT_SCRIPT's last event.

#include <cstdint>

#include "dev_link.h"

namespace PerfMonitor {

// vi.cpp, once per presented frame on the game thread (also hands the frame to the dev link).
void OnFrame(const DevLink::FrameSample& sample) noexcept;
// audio_backend.cpp: `frames` stereo sample frames the game produced at `rate` Hz; `queued` is
// false when the output queue was full and they were dropped.
void NoteAudio(uint32_t frames, uint32_t rate, bool queued) noexcept;
// settings_overlay.cpp, inside the ImGui frame.
void DrawOverlay() noexcept;
void SetOverlay(bool on) noexcept;
bool Overlay() noexcept;
// Logs the per-scene table now ("[perf] ..." on stderr); also done before a runtime-initiated exit.
void LogRunSummary(const char* reason) noexcept;
// Ends the program the way closing it does, after logging the summary (the dev link's "quit",
// the NSMBW_EXIT_AFTER_* limits).
void RequestExit(const char* reason) noexcept;

} // namespace PerfMonitor
