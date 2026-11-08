// Performance monitor; see include/perf_monitor.h.
#include "perf_monitor.h"

#include "aurora_events.h"
#include "runtime_log.h"
#include "wii_remote_input.h"

#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace PerfMonitor {
namespace {

using Clock = std::chrono::steady_clock;

// Thread groups in a fixed order (overlay rows, CSV columns on the PC).
constexpr std::array<const char*, 8> kGroups = {"game",     "gx_fifo",         "frame_worker", "presenter",
                                                "pipeline", "pipeline_writer", "audio_mix",    "devlink"};
constexpr std::array<const char*, 8> kGroupLabels = {"game", "gx fifo", "frame wrk", "presenter",
                                                     "pipeline", "pipe write", "audio mix", "devlink"};

// Frame-time histogram for the 1 % low: 0.25 ms buckets up to 250 ms, the last one open.
constexpr int kBuckets = 1000;
constexpr double kBucketMs = 0.25;

struct Histogram {
    std::vector<uint32_t> counts = std::vector<uint32_t>(kBuckets, 0);
    uint64_t total = 0;
    void Add(double ms) {
        const int bucket = std::clamp(static_cast<int>(ms / kBucketMs), 0, kBuckets - 1);
        ++counts[static_cast<size_t>(bucket)];
        ++total;
    }
    // The frame time that 99 % of frames stay under, as frames per second.
    double LowFps(double fraction = 0.01) const {
        if (total == 0) return 0;
        const uint64_t allowed = static_cast<uint64_t>(static_cast<double>(total) * fraction);
        uint64_t above = 0;
        for (int bucket = kBuckets - 1; bucket >= 0; --bucket) {
            above += counts[static_cast<size_t>(bucket)];
            if (above > allowed) return 1000.0 / ((bucket + 1) * kBucketMs);
        }
        return 0;
    }
};

struct SceneStats {
    uint64_t frames = 0;
    double frameMsSum = 0, gameMsSum = 0, gxMsSum = 0, expectedFrames = 0;
    uint64_t audioFrames = 0;
    double audioExpected = 0;
    Histogram histogram;
};

struct Snapshot {
    bool valid = false;
    uint32_t frame = 0, scene = 0;
    double seconds = 0;
    double fps = 0, low1 = 0, viHz = 0, nominalHz = 0, speedPct = 0, audioPct = 0;
    uint32_t audioRate = 0;
    uint64_t audioDropped = 0;
    double frameAvg = 0, frameMax = 0, gameAvg = 0, gxAvg = 0, presentAvg = 0, paceAvg = 0;
    uint32_t slow = 0, pipelines = 0;
    std::array<double, kGroups.size()> cpuPct{};
    std::array<double, kGroups.size()> cpuMsPerFrame{};
    std::array<uint32_t, kGroups.size()> threads{};
    uint32_t cpuMHz = 0, gpuMHz = 0, emcMHz = 0;
    int temperatureC = -1000;
    int docked = -1;
};

// Audio, from the threads that push samples.
std::atomic<uint64_t> g_audioFrames{0}, g_audioDropped{0};
std::atomic<uint32_t> g_audioRate{0};

std::atomic<bool> g_overlay{false};
std::atomic<bool> g_exitPending{false};

std::mutex g_mutex;  // guards everything below (game thread writes, overlay/devlink/summary read)
Snapshot g_snapshot;
std::map<uint32_t, SceneStats> g_scenes;

// The open one-second window (game thread only).
struct Window {
    Clock::time_point start{};
    uint32_t frames = 0, slow = 0, retraceStart = 0, frameStart = 0;
    double frameSum = 0, frameMax = 0, gameSum = 0, gxSum = 0, presentSum = 0, paceSum = 0;
    uint64_t audioStart = 0, droppedStart = 0;
    std::array<uint64_t, kGroups.size()> cpuStart{};
    Histogram histogram;
} g_window;

Clock::time_point g_firstFrame{};
uint64_t g_lastAudioFrames = 0;

void ReadThreadCpu(std::array<uint64_t, kGroups.size()>& nanos, std::array<uint32_t, kGroups.size()>& threads) {
    AuroraPerfThreadCpu groups[16];
    const size_t count = aurora_perf_thread_cpu(groups, 16);
    nanos.fill(0);
    threads.fill(0);
    for (size_t i = 0; i < count; ++i) {
        for (size_t g = 0; g < kGroups.size(); ++g) {
            if (std::strcmp(groups[i].group, kGroups[g]) == 0) {
                nanos[g] = groups[i].cpuNanos;
                threads[g] = groups[i].threads;
            }
        }
    }
}

#if defined(__SWITCH__)
// Clocks and temperature, where this process may ask (an application normally may not use clkrst;
// then they stay 0 and the overlay says so).
struct SystemSensors {
    bool tried = false, clocks = false, temperature = false;
    ClkrstSession cpu{}, gpu{}, emc{};
    void Read(Snapshot& out) {
        if (!tried) {
            tried = true;
            if (hosversionAtLeast(8, 0, 0) && R_SUCCEEDED(clkrstInitialize())) {
                clocks = R_SUCCEEDED(clkrstOpenSession(&cpu, PcvModuleId_CpuBus, 3)) &&
                         R_SUCCEEDED(clkrstOpenSession(&gpu, PcvModuleId_GPU, 3)) &&
                         R_SUCCEEDED(clkrstOpenSession(&emc, PcvModuleId_EMC, 3));
            }
            temperature = R_SUCCEEDED(tsInitialize());
            RT_LOGF(RT_TAG_RUNTIME, "perf: clock readout %s, temperature readout %s\n",
                    clocks ? "available" : "not available", temperature ? "available" : "not available");
        }
        u32 hz = 0;
        if (clocks) {
            if (R_SUCCEEDED(clkrstGetClockRate(&cpu, &hz))) out.cpuMHz = hz / 1000000u;
            if (R_SUCCEEDED(clkrstGetClockRate(&gpu, &hz))) out.gpuMHz = hz / 1000000u;
            if (R_SUCCEEDED(clkrstGetClockRate(&emc, &hz))) out.emcMHz = hz / 1000000u;
        }
        s32 celsius = 0;
        if (temperature && R_SUCCEEDED(tsGetTemperature(TsLocation_External, &celsius))) {
            out.temperatureC = celsius;
        }
        out.docked = appletGetOperationMode() == AppletOperationMode_Console ? 1 : 0;
    }
} g_sensors;
#endif

std::string SummaryJson(const Snapshot& s) {
    char buffer[2048];
    int length = std::snprintf(
        buffer, sizeof(buffer),
        "{\"t_ms\":%llu,\"frame\":%u,\"scene\":%u,\"fps\":%.2f,\"low1_fps\":%.2f,\"speed_pct\":%.1f,"
        "\"vi_hz\":%.2f,\"nominal_hz\":%.3f,\"audio_pct\":%.1f,\"audio_rate\":%u,\"audio_dropped\":%llu,"
        "\"frame_avg\":%.2f,\"frame_max\":%.2f,\"game_avg\":%.2f,\"gx_avg\":%.2f,\"present_avg\":%.2f,"
        "\"pace_avg\":%.2f,\"slow\":%u,\"pipelines_queued\":%u,\"cpu_mhz\":%u,\"gpu_mhz\":%u,\"emc_mhz\":%u,"
        "\"temp_c\":%d,\"docked\":%d",
        static_cast<unsigned long long>(s.seconds * 1000.0), s.frame, s.scene, s.fps, s.low1, s.speedPct, s.viHz,
        s.nominalHz, s.audioPct, s.audioRate, static_cast<unsigned long long>(s.audioDropped), s.frameAvg, s.frameMax,
        s.gameAvg, s.gxAvg, s.presentAvg, s.paceAvg, s.slow, s.pipelines, s.cpuMHz, s.gpuMHz, s.emcMHz,
        s.temperatureC, s.docked);
    std::string json(buffer, static_cast<size_t>(std::max(0, std::min<int>(length, sizeof(buffer) - 1))));
    for (size_t g = 0; g < kGroups.size(); ++g) {
        length = std::snprintf(buffer, sizeof(buffer), ",\"cpu_%s\":%.1f,\"ms_%s\":%.2f,\"threads_%s\":%u", kGroups[g],
                               s.cpuPct[g], kGroups[g], s.cpuMsPerFrame[g], kGroups[g], s.threads[g]);
        json.append(buffer, static_cast<size_t>(std::max(0, std::min<int>(length, sizeof(buffer) - 1))));
    }
    json += "}";
    return json;
}

void CloseWindow(Clock::time_point now, const DevLink::FrameSample& last) {
    Window& w = g_window;
    const double seconds = std::chrono::duration<double>(now - w.start).count();
    if (seconds <= 0 || w.frames == 0) return;
    Snapshot s;
    s.valid = true;
    s.frame = last.frame;
    s.scene = last.scene;
    s.seconds = std::chrono::duration<double>(now - g_firstFrame).count();
    const double n = w.frames;
    s.fps = n / seconds;
    s.low1 = w.histogram.LowFps();
    s.viHz = (last.retraceCount - w.retraceStart) / seconds;
    s.nominalHz = last.viIntervalNs > 0 ? 1e9 / static_cast<double>(last.viIntervalNs) : 60000.0 / 1001.0;
    s.speedPct = s.fps / s.nominalHz * 100.0;
    s.audioRate = g_audioRate.load(std::memory_order_relaxed);
    const uint64_t audioFrames = g_audioFrames.load(std::memory_order_relaxed);
    s.audioPct = s.audioRate > 0 ? (audioFrames - w.audioStart) / seconds / s.audioRate * 100.0 : 0;
    s.audioDropped = g_audioDropped.load(std::memory_order_relaxed) - w.droppedStart;
    s.frameAvg = w.frameSum / n;
    s.frameMax = w.frameMax;
    s.gameAvg = w.gameSum / n;
    s.gxAvg = w.gxSum / n;
    s.presentAvg = w.presentSum / n;
    s.paceAvg = w.paceSum / n;
    s.slow = w.slow;
    s.pipelines = aurora_get_queued_pipeline_count();
    std::array<uint64_t, kGroups.size()> cpu{};
    ReadThreadCpu(cpu, s.threads);
    for (size_t g = 0; g < kGroups.size(); ++g) {
        const double ms = cpu[g] >= w.cpuStart[g] ? (cpu[g] - w.cpuStart[g]) / 1e6 : 0;
        s.cpuPct[g] = ms / (seconds * 1000.0) * 100.0;
        s.cpuMsPerFrame[g] = ms / n;
    }
#if defined(__SWITCH__)
    g_sensors.Read(s);
#endif
    DevLink::PushSummary(SummaryJson(s));
    {
        std::lock_guard lock(g_mutex);
        g_snapshot = s;
    }
    // The next window starts here.
    w = Window{};
    w.start = now;
    w.retraceStart = last.retraceCount;
    w.audioStart = audioFrames;
    w.droppedStart = g_audioDropped.load(std::memory_order_relaxed);
    w.cpuStart = cpu;
}

double EnvNumber(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? std::strtod(value, nullptr) : 0.0;
}

void CheckExitLimits(Clock::time_point now) {
    if (g_exitPending.load(std::memory_order_acquire)) return;
    static const double exitAfterS = EnvNumber("NSMBW_EXIT_AFTER_S");
    static const double exitAfterScriptMs = EnvNumber("NSMBW_EXIT_AFTER_SCRIPT");
    if (exitAfterS > 0 && std::chrono::duration<double>(now - g_firstFrame).count() >= exitAfterS) {
        RequestExit("NSMBW_EXIT_AFTER_S reached");
    } else if (exitAfterScriptMs > 0 && WiiRemoteInput::ScriptFinished(static_cast<uint64_t>(exitAfterScriptMs))) {
        RequestExit("input script finished (NSMBW_EXIT_AFTER_SCRIPT)");
    }
}

} // namespace

