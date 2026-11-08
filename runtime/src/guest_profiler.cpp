// NSMBW_PROFILE=<start s>:<length s>: a sampling profiler for the game thread. From <start> seconds
// after the first presented frame, for <length> seconds, the thread that presents frames is sampled
// about a thousand times a second: its instruction pointer and call stack are recorded (Windows:
// suspended and unwound; Android and Linux: interrupted by SIGPROF, which walks the frame-pointer
// chain). At the end, samples are mapped back to the translated guest function whose code they fall
// in (by each translated function's host entry point) and the top entries are logged with their
// guest address, so a slow scene can be traced to the game code (or the runtime and aurora code) it
// spends time in. On Android set it with `adb shell setprop debug.nsmbw.env NSMBW_PROFILE=20:20`.
#include "guest_profiler.h"

#include "abi_bridge.h"
#include "runtime_log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <atomic>
#include <dlfcn.h>
#include <link.h>
#include <csignal>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>
#endif
#if defined(__SWITCH__)
#include <aurora/aurora.h>
#include <sys/stat.h>
#include <switch.h>
#include <cstdarg>
#include "dev_link.h"
#include "platform/switch_platform.h"
#endif

extern "C" {
extern const uint32_t kGuestMapSymbolCount;
extern const uint32_t kGuestMapSymbolAddresses[];
extern const char* const kGuestMapSymbolNames[];
}

std::filesystem::path RuntimeRunLogDirectory();  // main.cpp

namespace GuestProfiler {
namespace {

#if defined(_WIN32) || defined(__linux__)
struct Entry {
    uintptr_t host;
    uint32_t guest;
};

const char* GuestName(uint32_t address) {
    const uint32_t* begin = kGuestMapSymbolAddresses;
    const uint32_t* end = kGuestMapSymbolAddresses + kGuestMapSymbolCount;
    const uint32_t* it = std::lower_bound(begin, end, address);
    return it != end && *it == address ? kGuestMapSymbolNames[it - begin] : "";
}
#endif

// The last present, for NSMBW_PROFILE_HITCH.
std::atomic<int64_t> g_lastPresentNanos{0};
std::atomic<uint32_t> g_presentedFrames{0};
// The game's scene (vi.cpp): Linux samples carry it, Switch windows close on a change.
std::atomic<unsigned> g_scene{0xFFFFu};

int64_t NowNanos() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// NSMBW_PROFILE_HITCH=<ms>: the threshold (in ns) past which no present means a hitch, else 0.
int64_t HitchNanos() {
    static const int64_t nanos = [] {
        const char* value = std::getenv("NSMBW_PROFILE_HITCH");
        return value != nullptr ? static_cast<int64_t>(std::atof(value) * 1e6) : int64_t{0};
    }();
    return nanos;
}

bool InHitch() {
    const int64_t last = g_lastPresentNanos.load(std::memory_order_relaxed);
    return last != 0 && NowNanos() - last >= HitchNanos();
}

#ifdef _WIN32
void Report(const std::unordered_map<uintptr_t, uint32_t>& samples, const std::vector<std::vector<uintptr_t>>& stacks,
            uint32_t total) {
    // Every translated function's host entry point, sorted, to find which one a sample falls in.
    std::vector<Entry> entries;
    for (const auto& [lo, hi] : {std::pair{0x80004000u, 0x80340000u}, std::pair{0x807684C0u, 0x80B8E340u}}) {
        for (uint32_t address = lo; address < hi; address += 4) {
            if (const TranslatedFunctionInfo* info = TranslatedFunctionRegistry::FindByAddressPtr(address);
                info != nullptr && info->entryPoint != nullptr && info->address == address) {
                entries.push_back({reinterpret_cast<uintptr_t>(info->entryPoint), address});
            }
        }
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.host < b.host; });
    const auto exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exeBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(exeBase + dos->e_lfanew);
    const uintptr_t exeEnd = exeBase + nt->OptionalHeader.SizeOfImage;

    // A host address as a translated guest function, or exe+RVA (resolve with llvm-nm), or [dll].
    const auto describe = [&](uintptr_t rip) -> std::string {
        if (rip >= exeBase && rip < exeEnd) {
            auto it = std::upper_bound(entries.begin(), entries.end(), rip,
                                       [](uintptr_t value, const Entry& e) { return value < e.host; });
            // Translated code sits in its own range: past the last entry point it is runtime code.
            if (it != entries.begin() && it != entries.end()) {
                --it;
                char text[160];
                std::snprintf(text, sizeof(text), "%08X %s", it->guest, GuestName(it->guest));
                return text;
            }
            char text[40];
            std::snprintf(text, sizeof(text), "exe+%llX", static_cast<unsigned long long>(rip - exeBase));
            return text;
        }
        HMODULE module = nullptr;
        char path[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(rip), &module)) {
            GetModuleFileNameA(module, path, MAX_PATH);
        }
        const char* slash = std::strrchr(path, '\\');
        return std::string("[") + (slash ? slash + 1 : path) + "]";
    };
    // Where each sample's stack enters system code from ours: the first frames inside the exe.
    std::unordered_map<std::string, uint32_t> chains;
    for (const auto& stack : stacks) {
        std::string chain;
        int exeFrames = 0;
        for (const uintptr_t rip : stack) {
            if (rip >= exeBase && rip < exeEnd) {
                chain += (chain.empty() ? "" : " < ") + describe(rip);
                if (++exeFrames == 5) break;
            } else if (exeFrames > 0) {
                break;
            }
        }
        if (stack.empty() || (stack[0] >= exeBase && stack[0] < exeEnd)) {
            continue;  // already in our code: the leaf table covers it
        }
        chains[describe(stack[0]) + " from " + (chain.empty() ? "?" : chain)]++;
    }
    std::vector<std::pair<std::string, uint32_t>> sortedChains(chains.begin(), chains.end());
    std::sort(sortedChains.begin(), sortedChains.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < sortedChains.size() && i < 25; ++i) {
        RT_LOGF(RT_TAG_RUNTIME, "profile chain: %5.1f%%  %s\n", 100.0 * sortedChains[i].second / total,
                sortedChains[i].first.c_str());
    }

