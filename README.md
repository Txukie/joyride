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

Bilinear texture filtering, the 3dfx version's look, is on by default in
32-bit colour modes (F10 toggles, `JOYRIDE_FILTER=0` starts with it off);
see [`docs/exe-3dfx.md`](docs/exe-3dfx.md).

### Screen size

The game plays at 1024×768 on its own. `JOYRIDE_RES=<width>x<height>` (or
`JOYRIDE_RES=desktop`) plays at another size, widescreen and ultrawide
included: the view keeps the game's zoom from top to bottom and shows more
of the city to the sides, instead of the game's own behaviour of zooming in.
Sizes from 640×480 to 3840×1600 that joyride's display list offers (the
common 4:3, 16:9, 16:10 and 21:9 sizes, and your desktop's); the log lists
the game's modes. Menus stay 640×480.

The renderer is software, as in the original: up to 2560×1440 runs at full
speed with filtering; 3440×1440 needs filtering off (F10) to keep it.

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
| F10 | texture filtering: bilinear (default) or off |
| F11, Alt+Enter | fullscreen |
| F12 | scaling: sharp (default), smooth, nearest, integer |
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
# optional: ffmpeg's libavformat/libavcodec for the intro movie
mkdir -p game_data && cp -r "<your install>/WINO" game_data/
tools/lift.sh        # writes the generated C to gen/ (never committed)
cmake -S . -B build -G Ninja && cmake --build build
build/joyride "<your install>"   # the folder holding WINO/ and GTADATA/
```

Nothing in the install is modified: saves, settings and the registry go to
`~/.local/share/joyride/overlay`. Developer switches: `JOYRIDE_HEADLESS=1`,
`JOYRIDE_DUMP=<dir>` (one frame per second as PPM), `JOYRIDE_QUIT_AFTER=<s>`,
`JOYRIDE_KEYS="4:RETURN,15:UP:6"` (key presses, or holds, at times in seconds),
`JOYRIDE_MAX_FPS` (default 60, 0 = unlimited), `JOYRIDE_SCALE=sharp|smooth|nearest|integer`,
`JOYRIDE_GPU_DEBUG=1` (SDL_GPU validation), `JOYRIDE_FPS=1` (the game's frame
rate every 5 s; it should hold 23.3).

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
