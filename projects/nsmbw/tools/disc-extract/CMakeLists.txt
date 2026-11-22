# WiiDiscExtract on its own, for unpack_game.py: a host build (gcc or clang), no Switch toolchain and
# none of the runtime's dependencies besides the vendored Crypto++ (AES for the disc's encryption).
#
#   cmake -S projects/nsmbw/tools/disc-extract -B ../build-disc-extract -DCMAKE_BUILD_TYPE=Release
#   cmake --build ../build-disc-extract
#
# unpack_game.py does this itself the first time it is given a disc image (about a minute).
cmake_minimum_required(VERSION 3.20)
project(WiiDiscExtract CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(_runtime "${CMAKE_CURRENT_LIST_DIR}/../../../../runtime" ABSOLUTE)

# The same Crypto++ sources and settings as runtime/CMakeLists.txt; the linker keeps what AES needs.
file(GLOB _cryptopp_sources "${_runtime}/third_party/cryptopp/*.cpp")
list(FILTER _cryptopp_sources EXCLUDE REGEX
    "/(adhoc|bench[123]|datatest|dlltest|fipsalgt|fipstest|pch|regtest[1234]|simple|test|validat[0-9]+)\.cpp$")
list(FILTER _cryptopp_sources EXCLUDE REGEX "(_avx|_ppc|_simd|_sse)\.cpp$")
add_library(disc_cryptopp STATIC ${_cryptopp_sources})
target_include_directories(disc_cryptopp SYSTEM PUBLIC "${_runtime}/third_party")
target_compile_features(disc_cryptopp PUBLIC cxx_std_17)
target_compile_definitions(disc_cryptopp PUBLIC CRYPTOPP_DISABLE_ASM CRYPTOPP_NO_GLOBAL_BYTE)
target_compile_options(disc_cryptopp PRIVATE -w)

add_executable(WiiDiscExtract "${CMAKE_CURRENT_LIST_DIR}/../WiiDiscExtract.cpp" "${_runtime}/src/wii_disc.cpp")
target_include_directories(WiiDiscExtract PRIVATE "${_runtime}/include")
target_compile_features(WiiDiscExtract PRIVATE cxx_std_20)
target_link_libraries(WiiDiscExtract PRIVATE disc_cryptopp)
