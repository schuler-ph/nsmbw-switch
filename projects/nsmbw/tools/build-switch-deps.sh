#!/usr/bin/env bash
# Build the Switch graphics dependencies for build-switch.sh: libuam (GLSL -> DKSH shader compiler) and
# Dawn with its deko3d backend, as static libraries for devkitA64, into one prefix folder.
#
# Recipe after yashin-sh/WiiCompiled-Switch scripts/prepare-native-deko3d-deps.sh (GPL-3.0). The Dawn
# patch is aurora-main/cmake/patches/dawn-nintendo-switch-platform.patch (taken unchanged from
# dorPXP/driftdroid 9684e3a, GPL-3.0); the fixes in projects/nsmbw/tools/patches/ go on top of it.
# WiiCompiled-Switch's two patcher scripts are fetched at a pinned commit and checked by SHA-256.
#
# Needs: devkitPro (devkitA64, libnx, deko3d, switch-mesa, switch-tools), git, CMake, Ninja, Python 3
# with mako, meson, bison and flex. About 7 minutes plus the download with 8 jobs on a 16-thread PC, and about 5 GB of disk.
#
# Usage: build-switch-deps.sh [PREFIX]   (default: ../switch-deps next to the repository)
# Then:  build-switch.sh --dawn-src PREFIX/dawn --dawn-build PREFIX/dawn-build-switch \
#                        --uam PREFIX/uam/build-switch/libuam.a
set -euo pipefail

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
deps=${1:-$repo/../switch-deps}
jobs=${JOBS:-12}

uam_pin=f8c9eef01ffe06334d530393d636d69e2b52744b
dawn_pin=13abc3bc8ea2d3c2050f9e77a12d012108ceee24
abseil_pin=d16e32215c3ab90ba57c2e904a5344d85c7353e4
wcs_pin=b851db949cc9764c306877ed749bc70800f9658f
wcs_abseil_sha=2ab5b943bfbf128b790ad6c0ba0df1a0d28b28cf0ba1357ff6d965dc8a5feba5
wcs_switch_sha=fa30af74deb9c9df6caef5d8b69ef486822031b2ba86c630aef3e34fe8557b9c

fail() {
    echo "build-switch-deps.sh: error: $*" >&2
    exit 1
}

[[ -n "${DEVKITPRO:-}" ]] || fail "DEVKITPRO is not set (source /etc/profile.d/devkitpro.sh)"
for tool in git cmake ninja meson bison flex python3; do
    command -v "$tool" >/dev/null || fail "$tool not found (Ubuntu: apt-get install meson bison flex python3-mako)"
done
python3 -c "import mako" 2>/dev/null || fail "Python module mako not found (Ubuntu: apt-get install python3-mako)"

dawn_patch=$repo/aurora-main/cmake/patches/dawn-nintendo-switch-platform.patch
[[ -f "$dawn_patch" ]] || fail "missing $dawn_patch"

checkout() { # url dir commit
    [[ -d "$2/.git" ]] || git clone -q --filter=blob:none --no-checkout "$1" "$2"
    git -C "$2" cat-file -e "$3^{commit}" 2>/dev/null || git -C "$2" fetch -q origin "$3"
    git -C "$2" checkout -q --detach "$3"
}

mkdir -p "$deps"

echo "[1/5] patcher scripts from WiiCompiled-Switch @ $wcs_pin"
checkout https://github.com/yashin-sh/WiiCompiled-Switch.git "$deps/WiiCompiled-Switch" "$wcs_pin"
patcher=$deps/WiiCompiled-Switch/scripts
echo "$wcs_abseil_sha  $patcher/patch-dawn-abseil-switch.py" | sha256sum -c -
echo "$wcs_switch_sha  $patcher/patch-dawn-switch.py" | sha256sum -c -

echo "[2/5] libuam @ $uam_pin"
checkout https://github.com/averne/libuam.git "$deps/uam" "$uam_pin"
git -C "$deps/uam" apply --check "$repo/projects/nsmbw/tools/patches/uam-free-compiler-memory.patch" 2>/dev/null &&
    git -C "$deps/uam" apply "$repo/projects/nsmbw/tools/patches/uam-free-compiler-memory.patch"
cat > "$deps/uam-switch.cross" <<EOF
[built-in options]
buildtype = 'release'
default_library = 'static'
wrap_mode = 'nodownload'
[binaries]
c = '$DEVKITPRO/devkitA64/bin/aarch64-none-elf-gcc'
cpp = '$DEVKITPRO/devkitA64/bin/aarch64-none-elf-g++'
ar = '$DEVKITPRO/devkitA64/bin/aarch64-none-elf-ar'
strip = '$DEVKITPRO/devkitA64/bin/aarch64-none-elf-strip'
pkgconfig = '$DEVKITPRO/portlibs/switch/bin/aarch64-none-elf-pkg-config'
[host_machine]
system = 'hos'
cpu_family = 'aarch64'
cpu = 'armv8a'
endian = 'little'
EOF
set +u; source "$DEVKITPRO/switchvars.sh"; set -u
[[ -f "$deps/uam/build-switch/meson-private/coredata.dat" ]] ||
    meson setup "$deps/uam/build-switch" "$deps/uam" --cross-file "$deps/uam-switch.cross"
