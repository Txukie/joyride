# The 3dfx build, and what joyride takes from it

The original CD ships three DOS builds in `GTADOS/`, one of them for 3dfx
Voodoo cards:

| File | What |
| --- | --- |
| `GTA8.EXE` | DOS, 8-bit software renderer (VESA through SciTech UniVBE) |
| `GTA24.EXE` | DOS, 16/24-bit software renderer |
| `GTAFX.EXE` | DOS, **3dfx Glide** renderer |
| `3DFX/GRTVGR.EXE` | not the game: 3dfx's own Glide 2.x runtime installer (InstallShield in a self-extracting zip, `glide2x.dll`/`.ovl` for Voodoo and Voodoo Rush, Oct 1997) |

There was never a Direct3D build; the Windows exe (the 2002 one joyride
targets, [exe-2002.md](exe-2002.md)) is software only. Players remember the
3dfx version as smoother and brighter, with no other differences, and the
code agrees.

## GTAFX.EXE

1,994,131 bytes: a DOS/4GW bound executable whose program is a standard
Linear Executable at file offset `0x3B80C` (its MZ stub at `0x38D64`; LE
data pages are relative to that stub). Watcom C/C++32. Five objects, code
at `0x20000` (`0x1226D8` bytes). It loads `glide2x.ovl` with its own DLL
loader and resolves Glide by name lazily: 129 five-byte stubs at
`0x142244 + 5*i`, names at `0x1424D0 + 4*i`; the first call patches each
stub into a `jmp`.

Of the 129 functions listed, **23 are ever called**:

| Glide call | Use |
| --- | --- |
| `grGlideInit`, `grSstQueryHardware`, `grSstSelect(0)`, `grSstWinOpen`, `grSstWinClose`, `grGlideShutdown` | start-up and shutdown |
| `grSstWinOpen(0, res, 60 Hz, ARGB, upper-left origin, 2 colour buffers, 0 aux)` | resolutions 512×384, 640×350, 640×400, 640×480, 800×600; no depth buffer |
| `guColorCombineFunction(DECAL_TEXTURE)`, `grTexCombineFunction(0, DECAL)` | colour is the texel, unlit, no vertex colour |
| `grAlphaCombine(SCALE_OTHER, ONE, CONSTANT, TEXTURE)`, `grAlphaTestReferenceValue(0x80)`, `grAlphaTestFunction(GREATER)` | alpha is the texture's 1-bit alpha; cut-out transparency, no blending |
| `grTexFilterMode(0, BILINEAR, BILINEAR)` | **bilinear filtering, always**: the only call, no toggle |
| `grTexMipMapMode(0, DISABLE, FALSE)` | no mipmaps |
| `guTexAllocateMemory(…, ARGB_1555, …)`, `guTexDownloadMipMap`, `guTexMemQueryAvail`, `guTexMemReset`, `guTexSource` | textures uploaded as 16-bit ARGB1555, converted from the 8-bit pages |
| `guDrawTriangleWithClip` | 10 call sites in 5 functions, each a quad as two triangles (one triangle when a flag says so) |
| `grLfbLock(WRITE_ONLY, BACKBUFFER, 565, …)`, `grLfbUnlock` | the HUD and text drawn by the CPU straight into the back buffer |
| `grBufferSwap`, `grSstOrigin` | present |

Not used: fog, gamma, depth buffering, alpha blending, antialiasing,
mipmapping, per-vertex shading. The brighter look was the Voodoo's own
gamma, not the game.

The CPU still does all the geometry: the 5 quad functions (`0x11BCA7`,
`0x11C5D3`, `0x11CE4B`, `0x11D2B2`, `0x11D5F3`) take the four screen-space
corners, a texture id and flags, and only rasterization moved to the card.
`0x11CE4B` is called from 24 places in the block renderer (`0x7BE85` to
`0x8369B`), `0x11D2B2`/`0x11D5F3` 3 times each from the sprite code. Texture
ids address 64×64 tiles in 256×256 pages: page `id >> 4`, column
`(id & 3) * 64`, row `((id >> 2) & 3) * 64`. That is the same layout the
Windows exe uses.

## The Windows exe's renderer

Found by putting gdb hardware watchpoints on back-buffer pixels during
play: the game rasterizes straight into
the DirectDraw back buffer with a hand-written assembly module at
`0x496000`–`0x49B100` (odd, unaligned entry points). Its entry points:

| Entry | Called from | Draws | Loops (by depth) |
| --- | --- | --- | --- |
| `0x497035` | 9 sites in the block renderer `0x438D60`–`0x43B607` | wall faces, horizontal spans | `0x49A78C`: D32, D32 transparent |
| `0x497332` | 7 sites, same | wall faces, vertical spans | `0x49ABDC`: D32, D32 transparent |
| `0x497710` | 16 sites, same | lids (roads, roofs) | `0x496CF0` → A8/A16/A32, transparent and translucent variants |
| `0x49787C`, `0x4979C9` | `0x47C007`, `0x47C92D`, `0x47BFCC`, `0x47C8F2` | sprites (cars, people) | `0x496CF0`, as above |
| `0x49806A` | `0x481718`, `0x4817A1` | scaled blits | C16, C32 |

