#!/usr/bin/env python3
"""First-pass function discovery over an unpacked image.

    .venv/bin/python tools/funcs.py gen/analysis

Reads the artefacts written by tools/unpack.py and does recursive-descent
disassembly from every known code entry point:

  * the PE entry point and exports
  * targets of direct calls (found as we go)
  * code addresses stored in data (relocations from .data/.rdata into .text:
    vtables, callback tables, CRT init tables)
  * code addresses used as immediates in code (push offset / mov reg, offset)
  * prologue-looking starts in the gaps left over (reported separately)

Jump tables (`jmp [reg*4 + table]`) are followed through the relocation
table, which tells us exactly how many entries each table has.

Writes gen/analysis/functions.json and prints coverage statistics.
"""

from __future__ import annotations

import bisect
import collections
import json
import os
import struct
import sys

from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_INVALID

STOP_MNEM = {"ret", "retf", "iret", "iretd", "hlt", "int3", "ud2"}


class Image:
    def __init__(self, d: str):
        self.dir = d
        self.mem = open(os.path.join(d, "image.bin"), "rb").read()
        self.pe = json.load(open(os.path.join(d, "pe.json")))
        rel = json.load(open(os.path.join(d, "relocs.json")))["highlow"]
        self.imports = json.load(open(os.path.join(d, "imports.json")))
        self.base = self.pe["image_base"]
        t = next(s for s in self.pe["sections"] if s["name"] == ".text")
        self.text_lo, self.text_hi = t["va"], t["va"] + t["vsize"]
        self.reloc_target = {r["at"]: r["target"] for r in rel}
        self.reloc_rows = rel
        self.iat = {i["iat_va"]: f'{i["dll"]}!{i["name"] or "#%d" % i["ordinal"]}'
                    for i in self.imports}

    def in_text(self, va: int) -> bool:
        return self.text_lo <= va < self.text_hi

    def u32(self, va: int) -> int:
        return struct.unpack_from("<I", self.mem, va - self.base)[0]

    def code(self, va: int, n: int = 16) -> bytes:
        o = va - self.base
        return self.mem[o:o + n]


