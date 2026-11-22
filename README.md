# joyride

A static recompilation of the Windows release of *Grand Theft Auto*
(DMA Design / BMG Interactive) into portable C++, running on a modern
SDL3 platform layer (SDL_GPU for rendering: Vulkan, Metal or Direct3D 12).

> **No game code or data is included in this repository.** You need your own
> copy of the game. The original executable is read at build time from your
> copy; the recompiled output is generated locally and is never committed.

## Status

**Plays on Linux: menus, city select and Liberty City in game.** The whole
executable is lifted to C (see [`docs/exe-2002.md`](docs/exe-2002.md)), linked
against pcrecomp's win32hle Win32 layer plus joyride's own Miles, Smacker and
DirectDraw additions, and runs the game's own code: its C runtime, `WinMain`,
SciTech MGL's DirectDraw driver, the front end and the game itself.

The platform layer is SDL3 only: window, input and audio through SDL3, the
picture through SDL_GPU (Vulkan, Metal or Direct3D 12).

The intro movie plays (decoded with ffmpeg). The 2002 release itself ships
with it switched off; joyride turns it back on (`JOYRIDE_NO_INTRO=1` keeps it
off, any key skips it).

Network play works between joyride instances (see below).

### Settings (F12)

**F12** opens joyride's settings over the game (in play the game pauses
while it is open). Up/Down pick, Left/Right change, Enter applies a screen
size, F12 or Esc closes. They are kept in `~/.local/share/joyride/joyride.cfg`.

* **Screen size** — the game plays at 1024×768 on its own; any size joyride
  offers from 640×480 to 3840×1600 (the common 4:3, 16:9, 16:10 and 21:9
  sizes, and your desktop's), changed in play. Wider screens show more of
  the city to the sides at the game's usual zoom, instead of the game's own
  behaviour of zooming in. Menus stay 640×480.
* **Renderer** — software (default: the game's own rasterizer, as
  released) or GPU: the world drawn by the graphics card the way the 3dfx
  version did it, the CPU keeping only the geometry. Same picture; it keeps
  the display's rate at any size (60 fps at 60 Hz up to 3440×1440, ~140 at
  144 Hz), where the software renderer manages 23 at 2560×1440 and 17 at
  3440×1440. Vulkan only for now. See
  [`docs/exe-3dfx.md`](docs/exe-3dfx.md).
* **Textures** — bilinear filtering, the 3dfx version's look (default), or
  sharp. Filtered on the GPU like the 3dfx card did (Vulkan; on the CPU
  elsewhere); see [`docs/exe-3dfx.md`](docs/exe-3dfx.md). 32-bit colour only.
* **Motion** — smooth (default): frames drawn between the game's own steps,
  up to the display's refresh rate, with the game's speed unchanged (it
  still steps 23.3 times a second; the picture runs one step, 43 ms,
  behind). Or original: 23.3 frames a second. See
  [`src/hle/smooth.c`](src/hle/smooth.c). At 1024×768 it reaches the
  display's rate (60 fps at 60 Hz), filtered or not; larger sizes get fewer
  as the software renderer takes longer (about 30–36 at 1920×1080). Turning the game's limiter
  off (F8) still just runs the game faster.
* **Scaling** of the picture to the window: sharp (default), smooth,
  nearest, integer.
* **Fullscreen** (also F11 or Alt+Enter).

With the software renderer, up to 2560×1440 keeps the game's full speed
with filtering, but with few frames between steps; the GPU renderer keeps
the display's rate there.
`JOYRIDE_RES=<w>x<h>|desktop|game`, `JOYRIDE_RENDERER=software|gpu`,
`JOYRIDE_FILTER=0|1`, `JOYRIDE_SMOOTH=0|1` and `JOYRIDE_SCALE=<mode>` override
the file for one run.

Not yet: Windows and macOS builds.

### Network play

GTA's multiplayer (up to 4 players) runs on joyride's own DirectPlay, so
joyride plays with joyride, not with the original Windows game.

* **Host:** *Gather Network* → character → city and level → options →
  connection *Internet TCP/IP Connection (joyride)* → name the session → wait
  for players → *Enter* to start.
* **Join:** *Join Network* → character → the same connection → pick the game.
* Games on the **local network** are found automatically.
* Over the **internet**, the joining player starts joyride with
  `JOYRIDE_NET_HOST=<host's address>` and the host forwards **TCP and UDP
  port 47624** to their machine. `JOYRIDE_NET_PORT` changes the port (same
  on every machine).
* A player who leaves is reported by the game ("Connection to … has failed")
  and the others play on.

### Keys

| Key | |
| --- | --- |
| F12 | settings: screen size, renderer, textures, motion, scaling, fullscreen |
| F11, Alt+Enter | fullscreen |
| Esc, then Enter | quit (the game's own menu); closing the window twice also quits |

## Approach

1. **Load** `WINO/Grand Theft Auto.exe` (PE32, MSVC 6, Nov 2002) — the build
   shipped on Steam and as the free Rockstar Classics download.
2. **Analyse** — discover functions, jump tables and indirect-call targets;
   track progress in a function database.
3. **Translate** each x86 function to C++ against a guest CPU context and a
   flat guest memory image.
4. **Replace the OS layer** — DirectDraw, Miles Sound System, Smacker,
   DirectPlay, WinMM and Win32 calls are reimplemented on SDL3 (SDL_GPU, audio streams, input).
5. **Validate** generated code against traces from the original running game.

## Getting started

```bash
git clone --recursive https://github.com/Txukie/joyride && cd joyride
python3 -m venv .venv && .venv/bin/pip install -r tools/requirements.txt
# needs: CMake, Ninja, a C compiler, SDL3 and SDL3_ttf (development files);
# optional: ffmpeg's libavformat/libavcodec for the intro movie, glslc
# (shaderc) for texture filtering on the GPU
mkdir -p game_data && cp -r "<your install>/WINO" game_data/
tools/lift.sh        # writes the generated C to gen/ (never committed)
cmake -S . -B build -G Ninja && cmake --build build
build/joyride "<your install>"   # the folder holding WINO/ and GTADATA/
```

Nothing in the install is modified: saves, settings and the registry go to
`~/.local/share/joyride/overlay`. Developer switches: `JOYRIDE_HEADLESS=1`,
`JOYRIDE_DUMP=<dir>` (one frame per second as PPM), `JOYRIDE_QUIT_AFTER=<s>`,
`JOYRIDE_KEYS="4:RETURN,15:UP:6"` (key presses, or holds, at times in seconds),
`JOYRIDE_MAX_FPS` (default the display's refresh rate in play, 60 in menus;
0 = unlimited), `JOYRIDE_SMOOTH_DEBUG=1` (frame timing),
`JOYRIDE_GPU_DEBUG=1` (SDL_GPU validation), `JOYRIDE_FPS=1` (frames shown and game
steps a second, every 5 s; steps should hold 23.3).

## Supported game data

The game data is identical between the original CD, the Steam release and the
free 2002 Rockstar Classics release (apart from saves), so any of them works. See
[`docs/sources.md`](docs/sources.md).

| Source | Executable used for recompilation | Music |
| --- | --- | --- |
| Steam / Rockstar freeware (2002) | **`WINO/Grand Theft Auto.exe`** | `Music/Track1–10.wav` |
| Original CD (1997) | data only (its 1997 exe is not the target) | CD audio tracks 2–11 |

## License

joyride's own source code is released under the [MIT License](LICENSE).
This covers only the code in this repository — not the original game, its
executable or data, nor any code generated from that executable, which remain
the property of their respective rights holders.
