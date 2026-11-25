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

### Settings (F1)

**F1**, or **L3+R3** on a controller, opens joyride's settings over the
game (in play the game pauses while it is open). Up/Down pick, Left/Right
change, Enter applies a screen size, F1 or Esc closes; on a controller the
d-pad or left stick move, A accepts, B goes back. They are kept in `~/.local/share/joyride/joyride.cfg`.

In the order the menu shows them, the defaults first.

*Graphics*

* **Screen size** — your desktop's (default), the game's own 1024×768, or
  any size joyride offers from 640×480 to 3840×2160 (the common 4:3, 16:9,
  16:10 and 21:9 sizes), changed in play. Wider screens show more of the
  city to the sides at the game's usual zoom, instead of the game's own
  behaviour of zooming in, and people and traffic are made and removed
  out of sight at the screen's real edges. Menus stay 640×480.
* **Fullscreen** — yes (default) or no (also F11 or Alt+Enter).
* **Scaling** of the picture to the window: sharp (default), smooth,
  nearest, integer.
* **Renderer** — GPU (default): the world drawn by the graphics card, the
  CPU keeping only the geometry; it keeps the display's rate at any size
  (60 fps at 60 Hz up to 3440×1440, ~140 at 144 Hz). Or software: the
  game's own rasterizer, as released, which manages 23 fps at 2560×1440
  and 17 at 3440×1440. Same picture. Vulkan only for now; how it works is
  in [`docs/exe-3dfx.md`](docs/exe-3dfx.md).
* **Textures** — bilinear (default) or sharp. Filtered on the GPU (Vulkan;
  on the CPU elsewhere). 32-bit colour only.
* **Motion** — smooth (default): frames drawn between the game's own steps,
  up to the display's refresh rate, with the game's speed unchanged (it
  still steps 23.3 times a second; the picture runs one step, 43 ms,
  behind). Or as released: 23.3 frames a second. See
  [`src/hle/smooth.c`](src/hle/smooth.c). Turning the game's limiter off
  (F8) still just runs the game faster.
* **HUD size** (GPU renderer) — the game draws its HUD at its 640×480
  size whatever the screen; it is enlarged by whole times, piece by piece
  (score and weapon stay in their corners, centred messages keep their
  layout), the direction arrows with it: auto (default: ×3 at 4K, ×2 at
  1080p and 1440p) or ×1–×4.

*Gameplay*

* **Controller** — modern (default) or classic; see *Controllers* below.
* **Drive-bys** — *like in the game* (default: only where London 1961's
  missions allow them) or *always*, in every game: fire in a car shoots
  your weapon out of its side (see the Launcher section), except in a car
  with a bomb fitted, where fire is the bomb's. In a network game
  the host's choice is everyone's, for the whole game.
* **Retry missions** — yes (default) or no (as the game: a failed job is
  gone). With it, a few seconds after "MISSION FAILED!" (or "FRENZY
  FAILED!") the city goes back by itself to how it was before the mission,
  so you can return to where it started and take it again: the phone
  rings again, a car you get into to start a job is back where it was
  parked (made again if it was wrecked; if you are still in it, it is
  yours to keep as it is and a new one is parked there), and the mission's
  own cars and people are taken away so it starts afresh. Your score,
  weapons and place are kept. Not in network games.

**Pause menu and saving (Start, or F6).** GTA never let you save; joyride
does. In play, Start (F6 or Esc on the keyboard) pauses (the game, its
music, sounds and timers all held) with *Continue*, *Save
game* and *Quit* (the game asks "Ya want out?": Enter or A ends the level,
Esc or B plays on). Esc no longer ends a level by itself. One save per
character and level (each city's chapter), in
`~/.local/share/joyride/saves/`: your score and its multiplier, your
weapons' ammunition and the missions you have passed. Starting that level
again puts them back, always at the level's spawn point, with no wanted
level and nothing broken. A mission is passed or not started: the game
saves itself as each mission starts, and a save during one keeps your
score and weapons but not that mission (the menu says first how long ago
it started; a failed mission is offered again). See
`src/platform/savegame.c`. Not in network games.

**joyride's own art** (`art/`: the icon, `joyride.svg`, and the launcher's
background, a city at night seen from above) is original, made for joyride
by `tools/art.py`, and free to use (CC0); nothing in it is from the games.

**Menus and the intro on wide screens.** The menus stay 640×480; on a
screen that is not 4:3 joyride puts them on a wall that goes with each
game, made from that game's own menu art when joyride is prepared (or the
first time the game starts), by `tools/backdrops.py`: GTA's rust with a
city seen from above in it, London 1969's teal with mod roundels, London
1961's with a houndstooth. They are in `~/.local/share/joyride/extracted/menu/` as
`gta1.png`, `london1969.png` and `london1961.png` (32:9, cropped to the
window); put your own picture there under that name to use it instead
(`delta.png` or `delta.jpg` there is used for a game that has none). The
intro movie is shown as large as the screen's height allows, and the
space beside it is filled with the movie itself, blurred, so it follows
each scene.

