# joyride

A static recompilation of the 1997 Windows release of *Grand Theft Auto*
(DMA Design / BMG Interactive) into portable C++, running on a modern
SDL3 platform layer (SDL_GPU for rendering: Vulkan, Metal or Direct3D 12).

> **No game code or data is included in this repository.** You need your own
> copy of the game. The original executable is read at build time from your
> copy; the recompiled output is generated locally and is never committed.

## Status

Early setup. Nothing builds yet.

## Approach

1. **Load** the original `GTAWIN/Grand Theft Auto.exe` (PE32, MSVC, Oct 1997).
   Its relocation table identifies every pointer in the image.
2. **Analyse** — discover functions, jump tables and indirect-call targets;
   track progress in a function database.
3. **Translate** each x86 function to C++ against a guest CPU context and a
   flat guest memory image.
4. **Replace the OS layer** — DirectDraw, Miles Sound System, Smacker,
   DirectPlay, WinMM and Win32 calls are reimplemented on SDL3 (SDL_GPU, audio streams, input).
5. **Validate** generated code against traces from the original running game.

## Supported game data

The game data is identical between the original CD, the Steam release and the
free 2002 Rockstar Classics release (apart from saves), so any of them works. See
[`docs/sources.md`](docs/sources.md).

| Source | Executable used for recompilation | Music |
| --- | --- | --- |
| Original CD (1997) | `GTAWIN/Grand Theft Auto.exe` | CD audio tracks 2–11 |
| Steam / Rockstar freeware (2002) | not yet supported (2002 rebuild) | `Music/Track1–10.wav` |

## License

joyride's own source code is released under the [MIT License](LICENSE).
This covers only the code in this repository — not the original game, its
executable or data, nor any code generated from that executable, which remain
the property of their respective rights holders.
