#pragma once

// A native FxReverbHi::UpdateBuffer for couch play (axfx_reverb_native.cpp). Install, once at
// startup after lockstep (LAN) mode is decided, routes the game's calls to it; in lockstep play it
// does nothing and the translated, bit-exact reverb runs.
namespace AxfxReverbNative {
void Install();
// NSMBW_REVERB_MT: waits until the reverb block handed to the worker thread is complete. AxDspHle's
// JoinMix calls it, so everything that waits for the mix also waits for the reverb. Cheap when off.
void JoinWorker();
} // namespace AxfxReverbNative
