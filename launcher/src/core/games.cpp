// joyride launcher - the games.
//
// London 1969 and 1961 shipped with executables of their own (1999), the
// 1997 engine with London's files under gtadata\uk\ and a few renames. Their
// mission scripts use nothing the 2002 executable lacks but 1961's drive-by
// switch, so they run on joyride as they are, given:
//   * GTA's GTADATA with London's files over it under GTA's names
//     (missuk.ini -> mission.ini, enguk.fxt -> english.fxt, ...; 1961 adds an
//     'e': missuke.ini, f_logoe0.raw, 0e.wav, ...), the maps kept in uk/
//     where the scripts name them (uk\uk.cmp)
//   * the levels where the 2002 front end looks for them: three cities of two
//     chapters ([1] [2], [102] [103], [202] [203], each city its own
//     style00N.g24) against London's four levels of one city ([1] to [4],
//     1961 one, [1]); the text the front end shows for each slot
//   * 1961's PLAYER_DRIVE_BY / _OFF, which the 2002 parser does not know, as
//     DONOWT (the script's no-op) with a marker joyride acts on
//     (src/platform/driveby.c)
//   * London 1969's music: CD audio tracks 2 to 16, as Music/TrackN.wav.
#include "games.h"

#include "archive.h"
#include "bincue.h"
#include "is5cab.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace jl {

const char *const kGtaExeSha1 = "d36976d66599cd649bf24f8ccd45bda84c0ee3f8";

const std::vector<GameInfo> &games() {
    static const std::vector<GameInfo> g = {
        { "gta1", "Grand Theft Auto", "1997 \xc2\xb7 Liberty City, San Andreas, Vice City",
          "",
          "762e03126a226e92e2f6827ca0750489595b52b2", "", true },
        { "london1969", "GTA: London 1969", "Mission Pack #1 \xc2\xb7 1999", "", "", "gta1", true },
        { "london1961", "GTA: London 1961", "Mission Pack #2 \xc2\xb7 1999 \xc2\xb7 freeware",
          "",
          "f549a46ae1c98b3cd475f5428ab1f2632addd6f9", "london1969", true },
        { "gta2", "Grand Theft Auto 2", "1999 \xc2\xb7 coming later",
          "",
          "8c10c0d20f15912a9450b6fbe028f31928b29893", "", false },
    };
    return g;
}

const GameInfo *game(const std::string &id) {
    for (auto &g : games())
        if (g.id == id) return &g;
    return nullptr;
}

std::string dataRoot() {
    const char *xdg = std::getenv("XDG_DATA_HOME"), *home = std::getenv("HOME");
    if (xdg && *xdg) return std::string(xdg) + "/joyride";
    return std::string(home ? home : ".") + "/.local/share/joyride";
}
std::string gameDir(const std::string &id) { return dataRoot() + "/games/" + id; }

