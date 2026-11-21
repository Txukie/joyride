# Game sources compared

Comparison of the original 1997 CD, the Steam release (build 252037) and the
2002 Rockstar Classics freeware release (`GTAINSTALLER.exe`, InstallShield 5,
SHA-1 `762e03126a226e92e2f6827ca0750489595b52b2`), by SHA-1 of every file,
case-insensitive paths.

**The freeware release and Steam are byte-identical** — all 195 installed
files match; Steam only adds its own `installscript.vdf`. Below, "Steam"
stands for both.

## `GTADATA`

* 169 files on the CD, 173 on Steam; **167 byte-identical**.
* Differ: `PLAYER_A.DAT`, `REPLAY.REP` — save/replay files; the 2002 release
  ships different defaults (same in freeware and Steam, so not play state).
* Steam only: `F_RSTAR.RAW`, `F_RSTARN.RAW`, `F_RSTAR white.RAW` (Rockstar
  splash), `replay1.rep` (save).

The DOS folder (`GTADOS`) exists only on the CD.

## Music

| CD (Red Book audio) | Steam |
| --- | --- |
| tracks 2–11, 44.1 kHz stereo | `Music/Track1.wav` … `Track10.wav`, 22.05 kHz stereo 16-bit |

Steam `TrackN.wav` corresponds to CD track N+1. The 1997 executable plays them
through Miles `AIL_redbook_*`; the Steam executable opens the WAV files.

## Windows executables

| | CD `GTAWIN/Grand Theft Auto.exe` | Steam / freeware `WINO/Grand Theft Auto.exe` |
| --- | --- | --- |
| Timestamp | 1997-10-10 | 2002-11-11 |
| File size | 909,824 | 774,144 |
| `.text` | 0xB69A0 | 0xA53D9 |
| `.reloc` | yes (0x13EA0) | no |
| Static imports | mss32, smackw32, DPLAYX, WINMM, GDI32, USER32, KERNEL32, ADVAPI32 | same set |
| Loaded at run time (MGL) | ddraw, wing32, WDIR32 | same set |

The supporting DLLs (`mss32`, `smackw32`, `pmpro*`, `wdir*`) are identical
between the two releases.
