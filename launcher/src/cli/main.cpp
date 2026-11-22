// joyride-setup: the launcher's installing, from the command line
//   joyride-setup list
//   joyride-setup install <game> <file or folder>
//   joyride-setup assemble <london1969|london1961>
//   joyride-setup cab <data1.cab> <dest>
#include "../core/games.h"
#include "../core/is5cab.h"

#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char **argv) {
    auto log = [](const std::string &s) { std::printf("%s\n", s.c_str()); std::fflush(stdout); };
    auto prog = [](double) {};
    std::string err;
    if (argc >= 2 && !std::strcmp(argv[1], "list")) {
        for (auto &g : jl::games())
            std::printf("%-11s %-28s %s\n", g.id.c_str(), g.title.c_str(), jl::installed(g.id) ? jl::playDir(g.id).c_str() : "-");
        return 0;
    }
    if (argc == 4 && !std::strcmp(argv[1], "install")) {
        if (!jl::install(argv[2], argv[3], err, log, prog)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
        return 0;
    }
    if (argc == 3 && !std::strcmp(argv[1], "assemble")) {
        if (!jl::assembleLondon(argv[2], err, log)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
        return 0;
    }
    if (argc == 4 && !std::strcmp(argv[1], "cab")) {
        jl::Is5Cab c;
        if (!c.open(argv[2]) || !c.extractAll(argv[3])) { std::fprintf(stderr, "error: %s\n", c.error().c_str()); return 1; }
        std::printf("%zu files\n", c.files().size());
        return 0;
    }
    std::fprintf(stderr, "usage: joyride-setup list | install <game> <source> | assemble <london1969|london1961> | cab <data1.cab> <dest>\n");
    return 64;
}