void OnFrame(const DevLink::FrameSample& sample) noexcept {
    DevLink::OnFrame(sample);
    const Clock::time_point now = Clock::now();
    if (g_firstFrame == Clock::time_point{}) {
        g_firstFrame = now;
        aurora_register_perf_thread("game");
        if (const char* overlay = std::getenv("NSMBW_PERF_OVERLAY"); overlay != nullptr && *overlay == '1') {
            g_overlay.store(true, std::memory_order_relaxed);
        }
        g_window.start = now;
        g_window.retraceStart = sample.retraceCount;
        g_window.audioStart = g_audioFrames.load(std::memory_order_relaxed);
        std::array<uint32_t, kGroups.size()> threads{};
        ReadThreadCpu(g_window.cpuStart, threads);
        g_lastAudioFrames = g_window.audioStart;
    }
    Window& w = g_window;
    ++w.frames;
    w.slow += sample.frameMs > 20.0 ? 1u : 0u;
    w.frameSum += sample.frameMs;
    w.frameMax = std::max(w.frameMax, sample.frameMs);
    w.gameSum += sample.gameMs;
    w.gxSum += sample.gxMs;
    w.presentSum += sample.presentMs;
    w.paceSum += sample.paceMs;
    w.histogram.Add(sample.frameMs);
    {
        // Per scene: the frame counts against the frames the VI would have shown in its time.
        const double nominalHz = sample.viIntervalNs > 0 ? 1e9 / static_cast<double>(sample.viIntervalNs) : 59.94;
        const uint64_t audioFrames = g_audioFrames.load(std::memory_order_relaxed);
        const uint32_t rate = g_audioRate.load(std::memory_order_relaxed);
        std::lock_guard lock(g_mutex);
        SceneStats& scene = g_scenes[sample.scene];
        ++scene.frames;
        scene.frameMsSum += sample.frameMs;
        scene.gameMsSum += sample.gameMs;
        scene.gxMsSum += sample.gxMs;
        scene.expectedFrames += sample.frameMs / 1000.0 * nominalHz;
        scene.audioFrames += audioFrames - g_lastAudioFrames;
        scene.audioExpected += sample.frameMs / 1000.0 * rate;
        scene.histogram.Add(sample.frameMs);
        g_lastAudioFrames = audioFrames;
    }
    if (now - w.start >= std::chrono::seconds(1)) {
        CloseWindow(now, sample);
    }
    CheckExitLimits(now);
#if !defined(__SWITCH__)
    if (g_exitPending.load(std::memory_order_acquire)) {
        ExitForAuroraWindowClose();
    }
#endif
}

