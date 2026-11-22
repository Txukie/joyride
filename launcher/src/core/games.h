// joyride launcher - the games: where they come from, how they are
// installed, and how London is made to run on GTA's 2002 executable.
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace jl {

using Log = std::function<void(const std::string &)>;
using Progress = std::function<void(double)>;          // 0..1 of the current step

struct GameInfo {
    std::string id;           // gta1, london1969, london1961, gta2
    std::string title, subtitle;
    std::string url;          // the official freeware download, if there is one
    std::string sha1;         // of that download
    std::string needs;        // the game it is an expansion of ("" none)
    bool playable;            // joyride runs it
};
const std::vector<GameInfo> &games();
const GameInfo *game(const std::string &id);

// ~/.local/share/joyride (XDG_DATA_HOME), its games/ folder
std::string dataRoot();
std::string gameDir(const std::string &id);
// what a game is played from (the folder holding WINO/ and GTADATA/), "" if
// not installed; for GTA an install elsewhere may be used where it is
std::string playDir(const std::string &id);
bool installed(const std::string &id);
// GTA installs in Steam's libraries (libraryfolders.vdf), to offer
std::vector<std::string> steamGtaInstalls();

// The 2002 executable joyride is a recompilation of
extern const char *const kGtaExeSha1;
std::string sha1File(const std::string &path);

// Installing. `source` is a downloaded or chosen file, or a folder. Each
// returns false with `err` set; they write under gameDir(id).
//   gta1: the freeware installer (GTAINSTALLER.exe), or an install folder
//         (Steam, or one made by the installer), used where it is
//   london1969: the disc as BIN/CUE or ISO, or a folder (a mounted disc)
//   london1961: its installer (GTAL1961.EXE)
bool install(const std::string &id, const std::string &source, std::string &err, const Log &log, const Progress &progress);
bool uninstall(const std::string &id, std::string &err);

// London's play folder from GTA's install and London's own files: GTA's
// files linked, London's under GTA's names over them, its mission script
// and text fitted to the 2002 executable's levels. Done by install(); again
// when GTA moves.
bool assembleLondon(const std::string &id, std::string &err, const Log &log);

}  // namespace jl
