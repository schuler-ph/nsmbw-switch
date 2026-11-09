#pragma once

#include "../internal.hpp"
#include "../webgpu/gpu.hpp"

#include <cstring>

namespace aurora::gx::fifo {

namespace detail {
extern uint8_t* sBufferData;
extern uint32_t sBufferSize;
extern uint32_t sBufferCapacity;
extern bool sInDisplayList;
extern uint8_t* sDlBuffer;
extern uint32_t sDlSize;
extern uint32_t sDlWritePos;
// threaded(): whether the GX thread is wanted at all (NSMBW_GX_THREAD, the core count), decided
// once on the first call and read inline from then on.
extern bool sThreadWanted;
extern bool sThreadWantedKnown;
} // namespace detail

void init();

// Out-of-line slow path: grows internal buffer then appends data
void write_data_grow(const void* data, uint32_t length);

inline void write_data(const void* data, const uint32_t length) {
  if (!detail::sInDisplayList)
    LIKELY {
      if (detail::sBufferSize + length <= detail::sBufferCapacity)
        LIKELY {
          std::memcpy(detail::sBufferData + detail::sBufferSize, data, length);
          detail::sBufferSize += length;
          return;
        }
      write_data_grow(data, length);
    }
  else if (detail::sDlWritePos + length <= detail::sDlSize) {
    std::memcpy(detail::sDlBuffer + detail::sDlWritePos, data, length);
    detail::sDlWritePos += length;
  }
}

inline void write_u8(const uint8_t val) {
  if (!detail::sInDisplayList)
    LIKELY {
      if (detail::sBufferSize < detail::sBufferCapacity)
        LIKELY {
          detail::sBufferData[detail::sBufferSize++] = val;
          return;
        }
      write_data_grow(&val, 1);
    }
  else if (detail::sDlWritePos < detail::sDlSize) {
    detail::sDlBuffer[detail::sDlWritePos++] = val;
  }
}

inline void write_u16(const uint16_t val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

inline void write_u32(const uint32_t val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

inline void write_u64(const uint64_t val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

inline void write_f32(const float val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

// Display list recording
void begin_display_list(uint8_t* buf, uint32_t size);
uint32_t end_display_list();
bool in_display_list();

// Drain the internal FIFO buffer through the command processor. With the GX thread on, this is a
// full synchronization: everything queued so far has been decoded when it returns.
void drain();

// The GX thread (on with 4+ cores, on the Switch with its 3; never on OpenGL; NSMBW_GX_THREAD=0/1
// forces it): decoding (process()) runs on a worker thread, in command order, while the
// game thread carries on producing. Producers that only append commands call flush_async() where
// they used to drain; anything that reads or writes decoded state directly calls drain() (or
// sync_if_threaded()) first.
//
// Inline: every GX setter asks (GXSetVtxDesc alone made the out-of-line version, with its
// function-static guard, 1.1 % of the game thread on the Switch). Never on OpenGL (ES): its context
// belongs to one thread, so that renderer runs single-threaded. Off until the renderer is up, too;
// turning on later is safe, as nothing is left queued then.
bool thread_wanted_slow();
inline bool threaded() {
  const auto backend = webgpu::g_backendType;
  if (backend == wgpu::BackendType::Undefined || backend == wgpu::BackendType::Null ||
      backend == wgpu::BackendType::OpenGL || backend == wgpu::BackendType::OpenGLES) {
    return false;
  }
  return detail::sThreadWantedKnown ? detail::sThreadWanted : thread_wanted_slow();
}
// Hands what is buffered to the worker once enough has built up (threaded), else drains.
void flush_async();
// Waits for the worker to finish everything queued (threaded only; a no-op otherwise).
void sync_if_threaded();
// NSMBW_GX_HANDOFF=<flag>[,<flag>...] tunes the hand-off to the GX thread (one variable, as the
// Switch launcher passes at most 419 characters of arguments). Flags: kb<N> (hand-off threshold in
// KiB, default 16), copysrc (GXSetTexCopySrc without a sync), drawdone (GXDrawDone hands off and
// returns), overlap (frame worker encodes after SEALED, like NSMBW_OVERLAP_ENCODE=1).
bool handoff_option(const char* flag);
// "copysrc": GXSetTexCopySrc sets its producer-owned state without a sync.
bool copy_src_nosync();
// GXDrawDone / GXSetDrawDone: a full drain, as always, unless "drawdone" is set and the GX thread
// is on; then what is buffered is handed to the GX thread and the producer carries on. Anything
// that reads decoded state (EFB copies, the frame seal) still syncs on its own.
void draw_done();

// AuroraBeginCommandDigest: while on, process() and texture copies fold what they see into the digest.
extern bool g_commandDigestOn;
extern uint64_t g_commandDigest;
// Texture object IDs handed out inside a digest: from the same base every time, so that two runs of
// the same drawing (each initializing its texture objects afresh) digest alike.
extern uint32_t g_digestTexObjId;

// Internal buffer inspection
const uint8_t* get_buffer_data();
uint32_t get_buffer_size();
void clear_buffer();

} // namespace aurora::gx::fifo
