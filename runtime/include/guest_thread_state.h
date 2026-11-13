#pragma once

#include <cstdint>

#include "isa/ppc_isa_tls.h"

struct CpuContext;

// The thread-local state every guest call touches, kept in one thread_local. Android's build targets
// API 26, where thread_locals are emulated: each distinct variable a function touches costs a call to
// __emutls_get_address (pthread_getspecific underneath). With both fields in one variable, an
// indirect call looks its thread up once instead of once per field.
struct GuestThreadState {
    // The CpuContext translated code on this thread runs with (CpuContextScope, TryGetCpuContext).
    CpuContext* currentCpuContext = nullptr;
    // Diagnostic only, read by fatal reporters (RecompMod::ScopedTranslatedExecutionAddress).
    uint32_t translatedExecutionAddress = 0;
};

// `inline thread_local` with a constant initializer: no per-access init guard.
inline thread_local GuestThreadState g_guestThreadState;
// Hot-path access (isa/ppc_isa_tls.h): on the Switch an inline thread-pointer read instead of a
// __aarch64_read_tp call per access; elsewhere just g_guestThreadState.
MKW_HOT_TLS_ACCESSOR(GuestThreadStateTls, GuestThreadState, g_guestThreadState, "g_guestThreadState")
