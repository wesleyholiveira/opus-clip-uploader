#include "gpt/gpt-prompt-client.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QStringList>
#include <QMap>

#include <obs-module.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include "gpt/gpt-prompt-store.hpp"
#include "gpt/openai-chat-payload.hpp"
#include "curation/analysis/named-reference-detector.hpp"
#include "curation/compiler/opus-prompt-compiler.hpp"
#include "curation/compiler/prompt-quality-gate.hpp"
#include "curation/curation-preset.hpp"
#include "curation/curation-rules.hpp"
#include "curation/curation-signals.hpp"
#include "curation/opus-prompt-renderer.hpp"
#include "utils/config.hpp"

static constexpr const char *OPENAI_CHAT_COMPLETIONS_URL = "https://api.openai.com/v1/chat/completions";
static constexpr const char *SEMANTIC_GATE_FAILURE_PREFIX = "NO_STRONG_CLIP_FOUND:";
static constexpr const char *PROMPT_GENERATION_BLOCKED_PREFIX = "GPT_PROMPT_BLOCKED:";
static constexpr const char *OPUS_PROMPT_PREFIX = "OPUS_PROMPT:";
static constexpr const char *REPAIR_TEMPLATE_SECTION = "repair_opus_prompt";

static constexpr const char *CONFIG_GPT_INPUT_TEMPLATE_PREFIX = "gpt.input_text_template.v50.";
static constexpr const char *CONFIG_OPUS_SOURCE_LANGUAGE = "opus_source_lang";
static constexpr const char *CONFIG_GPT_PROMPT_REPAIR_MODE = "gpt.prompt_repair_mode";
static constexpr const char *CONFIG_GPT_TRANSCRIPT_CONTEXT_PADDING_SEC = "gpt_transcript_context_padding_sec";
static constexpr double DEFAULT_GPT_TRANSCRIPT_CONTEXT_PADDING_SEC = 60.0;
static constexpr const char *GPT_PROMPT_REPAIR_MODE_GPT = "gpt";
static QString loadTemplateSectionFromRuntimeFile(const QString &sectionName, const QString &logLabel);

static double configuredGptTranscriptContextPaddingSeconds()
{
	bool ok = false;
	const double value = PluginConfig::getValue(QString::fromLatin1(CONFIG_GPT_TRANSCRIPT_CONTEXT_PADDING_SEC),
						    QString::number(DEFAULT_GPT_TRANSCRIPT_CONTEXT_PADDING_SEC, 'f', 0))
				     .toDouble(&ok);

	if (!ok || !std::isfinite(value))
		return DEFAULT_GPT_TRANSCRIPT_CONTEXT_PADDING_SEC;

	return std::clamp(value, 0.0, 600.0);
}

static bool gptPromptRepairEnabled();
static QString normalizePromptLanguage(QString sourceLanguage)
{
	sourceLanguage = sourceLanguage.trimmed().toLower();

	if (sourceLanguage == QStringLiteral("pt") || sourceLanguage == QStringLiteral("pt-br") ||
	    sourceLanguage == QStringLiteral("portuguese"))
		return QStringLiteral("pt");

	if (sourceLanguage == QStringLiteral("en") || sourceLanguage == QStringLiteral("en-us") ||
	    sourceLanguage == QStringLiteral("english"))
		return QStringLiteral("en");

	return QStringLiteral("auto");
}

static bool usePortuguesePromptText(const QString &sourceLanguage)
{
	return normalizePromptLanguage(sourceLanguage) == QStringLiteral("pt");
}

static QString promptSourceLanguageForCuration(const CurationSettings &curationSettings)
{
	QString sourceLanguage = curationSettings.sourceLanguage.trimmed();
	if (sourceLanguage.isEmpty())
		sourceLanguage = PluginConfig::getValue(QString::fromLatin1(CONFIG_OPUS_SOURCE_LANGUAGE),
							QStringLiteral("auto"));

	return sourceLanguage.trimmed().isEmpty() ? QStringLiteral("auto") : sourceLanguage;
}

static QString formatSelectedRanges(const CurationSettings &curationSettings, const QString &sourceLanguage)
{
	QString text;
	const bool portuguese = usePortuguesePromptText(sourceLanguage);

	if (!curationSettings.clipDurations.isEmpty()) {
		text += portuguese
				? QStringLiteral("Ranges/marcações escolhidos pelo usuário, em ordem de prioridade:\n")
				: QStringLiteral("User-selected ranges/markers, in priority order:\n");
		int index = 1;
		for (const ClipDuration &range : curationSettings.clipDurations) {
			const QString rangeLine = portuguese ? QStringLiteral("%1. %2s-%3s\n")
							     : QStringLiteral("%1. %2s-%3s\n");
			text += rangeLine.arg(index++).arg(range.startSec, 0, 'f', 3).arg(range.endSec, 0, 'f', 3);
		}
	} else {
		text += portuguese
				? QStringLiteral(
					  "Não há ranges/marcações específicos escolhidos pelo usuário. Escolha os melhores trechos usando a transcrição.\n")
				: QStringLiteral(
					  "There are no specific user-selected ranges/markers. Choose the best moments using the transcript.\n");
	}

	return text.trimmed();
}