    std::unordered_map<std::string, uint32_t> byName;
    for (const auto& [rip, count] : samples) {
        std::string key;
        if (rip < exeBase || rip >= exeEnd) {
            HMODULE module = nullptr;
            char path[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(rip), &module)) {
                GetModuleFileNameA(module, path, MAX_PATH);
            }
            const char* slash = std::strrchr(path, '\\');
            key = std::string("[") + (slash ? slash + 1 : path) + "]";
        } else {
            auto it = std::upper_bound(entries.begin(), entries.end(), rip,
                                       [](uintptr_t value, const Entry& e) { return value < e.host; });
            if (it == entries.begin()) {
                key = "[exe, before translated code]";
            } else {
                --it;
                char text[160];
                std::snprintf(text, sizeof(text), "%08X %s", it->guest, GuestName(it->guest));
                key = text;
            }
        }
        byName[key] += count;
    }
    std::vector<std::pair<std::string, uint32_t>> sorted(byName.begin(), byName.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    RT_LOGF(RT_TAG_RUNTIME, "profile: %u samples, %zu translated functions\n", total, entries.size());
    for (size_t i = 0; i < sorted.size() && i < 60; ++i) {
        RT_LOGF(RT_TAG_RUNTIME, "profile: %5.1f%%  %s\n", 100.0 * sorted[i].second / total, sorted[i].first.c_str());
    }
    // Every sample inside the exe by its RVA, for naming runtime and aurora code offline (llvm-nm):
    // the table above files them under the nearest translated function.
    std::vector<std::pair<uintptr_t, uint32_t>> rvas;
    for (const auto& [rip, count] : samples) {
        if (rip >= exeBase && rip < exeEnd) {
            rvas.emplace_back(rip - exeBase, count);
        }
    }
    std::sort(rvas.begin(), rvas.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < rvas.size() && i < 1500; ++i) {
        RT_LOGF(RT_TAG_RUNTIME, "profile rva: %llX %u\n", static_cast<unsigned long long>(rvas[i].first),
                rvas[i].second);
    }
}

void Run(HANDLE thread, double startSeconds, double lengthSeconds) {
    std::this_thread::sleep_for(std::chrono::duration<double>(startSeconds));
    // NSMBW_PROFILE_HITCH=<ms>: keep only the samples taken while no frame has been presented for that
    // long, so the report shows what the game thread does during a hitch rather than on average.
    const int64_t hitchNanos = HitchNanos();
    std::unordered_map<uintptr_t, uint32_t> samples;
    std::vector<std::vector<uintptr_t>> stacks;
    uint32_t total = 0;
    // The sampler's own late wake-ups: when this thread misses its 1 ms tick by a lot too, the whole
    // process was off the CPU (another process, paging), not just the game thread.
    uint32_t lateTicks = 0;
    double lateMs = 0, worstLateMs = 0;
    auto next = std::chrono::steady_clock::now();
    const auto end = next + std::chrono::duration<double>(lengthSeconds);
    while (std::chrono::steady_clock::now() < end) {
        // About 1000 samples a second; Sleep() is too coarse for that, so wait by yielding.
        next += std::chrono::microseconds(1000);
        while (std::chrono::steady_clock::now() < next) {
            std::this_thread::yield();
        }
        const auto woke = std::chrono::steady_clock::now();
        const double late = std::chrono::duration<double, std::milli>(woke - next).count();
        if (late > 5.0) {
            ++lateTicks;
            lateMs += late;
            worstLateMs = std::max(worstLateMs, late);
            next = woke;
        }
        if (hitchNanos > 0 && !InHitch()) {
            continue;
        }
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread, &context)) {
                ++samples[static_cast<uintptr_t>(context.Rip)];
                ++total;
                // The call stack, by the unwind data every x64 function carries.
                std::vector<uintptr_t> stack;
                for (int depth = 0; depth < 24 && context.Rip != 0; ++depth) {
                    stack.push_back(static_cast<uintptr_t>(context.Rip));
                    DWORD64 imageBase = 0;
                    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                    if (function == nullptr) {
                        context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);  // a leaf: return address on top
                        context.Rsp += 8;
                        continue;
                    }
                    PVOID handlerData = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData,
                                     &establisher, nullptr);
                }
                stacks.push_back(std::move(stack));
            }
            ResumeThread(thread);
        }
    }
    RT_LOGF(RT_TAG_RUNTIME, "profile: sampler woke over 5 ms late %u times (%.0f ms in all, worst %.1f ms)%s\n",
            lateTicks, lateMs, worstLateMs, hitchNanos > 0 ? "; only samples during hitches kept" : "");
    if (total > 0) {
        Report(samples, stacks, total);
    }
}

