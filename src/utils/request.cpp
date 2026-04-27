#include "utils/request.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace {

using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
using HeaderList = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;

constexpr std::uint64_t DefaultChunkSize = 8 * 1024 * 1024; // 8 MB, multiple of 256 KB.

struct HeaderCapture {
	std::string location;
	std::string range;
};

struct ReadContext {
	std::ifstream *file = nullptr;
	std::uint64_t remaining = 0;
};

struct ProgressContext {
	std::uint64_t chunkStart = 0;
	std::uint64_t totalSize = 0;
	ProgressCallback onProgress;
};

std::optional<std::uint64_t> getFileSize(const std::string &path)
{
	std::ifstream file(path, std::ios::binary | std::ios::ate);

	if (!file) {
		return std::nullopt;
	}

	const std::streampos size = file.tellg();

	if (size < 0) {
		return std::nullopt;
	}

	return static_cast<std::uint64_t>(size);
}

std::string jsonEscape(const std::string &value)
{
	std::string escaped;
	escaped.reserve(value.size());

	for (char ch : value) {
		switch (ch) {
		case '"':
			escaped += "\\\"";
			break;
		case '\\':
			escaped += "\\\\";
			break;
		case '\n':
			escaped += "\\n";
			break;
		case '\r':
			escaped += "\\r";
			break;
		case '\t':
			escaped += "\\t";
			break;
		default:
			escaped += ch;
			break;
		}
	}

	return escaped;
}

std::string trim(std::string value)
{
	while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ')) {
		value.pop_back();
	}

	std::size_t start = 0;

	while (start < value.size() && value[start] == ' ') {
		++start;
	}

	return value.substr(start);
}

bool startsWithCaseInsensitive(const std::string &value, const std::string &prefix)
{
	if (value.size() < prefix.size()) {
		return false;
	}

	for (std::size_t i = 0; i < prefix.size(); ++i) {
		const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(value[i])));
		const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(prefix[i])));

		if (a != b) {
			return false;
		}
	}

	return true;
}

std::optional<std::uint64_t> parseNextOffsetFromRange(const std::string &rangeHeader)
{
	// Example: Range: bytes=0-1048575 => next offset is 1048576.
	const std::string prefix = "bytes=";
	const std::size_t prefixPos = rangeHeader.find(prefix);

	if (prefixPos == std::string::npos) {
		return std::nullopt;
	}

	const std::size_t dashPos = rangeHeader.find('-', prefixPos + prefix.size());

	if (dashPos == std::string::npos) {
		return std::nullopt;
	}

	std::string endStr = rangeHeader.substr(dashPos + 1);
	endStr = trim(endStr);

	if (endStr.empty()) {
		return std::nullopt;
	}

	try {
		const std::uint64_t lastReceived = static_cast<std::uint64_t>(std::stoull(endStr));
		return lastReceived + 1;
	} catch (...) {
		return std::nullopt;
	}
}

size_t writeCallback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	auto *response = static_cast<std::string *>(userdata);
	const size_t totalSize = size * nmemb;
	response->append(ptr, totalSize);
	return totalSize;
}

size_t headerCallback(char *buffer, size_t size, size_t nitems, void *userdata)
{
	const size_t totalSize = size * nitems;
	auto *headers = static_cast<HeaderCapture *>(userdata);
	std::string line(buffer, totalSize);

	if (startsWithCaseInsensitive(line, "Location:")) {
		headers->location = trim(line.substr(std::string("Location:").size()));
	}

	if (startsWithCaseInsensitive(line, "Range:")) {
		headers->range = trim(line.substr(std::string("Range:").size()));
	}

	return totalSize;
}

size_t readChunkCallback(char *buffer, size_t size, size_t nitems, void *userdata)
{
	auto *context = static_cast<ReadContext *>(userdata);

	if (!context || !context->file || context->remaining == 0) {
		return 0;
	}

	const std::uint64_t maxBytes = static_cast<std::uint64_t>(size * nitems);
	const std::uint64_t bytesToRead = std::min(maxBytes, context->remaining);

	context->file->read(buffer, static_cast<std::streamsize>(bytesToRead));
	const auto bytesRead = static_cast<std::uint64_t>(context->file->gcount());

	context->remaining -= bytesRead;
	return static_cast<size_t>(bytesRead);
}