static QString formatTopicKeywords(const CurationSettings &curationSettings, const QString &sourceLanguage)
{
	if (curationSettings.topicKeywords.isEmpty()) {
		return usePortuguesePromptText(sourceLanguage)
			       ? QStringLiteral("Nenhuma keyword/tema específico foi informado pelo usuário.")
			       : QStringLiteral("No specific keyword/topic was provided by the user.");
	}

	return curationSettings.topicKeywords.join(QStringLiteral(", ")).trimmed();
}

static bool gptPromptRepairEnabled()
{
	const QString mode =
		PluginConfig::getValue(QString::fromLatin1(CONFIG_GPT_PROMPT_REPAIR_MODE), QStringLiteral("off"))
			.trimmed()
			.toLower();
	return mode == QString::fromLatin1(GPT_PROMPT_REPAIR_MODE_GPT);
}

static QString formatTranscript(const RecordingTranscript &transcript)
{
	QString text;

	for (const TranscriptSegment &segment : transcript.segments) {
		if (segment.text.trimmed().isEmpty())
			continue;

		text += QStringLiteral("[%1s-%2s] %3\n")
				.arg(segment.startSec, 0, 'f', 2)
				.arg(segment.endSec, 0, 'f', 2)
				.arg(segment.text.trimmed());
	}

	return text.trimmed();
}

static bool transcriptSegmentIntersectsRange(const TranscriptSegment &segment, double startSec, double endSec)
{
	return segment.endSec > startSec && segment.startSec < endSec;
}

static QString formatExpandedContextTranscript(const RecordingTranscript &transcript,
					       const CurationSettings &curationSettings)
{
	const double contextPaddingSec = configuredGptTranscriptContextPaddingSeconds();
	constexpr int maxContextChars = 60000;
	QString text;
	const QVector<ClipDuration> ranges = [&curationSettings]() {
		QVector<ClipDuration> result;
		for (const ClipDuration &range : curationSettings.clipDurations) {
			if (range.endSec > range.startSec)
				result.append(range);
		}
		if (result.isEmpty() && curationSettings.rangeEndSec > curationSettings.rangeStartSec)
			result.append({curationSettings.rangeStartSec, curationSettings.rangeEndSec});
		return result;
	}();

	for (const TranscriptSegment &segment : transcript.segments) {
		if (segment.text.trimmed().isEmpty())
			continue;

		bool includeSegment = ranges.isEmpty();
		for (const ClipDuration &range : ranges) {
			const double expandedStartSec = std::max(0.0, range.startSec - contextPaddingSec);
			const double expandedEndSec = range.endSec + contextPaddingSec;
			if (transcriptSegmentIntersectsRange(segment, expandedStartSec, expandedEndSec)) {
				includeSegment = true;
				break;
			}
		}

		if (!includeSegment)
			continue;

		text += QStringLiteral("[%1s-%2s] %3\n")
				.arg(segment.startSec, 0, 'f', 2)
				.arg(segment.endSec, 0, 'f', 2)
				.arg(segment.text.trimmed());

		if (text.size() >= maxContextChars) {
			text += QStringLiteral("\n[context truncated to keep the GPT request focused]\n");
			break;
		}
	}

	return text.trimmed();
}

static QString appendPreviewText(QString currentText, const QString &segmentText, int maxChars)
{
	const QString trimmed = segmentText.trimmed();
	if (trimmed.isEmpty() || currentText.size() >= maxChars)
		return currentText;

	if (!currentText.isEmpty())
		currentText += QLatin1Char(' ');

	const qsizetype remaining = static_cast<qsizetype>(maxChars) - currentText.size();
	if (trimmed.size() <= remaining)
		currentText += trimmed;
	else
		currentText += trimmed.left(std::max<qsizetype>(0, remaining)).trimmed() + QStringLiteral("...");

	return currentText;
}

static QString formatSemanticTimelineContext(const RecordingTranscript &transcript)
{
	if (transcript.segments.isEmpty())
		return QStringLiteral("No full-transcript timeline context available.");

	constexpr double windowSizeSec = 300.0;
	constexpr int maxWindowPreviewChars = 520;
	QString timeline;
	double windowStartSec =
		std::max(0.0, std::floor(transcript.segments.first().startSec / windowSizeSec) * windowSizeSec);
	double windowEndSec = windowStartSec + windowSizeSec;
	QString windowText;

	auto flushWindow = [&timeline, &windowText, &windowStartSec, &windowEndSec]() {
		const QString preview = windowText.trimmed();
		if (!preview.isEmpty()) {
			timeline += QStringLiteral("[%1s-%2s] %3\n")
					    .arg(windowStartSec, 0, 'f', 0)
					    .arg(windowEndSec, 0, 'f', 0)
					    .arg(preview);
		}
		windowText.clear();
	};

	for (const TranscriptSegment &segment : transcript.segments) {
		if (segment.text.trimmed().isEmpty())
			continue;

		while (segment.startSec >= windowEndSec) {
			flushWindow();
			windowStartSec = windowEndSec;
			windowEndSec += windowSizeSec;
		}

		windowText = appendPreviewText(windowText, segment.text, maxWindowPreviewChars);
	}

	flushWindow();
	return timeline.trimmed();
}

