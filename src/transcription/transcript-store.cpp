#include "transcription/transcript-store.hpp"

#include "utils/config.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QDir>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextStream>
#include <QStringConverter>

#include <algorithm>

namespace {
constexpr const char *AutoLanguage = "auto";

QString normalizeTranscriptionLanguage(QString language)
{
	language = language.trimmed().toLower();
	if (language.isEmpty() || language == QStringLiteral("auto"))
		return QString::fromLatin1(AutoLanguage);

	if (language == QStringLiteral("pt-br") || language == QStringLiteral("portuguese"))
		return QStringLiteral("pt");

	if (language == QStringLiteral("en-us") || language == QStringLiteral("english"))
		return QStringLiteral("en");

	return language;
}

QString safeLanguageKey(const QString &language)
{
	QString normalized = normalizeTranscriptionLanguage(language);
	normalized.replace(QRegularExpression(QStringLiteral("[^a-z0-9_-]")), QStringLiteral("_"));
	return normalized.isEmpty() ? QString::fromLatin1(AutoLanguage) : normalized;
}

QString transcriptFileCacheDirectory()
{
	QString baseDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	if (baseDir.trimmed().isEmpty())
		baseDir = QDir::tempPath() + QStringLiteral("/clip-cropper");
	const QString dir = QDir(baseDir).filePath(QStringLiteral("transcription-cache"));
	QDir().mkpath(dir);
	return dir;
}

QJsonObject wordToJson(const WordTiming &word)
{
	QJsonObject item;
	item.insert(QStringLiteral("word"), word.word);
	item.insert(QStringLiteral("startSec"), word.startSec);
	item.insert(QStringLiteral("endSec"), word.endSec);
	item.insert(QStringLiteral("score"), word.score);
	return item;
}

WordTiming wordFromJson(const QJsonObject &item)
{
	WordTiming word;
	word.word = item.value(QStringLiteral("word")).toString().trimmed();
	word.startSec = item.value(QStringLiteral("startSec")).toDouble();
	word.endSec = item.value(QStringLiteral("endSec")).toDouble();
	word.score = item.value(QStringLiteral("score")).toDouble(0.0);
	return word;
}

QString normalizedRangeIdentity(const QVector<ClipDuration> &ranges)
{
	QStringList parts;
	for (const ClipDuration &range : ranges) {
		if (range.endSec <= range.startSec)
			continue;

		parts << QStringLiteral("%1_%2")
				 .arg(std::max(0.0, range.startSec), 0, 'f', 3)
				 .arg(std::max(range.startSec, range.endSec), 0, 'f', 3);
	}

	return parts.join(QStringLiteral("|"));
}
} // namespace

QString TranscriptStore::safeFileKey(const QString &videoPath)
{
	const QFileInfo info(videoPath);
	const QString resolvedPath = info.exists() ? info.absoluteFilePath() : videoPath;
	const QString lastModified = info.exists() ? info.lastModified().toUTC().toString(Qt::ISODateWithMs)
						   : QString();
	const QString identity =
		QStringLiteral("%1\n%2\n%3")
			.arg(resolvedPath, QString::number(info.exists() ? info.size() : 0), lastModified);
	const QByteArray hash = QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex();
	return QString::fromLatin1(hash.left(24));
}

QString TranscriptStore::keyForVideoPath(const QString &videoPath)
{
	return keyForVideoPath(videoPath, QString::fromLatin1(AutoLanguage));
}

QString TranscriptStore::keyForVideoPath(const QString &videoPath, const QString &transcriptionLanguage)
{
	return QStringLiteral("video_transcript_v3.%1.%2")
		.arg(safeFileKey(videoPath), safeLanguageKey(transcriptionLanguage));
}

QString TranscriptStore::keyForAlignedVideoPath(const QString &videoPath, const QString &transcriptionLanguage)
{
	return QStringLiteral("video_transcript_v4.%1.%2")
		.arg(safeFileKey(videoPath), safeLanguageKey(transcriptionLanguage));
}

QString TranscriptStore::alignedTranscriptFilePath(const QString &videoPath, const QString &transcriptionLanguage)
{
	return QDir(transcriptFileCacheDirectory())
		.filePath(keyForAlignedVideoPath(videoPath, transcriptionLanguage) + QStringLiteral(".jsonl"));
}

QString TranscriptStore::keyForVideoRanges(const QString &videoPath, const QString &transcriptionLanguage,
					   const QVector<ClipDuration> &ranges)
{
	const QString rangeIdentity = normalizedRangeIdentity(ranges);
	const QByteArray rangeHash =
		QCryptographicHash::hash(rangeIdentity.toUtf8(), QCryptographicHash::Sha256).toHex();
	return QStringLiteral("video_transcript_range_v3.%1.%2.%3")
		.arg(safeFileKey(videoPath), safeLanguageKey(transcriptionLanguage),
		     QString::fromLatin1(rangeHash.left(24)));
}

void TranscriptStore::saveForVideoPath(const QString &videoPath, const RecordingTranscript &transcript)
{
	saveForVideoPath(videoPath, QString::fromLatin1(AutoLanguage), transcript);
}