void NoteAudio(uint32_t frames, uint32_t rate, bool queued) noexcept {
    g_audioFrames.fetch_add(frames, std::memory_order_relaxed);
    g_audioRate.store(rate, std::memory_order_relaxed);
    if (!queued) g_audioDropped.fetch_add(frames, std::memory_order_relaxed);
}

void SetOverlay(bool on) noexcept {
    g_overlay.store(on, std::memory_order_relaxed);
}

bool Overlay() noexcept {
    return g_overlay.load(std::memory_order_relaxed);
}

void DrawOverlay() noexcept {
    if (!g_overlay.load(std::memory_order_relaxed)) return;
    Snapshot s;
    {
        std::lock_guard lock(g_mutex);
        s = g_snapshot;
    }
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(10.0f, io.DisplaySize.y - 10.0f), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
                                        ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("Perf Monitor", nullptr, kFlags)) {
        ImGui::SetWindowFontScale(0.85f);
        if (!s.valid) {
            ImGui::TextUnformatted("perf: waiting for the first second");
        } else {
            const ImVec4 speedColor = s.speedPct >= 99.0   ? ImVec4(0.4f, 0.9f, 0.4f, 1.0f)
                                      : s.speedPct >= 90.0 ? ImVec4(0.95f, 0.8f, 0.3f, 1.0f)
                                                           : ImVec4(0.95f, 0.35f, 0.3f, 1.0f);
            ImGui::TextColored(speedColor, "FPS %.1f  speed %.1f%%  audio %.0f%%", s.fps, s.speedPct, s.audioPct);
            ImGui::Text("1%% low %.1f  VI %.1f/%.1f Hz  scene 0x%03X", s.low1, s.viHz, s.nominalHz, s.scene);
            ImGui::Text("frame %.1f avg %.1f max ms, %u slow", s.frameAvg, s.frameMax, s.slow);
            ImGui::Text("game %.1f (gx %.1f)  present %.1f  pace %.1f ms", s.gameAvg, s.gxAvg, s.presentAvg,
                        s.paceAvg);
            ImGui::Separator();
            ImGui::TextDisabled("thread       ms/frame  %%core");
            for (size_t g = 0; g < kGroups.size(); ++g) {
                if (s.threads[g] == 0) continue;
                if (s.threads[g] > 1) {
                    ImGui::Text("%-10s %6.2f %6.0f  (%u)", kGroupLabels[g], s.cpuMsPerFrame[g], s.cpuPct[g],
                                s.threads[g]);
                } else {
                    ImGui::Text("%-10s %6.2f %6.0f", kGroupLabels[g], s.cpuMsPerFrame[g], s.cpuPct[g]);
                }
            }
            ImGui::Separator();
            if (s.cpuMHz != 0) {
                ImGui::Text("CPU %u  GPU %u  EMC %u MHz", s.cpuMHz, s.gpuMHz, s.emcMHz);
            } else {
                ImGui::TextDisabled("clocks: not readable");
            }
            if (s.temperatureC > -1000) {
                ImGui::Text("SoC %d C  %s", s.temperatureC, s.docked == 1 ? "docked" : s.docked == 0 ? "handheld" : "");
            }
            if (s.pipelines > 0) ImGui::Text("shaders queued %u", s.pipelines);
            if (s.audioDropped > 0) ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.3f, 1.0f), "audio dropped %llu",
                                                       static_cast<unsigned long long>(s.audioDropped));
        }
        ImGui::SetWindowFontScale(1.0f);
    }
    ImGui::End();
}

