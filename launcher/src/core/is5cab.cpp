// joyride launcher - InstallShield 5 cabinets.
//
// The layout (as unshield reads it, checked against the freeware GTA
// installer, two volumes, and London 1961's and the London 1969 disc's, one):
//   common header, 20 bytes: "ISc(", version, volume info, the offset and
//     size of the cabinet descriptor (data1.cab only)
//   volume header, at 0x14: data offset, unknown, first and last file index
//     whose data it holds, then the offset, expanded and packed size of the
//     first file's part (when it began in the volume before) and of the last
//     file's part (when it goes on in the next)
//   cabinet descriptor: +0x0C file table offset (from the descriptor),
//     +0x1C directory count, +0x28 file count
//   file table: u32 offsets (from the table) of the directory names, then of
//     the file descriptors, 0x2A bytes each: name offset, directory index,
//     flags (4 compressed, 8 unused entry), expanded size, packed size,
//     0x14 bytes, data offset (in its volume)
//   a compressed file's data: blocks of a u16 length and raw deflate; a file
//     split between volumes is one stream, its first part then its second.
#include "is5cab.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace jl {

static uint32_t u32(const std::vector<uint8_t> &d, size_t o) {
    if (o + 4 > d.size()) return 0;
    return d[o] | d[o + 1] << 8 | d[o + 2] << 16 | (uint32_t)d[o + 3] << 24;
}
static uint16_t u16(const std::vector<uint8_t> &d, size_t o) {
    if (o + 2 > d.size()) return 0;
    return (uint16_t)(d[o] | d[o + 1] << 8);
}
static std::string cstr(const std::vector<uint8_t> &d, size_t o) {
    std::string s;
    while (o < d.size() && d[o] && s.size() < 260) s += (char)d[o++];
    return s;
}
static std::string lower(std::string s) {
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// dataN.cab beside data1.cab, whatever its case
static std::string sibling(const fs::path &first, int n) {
    std::string want = "data" + std::to_string(n) + ".cab";
    std::error_code ec;
    for (auto &e : fs::directory_iterator(first.parent_path().empty() ? "." : first.parent_path(), ec))
        if (lower(e.path().filename().string()) == want) return e.path().string();
    return "";
}

bool Is5Cab::open(const std::string &path) {
    files_.clear(), vols_.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) { err_ = "cannot open " + path; return false; }
    std::vector<uint8_t> head(0x40);
    in.read((char *)head.data(), (std::streamsize)head.size());
    if (u32(head, 0) != 0x28635349) { err_ = path + " is not an InstallShield cabinet"; return false; }
    uint32_t cd = u32(head, 12), cdsize = u32(head, 16);
    if (!cd || !cdsize || cdsize > (64u << 20)) { err_ = path + ": no cabinet descriptor"; return false; }
    std::vector<uint8_t> desc(0x30);                     // the descriptor, then it and the file table
    in.seekg(cd);
    in.read((char *)desc.data(), (std::streamsize)desc.size());
    uint32_t span = std::max<uint32_t>(cdsize, u32(desc, 0x0C) + u32(desc, 0x14));   // table offset + its size
    if (!in || span > (64u << 20)) { err_ = path + ": short cabinet descriptor"; return false; }
    desc.resize(span);
    in.seekg(cd);
    in.read((char *)desc.data(), (std::streamsize)desc.size());
    if (!in) { err_ = path + ": short cabinet descriptor"; return false; }

    for (int n = 1;; n++) {                              // the volumes
        std::string p = n == 1 ? path : sibling(path, n);
        if (p.empty()) break;
        std::ifstream v(p, std::ios::binary);
        std::vector<uint8_t> h(0x40);
        v.read((char *)h.data(), (std::streamsize)h.size());
        if (!v || u32(h, 0) != 0x28635349) { err_ = p + " is not a cabinet volume"; return false; }
        Volume vol;
        vol.path = p;
        vol.first = u32(h, 0x1C), vol.last = u32(h, 0x20);
        vol.firstOffset = u32(h, 0x24), vol.firstPacked = u32(h, 0x2C);
        vol.lastOffset = u32(h, 0x30), vol.lastPacked = u32(h, 0x38);
        vols_.push_back(vol);
    }

    size_t table = u32(desc, 0x0C);
    uint32_t ndir = u32(desc, 0x1C), nfile = u32(desc, 0x28);
    if (ndir > 4096 || nfile > 65536 || table + 4ull * (ndir + nfile) > desc.size()) { err_ = path + ": bad file table"; return false; }
    std::vector<std::string> dirs;
    for (uint32_t i = 0; i < ndir; i++) dirs.push_back(cstr(desc, table + u32(desc, table + 4 * i)));
    for (uint32_t i = 0; i < nfile; i++) {
        size_t fd = table + u32(desc, table + 4 * (ndir + i));
        if (fd + 0x2A > desc.size()) continue;
        CabFile f;
        f.index = i;
        f.flags = u16(desc, fd + 8);
        if (f.flags & 8) continue;                       // an unused entry
        uint32_t di = u32(desc, fd + 4);
        f.name = cstr(desc, table + u32(desc, fd));
        f.dir = di < dirs.size() ? dirs[di] : "";
        f.size = u32(desc, fd + 10), f.packed = u32(desc, fd + 14), f.offset = u32(desc, fd + 38);
        if (f.name.empty()) continue;
        files_.push_back(f);
    }
    if (files_.empty()) { err_ = path + ": no files"; return false; }
    return true;
}