void TranscriptStore::saveForVideoPath(const QString &videoPath, const QString &transcriptionLanguage,
				       const RecordingTranscript &transcript)
{
	const QString normalizedLanguage = normalizeTranscriptionLanguage(transcriptionLanguage);

	QJsonObject root;
	root.insert(QStringLiteral("videoFileName"), QFileInfo(videoPath).fileName());
	root.insert(QStringLiteral("videoPath"), videoPath);
	root.insert(QStringLiteral("transcriptionLanguage"), normalizedLanguage);

	QJsonArray segments;
	for (const TranscriptSegment &segment : transcript.segments) {
		if (segment.text.trimmed().isEmpty())
			continue;

		QJsonObject item;
		item.insert(QStringLiteral("startSec"), segment.startSec);
		item.insert(QStringLiteral("endSec"), segment.endSec);
		item.insert(QStringLiteral("text"), segment.text.trimmed());
		segments.append(item);
	}

	root.insert(QStringLiteral("segments"), segments);
	PluginConfig::setValue(keyForVideoPath(videoPath, normalizedLanguage),
			       QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
}

RecordingTranscript TranscriptStore::loadForVideoPath(const QString &videoPath)
{
	return loadForVideoPath(videoPath, QString::fromLatin1(AutoLanguage));
}

RecordingTranscript TranscriptStore::loadForVideoPath(const QString &videoPath, const QString &transcriptionLanguage)
{
	RecordingTranscript aligned = loadAlignedForVideoPath(videoPath, transcriptionLanguage);
	if (!aligned.segments.isEmpty())
		return aligned;

	RecordingTranscript transcript;
	transcript.videoFileName = QFileInfo(videoPath).fileName();
	transcript.videoPath = videoPath;

	const QString raw = PluginConfig::getValue(keyForVideoPath(videoPath, transcriptionLanguage));
	if (raw.trimmed().isEmpty())
		return transcript;

	const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
	if (!doc.isObject())
		return transcript;

	const QJsonObject root = doc.object();
	transcript.videoFileName = root.value(QStringLiteral("videoFileName")).toString(transcript.videoFileName);
	transcript.videoPath = root.value(QStringLiteral("videoPath")).toString(videoPath);

	const QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
	for (const QJsonValue &value : segments) {
		const QJsonObject item = value.toObject();
		TranscriptSegment segment;
		segment.startSec = item.value(QStringLiteral("startSec")).toDouble();
		segment.endSec = item.value(QStringLiteral("endSec")).toDouble();
		segment.text = item.value(QStringLiteral("text")).toString().trimmed();

		if (!segment.text.isEmpty())
			transcript.segments.append(segment);
	}

	return transcript;
}

void TranscriptStore::saveAlignedForVideoPath(const QString &videoPath, const QString &transcriptionLanguage,
					      const RecordingTranscript &transcript)
{
	const QString normalizedLanguage = normalizeTranscriptionLanguage(transcriptionLanguage);
	const QString path = alignedTranscriptFilePath(videoPath, normalizedLanguage);
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
		return;

	QTextStream stream(&file);
	stream.setEncoding(QStringConverter::Utf8);
	QJsonObject meta;
	meta.insert(QStringLiteral("type"), QStringLiteral("meta"));
	meta.insert(QStringLiteral("schema"), QStringLiteral("clip-cropper-transcript-v4-jsonl"));
	meta.insert(QStringLiteral("videoFileName"), QFileInfo(videoPath).fileName());
	meta.insert(QStringLiteral("videoPath"), videoPath);
	meta.insert(QStringLiteral("transcriptionLanguage"), normalizedLanguage);
	meta.insert(QStringLiteral("wordAligned"), transcript.hasWordTimings());
	meta.insert(QStringLiteral("alignmentBackend"), transcript.alignmentBackend);
	stream << QString::fromUtf8(QJsonDocument(meta).toJson(QJsonDocument::Compact)) << '\n';

	for (int i = 0; i < static_cast<int>(transcript.segments.size()); ++i) {
		const TranscriptSegment &segment = transcript.segments.at(i);
		const QString text = segment.text.trimmed();
		if (text.isEmpty())
			continue;

		QJsonObject item;
		item.insert(QStringLiteral("type"), QStringLiteral("segment"));
		item.insert(QStringLiteral("index"), i);
		item.insert(QStringLiteral("startSec"), segment.startSec);
		item.insert(QStringLiteral("endSec"), segment.endSec);
		item.insert(QStringLiteral("text"), text);
		QJsonArray words;
		for (const WordTiming &word : segment.words)
			words.append(wordToJson(word));
		item.insert(QStringLiteral("words"), words);
		stream << QString::fromUtf8(QJsonDocument(item).toJson(QJsonDocument::Compact)) << '\n';
	}

	file.commit();
}

RecordingTranscript TranscriptStore::loadAlignedForVideoPath(const QString &videoPath,
							     const QString &transcriptionLanguage)
{
	RecordingTranscript transcript;
	transcript.videoFileName = QFileInfo(videoPath).fileName();
	transcript.videoPath = videoPath;

	QFile file(alignedTranscriptFilePath(videoPath, transcriptionLanguage));
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
		return transcript;

	QTextStream stream(&file);
	stream.setEncoding(QStringConverter::Utf8);
	while (!stream.atEnd()) {
		const QString line = stream.readLine().trimmed();
		if (line.isEmpty())
			continue;
		QJsonParseError parseError;
		const QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8(), &parseError);
		if (parseError.error != QJsonParseError::NoError || !doc.isObject())
			continue;
		const QJsonObject item = doc.object();
		const QString type = item.value(QStringLiteral("type")).toString();
		if (type == QStringLiteral("meta")) {
			transcript.videoFileName =
				item.value(QStringLiteral("videoFileName")).toString(transcript.videoFileName);
			transcript.videoPath = item.value(QStringLiteral("videoPath")).toString(videoPath);
			transcript.wordAligned = item.value(QStringLiteral("wordAligned")).toBool(false);
			transcript.alignmentBackend = item.value(QStringLiteral("alignmentBackend")).toString();
			continue;
		}
		if (type != QStringLiteral("segment"))
			continue;

		TranscriptSegment segment;
		segment.startSec = item.value(QStringLiteral("startSec")).toDouble();
		segment.endSec = item.value(QStringLiteral("endSec")).toDouble();
		segment.text = item.value(QStringLiteral("text")).toString().trimmed();
		const QJsonArray words = item.value(QStringLiteral("words")).toArray();
		segment.words.reserve(words.size());
		for (const QJsonValue &wordValue : words) {
			const WordTiming word = wordFromJson(wordValue.toObject());
			if (!word.word.isEmpty() && word.endSec >= word.startSec)
				segment.words.append(word);
		}
		if (!segment.text.isEmpty())
			transcript.segments.append(segment);
	}
	transcript.wordAligned = transcript.hasWordTimings();
	if (transcript.wordAligned && transcript.alignmentBackend.trimmed().isEmpty())
		transcript.alignmentBackend = QStringLiteral("whisperx");
	return transcript;
}