#endif

#if defined(__linux__)
// Every translated function's host entry point, sorted.
std::vector<Entry> TranslatedEntries() {
    std::vector<Entry> entries;
    for (const auto& [lo, hi] : {std::pair{0x80004000u, 0x80340000u}, std::pair{0x807684C0u, 0x80B8E340u}}) {
        for (uint32_t address = lo; address < hi; address += 4) {
            if (const TranslatedFunctionInfo* info = TranslatedFunctionRegistry::FindByAddressPtr(address);
                info != nullptr && info->entryPoint != nullptr && info->address == address) {
                entries.push_back({reinterpret_cast<uintptr_t>(info->entryPoint), address});
            }
        }
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.host < b.host; });
    return entries;
}

// A sample: the interrupted pc, then return addresses outward.
constexpr int kDepth = 32;
struct Sample {
    uintptr_t frames[kDepth];
    int depth;
    unsigned scene;
    uint32_t frame;  // frames presented so far, to group samples by frame (profile-frames.txt)
};
Sample* g_samples = nullptr;
size_t g_capacity = 0;
std::atomic<size_t> g_count{0};
std::atomic<bool> g_sampling{false};
std::string g_stacksName = "profile-stacks.txt";

// Reads a word of this process's memory without faulting on a bad frame pointer.
bool ReadWord(uintptr_t address, uintptr_t* out) {
    iovec local{out, sizeof(*out)};
    iovec remote{reinterpret_cast<void*>(address), sizeof(*out)};
    return syscall(SYS_process_vm_readv, getpid(), &local, 1, &remote, 1, 0) == static_cast<long>(sizeof(*out));
}

void OnSample(int, siginfo_t*, void* context) {
    if (!g_sampling.load(std::memory_order_relaxed)) {
        return;
    }
    const size_t index = g_count.fetch_add(1, std::memory_order_relaxed);
    if (index >= g_capacity) {
        return;
    }
    const auto* uc = static_cast<const ucontext_t*>(context);
    Sample& sample = g_samples[index];
    sample.scene = g_scene.load(std::memory_order_relaxed);
    sample.frame = g_presentedFrames.load(std::memory_order_relaxed);
#if defined(__aarch64__)
    uintptr_t fp = uc->uc_mcontext.regs[29];
    sample.frames[0] = uc->uc_mcontext.pc;
    // The interrupted function may not have stored its return address yet (a leaf, or its
    // prologue): x30 holds it. Where it has, the frame walk repeats it, and the report drops the
    // repeat.
    sample.frames[1] = uc->uc_mcontext.regs[30];
    int depth = 2;
#elif defined(__x86_64__)
    uintptr_t fp = uc->uc_mcontext.gregs[REG_RBP];
    sample.frames[0] = uc->uc_mcontext.gregs[REG_RIP];
    int depth = 1;
#else
    uintptr_t fp = 0;
    sample.frames[0] = 0;
    int depth = 1;
#endif
    // Frame records: [fp] = the caller's fp, [fp + 8] = the return address. Each record sits above
    // the last on the same stack.
    while (depth < kDepth && fp != 0 && (fp & 7) == 0) {
        uintptr_t next = 0, ret = 0;
        if (!ReadWord(fp, &next) || !ReadWord(fp + 8, &ret) || ret == 0) {
            break;
        }
        sample.frames[depth++] = ret;
        if (next <= fp || next - fp > (1u << 20)) {
            break;
        }
        fp = next;
    }
    sample.depth = depth;
}