int progressCallback(void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t ulnow)
{
	auto *context = static_cast<ProgressContext *>(clientp);

	if (!context || context->totalSize == 0) {
		return 0;
	}

	const auto uploadedSoFar = context->chunkStart + static_cast<std::uint64_t>(ulnow);
	const int progress = static_cast<int>((uploadedSoFar * 100) / context->totalSize);

	if (context->onProgress) {
		context->onProgress(progress);
	}

	return 0;
}

curl_slist *appendHeader(curl_slist *headers, const std::string &header)
{
	return curl_slist_append(headers, header.c_str());
}

UploadResult curlFailure(CURLcode code, const char *errorBuffer)
{
	std::string message;

	if (errorBuffer && errorBuffer[0] != '\0') {
		message = errorBuffer;
	} else {
		message = curl_easy_strerror(code);
	}

	return {false, false, 0, {}, {}, 0, 0, {static_cast<int>(code), message}};
}

UploadResult simpleError(const std::string &message, int code = -1)
{
	return {false, false, 0, {}, {}, 0, 0, {code, message}};
}

} // namespace

DriveRequest::DriveRequest(std::string accessToken) : accessToken(std::move(accessToken)) {}

UploadResult DriveRequest::createResumableSession(const std::string &fileName, const std::string &mimeType,
						  std::uint64_t fileSize, const std::string &folderId)
{
	CurlHandle curl(curl_easy_init(), curl_easy_cleanup);

	if (!curl) {
		return simpleError("Failed to initialize CURL");
	}

	CURL *ch = curl.get();
	std::string response;
	HeaderCapture responseHeaders;
	char errorBuffer[CURL_ERROR_SIZE] = {};

	const std::string url = "https://www.googleapis.com/upload/drive/v3/files?uploadType=resumable";

	std::string metadata = "{\"name\":\"" + jsonEscape(fileName) + "\"";
	if (!folderId.empty()) {
		metadata += ",\"parents\":[\"" + jsonEscape(folderId) + "\"]";
	}
	metadata += "}";

	curl_slist *rawHeaders = nullptr;
	rawHeaders = appendHeader(rawHeaders, "Authorization: Bearer " + accessToken);
	rawHeaders = appendHeader(rawHeaders, "Content-Type: application/json; charset=UTF-8");
	rawHeaders = appendHeader(rawHeaders, "X-Upload-Content-Type: " + mimeType);
	rawHeaders = appendHeader(rawHeaders, "X-Upload-Content-Length: " + std::to_string(fileSize));

	if (!rawHeaders) {
		return simpleError("Failed to create CURL headers");
	}

	HeaderList headers(rawHeaders, curl_slist_free_all);

	curl_easy_setopt(ch, CURLOPT_ERRORBUFFER, errorBuffer);
	curl_easy_setopt(ch, CURLOPT_URL, url.c_str());
	curl_easy_setopt(ch, CURLOPT_HTTPHEADER, headers.get());
	curl_easy_setopt(ch, CURLOPT_POST, 1L);
	curl_easy_setopt(ch, CURLOPT_POSTFIELDS, metadata.c_str());
	curl_easy_setopt(ch, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(metadata.size()));
	curl_easy_setopt(ch, CURLOPT_WRITEFUNCTION, writeCallback);
	curl_easy_setopt(ch, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(ch, CURLOPT_HEADERFUNCTION, headerCallback);
	curl_easy_setopt(ch, CURLOPT_HEADERDATA, &responseHeaders);

	CURLcode result = curl_easy_perform(ch);

	if (result != CURLE_OK) {
		return curlFailure(result, errorBuffer);
	}

	long httpStatus = 0;
	curl_easy_getinfo(ch, CURLINFO_RESPONSE_CODE, &httpStatus);

	if (httpStatus < 200 || httpStatus >= 300) {
		return {false,
			false,
			httpStatus,
			response,
			{},
			0,
			0,
			{static_cast<int>(httpStatus),
			 "Failed to create resumable upload session. HTTP status " + std::to_string(httpStatus)}};
	}

	if (responseHeaders.location.empty()) {
		return {false,      false,
			httpStatus, response,
			{},         0,
			0,          {-1, "Google Drive did not return a resumable session Location header"}};
	}

	return {true, false, httpStatus, response, responseHeaders.location, 0, 0, {}};
}

UploadResult DriveRequest::queryUploadStatus(const std::string &sessionUrl, std::uint64_t totalSize)
{
	CurlHandle curl(curl_easy_init(), curl_easy_cleanup);

	if (!curl) {
		return simpleError("Failed to initialize CURL");
	}

	CURL *ch = curl.get();
	std::string response;
	HeaderCapture responseHeaders;
	char errorBuffer[CURL_ERROR_SIZE] = {};

	curl_slist *rawHeaders = nullptr;
	rawHeaders = appendHeader(rawHeaders, "Content-Range: bytes */" + std::to_string(totalSize));
	rawHeaders = appendHeader(rawHeaders, "Content-Length: 0");

	if (!rawHeaders) {
		return simpleError("Failed to create status query headers");
	}

	HeaderList headers(rawHeaders, curl_slist_free_all);

	curl_easy_setopt(ch, CURLOPT_ERRORBUFFER, errorBuffer);
	curl_easy_setopt(ch, CURLOPT_URL, sessionUrl.c_str());
	curl_easy_setopt(ch, CURLOPT_HTTPHEADER, headers.get());
	curl_easy_setopt(ch, CURLOPT_CUSTOMREQUEST, "PUT");
	curl_easy_setopt(ch, CURLOPT_POSTFIELDS, "");
	curl_easy_setopt(ch, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(0));
	curl_easy_setopt(ch, CURLOPT_WRITEFUNCTION, writeCallback);
	curl_easy_setopt(ch, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(ch, CURLOPT_HEADERFUNCTION, headerCallback);
	curl_easy_setopt(ch, CURLOPT_HEADERDATA, &responseHeaders);

	CURLcode result = curl_easy_perform(ch);

	if (result != CURLE_OK) {
		return curlFailure(result, errorBuffer);
	}

	long httpStatus = 0;
	curl_easy_getinfo(ch, CURLINFO_RESPONSE_CODE, &httpStatus);

	if (httpStatus == 200 || httpStatus == 201) {
		return {true, true, httpStatus, response, sessionUrl, totalSize, totalSize, {}};
	}

	if (httpStatus == 308) {
		std::uint64_t nextOffset = 0;

		if (!responseHeaders.range.empty()) {
			auto parsedNextOffset = parseNextOffsetFromRange(responseHeaders.range);
			if (parsedNextOffset.has_value()) {
				nextOffset = parsedNextOffset.value();
			}
		}

		return {true, false, httpStatus, response, sessionUrl, nextOffset, nextOffset, {}};
	}

	return {false,
		false,
		httpStatus,
		response,
		sessionUrl,
		0,
		0,
		{static_cast<int>(httpStatus),
		 "Failed to query resumable upload status. HTTP status " + std::to_string(httpStatus)}};
}

UploadResult DriveRequest::uploadChunk(const std::string &sessionUrl, const std::string &path, std::uint64_t start,
				       std::uint64_t chunkSize, std::uint64_t totalSize, const std::string &mimeType,
				       ProgressCallback onProgress)
{
	std::ifstream file(path, std::ios::binary);

	if (!file) {
		return {false, false, 0, {}, sessionUrl, 0, start, {-1, "Failed to open file: " + path}};
	}

	file.seekg(static_cast<std::streamoff>(start), std::ios::beg);

	if (!file) {
		return {false,      false, 0,     {},
			sessionUrl, 0,     start, {-1, "Failed to seek file to offset " + std::to_string(start)}};
	}

	const std::uint64_t remaining = totalSize - start;
	const std::uint64_t currentChunkSize = std::min(chunkSize, remaining);
	const std::uint64_t end = start + currentChunkSize - 1;

	ReadContext readContext{&file, currentChunkSize};
	ProgressContext progressContext{start, totalSize, onProgress};

	CurlHandle curl(curl_easy_init(), curl_easy_cleanup);

	if (!curl) {
		return {false, false, 0, {}, sessionUrl, 0, start, {-1, "Failed to initialize CURL"}};
	}

	CURL *ch = curl.get();
	std::string response;
	HeaderCapture responseHeaders;
	char errorBuffer[CURL_ERROR_SIZE] = {};

	const std::string contentRange = "Content-Range: bytes " + std::to_string(start) + "-" + std::to_string(end) +
					 "/" + std::to_string(totalSize);

	curl_slist *rawHeaders = nullptr;
	rawHeaders = appendHeader(rawHeaders, "Content-Type: " + mimeType);
	rawHeaders = appendHeader(rawHeaders, contentRange);
	rawHeaders = appendHeader(rawHeaders, "Expect:");

	if (!rawHeaders) {
		return {false, false, 0, {}, sessionUrl, 0, start, {-1, "Failed to create upload chunk headers"}};
	}

	HeaderList headers(rawHeaders, curl_slist_free_all);

	curl_easy_setopt(ch, CURLOPT_ERRORBUFFER, errorBuffer);
	curl_easy_setopt(ch, CURLOPT_URL, sessionUrl.c_str());
	curl_easy_setopt(ch, CURLOPT_HTTPHEADER, headers.get());
	curl_easy_setopt(ch, CURLOPT_UPLOAD, 1L);
	curl_easy_setopt(ch, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(currentChunkSize));
	curl_easy_setopt(ch, CURLOPT_READFUNCTION, readChunkCallback);
	curl_easy_setopt(ch, CURLOPT_READDATA, &readContext);
	curl_easy_setopt(ch, CURLOPT_WRITEFUNCTION, writeCallback);
	curl_easy_setopt(ch, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(ch, CURLOPT_HEADERFUNCTION, headerCallback);
	curl_easy_setopt(ch, CURLOPT_HEADERDATA, &responseHeaders);
	curl_easy_setopt(ch, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(ch, CURLOPT_XFERINFOFUNCTION, progressCallback);
	curl_easy_setopt(ch, CURLOPT_XFERINFODATA, &progressContext);

	CURLcode result = curl_easy_perform(ch);

	if (result != CURLE_OK) {
		return curlFailure(result, errorBuffer);
	}

	long httpStatus = 0;
	curl_easy_getinfo(ch, CURLINFO_RESPONSE_CODE, &httpStatus);

	if (httpStatus == 200 || httpStatus == 201) {
		if (onProgress) {
			onProgress(100);
		}
		return {true, true, httpStatus, response, sessionUrl, totalSize, totalSize, {}};
	}

	if (httpStatus == 308) {
		std::uint64_t nextOffset = end + 1;

		if (!responseHeaders.range.empty()) {
			auto parsedNextOffset = parseNextOffsetFromRange(responseHeaders.range);
			if (parsedNextOffset.has_value()) {
				nextOffset = parsedNextOffset.value();
			}
		}

		if (onProgress && totalSize > 0) {
			onProgress(static_cast<int>((nextOffset * 100) / totalSize));
		}

		return {true, false, httpStatus, response, sessionUrl, nextOffset, nextOffset, {}};
	}

	return {false,
		false,
		httpStatus,
		response,
		sessionUrl,
		0,
		start,
		{static_cast<int>(httpStatus), "Upload chunk failed. HTTP status " + std::to_string(httpStatus)}};
}

UploadResult DriveRequest::uploadFileResumable(const std::string &path, const std::string &fileName,
					       const std::string &mimeType, const std::string &folderId,
					       ProgressCallback onProgress)
{
	const auto fileSizeResult = getFileSize(path);

	if (!fileSizeResult.has_value()) {
		return {false, false, 0, {}, {}, 0, 0, {-1, "Could not determine file size: " + path}};
	}

	const std::uint64_t fileSize = fileSizeResult.value();

	if (onProgress) {
		onProgress(0);
	}

	UploadResult sessionResult = createResumableSession(fileName, mimeType, fileSize, folderId);

	if (!sessionResult.ok) {
		return sessionResult;
	}

	const std::string uploadSessionUrl = sessionResult.sessionUrl;
	std::uint64_t offset = 0;

	while (offset < fileSize) {
		UploadResult chunkResult =
			uploadChunk(uploadSessionUrl, path, offset, DefaultChunkSize, fileSize, mimeType, onProgress);

		if (chunkResult.ok && chunkResult.completed) {
			if (onProgress) {
				onProgress(100);
			}
			return chunkResult;
		}

		if (chunkResult.ok && !chunkResult.completed) {
			offset = chunkResult.nextOffset;

			if (onProgress && fileSize > 0) {
				onProgress(static_cast<int>((offset * 100) / fileSize));
			}

			continue;
		}

		if (chunkResult.httpStatus == 0 || chunkResult.httpStatus >= 500) {
			UploadResult statusResult = queryUploadStatus(uploadSessionUrl, fileSize);

			if (statusResult.ok && statusResult.completed) {
				if (onProgress) {
					onProgress(100);
				}
				return statusResult;
			}

			if (statusResult.ok && !statusResult.completed) {
				offset = statusResult.nextOffset;

				if (onProgress && fileSize > 0) {
					onProgress(static_cast<int>((offset * 100) / fileSize));
				}

				continue;
			}

			return statusResult;
		}

		return chunkResult;
	}

	if (onProgress) {
		onProgress(100);
	}

	return {true, true, 200, {}, uploadSessionUrl, fileSize, fileSize, {}};
}
