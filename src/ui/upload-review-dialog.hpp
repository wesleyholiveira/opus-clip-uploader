#pragma once

#include "models/curation-settings.hpp"
#include "models/transcript.hpp"
#include "curation/feedback/curation-feedback-store.hpp"

#include <QDialog>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QVector>

class QObject;
class QEvent;

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressDialog;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTableWidgetItem;
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

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	QString videoPath;
	CurationSettings initialSettings;
	bool advancedSettingsOnly = false;

	VideoMarkerEditor *videoEditor = nullptr;
	QTableWidget *clipTable = nullptr;
	QLabel *creditEstimateLabel = nullptr;
	QLineEdit *topicKeywordsInput = nullptr;
	QPlainTextEdit *opusPromptInput = nullptr;
	QComboBox *genreInput = nullptr;
	QComboBox *curationPresetInput = nullptr;
	QComboBox *modelInput = nullptr;
	QComboBox *clipLengthInput = nullptr;
	QComboBox *sourceLanguageInput = nullptr;
	QComboBox *transcriptionLanguageInput = nullptr;
	QCheckBox *skipCurateInput = nullptr;
	QPushButton *suggestClipRangesButton = nullptr;
	QPushButton *finishReviewButton = nullptr;
	QProgressDialog *semanticSuggestionProgressDialog = nullptr;
	bool semanticSuggestionInProgress = false;
	int semanticSuggestionProgressGeneration = 0;
	Curation::Feedback::FeedbackSuggestionSnapshot lastSemanticSuggestion;
	QString lastSemanticSuggestionTrainingProfile;
	QMap<int, QString> explicitReviewDecisions;
	QMap<int, QJsonObject> explicitReviewFeedbackDetails;
	QMap<int, QString> diagnosticReviewStates;
	QMap<int, ClipDuration> diagnosticEditedRanges;
	QMap<int, int> diagnosticFeedbackSuggestedIndices;
	bool boundaryFeedbackSaved = false;
	bool updatingClipTable = false;
	bool diagnosticReviewMode = false;
	bool restoredReviewedPositiveMarkersFromMemory = false;
	bool feedbackTranscriptLoaded = false;
	RecordingTranscript feedbackTranscript;

	void refreshClipTable(const QVector<ClipDuration> &ranges);
	void refreshDiagnosticCandidateTable();
	void showDiagnosticCandidateTable();
	void showReviewMarkerTable();
	bool isDiagnosticTableRow(int row) const;
	bool diagnosticRangeForTableRow(int row, ClipDuration *outRange, QJsonObject *outDiagnostic = nullptr) const;
	void applyEditedDiagnosticRangeTime(int row, int column, const QString &rawValue);
	ClipDuration normalizedDiagnosticRange(const ClipDuration &range) const;
	QVector<int> selectedDiagnosticTableRows() const;
	int diagnosticIndexForTableRow(int row) const;
	QString diagnosticReviewState(int diagnosticIndex) const;
	QString pendingExplicitFeedbackDecisionForRange(const ClipDuration &range, int *outSuggestedIndex = nullptr) const;
	bool hasReviewDiagnosticCandidates() const;
	void updateDiagnosticModeControls();
	void updateSuggestClipRangesButtonState();
	void seekReviewTableRowStart(int row);
	void seekReviewTableRowEnd(int row);
	void handleClipTableItemChanged(QTableWidgetItem *item);
	void applyEditedRangeTime(int row, int column, const QString &rawValue);
	void setClipRangeAtIndex(int row, double startSec, double endSec, bool seekToChangedBoundary);
	void nudgeSelectedRangeOrBoundary(double deltaSec);
	void removeSelectedRangeWithoutFeedbackDecision();
	void exportReviewMarkers();
	void importReviewMarkers();
	void applyImportedReviewRanges(const QVector<ClipDuration> &ranges, const QString &sourcePath);
	void applyCurationSettings(const CurationSettings &settings);
	void updateCreditEstimate(const QVector<ClipDuration> &ranges);
	void requestSemanticClipSuggestions();
	void showSemanticSuggestionProgressDialog();
	void updateSemanticSuggestionProgress(const ReviewScoringProgressUpdate &progress);
	void closeSemanticSuggestionProgressDialog();
	void applySemanticClipSuggestionResult(const ReviewScoringPreparationResult &result);
	bool restoreReviewedPositiveMarkersFromFeedbackIfEmpty(const QString &reason);
	void captureInitialSemanticSuggestion();
	void ensureReviewFeedbackSnapshot(const QString &source);
	void saveBoundaryFeedback(const QString &eventName);
	void finishReviewFeedback();
	void showMarkerDiagnostics(int row);
	QJsonObject diagnosticForRange(const ClipDuration &range, int suggestedIndex = -1) const;
	bool collectStructuredFeedback(int row, const QString &decision, QJsonObject *outFeedback);
	void setClipReviewDecision(int row, const QString &decision);
	void setDiagnosticReviewDecision(int row, const QString &decision);
	int ensureDiagnosticFeedbackSuggestion(int diagnosticIndex, const ClipDuration &range,
					       const QJsonObject &diagnostic);
	int suggestedIndexForRange(const ClipDuration &range) const;
	int ensureFeedbackSuggestionForRange(const ClipDuration &range, const QString &source);
	bool loadFeedbackTranscriptIfAvailable();
	QJsonObject reevaluateFeedbackRange(const ClipDuration &range, int suggestedIndex, const QString &source);
	void upsertFeedbackDiagnostic(int suggestedIndex, const QJsonObject &diagnostic);
	void loadSavedCurationOptions();
	void saveCurationOptions() const;
	QString currentReviewSettingsKey() const;
};
