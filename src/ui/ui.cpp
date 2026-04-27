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

static const QString GOOGLE_CLIENT_ID = "778552824803-040l6873ccpv87l93n1qfnjkqc3cgit7.apps.googleusercontent.com";
static const QString GOOGLE_CLIENT_SECRET = "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";

// Para testar sem pasta específica, deixe vazio.
// Se quiser pasta específica, coloque o ID real da pasta do Drive.
static const QString DEFAULT_FOLDER_ID = "";

void ensure_google_access_token(QWidget *parent)
{
    QString accessToken = load_settings();

    if (!accessToken.trimmed().isEmpty()) {
        obs_log(LOG_INFO, "Google OAuth access token already exists");
        return;
    }

    obs_log(LOG_INFO, "No Google OAuth access token found. Starting OAuth flow.");

    auto *oauthServer = new OAuthCallbackServer(parent);

    if (!oauthServer->start(53682)) {
        obs_log(LOG_ERROR, "Failed to start OAuth callback server");

        QMessageBox::critical(
            parent,
            title,
            "Não foi possível iniciar o servidor local para autenticação OAuth."
        );

        oauthServer->deleteLater();
        return;
    }

    const QString redirectUri = oauthServer->redirectUri();

    const QString authUrl = GoogleOAuth::buildAuthUrl(
        GOOGLE_CLIENT_ID,
        redirectUri
    );

    QObject::connect(
        oauthServer,
        &OAuthCallbackServer::codeReceived,
        parent,
        [oauthServer, parent, redirectUri](const QString& code) {
            obs_log(LOG_INFO, "OAuth authorization code received");

            TokenResult tokenResult = GoogleOAuth::exchangeCodeForToken(
                GOOGLE_CLIENT_ID,
                GOOGLE_CLIENT_SECRET,
                redirectUri,
                code
            );

            oauthServer->deleteLater();

            if (!tokenResult.ok) {
                obs_log(
                    LOG_ERROR,
                    "Failed to exchange OAuth code for token: %s",
                    tokenResult.error.toStdString().c_str()
                );

                QMessageBox::critical(
                    parent,
                    title,
                    "Falha ao trocar o código OAuth pelo access token:\n" + tokenResult.error
                );

                return;
            }

            if (tokenResult.accessToken.trimmed().isEmpty()) {
                obs_log(LOG_ERROR, "OAuth token exchange returned empty access token");

                QMessageBox::critical(
                    parent,
                    title,
                    "O Google retornou um access token vazio."
                );

                return;
            }

            save_settings(tokenResult.accessToken);

            obs_log(LOG_INFO, "Google OAuth access token generated and saved successfully");

            QMessageBox::information(
                parent,
                title,
                "Google Drive conectado com sucesso.\nO upload poderá ser feito ao parar a gravação."
            );
        }
    );

    QObject::connect(
        oauthServer,
        &OAuthCallbackServer::errorReceived,
        parent,
        [oauthServer, parent](const QString& error) {
            oauthServer->deleteLater();

            obs_log(
                LOG_ERROR,
                "OAuth authorization failed: %s",
                error.toStdString().c_str()
            );

            QMessageBox::critical(
                parent,
                title,
                "Autenticação OAuth falhou:\n" + error
            );
        }
    );

    QObject::connect(
        oauthServer,
        &OAuthCallbackServer::serverError,
        parent,
        [oauthServer, parent](const QString& error) {
            oauthServer->deleteLater();

            obs_log(
                LOG_ERROR,
                "OAuth callback server error: %s",
                error.toStdString().c_str()
            );

            QMessageBox::critical(
                parent,
                title,
                "Erro no servidor local OAuth:\n" + error
            );
        }
    );

    const bool opened = QDesktopServices::openUrl(QUrl(authUrl));

    if (!opened) {
        obs_log(LOG_ERROR, "Failed to open OAuth URL in browser");

        QMessageBox::critical(
            parent,
            title,
            "Não foi possível abrir o navegador para autenticação OAuth."
        );

        oauthServer->stop();
        oauthServer->deleteLater();
    }
}

