#!/usr/bin/env bash
# Builds the Nintendo Switch version (.nro) of New Super Mario Bros. Wii with Docker as the only
# tool on the host, and lays out the SD card folder <out>/switch/nsmbw/.
#
# Usage: ./build.sh <game folder or disc image> [--deps FILE|URL | --build-deps] [--out DIR] [--jobs N] [--clean]
#
# The image (docker/Dockerfile) holds only the pinned toolchain. Your game is mounted read-only;
# everything made from it stays on this machine: generated/ in this repository, the output folder,
# and the Docker volume nsmbw-work-<id> (unpacked game, build tree). Dawn and libuam are built once
# into the volume nsmbw-deps-<id> and shared by checkouts with the same recipe.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: ./build.sh <game folder or disc image> [--deps FILE|URL | --build-deps] [--out DIR] [--jobs N] [--clean]

  <game>      New Super Mario Bros. Wii, USA SMNE01 revision 1, extracted with Dolphin's
              Extract Entire Disc (a folder with files/ and sys/), or the disc image itself
              (.iso, .wbfs; .rvz and .wia need nodtool)
  --deps F|U  prebuilt Dawn + libuam (build-switch-deps.sh --pack), a file or an http(s) URL,
              checked against the SHA-256 pinned in build-switch.sh. Default: the archive of the
              release switch-deps-1 on GitHub; if it can't be downloaded, they are built here
  --build-deps build Dawn and libuam here instead (about 10 minutes, once per recipe)
  --out DIR   output folder (default: out/ in this repository); the SD card folder is DIR/switch/nsmbw/
  --jobs N    parallel jobs (default: from the free memory, about 1 GiB per job)
  --clean     delete this checkout's work volume (unpacked game, build tree) first

Needs Docker (Linux, WSL2 or Docker Desktop) with about 20 GB of free disk and 8 GB of memory.
The first run takes about half an hour; later runs rebuild only what changed.
EOF
}

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
game=""
out=$repo/out
jobs=""
clean=0
deps=https://github.com/schuler-ph/nsmbw-switch/releases/download/switch-deps-1/nsmbw-switch-deps.tar.xz
while [[ $# -gt 0 ]]; do
    case "$1" in
        --out) out=$2; shift 2 ;;
        --jobs) jobs=$2; shift 2 ;;
        --deps) deps=$2; shift 2 ;;
        --build-deps) deps=""; shift ;;
        --clean) clean=1; shift ;;
        -h|--help) usage; exit 0 ;;
        -*) usage; echo "build.sh: unknown option: $1" >&2; exit 1 ;;
        *) [[ -z "$game" ]] || { usage; exit 1; }; game=$1; shift ;;
    esac
done

fail() {
    echo "build.sh: error: $*" >&2
    exit 1
}

[[ -n "$game" ]] || { usage; exit 1; }
[[ -e "$game" ]] || fail "$game does not exist"
[[ -z "$jobs" || "$jobs" =~ ^[1-9][0-9]*$ ]] || fail "--jobs needs a positive number"
command -v docker >/dev/null || fail "docker not found: install Docker (https://docs.docker.com/get-docker/)"
docker info >/dev/null 2>&1 || fail "Docker is not running, or this user may not use it (docker info fails)"

game=$(cd "$(dirname "$game")" && pwd)/$(basename "$game")
mkdir -p "$out"
out=$(cd "$out" && pwd)
case "$game/" in "$repo/"*) fail "keep the game outside the repository" ;; esac

# Volume names: the work volume belongs to this checkout (its build tree records the paths), the
# deps volume to the recipe in build-switch-deps.sh and its patches.
hash() { sha256sum | cut -c1-12; }
work_volume=nsmbw-work-$(printf '%s' "$repo" | hash)
deps_volume=nsmbw-deps-$(cat "$repo/projects/nsmbw/tools/build-switch-deps.sh" \
    "$repo/aurora-main/cmake/patches/dawn-nintendo-switch-platform.patch" \
    "$repo"/projects/nsmbw/tools/patches/*.patch | hash)
image=nsmbw-switch-build

echo "==== Docker image $image (cached after the first build)"
if ! log=$(docker build -t "$image" "$repo/docker" 2>&1); then
    printf "%s
" "$log" | tail -30 >&2
    fail "docker build failed"
fi

if (( clean )); then
    docker volume rm "$work_volume" >/dev/null 2>&1 || true
    echo "Deleted the volume $work_volume"
fi

if [[ -d "$game" ]]; then
    game_mount=(-v "$game:/game:ro")
    game_arg=/game
else
    # A disc image file: unpack_game.py decides whether it can read it.
    game_mount=(-v "$(dirname "$game"):/game-dir:ro")
    game_arg=/game-dir/$(basename "$game")
fi

deps_mount=()
deps_arg=""
if [[ -n "$deps" ]]; then
    case "$deps" in
        http://*|https://*) deps_arg=$deps ;;
        *)
            [[ -f "$deps" ]] || fail "no such file: $deps"
            deps=$(cd "$(dirname "$deps")" && pwd)/$(basename "$deps")
            deps_arg=/nsmbw/deps-archive/$(basename "$deps")
            deps_mount=(-v "$deps:$deps_arg:ro")
            ;;
    esac
fi

tty=()
[[ -t 1 ]] && tty=(-t)
docker run --rm "${tty[@]}" --init \
    --user "$(id -u):$(id -g)" \
    -v "$repo:/nsmbw/repo" \
    "${game_mount[@]}" \
    "${deps_mount[@]}" \
    -v "$out:/out" \
    -v "$work_volume:/nsmbw/work" \
    -v "$deps_volume:/nsmbw/switch-deps" \
    -e GAME_ARG="$game_arg" \
    -e JOBS="$jobs" \
    -e DEPS_ARG="$deps_arg" \
    "$image" bash /nsmbw/repo/docker/build-in-container.sh
