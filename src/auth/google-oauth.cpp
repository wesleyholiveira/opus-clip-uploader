#include "auth/google-oauth.hpp"

#include <curl/curl.h>

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>

#include <string>

namespace {

size_t writeCallback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	auto *response = static_cast<std::string *>(userdata);

	const size_t totalSize = size * nmemb;
	response->append(ptr, totalSize);

	return totalSize;
}

QString urlEncode(const QString &value)
{
	return QString::fromUtf8(QUrl::toPercentEncoding(value));
}

TokenResult parseTokenResponse(const std::string &response, long httpStatus)
{
	TokenResult result;
	result.rawResponse = QString::fromStdString(response);

	const QByteArray jsonBytes = QByteArray::fromStdString(response);

	QJsonParseError parseError{};
	const QJsonDocument document = QJsonDocument::fromJson(jsonBytes, &parseError);

	if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
		result.ok = false;
		result.error = QString("Invalid token JSON response. HTTP: %1. Parse error: %2. Raw: %3")
				       .arg(httpStatus)
				       .arg(parseError.errorString())
				       .arg(result.rawResponse);
		return result;
	}

	const QJsonObject object = document.object();

	if (httpStatus < 200 || httpStatus >= 300) {
		const QString error = object.value("error").toString();
		const QString description = object.value("error_description").toString();

		result.ok = false;
		result.error = QString("OAuth token request failed. HTTP: %1. Error: %2. Description: %3")
				       .arg(httpStatus)
				       .arg(error)
				       .arg(description);

		return result;
	}

	result.accessToken = object.value("access_token").toString();
	result.refreshToken = object.value("refresh_token").toString();
	result.tokenType = object.value("token_type").toString();
	result.scope = object.value("scope").toString();
	result.expiresIn = object.value("expires_in").toInt();

	if (result.accessToken.isEmpty()) {
		result.ok = false;
		result.error = "OAuth response did not contain access_token";
		return result;
	}

	result.ok = true;
	return result;
}

TokenResult postFormToTokenEndpoint(const std::string &body)
{
	TokenResult tokenResult;

	CURL *rawCurl = curl_easy_init();

	if (!rawCurl) {
		tokenResult.ok = false;
		tokenResult.error = "Failed to initialize CURL for OAuth token request";
		return tokenResult;
	}

	std::string response;
	char errorBuffer[CURL_ERROR_SIZE] = {};

	struct curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");

	curl_easy_setopt(rawCurl, CURLOPT_ERRORBUFFER, errorBuffer);
	curl_easy_setopt(rawCurl, CURLOPT_URL, "https://oauth2.googleapis.com/token");
	curl_easy_setopt(rawCurl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(rawCurl, CURLOPT_POST, 1L);
	curl_easy_setopt(rawCurl, CURLOPT_POSTFIELDS, body.c_str());
	curl_easy_setopt(rawCurl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
	curl_easy_setopt(rawCurl, CURLOPT_WRITEFUNCTION, writeCallback);
	curl_easy_setopt(rawCurl, CURLOPT_WRITEDATA, &response);

	const CURLcode curlCode = curl_easy_perform(rawCurl);

	long httpStatus = 0;
	curl_easy_getinfo(rawCurl, CURLINFO_RESPONSE_CODE, &httpStatus);

	if (headers) {
		curl_slist_free_all(headers);
	}

	curl_easy_cleanup(rawCurl);

	if (curlCode != CURLE_OK) {
		tokenResult.ok = false;

		if (errorBuffer[0] != '\0') {
			tokenResult.error = QString::fromUtf8(errorBuffer);
		} else {
			tokenResult.error = QString::fromUtf8(curl_easy_strerror(curlCode));
		}

		return tokenResult;
	}

	return parseTokenResponse(response, httpStatus);
}

} // namespace

QString GoogleOAuth::buildAuthUrl(const QString &clientId, const QString &redirectUri)
{
	QUrl url("https://accounts.google.com/o/oauth2/v2/auth");

	QUrlQuery query;
	query.addQueryItem("client_id", clientId);
	query.addQueryItem("redirect_uri", redirectUri);
	query.addQueryItem("response_type", "code");
	query.addQueryItem("scope", "https://www.googleapis.com/auth/drive");
	query.addQueryItem("access_type", "offline");
	query.addQueryItem("prompt", "consent");

	url.setQuery(query);

	return url.toString();
}

TokenResult GoogleOAuth::exchangeCodeForToken(const QString &clientId, const QString &clientSecret,
					      const QString &redirectUri, const QString &code)
{
	std::string body;

	body += "code=" + urlEncode(code).toStdString();
	body += "&client_id=" + urlEncode(clientId).toStdString();

	if (!clientSecret.isEmpty()) {
		body += "&client_secret=" + urlEncode(clientSecret).toStdString();
	}

	body += "&redirect_uri=" + urlEncode(redirectUri).toStdString();
	body += "&grant_type=authorization_code";

	return postFormToTokenEndpoint(body);
}

TokenResult GoogleOAuth::refreshAccessToken(const QString &clientId, const QString &clientSecret,
					    const QString &refreshToken)
{
	std::string body;

	body += "client_id=" + urlEncode(clientId).toStdString();

	if (!clientSecret.isEmpty()) {
		body += "&client_secret=" + urlEncode(clientSecret).toStdString();
	}

	body += "&refresh_token=" + urlEncode(refreshToken).toStdString();
	body += "&grant_type=refresh_token";

	return postFormToTokenEndpoint(body);
}