static QString renderInputTemplate(QString templateText, const QString &videoPath,
				   const RecordingTranscript &fullTranscript,
				   const RecordingTranscript &selectedRangeTranscript,
				   const CurationSettings &curationSettings, const QString &sourceLanguage)
{
	templateText.replace(QStringLiteral("{{video_file}}"), QFileInfo(videoPath).fileName());
	templateText.replace(QStringLiteral("{{range_start_sec}}"),
			     QString::number(curationSettings.rangeStartSec, 'f', 3));
	templateText.replace(QStringLiteral("{{range_end_sec}}"),
			     QString::number(curationSettings.rangeEndSec, 'f', 3));
	templateText.replace(QStringLiteral("{{selected_ranges}}"),
			     formatSelectedRanges(curationSettings, sourceLanguage));
	templateText.replace(QStringLiteral("{{topic_keywords}}"),
			     formatTopicKeywords(curationSettings, sourceLanguage));
	templateText.replace(QStringLiteral("{{genre}}"), curationSettings.genre.trimmed().isEmpty()
								  ? QStringLiteral("Auto")
								  : curationSettings.genre.trimmed());
	const Curation::Intent intent = Curation::resolveIntent(curationSettings, selectedRangeTranscript);
	blog(LOG_INFO,
	     "Resolved GPT curation preset. requested=%s resolved=%s viewerSignals=%s scope=%s selectedSegments=%d selectedDurationSec=%.3f",
	     curationSettings.curationPreset.toUtf8().constData(), intent.resolvedPresetId.toUtf8().constData(),
	     intent.viewerSignals ? "true" : "false", intent.scope.toUtf8().constData(),
	     static_cast<int>(selectedRangeTranscript.segments.size()), intent.selectedDurationSec);
	templateText.replace(QStringLiteral("{{curation_preset}}"),
			     CurationPreset::labelForId(intent.resolvedPresetId));
	templateText.replace(QStringLiteral("{{curation_preset_context}}"),
			     CurationPreset::gptContextForId(intent.resolvedPresetId));
	templateText.replace(QStringLiteral("{{named_references}}"),
			     Curation::formatImportantNamedReferences(selectedRangeTranscript));
	templateText.replace(QStringLiteral("{{source_language}}"), normalizePromptLanguage(sourceLanguage));
	templateText.replace(QStringLiteral("{{clip_length_preset}}"), curationSettings.clipLengthPreset.trimmed());
	const double durationSec = Curation::selectedDurationSeconds(selectedRangeTranscript, curationSettings);
	templateText.replace(QStringLiteral("{{selected_duration_sec}}"), QString::number(durationSec, 'f', 3));
	templateText.replace(QStringLiteral("{{selected_duration_min}}"), QString::number(durationSec / 60.0, 'f', 2));
	templateText.replace(QStringLiteral("{{curation_scope}}"), Curation::scopeForDuration(durationSec));
	templateText.replace(QStringLiteral("{{semantic_timeline}}"), formatSemanticTimelineContext(fullTranscript));
	templateText.replace(QStringLiteral("{{context_transcript}}"),
			     formatExpandedContextTranscript(fullTranscript, curationSettings));
	templateText.replace(QStringLiteral("{{selected_range_transcript}}"),
			     formatTranscript(selectedRangeTranscript));
	templateText.replace(QStringLiteral("{{transcript}}"), formatTranscript(selectedRangeTranscript));

	return templateText.trimmed();
}

static QString fallbackRepairInputTextTemplate()
{
	return QStringLiteral(
		"You are repairing an Opus Clip ClipAnything prompt that violated predictability rules.\n"
		"Return only one line: OPUS_PROMPT: <repaired English prompt>\n"
		"Rewrite the prompt into exactly 3 concise sentences.\n"
		"Sentence 1 must start with Find and describe exactly one search target.\n"
		"Sentence 2 must start with Prioritize moments that and describe the opening plus the same continuous sequence.\n"
		"Sentence 3 must start with Prefer clips that and describe the ending/resolution plus one boundary criterion.\n"
		"Do not use especially, a colon topic catalog, broad OR chains, duration, timestamps, editing instructions, or more than one adjacent sub-arc.\n"
		"Do not assume the speaker\'s gender; use the speaker instead of his, her, he, or she unless the transcript itself makes it necessary.\n"
		"For short clip presets, prefer one smaller resolved local idea over a method + roadmap + tools + motivation bundle.\n"
		"If the selected range contains an important named person, creator, source, app, book, or method, preserve that name when it is needed to make pronouns or indirect references understandable.\n"
		"If a named reference is central to an unlock/method explanation, make the repaired target only that reference-backed method explanation and do not include daily routine, motivation, roadmap, tools, input, vocabulary, or practice unless it is directly inside the same reference-backed explanation.\n"
		"Do not write meta-instructions such as making sure a name is included; write the search target naturally.\n\n"
		"Detected issues: {{issues}}\n"
		"Curation scope: {{curation_scope}}\n"
		"Configured clip length preset: {{clip_length_preset}}\n"
		"User-selected ranges/markers:\n{{selected_ranges}}\n\n"
		"Desired keywords/topics from the user:\n{{topic_keywords}}\n\n"
		"Important named references detected in the selected range:\n{{named_references}}\n\n"
		"Previous OPUS_PROMPT:\n{{generated_opus_prompt}}\n\n"
		"Selected-range transcript:\n{{selected_range_transcript}}\n");
}

