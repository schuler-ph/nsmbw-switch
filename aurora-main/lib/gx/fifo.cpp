#include "fifo.hpp"
#include "command_processor.hpp"
#include "../gfx/common.hpp"
#include "../internal.hpp"
#include "../webgpu/gpu.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "tracy/Tracy.hpp"
#include <aurora/aurora.h>

#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace aurora::gx::fifo {
static Module Log("aurora::gx::fifo");

// Cores this process may run on. libnx's std::thread::hardware_concurrency() is 0 (no
// sysconf(_SC_NPROCESSORS_ONLN)), so on the Switch the kernel's core mask for the process is
// counted instead: homebrew gets cores 0-2 (mask 0x7), core 3 stays with the system.
static unsigned usable_cores() {
  unsigned cores = std::thread::hardware_concurrency();
#if defined(__SWITCH__)
  if (cores == 0) {
    u64 mask = 0;
    cores = R_SUCCEEDED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) && mask != 0
                ? static_cast<unsigned>(__builtin_popcountll(mask))
                : 3u;
  }
#endif
  return cores;
}

namespace detail {
uint8_t* sBufferData = nullptr;
uint32_t sBufferSize = 0;
uint32_t sBufferCapacity = 0;
bool sInDisplayList = false;
uint8_t* sDlBuffer = nullptr;
uint32_t sDlSize = 0;
uint32_t sDlWritePos = 0;
bool sThreadWanted = false;
bool sThreadWantedKnown = false;
} // namespace detail

void init() {
  constexpr uint32_t initialCapacity = 64 * 1024;
  reset_cp_register_cache();
  free(detail::sBufferData);
  detail::sBufferData = static_cast<uint8_t*>(malloc(initialCapacity));
  detail::sBufferSize = 0;
  detail::sBufferCapacity = initialCapacity;
  detail::sInDisplayList = false;
  detail::sDlBuffer = nullptr;
  detail::sDlSize = 0;
  detail::sDlWritePos = 0;
}

void write_data_grow(const void* data, uint32_t length) {
  uint32_t needed = detail::sBufferSize + length;
  uint32_t newCap = std::max(detail::sBufferCapacity * 2, needed);
  detail::sBufferData = static_cast<uint8_t*>(realloc(detail::sBufferData, newCap));
  std::memcpy(detail::sBufferData + detail::sBufferSize, data, length);
  detail::sBufferSize = needed;
  detail::sBufferCapacity = newCap;
}

void begin_display_list(uint8_t* buf, uint32_t size) {
  detail::sInDisplayList = true;
  detail::sDlBuffer = buf;
  detail::sDlSize = size;
  detail::sDlWritePos = 0;
}

uint32_t end_display_list() {
  detail::sInDisplayList = false;
  uint32_t bytesWritten = detail::sDlWritePos;
  uint32_t padded = (bytesWritten + 31) & ~31u;
  while (detail::sDlWritePos < padded && detail::sDlWritePos < detail::sDlSize) {
    detail::sDlBuffer[detail::sDlWritePos++] = 0;
  }
  detail::sDlBuffer = nullptr;
  detail::sDlSize = 0;
  detail::sDlWritePos = 0;
  return padded;
}

bool in_display_list() { return detail::sInDisplayList; }

// How much of the producer's frame is spent blocked before it may decode the next batch of GX commands.
static void note_drain_wait(uint64_t nanos) noexcept {
  ZoneScopedN("FIFO drain wait");
  TracyPlot("aurora: fifoDrainWaitUs", static_cast<int64_t>(nanos / 1000));
}

// Decodes a whole command buffer, splitting the staging batch whenever it fills.
static void process_buffer(const uint8_t* data, uint32_t size) {
  // SEALED, not DONE.
  const auto waited = aurora::wait_for_frame_worker_sealed();
  if (waited.count() > 0) UNLIKELY {
    note_drain_wait(static_cast<uint64_t>(waited.count()));
  }
  uint32_t consumed = 0;
  bool retried = false;
  while (consumed < size) {
    const auto count = process(data + consumed, size - consumed, true);
    if (count == 0 && retried)
      throw gfx::StagingCapacityError("FIFO draw does not fit after capacity submission");
    consumed += count;
    if (consumed == size) break;
    // process returned with its renderer lock released. No recursive drain.
    gfx::split_staging_batch();
    retried = true;
  }
}

