// joyride launcher - archives through libarchive.
#include "archive.h"

#include <archive.h>
#include <archive_entry.h>

#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace jl {

static std::string lower(std::string s) {
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool extractArchive(const std::string &path, const std::string &dest, std::string &err,
                    const std::function<void(uint64_t, uint64_t)> &progress) {
    struct archive *a = archive_read_new();
    archive_read_support_format_zip_seekable(a);     // reads the central directory: a stub in front is fine
    archive_read_support_format_iso9660(a);
    archive_read_support_format_7zip(a);
    archive_read_support_format_tar(a);
    archive_read_support_filter_all(a);
    if (archive_read_open_filename(a, path.c_str(), 1 << 16) != ARCHIVE_OK) {
        err = path + ": " + archive_error_string(a);
        archive_read_free(a);
        return false;
    }
    std::error_code sec;
    uint64_t total = fs::file_size(path, sec), done = 0;   // bytes out against the file's size: near enough
    struct archive_entry *e;
    std::vector<char> buf(1 << 16);
    bool ok = true;
    int r;
    while ((r = archive_read_next_header(a, &e)) == ARCHIVE_OK) {
        std::string name = archive_entry_pathname(e) ? archive_entry_pathname(e) : "";
        for (auto &c : name) if (c == '\\') c = '/';
        fs::path rel = fs::path(lower(name)).lexically_normal();
        if (name.empty() || rel.is_absolute() || (!rel.empty() && *rel.begin() == "..")) continue;
        fs::path out = fs::path(dest) / rel;
        std::error_code ec;
        if (archive_entry_filetype(e) == AE_IFDIR) { fs::create_directories(out, ec); continue; }
        if (archive_entry_filetype(e) != AE_IFREG) continue;
        fs::create_directories(out.parent_path(), ec);
        std::ofstream o(out, std::ios::binary);
        la_ssize_t n;
        while ((n = archive_read_data(a, buf.data(), buf.size())) > 0) {
            o.write(buf.data(), n);
            done += (uint64_t)n;
        }
        if (n < 0 || !o) { err = "cannot extract " + name + ": " + (n < 0 ? archive_error_string(a) : "write failed"); ok = false; break; }
        if (progress) progress(std::min(done, total), total);
    }
    if (ok && r != ARCHIVE_EOF) { err = path + ": " + archive_error_string(a); ok = false; }
    archive_read_free(a);
    return ok;
}

}  // namespace jl