class Discovery:
    def __init__(self, img: Image):
        self.img = img
        self.cs = Cs(CS_ARCH_X86, CS_MODE_32)
        self.cs.detail = True
        self.insn: dict[int, tuple[int, str, str]] = {}   # va -> (size, mnem, ops)
        self.owner: dict[int, int] = {}                     # insn va -> function
        self.funcs: dict[int, dict] = {}
        self.seed_src: dict[int, set[str]] = collections.defaultdict(set)
        self.jump_tables: dict[int, list[int]] = {}         # table va -> targets
        self.imm_code_refs: set[int] = set()
        self.queue: list[int] = []

    def seed(self, va: int, why: str) -> None:
        if not self.img.in_text(va):
            return
        self.seed_src[va].add(why)
        if va not in self.funcs and va not in self.queue:
            self.queue.append(va)

    def decode(self, va: int):
        for i in self.cs.disasm(self.img.code(va), va, 1):
            return i
        return None

    def jump_table(self, op_mem, table: int) -> list[int]:
        out = []
        va = table
        while va in self.img.reloc_target and self.img.in_text(self.img.reloc_target[va]):
            out.append(self.img.reloc_target[va])
            va += 4
            if va in self.jump_tables:   # ran into another table
                break
        return out

    def explore(self, start: int) -> dict:
        f = {"start": start, "blocks": 0, "insns": 0, "bytes": 0, "calls": set(),
             "imports": set(), "jump_tables": [], "tail_jumps": set(),
             "bad": [], "max": start, "kind": "func"}
        work, seen = [start], set()
        while work:
            pc = work.pop()
            if pc in seen:
                continue
            f["blocks"] += 1
            while True:
                if pc in seen:
                    break
                if not self.img.in_text(pc):
                    f["bad"].append(pc)
                    break
                if pc in self.owner and self.owner[pc] != start and pc in self.funcs:
                    # fell into another known function: treat as tail call
                    f["tail_jumps"].add(pc)
                    break
                i = self.decode(pc)
                if i is None:
                    f["bad"].append(pc)
                    break
                seen.add(pc)
                ops = i.op_str
                self.insn[pc] = (i.size, i.mnemonic, ops)
                self.owner.setdefault(pc, start)
                f["insns"] += 1
                f["bytes"] += i.size
                f["max"] = max(f["max"], pc + i.size)
                m = i.mnemonic
                # immediates that are relocated code addresses -> callbacks
                for k in range(pc, pc + i.size - 3):
                    if k in self.img.reloc_target:
                        tgt = self.img.reloc_target[k]
                        if self.img.in_text(tgt) and m not in ("jmp", "call"):
                            self.imm_code_refs.add(tgt)
                nxt = pc + i.size
                if m == "call":
                    op = i.operands[0]
                    if op.type == X86_OP_IMM:
                        f["calls"].add(op.imm)
                        self.seed(op.imm, "call")
                    elif op.type == X86_OP_MEM and op.mem.base == X86_REG_INVALID \
                            and op.mem.index == X86_REG_INVALID:
                        tgt = op.mem.disp & 0xFFFFFFFF
                        if tgt in self.img.iat:
                            f["imports"].add(self.img.iat[tgt])
                    pc = nxt
                    continue
                if m == "jmp":
                    op = i.operands[0]
                    if op.type == X86_OP_IMM:
                        tgt = op.imm
                        if tgt in self.funcs and tgt != start:
                            f["tail_jumps"].add(tgt)
                        else:
                            work.append(tgt)
                    elif op.type == X86_OP_MEM:
                        disp = op.mem.disp & 0xFFFFFFFF
                        if op.mem.base == X86_REG_INVALID and op.mem.index == X86_REG_INVALID \
                                and disp in self.img.iat:
                            f["imports"].add(self.img.iat[disp])
                            if f["insns"] == 1:
                                f["kind"] = "import_thunk"
                        elif op.mem.index != X86_REG_INVALID and op.mem.scale == 4:
                            tbl = self.jump_table(op.mem, disp)
                            if tbl:
                                self.jump_tables[disp] = tbl
                                f["jump_tables"].append(disp)
                                work.extend(tbl)
                    break
                if m.startswith("j") or m in ("loop", "loope", "loopne", "jecxz"):
                    work.append(i.operands[0].imm)
                    pc = nxt
                    continue
                if m in STOP_MNEM:
                    break
                pc = nxt
        f["calls"] = sorted(f["calls"])
        f["imports"] = sorted(f["imports"])
        f["tail_jumps"] = sorted(f["tail_jumps"])
        return f

    def run(self) -> None:
        img = self.img
        self.seed(img.pe["entry"], "entry")
        for e in img.pe["exports"]:
            self.seed(img.base + e["rva"], "export")
        for r in img.reloc_rows:
            if r["to"] == ".text" and r["in"] in (".data", ".rdata"):
                self.seed(r["target"], "data_ptr")
        rounds = 0
        while True:
            rounds += 1
            while self.queue:
                va = self.queue.pop()
                if va in self.funcs:
                    continue
                self.funcs[va] = self.explore(va)
            # code addresses taken as immediates (callbacks, SEH handlers)
            added = 0
            for t in sorted(self.imm_code_refs):
                if t not in self.funcs and t not in self.insn:
                    self.seed(t, "imm_ref")
                    added += 1
            if not added:
                break
        self.rounds = rounds

    def gaps(self) -> list[tuple[int, int]]:
        covered = bytearray(self.img.text_hi - self.img.text_lo)
        lo = self.img.text_lo
        for va, (sz, _, _) in self.insn.items():
            covered[va - lo:va - lo + sz] = b"\1" * sz
        for t, ents in self.jump_tables.items():
            covered[t - lo:t - lo + 4 * len(ents)] = b"\1" * (4 * len(ents))
        out, i, n = [], 0, len(covered)
        while i < n:
            if covered[i]:
                i += 1
                continue
            j = i
            while j < n and not covered[j]:
                j += 1
            out.append((lo + i, lo + j))
            i = j
        self.covered = covered
        return out


