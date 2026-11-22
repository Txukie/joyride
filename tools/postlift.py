#!/usr/bin/env python3
"""Post-pass over pcrecomp's generated C (tools/lift.sh runs it).

    python3 tools/postlift.py gen/wino/recomp

Adds RECOMP_BACKEDGE() before every branch back to an earlier instruction,
i.e. every loop. pcrecomp's generate.py does this; the default translator
(`python -m tools`) does not. Without it a loop that spins on a variable
another guest thread updates never lets that thread run: GTA's frame limiter
(0x0047DC96) waits on a counter its Miles timer callback advances, and the
timer thread waits for the machine the spinning loop holds.

The generated lines carry the instruction's address in their comment
(`/* 0x0047DC9C: jb 0x47dc96 */`), so a backward goto is recognisable from
the text alone. Idempotent: a line already preceded by RECOMP_BACKEDGE()
is left alone.

Also splices joyride's texture filter (src/hle/texfilter.c) into the
renderer's 32-bit span loops; see TEXHOOKS.
"""

import os
import re
import sys

# Bilinear texture filtering (src/hle/texfilter.c, docs/exe-3dfx.md).
# Per span loop: the texel fetch (`mov dh, byte ptr [ebx]`, after which
# joyride_bilerp32 gets the texel address, the two 16-bit coordinate
# fractions, the shading table and whether colour 0 is transparent), then
# the colour load whose register takes the blend.
#   A: affine quads, both coordinates step; fractions in the top halves of
#      eax (u) and ebp (v), read before they advance.
#   D: wall spans (horizontal, then vertical), u steps by ecx's top half,
#      v is fixed per span at [0x4B8C0C] (16.16); where the add comes
#      before the fetch the fraction is the one before it, ecx - eax.
#   The pixel about to be written is [edi], or [edi - 4] where the loop
#   has already stepped edi (0x49AA88).
TEXHOOKS = [
    # fetch,     colour,      reg,   pixel,     u fraction,          v fraction,        transparent
    (0x00499211, 0x00499217, "edx", "edi",     "eax >> 16",         "ebp >> 16",       0),
    (0x00499813, 0x0049981E, "edx", "edi",     "eax >> 16",         "ebp >> 16",       1),
    (0x0049AA88, 0x0049AA8C, "esi", "edi - 4", "ecx >> 16",         "MEM32(0x4B8C0C)", 0),
    (0x0049AB8A, 0x0049AB92, "esi", "edi",     "(ecx - eax) >> 16", "MEM32(0x4B8C0C)", 1),
    (0x0049AEDE, 0x0049AEE2, "esi", "edi",     "ecx >> 16",         "MEM32(0x4B8C0C)", 0),
    (0x0049AFEE, 0x0049AFF6, "esi", "edi",     "(ecx - eax) >> 16", "MEM32(0x4B8C0C)", 1),
]
TEXMARK = "joyride_tex"

# The A loops (0x496CF0) draw world lids (entry 0x497710, 64x64 tiles) and
# sprites (0x49787C, 0x4979C9: free-sized, packed in the page); the D loops
# (walls, 0x497035 and 0x497332) are tiles too. Each entry
# tells the filter which to clamp neighbours to.
TEXENTRY = {0x00497035: 1, 0x00497332: 1, 0x00497710: 1, 0x0049787C: 0, 0x004979C9: 0}


def texhook_lines():
    after = {}
    for fetch, col, reg, dst, fu, fv, tr in TEXHOOKS:
        # joyride_tex_record is inline (src/hle/texfilter_inline.h, force-included)
        after[fetch] = (f"{{ extern uint32_t joyride_texfilter; "
                        f"if (joyride_texfilter) joyride_tex_record({dst}, ebx, {fu}, {fv}, edx, {tr}); }}")
        after[col] = (f"{{ extern uint32_t joyride_texfilter, joyride_texc; "
                      f"if (joyride_texfilter == 1) {reg} = joyride_texc; }}")
    for a, tile in TEXENTRY.items():
        after[a] = f"{{ extern void joyride_tex_begin(uint32_t); joyride_tex_begin({tile}); }}"
    return after


TEXAFTER = texhook_lines()

