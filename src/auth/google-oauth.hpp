#pragma once

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>

struct TokenResult {
	bool ok = false;

	QString accessToken;
	QString refreshToken;
	QString tokenType;
	QString scope;

	int expiresIn = 0;
	int httpStatus = 0;

	QString error;
	QString rawResponse;
};

class GoogleOAuth : public QObject {
	Q_OBJECT

public:
	explicit GoogleOAuth(QObject *parent = nullptr);

	static QString buildAuthUrl(const QString &clientId, const QString &redirectUri);

	void exchangeCodeForTokenAsync(const QString &clientId, const QString &clientSecret, const QString &redirectUri,
				       const QString &code);

	void refreshAccessTokenAsync(const QString &clientId, const QString &clientSecret, const QString &refreshToken);

signals:
	void tokenReceived(const TokenResult &result);
	void tokenFailed(const TokenResult &result);

private:
	QNetworkAccessManager network;

	void postFormToTokenEndpoint(const QByteArray &body);
	static TokenResult parseTokenResponse(const QByteArray &response, int httpStatus);
};