// NSMBW: the GX thread (see threaded()). Buffers of commands are decoded on it in the order the
// game thread queued them; the game thread waits for it only where it needs decoded state.
namespace {
// Queue a buffer once this much has built up. 16 KiB by default; the kb<N> flag picks another size
// (smaller: the GX thread gets its work earlier and the tail a drain waits for is shorter, at the
// cost of more hand-offs per frame).
uint32_t hand_off_bytes() {
  static const uint32_t bytes = [] {
    const char* list = std::getenv("NSMBW_GX_HANDOFF");
    if (list != nullptr) {
      for (const char* p = list; *p != '\0';) {
        const char* end = std::strchr(p, ',');
        const size_t len = end != nullptr ? static_cast<size_t>(end - p) : std::strlen(p);
        if (len > 2 && std::strncmp(p, "kb", 2) == 0) {
          const long kib = std::strtol(p + 2, nullptr, 10);
          if (kib >= 1 && kib <= 4096) {
            return static_cast<uint32_t>(kib) * 1024u;
          }
        }
        if (end == nullptr) break;
        p = end + 1;
      }
    }
    return 16u * 1024u;
  }();
  return bytes;
}

struct GxThread {
  std::thread thread;
  std::mutex mutex;
  std::condition_variable work;
  std::condition_variable idle;
  std::deque<std::vector<uint8_t>> jobs;
  std::vector<std::vector<uint8_t>> spare;
  bool busy = false;
};

GxThread& gx_thread() {
  static GxThread* t = [] {
    auto* state = new GxThread();
    state->thread = std::thread([state] {
      aurora_register_perf_thread("gx_fifo");
      for (;;) {
        std::vector<uint8_t> job;
        {
          std::unique_lock lock(state->mutex);
          state->work.wait(lock, [state] { return !state->jobs.empty(); });
          job = std::move(state->jobs.front());
          state->jobs.pop_front();
          state->busy = true;
        }
        process_buffer(job.data(), static_cast<uint32_t>(job.size()));
        {
          std::lock_guard lock(state->mutex);
          state->busy = false;
          job.clear();
          state->spare.push_back(std::move(job));
          if (state->jobs.empty()) {
            state->idle.notify_all();
          }
        }
      }
    });
    state->thread.detach();
    Log.info("GX thread started ({} usable cores)", usable_cores());
    return state;
  }();
  return *t;
}

void hand_off() {
  if (detail::sBufferSize == 0) {
    return;
  }
  auto& t = gx_thread();
  std::vector<uint8_t> job;
  {
    std::lock_guard lock(t.mutex);
    if (!t.spare.empty()) {
      job = std::move(t.spare.back());
      t.spare.pop_back();
    }
  }
  job.assign(detail::sBufferData, detail::sBufferData + detail::sBufferSize);
  detail::sBufferSize = 0;
  {
    std::lock_guard lock(t.mutex);
    t.jobs.push_back(std::move(job));
  }
  t.work.notify_one();
}
} // namespace

// The slow half of threaded() (fifo.hpp): on by default when there's a core to spare for it;
// NSMBW_GX_THREAD=0 or 1 forces it. Decided once; the game thread is the only caller before the
// GX thread exists, and the GX thread only exists once this said yes.
bool thread_wanted_slow() {
  bool wanted = false;
  const char* value = std::getenv("NSMBW_GX_THREAD");
  if (value != nullptr && value[0] != '\0') {
    wanted = value[0] == '1';
  } else {
#if defined(__SWITCH__)
    // Switch: on with the 3 cores homebrew gets. Measured on the hardware (A/B 2026-11-08, same
    // build and clock): title demo 29.8 -> 38.0 FPS, level 45.3 -> 46.2 FPS; the game thread
    // otherwise decodes every GX command itself. Any core besides the guest's is enough.
    wanted = usable_cores() >= 2;
#else
    wanted = usable_cores() >= 4;
#endif
  }
  detail::sThreadWanted = wanted;
  detail::sThreadWantedKnown = true;
  return wanted;
}

void flush_async() {
  if (!threaded()) {
    drain();
    return;
  }
  if (detail::sBufferSize >= hand_off_bytes()) {
    hand_off();
  }
}

bool handoff_option(const char* flag) {
  const char* list = std::getenv("NSMBW_GX_HANDOFF");
  if (list == nullptr) {
    return false;
  }
  const size_t flagLen = std::strlen(flag);
  for (const char* p = list; *p != '\0';) {
    const char* end = std::strchr(p, ',');
    const size_t len = end != nullptr ? static_cast<size_t>(end - p) : std::strlen(p);
    if (len == flagLen && std::strncmp(p, flag, len) == 0) {
      return true;
    }
    if (end == nullptr) break;
    p = end + 1;
  }
  return false;
}

bool copy_src_nosync() {
  static const bool on = handoff_option("copysrc");
  return on;
}

bool no_sealed_wait() {
  static const bool on = handoff_option("nosealed");
  return on;
}

void sync_if_threaded() {
  if (!threaded()) {
    return;
  }
  hand_off();
  auto& t = gx_thread();
  std::unique_lock lock(t.mutex);
  t.idle.wait(lock, [&t] { return t.jobs.empty() && !t.busy; });
}

void draw_done() {
  static const bool async = handoff_option("drawdone");
  if (async && threaded()) {
    hand_off();
    return;
  }
  drain();
}

void drain() {
  if (threaded()) {
    sync_if_threaded();
    return;
  }
  if (detail::sBufferSize == 0) {
    // Still waits for the frame worker to seal, as it always has.
    process_buffer(nullptr, 0);
    return;
  }
  process_buffer(detail::sBufferData, detail::sBufferSize);
  detail::sBufferSize = 0;
}

const uint8_t* get_buffer_data() { return detail::sBufferData; }
uint32_t get_buffer_size() { return detail::sBufferSize; }
void clear_buffer() {
  detail::sBufferSize = 0;
}

} // namespace aurora::gx::fifo
