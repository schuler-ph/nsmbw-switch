// Nintendo Switch process services; see include/platform/switch_platform.h. The crash handler, the
// module-relative address helpers and the thread-stack wrap are ported from DriftDroid
// (dorPXP/driftdroid, GPL-3.0), runtime/src/main.cpp; the applet-mode check from its
// runtime/src/switch_launcher.cpp.
#if defined(__SWITCH__)

#include "platform/switch_platform.h"

#include <atomic>
#include "dev_link.h"

#include <cerrno>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <sys/iosupport.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <switch.h>
#include <cstring>

extern "C" {
extern int __system_argc;
extern char** __system_argv;
}

namespace {

// Writes one line of crash output to stderr (stderr.txt on the SD card, see EarlyInit).
void SwitchCrashWrite(const char* format, ...) {
    std::va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fflush(stderr);
}

// The NRO is loaded at a different randomised base on every launch, so crash addresses are printed
// relative to the module: `aarch64-none-elf-addr2line -e WiiCompiled.elf <offset>` resolves them
// against the unstripped ELF of the same build. `__start__` stays 0 at run time, so ask the kernel
// for the code region holding one of our own functions instead.
u64 SwitchModuleBase() {
    static const u64 base = [] {
        MemoryInfo info{};
        u32 pageInfo = 0;
        if (R_FAILED(svcQueryMemory(&info, &pageInfo, reinterpret_cast<u64>(&SwitchModuleBase)))) {
            return u64{0};
        }
        return info.addr;
    }();
    return base;
}

u64 SwitchModuleOffset(u64 address) {
    const u64 base = SwitchModuleBase();
    return address >= base ? address - base : address;
}

const char* AppletTypeName(AppletType type) {
    switch (type) {
    case AppletType_Application:
        return "application";
    case AppletType_SystemApplication:
        return "system application";
    case AppletType_LibraryApplet:
        return "library applet";
    case AppletType_OverlayApplet:
        return "overlay applet";
    case AppletType_SystemApplet:
        return "system applet";
    default:
        return "other";
    }
}

} // namespace

