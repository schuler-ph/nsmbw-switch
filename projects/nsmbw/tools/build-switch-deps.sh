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
# Usage: build-switch-deps.sh [PREFIX] [--pack ARCHIVE.tar.xz]   (default PREFIX: ../switch-deps)
# Then:  build-switch.sh --dawn-src PREFIX/dawn --dawn-build PREFIX/dawn-build-switch \
#                        --uam PREFIX/uam/build-switch/libuam.a
# or, after --pack:  build-switch.sh --deps ARCHIVE.tar.xz
#
# --pack also writes a reproducible archive of what build-switch.sh needs: both static libraries
# without debug info, Dawn's public and generated headers, the license texts, the patches and patcher
# scripts that were applied, a MANIFEST and SHA256SUMS; ARCHIVE.sha256 goes beside it. Source paths
# are compiled in as relative ones (-ffile-prefix-map), so the same pins and toolchain give the same
# bytes in any PREFIX. Packing checks for the toolchain of devkitpro/devkita64:20260219.
set -euo pipefail

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
deps=""
pack=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pack) pack=$2; shift 2 ;;
        -h|--help) sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
        -*) echo "build-switch-deps.sh: unknown argument: $1" >&2; exit 1 ;;
        *) [[ -z "$deps" ]] || { echo "build-switch-deps.sh: more than one PREFIX" >&2; exit 1; }
           deps=$1; shift ;;
    esac
done
deps=${deps:-$repo/../switch-deps}
jobs=${JOBS:-12}
# The toolchain a published archive is built with: the packages of devkitpro/devkita64:20260219.
toolchain_image=devkitpro/devkita64:20260219
toolchain_pkgs=("deko3d 0.5.0-1" "devkitA64 r29.2-1" "devkita64-cmake 1.1.3-1" "libnx 4.12.0-1" "switch-cmake 1.5.2-1")

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
# Canonical, so that -ffile-prefix-map below matches the paths the compiler sees.
deps=$(cd "$deps" && pwd -P)
if [[ -n "$pack" ]]; then
    command -v xz >/dev/null || fail "--pack needs xz"
    installed=$("$DEVKITPRO/pacman/bin/pacman" -Q 2>/dev/null) || fail "cannot list devkitPro packages"
    for p in "${toolchain_pkgs[@]}"; do
        grep -qxF "$p" <<< "$installed" || fail "--pack needs $p (the toolchain of $toolchain_image)"
    done
fi

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
    -DCMAKE_C_FLAGS_RELEASE="-g -O2 -DNDEBUG -ffile-prefix-map=$deps/=" \
    -DCMAKE_CXX_FLAGS_RELEASE="-g -O2 -DNDEBUG -ffile-prefix-map=$deps/=" \
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
[[ -n "$pack" ]] || exit 0

pack=$(realpath -m "$pack")
echo "[pack] $pack"
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
top=$stage/nsmbw-switch-deps
mkdir -p "$top/dawn" "$top/dawn-build-switch/src/dawn/native" "$top/uam" "$top/licenses" "$top/patches"
cp -r "$deps/dawn/include" "$top/dawn/"
mkdir -p "$top/dawn-build-switch/gen"
cp -r "$deps/dawn-build-switch/gen/include" "$top/dawn-build-switch/gen/"
# AuroraDawnProvider.cmake reads only this line of Dawn's cache (OpenGL ES fallback on or off).
grep '^DAWN_ENABLE_OPENGLES:BOOL=' "$deps/dawn-build-switch/CMakeCache.txt" > "$top/dawn-build-switch/CMakeCache.txt"
# Debug info only: the code is what the linker puts into the .nro. -D keeps the ar headers fixed.
aarch64-none-elf-strip -g -D -o "$top/dawn-build-switch/src/dawn/native/libwebgpu_dawn.a" "$lib"
aarch64-none-elf-strip -g -D -o "$top/uam/libuam.a" "$deps/uam/build-switch/libuam.a"
for a in "$top/dawn-build-switch/src/dawn/native/libwebgpu_dawn.a" "$top/uam/libuam.a"; do
    # Built before -ffile-prefix-map? Then the absolute PREFIX is compiled in and the archive differs
    # by folder. grep reads a file here, not a pipe, so pipefail cannot trip on it.
    ! grep -qaF "$deps/" "$a" || fail "$a contains the path $deps: delete $deps/dawn-build-switch and rebuild"
