// joyride launcher - archives through libarchive: the installers'
// self-extracting zips, ISO 9660 images.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace jl {

// every entry of `archive` (any format libarchive reads, a zip with a stub
// in front included) under `dest`, names made lower case; false and `err`
bool extractArchive(const std::string &archive, const std::string &dest, std::string &err,
                    const std::function<void(uint64_t, uint64_t)> &progress = {});

}  // namespace jl
