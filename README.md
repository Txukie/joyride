# joyride

A static recompilation of the 1997 Windows release of *Grand Theft Auto*
(DMA Design / BMG Interactive) into portable C++, running on a modern
SDL2/OpenGL platform layer.

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
   DirectPlay, WinMM and Win32 calls are reimplemented on SDL2/OpenGL.
5. **Validate** generated code against traces from the original running game.

## Supported game data

The game data is identical between the original CD and the Steam release
(apart from saves), so either install works. See
[`docs/sources.md`](docs/sources.md).

| Source | Executable used for recompilation | Music |
| --- | --- | --- |
| Original CD (1997) | `GTAWIN/Grand Theft Auto.exe` | CD audio tracks 2–11 |
| Steam | not yet supported (2002 rebuild) | `Music/Track1–10.wav` |