void TranscriptStore::saveForVideoRanges(const QString &videoPath, const QString &transcriptionLanguage,
					 const QVector<ClipDuration> &ranges, const RecordingTranscript &transcript)
{
	const QString normalizedLanguage = normalizeTranscriptionLanguage(transcriptionLanguage);

	QJsonObject root;
	root.insert(QStringLiteral("videoFileName"), QFileInfo(videoPath).fileName());
	root.insert(QStringLiteral("videoPath"), videoPath);
	root.insert(QStringLiteral("transcriptionLanguage"), normalizedLanguage);
	root.insert(QStringLiteral("rangeTranscript"), true);
	root.insert(QStringLiteral("rangeIdentity"), normalizedRangeIdentity(ranges));

	QJsonArray segments;
	for (const TranscriptSegment &segment : transcript.segments) {
		if (segment.text.trimmed().isEmpty())
			continue;

		QJsonObject item;
		item.insert(QStringLiteral("startSec"), segment.startSec);
		item.insert(QStringLiteral("endSec"), segment.endSec);
		item.insert(QStringLiteral("text"), segment.text.trimmed());
		segments.append(item);
	}

	root.insert(QStringLiteral("segments"), segments);
	PluginConfig::setValue(keyForVideoRanges(videoPath, normalizedLanguage, ranges),
			       QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
}

RecordingTranscript TranscriptStore::loadForVideoRanges(const QString &videoPath, const QString &transcriptionLanguage,
							const QVector<ClipDuration> &ranges)
{
	RecordingTranscript transcript;
	transcript.videoFileName = QFileInfo(videoPath).fileName();
	transcript.videoPath = videoPath;

	const QString raw = PluginConfig::getValue(keyForVideoRanges(videoPath, transcriptionLanguage, ranges));
	if (raw.trimmed().isEmpty())
		return transcript;

	const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
	if (!doc.isObject())
		return transcript;

	const QJsonObject root = doc.object();
	transcript.videoFileName = root.value(QStringLiteral("videoFileName")).toString(transcript.videoFileName);
	transcript.videoPath = root.value(QStringLiteral("videoPath")).toString(videoPath);

	const QJsonArray segments = root.value(QStringLiteral("segments")).toArray();
	for (const QJsonValue &value : segments) {
		const QJsonObject item = value.toObject();
		TranscriptSegment segment;
		segment.startSec = item.value(QStringLiteral("startSec")).toDouble();
		segment.endSec = item.value(QStringLiteral("endSec")).toDouble();
		segment.text = item.value(QStringLiteral("text")).toString().trimmed();

		if (!segment.text.isEmpty())
			transcript.segments.append(segment);
	}

	return transcript;
}

void TranscriptStore::removeForVideoPath(const QString &videoPath)
{
	removeForVideoPath(videoPath, QString::fromLatin1(AutoLanguage));
}

void TranscriptStore::removeForVideoPath(const QString &videoPath, const QString &transcriptionLanguage)
{
	PluginConfig::removeValue(keyForVideoPath(videoPath, transcriptionLanguage));
	QFile::remove(alignedTranscriptFilePath(videoPath, transcriptionLanguage));
}
