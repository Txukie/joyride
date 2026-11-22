// joyride launcher - BIN/CUE images.
#include "bincue.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace jl {

static long long msf(const std::string &s) {
    int m = 0, sec = 0, f = 0;
    if (std::sscanf(s.c_str(), "%d:%d:%d", &m, &sec, &f) != 3) return -1;
    return ((long long)m * 60 + sec) * 75 + f;
}

// a FILE name in the sheet, found next to the sheet (case-insensitively:
// sheets made on Windows often disagree with the file's case)
static std::string beside(const fs::path &cue, const std::string &name) {
    fs::path dir = cue.parent_path(), p = dir / name;
    if (fs::exists(p)) return p.string();
    std::string want = fs::path(name).filename().string();
    for (auto &c : want) c = (char)std::tolower((unsigned char)c);
    std::error_code ec;
    for (auto &e : fs::directory_iterator(dir.empty() ? "." : dir, ec)) {
        std::string n = e.path().filename().string();
        for (auto &c : n) c = (char)std::tolower((unsigned char)c);
        if (n == want) return e.path().string();
    }
    return p.string();
}

bool CueSheet::open(const std::string &cuePath) {
    tracks_.clear();
    std::ifstream in(cuePath);
    if (!in) { err_ = "cannot open " + cuePath; return false; }
    std::string line, file;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string kw;
        ls >> kw;
        if (kw == "FILE") {
            size_t a = line.find('"'), b = line.rfind('"');
            std::string name = a != std::string::npos && b > a ? line.substr(a + 1, b - a - 1) : "";
            if (name.empty()) ls >> name;
            file = beside(cuePath, name);
        } else if (kw == "TRACK") {
            CueTrack t;
            std::string mode;
            ls >> t.number >> mode;
            t.file = file;
            t.audio = mode == "AUDIO";
            if (mode == "MODE1/2048" || mode == "MODE2/2048") t.sectorSize = 2048, t.dataOffset = 0;
            else if (mode == "MODE1/2352") t.dataOffset = 16;
            else if (mode == "MODE2/2352") t.dataOffset = 24;   // Form 1: sync, header, subheader
            tracks_.push_back(t);
        } else if (kw == "INDEX" && !tracks_.empty()) {
            int n;
            std::string at;
            ls >> n >> at;
            if (n == 1) tracks_.back().start = msf(at);
        }
    }
    if (tracks_.empty()) { err_ = cuePath + ": no tracks"; return false; }
    // lengths: up to the next track in the same file, else the file's end
    for (size_t i = 0; i < tracks_.size(); i++) {
        auto &t = tracks_[i];
        if (i + 1 < tracks_.size() && tracks_[i + 1].file == t.file) {
            t.length = tracks_[i + 1].start - t.start;
        } else {
            std::error_code ec;
            auto size = fs::file_size(t.file, ec);
            if (ec) { err_ = "missing " + t.file; return false; }
            t.length = (long long)(size / (uint64_t)t.sectorSize) - t.start;
        }
        if (t.start < 0 || t.length <= 0) { err_ = cuePath + ": bad track " + std::to_string(t.number); return false; }
    }
    return true;
}

bool CueSheet::writeDataTrack(const std::string &isoPath) {
    for (auto &t : tracks_) {
        if (t.audio) continue;
        std::ifstream in(t.file, std::ios::binary);
        std::ofstream out(isoPath, std::ios::binary);
        if (!in || !out) { err_ = "cannot copy the data track to " + isoPath; return false; }
        in.seekg(t.start * t.sectorSize);
        std::vector<char> s(t.sectorSize);
        for (long long i = 0; i < t.length && in.read(s.data(), t.sectorSize); i++) out.write(s.data() + t.dataOffset, 2048);
        if (!out) { err_ = "cannot write " + isoPath; return false; }
        return true;
    }
    err_ = "no data track";
    return false;
}

static void le32(std::ofstream &o, uint32_t v) { char b[4] = { (char)v, (char)(v >> 8), (char)(v >> 16), (char)(v >> 24) }; o.write(b, 4); }
static void le16(std::ofstream &o, uint16_t v) { char b[2] = { (char)v, (char)(v >> 8) }; o.write(b, 2); }

bool CueSheet::writeAudioTrack(int number, const std::string &wavPath) {
    for (auto &t : tracks_) {
        if (t.number != number || !t.audio) continue;
        std::ifstream in(t.file, std::ios::binary);
        std::ofstream out(wavPath, std::ios::binary);
        if (!in || !out) { err_ = "cannot write " + wavPath; return false; }
        uint32_t bytes = (uint32_t)(t.length * 2352);
        out.write("RIFF", 4), le32(out, 36 + bytes), out.write("WAVEfmt ", 8);
        le32(out, 16), le16(out, 1), le16(out, 2), le32(out, 44100), le32(out, 44100 * 4), le16(out, 4), le16(out, 16);
        out.write("data", 4), le32(out, bytes);
        in.seekg(t.start * 2352);
        std::vector<char> buf(2352 * 75);
        uint64_t left = bytes;
        while (left && in) {
            in.read(buf.data(), (std::streamsize)std::min<uint64_t>(left, buf.size()));
            out.write(buf.data(), in.gcount());
            left -= (uint64_t)in.gcount();
        }
        if (left || !out) { err_ = "short audio track " + std::to_string(number); return false; }
        return true;
    }
    err_ = "no audio track " + std::to_string(number);
    return false;
}

}  // namespace jl