done
cp "$deps/dawn/LICENSE" "$top/licenses/Dawn-BSD-3-Clause.txt"
cp "$deps/dawn/third_party/abseil-cpp/LICENSE" "$top/licenses/Abseil-Apache-2.0.txt"
cp "$deps/uam/LICENSE" "$top/licenses/libuam-zlib-and-Mesa-MIT.txt"
cp "$repo/LICENSE" "$top/licenses/GPL-3.0.txt"
cp "$dawn_patch" "$repo"/projects/nsmbw/tools/patches/dawn-*.patch \
    "$repo"/projects/nsmbw/tools/patches/uam-*.patch "$patcher/patch-dawn-abseil-switch.py" \
    "$patcher/patch-dawn-switch.py" "${BASH_SOURCE[0]}" "$top/patches/"
{
    echo "format=1"
    echo "dawn=https://dawn.googlesource.com/dawn $dawn_pin"
    echo "dawn_abseil_cpp=$abseil_pin"
    echo "libuam=https://github.com/averne/libuam $uam_pin"
    echo "wiicompiled_switch=https://github.com/yashin-sh/WiiCompiled-Switch $wcs_pin"
    echo "toolchain_image=$toolchain_image"
    for p in "${toolchain_pkgs[@]}"; do echo "toolchain_package=$p"; done
    echo "compiler=$(aarch64-none-elf-gcc --version | head -n 1)"
    echo "dawn_build=Release, deko3d backend only, static monolithic, -g -O2 -DNDEBUG -ffile-prefix-map, debug info stripped"
    echo "uam_build=meson release, static"
} > "$top/MANIFEST"
cat > "$top/README.txt" <<'EOF'
Prebuilt Switch graphics dependencies for nsmbw-switch (https://github.com/schuler-ph/nsmbw-switch):
Dawn (WebGPU) with its deko3d backend and libuam, static libraries for devkitA64.
Use: projects/nsmbw/tools/build-switch.sh --deps <this archive>. Made by build-switch-deps.sh --pack
(in patches/), which rebuilds the same bytes from the pins in MANIFEST with the toolchain named there.

Licenses (texts in licenses/):
- Dawn and Tint: BSD-3-Clause, Copyright The Dawn & Tint Authors.
- Abseil (inside libwebgpu_dawn.a): Apache-2.0, Copyright The Abseil Authors.
- libuam: zlib, Copyright (C) 2018-2020 fincs; its mesa-imported/ files are MIT (Mesa 3-D).
- The Switch patches applied to Dawn: dawn-nintendo-switch-platform.patch from DriftDroid
  (https://github.com/dorPXP/driftdroid, GPL-3.0), patch-dawn-*.py from WiiCompiled-Switch
  (https://github.com/yashin-sh/WiiCompiled-Switch, GPL-3.0-only) and the nsmbw-switch patches
  (GPL-3.0). The patched libraries are therefore conveyed as a whole under the GNU GPL version 3.

Corresponding source: the upstream commits in MANIFEST plus everything in patches/, applied by
build-switch-deps.sh. The same source is also offered at the place this archive was downloaded from.
EOF
(cd "$top" && find . -type f ! -name SHA256SUMS | LC_ALL=C sort | sed 's|^\./||' | xargs sha256sum > SHA256SUMS)
find "$top" -type d -exec chmod 755 {} +
find "$top" -type f -exec chmod 644 {} +
mkdir -p "$(dirname "$pack")"
tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner --format=gnu -C "$stage" -cf - nsmbw-switch-deps |
    xz -9 -T1 -c > "$pack.part"
mv "$pack.part" "$pack"
(cd "$(dirname "$pack")" && sha256sum "$(basename "$pack")" > "$(basename "$pack").sha256")
echo "Packed: $pack ($(du -h "$pack" | cut -f1))"
cat "$pack.sha256"