// The crash handler runs on libnx's exception stack; the default one is small for vfprintf and a
// frame walk, so give it room.
extern "C" {
alignas(16) u8 __nx_exception_stack[0x10000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
}

// Thread placement, after DriftDroid's runtime/src/main.cpp (SwitchSpreadHelperThread and the
// Switch*Thread hooks). libnx's __syscall_thread_create (disassembled from libnx.a: threadCreate
// with priority 0x3B and cpuid -2, then svcSetThreadCoreMask(-1, process mask)) starts every
// std::thread on the process default core - the core the guest thread runs on - at the
// time-sliced priority 0x3B. In runs 12/13 aurora's pipeline worker, the frame and presentation
// workers and the AX mixer all competed for core 0's leftovers while cores 1 and 2 idled: the boot
// prewarm took ~1 100 s and game frames ran 25-220 ms.
//
// So every helper gets an ideal core drawn round-robin from the cores the guest is NOT on, with all
// non-guest cores in its mask (biased, not pinned). Threads on the audio path ask for a priority
// above 0x3B so they preempt the pipeline compiler, which stays at 0x3B on one core.
namespace {
std::atomic<int> g_switchGuestCore{-1};
std::atomic<uint32_t> g_switchHelperCoreCursor{0};

u64 SwitchHelperCoreMask() noexcept {
    const int guestCore = g_switchGuestCore.load(std::memory_order_relaxed);
    if (guestCore < 0) {
        return 0;
    }
    u64 processMask = 0;
    if (R_FAILED(svcGetInfo(&processMask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) {
        return 0;
    }
    return processMask & ~(1ull << static_cast<unsigned>(guestCore));
}

void SwitchSpreadHelperThread(Handle handle) noexcept {
    const u64 helperMask = SwitchHelperCoreMask();
    if (helperMask == 0) {
        return;  // guest core not recorded yet, or a single-core budget
    }
    const uint32_t slot = g_switchHelperCoreCursor.fetch_add(1, std::memory_order_relaxed);
    int target = static_cast<int>(slot % static_cast<uint32_t>(__builtin_popcountll(helperMask)));
    for (int core = 0; core < 64; ++core) {
        if ((helperMask & (1ull << core)) != 0 && target-- == 0) {
            svcSetThreadCoreMask(handle, core, helperMask);
            return;
        }
    }
}
} // namespace

// runtime/CMakeLists.txt links with -Wl,--wrap=__syscall_thread_create, which redirects every
// newlib/libnx thread creation here. libnx gives a std::thread a 128 KiB stack, which Tint's WGSL
// resolver (aurora's pipeline workers) and mesa's GLSL compiler overflow; raise the floor to 2 MiB.
// Only when libnx allocates the stack: a caller-supplied buffer must keep its declared size.
// This definition must exist whenever the link option does, or every std::thread fails (ENOSYS).
extern "C" int __real___syscall_thread_create(void** thread, void* entry, void* arg, void* stackAddr,
                                              size_t stackSize);
extern "C" int __wrap___syscall_thread_create(void** thread, void* entry, void* arg, void* stackAddr,
                                              size_t stackSize) {
    constexpr size_t kMinimumThreadStack = 2u * 1024u * 1024u;
    if (stackAddr == nullptr && stackSize < kMinimumThreadStack) {
        stackSize = kMinimumThreadStack;  // 4 KiB aligned, as libnx requires
    }
    const int result = __real___syscall_thread_create(thread, entry, arg, stackAddr, stackSize);
    if (result == 0 && thread != nullptr && *thread != nullptr) {
        // libnx's thread object starts with the kernel handle (its Thread struct; the disassembly
        // loads that first word and hands it to svcSetThreadCoreMask). The thread is already
        // running, so the new placement applies from its next scheduling point.
        SwitchSpreadHelperThread(*static_cast<const Handle*>(*thread));
    }
    return result;
}

// aurora's pipeline compilation workers (aurora-main/lib/gfx/pipeline_cache.cpp) call this when
// they start: the lowest priority the process may use (0x3B, time-sliced) and only the highest
// non-guest core, so the other helper core stays free for the frame/presentation workers and audio.
extern "C" void SwitchConfigurePipelineWorkerThread() {
    const u64 helperMask = SwitchHelperCoreMask();
    Result maskRc = 0;
    int core = -1;
    if (helperMask != 0) {
        core = 63 - __builtin_clzll(helperMask);
        maskRc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1ull << core);
    }
    const Result prioRc = svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3B);
    std::fprintf(stderr, "[switch] pipeline worker on core %d (rc 0x%x), priority 0x3B (rc 0x%x)\n", core,
                 static_cast<unsigned>(maskRc), static_cast<unsigned>(prioRc));
}

// The AX/DSP mix worker (runtime/src/hle/audio/ax_mix.cpp): the guest waits for it every audio
// block. Prefers the lowest helper core (away from the pipeline worker), may move; priority 0x2D,
// above every 0x3B helper and below the SDL output thread.
extern "C" void SwitchConfigureAudioMixThread() {
    const u64 helperMask = SwitchHelperCoreMask();
    Result maskRc = 0;
    int core = -1;
    if (helperMask != 0) {
        core = __builtin_ctzll(helperMask);
        maskRc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, helperMask);
    }
    const Result prioRc = svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2D);
    std::fprintf(stderr, "[switch] AX mix worker prefers core %d (rc 0x%x), priority 0x2D (rc 0x%x)\n", core,
                 static_cast<unsigned>(maskRc), static_cast<unsigned>(prioRc));
}

// SDL's audio device thread (runtime/src/audio_backend.cpp, postmix callback). It only copies a
// mixed buffer to audout but must never miss its turn, or audout's queue runs dry (DriftDroid
// measured that stutter). Highest helper priority.
extern "C" void SwitchRaiseAudioOutputThread() {
    const Result rc = svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2B);
    std::fprintf(stderr, "[switch] SDL audio output thread priority 0x2B (rc 0x%x, core %u)\n",
                 static_cast<unsigned>(rc), svcGetCurrentProcessorNumber());
}

