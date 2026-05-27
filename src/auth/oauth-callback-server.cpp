#include "auth/oauth-callback-server.hpp"

#include <QHostAddress>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

OAuthCallbackServer::OAuthCallbackServer(QObject *parent) : QObject(parent)
{
	connect(&server, &QTcpServer::newConnection, this, &OAuthCallbackServer::handleNewConnection);
}

bool OAuthCallbackServer::start(quint16 port)
{
	if (server.isListening()) {
		return true;
	}

	authorizationCodeAlreadyReceived = false;

	if (!server.listen(QHostAddress::LocalHost, port)) {
		emit serverError(server.errorString());
		return false;
	}

	selectedPort = server.serverPort();
	return true;
}

void OAuthCallbackServer::stop()
{
	if (server.isListening()) {
		server.close();
	}
}

QString OAuthCallbackServer::redirectUri() const
{
	return QString("http://127.0.0.1:%1/callback").arg(selectedPort);
}

void OAuthCallbackServer::handleNewConnection()
{
	QTcpSocket *socket = server.nextPendingConnection();

	if (!socket) {
		return;
	}

	connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
		const QByteArray request = socket->readAll();
		const QList<QByteArray> lines = request.split('\n');

		if (lines.isEmpty()) {
			socket->disconnectFromHost();
			socket->deleteLater();
			return;
		}

		const QByteArray requestLine = lines.first().trimmed();
		const QList<QByteArray> parts = requestLine.split(' ');

		if (parts.size() < 2) {
			socket->disconnectFromHost();
			socket->deleteLater();
			return;
		}

		const QString pathAndQuery = QString::fromUtf8(parts[1]);

		QUrl url;
		if (pathAndQuery.startsWith("http://") || pathAndQuery.startsWith("https://")) {
			url = QUrl(pathAndQuery);
		} else {
			url = QUrl(QString("http://127.0.0.1") + pathAndQuery);
		}

		QUrlQuery query(url);

		const QString requestPath = url.path();
		const QString code = query.queryItemValue("code", QUrl::FullyDecoded);
		const QString error = query.queryItemValue("error", QUrl::FullyDecoded);

		auto sendResponse = [socket](int statusCode, const QByteArray &statusText, const QByteArray &body) {
			QByteArray response;
			response += "HTTP/1.1 " + QByteArray::number(statusCode) + " " + statusText + "\r\n";
			response += "Content-Type: text/html; charset=utf-8\r\n";
			response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
			response += "Connection: close\r\n";
			response += "\r\n";
			response += body;

			socket->write(response);
			socket->flush();
			socket->disconnectFromHost();
			socket->deleteLater();
		};

		if (requestPath == "/favicon.ico") {
			sendResponse(204, "No Content", QByteArray());
			return;
		}

		if (!error.isEmpty()) {
			const QByteArray body = "<html><body>"
						"<h2>Authorization failed</h2>"
						"<p>You can close this window and return to OBS.</p>"
						"</body></html>";

			sendResponse(200, "OK", body);

			if (!authorizationCodeAlreadyReceived) {
				emit errorReceived(error);
			}

			stop();
			return;
		}

		if (!code.isEmpty()) {
			authorizationCodeAlreadyReceived = true;

			const QByteArray body = "<html><body>"
						"<h2>Authorization completed</h2>"
						"<p>You can close this window and return to OBS.</p>"
						"</body></html>";

			sendResponse(200, "OK", body);

			emit codeReceived(code);

			stop();
			return;
		}

		const QByteArray body = "<html><body>"
					"<h2>Invalid OAuth callback</h2>"
					"<p>Missing authorization code.</p>"
					"</body></html>";

		sendResponse(400, "Bad Request", body);

		if (!authorizationCodeAlreadyReceived) {
			emit serverError("Missing authorization code in callback");
			stop();
		}
	});
}