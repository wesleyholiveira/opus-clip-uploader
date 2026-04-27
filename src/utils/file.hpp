#pragma once

#include <string>

class FileInfo {
public:
    explicit FileInfo(const std::string &path);

    void parseFile();

    std::string filePath;
    std::string fileName;
    std::string mimeType;

private:
    std::string path;
};