static void start_upload(
    QDialog* dialog,
    QPushButton* btnUpload,
    QPushButton* btnCancel,
    QProgressBar* progressBar,
    const QString& accessToken
) {
    using ObsCharPtr = std::unique_ptr<char, decltype(&bfree)>;
    ObsCharPtr safePath(obs_frontend_get_last_recording(), bfree);

    FileInfo fInfo(safePath.get());
    fInfo.parseFile();

    btnUpload->setEnabled(false);
    btnCancel->setEnabled(false);

    progressBar->show();
    progressBar->setValue(0);

    obs_log(LOG_INFO, "Path: %s, Name: %s, Mime: %s",
        fInfo.filePath.c_str(),
        fInfo.fileName.c_str(),
        fInfo.mimeType.c_str()
    );

    auto* thread = new QThread;
    auto* worker = new UploadWorker(
        accessToken,
        QString::fromStdString(fInfo.filePath),
        QString::fromStdString(fInfo.fileName),
        QString::fromStdString(fInfo.mimeType),
        DEFAULT_FOLDER_ID
    );

    worker->moveToThread(thread);
    QObject::connect(
        thread,
        &QThread::started,
        worker,
        &UploadWorker::run
    );

    QObject::connect(
        worker,
        &UploadWorker::progressChanged,
        progressBar,
        &QProgressBar::setValue,
        Qt::QueuedConnection
    );

    QObject::connect(
        worker,
        &UploadWorker::finished,
        thread,
        &QThread::quit
    );

    QObject::connect(
        worker,
        &UploadWorker::finished,
        worker,
        &QObject::deleteLater
    );

    QObject::connect(
        thread,
        &QThread::finished,
        thread,
        &QObject::deleteLater
    );

    QObject::connect(
        worker,
        &UploadWorker::finished,
        dialog,
        [dialog]() {
            dialog->accept();
        },
        Qt::QueuedConnection
    );

    thread->start();
}

static void start_google_oauth_flow(
    QDialog* dialog,
    QPushButton* btnUpload,
    QPushButton* btnCancel,
    QProgressBar* progressBar
) {
    auto* oauthServer = new OAuthCallbackServer(dialog);

    // Porta fixa. Essa redirect URI precisa estar cadastrada no Google Cloud:
    // http://127.0.0.1:53682/callback
    if (!oauthServer->start(53682)) {
        obs_log(LOG_ERROR, "Failed to start OAuth callback server");

        QMessageBox::critical(
            dialog,
            title,
            "Não foi possível iniciar o servidor local para autenticação OAuth."
        );

        oauthServer->deleteLater();
        return;
    }

    const QString redirectUri = oauthServer->redirectUri();

    const QString authUrl = GoogleOAuth::buildAuthUrl(
        GOOGLE_CLIENT_ID,
        redirectUri
    );

    QObject::connect(
        oauthServer,
        &OAuthCallbackServer::codeReceived,
        dialog,
        [dialog, btnUpload, btnCancel, progressBar, oauthServer, redirectUri](const QString& code) {
            obs_log(LOG_INFO, "OAuth authorization code received");

            TokenResult tokenResult = GoogleOAuth::exchangeCodeForToken(
                GOOGLE_CLIENT_ID,
                GOOGLE_CLIENT_SECRET,
                redirectUri,
                code
            );

            oauthServer->deleteLater();

            if (!tokenResult.ok) {
                obs_log(
                    LOG_ERROR,
                    "Failed to exchange OAuth code for token: %s",
                    tokenResult.error.toStdString().c_str()
                );

                QMessageBox::critical(
                    dialog,
                    title,
                    "Falha ao trocar o código OAuth pelo access token:\n" + tokenResult.error
                );

                btnUpload->setEnabled(true);
                btnCancel->setEnabled(true);

                return;
            }

            if (tokenResult.accessToken.trimmed().isEmpty()) {
                obs_log(LOG_ERROR, "OAuth token exchange returned empty access token");

                QMessageBox::critical(
                    dialog,
                    title,
                    "O Google retornou um access token vazio."
                );

                btnUpload->setEnabled(true);
                btnCancel->setEnabled(true);

                return;
            }

            // Temporário: usando o mesmo save_settings que você já tinha.
            // Depois o ideal é salvar access_token, refresh_token e expires_at separadamente.
            save_settings(tokenResult.accessToken);

            obs_log(LOG_INFO, "OAuth access token obtained successfully");

            start_upload(
                dialog,
                btnUpload,
                btnCancel,
                progressBar,
                tokenResult.accessToken
            );
        }
    );

    QObject::connect(
        oauthServer,
        &OAuthCallbackServer::errorReceived,
        dialog,
        [dialog, btnUpload, btnCancel, oauthServer](const QString& error) {
            oauthServer->deleteLater();

            obs_log(
                LOG_ERROR,
                "OAuth authorization failed: %s",
                error.toStdString().c_str()
            );

            QMessageBox::critical(
                dialog,
                title,
                "Autenticação OAuth falhou:\n" + error
            );

            btnUpload->setEnabled(true);
            btnCancel->setEnabled(true);
        }
    );

    QObject::connect(
        oauthServer,
        &OAuthCallbackServer::serverError,
        dialog,
        [dialog, btnUpload, btnCancel, oauthServer](const QString& error) {
            oauthServer->deleteLater();

            obs_log(
                LOG_ERROR,
                "OAuth callback server error: %s",
                error.toStdString().c_str()
            );

            QMessageBox::critical(
                dialog,
                title,
                "Erro no servidor local OAuth:\n" + error
            );

            btnUpload->setEnabled(true);
            btnCancel->setEnabled(true);
        }
    );

    btnUpload->setEnabled(false);
    btnCancel->setEnabled(false);

    const bool opened = QDesktopServices::openUrl(QUrl(authUrl));

    if (!opened) {
        obs_log(LOG_ERROR, "Failed to open OAuth URL in browser");

        QMessageBox::critical(
            dialog,
            title,
            "Não foi possível abrir o navegador para autenticação OAuth."
        );

        btnUpload->setEnabled(true);
        btnCancel->setEnabled(true);

        oauthServer->stop();
        oauthServer->deleteLater();
    }
}