bool Is5Cab::bytes(const Volume &v, uint32_t offset, uint32_t n, std::vector<uint8_t> &out) {
    std::ifstream in(v.path, std::ios::binary);
    size_t at = out.size();
    out.resize(at + n);
    in.seekg(offset);
    in.read((char *)out.data() + at, n);
    return (bool)in;
}

bool Is5Cab::read(const CabFile &f, std::vector<uint8_t> &out) {
    std::vector<uint8_t> raw;                            // the packed stream, both parts of a split file
    size_t v = 0;
    while (v < vols_.size() && !(f.index >= vols_[v].first && f.index <= vols_[v].last)) v++;
    if (v == vols_.size()) { err_ = f.name + ": in no volume (one missing?)"; return false; }
    bool split = v + 1 < vols_.size() && f.index == vols_[v].last && vols_[v].lastPacked && vols_[v + 1].first == f.index;
    if (split) {
        if (!bytes(vols_[v], vols_[v].lastOffset, vols_[v].lastPacked, raw) ||
            !bytes(vols_[v + 1], vols_[v + 1].firstOffset, vols_[v + 1].firstPacked, raw)) { err_ = f.name + ": short read"; return false; }
    } else if (!bytes(vols_[v], f.offset, f.packed, raw)) {
        err_ = f.name + ": short read";
        return false;
    }
    out.clear();
    if (!(f.flags & 4)) {                                // stored
        out.assign(raw.begin(), raw.begin() + std::min<size_t>(f.size, raw.size()));
        return out.size() == f.size;
    }
    out.reserve(f.size);
    std::vector<uint8_t> buf(65536);
    size_t p = 0;
    while (p + 2 <= raw.size() && out.size() < f.size) {
        uint16_t n = u16(raw, p);
        p += 2;
        if (p + n > raw.size()) break;
        z_stream z{};
        if (inflateInit2(&z, -15) != Z_OK) return false;
        z.next_in = raw.data() + p, z.avail_in = n;
        int r;
        do {
            z.next_out = buf.data(), z.avail_out = (uInt)buf.size();
            r = inflate(&z, Z_NO_FLUSH);
            out.insert(out.end(), buf.data(), buf.data() + (buf.size() - z.avail_out));
        } while (r == Z_OK && (z.avail_in || !z.avail_out));
        inflateEnd(&z);
        if (r != Z_OK && r != Z_STREAM_END && r != Z_BUF_ERROR) break;
        p += n;
    }
    if (out.size() != f.size) { err_ = "damaged file in cabinet: " + f.name; return false; }
    return true;
}

bool Is5Cab::extractAll(const std::string &dest, const std::function<void(uint64_t, uint64_t)> &progress) {
    uint64_t total = 0, done = 0;
    for (auto &f : files_) total += f.size;
    std::vector<uint8_t> buf;
    for (auto &f : files_) {
        std::string dir = lower(f.dir);
        std::replace(dir.begin(), dir.end(), '\\', '/');
        if (dir.find("..") != std::string::npos || f.name.find_first_of("/\\") != std::string::npos ||
            f.name == "." || f.name == "..") { err_ = "unsafe path in cabinet: " + f.dir + "\\" + f.name; return false; }
        fs::path out = fs::path(dest) / dir / f.name;
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);
        if (!read(f, buf)) return false;
        std::ofstream o(out, std::ios::binary);
        o.write((const char *)buf.data(), (std::streamsize)buf.size());
        if (!o) { err_ = "cannot write " + out.string(); return false; }
        done += f.size;
        if (progress) progress(done, total);
    }
    return true;
}

}  // namespace jl