void ReportLinux(size_t count) {
    // This library's extent (dladdr is a linear search in bionic: far too slow per frame).
    Dl_info self{};
    dladdr(reinterpret_cast<const void*>(&ReportLinux), &self);
    const auto selfBase = reinterpret_cast<uintptr_t>(self.dli_fbase);
    const char* selfSlash = self.dli_fname ? std::strrchr(self.dli_fname, '/') : nullptr;
    const std::string selfName = selfSlash ? selfSlash + 1 : (self.dli_fname ? self.dli_fname : "?");
    struct Extent {
        uintptr_t base, end;
    } extent{selfBase, selfBase};
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* data) {
            auto* e = static_cast<Extent*>(data);
            if (info->dlpi_addr != e->base) {
                return 0;
            }
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                if (info->dlpi_phdr[i].p_type == PT_LOAD) {
                    e->end = std::max<uintptr_t>(e->end, info->dlpi_addr + info->dlpi_phdr[i].p_vaddr +
                                                             info->dlpi_phdr[i].p_memsz);
                }
            }
            return 1;
        },
        &extent);
    const auto inSelf = [&](uintptr_t address) { return address >= extent.base && address < extent.end; };
    // Other libraries' frames, as module+offset, looked up once each.
    std::unordered_map<uintptr_t, std::string> foreign;
    const auto foreignFrame = [&](uintptr_t address) -> const std::string& {
        auto [it, inserted] = foreign.try_emplace(address);
        if (inserted) {
            Dl_info info{};
            const char* module = "?";
            uintptr_t base = 0;
            if (dladdr(reinterpret_cast<const void*>(address), &info) != 0 && info.dli_fname != nullptr) {
                const char* slash = std::strrchr(info.dli_fname, '/');
                module = slash ? slash + 1 : info.dli_fname;
                base = reinterpret_cast<uintptr_t>(info.dli_fbase);
            }
            char text[160];
            std::snprintf(text, sizeof(text), "%s+%llX", module, static_cast<unsigned long long>(address - base));
            it->second = text;
        }
        return it->second;
    };

    // The stacks, for exact symbolizing offline (tools/profile-report.py, against the unstripped
    // library).
    const std::filesystem::path stacksPath = ::RuntimeRunLogDirectory() / g_stacksName;
    if (FILE* stacks = std::fopen(stacksPath.string().c_str(), "w")) {
        std::fprintf(stacks,
                     "# %s base %llX; one sample a line: S<scene>, then frames innermost first, as offsets in it "
                     "or module+offset\n",
                     selfName.c_str(), static_cast<unsigned long long>(selfBase));
        for (size_t i = 0; i < count; ++i) {
            const Sample& sample = g_samples[i];
            std::fprintf(stacks, "S%03X", sample.scene);
            for (int d = 0; d < sample.depth; ++d) {
                const uintptr_t address = sample.frames[d];
                if (inSelf(address)) {
                    std::fprintf(stacks, " %llX", static_cast<unsigned long long>(address - selfBase));
                } else {
                    std::fprintf(stacks, " %s", foreignFrame(address).c_str());
                }
            }
            std::fputc('\n', stacks);
        }
        std::fclose(stacks);
        RT_LOGF(RT_TAG_RUNTIME, "profile: %zu samples; stacks written to %s\n", count, stacksPath.string().c_str());
        // The frame each sample fell in, one a line in the same order (a sample is about 1 ms).
        std::filesystem::path framesPath = stacksPath;
        framesPath.replace_filename("profile-frames.txt");
        if (FILE* frames = std::fopen(framesPath.string().c_str(), "w")) {
            for (size_t i = 0; i < count; ++i) std::fprintf(frames, "%u\n", g_samples[i].frame);
            std::fclose(frames);
        }
    }

    // A rough self-time table on the device: by the translated function a sample stopped in (the
    // nearest entry point at or below it, so runtime code between translated functions lands on its
    // neighbour; the offline report has it exact).
    const std::vector<Entry> entries = TranslatedEntries();
    std::unordered_map<std::string, uint32_t> byFunction;
    for (size_t i = 0; i < count; ++i) {
        const uintptr_t pc = g_samples[i].frames[0];
        std::string key;
        auto it = std::upper_bound(entries.begin(), entries.end(), pc,
                                   [](uintptr_t value, const Entry& e) { return value < e.host; });
        if (inSelf(pc) && it != entries.begin() && it != entries.end()) {
            --it;
            char text[200];
            std::snprintf(text, sizeof(text), "%08X %s", it->guest, GuestName(it->guest));
            key = text;
        } else {
            key = inSelf(pc) ? selfName : foreignFrame(pc).substr(0, foreignFrame(pc).find('+'));
        }
        byFunction[key]++;
    }
    std::vector<std::pair<std::string, uint32_t>> sorted(byFunction.begin(), byFunction.end());
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < sorted.size() && i < 40; ++i) {
        RT_LOGF(RT_TAG_RUNTIME, "profile: %5.1f%%  %s\n", 100.0 * sorted[i].second / count, sorted[i].first.c_str());
    }
}

// NSMBW_PROFILE_THREAD=<name>: sample the thread of that name (as /proc lists it, e.g. FrameWorker)
// instead of the game thread.
pid_t ThreadNamed(const char* name) {
    for (const auto& task : std::filesystem::directory_iterator("/proc/self/task")) {
        if (FILE* comm = std::fopen((task.path() / "comm").string().c_str(), "r")) {
            char text[64] = {};
            const bool read = std::fgets(text, sizeof(text), comm) != nullptr;
            std::fclose(comm);
            if (read) {
                text[std::strcspn(text, "\n")] = 0;
                if (std::strcmp(text, name) == 0) {
                    return static_cast<pid_t>(std::atoi(task.path().filename().string().c_str()));
                }
            }
        }
    }
    return 0;
}

