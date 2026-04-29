#ifdef __cplusplus
extern "C" {
#endif

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#ifdef __cplusplus
}
#endif

#include <utils/config.hpp>
#include <utils/file.hpp>
#include <worker/upload-worker.hpp>
#include <auth/oauth-callback-server.hpp>
#include <auth/google-oauth.hpp>

#include <QDialog>
#include <QProgressBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QWidget>
#include <QThread>
#include <QDesktopServices>
#include <QUrl>
#include <QMessageBox>

static const QString title("Clip Cropper");

static bool oauthFlowInProgress = false;
static QString pendingRecordingPath;

static QString get_google_client_id()
{
	return PluginConfig::getValue("google_client_id");
}

static QString get_google_client_secret()
{
	return PluginConfig::getValue("google_client_secret");
}

void set_pending_recording_path(const QString &path)
{
	pendingRecordingPath = path;
	obs_log(LOG_INFO, "Pending recording path set: %s", pendingRecordingPath.toUtf8().constData());
}

void clear_pending_recording_path()
{
	pendingRecordingPath.clear();
	obs_log(LOG_INFO, "Pending recording path cleared");
}

static QString get_recording_path_for_upload()
{
	if (!pendingRecordingPath.trimmed().isEmpty()) {
		return pendingRecordingPath;
	}

	using ObsCharPtr = std::unique_ptr<char, decltype(&bfree)>;
	ObsCharPtr safePath(obs_frontend_get_last_recording(), bfree);

	if (!safePath || !safePath.get() || safePath.get()[0] == '\0') {
		return {};
	}

	return QString::fromUtf8(safePath.get());
}

void ensure_google_access_token(QWidget *parent)
{
	UNUSED_PARAMETER(parent);

	const QString accessToken = PluginConfig::getValue("google_access_token");

	if (!accessToken.trimmed().isEmpty()) {
		obs_log(LOG_INFO, "Google OAuth access token already exists");
		return;
	}

	obs_log(LOG_INFO, "Google OAuth access token does not exist yet");
}

static void start_upload(QDialog *dialog, QPushButton *btnUpload, QPushButton *btnCancel, QProgressBar *progressBar,
			 const QString &accessToken)
{
	const QString recordingPath = get_recording_path_for_upload();

	if (recordingPath.trimmed().isEmpty()) {
		obs_log(LOG_ERROR, "No recording path found for upload.");

		QMessageBox::critical(dialog, title, "Não foi possível identificar o arquivo da gravação.");

		btnUpload->setEnabled(true);
		btnCancel->setEnabled(true);

		return;
	}

	FileInfo fInfo(recordingPath.toStdString());
	fInfo.parseFile();

	btnUpload->setEnabled(false);
	btnCancel->setEnabled(true);

	progressBar->show();
	progressBar->setValue(0);

	obs_log(LOG_INFO, "Path: %s, Name: %s, Mime: %s", fInfo.filePath.c_str(), fInfo.fileName.c_str(),
		fInfo.mimeType.c_str());

	auto *thread = new QThread;
	auto *worker = new UploadWorker(accessToken, QString::fromStdString(fInfo.filePath),
					QString::fromStdString(fInfo.fileName), QString::fromStdString(fInfo.mimeType),
					PluginConfig::getValue("drive_folder_name"));

	worker->moveToThread(thread);

	QObject::connect(thread, &QThread::started, worker, &UploadWorker::run);

	QObject::connect(worker, &UploadWorker::progressChanged, progressBar, &QProgressBar::setValue,
			 Qt::QueuedConnection);

	QObject::connect(worker, &UploadWorker::finished, thread, &QThread::quit);
	QObject::connect(worker, &UploadWorker::finished, worker, &QObject::deleteLater);
	QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);

	QObject::connect(
		worker, &UploadWorker::finished, dialog,
		[dialog]() {
			clear_pending_recording_path();
			dialog->accept();
		},
		Qt::QueuedConnection);

	thread->start();
}

