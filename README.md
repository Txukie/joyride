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

Not yet: the intro movie (skipped), sound checked by ear, the SDL3 + SDL_GPU
presenter (bring-up uses win32hle's SDL2 screen), Windows and macOS builds.

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
mkdir -p game_data && cp -r "<your install>/WINO" game_data/
tools/lift.sh        # writes the generated C to gen/ (never committed)
cmake -S . -B build -G Ninja && cmake --build build
build/joyride "<your install>"   # the folder holding WINO/ and GTADATA/
```

Nothing in the install is modified: saves, settings and the registry go to
`~/.local/share/joyride/overlay`. Developer switches: `JOYRIDE_HEADLESS=1`,
`JOYRIDE_DUMP=<dir>` (one frame per second as PPM), `JOYRIDE_QUIT_AFTER=<s>`,
`JOYRIDE_KEYS="4:RETURN,7:DOWN"` (key presses at times in seconds).

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
