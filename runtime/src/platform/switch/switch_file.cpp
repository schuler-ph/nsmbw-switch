// Read-only libnx file access for the disc reads (include/platform/switch_file.h).
#if defined(__SWITCH__)

#include "platform/switch_file.h"

#include <switch.h>

#include <new>

namespace SwitchFile {

struct Handle {
    FsFile file;
};

Handle* Open(const char* path, uint64_t* size) noexcept {
    FsFileSystem* device = nullptr;
    char devicePath[FS_MAX_PATH];
    if (path == nullptr || fsdevTranslatePath(path, &device, devicePath) == -1 || device == nullptr) {
        return nullptr;
    }
    FsFile file;
    if (R_FAILED(fsFsOpenFile(device, devicePath, FsOpenMode_Read, &file))) {
        return nullptr;
    }
    s64 bytes = 0;
    if (R_FAILED(fsFileGetSize(&file, &bytes)) || bytes < 0) {
        fsFileClose(&file);
        return nullptr;
    }
    Handle* handle = new (std::nothrow) Handle{file};
    if (handle == nullptr) {
        fsFileClose(&file);
        return nullptr;
    }
    if (size != nullptr) {
        *size = static_cast<uint64_t>(bytes);
    }
    return handle;
}

bool Read(Handle* handle, uint64_t offset, void* destination, size_t length) noexcept {
    if (handle == nullptr) {
        return false;
    }
    auto* out = static_cast<uint8_t*>(destination);
    size_t done = 0;
    while (done < length) {
        u64 got = 0;
        if (R_FAILED(fsFileRead(&handle->file, static_cast<s64>(offset + done), out + done, length - done,
                                FsReadOption_None, &got)) ||
            got == 0) {
            return false;
        }
        done += static_cast<size_t>(got);
    }
    return true;
}

void Close(Handle* handle) noexcept {
    if (handle != nullptr) {
        fsFileClose(&handle->file);
        delete handle;
    }
}

} // namespace SwitchFile

#endif