static void start_google_oauth_flow(QDialog *dialog, QPushButton *btnUpload, QPushButton *btnCancel,
				    QProgressBar *progressBar)
{
	if (oauthFlowInProgress) {
		obs_log(LOG_INFO, "OAuth flow already in progress, ignoring duplicate request");

		QMessageBox::information(
			dialog, title,
			"Já existe um fluxo OAuth em andamento. Conclua ou cancele a autenticação aberta no navegador.");

		btnUpload->setEnabled(true);
		btnCancel->setEnabled(true);

		return;
	}

	oauthFlowInProgress = true;
	auto *oauthServer = new OAuthCallbackServer(dialog);

	if (!oauthServer->start(53682)) {
		oauthFlowInProgress = false;

		obs_log(LOG_ERROR, "Failed to start OAuth callback server");

		QMessageBox::critical(dialog, title,
				      "Não foi possível iniciar o servidor local para autenticação OAuth.");

		oauthServer->deleteLater();

		btnUpload->setEnabled(true);
		btnCancel->setEnabled(true);

		return;
	}

	const QString redirectUri = oauthServer->redirectUri();
	const QString clientId = get_google_client_id();
	const QString clientSecret = get_google_client_secret();

	if (clientId.trimmed().isEmpty() || clientSecret.trimmed().isEmpty()) {
		oauthFlowInProgress = false;

		QMessageBox::warning(dialog, title, "Configure o Client ID e o Client Secret antes de autenticar.");

		oauthServer->stop();
		oauthServer->deleteLater();

		btnUpload->setEnabled(true);
		btnCancel->setEnabled(true);

		return;
	}

	const QString authUrl = GoogleOAuth::buildAuthUrl(clientId, redirectUri);

	QObject::connect(oauthServer, &OAuthCallbackServer::codeReceived, dialog,
			 [dialog, btnUpload, btnCancel, progressBar, oauthServer, redirectUri, clientId,
			  clientSecret](const QString &code) {
				 oauthFlowInProgress = false;

				 obs_log(LOG_INFO, "OAuth authorization code received");

				 TokenResult tokenResult =
					 GoogleOAuth::exchangeCodeForToken(clientId, clientSecret, redirectUri, code);

				 oauthServer->deleteLater();

				 if (!tokenResult.ok) {
					 PluginConfig::removeValue("google_access_token");

					 obs_log(LOG_ERROR, "Failed to exchange OAuth code for token: %s",
						 tokenResult.error.toStdString().c_str());

					 QMessageBox::critical(dialog, title,
							       "Falha ao trocar o código OAuth pelo access token:\n" +
								       tokenResult.error);

					 btnUpload->setEnabled(true);
					 btnCancel->setEnabled(true);

					 return;
				 }

				 if (tokenResult.accessToken.trimmed().isEmpty()) {
					 PluginConfig::removeValue("google_access_token");

					 obs_log(LOG_ERROR, "OAuth token exchange returned empty access token");

					 QMessageBox::critical(dialog, title,
							       "O Google retornou um access token vazio.");

					 btnUpload->setEnabled(true);
					 btnCancel->setEnabled(true);

					 return;
				 }

				 PluginConfig::setValue("google_access_token", tokenResult.accessToken);

				 obs_log(LOG_INFO, "OAuth access token obtained successfully");

				 start_upload(dialog, btnUpload, btnCancel, progressBar, tokenResult.accessToken);
			 });

	QObject::connect(oauthServer, &OAuthCallbackServer::errorReceived, dialog,
			 [dialog, btnUpload, btnCancel, oauthServer](const QString &error) {
				 oauthFlowInProgress = false;
				 PluginConfig::removeValue("google_access_token");

				 oauthServer->deleteLater();

				 obs_log(LOG_ERROR, "OAuth authorization failed: %s", error.toStdString().c_str());

				 QMessageBox::critical(dialog, title, "Autenticação OAuth falhou:\n" + error);

				 btnUpload->setEnabled(true);
				 btnCancel->setEnabled(true);
			 });

	QObject::connect(oauthServer, &OAuthCallbackServer::serverError, dialog,
			 [dialog, btnUpload, btnCancel, oauthServer](const QString &error) {
				 oauthFlowInProgress = false;
				 PluginConfig::removeValue("google_access_token");

				 oauthServer->deleteLater();

				 obs_log(LOG_ERROR, "OAuth callback server error: %s", error.toStdString().c_str());

				 QMessageBox::critical(dialog, title, "Erro no servidor local OAuth:\n" + error);

				 btnUpload->setEnabled(true);
				 btnCancel->setEnabled(true);
			 });

	btnUpload->setEnabled(false);
	btnCancel->setEnabled(true);

	if (!QDesktopServices::openUrl(QUrl(authUrl))) {
		oauthFlowInProgress = false;
		PluginConfig::removeValue("google_access_token");

		oauthServer->stop();
		oauthServer->deleteLater();

		QMessageBox::critical(dialog, title, "Não foi possível abrir o navegador para autenticação OAuth.");

		btnUpload->setEnabled(true);
		btnCancel->setEnabled(true);
	}
}

