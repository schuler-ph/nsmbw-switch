#!/usr/bin/env bash
# Runs inside the image from docker/Dockerfile; started by ../build.sh, which mounts:
#   /nsmbw/repo (this repository), /game (the extracted disc, read-only), /out (the output folder),
#   /nsmbw/work and /nsmbw/switch-deps (volumes).
# Calls the repository's own scripts in order and lays out /out/switch/nsmbw/.
# Environment: JOBS (parallel jobs), GAME_ARG (the game path inside the container).
set -euo pipefail

repo=/nsmbw/repo
work=/nsmbw/work
deps=/nsmbw/switch-deps
build=$work/build-switch
if [[ -n "${JOBS:-}" ]]; then
    jobs=$JOBS
else
    # About 1 GiB per job: a translated shard needs that while compiling (build-switch.sh runs at
    # most four of them at once), Dawn files less; 3 GiB stay free for linking and the system.
    mem_gib=$(awk '/MemAvailable/ {print int($2 / 1048576)}' /proc/meminfo)
    jobs=$(( mem_gib - 3 < $(nproc) ? mem_gib - 3 : $(nproc) ))
    (( jobs >= 1 )) || jobs=1
    echo "Jobs: $jobs ($mem_gib GiB free, $(nproc) CPUs; override with --jobs N)"
fi
game=${GAME_ARG:-/game}
times=/out/build-times.txt

# Like /etc/profile.d/devkitpro.sh of a devkitPro install (devkita64.sh would also set CC/CXX to devkitA64,
# which breaks host tools in the dependency builds).
export DEVKITA64=$DEVKITPRO/devkitA64
export PATH=$DEVKITPRO/tools/bin:$DEVKITA64/bin:$PATH
mkdir -p "$HOME" "$work/game"
cd "$repo"

fail() {
    echo "build.sh: error: $*" >&2
    exit 1
}

t0=$(date +%s)
t_step=$t0
step_name=""
: > "$times"
step() {
    local now
    now=$(date +%s)
    [[ -z "$step_name" ]] || printf '%-26s %5d s\n' "$step_name" $((now - t_step)) >> "$times"
    step_name=$1
    t_step=$now
    printf '\n==== [%s] %s\n' "$(date +%H:%M:%S)" "$1"
}

step "check game"
# The disc header names the game and its revision; unpack_game.py then checks main.dol byte for byte.
if [[ -d "$game" ]]; then
    python3 - "$game" <<'EOF'
import sys
from pathlib import Path
path = Path(sys.argv[1])
for root in (path, path / "DATA"):
    boot = root / "sys" / "boot.bin"
    if boot.is_file() and (root / "files").is_dir():
        break
else:
    sys.exit("build.sh: error: the game folder needs files/ and sys/ (Dolphin: Extract Entire Disc)")
header = boot.read_bytes()[:8]
game_id, revision = header[:6].decode("ascii", "replace"), header[7]
print(f"Game: {game_id} revision {revision}")
if (game_id, revision) != ("SMNE01", 1):
    sys.exit(f"build.sh: error: this is {game_id} revision {revision}; the port needs "
             "New Super Mario Bros. Wii SMNE01 (USA) revision 1")
EOF
else
    echo "Game: disc image $(basename "$game") (unpack_game.py checks it)"
fi

step "unpack game"
if [[ -f "$game" ]]; then
    # Extract straight into the SD folder: switch/nsmbw/game/ is where the port reads the game, and a
    # later run reuses it instead of extracting again.
    mkdir -p /out/switch/nsmbw
    python3 projects/nsmbw/tools/unpack_game.py "$game" --extract-to /out/switch/nsmbw/game
else
    python3 projects/nsmbw/tools/unpack_game.py "$game"
fi
python3 projects/nsmbw/tools/prelink_rels.py

step "translate"
# The translator steps of build-linux.sh, without its Linux build (the Switch needs only generated/).
project=projects/nsmbw/recomp.yml
logs=$work/logs
mkdir -p "$logs"
logged() { # name command...: output to $logs/name.log, its end on failure
    local name=$1
    shift
    if ! "$@" > "$logs/$name.log" 2>&1; then
        tail -25 "$logs/$name.log" >&2
        fail "$name failed; full log in the volume: $logs/$name.log"
    fi
}
logged translator-build dotnet build translator/Translator.sln -c Release -v quiet -nologo
translator=(dotnet translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll)
logged translate "${translator[@]}" translate-recursive 0x80004050 --project "$project" \
    --outdir generated/functions --output-metadata generated/base_translation_output.json \
    --production-source-bundle generated/base_translation_sources.bin \
    --no-function-files --prune-stale --threads "$(nproc)"
grep -E "Recursively translated|skipped" "$logs/translate.log" || true
logged manifest "${translator[@]}" emit-base-manifest --project "$project" --out build/base \
    --functions-dir generated/functions --translation-output-metadata generated/base_translation_output.json \
    --region E
logged data-init "${translator[@]}" generate-data-init --project "$project"
logged shards "${translator[@]}" emit-build-shards --project "$project" \
    --base-metadata generated/base_translation_output.json --base-functions-dir generated/functions \
    --native-source-dir runtime/src --out generated/build_shards

step "switch deps"
if [[ -n "${DEPS_ARG:-}" ]]; then
    echo "Dawn and libuam: prebuilt archive $(basename "$DEPS_ARG")"
    switch_deps=(--deps "$DEPS_ARG")
else
    # The volume's name carries a hash of build-switch-deps.sh and its patches, so a finished volume
    # matches this checkout's recipe.
    if [[ -f "$deps/.build-docker-done" ]]; then
        echo "Dawn and libuam already built in this volume"
    else
        JOBS=$jobs projects/nsmbw/tools/build-switch-deps.sh "$deps"
        touch "$deps/.build-docker-done"
    fi
    switch_deps=(--dawn-src "$deps/dawn" --dawn-build "$deps/dawn-build-switch"
        --uam "$deps/uam/build-switch/libuam.a")
fi

step "switch build"
stage=$work/stage
rm -rf "$stage"
projects/nsmbw/tools/build-switch.sh "${switch_deps[@]}" --build "$build" --jobs "$jobs" --stage "$stage"

step "copy to output"
# Replace only the program and its data files: a game/ folder the user put there stays.
sd=/out/switch/nsmbw
mkdir -p "$sd"
rm -rf "$sd/wii_bootstrap"
cp -rf "$stage/switch/nsmbw/." "$sd/"
step ""
printf '%-26s %5d s\n' "total" $(( $(date +%s) - t0 )) >> "$times"

echo
cat "$times"
nro=$sd/nsmbw.nro
echo
echo "Done: $nro ($(du -m "$nro" | cut -f1) MiB, SHA-256 $(sha256sum "$nro" | cut -c1-16)...)"
if [[ -d "$sd/game/files" && -d "$sd/game/sys" ]]; then
    echo "Copy the folder switch/nsmbw/ to the root of your SD card; the game is already in it (game/)."
else
    echo "Copy the folder switch/nsmbw/ to the root of your SD card and your extracted disc into"
    echo "switch/nsmbw/game/ (so that switch/nsmbw/game/files/ and switch/nsmbw/game/sys/ exist)."
fi