# The screen size in play (src/hle/display.c, JOYRIDE_RES): the getter of
# the mode index (0x414D30 `mov al, byte ptr [0x503224]`), and the mode
# list filter (after 0x415415, before `cmp ecx, 0x640`: ecx the width,
# [esp + 0x18] the height), which may skip to its "keep" path 0x41545C.
TEXAFTER[0x00414D30] = ("{ extern uint32_t joyride_pick_mode(uint32_t); "
                        "SET_LO8(eax, joyride_pick_mode(LO8(eax))); }")
TEXAFTER[0x00415415] = ("{ extern int joyride_pick_mode_allowed(uint32_t, uint32_t); "
                        "if (joyride_pick_mode_allowed(ecx, MEM32(esp + 0x18))) goto L_0041545C; }")
# The camera (0x43BB30, 0x43C9C3: scale = view width * zoom / 320) and the
# y squash for non-4:3 modes (0x46456E), both made aspect-neutral when the
# size is joyride's: the scale follows the height, so wider shows more.
for _a in (0x0043BB30, 0x0043C9C3):
    TEXAFTER[_a] = ("{ extern uint32_t joyride_view_scale(uint32_t, uint32_t, uint32_t); "
                    "MEM32(ecx + 0x24) = joyride_view_scale(MEM32(ecx + 0xC), edi, edx); }")
TEXAFTER[0x0046456E] = ("{ extern int joyride_square_pixels(void); "
                        "if (joyride_square_pixels()) MEM8(ecx + 0x74F18C) = 0; }")
# The frame's safe point (src/platform/overlay.c): the frame limiter's first
# instruction, 0x47DC80 `mov al, byte ptr [0x502f34]`, once a frame in play.
TEXAFTER[0x0047DC80] = "{ extern void joyride_safe_point(void); joyride_safe_point(); }"
# Smooth motion (src/hle/smooth.c): around the game's own draw (after the
# update call 0x414999, after the draw call 0x41499E), and at the sprite
# object drawer's entry (0x47C050, object at [esp + 0x14] once it has
# reserved its locals) and exit (0x47C125, its locals released).
TEXAFTER[0x00414999] = "{ extern void joyride_smooth_begin(void); joyride_smooth_begin(); }"
TEXAFTER[0x0041499E] = "{ extern void joyride_smooth_end(void); joyride_smooth_end(); }"
TEXAFTER[0x0047C050] = ("{ extern void joyride_smooth_sprite_in(uint32_t); "
                        "joyride_smooth_sprite_in(MEM32(esp + 0x14)); }")
# The tile converter (0x49B044), before it writes the cache slot at
# [0x4B8C5C] (src/hle/texfilter.c: the GPU path keeps the old texels).
TEXAFTER[0x0049B044] = "{ extern void joyride_tex_overwrite(uint32_t); joyride_tex_overwrite(MEM32(0x4B8C5C)); }"
TEXAFTER[0x0047DC70] = "{ extern void joyride_smooth_tick(void); joyride_smooth_tick(); }"
TEXAFTER[0x0047C125] = "{ extern void joyride_smooth_sprite_out(void); joyride_smooth_sprite_out(); }"
# The hardware renderer (src/hle/hwrender.c): at the polygon fill and the
# two wall leaves, right after their first push; a captured quad skips the
# software (undo the push, return).
for _a, _f in ((0x00496CF0, "joyride_hw_polygon"), (0x0049A78C, "joyride_hw_hwall"), (0x0049ABDC, "joyride_hw_vwall")):
    TEXAFTER[_a] = (f"{{ extern int {_f}(void); "
                    f"if ({_f}()) {{ esp += 4; RECOMP_FLAGS_OUT(); esp += 4; return; }} }}")
# Controllers (src/platform/pad.c): the input reader 0x432E00 at 0x433141,
# its action word complete at [esp + 0xc]; the pad's changes are added to
# it there, then eax reloads it.
TEXAFTER[0x00433141] = ("{ extern void joyride_pad_input(uint32_t); "
                        "joyride_pad_input(esp + 0xC); eax = MEM32(esp + 0xC); }")
# MGL's mode size lookup (0x48B770), right after it stores the height
# (0x48BFE2 `mov [ecx], ebx`; the width went to [[esp + 0x420]]).
TEXAFTER[0x0048BFE2] = ("{ extern void joyride_mode_size(uint32_t, uint32_t); "
                        "joyride_mode_size(MEM32(esp + 0x420), ecx); }")
