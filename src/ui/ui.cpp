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

#include <QVector>
#include <QStringList>
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
#include <QFileInfo>
#include <QObject>

#include <functional>

static const QString title("Clip Cropper");

static bool oauthFlowInProgress = false;
static QStringList pendingRecordingPaths;

static QString get_google_client_id()
{
	return PluginConfig::getValue("google_client_id");
}

static QString get_google_client_secret()
{
	return PluginConfig::getValue("google_client_secret");
}

void set_pending_recording_paths(const QStringList &paths)
{
	pendingRecordingPaths = paths;

	for (const QString &path : pendingRecordingPaths) {
		obs_log(LOG_INFO, "Pending recording path set: %s", path.toUtf8().constData());
	}
}

void clear_pending_recording_paths()
{
	pendingRecordingPaths.clear();
	obs_log(LOG_INFO, "Pending recording paths cleared");
}

static QStringList get_recording_paths_for_upload()
{
	return pendingRecordingPaths;
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

static void resize_upload_dialog(QDialog *dialog, bool expanded)
{
	dialog->setMinimumWidth(560);

	if (expanded) {
		dialog->setMinimumHeight(240);
		dialog->setMaximumHeight(QWIDGETSIZE_MAX);
	} else {
		dialog->setMinimumHeight(0);
		dialog->setMaximumHeight(QWIDGETSIZE_MAX);
	}

	dialog->adjustSize();
}

static void start_upload(QDialog *dialog, QPushButton *btnUpload, QPushButton *btnCancel, QProgressBar *progressBar,
			 QLabel *uploadStatusLabel, const QString &accessToken)
{
	const QStringList recordingPaths = get_recording_paths_for_upload();

	if (recordingPaths.isEmpty()) {
		obs_log(LOG_ERROR, "No recording path found for upload.");

		QMessageBox::critical(dialog, title, "Nenhum arquivo de gravação válido foi encontrado.");

		btnUpload->setEnabled(true);
		btnCancel->setEnabled(true);
		progressBar->hide();

		if (uploadStatusLabel) {
			uploadStatusLabel->hide();
			uploadStatusLabel->clear();
		}

		return;
	}

	static constexpr int MAX_PARALLEL_UPLOADS = 3;

	struct UploadBatchState {
		QStringList paths;
		QVector<int> progress;
		int nextIndex = 0;
		int running = 0;
		int completed = 0;
		int failed = 0;
	};

	auto *state = new UploadBatchState();
	state->paths = recordingPaths;
	state->progress = QVector<int>(recordingPaths.size(), 0);

	btnUpload->setEnabled(false);
	btnCancel->setEnabled(false);

	progressBar->show();
	progressBar->setValue(0);

	if (uploadStatusLabel) {
		uploadStatusLabel->show();
		uploadStatusLabel->setText(QString("Preparando upload de %1 arquivo(s)...").arg(state->paths.size()));
	}

	resize_upload_dialog(dialog, true);

	auto updateProgress = [=]() {
		int totalProgress = 0;

		for (int value : state->progress) {
			totalProgress += value;
		}

		const int globalProgress = state->paths.isEmpty() ? 0 : totalProgress / state->paths.size();

		progressBar->setValue(globalProgress);

		if (uploadStatusLabel) {
			uploadStatusLabel->setText(
				QString("Enviando vídeos: %1/%2")
					.arg(qMin(state->completed + state->running, state->paths.size()))
					.arg(state->paths.size()));
		}
	};

	auto *startNext = new std::function<void()>();

	*startNext = [=]() mutable {
		while (state->running < MAX_PARALLEL_UPLOADS && state->nextIndex < state->paths.size()) {
			const int index = state->nextIndex++;
			const QString recordingPath = state->paths.at(index);
			const QFileInfo qFileInfo(recordingPath);

			if (!qFileInfo.exists() || !qFileInfo.isFile()) {
				obs_log(LOG_ERROR, "Invalid upload path. Expected file, got: %s",
					recordingPath.toUtf8().constData());

				state->progress[index] = 100;
				state->completed++;
				state->failed++;

				updateProgress();
				continue;
			}

			FileInfo fInfo(recordingPath.toStdString());
			fInfo.parseFile();

			obs_log(LOG_INFO, "Uploading part %d/%d - Path: %s, Name: %s, Mime: %s", index + 1,
				state->paths.size(), fInfo.filePath.c_str(), fInfo.fileName.c_str(),
				fInfo.mimeType.c_str());

			auto *thread = new QThread;
			auto *worker = new UploadWorker(accessToken, QString::fromStdString(fInfo.filePath),
							QString::fromStdString(fInfo.fileName),
							QString::fromStdString(fInfo.mimeType),
							PluginConfig::getValue("drive_folder_name"));

			state->running++;

			worker->moveToThread(thread);

			QObject::connect(thread, &QThread::started, worker, &UploadWorker::run);

			QObject::connect(
				worker, &UploadWorker::progressChanged, progressBar,
				[=](int value) {
					state->progress[index] = value;
					updateProgress();
				},
				Qt::QueuedConnection);

			QObject::connect(worker, &UploadWorker::finished, thread, &QThread::quit);
			QObject::connect(worker, &UploadWorker::finished, worker, &QObject::deleteLater);
			QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);

			QObject::connect(
				worker, &UploadWorker::failed, dialog,
				[=](const QString &message) {
					obs_log(LOG_ERROR, "Upload failed: %s", message.toUtf8().constData());
					state->failed++;
				},
				Qt::QueuedConnection);

			QObject::connect(
				worker, &UploadWorker::finished, dialog,
				[=]() mutable {
					state->running--;
					state->completed++;
					state->progress[index] = 100;

					updateProgress();

					if (state->completed >= state->paths.size()) {
						clear_pending_recording_paths();

						if (state->failed > 0) {
							QMessageBox::warning(
								dialog, title,
								QString("Upload finalizado com %1 falha(s).")
									.arg(state->failed));
						}

						delete state;
						delete startNext;

						dialog->accept();
						return;
					}

					(*startNext)();
				},
				Qt::QueuedConnection);

			thread->start();
		}
	};

	(*startNext)();
}

