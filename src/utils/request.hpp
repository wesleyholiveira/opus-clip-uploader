#pragma once

#include <cstdint>
#include <functional>
#include <string>

using ProgressCallback = std::function<void(int)>;

struct CurlError {
	int code = 0;
	std::string message;
};

struct UploadResult {
	bool ok = false;
	bool completed = false;

	long httpStatus = 0;

	std::string response;
	std::string sessionUrl;

	std::uint64_t uploadedBytes = 0;
	std::uint64_t nextOffset = 0;

	CurlError error;
};

class DriveRequest {
public:
	explicit DriveRequest(std::string accessToken);

	UploadResult uploadFileResumable(const std::string &path, const std::string &fileName,
					 const std::string &mimeType = "application/octet-stream",
					 const std::string &folderId = "", ProgressCallback onProgress = nullptr);

private:
	std::string accessToken;

	UploadResult createResumableSession(const std::string &fileName, const std::string &mimeType,
					    std::uint64_t fileSize, const std::string &folderId);

	UploadResult uploadChunk(const std::string &sessionUrl, const std::string &path, std::uint64_t start,
				 std::uint64_t chunkSize, std::uint64_t totalSize, const std::string &mimeType,
				 ProgressCallback onProgress);

	UploadResult queryUploadStatus(const std::string &sessionUrl, std::uint64_t totalSize);
};
