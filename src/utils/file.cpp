#include "file.hpp"
#include <filesystem>

namespace fs = std::filesystem;

FileInfo::FileInfo(const std::string &path)
    : path(path) {}

void FileInfo::parseFile() {
    fs::path p(path);

    filePath = path;
    fileName = p.filename().string();

    std::string ext = p.extension().string();

    if (!ext.empty() && ext[0] == '.') {
        ext.erase(0, 1);
    }

    mimeType = "video/" + ext;
}