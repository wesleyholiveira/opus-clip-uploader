#pragma once

#include <string>


struct UploadError {
	int code;
	std::string message;
};

struct OpusUploadResult {
	bool ok;
	std::string projectId;
	int httpStatus;
	UploadError error;
};