void RunLinux(pid_t thread, double startSeconds, double lengthSeconds) {
    std::this_thread::sleep_for(std::chrono::duration<double>(startSeconds));
    if (const char* name = std::getenv("NSMBW_PROFILE_THREAD"); name != nullptr && *name != 0) {
        thread = ThreadNamed(name);
        RT_LOGF(RT_TAG_RUNTIME, "profile: sampling thread %s (%d)\n", name, static_cast<int>(thread));
        if (thread == 0) {
            return;
        }
        g_stacksName = std::string("profile-stacks-") + name + ".txt";
    }
    g_capacity = static_cast<size_t>(lengthSeconds * 1100) + 1000;
    g_samples = new Sample[g_capacity];
    struct sigaction action {};
    action.sa_sigaction = OnSample;
    action.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    sigaction(SIGPROF, &action, nullptr);
    g_sampling.store(true);
    auto next = std::chrono::steady_clock::now();
    const auto end = next + std::chrono::duration<double>(lengthSeconds);
    while (next < end) {
        next += std::chrono::microseconds(1000);
        std::this_thread::sleep_until(next);
        if (HitchNanos() > 0 && !InHitch()) {
            continue;  // NSMBW_PROFILE_HITCH: only while no frame has been presented for that long
        }
        syscall(SYS_tgkill, getpid(), thread, SIGPROF);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    g_sampling.store(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ReportLinux(std::min(g_count.load(), g_capacity));
}
#endif

} // namespace

#if defined(__SWITCH__)
// Switch: Horizon has no perf and the GDB stub is far too slow to sample at any useful rate, so
// sample in-process, after DriftDroid (dorPXP/driftdroid, GPL-3.0, runtime/src/main.cpp). A thread
// pauses the game thread (svcSetThreadActivity), reads its registers (svcGetThreadContext3: real
// ARM64 PC, LR, FP, SP), walks the frame-pointer chain while it is paused, resumes it and
// histograms (PC bucket, call chain) in an open-addressed table. Helper threads (the GX thread, the
// frame worker, the presenter, the audio threads) are sampled the same way at a quarter of the rate:
// what they do decides how long the game thread waits for them. Nothing here reads a symbol table:
// offsets are module-relative and resolved offline against WiiCompiled.elf and MAP.txt
// (scripts/switch/profile-report.py), which also tells waits (PC in a libnx wait SVC or a sync
// wrapper) from work and names the function that waits.
//
// Every ~10 s (NSMBW_PROFILE_WINDOW_S), or sooner when the scene changes, the tables are written as
// one text window - "P" record over the development link, or Logs/<run>/profile/wNNN.txt on the SD
// card without one - and cleared, so one level yields a level-only profile. Off by default; with
// NSMBW_PROFILE unset no thread is created and nothing is allocated.
//
// PCs and return addresses are kept exact, not bucketed: DriftDroid bucketed to 64 bytes to fit a
// cumulative table, but libnx's SVC stubs lie 8-20 bytes apart, so a bucket mixed svcSleepThread
// with svcCreateThread and a condition-variable wait with svcConnectToNamedPort (run 1 of the
// sampler). A window holds at most as many distinct chains as it has samples (~12 500), well under
// the 65 536 slots, and the table is cleared after every window.
namespace {

constexpr int kSwitchFrames = 6;  // LR, then up to five return addresses off the FP chain
constexpr int kMaxProbes = 64;
constexpr uint64_t kModuleSpanLimit = 512ull << 20;  // a PC further than this past the base is not ours

struct StackSlot {
    uint64_t pc;                   // module-relative; hits == 0 marks an empty slot
    uint64_t frames[kSwitchFrames];
    uint64_t hits;
};
static_assert(sizeof(StackSlot) == 64, "one cache line per slot");

struct SampledThread {
    const char* group;   // aurora_register_perf_thread group ("game" is the guest thread)
    size_t slotCount;    // power of two
    Handle handle = INVALID_HANDLE;
    uint32_t handles = 0;   // threads registered under the group (only the first is sampled)
    StackSlot* slots = nullptr;
    uint64_t samples = 0, failures = 0, offModule = 0, dropped = 0;
    uint64_t pausedTicks = 0;   // system ticks the thread spent paused for us this window
    uint64_t cpuTicksStart = 0; // its CPU ticks at the window start
    Result lastError = 0;
};

SampledThread g_switchThreads[] = {
    {"game", 65536},  {"gx_fifo", 16384}, {"frame_worker", 16384},
    {"presenter", 8192}, {"audio_mix", 8192}, {"audio_out", 4096},
};
constexpr size_t kSwitchThreadCount = sizeof(g_switchThreads) / sizeof(g_switchThreads[0]);

std::atomic<Handle> g_switchGameThread{INVALID_HANDLE};
std::atomic<uint32_t> g_switchFlushRequests{0};
std::atomic<uint32_t> g_switchFlushesDone{0};
std::atomic<bool> g_switchSamplerRunning{false};

uint64_t ThreadCpuTicks(Handle handle) {
    u64 ticks = 0;
    if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, handle, TickCountInfo_Total)) &&
        R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCountDeprecated, handle, TickCountInfo_Total))) {
        return 0;
    }
    return ticks;
}

// Walks the paused thread's frame-pointer chain. Every read is bounds-checked against the one
// mapping that holds the stack pointer (the thread stack, or the heap block of a guest fiber
// stack), and frames must move strictly upward, so a garbage FP ends the walk instead of faulting.
// frames[0] is LR: a leaf (or a prologue) has not stored it yet; where it has, the walk repeats it
// and the report drops the repeat.
void WalkPausedStack(const ThreadContext& ctx, uint64_t base, uint64_t (&frames)[kSwitchFrames]) {
    const auto rel = [base](u64 addr) -> uint64_t {
        return addr >= base && addr - base < kModuleSpanLimit ? addr - base : 0;
    };
    frames[0] = rel(ctx.lr);
    for (int i = 1; i < kSwitchFrames; ++i) {
        frames[i] = 0;
    }
    MemoryInfo info{};
    u32 pageInfo = 0;
    if (R_FAILED(svcQueryMemory(&info, &pageInfo, ctx.sp)) || (info.perm & Perm_R) == 0) {
        return;
    }
    const u64 lo = info.addr;
    const u64 hi = info.addr + info.size;
    u64 fp = ctx.fp;
    for (int i = 1; i < kSwitchFrames; ++i) {
        if (fp < lo || fp + 16 > hi || (fp & 7) != 0) {
            return;
        }
        const u64* record = reinterpret_cast<const u64*>(fp);
        const u64 nextFp = record[0];
        frames[i] = rel(record[1]);
        if (nextFp <= fp) {
            return;
        }
        fp = nextFp;
    }
}

