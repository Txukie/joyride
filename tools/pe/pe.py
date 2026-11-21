"""Minimal, dependency-free PE32 parser for joyride.

Parses headers, sections, data directories, imports, relocations, exports
and the resource directory of a 32-bit Windows executable, and can map the
image into a flat buffer laid out exactly as the Windows loader would.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

DIR_NAMES = [
    "export", "import", "resource", "exception", "security", "basereloc",
    "debug", "architecture", "globalptr", "tls", "load_config",
    "bound_import", "iat", "delay_import", "com_descriptor", "reserved",
]

SCN_CODE = 0x00000020
SCN_IDATA = 0x00000040
SCN_UDATA = 0x00000080
SCN_EXEC = 0x20000000
SCN_READ = 0x40000000
SCN_WRITE = 0x80000000


class PEError(Exception):
    pass


@dataclass
class Section:
    name: str
    vaddr: int          # RVA
    vsize: int
    raw_off: int
    raw_size: int
    flags: int

    @property
    def perms(self) -> str:
        return ("r" if self.flags & SCN_READ else "-") + \
               ("w" if self.flags & SCN_WRITE else "-") + \
               ("x" if self.flags & SCN_EXEC else "-")

    def contains_rva(self, rva: int) -> bool:
        return self.vaddr <= rva < self.vaddr + max(self.vsize, self.raw_size)


@dataclass
class Import:
    dll: str
    name: str | None     # None when imported by ordinal
    ordinal: int | None
    hint: int | None
    iat_va: int          # VA of the IAT slot the game calls through


@dataclass
class Export:
    ordinal: int
    name: str | None
    rva: int
    forwarder: str | None = None


@dataclass
class PE:
    data: bytes
    machine: int
    timestamp: int
    characteristics: int
    image_base: int
    entry_rva: int
    section_align: int
    file_align: int
    size_of_image: int
    size_of_headers: int
    subsystem: int
    linker: tuple[int, int]
    os_version: tuple[int, int]
    stack: tuple[int, int]      # reserve, commit
    heap: tuple[int, int]
    checksum: int
    dirs: list[tuple[int, int]]
    sections: list[Section] = field(default_factory=list)
    imports: list[Import] = field(default_factory=list)
    exports: list[Export] = field(default_factory=list)
    relocs: list[int] = field(default_factory=list)        # VAs of HIGHLOW fixups
    reloc_other: list[tuple[int, int]] = field(default_factory=list)  # (type, va)
    resources: list[tuple[str, str, int, int]] = field(default_factory=list)

    # --- address helpers -------------------------------------------------
    def section_for_rva(self, rva: int) -> Section | None:
        for s in self.sections:
            if s.contains_rva(rva):
                return s
        return None

    def rva_to_off(self, rva: int) -> int:
        if rva < self.size_of_headers:
            return rva
        s = self.section_for_rva(rva)
        if s is None or rva - s.vaddr >= s.raw_size:
            raise PEError(f"RVA {rva:#x} has no file backing")
        return s.raw_off + (rva - s.vaddr)

    def read(self, rva: int, n: int) -> bytes:
        o = self.rva_to_off(rva)
        return self.data[o:o + n]

    def u16(self, rva: int) -> int:
        return struct.unpack_from("<H", self.data, self.rva_to_off(rva))[0]

    def u32(self, rva: int) -> int:
        return struct.unpack_from("<I", self.data, self.rva_to_off(rva))[0]

    def cstr(self, rva: int, limit: int = 512) -> str:
        o = self.rva_to_off(rva)
        end = self.data.find(b"\0", o, o + limit)
        return self.data[o:end].decode("latin-1")

    def map_image(self) -> bytearray:
        """Lay the file out in memory as the Windows loader would (no fixups,
        imports unresolved). Index 0 corresponds to image_base."""
        img = bytearray(self.size_of_image)
        img[:self.size_of_headers] = self.data[:self.size_of_headers]
        for s in self.sections:
            n = min(s.raw_size, s.vsize) if s.vsize else s.raw_size
            img[s.vaddr:s.vaddr + n] = self.data[s.raw_off:s.raw_off + n]
        return img


def parse(data: bytes) -> PE:
    if data[:2] != b"MZ":
        raise PEError("not an MZ executable")
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        raise PEError("no PE signature")
    coff = e_lfanew + 4
    machine, nsec, ts, _, _, opt_size, chars = struct.unpack_from("<HHIIIHH", data, coff)
    opt = coff + 20
    if struct.unpack_from("<H", data, opt)[0] != 0x10B:
        raise PEError("not PE32 (expected optional header magic 0x10b)")
    lmaj, lmin = data[opt + 2], data[opt + 3]
    entry, = struct.unpack_from("<I", data, opt + 16)
    image_base, salign, falign = struct.unpack_from("<III", data, opt + 28)
    osmaj, osmin = struct.unpack_from("<HH", data, opt + 40)
    size_img, size_hdr, csum = struct.unpack_from("<III", data, opt + 56)
    subsys, = struct.unpack_from("<H", data, opt + 68)
    sres, scom, hres, hcom = struct.unpack_from("<IIII", data, opt + 72)
    ndirs, = struct.unpack_from("<I", data, opt + 92)
    dirs = [struct.unpack_from("<II", data, opt + 96 + 8 * i) for i in range(min(ndirs, 16))]

    pe = PE(data, machine, ts, chars, image_base, entry, salign, falign,
            size_img, size_hdr, subsys, (lmaj, lmin), (osmaj, osmin),
            (sres, scom), (hres, hcom), csum, dirs)

    sh = opt + opt_size
    for i in range(nsec):
        name, vsize, va, rsize, roff, _, _, _, _, fl = struct.unpack_from("<8sIIIIIIHHI", data, sh + 40 * i)
        pe.sections.append(Section(name.rstrip(b"\0").decode("latin-1"), va, vsize, roff, rsize, fl))

    _parse_imports(pe)
    _parse_exports(pe)
    _parse_relocs(pe)
    _parse_resources(pe)
    return pe


def _parse_imports(pe: PE) -> None:
    rva, size = pe.dirs[1]
    if not rva:
        return
    while True:
        ilt, _, _, name_rva, iat = struct.unpack("<IIIII", pe.read(rva, 20))
        if not (ilt or name_rva or iat):
            break
        dll = pe.cstr(name_rva)
        lookup = ilt or iat          # Borland-style images have no ILT
        i = 0
        while True:
            entry = pe.u32(lookup + 4 * i)
            if entry == 0:
                break
            slot = pe.image_base + iat + 4 * i
            if entry & 0x80000000:
                pe.imports.append(Import(dll, None, entry & 0xFFFF, None, slot))
            else:
                pe.imports.append(Import(dll, pe.cstr(entry + 2), None, pe.u16(entry), slot))
            i += 1
        rva += 20


def _parse_exports(pe: PE) -> None:
    rva, size = pe.dirs[0]
    if not rva:
        return
    (_, _, _, _, _, base, nfuncs, nnames,
     funcs, names, ords) = struct.unpack("<IIHHIIIIIII", pe.read(rva, 40))
    by_index: dict[int, str] = {}
    for i in range(nnames):
        by_index[pe.u16(ords + 2 * i)] = pe.cstr(pe.u32(names + 4 * i))
    for i in range(nfuncs):
        frva = pe.u32(funcs + 4 * i)
        if not frva:
            continue
        fwd = pe.cstr(frva) if rva <= frva < rva + size else None
        pe.exports.append(Export(base + i, by_index.get(i), frva, fwd))


def _parse_relocs(pe: PE) -> None:
    rva, size = pe.dirs[5]
    end = rva + size
    while rva < end:
        page, block = struct.unpack("<II", pe.read(rva, 8))
        if block < 8:
            break
        for j in range((block - 8) // 2):
            e = pe.u16(rva + 8 + 2 * j)
            typ, off = e >> 12, e & 0xFFF
            va = pe.image_base + page + off
            if typ == 3:             # IMAGE_REL_BASED_HIGHLOW
                pe.relocs.append(va)
            elif typ != 0:           # 0 = ABSOLUTE padding
                pe.reloc_other.append((typ, va))
        rva += block
    pe.relocs.sort()


RT_NAMES = {1: "CURSOR", 2: "BITMAP", 3: "ICON", 4: "MENU", 5: "DIALOG",
            6: "STRING", 9: "ACCELERATOR", 10: "RCDATA", 12: "GROUP_CURSOR",
            14: "GROUP_ICON", 16: "VERSION", 24: "MANIFEST"}


def _parse_resources(pe: PE) -> None:
    base, size = pe.dirs[2]
    if not base:
        return

    def name_of(raw: int, level: int) -> str:
        if raw & 0x80000000:
            o = base + (raw & 0x7FFFFFFF)
            n = pe.u16(o)
            return pe.read(o + 2, 2 * n).decode("utf-16-le")
        return RT_NAMES.get(raw, str(raw)) if level == 0 else str(raw)

    def walk(off: int, level: int, path: list[str]) -> None:
        _, _, _, _, nnamed, nid = struct.unpack("<IIHHHH", pe.read(base + off, 16))
        for k in range(nnamed + nid):
            nm, child = struct.unpack("<II", pe.read(base + off + 16 + 8 * k, 8))
            p = path + [name_of(nm, level)]
            if child & 0x80000000:
                walk(child & 0x7FFFFFFF, level + 1, p)
            else:
                drva, dsize, _, _ = struct.unpack("<IIII", pe.read(base + child, 16))
                pe.resources.append((p[0], "/".join(p[1:]), drva, dsize))

    walk(0, 0, [])
