# Game sources compared

Comparison of the original 1997 CD against the Steam release (build 252037),
by SHA-1 of every file, case-insensitive paths.

## `GTADATA`

* 169 files on the CD, 173 on Steam; **167 byte-identical**.
* Differ: `PLAYER_A.DAT`, `REPLAY.REP` — save/replay state only.
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

| | CD `GTAWIN/Grand Theft Auto.exe` | Steam `WINO/Grand Theft Auto.exe` |
| --- | --- | --- |
| Timestamp | 1997-10-10 | 2002-11-11 |
| File size | 909,824 | 774,144 |
| `.text` | 0xB69A0 | 0xA53D9 |
| `.reloc` | yes (0x13EA0) | no |
| Imports | ddraw, mss32, smackw32, DPLAYX, WINMM, WDIR32, wing32, GDI32, USER32, KERNEL32, ADVAPI32 | same set |

The supporting DLLs (`mss32`, `smackw32`, `pmpro*`, `wdir*`) are identical
between the two releases.