static QString renderRepairTemplate(QString templateText, const QString &generatedPrompt, const QStringList &issues,
				    const RecordingTranscript &selectedRangeTranscript,
				    const CurationSettings &curationSettings, const QString &sourceLanguage)
{
	const double durationSec = Curation::selectedDurationSeconds(selectedRangeTranscript, curationSettings);
	templateText.replace(QStringLiteral("{{generated_opus_prompt}}"), Curation::opusPromptPayload(generatedPrompt));
	templateText.replace(QStringLiteral("{{issues}}"), issues.join(QStringLiteral("; ")));
	templateText.replace(QStringLiteral("{{curation_scope}}"), Curation::scopeForDuration(durationSec));
	templateText.replace(QStringLiteral("{{clip_length_preset}}"), curationSettings.clipLengthPreset.trimmed());
	templateText.replace(QStringLiteral("{{selected_ranges}}"),
			     formatSelectedRanges(curationSettings, sourceLanguage));
	templateText.replace(QStringLiteral("{{topic_keywords}}"),
			     formatTopicKeywords(curationSettings, sourceLanguage));
	templateText.replace(QStringLiteral("{{named_references}}"),
			     Curation::formatImportantNamedReferences(selectedRangeTranscript));
	templateText.replace(QStringLiteral("{{selected_range_transcript}}"),
			     formatTranscript(selectedRangeTranscript));
	return templateText.trimmed();
}

static QString buildRepairInputText(const QString &generatedPrompt, const QStringList &issues,
				    const RecordingTranscript &selectedRangeTranscript,
				    const CurationSettings &curationSettings, const QString &sourceLanguage)
{
	QString repairTemplate = loadTemplateSectionFromRuntimeFile(QString::fromLatin1(REPAIR_TEMPLATE_SECTION),
								    QStringLiteral("GPT repair template"));
	if (repairTemplate.trimmed().isEmpty())
		repairTemplate = fallbackRepairInputTextTemplate();

	return renderRepairTemplate(repairTemplate, generatedPrompt, issues, selectedRangeTranscript, curationSettings,
				    sourceLanguage);
}

static QString normalizePromptTemplateLineEndings(QString text)
{
	text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
	text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
	return text.trimmed();
}

static QString promptTemplateSection(const QString &fileText, const QString &sectionName)
{
	QStringList lines = normalizePromptTemplateLineEndings(fileText).split(QLatin1Char('\n'));
	QString currentSection;
	QStringList sectionLines;

	for (const QString &line : lines) {
		const QString trimmed = line.trimmed();
		if (trimmed.startsWith(QLatin1Char('[')) && trimmed.endsWith(QLatin1Char(']'))) {
			const QString nextSection = trimmed.mid(1, trimmed.size() - 2).trimmed().toLower();
			if (currentSection == sectionName)
				break;
			currentSection = nextSection;
			continue;
		}

		if (currentSection == sectionName)
			sectionLines << line;
	}

	return sectionLines.join(QLatin1Char('\n')).trimmed();
}

static QString obsModuleFilePath(const QString &relativePath)
{
	const QByteArray relativePathBytes = relativePath.toUtf8();
	using ObsCharPtr = std::unique_ptr<char, decltype(&bfree)>;
	ObsCharPtr modulePath(obs_module_file(relativePathBytes.constData()), bfree);

	if (!modulePath || !modulePath.get() || modulePath.get()[0] == '\0')
		return {};

	return QDir::fromNativeSeparators(QString::fromUtf8(modulePath.get()));
}

static QStringList promptTemplateFileCandidates()
{
	QStringList candidates;

	const QString configuredPath =
		QString::fromLocal8Bit(qgetenv("CLIP_CROPPER_GPT_PROMPT_TEMPLATE_PATH")).trimmed();
	if (!configuredPath.isEmpty())
		candidates.append(QDir::fromNativeSeparators(configuredPath));

	candidates.append(obsModuleFilePath(QStringLiteral("gpt-prompts/opus-curation.ini")));
	candidates.append(QDir(QDir::currentPath()).filePath(QStringLiteral("data/gpt-prompts/opus-curation.ini")));

	candidates.removeAll(QString());
	candidates.removeDuplicates();
	return candidates;
}

static QString loadTemplateSectionFromRuntimeFile(const QString &sectionName, const QString &logLabel)
{
	const QString normalizedSectionName = sectionName.trimmed().toLower();
	if (normalizedSectionName.isEmpty())
		return {};

	for (const QString &candidate : promptTemplateFileCandidates()) {
		QFile file(candidate);
		if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text))
			continue;

		const QString fileText = QString::fromUtf8(file.readAll());
		const QString templateText = promptTemplateSection(fileText, normalizedSectionName);
		if (!templateText.trimmed().isEmpty()) {
			blog(LOG_INFO, "Loaded %s from runtime file. path=%s section=%s chars=%d",
			     logLabel.toUtf8().constData(), candidate.toUtf8().constData(),
			     normalizedSectionName.toUtf8().constData(), templateText.size());
			return templateText.trimmed();
		}
	}

	return {};
}

static QString loadTemplateFromRuntimeFile(const QString &sourceLanguage)
{
	Q_UNUSED(sourceLanguage);
	return loadTemplateSectionFromRuntimeFile(QStringLiteral("en"), QStringLiteral("GPT prompt template"));
}