# The renderer's row table: the address of each screen row, [0x503228 +
# 4 * y], filled at each mode switch (0x497BFA). It has room for 1706 rows
# before the game's mode list at 0x504CD0, so taller screens (2160 rows)
# would overwrite that. joyride gives it a bigger one in guest memory
# (joyride_rows, src/hle/display.c): every indexed use is pointed there.
ROWS = (re.compile(r"(MEM32\(\w+ \* 4 \+ )0x503228\)"), r"\1joyride_rows)")
ROWS_M1 = (re.compile(r"(MEM32\(\w+ \* 4 \+ )0x503224\)"), r"\1joyride_rows - 4u)")

# The area around each view where peds (and cars) live, 0x43CA30: half
# width (height + 384) * 160 / zoom, half height 3/4 of it: a 4:3 screen's.
# Peds are made 64 pixels outside it and removed past it, so on a wider
# screen they pop in and out in view; joyride widens it to the view's
# shape (src/hle/display.c), once the half height is taken.
TEXAFTER[0x0043CA66] = ("{ extern uint32_t joyride_live_half_width(uint32_t, uint32_t); "
                        "edi = joyride_live_half_width(edi, esi); }")
# the same, each frame, in the camera update (0x43BB30; half width in ecx)
TEXAFTER[0x0043BB62] = ("{ extern uint32_t joyride_live_half_width(uint32_t, uint32_t); "
                        "ecx = joyride_live_half_width(ecx, esi); }")
ADDR = re.compile(r"/\* 0x([0-9A-F]{8}): ")

GOTO = re.compile(r"goto L_([0-9A-F]{8});.*/\* 0x([0-9A-F]{8}): (j\w*|loop\w*) ")


SPLICE = re.compile(r"^\s*\{ extern .*joyride_")


def process(path: str) -> int:
    with open(path) as f:
        original = f.readlines()
    lines = [l for l in original if not SPLICE.match(l)]            # joyride's splices are redone
    out, added = [], 0
    hooked = 0
    for i, line in enumerate(lines):
        for pat, rep in (ROWS, ROWS_M1):
            line = pat.sub(rep, line)
        m = GOTO.search(line)
        if m and int(m.group(1), 16) <= int(m.group(2), 16):
            prev = out[-1] if out else ""
            if "RECOMP_BACKEDGE()" not in prev:
                indent = line[:len(line) - len(line.lstrip())]
                out.append(f"{indent}RECOMP_BACKEDGE();\n")
                added += 1
        out.append(line)
        a = ADDR.search(line)
        if a and int(a.group(1), 16) in TEXAFTER and line.lstrip().startswith(("SET_HI8", "SET_LO8(eax, MEM8(0x503224))", "SET_LO8(eax, MEM8(0x502F34))", "RECOMP_CALL(sub_00430B20)", "RECOMP_CALL(sub_00430D40)",
        "esp = esp - 0x10u", "esp = esp + 0x10u", "PUSH32(esp, esi)", "eax = MEM32(0x775580)", "edi = MEM32(0x4B8C58)", "eax = MEM32(esp + 0xC)", "esp = esp + 0x10u", "MEM32(ecx) = ebx", "MEM32(ecx + 0x24) = edx", "MEM8(ecx + 0x74F18C)", "edx =", "esi =", "PUSH32(esp, ebp)")):
            if True:
                indent = line[:len(line) - len(line.lstrip())]
                out.append(f"{indent}{TEXAFTER[int(a.group(1), 16)]}\n")
                hooked += 1
    process.hooked = getattr(process, "hooked", 0) + hooked
    added += hooked
    if out != original:
        tmp = path + ".tmp"
        with open(tmp, "w") as f:
            f.writelines(out)
        os.replace(tmp, path)
    return added


def main() -> int:
    d = sys.argv[1] if len(sys.argv) > 1 else "gen/wino/recomp"
    total = 0
    for name in sorted(os.listdir(d)):
        if name.startswith("recomp_0") and name.endswith(".c"):
            total += process(os.path.join(d, name))
    hooked = getattr(process, "hooked", 0)
    print(f"postlift: {total - hooked} loop back-edges given a RECOMP_BACKEDGE(), {hooked} joyride splices")
    return 0


if __name__ == "__main__":
    sys.exit(main())
