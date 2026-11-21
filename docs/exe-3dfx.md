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

**Bilinear filtering, in the software renderer.** `tools/postlift.py`
splices two statements into each 32-bit A and D loop: at the texel fetch,
`joyride_bilerp32` (`src/hle/texfilter.c`) blends the four texels around
the exact sub-texel position, each through the face's own shading table; at
the colour load the loop's register takes the blend. The polygon entry
points record whether the texture is a 64×64 world tile (neighbours clamp to
the tile, so faces never bleed into the next texture) or a sprite (clamp to
the page). In transparent loops, colour-0 neighbours get no weight: cut-out
edges keep the original shape, with no dark fringe.

Everything else stays the game's: geometry, draw order, clipping, the HUD.
That is close to what the 3dfx build did (CPU geometry, filtered texturing).
The differences: filtering happens at the game's own resolution (the
Windows exe offers up to 1024×768 itself), and cut-outs follow the original
texels instead of a filtered alpha threshold.

The F12 settings overlay switches it (`JOYRIDE_FILTER=0|1` for one run). The blend is two
packed 32-bit lerps per row and one between rows, with the four texels'
colours cached while consecutive pixels share them. Up to 2560×1440
(`JOYRIDE_RES`, see `src/hle/display.c`) the game keeps its 23.3 fps (a
70 Hz Miles timer, 3 ticks a frame); at 3440×1440 filtering costs about a
quarter of that, and since the game's speed is tied to its frame rate,
sharp textures are the way to play there.

Not done yet:

* 16-bit colour modes (the A16 loops; the D loops only exist in 32-bit
  form, and how walls draw in 16-bit modes is not traced yet). 8-bit modes cannot be
  filtered: a blend has no palette entry.
* The scaled blits (C loops) and translucent polygons (B loops).
* Rendering the world on the GPU from the quads themselves, which would
  allow resolutions above the game's own.