void RecordSample(SampledThread& t, uint64_t pc, const uint64_t (&frames)[kSwitchFrames]) {
    uint64_t hash = (pc | 1) * 1099511628211ull;
    for (uint64_t frame : frames) {
        hash = (hash ^ frame) * 1099511628211ull;
    }
    size_t slot = static_cast<size_t>(hash >> 20) & (t.slotCount - 1);
    for (int probe = 0; probe < kMaxProbes; ++probe) {
        StackSlot& entry = t.slots[slot];
        if (entry.hits == 0) {
            entry.pc = pc;
            std::memcpy(entry.frames, frames, sizeof(entry.frames));
            entry.hits = 1;
            return;
        }
        if (entry.pc == pc && std::memcmp(entry.frames, frames, sizeof(entry.frames)) == 0) {
            ++entry.hits;
            return;
        }
        slot = (slot + 1) & (t.slotCount - 1);
    }
    ++t.dropped;  // this chain is not counted at all, so the window header says so
}

// One sample: pause, read, walk, resume. Nothing in here may allocate or take a lock the paused
// thread could hold (malloc, the log, the link) - the walk only reads memory the kernel says is
// mapped. A few microseconds of stall per sample, which is why a profiled run presents a little
// slower than a clean one; the header reports the paused time.
void SampleThread(SampledThread& t, uint64_t base) {
    if (t.handle == INVALID_HANDLE) {
        return;
    }
    ++t.samples;
    ThreadContext ctx{};
    uint64_t frames[kSwitchFrames]{};
    const u64 started = armGetSystemTick();
    const Result pauseRc = svcSetThreadActivity(t.handle, ThreadActivity_Paused);
    // svcGetThreadContext3 only dumps registers for a thread that is paused; on a running thread it
    // fails outright.
    const Result ctxRc = R_SUCCEEDED(pauseRc) ? svcGetThreadContext3(&ctx, t.handle) : pauseRc;
    if (R_SUCCEEDED(ctxRc)) {
        WalkPausedStack(ctx, base, frames);  // the stack is only stable while the thread is paused
    }
    if (R_SUCCEEDED(pauseRc)) {
        svcSetThreadActivity(t.handle, ThreadActivity_Runnable);
    }
    t.pausedTicks += armGetSystemTick() - started;
    if (R_FAILED(ctxRc)) {
        ++t.failures;
        t.lastError = ctxRc;
        return;
    }
    const u64 pc = ctx.pc.x;
    if (pc < base || pc - base >= kModuleSpanLimit) {
        ++t.offModule;
        return;
    }
    RecordSample(t, pc - base, frames);
}

void RefreshHelperHandles() {
    for (size_t i = 1; i < kSwitchThreadCount; ++i) {
        SampledThread& t = g_switchThreads[i];
        uint32_t handles[4] = {INVALID_HANDLE, INVALID_HANDLE, INVALID_HANDLE, INVALID_HANDLE};
        t.handles = static_cast<uint32_t>(aurora_perf_thread_handles(t.group, handles, 4));
        t.handle = t.handles > 0 ? handles[0] : INVALID_HANDLE;
    }
}

void AppendLine(std::string& out, const char* format, ...) __attribute__((format(printf, 2, 3)));
void AppendLine(std::string& out, const char* format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (length > 0) {
        out.append(line, std::min<size_t>(static_cast<size_t>(length), sizeof(line) - 1));
    }
}

struct WindowState {
    uint32_t number = 0;
    std::chrono::steady_clock::time_point start{};
    uint32_t framesAtStart = 0;
    unsigned sceneAtStart = 0xFFFFu;
    uint64_t profilerCpuStart = 0;
};

void BeginWindow(WindowState& w) {
    w.start = std::chrono::steady_clock::now();
    w.framesAtStart = g_presentedFrames.load(std::memory_order_relaxed);
    w.sceneAtStart = g_scene.load(std::memory_order_relaxed);
    w.profilerCpuStart = ThreadCpuTicks(CUR_THREAD_HANDLE);
    g_switchThreads[0].handle = g_switchGameThread.load(std::memory_order_acquire);
    RefreshHelperHandles();
    for (SampledThread& t : g_switchThreads) {
        t.cpuTicksStart = t.handle != INVALID_HANDLE ? ThreadCpuTicks(t.handle) : 0;
    }
}

