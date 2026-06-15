#pragma once

class QLabel;
class QDialog;
class QProgressBar;
class QPushButton;
class QString;
struct CurationSettings;

void start_upload(QDialog *dialog, QPushButton *btnUpload, QPushButton *btnCancel, QProgressBar *progressBar,
		  QLabel *uploadStatusLabel, const QString &apiKey, const CurationSettings &curationSettings);