// libnx implements no thread detach: newlib's pthread_detach returns ENOSYS when the weak
// __syscall_thread_detach is missing, so every std::thread::detach() throws (DVD reads, VI and the
// network lookups detach their workers). Supply it: a detached thread is handed to one reaper
// thread that joins them in turn, which frees each thread's stack once it has finished.
namespace {
std::mutex& ReaperMutex() {
    static std::mutex mutex;
    return mutex;
}
std::condition_variable& ReaperWake() {
    static std::condition_variable wake;
    return wake;
}
std::deque<void*>& ReaperQueue() {
    static std::deque<void*> queue;
    return queue;
}
void ReaperMain() {
    for (;;) {
        void* thread = nullptr;
        {
            std::unique_lock lock(ReaperMutex());
            ReaperWake().wait(lock, [] { return !ReaperQueue().empty(); });
            thread = ReaperQueue().front();
            ReaperQueue().pop_front();
        }
        __syscall_thread_join(static_cast<struct __pthread_t*>(thread));
    }
}
} // namespace

extern "C" int __syscall_thread_detach(struct __pthread_t* thread) {
    static std::once_flag started;
    std::call_once(started, [] {
        // The reaper itself is never detached (that would recurse): it is created once and leaked.
        new std::thread(ReaperMain);
    });
    {
        std::lock_guard lock(ReaperMutex());
        ReaperQueue().push_back(thread);
    }
    ReaperWake().notify_one();
    return 0;
}

// hbloader runs a homebrew .nro inside its own process, so an unhandled CPU fault produces no
// Atmosphere crash report naming this program; libnx hands it to this handler instead, and these
// [switch-crash] lines in stderr.txt are the only record of the crash.
extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    SwitchCrashWrite("[switch-crash] exception desc=0x%x pc=+0x%llx lr=+0x%llx sp=0x%llx far=0x%llx base=0x%llx\n",
                     ctx->error_desc, static_cast<unsigned long long>(SwitchModuleOffset(ctx->pc.x)),
                     static_cast<unsigned long long>(SwitchModuleOffset(ctx->lr.x)),
                     static_cast<unsigned long long>(ctx->sp.x), static_cast<unsigned long long>(ctx->far.x),
                     static_cast<unsigned long long>(SwitchModuleBase()));
    for (int reg = 0; reg < 29; reg += 4) {
        SwitchCrashWrite("[switch-crash] x%02d %016llx %016llx %016llx %016llx\n", reg,
                         static_cast<unsigned long long>(ctx->cpu_gprs[reg].x),
                         static_cast<unsigned long long>(reg + 1 < 29 ? ctx->cpu_gprs[reg + 1].x : 0),
                         static_cast<unsigned long long>(reg + 2 < 29 ? ctx->cpu_gprs[reg + 2].x : 0),
                         static_cast<unsigned long long>(reg + 3 < 29 ? ctx->cpu_gprs[reg + 3].x : 0));
    }
    // Frame-pointer walk; guest fibers run on heap stacks the frame chain still threads through.
    // The checks stop a corrupted chain from looping or faulting again.
    u64 fp = ctx->fp.x;
    for (int frame = 0; frame < 32 && fp != 0 && (fp & 0xF) == 0; ++frame) {
        MemoryInfo info{};
        u32 pageInfo = 0;
        if (R_FAILED(svcQueryMemory(&info, &pageInfo, fp)) || (info.perm & Perm_R) == 0 ||
            fp + 16 > info.addr + info.size) {
            break;
        }
        const auto* entry = reinterpret_cast<const u64*>(fp);
        const u64 next = entry[0];
        const u64 lr = entry[1];
        if (lr == 0) {
            break;
        }
        SwitchCrashWrite("[switch-crash] #%02d +0x%llx\n", frame, static_cast<unsigned long long>(SwitchModuleOffset(lr)));
        if (next <= fp) {
            break;  // frame pointers must grow towards the stack base
        }
        fp = next;
    }
    SwitchCrashWrite("[switch-crash] end\n");
    // The development link's thread still runs: give it the time to send the crash lines to the PC.
    DevLink::FlushForCrash(1500);
    // hbloader unmaps this module's code while sibling threads still run, so returning (or a normal
    // exit) faults again inside the unmapped text. Leave immediately.
    svcExitProcess();
}

// mbed TLS platform hooks (src/platform/switch/mbedtls_switch_config.h): entropy from the kernel's
// CSPRNG and a monotonic millisecond clock.
extern "C" int mbedtls_hardware_poll(void* data, unsigned char* output, size_t length, size_t* produced) {
    (void)data;
    randomGet(output, length);
    *produced = length;
    return 0;
}