void open_settings(void *private_data)
{
	UNUSED_PARAMETER(private_data);

	QWidget *parent = (QWidget *)obs_frontend_get_main_window();

	QDialog dialog(parent);
	dialog.setWindowTitle(title);
	dialog.resize(520, 220);

	QVBoxLayout *mainLayout = new QVBoxLayout(&dialog);
	mainLayout->setContentsMargins(20, 20, 20, 0);
	mainLayout->setSpacing(12);

	QFormLayout *formLayout = new QFormLayout();
	formLayout->setLabelAlignment(Qt::AlignLeft);

	QLineEdit *folderNameInput = new QLineEdit(&dialog);
	folderNameInput->setPlaceholderText("Ex: GRAVACIONES");
	folderNameInput->setText(PluginConfig::getValue("drive_folder_name"));

	QLineEdit *clientIdInput = new QLineEdit(&dialog);
	clientIdInput->setPlaceholderText("Google OAuth Client ID");
	clientIdInput->setText(PluginConfig::getValue("google_client_id"));

	QLineEdit *clientSecretInput = new QLineEdit(&dialog);
	clientSecretInput->setEchoMode(QLineEdit::Password);
	clientSecretInput->setPlaceholderText("Google OAuth Client Secret");
	clientSecretInput->setText(PluginConfig::getValue("google_client_secret"));

	formLayout->addRow("Nome da pasta no Drive:", folderNameInput);
	formLayout->addRow("Client ID:", clientIdInput);
	formLayout->addRow("Client Secret:", clientSecretInput);

	QPushButton *btn = new QPushButton("Salvar", &dialog);

	mainLayout->addLayout(formLayout);
	mainLayout->addWidget(btn);
	mainLayout->addStretch();

	QObject::connect(btn, &QPushButton::clicked, [&dialog, folderNameInput, clientIdInput, clientSecretInput]() {
		PluginConfig::setValue("drive_folder_name", folderNameInput->text().trimmed());
		PluginConfig::setValue("google_client_id", clientIdInput->text().trimmed());
		PluginConfig::setValue("google_client_secret", clientSecretInput->text().trimmed());

		PluginConfig::removeValue("google_access_token");
		oauthFlowInProgress = false;

		obs_log(LOG_INFO, "Clip Cropper settings saved. Access token invalidated.");

		dialog.accept();
	});

	dialog.exec();
}

void open_confirm_dialog(void *private_data)
{
	UNUSED_PARAMETER(private_data);

	QWidget *parent = (QWidget *)obs_frontend_get_main_window();

	QDialog dialog(parent);
	dialog.setWindowTitle(title + " - Confirmar Upload");

	QVBoxLayout *mainLayout = new QVBoxLayout(&dialog);
	mainLayout->setContentsMargins(22, 16, 22, 16);
	mainLayout->setSpacing(8);

	QLabel *label = new QLabel("Deseja fazer o upload do arquivo para o Drive?", &dialog);
	mainLayout->addWidget(label);

	QProgressBar *progressBar = new QProgressBar(&dialog);
	progressBar->setRange(0, 100);
	progressBar->setValue(0);
	progressBar->hide();
	mainLayout->addWidget(progressBar);

	QPushButton *btnUpload = new QPushButton("Sim", &dialog);
	QPushButton *btnCancel = new QPushButton("Não", &dialog);

	btnUpload->setMinimumHeight(32);
	btnCancel->setMinimumHeight(32);

	QHBoxLayout *btnLayout = new QHBoxLayout();
	btnLayout->setSpacing(12);
	btnLayout->addWidget(btnUpload);
	btnLayout->addWidget(btnCancel);

	mainLayout->addLayout(btnLayout);

	dialog.adjustSize();

	QSize size = dialog.sizeHint();
	size.setWidth(420);
	dialog.setFixedSize(size);

	QObject::connect(btnCancel, &QPushButton::clicked, [&dialog]() {
		obs_log(LOG_INFO, "Fechando Dialog de Upload");
		dialog.reject();
	});

	QObject::connect(btnUpload, &QPushButton::clicked, [&dialog, btnUpload, btnCancel, progressBar]() {
		const QString accessToken = PluginConfig::getValue("google_access_token");

		if (accessToken.trimmed().isEmpty()) {
			obs_log(LOG_INFO, "No access token found. Starting Google OAuth flow.");
			start_google_oauth_flow(&dialog, btnUpload, btnCancel, progressBar);
			return;
		}

		obs_log(LOG_INFO, "Access token found. Starting upload.");
		start_upload(&dialog, btnUpload, btnCancel, progressBar, accessToken);
	});

	dialog.exec();
}