static void start_google_oauth_flow(QDialog *dialog, QPushButton *btnUpload, QPushButton *btnCancel,
				    QProgressBar *progressBar, QLabel *uploadStatusLabel)
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
	const int port = PluginConfig::getValue("webserver_port", "53682").toInt();

	if (!oauthServer->start(port)) {
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

	QObject::connect(
		oauthServer, &OAuthCallbackServer::codeReceived, dialog,
		[dialog, btnUpload, btnCancel, progressBar, uploadStatusLabel, oauthServer, redirectUri, clientId,
		 clientSecret](const QString &code) {
			oauthFlowInProgress = false;

			obs_log(LOG_INFO, "OAuth authorization code received");

			oauthServer->stop();

			auto *oauth = new GoogleOAuth(dialog);

			QObject::connect(
				oauth, &GoogleOAuth::tokenReceived, dialog,
				[dialog, btnUpload, btnCancel, progressBar, uploadStatusLabel, oauth,
				 oauthServer](const TokenResult &result) {
					if (!result.ok) {
						PluginConfig::removeValue("google_access_token");

						obs_log(LOG_ERROR, "OAuth token response was not ok: %s",
							result.error.toUtf8().constData());

						QMessageBox::critical(
							dialog, title,
							"Falha ao trocar o código OAuth pelo access token:\n" +
								result.error);

						btnUpload->setEnabled(true);
						btnCancel->setEnabled(true);

						oauth->deleteLater();
						oauthServer->deleteLater();
						return;
					}

					if (result.accessToken.trimmed().isEmpty()) {
						PluginConfig::removeValue("google_access_token");

						obs_log(LOG_ERROR, "OAuth token exchange returned empty access token");

						QMessageBox::critical(dialog, title,
								      "O Google retornou um access token vazio.");

						btnUpload->setEnabled(true);
						btnCancel->setEnabled(true);

						oauth->deleteLater();
						oauthServer->deleteLater();
						return;
					}

					PluginConfig::setValue("google_access_token", result.accessToken);

					if (!result.refreshToken.trimmed().isEmpty()) {
						PluginConfig::setValue("google_refresh_token", result.refreshToken);
					}

					obs_log(LOG_INFO, "OAuth access token obtained successfully");

					oauth->deleteLater();
					oauthServer->deleteLater();

					start_upload(dialog, btnUpload, btnCancel, progressBar, uploadStatusLabel,
						     result.accessToken);
				},
				Qt::QueuedConnection);

			QObject::connect(
				oauth, &GoogleOAuth::tokenFailed, dialog,
				[dialog, btnUpload, btnCancel, oauth, oauthServer](const TokenResult &result) {
					PluginConfig::removeValue("google_access_token");

					obs_log(LOG_ERROR, "Failed to exchange OAuth code for token: %s",
						result.error.toUtf8().constData());

					QMessageBox::critical(dialog, title,
							      "Falha ao trocar o código OAuth pelo access token:\n" +
								      result.error);

					btnUpload->setEnabled(true);
					btnCancel->setEnabled(true);

					oauth->deleteLater();
					oauthServer->deleteLater();
				},
				Qt::QueuedConnection);

			oauth->exchangeCodeForTokenAsync(clientId, clientSecret, redirectUri, code);
		},
		Qt::QueuedConnection);

	QObject::connect(
		oauthServer, &OAuthCallbackServer::errorReceived, dialog,
		[dialog, btnUpload, btnCancel, oauthServer](const QString &error) {
			oauthFlowInProgress = false;
			PluginConfig::removeValue("google_access_token");

			oauthServer->stop();
			oauthServer->deleteLater();

			obs_log(LOG_ERROR, "OAuth authorization failed: %s", error.toStdString().c_str());

			QMessageBox::critical(dialog, title, "Autenticação OAuth falhou:\n" + error);

			btnUpload->setEnabled(true);
			btnCancel->setEnabled(true);
		},
		Qt::QueuedConnection);

	QObject::connect(
		oauthServer, &OAuthCallbackServer::serverError, dialog,
		[dialog, btnUpload, btnCancel, oauthServer](const QString &error) {
			oauthFlowInProgress = false;
			PluginConfig::removeValue("google_access_token");

			oauthServer->stop();
			oauthServer->deleteLater();

			obs_log(LOG_ERROR, "OAuth callback server error: %s", error.toStdString().c_str());

			QMessageBox::critical(dialog, title, "Erro no servidor local OAuth:\n" + error);

			btnUpload->setEnabled(true);
			btnCancel->setEnabled(true);
		},
		Qt::QueuedConnection);

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

	QLineEdit *webServerPortInput = new QLineEdit(&dialog);
	webServerPortInput->setPlaceholderText("53682");
	webServerPortInput->setText(PluginConfig::getValue("webserver_port", "53682"));

	QLineEdit *clientSecretInput = new QLineEdit(&dialog);
	clientSecretInput->setEchoMode(QLineEdit::Password);
	clientSecretInput->setPlaceholderText("Google OAuth Client Secret");
	clientSecretInput->setText(PluginConfig::getValue("google_client_secret"));

	formLayout->addRow("Nome da pasta no Drive:", folderNameInput);
	formLayout->addRow("Client ID:", clientIdInput);
	formLayout->addRow("Client Secret:", clientSecretInput);
	formLayout->addRow("Webserver Port:", webServerPortInput);

	QPushButton *btn = new QPushButton("Salvar", &dialog);

	mainLayout->addLayout(formLayout);
	mainLayout->addWidget(btn);
	mainLayout->addStretch();

	QObject::connect(btn, &QPushButton::clicked,
			 [&dialog, folderNameInput, clientIdInput, clientSecretInput, webServerPortInput]() {
				 PluginConfig::setValue("drive_folder_name", folderNameInput->text().trimmed());
				 PluginConfig::setValue("google_client_id", clientIdInput->text().trimmed());
				 PluginConfig::setValue("google_client_secret", clientSecretInput->text().trimmed());
				 PluginConfig::setValue("webserver_port", webServerPortInput->text().trimmed());

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

	QLabel *uploadStatusLabel = new QLabel("", &dialog);
	uploadStatusLabel->setWordWrap(true);
	uploadStatusLabel->setMinimumHeight(36);
	uploadStatusLabel->hide();
	mainLayout->addWidget(uploadStatusLabel);

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

	dialog.setMinimumWidth(520);
	dialog.adjustSize();
	dialog.setFixedSize(dialog.sizeHint());

	QObject::connect(btnCancel, &QPushButton::clicked, [&dialog]() {
		obs_log(LOG_INFO, "Fechando Dialog de Upload");
		dialog.reject();
	});

	QObject::connect(
		btnUpload, &QPushButton::clicked, [&dialog, btnUpload, btnCancel, progressBar, uploadStatusLabel]() {
			const QString accessToken = PluginConfig::getValue("google_access_token");

			if (accessToken.trimmed().isEmpty()) {
				obs_log(LOG_INFO, "No access token found. Starting Google OAuth flow.");
				start_google_oauth_flow(&dialog, btnUpload, btnCancel, progressBar, uploadStatusLabel);
				return;
			}

			obs_log(LOG_INFO, "Access token found. Starting upload.");
			start_upload(&dialog, btnUpload, btnCancel, progressBar, uploadStatusLabel, accessToken);
		});

	dialog.exec();
}
