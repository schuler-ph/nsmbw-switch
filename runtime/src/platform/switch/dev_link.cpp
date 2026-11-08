// Development link for Switch bring-up; see include/dev_link.h for what it does and the wire format.
#if defined(__SWITCH__)

#include "dev_link.h"

#include "platform/switch_platform.h"
#include "runtime_log.h"

#include <aurora/aurora.h>
#include <aurora/gfx.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <switch.h>

extern "C" void AuroraSwitchRequestExit();  // aurora-main/lib/window.cpp

namespace DevLink {
namespace {

constexpr uint16_t kDefaultPort = 28772;
constexpr const char* kHostFile = "sdmc:/switch/nsmbw/devhost.txt";
// Log lines and summaries waiting for the PC; past this the oldest go (and are counted).
constexpr size_t kQueueLimitBytes = 8u * 1024u * 1024u;
constexpr size_t kMaxLineBytes = 8192;
constexpr uint32_t kMaxScreenshotsPerRequest = 30;

enum class State : int { Undecided, On, Off };

std::atomic<State> g_state{State::Undecided};  // Undecided: buffer until Start() has read devhost.txt
std::atomic<bool> g_connected{false};
std::atomic<size_t> g_backlogBytes{0};  // queued plus not yet sent, for FlushForCrash
std::atomic<uint64_t> g_droppedLines{0};
std::atomic<uint32_t> g_screenshotId{0};
uint64_t g_startMs = 0;  // set once in Start(): tells the PC a reconnect from a new process

std::mutex g_mutex;  // guards everything below
std::deque<std::string> g_queue;   // records, oldest first
std::deque<std::string> g_images;  // screenshots, sent before g_queue, never dropped for the limit
size_t g_queuedBytes = 0;
std::string g_partial[3];  // unfinished stdout/stderr lines

// Last frame and the running one-second summary (game thread only, apart from g_lastSample's
// copy under g_mutex for "status").
FrameSample g_lastSample;
struct Window {
    uint64_t startNs = 0;
    uint32_t frames = 0, slow = 0;
    double frameSum = 0, frameMax = 0, gameSum = 0, gxSum = 0, cpuSum = 0, discSum = 0;
} g_window;

std::string g_hostText;
in_addr g_hostAddr{};
uint16_t g_hostPort = kDefaultPort;

uint64_t NowNs() {
    return armTicksToNs(armGetSystemTick());
}

// Takes g_mutex without ever waiting long: the crash handler logs through here, and the thread
// that crashed may be the one holding it.
bool LockBriefly(std::unique_lock<std::mutex>& lock) {
    for (int attempt = 0; attempt < 2000; ++attempt) {
        if (lock.try_lock()) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

void TrimQueueLocked() {
    while (g_queuedBytes > kQueueLimitBytes && !g_queue.empty()) {
        g_queuedBytes -= g_queue.front().size();
        g_backlogBytes.fetch_sub(g_queue.front().size(), std::memory_order_relaxed);
        g_queue.pop_front();
        g_droppedLines.fetch_add(1, std::memory_order_relaxed);
    }
}

void PushLocked(std::string record) {
    g_queuedBytes += record.size();
    g_backlogBytes.fetch_add(record.size(), std::memory_order_relaxed);
    g_queue.push_back(std::move(record));
    TrimQueueLocked();
}

void Push(std::string record) {
    std::unique_lock lock(g_mutex, std::defer_lock);
    if (LockBriefly(lock)) {
        PushLocked(std::move(record));
    } else {
        g_droppedLines.fetch_add(1, std::memory_order_relaxed);
    }
}

void Answer(const std::string& text) {
    Push("A " + text + "\n");
}

std::string Json(const char* format, ...) __attribute__((format(printf, 1, 2)));
std::string Json(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return std::string(buffer, length > 0 ? std::min<size_t>(static_cast<size_t>(length), sizeof(buffer) - 1) : 0);
}

std::string SampleJson(const FrameSample& s) {
    return Json("{\"frame\":%u,\"scene\":%u,\"frame_ms\":%.2f,\"game_ms\":%.2f,\"gx_ms\":%.2f,\"disc_ms\":%.2f,"
                "\"disc_reads\":%u,\"present_ms\":%.2f,\"end_frame_ms\":%.2f,\"pace_ms\":%.2f,\"cpu_ms\":%.2f,"
                "\"t_ms\":%llu}",
                s.frame, s.scene, s.frameMs, s.gameMs, s.gxMs, s.discMs, s.discReads, s.presentMs, s.endFrameMs,
                s.paceMs, s.cpuMs, static_cast<unsigned long long>(NowNs() / 1000000ull));
}

// Screenshot readback, on aurora's presentation thread while the buffer is mapped.
void OnReadback(void*, const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t bytesPerRow, bool bgra,
                const char* error) {
    if (pixels == nullptr) {
        Answer(std::string("screenshot error ") + (error != nullptr ? error : "unknown"));
        return;
    }
    if (!g_connected.load(std::memory_order_acquire)) {
        return;
    }
    const uint32_t id = g_screenshotId.fetch_add(1, std::memory_order_relaxed) + 1;
    const size_t bytes = static_cast<size_t>(width) * height * 4u;
    char header[96];
    const int headerLength = std::snprintf(header, sizeof(header), "I %u %u %u %zu\n", id, width, height, bytes);
    std::string record;
    record.reserve(static_cast<size_t>(headerLength) + bytes);
    record.append(header, static_cast<size_t>(headerLength));
    record.resize(static_cast<size_t>(headerLength) + bytes);
    auto* out = reinterpret_cast<uint8_t*>(record.data()) + headerLength;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = pixels + static_cast<size_t>(y) * bytesPerRow;
        if (!bgra) {
            std::memcpy(out, row, static_cast<size_t>(width) * 4u);
        } else {
            for (uint32_t x = 0; x < width; ++x) {
                out[x * 4 + 0] = row[x * 4 + 2];
                out[x * 4 + 1] = row[x * 4 + 1];
                out[x * 4 + 2] = row[x * 4 + 0];
                out[x * 4 + 3] = row[x * 4 + 3];
            }
        }
        out += static_cast<size_t>(width) * 4u;
    }
    std::lock_guard lock(g_mutex);
    g_backlogBytes.fetch_add(record.size(), std::memory_order_relaxed);
    g_images.push_back(std::move(record));
}

// ---- the link thread ----------------------------------------------------------------------------

int g_socket = -1;
std::string g_sendBuffer;
size_t g_sendOffset = 0;
std::string g_commandBuffer;
bool g_quitAfterFlush = false;

void Disconnect(const char* why, int error) {
    if (g_socket >= 0) {
        close(g_socket);
        g_socket = -1;
    }
    const bool wasConnected = g_connected.exchange(false, std::memory_order_acq_rel);
    g_backlogBytes.fetch_sub(g_sendBuffer.size() - g_sendOffset, std::memory_order_relaxed);
    g_sendBuffer.clear();
    g_sendOffset = 0;
    g_commandBuffer.clear();
    if (wasConnected) {
        std::fprintf(stderr, "[devlink] disconnected from %s: %s (errno %d)\n", g_hostText.c_str(), why, error);
    }
}

bool TryConnect() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(g_hostPort);
    address.sin_addr = g_hostAddr;
    if (connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 && errno != EINPROGRESS) {
        close(fd);
        return false;
    }
    pollfd waiter{fd, POLLOUT, 0};
    int error = 0;
    socklen_t errorLength = sizeof(error);
    if (poll(&waiter, 1, 2000) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &errorLength) != 0 || error != 0) {
        close(fd);
        return false;
    }
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    g_socket = fd;
    return true;
}

void SendHello() {
    u64 totalMemory = 0;
    svcGetInfo(&totalMemory, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    const std::time_t now = std::time(nullptr);
    char clock[32] = {};
    std::strftime(clock, sizeof(clock), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    uint32_t frame = 0;
    {
        std::lock_guard lock(g_mutex);
        frame = g_lastSample.frame;
    }
    const std::string hello = "H " +
        Json("{\"proto\":1,\"program\":\"nsmbw\",\"clock\":\"%s\",\"start_ms\":%llu,\"uptime_ms\":%llu,\"frame\":%u,"
             "\"memory_mib\":%llu,\"dropped\":%llu}",
             clock, static_cast<unsigned long long>(g_startMs),
             static_cast<unsigned long long>(NowNs() / 1000000ull), frame,
             static_cast<unsigned long long>(totalMemory >> 20),
             static_cast<unsigned long long>(g_droppedLines.load())) +
        "\n";
    // Ahead of the backlog, so the PC can open its run folder before the first line arrives.
    std::lock_guard lock(g_mutex);
    g_queuedBytes += hello.size();
    g_backlogBytes.fetch_add(hello.size(), std::memory_order_relaxed);
    g_queue.push_front(hello);
}

void HandleCommand(const std::string& line) {
    const size_t space = line.find(' ');
    const std::string verb = line.substr(0, space);
    const std::string rest = space == std::string::npos ? std::string() : line.substr(space + 1);
    if (verb == "ping") {
        std::lock_guard lock(g_mutex);
        PushLocked("A pong frame=" + std::to_string(g_lastSample.frame) + "\n");
    } else if (verb == "status") {
        FrameSample last;
        size_t queued = 0;
        {
            std::lock_guard lock(g_mutex);
            last = g_lastSample;
            queued = g_queuedBytes;
        }
        Answer("status " + Json("{\"last\":%s,\"pipelines_queued\":%u,\"queued_bytes\":%zu,\"dropped\":%llu}",
                                SampleJson(last).c_str(), aurora_get_queued_pipeline_count(), queued,
                                static_cast<unsigned long long>(g_droppedLines.load())));
    } else if (verb == "screenshot") {
        uint32_t count = rest.empty() ? 1u : static_cast<uint32_t>(std::strtoul(rest.c_str(), nullptr, 10));
        count = std::clamp<uint32_t>(count, 1u, kMaxScreenshotsPerRequest);
        aurora_request_frame_readback(count, OnReadback, nullptr);
        Answer("screenshot requested " + std::to_string(count));
    } else if (verb == "press") {
        const size_t split = rest.find(' ');
        const std::string buttons = rest.substr(0, split);
        const uint32_t duration =
            split == std::string::npos ? 150u : static_cast<uint32_t>(std::strtoul(rest.c_str() + split + 1, nullptr, 10));
        if (DevInput::Press(buttons.c_str(), std::clamp<uint32_t>(duration, 16u, 60000u))) {
            Answer("press " + buttons + " " + std::to_string(duration));
        } else {
            Answer("error unknown button in '" + buttons + "'");
        }
    } else if (verb == "mark") {
        std::fprintf(stderr, "[devlink] mark: %s\n", rest.c_str());
        Answer("mark");
    } else if (verb == "quit") {
        std::fprintf(stderr, "[devlink] quit requested from the PC\n");
        Answer("quit");
        g_quitAfterFlush = true;
    } else if (!verb.empty()) {
        Answer("error unknown command '" + verb + "'");
    }
}

// Moves queued records into the send buffer; images first.
void RefillSendBuffer() {
    if (g_sendOffset < g_sendBuffer.size()) {
        return;
    }
    g_sendBuffer.clear();
    g_sendOffset = 0;
    std::lock_guard lock(g_mutex);
    if (!g_images.empty()) {
        g_sendBuffer = std::move(g_images.front());
        g_images.pop_front();
        return;
    }
    while (!g_queue.empty() && g_sendBuffer.size() < 64u * 1024u) {
        g_queuedBytes -= g_queue.front().size();
        g_sendBuffer += g_queue.front();
        g_queue.pop_front();
    }
}

// One round: wait up to `timeoutMs` for the socket, read commands, send what is queued.
void ServiceSocket(int timeoutMs) {
    RefillSendBuffer();
    const bool wantWrite = g_sendOffset < g_sendBuffer.size();
    pollfd waiter{g_socket, static_cast<short>(POLLIN | (wantWrite ? POLLOUT : 0)), 0};
    const int ready = poll(&waiter, 1, wantWrite ? 0 : timeoutMs);
    if (ready < 0) {
        Disconnect("poll failed", errno);
        return;
    }
    if ((waiter.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 && (waiter.revents & POLLIN) == 0) {
        Disconnect("socket closed", 0);
        return;
    }
    if ((waiter.revents & POLLIN) != 0) {
        char buffer[1024];
        const ssize_t received = recv(g_socket, buffer, sizeof(buffer), 0);
        if (received == 0 || (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            Disconnect(received == 0 ? "closed by the PC" : "recv failed", received == 0 ? 0 : errno);
            return;
        }
        if (received > 0) {
            g_commandBuffer.append(buffer, static_cast<size_t>(received));
            size_t newline;
            while ((newline = g_commandBuffer.find('\n')) != std::string::npos) {
                std::string line = g_commandBuffer.substr(0, newline);
                g_commandBuffer.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                HandleCommand(line);
            }
            if (g_commandBuffer.size() > 4096) g_commandBuffer.clear();
        }
    }
    if (wantWrite) {
        const ssize_t sent =
            send(g_socket, g_sendBuffer.data() + g_sendOffset, g_sendBuffer.size() - g_sendOffset, 0);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                pollfd writable{g_socket, POLLOUT, 0};
                poll(&writable, 1, timeoutMs);
                return;
            }
            Disconnect("send failed", errno);
            return;
        }
        g_sendOffset += static_cast<size_t>(sent);
        g_backlogBytes.fetch_sub(static_cast<size_t>(sent), std::memory_order_relaxed);
    }
}

void LinkMain() {
    if (!SwitchPlatform::EnsureSockets()) {
        std::fprintf(stderr, "[devlink] socket service unavailable; link off\n");
        return;
    }
    bool announcedWait = false;
    for (;;) {
        if (g_socket < 0) {
            if (!TryConnect()) {
                if (!announcedWait) {
                    announcedWait = true;
                    std::fprintf(stderr, "[devlink] no harness at %s yet; retrying every 3 s\n", g_hostText.c_str());
                }
                std::this_thread::sleep_for(std::chrono::seconds(3));
                continue;
            }
            g_connected.store(true, std::memory_order_release);
            SendHello();
            std::fprintf(stderr, "[devlink] connected to %s\n", g_hostText.c_str());
        }
        ServiceSocket(20);
        if (g_quitAfterFlush) {
            // Let the answer and the last lines out, then end the program like HOME -> Close.
            const uint64_t deadline = NowNs() + 1000000000ull;
            while (g_socket >= 0 && g_backlogBytes.load() > 0 && NowNs() < deadline) {
                ServiceSocket(10);
            }
            g_quitAfterFlush = false;
            AuroraSwitchRequestExit();
        }
    }
}

bool ReadHostFile() {
    FILE* file = std::fopen(kHostFile, "r");
    if (file == nullptr) {
        return false;
    }
    char text[128] = {};
    const size_t length = std::fread(text, 1, sizeof(text) - 1, file);
    std::fclose(file);
    std::string value(text, length);
    const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!value.empty() && isSpace(value.back())) value.pop_back();
    while (!value.empty() && isSpace(value.front())) value.erase(value.begin());
    if (const size_t newline = value.find_first_of("\r\n"); newline != std::string::npos) value.resize(newline);
    std::string address = value;
    if (const size_t colon = value.find(':'); colon != std::string::npos) {
        address = value.substr(0, colon);
        const unsigned long port = std::strtoul(value.c_str() + colon + 1, nullptr, 10);
        if (port > 0 && port < 65536) g_hostPort = static_cast<uint16_t>(port);
    }
    if (inet_aton(address.c_str(), &g_hostAddr) == 0) {
        std::fprintf(stderr, "[devlink] %s: '%s' is not an IPv4 address; link off\n", kHostFile, value.c_str());
        return false;
    }
    g_hostText = address + ":" + std::to_string(g_hostPort);
    return true;
}

} // namespace

void Start() noexcept {
    static std::once_flag once;
    std::call_once(once, [] {
        g_startMs = NowNs() / 1000000ull;
        if (!ReadHostFile()) {
            g_state.store(State::Off, std::memory_order_release);
            std::lock_guard lock(g_mutex);
            g_queue.clear();
            g_queuedBytes = 0;
            g_backlogBytes.store(0);
            for (auto& partial : g_partial) partial.clear();
            return;
        }
        g_state.store(State::On, std::memory_order_release);
        std::fprintf(stderr, "[devlink] on: harness %s (devhost.txt)\n", g_hostText.c_str());
        // Never joined: it runs until the process ends. Created with std::thread so it gets the
        // 2 MiB stack floor (switch_platform.cpp) like every other runtime thread.
        new std::thread(LinkMain);
    });
}

void LogChunk(int stream, const char* data, size_t length) noexcept {
    if (stream < 1 || stream > 2 || length == 0 || g_state.load(std::memory_order_acquire) == State::Off) {
        return;
    }
    std::unique_lock lock(g_mutex, std::defer_lock);
    if (!LockBriefly(lock)) {
        g_droppedLines.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::string& partial = g_partial[stream];
    const char* prefix = stream == 1 ? "L1 " : "L2 ";
    for (size_t i = 0; i < length; ++i) {
        if (data[i] == '\n' || partial.size() >= kMaxLineBytes) {
            PushLocked(prefix + partial + "\n");
            partial.clear();
            if (data[i] == '\n') continue;
        }
        partial.push_back(data[i]);
    }
}

void OnFrame(const FrameSample& sample) noexcept {
    if (g_state.load(std::memory_order_acquire) != State::On) {
        return;
    }
    const uint64_t now = NowNs();
    Window& w = g_window;
    if (w.startNs == 0) w.startNs = now;
    ++w.frames;
    w.slow += sample.frameMs > 20.0 ? 1u : 0u;
    w.frameSum += sample.frameMs;
    w.frameMax = std::max(w.frameMax, sample.frameMs);
    w.gameSum += sample.gameMs;
    w.gxSum += sample.gxMs;
    w.cpuSum += sample.cpuMs;
    w.discSum += sample.discMs;
    std::string summary;
    if (now - w.startNs >= 1000000000ull) {
        const double seconds = static_cast<double>(now - w.startNs) / 1e9;
        const double n = w.frames;
        summary = "S " +
            Json("{\"t_ms\":%llu,\"frame\":%u,\"scene\":%u,\"fps\":%.2f,\"frame_avg\":%.2f,\"frame_max\":%.2f,"
                 "\"game_avg\":%.2f,\"gx_avg\":%.2f,\"cpu_avg\":%.2f,\"disc_ms\":%.2f,\"slow\":%u,"
                 "\"pipelines_queued\":%u,\"dropped\":%llu,\"connected\":%s}",
                 static_cast<unsigned long long>(now / 1000000ull), sample.frame, sample.scene, n / seconds,
                 w.frameSum / n, w.frameMax, w.gameSum / n, w.gxSum / n, w.cpuSum / n, w.discSum, w.slow,
                 aurora_get_queued_pipeline_count(), static_cast<unsigned long long>(g_droppedLines.load()),
                 g_connected.load() ? "true" : "false") +
            "\n";
        w = Window{};
        w.startNs = now;
    }
    const bool connected = g_connected.load(std::memory_order_acquire);
    std::string frame = connected ? "F " + SampleJson(sample) + "\n" : std::string();
    std::lock_guard lock(g_mutex);
    g_lastSample = sample;
    if (!frame.empty()) PushLocked(std::move(frame));
    if (!summary.empty()) PushLocked(std::move(summary));
}

void FlushForCrash(uint32_t timeoutMs) noexcept {
    if (g_state.load(std::memory_order_acquire) != State::On) {
        return;
    }
    const uint64_t deadline = NowNs() + static_cast<uint64_t>(timeoutMs) * 1000000ull;
    while (g_connected.load(std::memory_order_acquire) && g_backlogBytes.load(std::memory_order_relaxed) > 0 &&
           NowNs() < deadline) {
        svcSleepThread(5000000);
    }
}

} // namespace DevLink

#endif // __SWITCH__
