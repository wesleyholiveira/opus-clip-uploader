#include <obs-module.h>

#include "ui/upload-review-dialog.hpp"

#include "curation/curation-preset-profile.hpp"

#include "ui/review/upload-review-dialog-utils.hpp"
#include "ui/video-marker-editor.hpp"

#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QByteArray>
#include <QFileInfo>
#include <QIODevice>

#include <algorithm>

using namespace ReviewDialogUtils;

void UploadReviewDialog::exportReviewMarkers()
{
	if (!videoEditor)
		return;

	const QVector<ClipDuration> ranges = videoEditor->clipRanges();
	if (ranges.isEmpty()) {
		QMessageBox::information(this, QStringLiteral("Export markers"),
					 QStringLiteral("There are no review markers to export."));
		return;
	}

	const QString baseName = QFileInfo(videoPath).completeBaseName().trimmed().isEmpty()
					 ? QStringLiteral("clip-markers")
					 : QFileInfo(videoPath).completeBaseName().trimmed();
	const QString defaultPath =
		QFileInfo(videoPath).absoluteDir().filePath(baseName + QStringLiteral("-markers.json"));
	const QString filePath = QFileDialog::getSaveFileName(
		this, QStringLiteral("Export review markers"), defaultPath,
		QStringLiteral("Marker JSON (*.json);;Text files (*.txt);;All files (*.*)"));
	if (filePath.trimmed().isEmpty())
		return;

	QJsonArray rangesJson;
	for (int i = 0; i < ranges.size(); ++i) {
		const ClipDuration &range = ranges.at(i);
		QJsonObject item;
		item.insert(QStringLiteral("index"), i);
		item.insert(QStringLiteral("start_sec"), range.startSec);
		item.insert(QStringLiteral("end_sec"), range.endSec);
		item.insert(QStringLiteral("duration_sec"), std::max(0.0, range.endSec - range.startSec));
		item.insert(QStringLiteral("start"), formatEditableReviewTime(range.startSec));
		item.insert(QStringLiteral("end"), formatEditableReviewTime(range.endSec));
		rangesJson.append(item);
	}

	QJsonArray markersJson;
	const QVector<double> markers = videoEditor->markerPositions();
	for (double marker : markers)
		markersJson.append(marker);

	QJsonObject root;
	root.insert(QStringLiteral("schema_version"), 1);
	root.insert(QStringLiteral("type"), QStringLiteral("clip-cropper-review-markers"));
	root.insert(QStringLiteral("video_path"), videoPath);
	root.insert(QStringLiteral("video_file"), QFileInfo(videoPath).fileName());
	root.insert(QStringLiteral("exported_at_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
	root.insert(QStringLiteral("duration_sec"),
		    videoEditor->durationMilliseconds() > 0 ? videoEditor->durationMilliseconds() / 1000.0 : 0.0);
	root.insert(QStringLiteral("markers_sec"), markersJson);
	root.insert(QStringLiteral("ranges"), rangesJson);

	QFile file(filePath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
		QMessageBox::warning(this, QStringLiteral("Export markers"),
				     QStringLiteral("Could not write marker file:\n%1").arg(file.errorString()));
		return;
	}

	file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	file.close();
	blog(LOG_INFO, "Exported review markers. video=%s path=%s ranges=%d markers=%d", videoPath.toUtf8().constData(),
	     filePath.toUtf8().constData(), static_cast<int>(ranges.size()), static_cast<int>(markers.size()));
	QMessageBox::information(this, QStringLiteral("Export markers"),
				 QStringLiteral("Exported %1 marker range(s).").arg(ranges.size()));
}

void UploadReviewDialog::importReviewMarkers()
{
	if (!videoEditor)
		return;

	const QString startDir = QFileInfo(videoPath).absolutePath();
	const QString filePath =
		QFileDialog::getOpenFileName(this, QStringLiteral("Import review markers"), startDir,
					     QStringLiteral("Marker files (*.json *.txt *.csv);;All files (*.*)"));
	if (filePath.trimmed().isEmpty())
		return;

	QFile file(filePath);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		QMessageBox::warning(this, QStringLiteral("Import markers"),
				     QStringLiteral("Could not read marker file:\n%1").arg(file.errorString()));
		return;
	}

	const QByteArray data = file.readAll();
	file.close();

	QVector<ClipDuration> importedRanges;
	const double durationSec =
		videoEditor->durationMilliseconds() > 0 ? videoEditor->durationMilliseconds() / 1000.0 : 0.0;
	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
	if (parseError.error == QJsonParseError::NoError && (!document.isNull()))
		importedRanges = parseReviewRangesFromJson(document, durationSec);
	else
		importedRanges = parseReviewRangesFromPlainText(QString::fromUtf8(data), durationSec);

	if (importedRanges.isEmpty()) {
		QMessageBox::warning(this, QStringLiteral("Import markers"),
				     QStringLiteral("No valid marker ranges were found in this file."));
		return;
	}

	if (!videoEditor->clipRanges().isEmpty()) {
		const QMessageBox::StandardButton answer = QMessageBox::question(
			this, QStringLiteral("Import markers"),
			QStringLiteral("Replace the current review markers with %1 imported range(s)?")
				.arg(importedRanges.size()),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
		if (answer != QMessageBox::Yes)
			return;
	}

	applyImportedReviewRanges(importedRanges, filePath);
}

void UploadReviewDialog::applyImportedReviewRanges(const QVector<ClipDuration> &ranges, const QString &sourcePath)
{
	if (!videoEditor || ranges.isEmpty())
		return;

	lastSemanticSuggestion = Curation::Feedback::FeedbackSuggestionSnapshot{};
	const CurationSettings settings = curationSettings();
	lastSemanticSuggestionTrainingProfile = Curation::resolvePresetProfileId(settings, settings.aiPrompt);
	explicitReviewDecisions.clear();
	explicitReviewFeedbackDetails.clear();
	boundaryFeedbackSaved = false;

	videoEditor->setMarkerPositions(markerPositionsFromRanges(ranges));
	lastSemanticSuggestion.ranges = videoEditor->clipRanges();
	lastSemanticSuggestion.contentId = Curation::Feedback::CurationFeedbackStore::fileContentId(videoPath);
	lastSemanticSuggestion.contentIdAliases.clear();
	lastSemanticSuggestion.source = QStringLiteral("imported_review_markers");
	lastSemanticSuggestion.summary = QStringLiteral("review markers imported from file");
	lastSemanticSuggestion.candidateDiagnostics = QJsonArray{};
	diagnosticReviewStates.clear();
	diagnosticReviewMode = false;

	if (finishReviewButton) {
		finishReviewButton->setEnabled(true);
		finishReviewButton->setText(QStringLiteral("Finish review (save feedback)"));
	}
	if (clipTable && clipTable->rowCount() > 0)
		clipTable->selectRow(0);
	videoEditor->selectRange(0);
	blog(LOG_INFO, "Imported review markers. video=%s path=%s ranges=%d", videoPath.toUtf8().constData(),
	     sourcePath.toUtf8().constData(), static_cast<int>(lastSemanticSuggestion.ranges.size()));
}