static std::string lower(std::string s) {
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
// an entry of `dir` by name, whatever its case ("" if none)
static fs::path find(const fs::path &dir, const std::string &name) {
    std::error_code ec;
    std::string want = lower(name);
    for (auto &e : fs::directory_iterator(dir, ec))
        if (lower(e.path().filename().string()) == want) return e.path();
    return {};
}
static fs::path findPath(fs::path dir, const std::string &rel) {   // "gtadata/uk", whatever the case
    std::stringstream ss(rel);
    std::string part;
    while (std::getline(ss, part, '/')) {
        if (part.empty()) continue;
        dir = find(dir, part);
        if (dir.empty()) return {};
    }
    return dir;
}
static std::string readFile(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}
static bool writeFile(const fs::path &p, const std::string &s) {
    std::ofstream o(p, std::ios::binary);
    o.write(s.data(), (std::streamsize)s.size());
    return (bool)o;
}
static bool isGtaInstall(const fs::path &d) {
    return !d.empty() && !find(d, "WINO").empty() && !find(find(d, "WINO"), "Grand Theft Auto.exe").empty() &&
           !find(d, "GTADATA").empty();
}

// ---- SHA-1 (for the downloads and the executable) ---------------------------

namespace {
struct Sha1 {
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    uint8_t buf[64];
    uint64_t len = 0;
    static uint32_t rol(uint32_t x, int n) { return x << n | x >> (32 - n); }
    void block(const uint8_t *p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    void add(const uint8_t *p, size_t n) {
        while (n--) {
            buf[len++ % 64] = *p++;
            if (len % 64 == 0) block(buf);
        }
    }
    std::string hex() {
        uint64_t bits = len * 8;
        uint8_t one = 0x80, zero = 0;
        add(&one, 1);
        while (len % 64 != 56) add(&zero, 1);
        for (int i = 7; i >= 0; i--) { uint8_t b = (uint8_t)(bits >> (8 * i)); add(&b, 1); }
        char s[41];
        for (int i = 0; i < 5; i++) std::snprintf(s + 8 * i, 9, "%08x", h[i]);
        return s;
    }
};
}  // namespace

std::string sha1File(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    Sha1 s;
    std::vector<char> b(1 << 20);
    while (in) {
        in.read(b.data(), (std::streamsize)b.size());
        s.add((const uint8_t *)b.data(), (size_t)in.gcount());
    }
    return s.hex();
}

// ---- where each game is -----------------------------------------------------

// gta1 may be an install elsewhere (Steam's): games/gta1/location holds its path
std::string playDir(const std::string &id) {
    fs::path d = gameDir(id);
    if (id == "gta1") {
        std::string loc = readFile(d / "location");
        while (!loc.empty() && (loc.back() == '\n' || loc.back() == '\r')) loc.pop_back();
        if (!loc.empty() && isGtaInstall(loc)) return loc;
        if (isGtaInstall(d / "install")) return (d / "install").string();
        return "";
    }
    if (id == "london1969" || id == "london1961") return isGtaInstall(d / "play") ? (d / "play").string() : "";
    return "";
}
bool installed(const std::string &id) { return !playDir(id).empty(); }

std::vector<std::string> steamGtaInstalls() {
    const char *home = std::getenv("HOME");
    std::vector<std::string> libs, out;
    for (const char *base : { "/.local/share/Steam", "/.steam/steam", "/.var/app/com.valvesoftware.Steam/.local/share/Steam" }) {
        std::string root = std::string(home ? home : "") + base;
        libs.push_back(root);
        std::ifstream vdf(root + "/steamapps/libraryfolders.vdf");
        std::string line;
        std::regex path(R"re("path"\s+"([^"]+)")re");
        std::smatch m;
        while (std::getline(vdf, line))
            if (std::regex_search(line, m, path)) libs.push_back(m[1].str());
    }
    for (auto &l : libs) {
        fs::path p = fs::path(l) / "steamapps" / "common" / "Grand Theft Auto";
        std::error_code ec;
        if (!isGtaInstall(p)) continue;
        std::string c = fs::canonical(p, ec).string();
        if (!ec && std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
    }
    return out;
}

// ---- the FXT text files -----------------------------------------------------

// The text is stored with each byte one up and the first eight also
// 0x63 << i up (mod 256): [key]text\0 ..., ending in an empty key "[]".
static std::string fxtDecode(std::string s) {
    uint8_t k = 0x63;
    for (size_t i = 0; i < s.size(); i++) {
        if (i < 8) s[i] = (char)(s[i] - k), k = (uint8_t)(k << 1);
        s[i] = (char)(s[i] - 1);
    }
    return s;
}
static std::string fxtEncode(std::string s) {
    uint8_t k = 0x63;
    for (size_t i = 0; i < s.size(); i++) {
        s[i] = (char)(s[i] + 1);
        if (i < 8) s[i] = (char)(s[i] + k), k = (uint8_t)(k << 1);
    }
    return s;
}
// the text of `key` ("" if absent)
static std::string fxtGet(const std::string &plain, const std::string &key) {
    size_t p = plain.find("[" + key + "]");
    if (p == std::string::npos) return "";
    p += key.size() + 2;
    size_t e = plain.find('\0', p);
    return plain.substr(p, e == std::string::npos ? std::string::npos : e - p);
}
// keys added (or kept, if there) before the closing "[]"
static bool fxtAdd(const fs::path &file, const std::vector<std::pair<std::string, std::string>> &keyFrom) {
    std::string plain = fxtDecode(readFile(file)), extra;
    size_t end = plain.rfind("[]");
    if (end == std::string::npos) return false;
    for (auto &[key, from] : keyFrom) {
        std::string have = fxtGet(plain, key);
        if (!have.empty() && have.size() > 1) continue;  // 1961 keeps one-letter placeholders: replaced
        if (!have.empty()) {                             // drop the placeholder
            size_t p = plain.find("[" + key + "]"), e = plain.find('\0', p);
            plain.erase(p, e + 1 - p);
            end = plain.rfind("[]");
        }
        std::string text = fxtGet(plain, from);
        if (!text.empty()) extra += "[" + key + "]" + text + std::string(1, '\0');
    }
    plain.insert(end, extra);
    return writeFile(file, fxtEncode(plain));
}

// ---- the mission script ------------------------------------------------------

// [n] sections of a mission.ini, in order
static std::vector<std::pair<int, std::string>> sections(const std::string &ini, std::string &head) {
    std::vector<std::pair<int, std::string>> out;
    std::regex hdr(R"((^|\n)\[(\d+)\][ \t]*\r?\n)");
    auto it = std::sregex_iterator(ini.begin(), ini.end(), hdr);
    size_t prevEnd = 0;
    int prevNum = -1;
    for (; it != std::sregex_iterator(); ++it) {
        size_t at = (size_t)it->position(0) + it->str(1).size();
        if (prevNum < 0) head = ini.substr(0, at);
        else out.push_back({ prevNum, ini.substr(prevEnd, at - prevEnd) });
        prevNum = std::stoi(it->str(2));
        prevEnd = (size_t)(it->position(0) + it->length(0));
    }
    if (prevNum >= 0) out.push_back({ prevNum, ini.substr(prevEnd) });
    else head = ini;
    return out;
}

// London's levels into the 2002 front end's slots; returns the text keys the
// slots need, each with the key whose text it shows
static std::vector<std::pair<std::string, std::string>> fitMissions(const fs::path &file, bool driveBy) {
    std::string head, ini = readFile(file);
    // the drive-by switch as DONOWT with joyride's marker in its last
    // argument (src/platform/driveby.c): 31961 on, 31960 off
    if (driveBy) {
        ini = std::regex_replace(ini, std::regex(R"(\bPLAYER_DRIVE_BY_OFF[ \t]+-?\d+[ \t]+-?\d+[ \t]+-?\d+[ \t]+-?\d+[ \t]+-?\d+)"), "DONOWT 0 0 0 0 31960");
        ini = std::regex_replace(ini, std::regex(R"(\bPLAYER_DRIVE_BY[ \t]+-?\d+[ \t]+-?\d+[ \t]+-?\d+[ \t]+-?\d+[ \t]+-?\d+)"), "DONOWT 0 0 0 0 31961");
    }
    auto secs = sections(ini, head);
    std::map<int, std::string> by;
    for (auto &[n, body] : secs) by[n] = body;
    std::vector<int> levels;                             // London's own, in order
    for (auto &[n, body] : secs)
        if (n < 1000) levels.push_back(n);
    std::map<int, int> slot;                             // the front end's slot -> London's level
    const int slots[] = { 1, 2, 102, 103, 202, 203 };
    for (size_t i = 0; i < 6 && !levels.empty(); i++) slot[slots[i]] = levels[std::min(i, levels.size() - 1)];
    if (levels.size() == 4) slot[202] = levels[2], slot[203] = levels[3];   // the third city: the harder two again
    for (int m : { 1001, 1002, 1003, 1004 })
        for (int city : { 0, 100, 200 })
            if (by.count(m)) slot[m + city] = m;
    std::string out = head;
    std::vector<std::pair<std::string, std::string>> keys;
    for (int n : { 1, 2, 1001, 1002, 1003, 1004, 102, 103, 1101, 1102, 1103, 1104, 202, 203, 1201, 1202, 1203, 1204 }) {
        if (!slot.count(n)) continue;
        if (!out.empty() && out.back() != '\n') out += "\r\n";
        out += "[" + std::to_string(n) + "] \r\n" + by[slot[n]];
        keys.push_back({ "mission" + std::to_string(n), "mission" + std::to_string(slot[n]) });
    }
    writeFile(file, out);
    return keys;
}

// ---- installing ----------------------------------------------------------------

static bool copyTree(const fs::path &from, const fs::path &to, std::string &err) {
    std::error_code ec;
    fs::create_directories(to, ec);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) { err = "cannot copy " + from.string() + ": " + ec.message(); return false; }
    return true;
}

static bool installGta(const fs::path &src, std::string &err, const Log &log, const Progress &progress) {
    fs::path d = gameDir("gta1");
    std::error_code ec;
    fs::create_directories(d, ec);
    if (fs::is_directory(src)) {                         // an install: used where it is
        if (!isGtaInstall(src)) { err = src.string() + " has no WINO/Grand Theft Auto.exe and GTADATA/"; return false; }
        writeFile(d / "location", fs::absolute(src).string() + "\n");
        log("Using the install at " + src.string());
    } else {                                             // the freeware installer
        fs::path tmp = d / "unpack";
        fs::remove_all(tmp, ec);
        log("Unpacking " + src.filename().string());
        if (!extractArchive(src.string(), tmp.string(), err, [&](uint64_t a, uint64_t b) { if (b) progress(0.2 * a / b); })) return false;
        fs::path cab;
        for (auto &e : fs::recursive_directory_iterator(tmp, ec))
            if (lower(e.path().filename().string()) == "data1.cab") cab = e.path();
        if (cab.empty()) { err = src.string() + " holds no InstallShield cabinet"; return false; }
        Is5Cab c;
        if (!c.open(cab.string())) { err = c.error(); return false; }
        fs::path inst = d / "install";
        fs::remove_all(inst, ec);
        log("Installing " + std::to_string(c.files().size()) + " files");
        if (!c.extractAll(inst.string(), [&](uint64_t a, uint64_t b) { if (b) progress(0.2 + 0.8 * a / b); })) { err = c.error(); return false; }
        // the cabinet's folders, lower case from extractAll: GTA's own case back
        for (const char *n : { "WINO", "GTADATA", "Music" }) {
            fs::path p = find(inst, n);
            if (!p.empty() && p.filename() != n) fs::rename(p, inst / n, ec);
        }
        fs::remove_all(tmp, ec);
        fs::remove(d / "location", ec);
        if (!isGtaInstall(inst)) { err = "the installer did not give WINO/ and GTADATA/"; return false; }
    }
    std::string exe = (find(find(playDir("gta1"), "WINO"), "Grand Theft Auto.exe")).string();
    std::string sum = sha1File(exe);
    if (sum != kGtaExeSha1) log("Warning: Grand Theft Auto.exe is not the 2002 release joyride is made from (SHA-1 " + sum + ")");
    progress(1);
    return true;
}

// London 1969's disc: its UK/ folder (what the game reads from the disc), the
// setup's cabinet (what it installs), its music
static bool installLondon69(const fs::path &src, std::string &err, const Log &log, const Progress &progress) {
    fs::path d = gameDir("london1969"), disc = d / "disc";
    std::error_code ec;
    fs::remove_all(disc, ec);
    fs::create_directories(d / "Music", ec);
    std::string ext = lower(src.extension().string());
    if (fs::is_directory(src)) {
        log("Copying the disc's files");
        if (!copyTree(src, disc, err)) return false;
    } else if (ext == ".cue" || ext == ".bin") {
        fs::path cue = src;
        if (ext == ".bin") {                             // its sheet beside it
            cue = find(src.parent_path(), src.stem().string() + ".cue");
            if (cue.empty()) { err = "no .cue beside " + src.string(); return false; }
        }
        CueSheet sheet;
        if (!sheet.open(cue.string())) { err = sheet.error(); return false; }
        fs::path iso = d / "data.iso";
        log("Reading the data track");
        if (!sheet.writeDataTrack(iso.string())) { err = sheet.error(); return false; }
        progress(0.15);
        if (!extractArchive(iso.string(), disc.string(), err)) return false;
        fs::remove(iso, ec);
        int n = 0, audio = 0;
        for (auto &t : sheet.tracks()) audio += t.audio;
        for (auto &t : sheet.tracks()) {
            if (!t.audio) continue;
            log("Music: track " + std::to_string(t.number));
            if (!sheet.writeAudioTrack(t.number, (d / "Music" / ("Track" + std::to_string(t.number - 1) + ".wav")).string())) {
                err = sheet.error();
                return false;
            }
            progress(0.3 + 0.4 * ++n / std::max(1, audio));
        }
    } else {                                             // an ISO
        log("Reading the disc image");
        if (!extractArchive(src.string(), disc.string(), err)) return false;
        log("No music: an ISO has no audio tracks (a BIN/CUE does)");
    }
    fs::path uk = find(disc, "uk"), cab = findPath(disc, "setup/data1.cab");
    if (uk.empty() || cab.empty()) { err = "this is not the London 1969 disc (no UK/ and SETUP/DATA1.CAB)"; return false; }
    Is5Cab c;
    if (!c.open(cab.string())) { err = c.error(); return false; }
    fs::path inst = d / "setup";
    fs::remove_all(inst, ec);
    log("Installing London's files");
    if (!c.extractAll(inst.string(), [&](uint64_t a, uint64_t b) { if (b) progress(0.7 + 0.3 * a / b); })) { err = c.error(); return false; }
    return true;
}

static bool installLondon61(const fs::path &src, std::string &err, const Log &log, const Progress &progress) {
    fs::path d = gameDir("london1961"), tmp = d / "unpack", inst = d / "setup";
    std::error_code ec;
    fs::remove_all(tmp, ec), fs::remove_all(inst, ec);
    log("Unpacking " + src.filename().string());
    if (!extractArchive(src.string(), tmp.string(), err)) return false;
    fs::path cab;
    for (auto &e : fs::recursive_directory_iterator(tmp, ec))
        if (lower(e.path().filename().string()) == "data1.cab") cab = e.path();
    Is5Cab c;
    if (cab.empty() || !c.open(cab.string())) { err = cab.empty() ? "no cabinet in " + src.string() : c.error(); return false; }
    log("Installing London 1961's files");
    if (!c.extractAll(inst.string(), [&](uint64_t a, uint64_t b) { if (b) progress(a / (double)b); })) { err = c.error(); return false; }
    fs::remove_all(tmp, ec);
    return true;
}

bool install(const std::string &id, const std::string &source, std::string &err, const Log &log, const Progress &progress) {
    fs::path src(source);
    if (!fs::exists(src)) { err = source + " does not exist"; return false; }
    if (id == "gta1") {
        if (!installGta(src, err, log, progress)) return false;
        // London is built on GTA's files: again, for the install now used
        for (const char *l : { "london1969", "london1961" })
            if (fs::exists(fs::path(gameDir(l)) / "setup") && !assembleLondon(l, err, log)) return false;
        return true;
    }
    if (id == "london1969") return installLondon69(src, err, log, progress) && assembleLondon(id, err, log);
    if (id == "london1961") return installLondon61(src, err, log, progress) && assembleLondon(id, err, log);
    err = "not installable yet";
    return false;
}

bool uninstall(const std::string &id, std::string &err) {
    std::error_code ec;
    fs::remove_all(gameDir(id), ec);                     // an install used where it is is left alone
    if (ec) { err = ec.message(); return false; }
    return true;
}

// London's files from `from` (a folder like gtadata/uk) into GTADATA under
// GTA's names; `e` is 1961's suffix
static void overlay(const fs::path &from, const fs::path &gtadata, bool e, const Log &log) {
    static const std::map<std::string, std::string> names = {
        { "missuk.ini", "MISSION.INI" }, { "enguk.fxt", "ENGLISH.FXT" }, { "speuk.fxt", "SPECIAL.FXT" },
        { "freuk.fxt", "FRENCH.FXT" }, { "geruk.fxt", "GERMAN.FXT" }, { "itauk.fxt", "ITALIAN.FXT" }, { "japuk.fxt", "JAPANESE.FXT" },
        { "missuke.ini", "MISSION.INI" }, { "enguke.fxt", "ENGLISH.FXT" }, { "speuke.fxt", "SPECIAL.FXT" },
        { "freuke.fxt", "FRENCH.FXT" }, { "geruke.fxt", "GERMAN.FXT" }, { "itauke.fxt", "ITALIAN.FXT" }, { "japuke.fxt", "JAPANESE.FXT" },
        { "player_e.dat", "PLAYER_A.DAT" }, { "cut0e.raw", "CUT0.RAW" }, { "sty001.g24", "STYLE001.G24" },
    };
    std::error_code ec;
    int n = 0;
    for (auto &ent : fs::recursive_directory_iterator(from, ec)) {
        if (!ent.is_regular_file()) continue;
        std::string rel = lower(fs::relative(ent.path(), from).generic_string()), name = ent.path().filename().string();
        std::string l = lower(name);
        fs::path dir = gtadata;
        std::smatch m;
        if (rel.rfind("audio/", 0) == 0) {
            dir = gtadata / "AUDIO";
            if (e && std::regex_match(l, m, std::regex(R"((\d+)e\.wav)"))) name = m[1].str() + ".WAV";
        } else if (rel.find('/') != std::string::npos) {
            continue;                                    // nothing else nests
        } else if (l.size() > 4 && l.substr(l.size() - 4) == ".cmp") {
            dir = gtadata / "uk";                        // where the script names it: uk\uk.cmp
            name = l;
        } else if (names.count(l)) {
            name = names.at(l);
        } else if (std::regex_match(l, m, std::regex(R"(f_logoe(\d)\.raw)"))) {
            name = "F_LOGO" + m[1].str() + ".RAW";
        } else if (std::regex_match(l, m, std::regex(R"(cut0(\d)e\.fon)"))) {
            name = "CUT0" + m[1].str() + ".FON";
        } else {
            name = l;
            for (auto &c : name) c = (char)std::toupper((unsigned char)c);
        }
        fs::create_directories(dir, ec);
        fs::path old = find(dir, name);
        if (!old.empty()) fs::remove(old, ec);           // a link to GTA's, or an earlier copy
        fs::copy_file(ent.path(), dir / name, fs::copy_options::overwrite_existing, ec);
        n++;
    }
    log("  " + std::to_string(n) + " files from " + from.string());
}

bool assembleLondon(const std::string &id, std::string &err, const Log &log) {
    std::string gta = playDir("gta1");
    if (gta.empty()) { err = "Grand Theft Auto is needed first"; return false; }
    fs::path l69 = gameDir("london1969");
    if (!fs::exists(l69 / "setup")) { err = "London 1969 is needed first"; return false; }
    fs::path d = gameDir(id), play = d / "play", data = play / "GTADATA";
    std::error_code ec;
    fs::remove_all(play, ec);
    fs::create_directories(data, ec);
    log("Building " + id + "'s game folder");
    // GTA's executable and data, linked file by file: a linked folder would
    // not do, the game reaches its data as ..\gtadata from WINO, and .. of a
    // linked folder is the real one's parent (GTA's own install)
    fs::create_directories(play / "WINO", ec);
    for (auto &e : fs::directory_iterator(find(gta, "WINO"), ec))
        if (!e.is_directory()) fs::create_symlink(e.path(), play / "WINO" / e.path().filename(), ec);
    if (ec) { err = "cannot link GTA's WINO: " + ec.message(); return false; }
    fs::path gdata = find(gta, "GTADATA");
    for (auto &e : fs::directory_iterator(gdata, ec)) {
        if (e.is_directory()) {                          // AUDIO: a folder of links
            fs::create_directories(data / e.path().filename(), ec);
            for (auto &f : fs::directory_iterator(e.path(), ec)) fs::create_symlink(f.path(), data / e.path().filename() / f.path().filename(), ec);
        } else {
            fs::create_symlink(e.path(), data / e.path().filename(), ec);
        }
    }
    // London 1969: the disc's UK folder, then what its setup installs
    overlay(find(l69 / "disc", "uk"), data, false, log);
    overlay(findPath(l69 / "setup", "gtadata/uk"), data, false, log);
    bool is61 = id == "london1961";
    if (is61) overlay(findPath(d / "setup", "gtadata/uk"), data, true, log);
    // the levels and their names where the 2002 front end looks
    auto keys = fitMissions(data / "MISSION.INI", is61);
    if (!fxtAdd(data / "ENGLISH.FXT", keys)) { err = "London's text file is not as expected"; return false; }
    for (const char *ext : { "G24", "GRY" }) {           // one style for the three cities
        fs::path s = data / (std::string("STYLE001.") + ext);
        if (!fs::exists(s)) continue;
        for (int n : { 2, 3 }) {
            fs::path t = data / ("STYLE00" + std::to_string(n) + "." + ext);
            fs::remove(t, ec);
            fs::create_symlink(s.filename(), t, ec);
        }
    }
    // music: London 1969's
    fs::create_directory_symlink(l69 / "Music", play / "Music", ec);
    if (!isGtaInstall(play)) { err = "the game folder is incomplete"; return false; }
    log("Ready: " + play.string());
    return true;
}

}  // namespace jl
