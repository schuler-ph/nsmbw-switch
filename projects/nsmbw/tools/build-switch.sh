#!/usr/bin/env bash
# Build New Super Mario Bros. Wii as a Nintendo Switch homebrew program (.nro) with devkitPro's
# devkitA64 toolchain: CMake + Ninja with Switch.cmake, then elf2nro. The translated code in
# generated/ is the same as for the Linux build (run build-linux.sh, or its translator steps, first).
#
# Needs: devkitPro (devkitA64, libnx, deko3d, switch-mesa, switch-tools) and a Dawn + uam built for
# Switch with aurora-main/cmake/patches/dawn-nintendo-switch-platform.patch (see --dawn / --uam).
# The game data is never part of the package: copy your own extracted disc to sdmc:/switch/nsmbw/game/.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: build-switch.sh --dawn-src DIR --dawn-build DIR --uam LIB [--build DIR] [--jobs N] [--stage DIR]

  --dawn-src DIR    patched Dawn source tree (AURORA_SWITCH_DAWN_SOURCE_DIR)
  --dawn-build DIR  its devkitA64 build tree with src/dawn/native/libwebgpu_dawn.a
  --uam LIB         uam static library for Switch (libuam.a / libuamcore.a)
  --build DIR       CMake build directory (default ../build-wb-switch, outside the Git tree: git apply
                    and FetchContent patches misbehave inside another work tree)
  --jobs N          parallel jobs; translated shards are capped at 4 (about 1 GiB each with GCC)
  --stage DIR       also lay out an SD-card folder there: DIR/switch/nsmbw/{nsmbw.nro,dsp_coef.bin,...}
EOF
}

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
build=$repo/../build-wb-switch
jobs=4
dawn_src=""
dawn_build=""
uam=""
stage=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dawn-src) dawn_src=$2; shift 2 ;;
        --dawn-build) dawn_build=$2; shift 2 ;;
        --uam) uam=$2; shift 2 ;;
        --build) build=$2; shift 2 ;;
        --jobs) jobs=$2; shift 2 ;;
        --stage) stage=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) usage; echo "build-switch.sh: unknown argument: $1" >&2; exit 1 ;;
    esac
done

fail() {
    echo "build-switch.sh: error: $*" >&2
    exit 1
}

[[ -n "${DEVKITPRO:-}" ]] || fail "DEVKITPRO is not set (source /etc/profile.d/devkitpro.sh)"
[[ -n "$dawn_src" && -n "$dawn_build" && -n "$uam" ]] || { usage; fail "--dawn-src, --dawn-build and --uam are required"; }
[[ -f "$repo/generated/build_shards/shards.cmake" ]] ||
    fail "no translated code in generated/: run build-linux.sh (or its translator steps) first"

cmake -S "$repo/runtime" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DMKW_TRANSLATED_COMPILE_JOBS=4 -DNSMBW_GAME_ID=SMNE01 \
    -DAURORA_SWITCH_DAWN_SOURCE_DIR="$dawn_src" -DAURORA_SWITCH_DAWN_BUILD_DIR="$dawn_build" \
    -DAURORA_SWITCH_UAM_LIB="$uam"
cmake --build "$build" --target nsmbw_nro --parallel "$jobs"
echo "Done: $build/nsmbw.nro (symbols: $build/WiiCompiled.elf)"

if [[ -n "$stage" ]]; then
    sd=$stage/switch/nsmbw
    mkdir -p "$sd"
    cp -f "$build/nsmbw.nro" "$build/dsp_coef.bin" "$build/initial_pipeline_cache.db" "$sd/"
    cp -rf "$build/wii_bootstrap" "$sd/"
    echo "SD layout: $sd (add your extracted disc as $sd/game/)"
fi