static QString fallbackEnglishInputTextTemplate()
{
	return QStringLiteral(
		"You are a ClipAnything prompt planner for Opus Clip. Return valid JSON only. "
		"Do not write the final prompt. The plugin will render the final English prompt. "
		"Use these fields: no_strong_clip_found, reason, arc_type, main_target, context_phrase, opening_criteria, "
		"development_criteria, ending_criteria, boundary_criteria. "
		"Choose one dominant discourse arc, using genre and topics only as weak priors and never as a catalog. "
		"For chat, Q&A, or advice, choose one complete exchange instead of a montage of unrelated comments. "
		"All text fields must be concise and in English. Never copy Portuguese wording from the transcript into JSON fields.\n\n"
		"Scope: {{curation_scope}}\nGenre: {{genre}}\nPreset: {{curation_preset}}\nPreset context: {{curation_preset_context}}\nRanges: {{selected_ranges}}\nTopics: {{topic_keywords}}\n"
		"Named references: {{named_references}}\nSelected transcript:\n{{selected_range_transcript}}\n");
}

bool GptPromptClient::isSemanticGateFailurePrompt(const QString &prompt)
{
	return prompt.trimmed().startsWith(QString::fromLatin1(SEMANTIC_GATE_FAILURE_PREFIX), Qt::CaseInsensitive);
}

QString GptPromptClient::semanticGateFailureReason(const QString &prompt)
{
	QString trimmedPrompt = prompt.trimmed();
	if (!isSemanticGateFailurePrompt(trimmedPrompt))
		return {};

	trimmedPrompt = trimmedPrompt.mid(QString::fromLatin1(SEMANTIC_GATE_FAILURE_PREFIX).size()).trimmed();
	if (trimmedPrompt.isEmpty())
		return QStringLiteral(
			"No complete self-contained moment with a clear payoff was found in the selected range.");

	return trimmedPrompt;
}

QString GptPromptClient::promptGenerationBlockedPrompt(const QString &reason)
{
	const QString trimmedReason = reason.trimmed();
	return QString::fromLatin1(PROMPT_GENERATION_BLOCKED_PREFIX) + QLatin1Char(' ') +
	       (trimmedReason.isEmpty() ? QStringLiteral("The GPT prompt could not be generated.") : trimmedReason);
}

bool GptPromptClient::isPromptGenerationBlockedPrompt(const QString &prompt)
{
	return prompt.trimmed().startsWith(QString::fromLatin1(PROMPT_GENERATION_BLOCKED_PREFIX), Qt::CaseInsensitive);
}

QString GptPromptClient::promptGenerationBlockedReason(const QString &prompt)
{
	QString trimmedPrompt = prompt.trimmed();
	if (!isPromptGenerationBlockedPrompt(trimmedPrompt))
		return {};

	trimmedPrompt = trimmedPrompt.mid(QString::fromLatin1(PROMPT_GENERATION_BLOCKED_PREFIX).size()).trimmed();
	if (trimmedPrompt.isEmpty())
		return QStringLiteral("The GPT prompt could not be generated.");

	return trimmedPrompt;
}

QString GptPromptClient::inputTemplateConfigKey()
{
	return inputTemplateConfigKey(QStringLiteral("auto"));
}

QString GptPromptClient::inputTemplateConfigKey(const QString &sourceLanguage)
{
	return QString::fromLatin1(CONFIG_GPT_INPUT_TEMPLATE_PREFIX) + normalizePromptLanguage(sourceLanguage);
}

QString GptPromptClient::defaultInputTextTemplate()
{
	return defaultInputTextTemplate(QStringLiteral("auto"));
}

QString GptPromptClient::defaultInputTextTemplate(const QString &sourceLanguage)
{
	const QString normalizedLanguage = normalizePromptLanguage(sourceLanguage);
	const QString runtimeTemplate = loadTemplateFromRuntimeFile(sourceLanguage);
	if (!runtimeTemplate.trimmed().isEmpty())
		return runtimeTemplate.trimmed();

	Q_UNUSED(normalizedLanguage);
	return fallbackEnglishInputTextTemplate();
}

QString GptPromptClient::configuredInputTextTemplate()
{
	return configuredInputTextTemplate(QStringLiteral("auto"));
}

QString GptPromptClient::configuredInputTextTemplate(const QString &sourceLanguage)
{
	const QString configuredTemplate = PluginConfig::getValue(inputTemplateConfigKey(sourceLanguage)).trimmed();
	if (!configuredTemplate.isEmpty())
		return configuredTemplate;

	if (normalizePromptLanguage(sourceLanguage) != QStringLiteral("auto")) {
		const QString autoTemplate =
			PluginConfig::getValue(inputTemplateConfigKey(QStringLiteral("auto"))).trimmed();
		if (!autoTemplate.isEmpty())
			return autoTemplate;
	}

	return defaultInputTextTemplate(sourceLanguage).trimmed();
}

GptPromptClient::GptPromptClient(QString apiKey, QString model, QObject *parent)
	: QObject(parent),
	  apiKey(std::move(apiKey)),
	  model(std::move(model))
{
	if (this->model.trimmed().isEmpty())
		this->model = QStringLiteral("gpt-5.4-mini");
}

void GptPromptClient::cancel()
{
	cancelRequested = true;
	if (currentReply)
		currentReply->abort();
}

