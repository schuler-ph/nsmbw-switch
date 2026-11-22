#!/usr/bin/env bash
# Build New Super Mario Bros. Wii as a Nintendo Switch homebrew program (.nro) with devkitPro's
# devkitA64 toolchain: CMake + Ninja with Switch.cmake, then elf2nro. The translated code in
# generated/ is the same as for the Linux build (run build-linux.sh, or its translator steps, first).
#
# Needs: devkitPro (devkitA64, libnx, deko3d, switch-mesa, switch-tools) and a Dawn + uam built for
# Switch with aurora-main/cmake/patches/dawn-nintendo-switch-platform.patch (see --dawn-src / --uam),
# from build-switch-deps.sh or as its prebuilt archive (--deps).
# The game data is never part of the package: copy your own extracted disc to sdmc:/switch/nsmbw/game/.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: build-switch.sh --dawn-src DIR --dawn-build DIR --uam LIB [--build DIR] [--jobs N] [--stage DIR]
                       [--cmake-arg ARG]...
       build-switch.sh --deps ARCHIVE|URL [--deps-sha256 HEX] [--build DIR] ...

  --deps ARCHIVE|URL  prebuilt Dawn + uam from build-switch-deps.sh --pack, a file or an http(s) URL,
                    instead of the three options below. It must match the SHA-256 pinned in this
                    script (or --deps-sha256); it is unpacked into BUILD/prebuilt-deps/ and each file
                    checked against the archive's SHA256SUMS.
  --deps-sha256 HEX accept an archive with this SHA-256 instead of the pinned one
  --dawn-src DIR    patched Dawn source tree (AURORA_SWITCH_DAWN_SOURCE_DIR)
  --dawn-build DIR  its devkitA64 build tree with src/dawn/native/libwebgpu_dawn.a
  --uam LIB         uam static library for Switch (libuam.a / libuamcore.a)
  --build DIR       CMake build directory (default ../build-wb-switch, outside the Git tree: git apply
                    and FetchContent patches misbehave inside another work tree)
  --jobs N          parallel jobs; translated shards are capped at 4 (about 1 GiB each with GCC)
  --stage DIR       also lay out an SD-card folder there: DIR/switch/nsmbw/{nsmbw.nro,dsp_coef.bin,...}
  --cmake-arg ARG   extra configure argument, repeatable (e.g. --cmake-arg -DMKW_SWITCH_PIE_OBJECTS=ON)
EOF
}

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
build=$repo/../build-wb-switch
jobs=4
dawn_src=""
dawn_build=""
uam=""
stage=""
deps=""
deps_sha256=""
cmake_args=()
# SHA-256 of the archive build-switch-deps.sh --pack makes from its pins with the toolchain of
# devkitpro/devkita64:20260219 (reproducible: the same pins give the same bytes).
pinned_deps_sha256=PINNED_DEPS_SHA256
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dawn-src) dawn_src=$2; shift 2 ;;
        --dawn-build) dawn_build=$2; shift 2 ;;
        --uam) uam=$2; shift 2 ;;
        --deps) deps=$2; shift 2 ;;
        --deps-sha256) deps_sha256=$2; shift 2 ;;
        --build) build=$2; shift 2 ;;
        --jobs) jobs=$2; shift 2 ;;
        --stage) stage=$2; shift 2 ;;
        --cmake-arg) cmake_args+=("$2"); shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) usage; echo "build-switch.sh: unknown argument: $1" >&2; exit 1 ;;
    esac
done

fail() {
    echo "build-switch.sh: error: $*" >&2
    exit 1
}

[[ -n "${DEVKITPRO:-}" ]] || fail "DEVKITPRO is not set (source /etc/profile.d/devkitpro.sh)"
if [[ -n "$deps" ]]; then
    [[ -z "$dawn_src$dawn_build$uam" ]] || fail "--deps replaces --dawn-src, --dawn-build and --uam"
    expect=${deps_sha256:-$pinned_deps_sha256}
    [[ "$expect" =~ ^[0-9a-f]{64}$ ]] || fail "no SHA-256 to check $deps against: pass --deps-sha256"
    prebuilt=$build/prebuilt-deps
    mkdir -p "$prebuilt"
    case "$deps" in
        http://*|https://*)
            file=$prebuilt/$(basename "${deps%%[?#]*}")
            if ! [[ -f "$file" && "$(sha256sum "$file" | cut -d' ' -f1)" == "$expect" ]]; then
                command -v curl >/dev/null || fail "curl not found"
                echo "Downloading $deps"
                curl -fL --retry 3 -o "$file.part" "$deps" || fail "download failed: $deps"
                mv "$file.part" "$file"
            fi ;;
        *) file=$deps; [[ -f "$file" ]] || fail "no such file: $file" ;;
    esac
    have=$(sha256sum "$file" | cut -d' ' -f1)
    [[ "$have" == "$expect" ]] ||
        fail "$file has SHA-256 $have, expected $expect (another archive needs --deps-sha256)"
    unpacked=$prebuilt/$have
    if [[ ! -f "$unpacked/.checked" ]]; then
        find "$prebuilt" -mindepth 1 -maxdepth 1 -type d -exec rm -rf {} +
        mkdir -p "$unpacked"
        tar -xJf "$file" -C "$unpacked" --no-same-owner
        (cd "$unpacked/nsmbw-switch-deps" && sha256sum -c --quiet SHA256SUMS) ||
            fail "files in $file do not match its SHA256SUMS"
        touch "$unpacked/.checked"
    fi
    root=$unpacked/nsmbw-switch-deps
    echo "Prebuilt dependencies: $file (SHA-256 $have)"
    sed -n 's/^\(dawn\|libuam\|toolchain_image\)=/  \1: /p' "$root/MANIFEST"
    dawn_src=$root/dawn
    dawn_build=$root/dawn-build-switch
    uam=$root/uam/libuam.a
fi
[[ -n "$dawn_src" && -n "$dawn_build" && -n "$uam" ]] ||
    { usage; fail "--deps, or --dawn-src, --dawn-build and --uam, are required"; }
[[ -f "$repo/generated/build_shards/shards.cmake" ]] ||
    fail "no translated code in generated/: run build-linux.sh (or its translator steps) first"

cmake -S "$repo/runtime" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DMKW_TRANSLATED_COMPILE_JOBS=4 -DNSMBW_GAME_ID=SMNE01 \
    -DAURORA_SWITCH_DAWN_SOURCE_DIR="$dawn_src" -DAURORA_SWITCH_DAWN_BUILD_DIR="$dawn_build" \
    -DAURORA_SWITCH_UAM_LIB="$uam" -DSDL_PTHREADS=ON "${cmake_args[@]}"
# SDL_PTHREADS=ON up front: the SDL patch only forces it after SDL has chosen its semaphores, so a
# first configure would get SDL's generic semaphores and only a second one the pthread ones.
cmake --build "$build" --target nsmbw_nro --parallel "$jobs"
echo "Done: $build/nsmbw.nro (symbols: $build/WiiCompiled.elf)"

if [[ -n "$stage" ]]; then
    sd=$stage/switch/nsmbw
    mkdir -p "$sd"
    cp -f "$build/nsmbw.nro" "$build/dsp_coef.bin" "$build/initial_pipeline_cache.db" "$sd/"
    cp -rf "$build/wii_bootstrap" "$sd/"
    echo "SD layout: $sd (add your extracted disc as $sd/game/)"
fi
