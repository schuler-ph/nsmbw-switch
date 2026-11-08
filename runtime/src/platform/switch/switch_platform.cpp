// Nintendo Switch process services; see include/platform/switch_platform.h. The crash handler, the
// module-relative address helpers and the thread-stack wrap are ported from DriftDroid
// (dorPXP/driftdroid, GPL-3.0), runtime/src/main.cpp; the applet-mode check from its
// runtime/src/switch_launcher.cpp.
#if defined(__SWITCH__)

#include "platform/switch_platform.h"

#include <cerrno>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
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
    return __real___syscall_thread_create(thread, entry, arg, stackAddr, stackSize);
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
    if (__nxlink_host.s_addr != 0 && R_SUCCEEDED(socketInitializeDefault())) {
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
