// joyride launcher - InstallShield 5 cabinets: data1.cab (its header inside)
// and any further volumes data2.cab, ... beside it, as the 2002 GTA and
// London installers have them.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace jl {

struct CabFile {
    uint32_t index = 0;       // in the file table: what the volumes' ranges count
    std::string dir;          // as stored, '\\' separated ("GTADATA\\AUDIO"), may be empty
    std::string name;
    uint32_t size = 0;        // expanded
    uint32_t packed = 0;
    uint32_t offset = 0;      // of its data in its volume
    uint16_t flags = 0;
};

class Is5Cab {
public:
    // data1.cab; false (and error() says why) if it is not an IS5 cabinet
    bool open(const std::string &path);
    const std::vector<CabFile> &files() const { return files_; }
    bool read(const CabFile &f, std::vector<uint8_t> &out);
    // every file under `dest`; directories made lower case ("gtadata/audio"),
    // names kept; progress(done, total) in bytes
    bool extractAll(const std::string &dest, const std::function<void(uint64_t, uint64_t)> &progress = {});
    const std::string &error() const { return err_; }

private:
    struct Volume {
        std::string path;
        uint32_t first = 0, last = 0;                    // the files whose data it holds
        uint32_t firstOffset = 0, firstPacked = 0;       // the start's share of a file split with the volume before
        uint32_t lastOffset = 0, lastPacked = 0;         // the end's share of one split with the next
    };
    bool bytes(const Volume &v, uint32_t offset, uint32_t n, std::vector<uint8_t> &out);
    std::vector<Volume> vols_;
    std::vector<CabFile> files_;
    std::string err_;
};

}  // namespace jl