With the software renderer, up to 2560×1440 keeps the game's full speed
with filtering, but with few frames between steps; the GPU renderer keeps
the display's rate there.
`JOYRIDE_RES=<w>x<h>|desktop|game`, `JOYRIDE_RENDERER=software|gpu`, `JOYRIDE_CONTROLLER=modern|classic`,
`JOYRIDE_FILTER=0|1`, `JOYRIDE_SMOOTH=0|1`, `JOYRIDE_HUD=auto|1-4`, `JOYRIDE_DRIVEBY=game|always`, `JOYRIDE_RETRY=0|1` and `JOYRIDE_SCALE=<mode>` override
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

### Controllers

Any controller SDL knows (Xbox, PlayStation, Switch, most others) works
with no setup. Two schemes, switched in F1 → Controller
(`controller=modern|classic` in `joyride.cfg`, `JOYRIDE_CONTROLLER` for one
run). The keyboard keeps working alongside either.

**Modern, on foot**

| | |
| --- | --- |
| Left stick | run in that direction (the character turns to it at once) |
| Right stick | face / aim that way; moving against it walks backwards, slower |
| X | shoot / attack |
| A | jump (over cars) |
| Y | enter vehicle |
| B | special |
| L1 / R1 | previous / next weapon |

With the right stick released the character faces where it runs, or where
it last ran. GTA has no walk on foot (any push runs), so how far the stick
is pushed doesn't change the speed; and no sidestep, so moving across where
you aim doesn't move.

**Modern, driving**

