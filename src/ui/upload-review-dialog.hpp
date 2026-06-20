#pragma once

#include "models/curation-settings.hpp"

#include <QDialog>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressDialog;
class QPushButton;
class QTableWidget;
class VideoMarkerEditor;
class QWidget;
struct ReviewScoringPreparationResult;
struct ReviewScoringProgressUpdate;

class UploadReviewDialog : public QDialog {
	Q_OBJECT

public:
	explicit UploadReviewDialog(const QString &videoPath, QWidget *parent = nullptr);
	UploadReviewDialog(const QString &videoPath, const CurationSettings &initialCurationSettings,
			   bool showAdvancedSettingsOnly, QWidget *parent = nullptr);

	CurationSettings curationSettings() const;

private:
	QString videoPath;
	CurationSettings initialSettings;
	bool advancedSettingsOnly = false;

	VideoMarkerEditor *videoEditor = nullptr;
	QTableWidget *clipTable = nullptr;
	QLabel *creditEstimateLabel = nullptr;
	QLineEdit *topicKeywordsInput = nullptr;
	QComboBox *genreInput = nullptr;
	QComboBox *curationPresetInput = nullptr;
	QComboBox *modelInput = nullptr;
	QComboBox *clipLengthInput = nullptr;
	QComboBox *sourceLanguageInput = nullptr;
	QComboBox *transcriptionLanguageInput = nullptr;
	QCheckBox *skipCurateInput = nullptr;
	QPlainTextEdit *customPromptInput = nullptr;
	QPushButton *suggestClipRangesButton = nullptr;
	QProgressDialog *semanticSuggestionProgressDialog = nullptr;
	bool semanticSuggestionInProgress = false;
	int semanticSuggestionProgressGeneration = 0;

	void refreshClipTable(const QVector<ClipDuration> &ranges);
	void applyCurationSettings(const CurationSettings &settings);
	void updateCreditEstimate(const QVector<ClipDuration> &ranges);
	void requestSemanticClipSuggestions();
	void showSemanticSuggestionProgressDialog();
	void updateSemanticSuggestionProgress(const ReviewScoringProgressUpdate &progress);
	void closeSemanticSuggestionProgressDialog();
	void applySemanticClipSuggestionResult(const ReviewScoringPreparationResult &result);
	void loadSavedCurationOptions();
	void saveCurationOptions() const;
	QString currentReviewSettingsKey() const;
};

inline bool review_generated_prompt_before_upload(QWidget *parent, const QString &videoPath, CurationSettings &settings)
{
	if (settings.aiPrompt.trimmed().isEmpty())
		return true;

	UploadReviewDialog promptReviewDialog(videoPath, settings, true, parent);
	if (promptReviewDialog.exec() != QDialog::Accepted)
		return false;

	settings = promptReviewDialog.curationSettings();
	return true;
}