void LogRunSummary(const char* reason) noexcept {
    std::lock_guard lock(g_mutex);
    RT_LOGF(RT_TAG_RUNTIME, "perf: run summary (%s); fps = presented game frames per second, speed = frames / "
            "frames the VI shows in that time, low1 = 1000 / 99th-percentile frame time\n", reason);
    for (const auto& [scene, stats] : g_scenes) {
        if (stats.frames == 0) continue;
        const double seconds = stats.frameMsSum / 1000.0;
        RT_LOGF(RT_TAG_RUNTIME,
                "perf: scene 0x%03X: %llu frames in %.1f s, fps %.2f, low1 %.2f, speed %.1f%%, audio %.1f%%, "
                "frame %.2f ms (game %.2f, gx %.2f)\n",
                scene, static_cast<unsigned long long>(stats.frames), seconds,
                seconds > 0 ? stats.frames / seconds : 0.0, stats.histogram.LowFps(),
                stats.expectedFrames > 0 ? stats.frames / stats.expectedFrames * 100.0 : 0.0,
                stats.audioExpected > 0 ? stats.audioFrames / stats.audioExpected * 100.0 : 0.0,
                stats.frameMsSum / stats.frames, stats.gameMsSum / stats.frames, stats.gxMsSum / stats.frames);
    }
}

void RequestExit(const char* reason) noexcept {
    if (g_exitPending.exchange(true, std::memory_order_acq_rel)) return;
    RT_LOGF(RT_TAG_RUNTIME, "perf: ending the program: %s\n", reason);
    LogRunSummary(reason);
#if defined(__SWITCH__)
    // Lets the dev link send what is queued, then ends like HOME -> Close (and returns to the
    // launcher when there is one, SwitchPlatform::ChainToLauncher).
    DevLink::RequestQuit();
#endif
}

} // namespace PerfMonitor
