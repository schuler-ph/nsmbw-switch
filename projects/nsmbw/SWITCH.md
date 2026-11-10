# Nintendo Switch (homebrew)

An experimental port of this project to the Nintendo Switch 1 as a homebrew program (`nsmbw.nro`).
It uses the same translated game code as the Linux build, with a Switch platform layer, aurora on
Dawn's deko3d backend, and libnx. There are **no downloads**: you build it yourself from your own
disc. The `.nro` contains the game's translated code, so do not share it.

> [!IMPORTANT]
> You need your own **USA `SMNE01` revision 1** disc, extracted with Dolphin (see the main README),
> and a Switch that runs custom firmware. Nothing here tells you how to get either.

## What has been measured

Measured on one Switch 1 (Erista, Atmosphère), **docked, with the CPU at 1785 MHz** set by sys-clk
(GPU 768 MHz, memory 1600 MHz), two players, with builds of this branch from November 2026 and no
options set:

- Levels 1-1, 4-1 (water), 2-3 (spotlights), 1-4, 4-37 (airship), castle 1 (lava) and 5-3 (poison):
  59-60 fps in the level, 98.1-99.8 % game speed, audio at or above 99.6 % speed.
- The first visit to a level compiles new GPU pipelines; while that happens, some objects can be
  missing for a moment instead of the game stalling.
- At the stock docked CPU clock (1020 MHz) the game thread is the limit: about 43 fps and 72 % game
  speed in the last measurement, before later speed-ups. Handheld mode is not a target and has not
  been measured with the current build.

Not checked yet: a full playthrough, quicksand levels, other Switch models (Mariko, Lite, OLED).
Raising clocks with sys-clk is at your own risk.

## What you need

- A Switch 1 with Atmosphère and hbmenu. Start hbmenu through a title override (hold R while
  starting a game), not from the album: the game needs the memory of a full application.
- [sys-clk](https://github.com/retronx-team/sys-clk) with a CPU profile of 1785 MHz for this
  program while docked, if you want full speed.
- A Linux machine or WSL2 (tested: Ubuntu 24.04) with about 20 GB free and 16 GB of RAM.
- devkitPro with devkitA64, libnx, deko3d, switch-mesa and switch-tools. Tested versions: the
  `devkitpro/devkita64:20260219` Docker image (devkitA64 r29.2, libnx 4.12.0, deko3d 0.5.0,
  switch-tools 1.13.1).
- Everything the Linux build needs for translating the game ([`BUILDING.md`](BUILDING.md), steps
  1-3 and 6), plus `meson`, `bison`, `flex` and `python3-mako` for the graphics dependencies.

## Build

1. Translate the game as for Linux ([`BUILDING.md`](BUILDING.md), section 6). `build-linux.sh`
   does steps 2 and 3 and fills `generated/`; the Switch build uses the same translated code.

2. Build Dawn with the deko3d backend and libuam once (about 35 minutes):

   ```
   source /etc/profile.d/devkitpro.sh
   projects/nsmbw/tools/build-switch-deps.sh ../switch-deps
   ```

3. Build the `.nro` and lay out an SD card folder:

   ```
   D=../switch-deps
   projects/nsmbw/tools/build-switch.sh --dawn-src $D/dawn --dawn-build $D/dawn-build-switch \
     --uam $D/uam/build-switch/libuam.a --stage ../sd-stage
   ```

   The first build takes about 15 minutes with 4 jobs (each translated shard needs about 1 GB of
   RAM); the result is `../build-wb-switch/nsmbw.nro`, about 110 MiB.

## Install

Copy `../sd-stage/switch/nsmbw/` to `sdmc:/switch/nsmbw/` on the SD card, then add your extracted
disc as `sdmc:/switch/nsmbw/game/` (with `files/` and `sys/` in it):

```
sdmc:/switch/nsmbw/
  nsmbw.nro
  dsp_coef.bin
  initial_pipeline_cache.db
  wii_bootstrap/
  game/files/ ...
  game/sys/ ...
```

Start `nsmbw` from hbmenu. The first start compiles about 1 700 shaders (about 10 minutes, with a
counter in the top left); later starts reuse the cache.

## Controllers

Player 1 is the console's player-1 controller, or the attached Joy-Cons in handheld mode. Players 2-4
join as their controllers connect, in the console's own order. A single Joy-Con held sideways works
as a sideways Wii Remote (pair it with SL+SR); tapping its +/- button pauses, holding it opens the
quick panel.

## Logs and options

- `sdmc:/switch/nsmbw/log.txt` and `stderr.txt` are written on every start; attach them to issues.
- `sdmc:/switch/nsmbw/env.txt` sets runtime options, one `NAME=value` per line (`#` starts a
  comment). The Switch defaults are built in; you only need this file for experiments.
- `sdmc:/switch/nsmbw/gfx_flags.txt` sets `AURORA_GFX_FLAGS` (for example `validate`) for graphics
  debugging. Delete it afterwards: it slows the game down.

## Where the Switch code lives

| Path | What |
| --- | --- |
| `runtime/src/platform/switch/` | entry point, file system, logging, memory and thread setup |
| `aurora-main/lib/switch/` | window/surface, controller input, GL entry points for switch-mesa |
| `aurora-main/cmake/patches/*-nintendo-switch-platform.patch` | Switch support for Dawn (with the deko3d backend), SDL3, Abseil, SQLite and Tracy, taken unchanged from DriftDroid |
| `projects/nsmbw/tools/patches/` | fixes on top of Dawn and libuam |
| `projects/nsmbw/tools/build-switch-deps.sh`, `build-switch.sh` | the two build steps above |

Several runtime parts are ported from [DriftDroid](https://github.com/dorPXP/driftdroid) (Mario Kart
Wii on Switch, GPL-3.0); the files say which. See [`THIRD-PARTY-NOTICES.md`](../../THIRD-PARTY-NOTICES.md).