// Writes the window: a header, one "# thread" line per sampled thread, then "S <thread> <hits>
// <pc> <frame>..." lines (module-relative hex, exact, most hits first, trailing empty frames left
// off).
// Runs with every thread resumed, so it may allocate and use the link.
void EndWindow(WindowState& w, const char* reason) {
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - w.start).count();
    const uint32_t frames = g_presentedFrames.load(std::memory_order_relaxed) - w.framesAtStart;
    const uint64_t base = SwitchPlatform::ModuleBase();
    const double profilerCpuMs =
        static_cast<double>(armTicksToNs(ThreadCpuTicks(CUR_THREAD_HANDLE) - w.profilerCpuStart)) / 1e6;

    std::string out;
    out.reserve(256u * 1024u);
    AppendLine(out,
               "# nsmbw-profile 1 window=%u reason=%s seconds=%.2f frames=%u fps=%.2f scene=0x%X scene_start=0x%X "
               "module_base=0x%llx t_ms=%llu profiler_cpu_ms=%.1f\n",
               w.number, reason, seconds, frames, seconds > 0 ? frames / seconds : 0.0,
               g_scene.load(std::memory_order_relaxed), w.sceneAtStart, static_cast<unsigned long long>(base),
               static_cast<unsigned long long>(armTicksToNs(armGetSystemTick()) / 1000000ull), profilerCpuMs);
    std::vector<const StackSlot*> ranked;
    ranked.reserve(4096);
    for (SampledThread& t : g_switchThreads) {
        if (t.samples == 0 && t.handle == INVALID_HANDLE) {
            continue;
        }
        const double cpuMs = t.handle != INVALID_HANDLE
                                 ? static_cast<double>(armTicksToNs(ThreadCpuTicks(t.handle) - t.cpuTicksStart)) / 1e6
                                 : 0.0;
        AppendLine(out,
                   "# thread %s samples=%llu failures=%llu last_rc=0x%x off_module=%llu dropped=%llu handles=%u "
                   "cpu_ms=%.1f paused_ms=%.2f\n",
                   t.group, static_cast<unsigned long long>(t.samples), static_cast<unsigned long long>(t.failures),
                   static_cast<unsigned>(t.lastError), static_cast<unsigned long long>(t.offModule),
                   static_cast<unsigned long long>(t.dropped), t.handles, cpuMs,
                   static_cast<double>(armTicksToNs(t.pausedTicks)) / 1e6);
        ranked.clear();
        for (size_t i = 0; i < t.slotCount; ++i) {
            if (t.slots[i].hits != 0) {
                ranked.push_back(&t.slots[i]);
            }
        }
        std::sort(ranked.begin(), ranked.end(),
                  [](const StackSlot* a, const StackSlot* b) { return a->hits > b->hits; });
        for (const StackSlot* entry : ranked) {
            int depth = kSwitchFrames;
            while (depth > 0 && entry->frames[depth - 1] == 0) {
                --depth;
            }
            AppendLine(out, "S %s %llu 0x%llx", t.group, static_cast<unsigned long long>(entry->hits),
                       static_cast<unsigned long long>(entry->pc));
            for (int i = 0; i < depth; ++i) {
                AppendLine(out, " 0x%llx", static_cast<unsigned long long>(entry->frames[i]));
            }
            out.push_back('\n');
        }
        std::memset(t.slots, 0, t.slotCount * sizeof(StackSlot));
        t.samples = t.failures = t.offModule = t.dropped = t.pausedTicks = 0;
        t.lastError = 0;
    }

    RT_LOGF(RT_TAG_RUNTIME, "profile: window %u (%s): %.1f s, %u frames, %zu bytes; %s\n", w.number, reason, seconds,
            frames, out.size(), DevLink::Active() ? "sent over the dev link" : "written to Logs/<run>/profile/");
    if (DevLink::Active()) {
        DevLink::PushProfileWindow(w.number, out);
    } else {
        const std::string directory = (::RuntimeRunLogDirectory() / "profile").string();
        mkdir(directory.c_str(), 0777);
        char name[32];
        std::snprintf(name, sizeof(name), "/w%03u.txt", w.number);
        if (FILE* file = std::fopen((directory + name).c_str(), "w")) {
            std::fwrite(out.data(), 1, out.size(), file);
            std::fclose(file);
        }
    }
    ++w.number;
}

// The sampler thread: a tick a millisecond; the game thread every tick, one helper every fourth.
void SwitchSamplerMain(double startSeconds, double windowSeconds) {
    aurora_register_perf_thread("profiler");
    // Above the time-sliced helpers (0x3B) so the ticks stay regular, below the audio threads.
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2E);
    if (startSeconds > 0) {
        std::this_thread::sleep_for(std::chrono::duration<double>(startSeconds));
    }
    for (SampledThread& t : g_switchThreads) {
        t.slots = new StackSlot[t.slotCount]();
    }
    const uint64_t base = SwitchPlatform::ModuleBase();
    WindowState window;
    BeginWindow(window);
    uint32_t tick = 0;
    uint32_t flushesSeen = 0;
    for (;;) {
        svcSleepThread(1000000);  // 1 ms
        SampleThread(g_switchThreads[0], base);
        if ((tick & 3) == 0) {
            const size_t helper = 1 + ((tick >> 2) % (kSwitchThreadCount - 1));
            SampleThread(g_switchThreads[helper], base);
        }
        ++tick;
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - window.start).count();
        const uint32_t flushes = g_switchFlushRequests.load(std::memory_order_acquire);
        const bool sceneChanged = g_scene.load(std::memory_order_relaxed) != window.sceneAtStart;
        const char* reason = nullptr;
        if (flushes != flushesSeen) {
            reason = "flush";
        } else if (elapsed >= windowSeconds) {
            reason = "timer";
        } else if (sceneChanged && elapsed >= 2.0) {
            reason = "scene";
        }
        if (reason != nullptr) {
            EndWindow(window, reason);
            if (flushes != flushesSeen) {
                flushesSeen = flushes;
                g_switchFlushesDone.store(flushes, std::memory_order_release);
            }
            BeginWindow(window);
        }
    }
}