extern "C" int64_t mbedtls_ms_time(void) {
    return static_cast<int64_t>(armTicksToNs(armGetSystemTick()) / 1000000ull);
}

namespace {

// stdout and stderr go to their file on the SD card and, for an nxlink launch (nxlink -s), to the
// PC as well: libnx's nxlinkStdio() would replace the files, so both descriptors get a small
// device that writes every chunk to both places.
int g_logFile[3] = {-1, -1, -1};
int g_nxlinkSocket = -1;

ssize_t TeeWrite(int stream, const char* data, size_t length) {
    DevLink::LogChunk(stream, data, length);
    if (g_logFile[stream] >= 0) {
        size_t written = 0;
        while (written < length) {
            const ssize_t chunk = write(g_logFile[stream], data + written, length - written);
            if (chunk <= 0) {
                break;
            }
            written += static_cast<size_t>(chunk);
        }
    }
    if (g_nxlinkSocket >= 0) {
        size_t sent = 0;
        while (sent < length) {
            const ssize_t chunk = send(g_nxlinkSocket, data + sent, length - sent, 0);
            if (chunk <= 0) {
                const int sendErrno = errno;
                g_nxlinkSocket = -1;  // the PC went away; keep writing the file
                if (g_logFile[2] >= 0) {
                    char note[96];
                    const int noteLength = std::snprintf(note, sizeof(note),
                        "\n[switch] nxlink stdio lost: send returned %zd, errno %d\n", chunk, sendErrno);
                    if (noteLength > 0) {
                        (void)write(g_logFile[2], note, static_cast<size_t>(noteLength));
                    }
                }
                break;
            }
            sent += static_cast<size_t>(chunk);
        }
    }
    return static_cast<ssize_t>(length);
}

ssize_t TeeStdout(struct _reent*, void*, const char* data, size_t length) {
    return TeeWrite(1, data, length);
}

ssize_t TeeStderr(struct _reent*, void*, const char* data, size_t length) {
    return TeeWrite(2, data, length);
}

devoptab_t g_teeStdout{};
devoptab_t g_teeStderr{};

void StartLogTee() noexcept {
    g_logFile[1] = open("sdmc:/switch/nsmbw/log.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    g_logFile[2] = open("sdmc:/switch/nsmbw/stderr.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (__nxlink_host.s_addr != 0 && SwitchPlatform::EnsureSockets()) {
        g_nxlinkSocket = nxlinkConnectToHost(false, false);
    }
    g_teeStdout.name = "stdout";
    g_teeStdout.write_r = TeeStdout;
    g_teeStderr.name = "stderr";
    g_teeStderr.write_r = TeeStderr;
    devoptab_list[STD_OUT] = &g_teeStdout;
    devoptab_list[STD_ERR] = &g_teeStderr;
}

} // namespace

namespace SwitchPlatform {

bool EnsureSockets() noexcept {
    static const bool up = R_SUCCEEDED(socketInitializeDefault());
    return up;
}

bool InAppletMode() noexcept {
    const AppletType type = appletGetAppletType();
    return type != AppletType_Application && type != AppletType_SystemApplication;
}

void EarlyInit() noexcept {
    mkdir("sdmc:/switch", 0777);
    mkdir(kDataDirectory, 0777);
    StartLogTee();
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    u64 totalMemory = 0;
    svcGetInfo(&totalMemory, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    const std::time_t now = std::time(nullptr);
    char started[32] = {};
    std::strftime(started, sizeof(started), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    std::fprintf(stderr, "[switch] NSMBW Recompiled (Switch) starting at %s\n", started);
    std::fprintf(stderr, "[switch] applet type: %s, process memory: %llu MiB, module base 0x%llx, core %u\n",
                 AppletTypeName(appletGetAppletType()), static_cast<unsigned long long>(totalMemory >> 20),
                 static_cast<unsigned long long>(SwitchModuleBase()), svcGetCurrentProcessorNumber());
    // EarlyInit runs on the thread that becomes the guest thread (RuntimeMain); helpers created
    // from here on are steered off this core (SwitchSpreadHelperThread).
    g_switchGuestCore.store(static_cast<int>(svcGetCurrentProcessorNumber()), std::memory_order_relaxed);
    {
        u64 coreMask = 0;
        svcGetInfo(&coreMask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
        std::fprintf(stderr, "[switch] guest core %d, process core mask 0x%llx\n",
                     g_switchGuestCore.load(std::memory_order_relaxed), static_cast<unsigned long long>(coreMask));
    }
    std::fprintf(stderr, "[switch] data folder %s, game folder %s\n", kDataDirectory, kGameDirectory);
    std::fprintf(stderr, "[switch] nxlink stdio: %s\n", g_nxlinkSocket >= 0 ? "connected" : "off");
    // Horizon has no environment for a launch; sdmc:/switch/nsmbw/gfx_flags.txt stands in for
    // AURORA_GFX_FLAGS (for example "validate" turns Dawn's validation back on).
    if (FILE* flags = std::fopen("sdmc:/switch/nsmbw/gfx_flags.txt", "r")) {
        char value[256] = {};
        const size_t length = std::fread(value, 1, sizeof(value) - 1, flags);
        std::fclose(flags);
        value[length] = 0;
        for (size_t i = length; i > 0 && (value[i - 1] == '\n' || value[i - 1] == '\r' || value[i - 1] == ' '); --i) {
            value[i - 1] = 0;
        }
        setenv("AURORA_GFX_FLAGS", value, 1);
        std::fprintf(stderr, "[switch] AURORA_GFX_FLAGS=%s (gfx_flags.txt)\n", value);
    }
    // A launch argument wins over the file, so A/B runs need no SD card edit:
    // nxlink ... nsmbw.nro --gfx-flags=dk_copybarrier
    for (int i = 1; i < __system_argc; ++i) {
        constexpr char kPrefix[] = "--gfx-flags=";
        const char* arg = __system_argv[i];
        if (arg != nullptr && std::strncmp(arg, kPrefix, sizeof(kPrefix) - 1) == 0) {
            setenv("AURORA_GFX_FLAGS", arg + sizeof(kPrefix) - 1, 1);
            std::fprintf(stderr, "[switch] AURORA_GFX_FLAGS=%s (launch argument)\n", arg + sizeof(kPrefix) - 1);
        }
    }
    // Any other variable (NSMBW_LOG_FPS, NSMBW_WARP, NSMBW_INPUT_SCRIPT, ...): one NAME=value per
    // line in sdmc:/switch/nsmbw/env.txt; lines starting with # are skipped.
    if (FILE* env = std::fopen("sdmc:/switch/nsmbw/env.txt", "r")) {
        char line[4096];
        while (std::fgets(line, sizeof(line), env) != nullptr) {
            size_t length = std::strlen(line);
            while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r' || line[length - 1] == ' ')) {
                line[--length] = 0;
            }
            char* eq = std::strchr(line, '=');
            if (line[0] == '#' || eq == nullptr || eq == line) {
                continue;
            }
            *eq = 0;
            setenv(line, eq + 1, 1);
            std::fprintf(stderr, "[switch] %s=%s (env.txt)\n", line, eq + 1);
        }
        std::fclose(env);
    }
    // Last, so that everything above reaches the PC from the link's queue (devhost.txt).
    DevLink::Start();
}

void ShowConsoleMessage(const std::string& text) noexcept {
    consoleInit(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);
    std::printf("%s\n\nPress + to close.\n", text.c_str());
    while (appletMainLoop()) {
        padUpdate(&pad);
        if ((padGetButtonsDown(&pad) & HidNpadButton_Plus) != 0) {
            break;
        }
        consoleUpdate(nullptr);
    }
    consoleExit(nullptr);
}

void ShowErrorDialog(const std::string& shortText, const std::string& longText) noexcept {
    std::fprintf(stderr, "[switch] error: %s\n%s\n", shortText.c_str(), longText.c_str());
    ErrorApplicationConfig config;
    if (R_SUCCEEDED(errorApplicationCreate(&config, shortText.c_str(), longText.c_str()))) {
        errorApplicationShow(&config);
    }
}

[[noreturn]] void TerminateNow(int code) noexcept {
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(code);
}

} // namespace SwitchPlatform

#endif // __SWITCH__
