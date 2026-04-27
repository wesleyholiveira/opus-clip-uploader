#include "auth/oauth-callback-server.hpp"

#include <QHostAddress>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

OAuthCallbackServer::OAuthCallbackServer(QObject* parent)
    : QObject(parent)
{
    connect(
        &server,
        &QTcpServer::newConnection,
        this,
        &OAuthCallbackServer::handleNewConnection
    );
}

bool OAuthCallbackServer::start(quint16 port)
{
    if (server.isListening()) {
        return true;
    }

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
    QTcpSocket* socket = server.nextPendingConnection();

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
        QUrl url("http://127.0.0.1" + pathAndQuery);
        QUrlQuery query(url);

        const QString code = query.queryItemValue("code");
        const QString error = query.queryItemValue("error");

        QByteArray body;

        if (!error.isEmpty()) {
            body =
                "<html><body>"
                "<h2>Authorization failed</h2>"
                "<p>You can close this window and return to OBS.</p>"
                "</body></html>";

            emit errorReceived(error);
        } else if (!code.isEmpty()) {
            body =
                "<html><body>"
                "<h2>Authorization completed</h2>"
                "<p>You can close this window and return to OBS.</p>"
                "</body></html>";

            emit codeReceived(code);
        } else {
            body =
                "<html><body>"
                "<h2>Invalid OAuth callback</h2>"
                "<p>Missing authorization code.</p>"
                "</body></html>";

            emit serverError("Missing authorization code in callback");
        }

        QByteArray response;
        response += "HTTP/1.1 200 OK\r\n";
        response += "Content-Type: text/html; charset=utf-8\r\n";
        response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        response += "Connection: close\r\n";
        response += "\r\n";
        response += body;

        socket->write(response);
        socket->flush();
        socket->disconnectFromHost();
        socket->deleteLater();

        stop();
    });
}