QString GptPromptClient::buildInputText(const QString &videoPath, const RecordingTranscript &fullTranscript,
					const RecordingTranscript &selectedRangeTranscript,
					const CurationSettings &curationSettings) const
{
	const QString sourceLanguage = promptSourceLanguageForCuration(curationSettings);
	return renderInputTemplate(configuredInputTextTemplate(QStringLiteral("en")), videoPath, fullTranscript,
				   selectedRangeTranscript, curationSettings, sourceLanguage);
}

void GptPromptClient::createOpusPromptAsync(const QString &videoPath, const RecordingTranscript &transcript,
					    const CurationSettings &curationSettings)
{
	createOpusPromptAsync(videoPath, transcript, transcript, curationSettings);
}

void GptPromptClient::createOpusPromptAsync(const QString &videoPath, const RecordingTranscript &fullTranscript,
					    const RecordingTranscript &selectedRangeTranscript,
					    const CurationSettings &curationSettings)
{
	if (apiKey.trimmed().isEmpty()) {
		emit promptFailed(QStringLiteral("OpenAI API key is empty"));
		return;
	}

	if (selectedRangeTranscript.segments.isEmpty()) {
		emit promptFailed(QStringLiteral("No transcript available for this video"));
		return;
	}

	QNetworkRequest request{QUrl(QString::fromLatin1(OPENAI_CHAT_COMPLETIONS_URL))};
	request.setRawHeader("Accept", "application/json");
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	request.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(apiKey.trimmed()).toUtf8());

	const QString inputText = buildInputText(videoPath, fullTranscript, selectedRangeTranscript, curationSettings);
	const QString inputCacheKey = GptPromptStore::keyForInput(model, inputText);
	const QString cachedPrompt = GptPromptStore::loadForInput(model, inputText);
	cancelRequested = false;

	const double selectedSec = Curation::selectedDurationSeconds(selectedRangeTranscript, curationSettings);
	const QString curationScope = Curation::scopeForDuration(selectedSec);
	blog(LOG_INFO,
	     "GPT curation input prepared. video=%s fullSegments=%d selectedSegments=%d selectedDurationSec=%.3f scope=%s inputChars=%d cacheKey=%s",
	     videoPath.toUtf8().constData(), static_cast<int>(fullTranscript.segments.size()),
	     static_cast<int>(selectedRangeTranscript.segments.size()), selectedSec, curationScope.toUtf8().constData(),
	     inputText.size(), inputCacheKey.toUtf8().constData());

	if (!cachedPrompt.trimmed().isEmpty()) {
		QStringList cachedIssues;
		if (!Curation::shouldRepairPrompt(cachedPrompt, selectedRangeTranscript, curationSettings,
						  &cachedIssues)) {
			blog(LOG_INFO,
			     "GPT prompt cache hit. Skipping OpenAI request. video=%s cacheKey=%s promptChars=%d",
			     videoPath.toUtf8().constData(), inputCacheKey.toUtf8().constData(),
			     cachedPrompt.trimmed().size());
			QTimer::singleShot(0, this, [this, cachedPrompt]() {
				if (!cancelRequested)
					emit promptReady(cachedPrompt.trimmed());
			});
			return;
		}

		blog(LOG_WARNING,
		     "GPT prompt cache hit failed predictability checks. Ignoring cached prompt. video=%s cacheKey=%s issues=%s",
		     videoPath.toUtf8().constData(), inputCacheKey.toUtf8().constData(),
		     cachedIssues.join(QStringLiteral("; ")).toUtf8().constData());
	}

	blog(LOG_INFO, "GPT prompt cache miss. Sending OpenAI request. video=%s cacheKey=%s model=%s",
	     videoPath.toUtf8().constData(), inputCacheKey.toUtf8().constData(), model.trimmed().toUtf8().constData());

	const QJsonObject payload = OpenAiChatPayload::build(model, inputText, 500);

	QNetworkReply *reply = network.post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
	reply->setProperty("clipCropperGptInputText", inputText);
	currentReply = reply;

	connect(reply, &QNetworkReply::finished, this, [this, reply, selectedRangeTranscript, curationSettings]() {
		const QByteArray response = reply->readAll();
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const QNetworkReply::NetworkError error = reply->error();
		const QString errorString = reply->errorString();
		if (currentReply == reply)
			currentReply = nullptr;
		reply->deleteLater();

		if (cancelRequested || error == QNetworkReply::OperationCanceledError) {
			cancelRequested = false;
			emit promptFailed(QStringLiteral("Canceled"));
			return;
		}

		if (error != QNetworkReply::NoError || status < 200 || status >= 300) {
			emit promptFailed(QStringLiteral("GPT prompt generation failed: %1 - %2")
						  .arg(errorString, QString::fromUtf8(response)));
			return;
		}

		const QString rawOutputText = extractOutputText(response).trimmed();
		if (rawOutputText.isEmpty()) {
			emit promptFailed(QStringLiteral("GPT response did not contain output text"));
			return;
		}

		blog(LOG_INFO, "GPT raw curation response received. status=%d rawChars=%d", status,
		     rawOutputText.size());

		const QString normalizedOutputText =
			Curation::normalizePlanOutput(rawOutputText, selectedRangeTranscript, curationSettings,
						      loadTemplateSectionFromRuntimeFile)
				.trimmed();
		const QString outputText = Curation::applyResolvedPresetPromptGuard(
						   normalizedOutputText, selectedRangeTranscript, curationSettings)
						   .trimmed();
		const Curation::Intent normalizedIntent =
			Curation::resolveIntent(curationSettings, selectedRangeTranscript, outputText);
		const bool structuredPlanOutput = rawOutputText.trimmed().startsWith(QLatin1Char('{')) ||
						  rawOutputText.contains(QStringLiteral("\"main_target\"")) ||
						  rawOutputText.contains(QStringLiteral("\"plan\""));
		const bool guardApplied = normalizedOutputText != outputText;
		blog(LOG_INFO,
		     "GPT curation prompt normalized. preset=%s scope=%s strategy=%s structuredPlan=%s guardApplied=%s promptChars=%d",
		     normalizedIntent.resolvedPresetId.toUtf8().constData(),
		     normalizedIntent.scope.toUtf8().constData(),
		     Curation::shouldUseMultipleClips(normalizedIntent) ? "multiple" : "single",
		     structuredPlanOutput ? "true" : "false", guardApplied ? "true" : "false", outputText.size());
		if (outputText.isEmpty()) {
			emit promptFailed(QStringLiteral("GPT response did not contain a valid Opus prompt"));
			return;
		}

		const QString inputText = reply->property("clipCropperGptInputText").toString();
		const bool semanticGateFailure =
			outputText.startsWith(QString::fromLatin1(SEMANTIC_GATE_FAILURE_PREFIX), Qt::CaseInsensitive);
		if (semanticGateFailure) {
			blog(LOG_INFO, "GPT semantic gate result was not cached. reason=%s",
			     outputText.toUtf8().constData());
			emit promptReady(outputText);
			return;
		}

		QStringList qualityIssues;
		if (Curation::shouldRepairPrompt(outputText, selectedRangeTranscript, curationSettings,
						 &qualityIssues)) {
			blog(LOG_WARNING, "GPT Opus prompt failed predictability checks. issues=%s prompt=%s",
			     qualityIssues.join(QStringLiteral("; ")).toUtf8().constData(),
			     outputText.toUtf8().constData());

			if (!gptPromptRepairEnabled()) {
				const QString fallbackPrompt = Curation::deterministicOpusPromptFallback(
					outputText, selectedRangeTranscript, curationSettings);
				QStringList fallbackIssues;
				const bool fallbackStillInvalid = Curation::shouldRepairPrompt(
					fallbackPrompt, selectedRangeTranscript, curationSettings, &fallbackIssues);
				blog(LOG_WARNING,
				     "GPT prompt repair is disabled. Using deterministic local fallback after predictability failure. issues=%s fallbackIssues=%s fallback=%s",
				     qualityIssues.join(QStringLiteral("; ")).toUtf8().constData(),
				     fallbackIssues.join(QStringLiteral("; ")).toUtf8().constData(),
				     fallbackPrompt.toUtf8().constData());
				if (!fallbackStillInvalid && !inputText.trimmed().isEmpty())
					GptPromptStore::saveForInput(model, inputText, fallbackPrompt);
				emit promptReady(fallbackPrompt);
				return;
			}

			const QString sourceLanguage = promptSourceLanguageForCuration(curationSettings);
			const QString repairInputText = buildRepairInputText(
				outputText, qualityIssues, selectedRangeTranscript, curationSettings, sourceLanguage);

			QNetworkRequest repairRequest{QUrl(QString::fromLatin1(OPENAI_CHAT_COMPLETIONS_URL))};
			repairRequest.setRawHeader("Accept", "application/json");
			repairRequest.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
			repairRequest.setRawHeader("Authorization",
						   QStringLiteral("Bearer %1").arg(apiKey.trimmed()).toUtf8());

			const QJsonObject repairPayload = OpenAiChatPayload::build(model, repairInputText, 350, false);

			QNetworkReply *repairReply = network.post(
				repairRequest, QJsonDocument(repairPayload).toJson(QJsonDocument::Compact));
			repairReply->setProperty("clipCropperGptInputText", inputText);
			currentReply = repairReply;

			connect(repairReply, &QNetworkReply::finished, this,
				[this, repairReply, outputText, inputText, selectedRangeTranscript,
				 curationSettings]() {
					const QByteArray repairResponse = repairReply->readAll();
					const int repairStatus =
						repairReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
					const QNetworkReply::NetworkError repairError = repairReply->error();
					const QString repairErrorString = repairReply->errorString();
					if (currentReply == repairReply)
						currentReply = nullptr;
					repairReply->deleteLater();

					if (cancelRequested || repairError == QNetworkReply::OperationCanceledError) {
						cancelRequested = false;
						emit promptFailed(QStringLiteral("Canceled"));
						return;
					}

					if (repairError != QNetworkReply::NoError || repairStatus < 200 ||
					    repairStatus >= 300) {
						const QString fallbackPrompt =
							Curation::deterministicOpusPromptFallback(
								outputText, selectedRangeTranscript, curationSettings);
						QStringList fallbackIssues;
						const bool fallbackStillInvalid = Curation::shouldRepairPrompt(
							fallbackPrompt, selectedRangeTranscript, curationSettings,
							&fallbackIssues);
						blog(LOG_WARNING,
						     "GPT prompt repair failed. Using deterministic local fallback. status=%d error=%s response=%s fallbackIssues=%s fallback=%s",
						     repairStatus, repairErrorString.toUtf8().constData(),
						     repairResponse.constData(),
						     fallbackIssues.join(QStringLiteral("; ")).toUtf8().constData(),
						     fallbackPrompt.toUtf8().constData());
						if (!fallbackStillInvalid && !inputText.trimmed().isEmpty())
							GptPromptStore::saveForInput(model, inputText, fallbackPrompt);
						emit promptReady(fallbackPrompt);
						return;
					}

					const QString rawRepairOutputText = extractOutputText(repairResponse).trimmed();
					const QString repairedOutputText =
						Curation::normalizePlanOutput(rawRepairOutputText,
									      selectedRangeTranscript, curationSettings,
									      loadTemplateSectionFromRuntimeFile)
							.trimmed();
					if (repairedOutputText.isEmpty()) {
						const QString fallbackPrompt =
							Curation::deterministicOpusPromptFallback(
								outputText, selectedRangeTranscript, curationSettings);
						QStringList fallbackIssues;
						const bool fallbackStillInvalid = Curation::shouldRepairPrompt(
							fallbackPrompt, selectedRangeTranscript, curationSettings,
							&fallbackIssues);
						blog(LOG_WARNING,
						     "GPT prompt repair returned empty output. Using deterministic local fallback. fallbackIssues=%s fallback=%s",
						     fallbackIssues.join(QStringLiteral("; ")).toUtf8().constData(),
						     fallbackPrompt.toUtf8().constData());
						if (!fallbackStillInvalid && !inputText.trimmed().isEmpty())
							GptPromptStore::saveForInput(model, inputText, fallbackPrompt);
						emit promptReady(fallbackPrompt);
						return;
					}

					const Curation::Intent repairedIntent = Curation::resolveIntent(
						curationSettings, selectedRangeTranscript, repairedOutputText);
					blog(LOG_INFO,
					     "GPT repaired curation prompt normalized. preset=%s scope=%s strategy=%s promptChars=%d",
					     repairedIntent.resolvedPresetId.toUtf8().constData(),
					     repairedIntent.scope.toUtf8().constData(),
					     Curation::shouldUseMultipleClips(repairedIntent) ? "multiple" : "single",
					     repairedOutputText.size());

					QStringList repairedIssues;
					const bool repairedStillNeedsRepair = Curation::shouldRepairPrompt(
						repairedOutputText, selectedRangeTranscript, curationSettings,
						&repairedIssues);
					if (repairedStillNeedsRepair) {
						const QString fallbackPrompt = Curation::deterministicOpusPromptFallback(
							repairedOutputText, selectedRangeTranscript, curationSettings);
						QStringList fallbackIssues;
						const bool fallbackStillInvalid = Curation::shouldRepairPrompt(
							fallbackPrompt, selectedRangeTranscript, curationSettings,
							&fallbackIssues);
						blog(LOG_WARNING,
						     "GPT prompt repair still has predictability issues. Using deterministic local fallback. repairedIssues=%s fallbackIssues=%s fallback=%s",
						     repairedIssues.join(QStringLiteral("; ")).toUtf8().constData(),
						     fallbackIssues.join(QStringLiteral("; ")).toUtf8().constData(),
						     fallbackPrompt.toUtf8().constData());
						if (!fallbackStillInvalid && !inputText.trimmed().isEmpty())
							GptPromptStore::saveForInput(model, inputText, fallbackPrompt);
						emit promptReady(fallbackPrompt);
						return;
					}

					blog(LOG_INFO, "GPT Opus prompt repaired successfully. promptChars=%d",
					     repairedOutputText.size());
					if (!inputText.trimmed().isEmpty())
						GptPromptStore::saveForInput(model, inputText, repairedOutputText);

					emit promptReady(repairedOutputText);
				});
			return;
		}

		blog(LOG_INFO, "GPT Opus prompt generated successfully. promptChars=%d", outputText.size());
		if (!inputText.trimmed().isEmpty())
			GptPromptStore::saveForInput(model, inputText, outputText);

		emit promptReady(outputText);
	});
}