`0x497DFC` selects the texture: tile `t` lives at `[0x78C110] + (t >> 4) *
0x10000`, offset `(t & 3) * 64`, `((t >> 2) & 3) * 64` (a cache at
`0x78C120` holds converted transparent tiles). The colour of texel `x` is
the 32-bit pixel at `shade + x * 256`, `shade` being the face's lighting
table (`[0x78C10C]` for walls, `[0x4B8974]` for polygons).

The texel loops (all `mov dh, byte ptr [ebx]`, `ebx` = page | v << 8 | u):

| Loop | Kind | u fraction | v fraction | Colour |
| --- | --- | --- | --- | --- |
| `0x499211` / `0x499813` | A32 / A32 transparent | `eax >> 16` | `ebp >> 16` | `edx` (`0x499217` / `0x49981E`) |
| `0x49AA88` / `0x49AB8A` | D32 horizontal / transparent | `ecx >> 16` / `(ecx - eax) >> 16` | `[0x4B8C0C]` low half | `esi` (`0x49AA8C` / `0x49AB92`) |
| `0x49AEDE` / `0x49AFEE` | D32 vertical / transparent | `ecx >> 16` / `(ecx - eax) >> 16` | `[0x4B8C0C]` low half | `esi` (`0x49AEE2` / `0x49AFF6`) |
| `0x498E32`, `0x499402` | A8, A8 transparent | | | 8-bit |
| `0x49901F`, `0x499607` | A16, A16 transparent | | | 16-bit |
| `0x499A0E`, `0x499C79`, `0x499ED9` | translucent (555, 565, 32) | | | blend with the screen through `[0x78C108]` |
| `0x49A4F1`, `0x49A743` | C16, C32 | | | one texel per step |

## What joyride does with it

**Bilinear filtering on the GPU, as the 3dfx card did it.** The CPU keeps
doing what it did for the Voodoo too, all the geometry, and draws its spans
as always. `tools/postlift.py` splices a call into each 32-bit A and D loop's
texel fetch: `joyride_tex_record` (`src/hle/texfilter_inline.h`, compiled
into the lifted loops themselves; slow paths in `src/hle/texfilter.c`)
records, for the pixel about to be written, where the texel came from:

| | bits |
| --- | --- |
| u, v in the 256×256 page, to 1/64 texel | 14 + 14 |
| texture page slot (0 = not recorded) | 7 |
| shading table slot | 10 |
| transparent (colour 0 skipped), tile clamp | 1 + 1 |
| check: 17 bits of the colour written | 4 + 13 |

The presenter (`src/platform/screen_sdl3.c`) uploads that record, the pages
and shading tables that changed (each compared by hash; a table is 256
colours 256 bytes apart in the game's memory), and runs
`shaders/texfilter.frag` over the frame: each recorded pixel is re-sampled
from the four texels around its point, through its table. World textures
clamp to their 64×64 tile, sprites to their page; in transparent loops
colour 0 gets no weight, so cut-outs keep their shape. A pixel whose colour
no longer matches its check was drawn over afterwards (HUD, text, effects)
or is left from an earlier frame, and stays as the game drew it, so the
record is never cleared.

The game rewrites part of its memory mid-frame: a cache of converted
(rotated) tiles whose slots it reuses (`0x49B044`). A splice there copies the
page aside for the pixels already recorded from it before it changes.

Shaders are GLSL, compiled to SPIR-V by the build (`glslc`), so the GPU path
runs where SDL_GPU takes SPIR-V (Vulkan). Elsewhere, or without `glslc`,
`joyride_bilerp32` filters on the CPU instead: it blends at the fetch and
the loop writes the blend. Same picture; the CPU path costs about twice the
draw time.

Measured with smooth motion (`src/hle/smooth.c`), the game holding 23.3
steps a second: a filtered draw at 1024×768 takes 6.4 ms with the GPU path,
so it runs at the display's full rate (60 fps at 60 Hz; 41 with the CPU
filter); at 2560×1440 the game's own step
takes 28.6 ms of its 42.9 with the GPU path, so the game keeps its speed
(the CPU filter slows it to 20 steps a second). What limits it now is the
software rasterizer itself; drawing the game's quads on the GPU outright
is the next step.

The F12 settings overlay switches it (`JOYRIDE_FILTER=0|1|cpu` for one run).
`JOYRIDE_HEADLESS_GPU=1` with `JOYRIDE_GPU_DUMP=<dir>` filters without a
window and saves the result (tests).

Not done yet:

* 16-bit colour modes (the A16 loops; the D loops only exist in 32-bit
  form, and how walls draw in 16-bit modes is not traced yet).
* The scaled blits (C loops) and translucent polygons (B loops).
