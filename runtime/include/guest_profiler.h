#pragma once

// NSMBW_PROFILE=<start s>:<length s>: sample the game thread and log where its time goes, by
// translated guest function (guest_profiler.cpp; Windows, Android and Linux).
// Switch: NSMBW_PROFILE=1 (or <start s>:<window s>) runs an in-process sampling profiler that
// pauses the game thread about a thousand times a second, reads its real ARM64 PC and call chain
// and sends each window's histogram to the development link ("P" record) or to
// Logs/<run>/profile/ on the SD card. Resolved offline by scripts/switch/profile-report.py.
namespace GuestProfiler {
// From the thread that presents frames, at each present: the first call starts the sampler.
void OnFramePresented();
// The game's current scene (vi.cpp, on each change): every sample carries it, so the report can be
// split by scene (Switch: a scene change closes the current window).
void NoteScene(unsigned scene);
// Switch: writes the window in progress now (the run is ending, or the dev link asked for it) and
// waits briefly for it to be written. Elsewhere nothing.
void FlushWindow(const char* reason);
// For the slow-frame log: this process's page faults so far (Windows; zero elsewhere), and the calling
// thread's CPU time so far in user and kernel mode, in ms (Windows; Switch as user time only; zero
// elsewhere).
unsigned ProcessPageFaults();
void ThreadCpuMs(double* user, double* kernel);
} // namespace GuestProfiler
