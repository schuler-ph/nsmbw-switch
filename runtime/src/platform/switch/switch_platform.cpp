// Nintendo Switch process services; see include/platform/switch_platform.h. The crash handler, the
// module-relative address helpers and the thread-stack wrap are ported from DriftDroid
// (dorPXP/driftdroid, GPL-3.0), runtime/src/main.cpp; the applet-mode check from its
// runtime/src/switch_launcher.cpp.
#if defined(__SWITCH__)

#include "platform/switch_platform.h"

#include "dev_link.h"
#include "guest_flat_memory.h"

#include <aurora/aurora.h>

#include <algorithm>
#include <atomic>

#include <cerrno>
#include <chrono>
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
    aurora_register_perf_thread("audio_out");  // PerfMonitor's thread table
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

namespace {
void FlushLogTee(uint32_t timeoutMs) noexcept;  // the log tee, below
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
    // The logger thread still runs: let it write what is queued (the crash lines included), then
    // write the rest from here; after that the development link gets time to send them to the PC.
    FlushLogTee(1500);
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
//
// Nothing of that happens on the writing thread any more. Every chunk goes into a ring buffer, and
// one logger thread writes the SD files, the nxlink socket and the development link's queue from
// it, in order: an SD write costs the writer about 20 ms (9-17 % of the game thread in the played
// 1-1 of the 2026-11-08 GX-hand-off runs, with a "slow frame" line per hitch), the nxlink send is
// an IPC round trip, and DevLink::LogChunk takes a mutex the link thread also holds. Only a crash
// (and FlushLogTee's timeout) falls back to writing synchronously, so the [switch-crash] lines
// cannot be lost to a thread that no longer runs.
int g_logFile[3] = {-1, -1, -1};
int g_nxlinkSocket = -1;

// A record is a 4-byte length, a 1-byte stream (1 stdout, 2 stderr, 0 a command for the logger
// thread) and the bytes; kWrap in the length field means "continue at the start". Records are
// contiguous, so the logger thread can hand them out without reassembling them.
constexpr size_t kLogRingBytes = 256u * 1024u;
constexpr size_t kLogRecordHeader = 5;
constexpr size_t kLogMaxChunk = 4096;  // longer writes are split; newlib's unbuffered vfprintf uses <= 1 KiB
constexpr uint32_t kWrap = 0xFFFFFFFFu;
constexpr uint8_t kCmdCloseSdFiles = 0;  // the command (stream 0, length 0)

struct LogRing {
    std::mutex mutex;
    std::condition_variable work;  // a record arrived
    std::condition_variable idle;  // the ring is empty and the logger thread is not writing
    uint8_t data[kLogRingBytes];
    size_t head = 0;  // next byte the logger reads
    size_t tail = 0;  // next byte a writer fills
    size_t used = 0;  // bytes between them, wrap markers included
    bool busy = false;  // the logger thread holds a record it has not written yet
    std::atomic<bool> synchronous{false};  // crash: writers go straight to the sinks
    std::atomic<uint32_t> dropped{0};  // records that found the ring full
    std::atomic<bool> threadStarted{false};
};
LogRing g_logRing;

// Writes to the sinks themselves; the logger thread's side (and the crash handler's).
void LogSinkWrite(int stream, const char* data, size_t length) {
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
}

void LogSinkCloseSdFiles() {
    for (int stream = 1; stream <= 2; ++stream) {
        if (g_logFile[stream] >= 0) {
            close(g_logFile[stream]);
            g_logFile[stream] = -1;
        }
    }
}

// Appends one record; with the lock held. False when it does not fit (the record is dropped, the
// log keeps going).
bool LogRingPushLocked(uint8_t stream, const char* data, size_t length) {
    const size_t need = kLogRecordHeader + length;
    size_t tail = g_logRing.tail;
    size_t extra = 0;
    if (tail + need > kLogRingBytes) {
        // Not enough room before the end: mark the rest as skipped and start over.
        extra = kLogRingBytes - tail;
        if (g_logRing.used + extra + need > kLogRingBytes) {
            return false;
        }
        if (extra >= sizeof(uint32_t)) {
            const uint32_t wrap = kWrap;
            std::memcpy(g_logRing.data + tail, &wrap, sizeof(wrap));
        }  // less than a header left: the reader sees that and wraps on its own
        tail = 0;
    } else if (g_logRing.used + need > kLogRingBytes) {
        return false;
    }
    const uint32_t len = static_cast<uint32_t>(length);
    std::memcpy(g_logRing.data + tail, &len, sizeof(len));
    g_logRing.data[tail + sizeof(len)] = stream;
    if (length != 0) {
        std::memcpy(g_logRing.data + tail + kLogRecordHeader, data, length);
    }
    g_logRing.tail = tail + need;
    g_logRing.used += extra + need;
    return true;
}

// Takes the oldest record out; with the lock held. False when the ring is empty. The bytes are
// copied to `out` (at most kLogMaxChunk), so the lock is not held while the sinks write.
bool LogRingPopLocked(uint8_t& stream, char* out, size_t& length) {
    if (g_logRing.used == 0) {
        return false;
    }
    size_t head = g_logRing.head;
    if (kLogRingBytes - head < kLogRecordHeader) {
        g_logRing.used -= kLogRingBytes - head;
        head = 0;
    } else {
        uint32_t len = 0;
        std::memcpy(&len, g_logRing.data + head, sizeof(len));
        if (len == kWrap) {
            g_logRing.used -= kLogRingBytes - head;
            head = 0;
        }
    }
    uint32_t len = 0;
    std::memcpy(&len, g_logRing.data + head, sizeof(len));
    stream = g_logRing.data[head + sizeof(len)];
    length = len;
    if (length != 0) {
        std::memcpy(out, g_logRing.data + head + kLogRecordHeader, length);
    }
    g_logRing.head = head + kLogRecordHeader + length;
    g_logRing.used -= kLogRecordHeader + length;
    if (g_logRing.used == 0) {
        g_logRing.head = g_logRing.tail = 0;
    }
    return true;
}

void LogThreadMain() {
    static char buffer[kLogMaxChunk];
    for (;;) {
        uint8_t stream = 0;
        size_t length = 0;
        {
            std::unique_lock lock(g_logRing.mutex);
            g_logRing.work.wait(lock, [] { return g_logRing.used != 0; });
            LogRingPopLocked(stream, buffer, length);
            g_logRing.busy = true;
        }
        if (stream == 0) {
            if (length == 0) {
                LogSinkCloseSdFiles();
            }
        } else {
            LogSinkWrite(stream, buffer, length);
        }
        {
            std::lock_guard lock(g_logRing.mutex);
            g_logRing.busy = false;
            if (g_logRing.used == 0) {
                g_logRing.idle.notify_all();
            }
        }
    }
}

ssize_t TeeWrite(int stream, const char* data, size_t length) {
    if (g_logRing.synchronous.load(std::memory_order_acquire)) {
        LogSinkWrite(stream, data, length);
        return static_cast<ssize_t>(length);
    }
    size_t offset = 0;
    bool pushed = false;
    while (offset < length) {
        const size_t piece = std::min(length - offset, kLogMaxChunk);
        {
            std::lock_guard lock(g_logRing.mutex);
            if (LogRingPushLocked(static_cast<uint8_t>(stream), data + offset, piece)) {
                pushed = true;
            } else {
                g_logRing.dropped.fetch_add(1, std::memory_order_relaxed);
            }
        }
        offset += piece;
    }
    if (pushed) {
        g_logRing.work.notify_one();
    }
    return static_cast<ssize_t>(length);
}

// Queues a command record for the logger thread (ordered behind everything written so far).
void TeeCommand(uint8_t command) {
    std::lock_guard lock(g_logRing.mutex);
    if (command == kCmdCloseSdFiles) {
        (void)LogRingPushLocked(0, nullptr, 0);
    }
    g_logRing.work.notify_one();
}

// Waits until the logger thread has written everything queued, at most timeoutMs. Before the
// thread exists (or when it is stuck) the rest is written here, synchronously, so an exit or a
// crash loses no line. Only ever called from the thread about to end the process.
void FlushLogTee(uint32_t timeoutMs) noexcept {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    if (g_logRing.threadStarted.load(std::memory_order_acquire)) {
        std::unique_lock lock(g_logRing.mutex, std::try_to_lock);
        while (!lock.owns_lock() && std::chrono::steady_clock::now() < deadline) {
            svcSleepThread(1000000);
            lock.try_lock();
        }
        if (lock.owns_lock()) {
            g_logRing.idle.wait_until(lock, deadline, [] { return g_logRing.used == 0 && !g_logRing.busy; });
            lock.unlock();
        }
    }
    // From here on every write goes straight to the sinks, including what is still in the ring.
    g_logRing.synchronous.store(true, std::memory_order_release);
    std::unique_lock lock(g_logRing.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return;  // the logger thread holds it mid-record (crash inside it); its chunk is lost
    }
    static char buffer[kLogMaxChunk];
    uint8_t stream = 0;
    size_t length = 0;
    while (LogRingPopLocked(stream, buffer, length)) {
        if (stream == 0) {
            LogSinkCloseSdFiles();
        } else {
            LogSinkWrite(stream, buffer, length);
        }
    }
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

// Started once the guest core is known, so the thread lands on a helper core
// (SwitchSpreadHelperThread) at the lowest priority; what was written before waits in the ring.
void StartLogThread() noexcept {
    // Never joined: it runs until the process ends.
    new std::thread(LogThreadMain);
    g_logRing.threadStarted.store(true, std::memory_order_release);
}

} // namespace

namespace SwitchPlatform {

bool EnsureSockets() noexcept {
    static const bool up = R_SUCCEEDED(socketInitializeDefault());
    return up;
}

uint64_t ModuleBase() noexcept {
    return SwitchModuleBase();
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
    StartLogThread();
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
    // --env=NAME=value launch arguments (nxlink ... nsmbw.nro --env=NSMBW_PERF_OVERLAY=1) win over
    // env.txt, like --gfx-flags= over gfx_flags.txt.
    for (int i = 1; i < __system_argc; ++i) {
        constexpr char kPrefix[] = "--env=";
        const char* arg = __system_argv[i];
        if (arg == nullptr || std::strncmp(arg, kPrefix, sizeof(kPrefix) - 1) != 0) {
            continue;
        }
        const std::string item(arg + sizeof(kPrefix) - 1);
        if (const size_t eq = item.find('='); eq != std::string::npos && eq > 0) {
            setenv(item.substr(0, eq).c_str(), item.substr(eq + 1).c_str(), 1);
            std::fprintf(stderr, "[switch] %s (launch argument)\n", item.c_str());
        }
    }
    // Switch defaults (2026-11-10, reports switch-nacht1-spielthread and switch-wasser-zwei-spieler):
    // measured A/B switches that are on unless env.txt or a --env= launch argument names the
    // variable. Each stays switchable: NSMBW_GPU_TIMING=0, NSMBW_GX_HANDOFF=none, NSMBW_GXFAST=0,
    // NSMBW_FSCACHE=0, NSMBW_REVERB_MT=0 restore the old path.
    // NSMBW_WATERHLE=1 (2026-11-10, report switch-waterhle-wasserarten): the ordinary water surface
    // as one GXBegin; it only acts on that water (lava and poison keep their path). =0 turns it off.
    // NSMBW_SKIPUNREADY=all (2026-11-10, report switch-pipeskip): a draw whose pipeline is still
    // compiling is skipped in every pass, also in an EFB copy not made in the previous frame; that
    // wait hung the first visit of airship 4-37 for 1.7 s. NSMBW_SKIPUNREADY=1 restores the wait
    // there, 0 waits for every pipeline.
    {
        static constexpr const char* kDefaults[][2] = {
            {"NSMBW_GPU_TIMING", "nosnap,gpucopy"},  // no full-EFB source snapshot, GPU staging copies
            {"NSMBW_GX_HANDOFF", "overlap"},         // frame worker encodes in parallel after SEALED
            {"NSMBW_GXFAST", "1"},                   // per-draw shortcuts for raw immediate-mode draws
            {"NSMBW_FSCACHE", "1"},                  // disc read cache off the game thread
            {"NSMBW_SKIPUNREADY", "all"},            // never wait for a compiling pipeline mid-level
            {"NSMBW_REVERB_MT", "1"},                // reverb block on a worker thread
            {"NSMBW_WATERHLE", "1"},                 // water surface quads as one GXBegin
        };
        for (const auto& item : kDefaults) {
            if (std::getenv(item[0]) == nullptr) {
                setenv(item[0], item[1], 1);
                std::fprintf(stderr, "[switch] %s=%s (Switch default)\n", item[0], item[1]);
            }
        }
    }
    // Decided while the statics initialized (guest_flat_memory.cpp); said here, after the launch
    // arguments, so the log shows the switch next to its effect.
    std::fprintf(stderr, "[switch] guest memory: %s\n", GuestFlat::SwitchAccessPathNote());
    // NSMBW_LOG_SD=0: no log files on the SD card from here on (the logger thread closes them once
    // it has written everything up to here). The SD writes left the game thread with the logger
    // thread; the switch stays for runs that should not touch the card at all. The devlink harness
    // on the PC keeps the full log either way.
    if (const char* sd = std::getenv("NSMBW_LOG_SD"); sd != nullptr && sd[0] == '0') {
        static const char note[] = "[switch] NSMBW_LOG_SD=0: log continues on the devlink only\n";
        TeeWrite(1, note, sizeof(note) - 1);
        TeeWrite(2, note, sizeof(note) - 1);
        TeeCommand(kCmdCloseSdFiles);
    }
    // Last, so that everything above reaches the PC from the link's queue (devhost.txt).
    DevLink::Start();
}

[[noreturn]] void ExitProgram(int code) noexcept {
    std::fflush(stdout);
    std::fflush(stderr);
    // Always the whole process, back to the HOME menu, as HOME -> Close does. Returning to hbloader
    // (and from there to a launcher) is not safe: with the program's threads still running,
    // hbloader unmaps their code (run 17: the pipeline worker faulted inside libuam's compiler);
    // with them paused, a thread stopped inside a socket call took the system's bsdsocket service
    // down when the process ended later (run 18, error 2162-0002 in 0100000000000012).
    std::fprintf(stderr, "[switch] ending the process (code %d)\n", code);
    FlushLogTee(2000);
    svcExitProcess();
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
    ExitProgram(code);
}

} // namespace SwitchPlatform

#endif // __SWITCH__