void open_settings(void *private_data)
{
    UNUSED_PARAMETER(private_data);

    QWidget *parent = (QWidget *)obs_frontend_get_main_window();

    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    dialog.resize(420, 160);

    QVBoxLayout *mainLayout = new QVBoxLayout(&dialog);
    mainLayout->setContentsMargins(20, 20, 20, 0);
    mainLayout->setSpacing(12);

    QFormLayout *formLayout = new QFormLayout();
    formLayout->setLabelAlignment(Qt::AlignLeft);

    QLineEdit *accessTokenInput = new QLineEdit(&dialog);
    accessTokenInput->setEchoMode(QLineEdit::Password);
    accessTokenInput->setPlaceholderText("OAuth Access Token temporário");
    accessTokenInput->setText(load_settings());

    formLayout->addRow("Drive Access Token:", accessTokenInput);

    QPushButton *btn = new QPushButton("Salvar", &dialog);

    mainLayout->addLayout(formLayout);
    mainLayout->addWidget(btn);
    mainLayout->addStretch();

    QObject::connect(btn, &QPushButton::clicked, [&dialog, accessTokenInput]() {
        QString accessToken = accessTokenInput->text();

        save_settings(accessToken);

        obs_log(LOG_INFO, "Drive access token saved");
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

    QObject::connect(
        btnUpload,
        &QPushButton::clicked,
        [&dialog, btnUpload, btnCancel, progressBar]() {
            QString accessToken = load_settings();

            if (accessToken.trimmed().isEmpty()) {
                obs_log(LOG_INFO, "No access token found. Starting Google OAuth flow.");

                start_google_oauth_flow(
                    &dialog,
                    btnUpload,
                    btnCancel,
                    progressBar
                );

                return;
            }

            obs_log(LOG_INFO, "Access token found. Starting upload.");

            start_upload(
                &dialog,
                btnUpload,
                btnCancel,
                progressBar,
                accessToken
            );
        }
    );

    dialog.exec();
}