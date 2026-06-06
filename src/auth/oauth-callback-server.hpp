#pragma once

#include <QObject>
#include <QTcpServer>
#include <QString>

class OAuthCallbackServer : public QObject {
	Q_OBJECT

public:
	explicit OAuthCallbackServer(QObject *parent = nullptr);

	bool start(quint16 port = 0);
	void stop();

	QString redirectUri() const;

signals:
	void codeReceived(QString code);
	void errorReceived(QString error);
	void serverError(QString message);

private slots:
	void handleNewConnection();

private:
	QTcpServer server;
	quint16 selectedPort = 0;
	bool authorizationCodeAlreadyReceived = false;
};
