#include "ui/upload-review-dialog.hpp"

#include "ui/review/upload-review-dialog-utils.hpp"
#include "ui/ui-common.hpp"
#include "curation/curation-preset.hpp"
#include "ui/video-marker-editor.hpp"
#include "utils/config.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using namespace ReviewDialogUtils;

void UploadReviewDialog::updateCreditEstimate(const QVector<ClipDuration> &ranges)
{
	if (!creditEstimateLabel)
		return;

	const double totalSeconds = totalRangeDurationSeconds(ranges);
	const int estimatedCredits = estimate_opus_credits(totalSeconds);

	if (estimatedCredits <= 0) {
		creditEstimateLabel->setText(obsText("Label.OpusCreditsNoSelection"));
		return;
	}

	creditEstimateLabel->setText(obsText("Label.OpusCreditsSelectedEstimate")
					     .arg(estimatedCredits)
					     .arg(format_duration_seconds(totalSeconds)));
}

void UploadReviewDialog::loadSavedCurationOptions()
{
	const QString key = currentReviewSettingsKey();
	if (key.isEmpty())
		return;

	const QString raw = PluginConfig::getValue(key).trimmed();
	if (raw.isEmpty())
		return;

	const QJsonDocument document = QJsonDocument::fromJson(raw.toUtf8());
	if (!document.isObject())
		return;

	const QJsonObject root = document.object();

	if (topicKeywordsInput && root.contains(QStringLiteral("topicKeywords"))) {
		QStringList keywords;
		const QJsonValue value = root.value(QStringLiteral("topicKeywords"));
		if (value.isArray()) {
			const QJsonArray array = value.toArray();
			for (const QJsonValue &item : array) {
				const QString keyword = item.toString().trimmed();
				if (!keyword.isEmpty())
					keywords.append(keyword);
			}
		} else {
			const QString rawKeywords = value.toString().trimmed();
			if (!rawKeywords.isEmpty())
				keywords = rawKeywords.split(QStringLiteral(","), Qt::SkipEmptyParts);
		}

		for (QString &keyword : keywords)
			keyword = keyword.trimmed();

		topicKeywordsInput->setText(keywords.join(QStringLiteral(", ")));
	}

	setComboCurrentTextIfExists(genreInput, root.value(QStringLiteral("genre")).toString());
	setComboCurrentDataIfExists(
		curationPresetInput,
		CurationPreset::normalizeId(
			root.value(QStringLiteral("curationPreset")).toString(QStringLiteral("auto"))));
	setOpusModelOrDefault(modelInput, root.value(QStringLiteral("model")).toString());
	setComboCurrentDataIfExists(clipLengthInput,
				    root.value(QStringLiteral("clipLengthPreset")).toString(QStringLiteral("Medium")));
	setComboCurrentDataIfExists(
		sourceLanguageInput,
		normalizeLanguageSetting(
			root.value(QStringLiteral("sourceLanguage"))
				.toString(PluginConfig::getValue(QString::fromLatin1(CONFIG_OPUS_SOURCE_LANGUAGE),
								 QString::fromLatin1(LANGUAGE_AUTO)))));
	setComboCurrentDataIfExists(transcriptionLanguageInput,
				    normalizeLanguageSetting(root.value(QStringLiteral("transcriptionLanguage"))
								     .toString(QString::fromLatin1(LANGUAGE_AUTO))));

	if (skipCurateInput && root.contains(QStringLiteral("skipCurate")))
		skipCurateInput->setChecked(root.value(QStringLiteral("skipCurate")).toBool(false));

	if (opusPromptInput && root.contains(QStringLiteral("aiPrompt")))
		opusPromptInput->setPlainText(root.value(QStringLiteral("aiPrompt")).toString().trimmed());
}