meson compile -C "$deps/uam/build-switch" -j "$jobs"

echo "[3/5] Dawn @ $dawn_pin + Switch patches"
# Patched once; a later run skips this step. To start over, delete $deps/dawn.
patched_mark=$deps/dawn/.switch-patches-applied
if [[ -f "$patched_mark" ]]; then
    echo "  already patched ($patched_mark)"
else
checkout https://github.com/google/dawn.git "$deps/dawn" "$dawn_pin"
git -C "$deps/dawn" -c submodule.third_party/angle.update=none -c submodule.third_party/swiftshader.update=none \
    submodule update --init --recursive --jobs "$jobs" --depth 1
[[ "$(git -C "$deps/dawn/third_party/abseil-cpp" rev-parse HEAD)" == "$abseil_pin" ]] ||
    fail "Dawn's abseil-cpp is not at $abseil_pin"
if ! python3 "$patcher/patch-dawn-switch.py" "$deps/dawn" --verify-base-patch "$dawn_patch" >/dev/null 2>&1; then
    git -C "$deps/dawn" apply --exclude=third_party/EGL-Registry/src --exclude=third_party/abseil-cpp "$dawn_patch"
fi
python3 "$patcher/patch-dawn-abseil-switch.py" "$deps/dawn/third_party/abseil-cpp"
python3 "$patcher/patch-dawn-switch.py" "$deps/dawn"
python3 "$patcher/patch-dawn-switch.py" "$deps/dawn" --check
for p in "$repo"/projects/nsmbw/tools/patches/dawn-*.patch; do
    if git -C "$deps/dawn" apply --check "$p" 2>/dev/null; then
        git -C "$deps/dawn" apply "$p"
    elif git -C "$deps/dawn" apply --check --reverse "$p" 2>/dev/null; then
        echo "  already applied: $(basename "$p")"
    else
        fail "$(basename "$p") does not apply to $deps/dawn"
    fi
done
touch "$patched_mark"
fi

echo "[4/5] configure Dawn (deko3d backend only, static monolithic library)"
cmake -S "$deps/dawn" -B "$deps/dawn-build-switch" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DDAWN_FETCH_DEPENDENCIES=OFF \
    -DDAWN_ENABLE_DEKO3D=ON -DDAWN_UAM_DIR="$deps/uam" \
    -DDAWN_ENABLE_VULKAN=OFF -DDAWN_ENABLE_NULL=OFF -DDAWN_ENABLE_OPENGLES=OFF -DDAWN_ENABLE_DESKTOP_GL=OFF \
    -DDAWN_ENABLE_METAL=OFF -DDAWN_ENABLE_D3D11=OFF -DDAWN_ENABLE_D3D12=OFF -DDAWN_ENABLE_SPIRV_VALIDATION=OFF \
    -DDAWN_USE_X11=OFF -DDAWN_USE_WAYLAND=OFF -DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_TESTS=OFF \
    -DDAWN_BUILD_NODE_BINDINGS=OFF -DDAWN_BUILD_BENCHMARKS=OFF -DDAWN_BUILD_PROTOBUF=OFF -DDAWN_USE_GLFW=OFF \
    -DTINT_BUILD_TESTS=OFF -DTINT_BUILD_CMD_TOOLS=OFF -DTINT_BUILD_WGSL_READER=ON -DTINT_BUILD_WGSL_WRITER=ON \
    -DTINT_BUILD_GLSL_WRITER=ON -DTINT_BUILD_GLSL_VALIDATOR=OFF -DTINT_BUILD_SPV_READER=OFF -DTINT_BUILD_SPV_WRITER=OFF \
    -DTINT_BUILD_HLSL_WRITER=OFF -DTINT_BUILD_MSL_WRITER=OFF -DTINT_BUILD_NULL_WRITER=OFF \
    -DDAWN_BUILD_MONOLITHIC_LIBRARY=STATIC

echo "[5/5] build Dawn"
cmake --build "$deps/dawn-build-switch" --target webgpu_dawn --parallel "$jobs"
lib=$deps/dawn-build-switch/src/dawn/native/libwebgpu_dawn.a
# grep without -q reads all of nm's output, so pipefail does not see nm killed by SIGPIPE.
aarch64-none-elf-nm -C "$lib" | grep 'dawn::native::deko3d' >/dev/null || fail "$lib has no deko3d backend"
echo "Done: $lib"
echo "      $deps/uam/build-switch/libuam.a"
