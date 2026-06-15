#pragma once

#include "models/curation-settings.hpp"

#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QTableWidget;
class VideoMarkerEditor;

class UploadReviewDialog : public QDialog {
	Q_OBJECT

public:
	explicit UploadReviewDialog(const QString &videoPath, QWidget *parent = nullptr);

	CurationSettings curationSettings() const;

private:
	QString videoPath;

	VideoMarkerEditor *videoEditor = nullptr;
	QTableWidget *clipTable = nullptr;
	QLineEdit *topicKeywordsInput = nullptr;
	QComboBox *genreInput = nullptr;
	QComboBox *modelInput = nullptr;
	QCheckBox *skipCurateInput = nullptr;
	QPlainTextEdit *customPromptInput = nullptr;

	void refreshClipTable(const QVector<ClipDuration> &ranges);
	void loadSavedCurationOptions();
	void saveCurationOptions() const;
};