| | |
| --- | --- |
| Left stick | steer (the game's own analog steering, 7 steps each way) |
| Right stick | aim the tank's turret where the stick points |
| R2 / L2 | accelerate / brake and reverse, progressive: half way is half the pull |
| X | shoot (the tank's cannon; drive-bys when they are on) |
| A | handbrake |
| Y | exit vehicle |
| B | special |
| L1 / R1 | previous / next weapon (for drive-bys) |
| D-pad left / right | previous / next radio station |

**Both:** Start opens the pause menu (continue, save, quit; F6 on the
keyboard); Select shows the city map; d-pad up / down zoom the game in and
out (from twice as close to twice as far; + and - on the keyboard);
L3+R3 opens the settings; the left back buttons (L4 / L5 on a Steam
Controller or Steam Deck) are the debugging keys below. In the
game's menus the d-pad or left stick move, A (or Start) accepts and B goes
back.

**Classic** puts the game's own tank controls on the pad: left stick steers,
up/down or R2/L2 go forward and back, same buttons.

How it works: joyride adds the pad's input to the game's own per-step action
word through the game's own handlers (`src/platform/pad.c`), so network
games stay in step; the one exception is the instant turn on foot, which
sets the character's heading directly, and the triggers' share of the
engine. The game's throttle and brake are on or off, so joyride keeps them
on and scales the push the game gives the car each step by how far the
trigger is pressed; the cars have drag, so half throttle also levels off
at about half the top speed. The radio stations are the game's ten music tracks;
d-pad left/right move the car's music to the next or previous one without stopping,
and the next car you get into plays its own station again.

The tank is the only vehicle with a turret in this version of the game:
the fire engine has none to aim. The game itself turns a turret only while
Special is held with the steering pushed, and it doesn't steer the tank
meanwhile, so joyride does the same: while the turret turns towards the
right stick (about a third of a second for a quarter turn) the left stick
doesn't steer; once it's on target, it does again.

### City map (Select, or M)

Select on the controller, or M on the keyboard, shows a map of the whole
city over the game at 70% (press again to hide it): you as a bright blue
dot, the current objectives (the game's arrow targets: a place, a car or a
person, and the red arrow's) as bright red dots. GTA has no map picture, so
joyride draws one for each of the three cities from the game's own map and
texture files the first time it runs (a few seconds, in the background) and
keeps them in `~/.local/share/joyride/maps/`; delete them to rebuild.
See [`src/platform/map.c`](src/platform/map.c).

### Debugging (L4 / L5, F7 / F8)

L4 (F7) writes a report to `~/.local/share/joyride/debug/<time>/`
for a bug report: `report.txt` (the settings, and the last ten seconds of
play step by step and frame by frame: player, car, camera, controls,
frame times; cars that turn up above street level and which of the game's spawners set them up), the next ten frames on screen (`screen_00.png` to
`screen_09.png`, written in the background over a few seconds), the
game's own surface (`cpu.png`) and its memory (`memory.bin`). L5
(F8) gives every weapon with 99 rounds and, in a vehicle, turns it into
the next model the city has (all but the tank). See
[`src/platform/debug.c`](src/platform/debug.c).

### Keys

| Key | |
| --- | --- |
| F1 | settings: screen size, renderer, HUD size, textures, motion, controller, drive-bys, failed missions, scaling, fullscreen |
| M | city map |
| + / - | zoom the game in / out |
| F6 | the pause menu: continue, save, quit |
| F7 | debug report (as L4) |
| F8 | all weapons, next vehicle (as L5) |
| F11, Alt+Enter | fullscreen |
| Esc | the pause menu in play (Quit there ends the level, after the game's question); in the menus, back |

## Approach

1. **Load** `WINO/Grand Theft Auto.exe` (PE32, MSVC 6, Nov 2002) — the build
   shipped on Steam and as the 2002 Rockstar Classics release.
2. **Analyse** — discover functions, jump tables and indirect-call targets;
   track progress in a function database.
3. **Translate** each x86 function to C++ against a guest CPU context and a
   flat guest memory image.
4. **Replace the OS layer** — DirectDraw, Miles Sound System, Smacker,
   DirectPlay, WinMM and Win32 calls are reimplemented on SDL3 (SDL_GPU, audio streams, input).
5. **Validate** generated code against traces from the original running game.

## Launcher

`launcher/` is a front end for all of it, made for a controller as much as
for keys and mouse (Qt Quick; controllers through SDL3: d-pad or stick
move, A chooses, B goes back, Start plays):

The launcher downloads nothing: every game comes from a copy you own.

* **Grand Theft Auto** — a Steam copy it finds, or an install folder or the
  2002 installer (`GTAINSTALLER.exe`) you choose.
* **GTA: London 1969** — from your own disc: a BIN/CUE image (its music is
  taken from the audio tracks), an ISO, or the mounted disc.
* **GTA: London 1961** — from your copy of its installer, `GTAL1961.EXE`.
  Needs London 1969.
* **Grand Theft Auto 2** — listed, not playable yet.
* **Prepare joyride** runs `tools/lift.sh` on your GTA executable and the
  build (once, a few minutes); **Settings** edits the same
  `joyride.cfg` as the in-game F1 menu; **Play** starts the game.

London runs on the same recompiled 2002 executable: its files go over GTA's
under GTA's names, and its levels into the slots the 2002 front end has
(see `launcher/src/core/games.cpp`). London 1961's drive-bys, which the 2002
executable lacks, are joyride's: while a mission allows them, fire in a car
shoots your weapon out of its side as London 1961's own executable does
(`src/platform/driveby.c`); the Drive-bys setting makes them always on, in
any of the games. Each London game keeps its own saves and city
maps (`JOYRIDE_PROFILE`), and the city map (M) shows London.

```bash
cmake -S launcher -B build-launcher -G Ninja && cmake --build build-launcher
build-launcher/joyride-launcher
# needs: Qt 6.5+ (Quick, QuickControls2, Network), SDL3, libarchive, zlib.
# Without Qt, build-launcher/joyride-setup does the installing from the command line.
```

Installs go to `~/.local/share/joyride/games/`. On starting, the launcher puts its
`joyride.desktop` and icon in `~/.local/share/applications/` and
`~/.local/share/icons/`, so the desktop knows it (its icon, the menu entry).
B (Esc) goes back a page, and on the first one closes the launcher.

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
`JOYRIDE_PAD_SCRIPT="20:1,0,0,0,0,0,0;23:..."` (a scripted controller: time,
then left x/y, right x/y, L2, R2, buttons in hex), `JOYRIDE_PAD_LOG=1`,
`JOYRIDE_MAX_FPS` (default the display's refresh rate in play, 60 in menus;
0 = unlimited), `JOYRIDE_SMOOTH_DEBUG=1` (frame timing),
`JOYRIDE_GPU_DEBUG=1` (SDL_GPU validation), `JOYRIDE_FPS=1` (frames shown and game
steps a second, every 5 s; steps should hold 23.3).

## Supported game data

The game data is identical between the original CD, the Steam release and the
2002 Rockstar Classics release (apart from saves), so any of them works. See
[`docs/sources.md`](docs/sources.md).

| Source | Executable used for recompilation | Music |
| --- | --- | --- |
| Steam / Rockstar Classics (2002) | **`WINO/Grand Theft Auto.exe`** | `Music/Track1–10.wav` |
| Original CD (1997) | data only (its 1997 exe is not the target) | CD audio tracks 2–11 |

## License

joyride's own source code is released under the [MIT License](LICENSE).
This covers only the code in this repository — not the original game, its
executable or data, nor any code generated from that executable, which remain
the property of their respective rights holders.