PROLOGUES = [b"\x55\x8b\xec", b"\x83\xec", b"\x81\xec", b"\x53\x56\x57", b"\x56\x57", b"\x53\x56"]


def main() -> int:
    d = sys.argv[1] if len(sys.argv) > 1 else "gen/analysis"
    img = Image(d)
    dz = Discovery(img)
    dz.run()

    gaps = dz.gaps()
    pad = unk = 0
    prologue_gaps = []
    for a, b in gaps:
        chunk = img.code(a, b - a)
        if all(c in (0x90, 0xCC, 0x00) for c in chunk):
            pad += b - a
        else:
            unk += b - a
            s = chunk.lstrip(b"\x90\xcc\x00")
            if any(s.startswith(p) for p in PROLOGUES):
                prologue_gaps.append(a + (len(chunk) - len(s)))
    # second pass: explore prologue-looking gaps as functions too
    for va in prologue_gaps:
        dz.seed(va, "gap_prologue")
    dz.run()
    gaps2 = dz.gaps()
    unk2 = sum(b - a for a, b in gaps2
               if not all(c in (0x90, 0xCC, 0x00) for c in img.code(a, b - a)))

    text_size = img.text_hi - img.text_lo
    covered = sum(dz.covered)
    kinds = collections.Counter(f["kind"] for f in dz.funcs.values())
    srcs = collections.Counter()
    for va in dz.funcs:
        for s in dz.seed_src.get(va, ()):
            srcs[s] += 1
    bad = [f for f in dz.funcs.values() if f["bad"]]
    sizes = sorted(f["bytes"] for f in dz.funcs.values())
    callers = collections.Counter()
    for f in dz.funcs.values():
        for c in f["calls"]:
            callers[c] += 1
    import_use = collections.Counter()
    for f in dz.funcs.values():
        for i in f["imports"]:
            import_use[i] += 1

    out = []
    for va in sorted(dz.funcs):
        f = dz.funcs[va]
        out.append({"va": va, "end": f["max"], "bytes": f["bytes"], "insns": f["insns"],
                    "blocks": f["blocks"], "kind": f["kind"],
                    "found_by": sorted(dz.seed_src.get(va, ())),
                    "callers": callers.get(va, 0), "calls": f["calls"],
                    "tail_jumps": f["tail_jumps"], "imports": f["imports"],
                    "jump_tables": f["jump_tables"], "bad": f["bad"]})
    json.dump({"functions": out,
               "jump_tables": {str(k): v for k, v in dz.jump_tables.items()}},
              open(os.path.join(d, "functions.json"), "w"))

    p = print
    p(f".text            {img.text_lo:#x}-{img.text_hi:#x}  ({text_size:,} bytes)")
    p(f"functions        {len(dz.funcs):,}  ({dict(kinds)})   rounds {dz.rounds}")
    p(f"instructions     {len(dz.insn):,}")
    p(f"jump tables      {len(dz.jump_tables):,}  "
      f"({sum(len(v) for v in dz.jump_tables.values()):,} entries)")
    p(f"found by         {dict(srcs)}")
    p(f"coverage         {covered:,} / {text_size:,} bytes = {100 * covered / text_size:.2f}%")
    p(f"gaps             first pass: {len(gaps):,} gaps, padding {pad:,} B, unexplained {unk:,} B")
    p(f"                 after {len(prologue_gaps)} prologue seeds: {len(gaps2):,} gaps, "
      f"unexplained {unk2:,} B")
    p(f"decode problems  {len(bad)} functions ran into undecodable/out-of-text bytes")
    p(f"function size    median {sizes[len(sizes) // 2]} B, largest {sizes[-1]:,} B")
    p("\nmost-called functions")
    for va, n in callers.most_common(10):
        f = dz.funcs.get(va, {})
        p(f"  {va:#010x}  {n:5} callers  {f.get('bytes', 0):6} B")
    p("\nmost-used imports (by number of calling functions)")
    for name, n in import_use.most_common(15):
        p(f"  {n:4}  {name}")
    unused = set(img.iat.values()) - set(import_use)
    p(f"\nimports never called directly: {len(unused)}")
    for u in sorted(unused):
        p(f"  {u}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
