#pragma once
// Inline thread-pointer access for the few thread_locals every guest call touches
// (g_currentCpuContext, the translated-execution address, the FPSCR[NI] mirror and the
// indirect-dispatch memos).
//
// devkitA64 compiles the Switch build with -mtp=soft: each thread_local access is a call to
// libnx's __aarch64_read_tp (mrs tpidrro_el0; ldr x0, [x0, #0x1f8]; ret), and GCC treats that
// call as volatile, so it is repeated for every access - four times per InvokeIndirectCpu, about
// 48k call sites in the whole program, and a forced frame record in any leaf that has one.
// MKW_HOT_TLS_ACCESSOR emits the same two instructions inline and then adds the variable's
// local-exec offset with the relocation pair GCC itself emits for -ftls-model=local-exec, so the
// result is exactly &variable on every thread. VerifyHotTlsAccessors() (abi_bridge.cpp) compares
// both addresses once at startup and stops the program if they ever disagree.
//
// Every other platform (and a Switch build with -DMKW_SWITCH_SOFT_TP_ONLY) gets accessors that
// simply return the variable.

#include <cstdint>

#if defined(__SWITCH__) && defined(__aarch64__) && !defined(MKW_SWITCH_SOFT_TP_ONLY)
#define MKW_SWITCH_INLINE_TP 1

// libnx: tpidrro_el0 holds the thread's 0x200-byte thread-local region; its last 0x20 bytes are
// libnx's ThreadVars, whose final field is the ELF TLS thread pointer. Same offset as the
// #504 in __aarch64_read_tp.
inline constexpr uintptr_t kMkwNxThreadVarsTlsTpOffset = 0x1F8;

__attribute__((always_inline)) inline uintptr_t MkwSwitchThreadPointer() noexcept
{
    uintptr_t region;
    // volatile: never reuse a thread pointer across a call, exactly like the soft-tp call it
    // replaces (a host fiber must not carry one thread's TLS address over to another).
    __asm__ __volatile__("mrs %0, tpidrro_el0" : "=r"(region));
    return *reinterpret_cast<const uintptr_t*>(region + kMkwNxThreadVarsTlsTpOffset);
}

// `asm_symbol` is the variable's assembler name: unmangled for the global namespace, the
// Itanium-mangled name otherwise.
#define MKW_HOT_TLS_ACCESSOR(accessor, type, variable, asm_symbol)                        \
    __attribute__((always_inline)) inline type& accessor() noexcept                       \
    {                                                                                     \
        uintptr_t address;                                                                \
        __asm__("add %0, %1, #:tprel_hi12:" asm_symbol ", lsl #12\n\t"                     \
                "add %0, %0, #:tprel_lo12_nc:" asm_symbol                                  \
                : "=r"(address)                                                           \
                : "r"(MkwSwitchThreadPointer()));                                         \
        return *reinterpret_cast<type*>(address);                                         \
    }
#else
#define MKW_HOT_TLS_ACCESSOR(accessor, type, variable, asm_symbol)                        \
    inline type& accessor() noexcept { return variable; }
#endif
