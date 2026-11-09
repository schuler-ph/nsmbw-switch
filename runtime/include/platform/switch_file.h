#pragma once

// Read-only files through libnx's filesystem service directly (fsFsOpenFile / fsFileRead), without
// newlib's stdio and file-descriptor layers: one IPC per read at an explicit offset, no seek, no
// size query per read. Used by the disc reads' kept-open handles (hle/storage/dvd.cpp,
// NSMBW_FSCACHE). Implemented in src/platform/switch/switch_file.cpp.
#if defined(__SWITCH__)

#include <cstddef>
#include <cstdint>

namespace SwitchFile {

struct Handle;

// Opens `path` ("sdmc:/..." or relative to the current directory) for reading; nullptr on failure.
Handle* Open(const char* path, uint64_t* size) noexcept;
// Reads exactly `length` bytes at `offset`; false on an error or a short read.
bool Read(Handle* handle, uint64_t offset, void* destination, size_t length) noexcept;
void Close(Handle* handle) noexcept;

} // namespace SwitchFile

#endif
