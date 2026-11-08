#pragma once

// NSMBW_PROFILE=<start s>:<length s>: sample the game thread and log where its time goes, by
// translated guest function (guest_profiler.cpp; Windows, Android and Linux).
namespace GuestProfiler {
// From the thread that presents frames, at each present: the first call starts the sampler.
void OnFramePresented();
// The game's current scene (vi.cpp, on each change): every sample carries it, so the report can be
// split by scene.
void NoteScene(unsigned scene);
// For the slow-frame log: this process's page faults so far (Windows; zero elsewhere), and the calling
// thread's CPU time so far in user and kernel mode, in ms (Windows; Switch as user time only; zero
// elsewhere).
unsigned ProcessPageFaults();
void ThreadCpuMs(double* user, double* kernel);
} // namespace GuestProfiler
