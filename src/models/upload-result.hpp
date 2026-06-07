#pragma once

#include <cstdint>
#include <string>

#include <QString>

struct UploadError {
	int code;
	std::string message;
};

struct UploadChunkResult {
	bool ok = false;
	int httpStatus = 0;

	bool completed = false;

	std::string response;
	std::string sessionUrl;

	uint64_t nextOffset = 0;
	uint64_t uploadedBytes = 0;

	UploadError error;
};

struct OpusUploadResult {
	bool ok;
	std::string projectId;
	int httpStatus;
	UploadError error;
};