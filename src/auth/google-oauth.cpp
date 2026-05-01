#include "auth/google-oauth.hpp"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>

GoogleOAuth::GoogleOAuth(QObject *parent) : QObject(parent) {}

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

void GoogleOAuth::exchangeCodeForTokenAsync(const QString &clientId, const QString &clientSecret,
					    const QString &redirectUri, const QString &code)
{
	QUrlQuery form;
	form.addQueryItem("code", code);
	form.addQueryItem("client_id", clientId);

	if (!clientSecret.isEmpty()) {
		form.addQueryItem("client_secret", clientSecret);
	}

	form.addQueryItem("redirect_uri", redirectUri);
	form.addQueryItem("grant_type", "authorization_code");

	postFormToTokenEndpoint(form.query(QUrl::FullyEncoded).toUtf8());
}

void GoogleOAuth::refreshAccessTokenAsync(const QString &clientId, const QString &clientSecret,
					  const QString &refreshToken)
{
	QUrlQuery form;
	form.addQueryItem("client_id", clientId);

	if (!clientSecret.isEmpty()) {
		form.addQueryItem("client_secret", clientSecret);
	}

	form.addQueryItem("refresh_token", refreshToken);
	form.addQueryItem("grant_type", "refresh_token");

	postFormToTokenEndpoint(form.query(QUrl::FullyEncoded).toUtf8());
}

void GoogleOAuth::postFormToTokenEndpoint(const QByteArray &body)
{
	QNetworkRequest request{QUrl("https://oauth2.googleapis.com/token")};

	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");

	request.setHeader(QNetworkRequest::ContentLengthHeader, body.size());

	QNetworkReply *reply = network.post(request, body);

	connect(reply, &QNetworkReply::finished, this, [this, reply]() {
		reply->deleteLater();

		const long httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toLongLong();

		const QByteArray response = reply->readAll();

		if (reply->error() != QNetworkReply::NoError) {
			TokenResult result;
			result.ok = false;
			result.rawResponse = QString::fromUtf8(response);
			result.error = QString("OAuth token request failed. Network error: %1. HTTP: %2. Raw: %3")
					       .arg(reply->errorString())
					       .arg(httpStatus)
					       .arg(result.rawResponse);

			emit tokenFailed(result);
			return;
		}

		TokenResult result = parseTokenResponse(response, httpStatus);

		if (result.ok) {
			emit tokenReceived(result);
		} else {
			emit tokenFailed(result);
		}
	});
}

TokenResult GoogleOAuth::parseTokenResponse(const QByteArray &response, long httpStatus)
{
	TokenResult result;
	result.rawResponse = QString::fromUtf8(response);

	QJsonParseError parseError{};
	const QJsonDocument document = QJsonDocument::fromJson(response, &parseError);

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