void UploadReviewDialog::saveCurationOptions() const
{
	const QString key = currentReviewSettingsKey();
	if (key.isEmpty())
		return;

	QJsonObject root;
	root.insert(QStringLiteral("videoFileName"), QFileInfo(videoPath).fileName());
	root.insert(QStringLiteral("videoPath"), videoPath);
	root.insert(QStringLiteral("reviewSettingsKey"), key);
	root.insert(QStringLiteral("originalVideoDurationSec"),
		    advancedSettingsOnly ? initialSettings.originalVideoDurationSec
					 : (videoEditor ? videoEditor->durationMilliseconds() / 1000.0 : 0.0));

	QJsonArray ranges;
	const QVector<ClipDuration> currentRanges =
		advancedSettingsOnly ? initialSettings.clipDurations
				     : (videoEditor ? videoEditor->clipRanges() : QVector<ClipDuration>{});
	for (const ClipDuration &range : currentRanges) {
		QJsonObject item;
		item.insert(QStringLiteral("start"), range.startSec);
		item.insert(QStringLiteral("end"), range.endSec);
		ranges.append(item);
	}
	root.insert(QStringLiteral("clipDurations"), ranges);

	QJsonArray topicKeywords;
	if (topicKeywordsInput) {
		const QStringList keywords = topicKeywordsInput->text().split(QStringLiteral(","), Qt::SkipEmptyParts);
		for (QString keyword : keywords) {
			keyword = keyword.trimmed();
			if (!keyword.isEmpty())
				topicKeywords.append(keyword);
		}
	}
	root.insert(QStringLiteral("topicKeywords"), topicKeywords);
	root.insert(QStringLiteral("genre"), genreInput ? genreInput->currentText() : QStringLiteral("Auto"));
	root.insert(QStringLiteral("curationPreset"),
		    curationPresetInput ? curationPresetInput->currentData().toString() : QStringLiteral("auto"));
	root.insert(QStringLiteral("model"), modelInput ? modelInput->currentText() : QStringLiteral("ClipAnything"));
	root.insert(QStringLiteral("clipLengthPreset"),
		    clipLengthInput ? clipLengthInput->currentData().toString() : QStringLiteral("Medium"));
	root.insert(QStringLiteral("skipCurate"), skipCurateInput ? skipCurateInput->isChecked() : false);
	root.insert(QStringLiteral("sourceLanguage"),
		    sourceLanguageInput ? normalizeLanguageSetting(sourceLanguageInput->currentData().toString())
					: QString::fromLatin1(LANGUAGE_AUTO));
	root.insert(QStringLiteral("transcriptionLanguage"),
		    transcriptionLanguageInput
			    ? normalizeLanguageSetting(transcriptionLanguageInput->currentData().toString())
			    : QString::fromLatin1(LANGUAGE_AUTO));
	root.insert(QStringLiteral("aiPrompt"), opusPromptInput ? opusPromptInput->toPlainText().trimmed() : QString{});

	PluginConfig::setValue(key, QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
}

QString UploadReviewDialog::currentReviewSettingsKey() const
{
	if (advancedSettingsOnly && !initialSettings.reviewSettingsKey.trimmed().isEmpty())
		return initialSettings.reviewSettingsKey;

	const QString rangeKey =
		advancedSettingsOnly
			? reviewRangeKeyFromRanges(initialSettings.clipDurations)
			: reviewRangeKeyFromMarkers(videoEditor ? videoEditor->markerPositions() : QVector<double>{});
	return reviewSettingsConfigKey(videoPath, rangeKey);
}

CurationSettings UploadReviewDialog::curationSettings() const
{
	CurationSettings settings = advancedSettingsOnly ? initialSettings : CurationSettings{};

	const QVector<ClipDuration> ranges =
		advancedSettingsOnly ? initialSettings.clipDurations
				     : (videoEditor ? videoEditor->clipRanges() : QVector<ClipDuration>{});

	settings.reviewSettingsKey = currentReviewSettingsKey();
	settings.originalVideoDurationSec =
		advancedSettingsOnly ? initialSettings.originalVideoDurationSec
				     : (videoEditor ? videoEditor->durationMilliseconds() / 1000.0 : 0.0);
	settings.rangeStartSec = ranges.isEmpty() ? settings.rangeStartSec : ranges.first().startSec;
	settings.rangeEndSec = ranges.isEmpty() ? settings.rangeEndSec : ranges.last().endSec;
	settings.clipDurations.clear();
	for (const auto &range : ranges)
		settings.clipDurations.append(range);
	settings.uploadClipRangesIndependently = settings.clipDurations.size() > 1;

	settings.genre = genreInput->currentText();
	settings.curationPreset = curationPresetInput ? curationPresetInput->currentData().toString()
						      : QStringLiteral("auto");
	settings.skipCurate = skipCurateInput->isChecked();
	settings.model = modelInput->currentText();
	settings.clipLengthPreset = clipLengthInput ? clipLengthInput->currentData().toString()
						    : QStringLiteral("Medium");
	settings.sourceLanguage = sourceLanguageInput
					  ? normalizeLanguageSetting(sourceLanguageInput->currentData().toString())
					  : QString::fromLatin1(LANGUAGE_AUTO);
	settings.transcriptionLanguage =
		transcriptionLanguageInput
			? normalizeLanguageSetting(transcriptionLanguageInput->currentData().toString())
			: QString::fromLatin1(LANGUAGE_AUTO);
	settings.aiPrompt = opusPromptInput ? opusPromptInput->toPlainText().trimmed() : QString{};

	settings.topicKeywords.clear();
	const QStringList keywords = topicKeywordsInput->text().split(",", Qt::SkipEmptyParts);
	for (QString keyword : keywords)
		settings.topicKeywords.append(keyword.trimmed());

	return settings;
}