void StartSwitchSampler(const char* value) {
    double start = 0, window = 0;
    if (std::sscanf(value, "%lf:%lf", &start, &window) != 2) {
        start = 0;
        window = 0;
        if (std::strcmp(value, "1") != 0 && std::strcmp(value, "on") != 0) {
            RT_LOGF(RT_TAG_RUNTIME, "profile: NSMBW_PROFILE=%s not understood (1, or <start s>:<window s>)\n", value);
            return;
        }
    }
    if (const char* text = std::getenv("NSMBW_PROFILE_WINDOW_S"); text != nullptr && std::atof(text) > 0) {
        window = std::atof(text);
    }
    if (window <= 0) {
        window = 10.0;
    }
    // This runs on the thread that presents frames - the guest thread. Its handle is the one the
    // sampler pauses; libnx's per-thread handle is a real kernel handle usable from any thread.
    g_switchGameThread.store(threadGetCurHandle(), std::memory_order_release);
    RT_LOGF(RT_TAG_RUNTIME,
            "profile: sampling the game thread at ~1 kHz (helpers at 250 Hz) from %.0f s in %.0f s windows; module "
            "base 0x%llx; %s\n",
            start, window, static_cast<unsigned long long>(SwitchPlatform::ModuleBase()),
            DevLink::Active() ? "windows go to the dev link (P records)" : "windows go to Logs/<run>/profile/");
    g_switchSamplerRunning.store(true, std::memory_order_release);
    // Leaked, not detached: pthread_detach is ENOSYS on devkitA64 (see switch_platform.cpp).
    new std::thread(SwitchSamplerMain, start, window);
}

} // namespace
#endif // __SWITCH__

void NoteScene(unsigned scene) {
    g_scene.store(scene, std::memory_order_relaxed);
}

void FlushWindow(const char* reason) {
#if defined(__SWITCH__)
    (void)reason;
    if (!g_switchSamplerRunning.load(std::memory_order_acquire)) {
        return;
    }
    const uint32_t request = g_switchFlushRequests.fetch_add(1, std::memory_order_acq_rel) + 1;
    // The sampler closes the window within a tick or two; give it two seconds, never more.
    for (int waited = 0; waited < 200 && g_switchFlushesDone.load(std::memory_order_acquire) < request; ++waited) {
        svcSleepThread(10000000);
    }
#else
    (void)reason;
#endif
}

unsigned ProcessPageFaults() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    return K32GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) ? counters.PageFaultCount : 0;
#else
    return 0;
#endif
}

void ThreadCpuMs(double* user, double* kernel) {
    *user = *kernel = 0;
#ifdef _WIN32
    FILETIME created, exited, kernelTime, userTime;
    if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernelTime, &userTime)) {
        const auto ms = [](const FILETIME& t) {
            return ((static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime) / 1e4;
        };
        *user = ms(userTime);
        *kernel = ms(kernelTime);
    }
#elif defined(__SWITCH__)
    // Horizon counts a thread's time on all cores in system ticks (19.2 MHz) without a user/kernel
    // split; it is reported as user time. InfoType 25 since 13.0.0, the older id before that.
    u64 ticks = 0;
    if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, CUR_THREAD_HANDLE, TickCountInfo_Total)) &&
        R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCountDeprecated, CUR_THREAD_HANDLE, TickCountInfo_Total))) {
        return;
    }
    *user = static_cast<double>(armTicksToNs(ticks)) / 1e6;
#endif
}

void OnFramePresented() {
    g_lastPresentNanos.store(NowNanos(), std::memory_order_relaxed);
    g_presentedFrames.fetch_add(1, std::memory_order_relaxed);
    static bool started = false;
    if (started) {
        return;
    }
    started = true;
    const char* value = std::getenv("NSMBW_PROFILE");
    if (value == nullptr || *value == '\0') {
        return;
    }
#if defined(__SWITCH__)
    StartSwitchSampler(value);
#elif defined(_WIN32) || defined(__linux__)
    double start = 0, length = 0;
    if (std::sscanf(value, "%lf:%lf", &start, &length) != 2 || length <= 0) {
        return;
    }
#ifdef _WIN32
    HANDLE thread = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &thread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    std::thread(Run, thread, start, length).detach();
#else
    // The handler runs on whatever stack the game thread is on, a guest thread's fiber stack among
    // them: give it one of its own.
    static std::vector<char> signalStack(64 * 1024);
    stack_t alternate{};
    alternate.ss_sp = signalStack.data();
    alternate.ss_size = signalStack.size();
    sigaltstack(&alternate, nullptr);
    RT_LOGF(RT_TAG_RUNTIME, "profile: sampling the game thread from %.0f s for %.0f s\n", start, length);
    std::thread(RunLinux, static_cast<pid_t>(syscall(SYS_gettid)), start, length).detach();
#endif
#endif
}

} // namespace GuestProfiler