QString GptPromptClient::extractOutputText(const QByteArray &response) const
{
	const QJsonDocument doc = QJsonDocument::fromJson(response);
	if (!doc.isObject())
		return {};

	const QJsonObject root = doc.object();
	const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
	for (const QJsonValue &choiceValue : choices) {
		const QJsonObject choice = choiceValue.toObject();
		const QJsonObject message = choice.value(QStringLiteral("message")).toObject();
		const QString content = message.value(QStringLiteral("content")).toString();
		if (!content.trimmed().isEmpty())
			return content;
	}

	const QString direct = root.value(QStringLiteral("output_text")).toString();
	if (!direct.trimmed().isEmpty())
		return direct;

	QString combined;
	const QJsonArray output = root.value(QStringLiteral("output")).toArray();
	for (const QJsonValue &itemValue : output) {
		const QJsonObject item = itemValue.toObject();
		const QJsonArray content = item.value(QStringLiteral("content")).toArray();
		for (const QJsonValue &contentValue : content) {
			const QJsonObject contentItem = contentValue.toObject();
			const QString text = contentItem.value(QStringLiteral("text")).toString();
			if (!text.trimmed().isEmpty()) {
				if (!combined.isEmpty())
					combined += QLatin1Char('\n');
				combined += text;
			}
		}
	}

	return combined;
}
