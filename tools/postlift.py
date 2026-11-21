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
TEXHOOKS = [
    # fetch,     colour,      reg,   u fraction,          v fraction,          transparent
    (0x00499211, 0x00499217, "edx", "eax >> 16",         "ebp >> 16",         0),
    (0x00499813, 0x0049981E, "edx", "eax >> 16",         "ebp >> 16",         1),
    (0x0049AA88, 0x0049AA8C, "esi", "ecx >> 16",         "MEM32(0x4B8C0C)",   0),
    (0x0049AB8A, 0x0049AB92, "esi", "(ecx - eax) >> 16", "MEM32(0x4B8C0C)",   1),
    (0x0049AEDE, 0x0049AEE2, "esi", "ecx >> 16",         "MEM32(0x4B8C0C)",   0),
    (0x0049AFEE, 0x0049AFF6, "esi", "(ecx - eax) >> 16", "MEM32(0x4B8C0C)",   1),
]
TEXMARK = "joyride_tex"

# The A loops (0x496CF0) draw world lids (entry 0x497710, 64x64 tiles) and
# sprites (0x49787C, 0x4979C9: free-sized, packed in the page); the D loops
# (walls, 0x497035 and 0x497332) are tiles too. Each entry
# tells the filter which to clamp neighbours to.
TEXENTRY = {0x00497035: 1, 0x00497332: 1, 0x00497710: 1, 0x0049787C: 0, 0x004979C9: 0}


def texhook_lines():
    after = {}
    for fetch, col, reg, fu, fv, tr in TEXHOOKS:
        after[fetch] = (f"{{ extern uint32_t joyride_texfilter, joyride_texc; "
                        f"extern uint32_t joyride_bilerp32(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t); "
                        f"if (joyride_texfilter) joyride_texc = joyride_bilerp32(ebx, {fu}, {fv}, edx, {tr}); }}")
        after[col] = (f"{{ extern uint32_t joyride_texfilter, joyride_texc; "
                      f"if (joyride_texfilter) {reg} = joyride_texc; }}")
    for a, tile in TEXENTRY.items():
        after[a] = f"{{ extern uint32_t joyride_texclamp; joyride_texclamp = {tile}; }}"
    return after


TEXAFTER = texhook_lines()
ADDR = re.compile(r"/\* 0x([0-9A-F]{8}): ")

GOTO = re.compile(r"goto L_([0-9A-F]{8});.*/\* 0x([0-9A-F]{8}): (j\w*|loop\w*) ")


def process(path: str) -> int:
    with open(path) as f:
        lines = f.readlines()
    out, added = [], 0
    hooked = 0
    for i, line in enumerate(lines):
        m = GOTO.search(line)
        if m and int(m.group(1), 16) <= int(m.group(2), 16):
            prev = out[-1] if out else ""
            if "RECOMP_BACKEDGE()" not in prev:
                indent = line[:len(line) - len(line.lstrip())]
                out.append(f"{indent}RECOMP_BACKEDGE();\n")
                added += 1
        out.append(line)
        a = ADDR.search(line)
        if a and int(a.group(1), 16) in TEXAFTER and line.lstrip().startswith(("SET_HI8", "edx =", "esi =", "PUSH32(esp, ebp)")):
            nxt = lines[i + 1] if i + 1 < len(lines) else ""
            if TEXMARK not in nxt:
                indent = line[:len(line) - len(line.lstrip())]
                out.append(f"{indent}{TEXAFTER[int(a.group(1), 16)]}\n")
                hooked += 1
    process.hooked = getattr(process, "hooked", 0) + hooked
    added += hooked
    if added:
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
    print(f"postlift: {total - hooked} loop back-edges given a RECOMP_BACKEDGE(), "
          f"{hooked} texture filter splices (of {2 * len(TEXHOOKS) + len(TEXENTRY)} sites)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
