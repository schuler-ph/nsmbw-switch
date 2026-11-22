# New Super Mario Bros. Wii for Nintendo Switch

<p align="center">
  <a href="#build"><img alt="Nintendo Switch homebrew (.nro)" src="https://img.shields.io/badge/Nintendo%20Switch-homebrew%20.nro-E60012"></a>
  <a href="#faq"><img alt="PowerPC static recompilation" src="https://img.shields.io/badge/PowerPC-static%20recompilation-FF9F0A"></a>
  <a href="LICENSE"><img alt="License: GPLv3" src="https://img.shields.io/badge/license-GPLv3-2EA44F?logo=gnu&amp;logoColor=white"></a>
</p>

A native **Nintendo Switch homebrew port of New Super Mario Bros. Wii (NSMBW)**, made with static
recompilation: the game's PowerPC code is translated to C++ and compiled for the Switch's ARM CPU
ahead of time. No emulator, no interpreter, no JIT. It runs as an `.nro` from hbmenu on a Switch 1
with Atmosphère, with local multiplayer for up to four players on Joy-Cons and Pro Controllers.



https://github.com/user-attachments/assets/31439f67-389d-44c6-852f-fbb9aaf4d526



This is a fork of [WitherBlitz](https://github.com/WitherBlitz)'s
[NSMB Wii Multiplayer Recompilation](https://github.com/WitherBlitz/NSMB-Wii-Multiplayer-Recompilation),
which this Switch port builds on.

> [!IMPORTANT]
> There are no game files and no downloads here: no disc image, no levels, music, graphics or
> sounds, and no prebuilt `.nro`. You build it yourself from your own copy of
> **New Super Mario Bros. Wii (USA, `SMNE01`, revision 1)**, extracted with
> [Dolphin](https://dolphin-emu.org/). The `.nro` you build contains the game's translated code, so
> do not share it.

## Features

- **Native on the Switch:** the whole game is compiled to AArch64 code; graphics run through
  [aurora](https://github.com/encounter/aurora) on Dawn's deko3d backend, the Switch's own GPU API.
- **Local multiplayer:** up to four players on Joy-Con pairs, single sideways Joy-Cons, Pro
  Controllers or the console in handheld mode (measured with two players so far).
- **Switch button layout and motion controls:** jump on A/B, run on Y/X, spin on the triggers, tilt and
  shake with the controller's motion sensors (see [Controls](#controls)).
- **Full speed docked:** 59-60 fps at 98-99.8 % game speed in the measured levels, docked with the CPU
  at 1785 MHz (see [Performance](#performance)).

## Performance

Measured on one Switch 1 (Erista, Atmosphère), **docked, with the CPU at 1785 MHz** set by sys-clk
(GPU 768 MHz, memory 1600 MHz), two players, builds from November 2026 with no options set:

- Levels 1-1, 4-1 (water), 2-3 (spotlights), 1-4, 4-37 (airship), castle 1 (lava) and 5-3 (poison):
  59-60 fps in the level, 98.1-99.8 % game speed, audio at or above 99.6 % speed.
- The first visit to a level compiles new GPU pipelines; while that happens, some objects can be
  missing for a moment instead of the game stalling.
- At the stock docked CPU clock (1020 MHz) the game thread was the limit: about 43 fps and 72 % game
  speed, measured before later speed-ups and not measured again since.

Not measured so far: other levels and a full playthrough, handheld mode with the current build, and
other Switch models (Mariko, Lite, OLED). Reports are welcome. Raising clocks with sys-clk is at your
own risk.

## Requirements

- A Switch 1 with Atmosphère and hbmenu. Start hbmenu through a title override (hold R while
  starting a game), not from the album: the game needs the memory of a full application.
- [sys-clk](https://github.com/retronx-team/sys-clk) with a docked CPU profile of 1785 MHz, for full
  speed. sys-clk sees the game you start hbmenu through, not the `.nro`, so set the profile on that
  game (or as the global profile).
- Your own **USA `SMNE01` revision 1** disc of New Super Mario Bros. Wii, as a disc image (`.iso`
  or `.wbfs`) or extracted with Dolphin. Other regions and revisions are rejected: the port hooks the
  game at exact addresses.
- To build: Docker on Linux or in WSL2 on Windows (tested: Ubuntu 24.04 in WSL2), about 20 GB of free
  disk and enough memory for 4 jobs (about 5 GB at the peak; tested with 16 GB). Everything else
  comes in a Docker image with pinned versions ([`docker/Dockerfile`](docker/Dockerfile)).

> [!NOTE]
> Nobody here will tell you where to get the game. Dumping your own disc is on you, and links to
> game files won't be provided or tolerated.

## Getting the game files

Dump your own disc as an `.iso` or `.wbfs` (for example with CleanRip on a Wii). `build.sh` checks
the game ID and revision before it does anything else.

- An `.rvz` or `.wia` from Dolphin: right-click the game in Dolphin, **Convert File...**, format
  ISO. (`projects/nsmbw/tools/unpack_game.py` reads them directly with
  [nodtool](https://github.com/encounter/nod) on `PATH`, but the Docker build doesn't ship it yet.)
- A folder works too: in Dolphin, **Properties** → **Filesystem**, right-click the disc, **Extract
  Entire Disc**. You get a folder with `files` and `sys` in it.

## Build

Clone the repository (in WSL2: inside the Linux file system, not under `/mnt/c`) and run one
command:

```
git clone https://github.com/schuler-ph/nsmbw-switch.git
cd nsmbw-switch
./build.sh "<your disc image or extracted game folder>"
```

`build.sh` builds the Docker image, checks your game, translates its code, builds the graphics
libraries (Dawn with deko3d, libuam) and the `.nro`, and puts the SD card folder into
`out/switch/nsmbw/`. The first run took 32 minutes with 4 jobs on a Ryzen 7 5800X3D; a second run
takes about 2 minutes. Options:

- `--deps <file or URL>`: a prebuilt archive of Dawn and libuam (`build-switch-deps.sh --pack`)
  instead of building them (about 10 minutes less); it is checked against the SHA-256 pinned in
  `build-switch.sh`.
- `--jobs N` (default: from the free memory, about 1 GB per job), `--out DIR`, `--clean`.

Your game never goes into the image. What is made from it stays on your machine: `generated/`,
`out/` and the Docker volume `nsmbw-work-<id>` (`docker volume rm` removes it).

<details>
<summary>Without Docker</summary>

You need devkitPro (devkitA64, libnx, deko3d, switch-mesa, switch-tools; tested: the
`devkitpro/devkita64:20260219` image), the .NET 8 SDK, Python 3, CMake, Ninja and Clang
([`projects/nsmbw/BUILDING.md`](projects/nsmbw/BUILDING.md), sections 1-3 and 6), plus `meson`,
`bison`, `flex` and `python3-mako` for Dawn. Then:

```
python3 projects/nsmbw/tools/unpack_game.py <disc image or extracted game folder>
python3 projects/nsmbw/tools/prelink_rels.py
projects/nsmbw/tools/build-linux.sh --game <extracted game folder>
source /etc/profile.d/devkitpro.sh
projects/nsmbw/tools/build-switch-deps.sh ../switch-deps
D=../switch-deps
projects/nsmbw/tools/build-switch.sh --dawn-src $D/dawn --dawn-build $D/dawn-build-switch \
  --uam $D/uam/build-switch/libuam.a --stage ../sd-stage
```

A disc image is extracted next to the repository (`../disc-SMNE01`); that is the folder for
`--game` and for the SD card. `build-switch.sh --deps <archive>` replaces the Dawn and libuam options.
Optional: `--cmake-arg -DMKW_SWITCH_NRO_ICON=/path/to/icon.jpg` (a 256×256 JPEG) gives the `.nro`,
and forwarders made from it, an icon instead of libnx's default one.

</details>

## Install

Copy `out/switch/nsmbw/` to `sdmc:/switch/nsmbw/` on the SD card. If you built from a disc image,
the game is already in it as `game/`; if you built from a folder, add that folder as
`sdmc:/switch/nsmbw/game/` (with `files/` and `sys/` in it):

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

To start it from the HOME menu instead, create a forwarder for `sdmc:/switch/nsmbw/nsmbw.nro` with
[sphaira](https://github.com/ITotalJustice/sphaira) (Install Forwarder, address space 39-bit); this
needs signature patches on the console.

## Controls

Player 1 is the console's player-1 controller, or the attached Joy-Cons in handheld mode. Players 2-4
join as their controllers connect, in the console's own order. A single Joy-Con held sideways counts
as one controller (pair it with SL+SR).

| Switch | Game |
| --- | --- |
| A, B | jump (2) |
| Y, X | run, pick up (1) |
| ZL, ZR, R (SL, SR on a sideways Joy-Con) | shake: spin jump |
| L (stick click on a sideways Joy-Con) | bubble in multiplayer (A) |
| + | pause |
| - (holding + on a sideways Joy-Con) | the remote's - |

In menus, A and B confirm and Y and X go back, as 2 and 1 do on the Wii Remote.

Tilting and shaking the controller works as on a Wii Remote. `NSMBW_SWITCH_MOTION` in `env.txt`
(below) chooses which controllers do that: `on` (default), `nohandheld` (not the console itself) or
`off`.

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
| `projects/nsmbw/tools/build-switch-deps.sh`, `build-switch.sh` | build steps 2 and 3 above |
| `projects/nsmbw/`, `runtime/`, `aurora-main/`, `translator/` | the recompilation project, runtime, renderer and translator from upstream |

The Android app (`android/`) and the Windows/Linux launcher (`Launcher/`) are upstream's and are not
part of the Switch build.

## FAQ

**Is this an emulator?**
No. Everything is compiled to native code before you start it. At runtime nothing emulates a Wii CPU
or GPU.

**Is it a decompilation?**
No, a static recompilation. WiiCompiled's translator turns the game's PowerPC code into C++
automatically, and that is compiled for the Switch. The runtime around it (graphics, audio, input,
the Wii's system software) is source code.

**Do you provide the game or an `.nro`?**
No.

**Which consoles does it run on?**
It is a homebrew program for a Switch 1 with Atmosphère, built and measured on one Erista console so
far. It does not run without custom firmware.

**Will you fix original bugs?**
No, behaving like the real game is the goal. Only report things where this port differs from the
original game on a Wii or in Dolphin.

**The game crashed or shows something wrong.**
Open an issue with your Switch model, docked or handheld, the clocks you set, what you did, and
`log.txt` and `stderr.txt` from `sdmc:/switch/nsmbw/`.

## AI usage

Like the upstream port, the Switch port was coded with Anthropic's Claude models, directed and tested
on the console by schuler-ph.

## Credits

- **[WitherBlitz](https://github.com/WitherBlitz)** - the
  [NSMB Wii Multiplayer Recompilation](https://github.com/WitherBlitz/NSMB-Wii-Multiplayer-Recompilation)
  this fork is built on: WiiCompiled's runtime moved to NSMBW, with its hooks and fixes.
- **[WiiCompiled](https://github.com/patchzyy/Wiicompiled)** by patchzyy and contributors - the
  static recompiler and runtime underneath.
- **[DriftDroid](https://github.com/dorPXP/driftdroid)** by dorPXP - Mario Kart Wii on Switch; the
  Switch platform patches for Dawn (with the deko3d backend), SDL3, Abseil, SQLite and Tracy and parts
  of the Switch runtime come from it (GPL-3.0).
- **[WiiCompiled-Switch](https://github.com/yashin-sh/WiiCompiled-Switch)** by yashin-sh - the
  recipe and patcher scripts for building Dawn for the Switch, fetched at build time (GPL-3.0).
- **[aurora](https://github.com/encounter/aurora)** - the GX rendering backend. MIT licensed.
- **[Dawn](https://dawn.googlesource.com/dawn)** - Google's WebGPU implementation, here with a
  deko3d backend.
- **[devkitPro](https://devkitpro.org/)** - devkitA64, libnx, deko3d and the Switch tools;
  **[libuam](https://github.com/averne/libuam)** - the shader compiler for deko3d.
- **[Dolphin Emulator](https://github.com/dolphin-emu/dolphin)** - the reference for Wii hardware
  behavior, the free DSP coefficient ROM and the default WiiConnect24 tree bundled with the runtime,
  and the tool you extract your game with.
- **The NSMBW reverse-engineering community** - RootCubed's
  [NSMBW symbols](https://github.com/RootCubed/nsmbw-symbols), the NSMBW decompilation and the
  NSMBW address maps, which the function map is built from.

Bundled third-party components and their licenses live in
[`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md).

## Changes in this fork

Modified from upstream `2ff64f5` (2026-11-05) by schuler-ph, November 2026, as required by
section 5 of the GPLv3. The full list is `git diff 2ff64f5`; in short:

- A Nintendo Switch platform layer: `runtime/src/platform/switch/`, `aurora-main/lib/switch/`, the
  Switch patches under `aurora-main/cmake/patches/` and `projects/nsmbw/tools/patches/`, and the build
  scripts `build-switch-deps.sh` and `build-switch.sh`.
- A one-command build: `build.sh` with `docker/`, disc images in `unpack_game.py` (WiiDiscExtract
  as a host tool, nodtool for compressed images) and prebuilt Dawn/libuam archives (`--pack`, `--deps`).
- Speed-ups for the Switch's CPU and GPU: a flat guest memory path, a separate GX thread, faster FIFO
  and pipeline handling, a native reverb, a high-level path for water effects, particle shortcuts, and
  Switch defaults for these.
- Switch controls: button layout, Joy-Con and Pro Controller motion as Wii Remote tilt and shake, and
  a launch option for texture replacements.
- Measurement and diagnostics: a frame/game-speed monitor, a guest profiler and a development link
  for test runs (the link stays off unless `sdmc:/switch/nsmbw/devhost.txt` exists).
- Changes to disc reading (read cache), Wii Remote input and the runtime build files for the Switch.
- This README, rewritten for the Switch; upstream's covers Android, Windows and Linux.

## License

This project is free software: you can redistribute it and/or modify it under the terms of the
[GNU General Public License, version 3](LICENSE) as published by the Free Software Foundation, like
WiiCompiled, which it is based on.

It is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the
implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public
License for more details.

The license covers this project's code only; it gives no rights to Nintendo's game, its code or its
assets. Not affiliated with, endorsed by, or associated with Nintendo. New Super Mario Bros. Wii and
Nintendo Switch are trademarks of Nintendo.
