#pragma once

#include <QString>

struct TokenResult {
	bool ok = false;

	QString accessToken;
	QString refreshToken;
	QString tokenType;
	QString scope;

	int expiresIn = 0;

	QString error;
	QString rawResponse;
};

class GoogleOAuth {
public:
	static QString buildAuthUrl(const QString &clientId, const QString &redirectUri);

	static TokenResult exchangeCodeForToken(const QString &clientId, const QString &clientSecret,
						const QString &redirectUri, const QString &code);

	static TokenResult refreshAccessToken(const QString &clientId, const QString &clientSecret,
					      const QString &refreshToken);
};
