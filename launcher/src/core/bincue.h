// joyride launcher - CD images as BIN/CUE: the data track as an ISO 9660
// image, the audio tracks as WAV files.
#pragma once
#include <string>
#include <vector>

namespace jl {

struct CueTrack {
    int number = 0;
    bool audio = false;
    int sectorSize = 2352;    // in the file: 2352 (raw) or 2048
    int dataOffset = 0;       // the 2048 user bytes in a raw data sector: 16 (MODE1), 24 (MODE2)
    std::string file;         // the BIN it is in (full path)
    long long start = 0;      // INDEX 01, in sectors from the start of that file
    long long length = -1;    // in sectors; -1 until known
};

class CueSheet {
public:
    bool open(const std::string &cuePath);
    const std::vector<CueTrack> &tracks() const { return tracks_; }
    // the first data track, as a plain ISO image
    bool writeDataTrack(const std::string &isoPath);
    // audio track `number` as a 44.1 kHz stereo 16-bit WAV
    bool writeAudioTrack(int number, const std::string &wavPath);
    const std::string &error() const { return err_; }

private:
    std::vector<CueTrack> tracks_;
    std::string err_;
};

}  // namespace jl
