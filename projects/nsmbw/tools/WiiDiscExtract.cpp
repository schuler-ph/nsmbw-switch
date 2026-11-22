// Extracts a Wii disc image (.iso, or .wbfs also split into .wbf1, .wbf2...) the way Dolphin's Extract
// Entire Disc does: the game partition's sys/ and files/. It uses WiiDisc (runtime/src/wii_disc.cpp),
// the code behind the game's own Open ISO. unpack_game.py runs it when it is given a disc image; it is
// built on its own with projects/nsmbw/tools/disc-extract/CMakeLists.txt (no Switch toolchain).
//
//   WiiDiscExtract --info <image>            prints "id=SMNE01 revision=1 bytes=<extracted size>"
//   WiiDiscExtract <image> <output folder>   extracts into the folder (sys/ and files/)
//
// Compressed images (.rvz, .wia, .gcz, .ciso, NKit) are refused with a sentence saying so; unpack_game.py
// hands those to nodtool instead.
#include "wii_disc.h"

#include <chrono>
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    const bool info = argc == 3 && std::strcmp(argv[1], "--info") == 0;
    if (argc != 3) {
        std::fprintf(stderr, "usage: WiiDiscExtract <image> <output folder>\n"
                             "       WiiDiscExtract --info <image>\n");
        return 2;
    }
    const char* image = info ? argv[2] : argv[1];
    std::string error;
    const auto disc = WiiDisc::Disc::Open(image, error);
    if (!disc) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    if (info) {
        std::printf("id=%s revision=%d bytes=%llu\n", disc->Id().c_str(), disc->Revision(),
                    static_cast<unsigned long long>(disc->ExtractedSize()));
        return 0;
    }
    std::printf("%s revision %d, %llu bytes to write\n", disc->Id().c_str(), disc->Revision(),
                static_cast<unsigned long long>(disc->ExtractedSize()));
    const std::atomic<bool> cancel{false};
    int lastTenth = -1;
    int files = 0;
    const auto start = std::chrono::steady_clock::now();
    error = disc->Extract(argv[2], [&](uint64_t done, uint64_t total) {
        const int tenth = int(done * 10 / (total ? total : 1));
        if (tenth != lastTenth) {
            lastTenth = tenth;
            std::printf("  %d%%\n", tenth * 10);
            std::fflush(stdout);
        }
    }, cancel, files);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (!error.empty()) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    std::printf("%d files in %.1f s\n", files, seconds);
    return 0;
}
