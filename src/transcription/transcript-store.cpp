#include "transcription/transcript-store.hpp"

#include "utils/config.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

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
	return QStringLiteral("video_transcript.%1.%2")
		.arg(safeFileKey(videoPath), safeLanguageKey(transcriptionLanguage));
}

QString TranscriptStore::keyForVideoRanges(const QString &videoPath, const QString &transcriptionLanguage,
					   const QVector<ClipDuration> &ranges)
{
	const QString rangeIdentity = normalizedRangeIdentity(ranges);
	const QByteArray rangeHash =
		QCryptographicHash::hash(rangeIdentity.toUtf8(), QCryptographicHash::Sha256).toHex();
	return QStringLiteral("video_transcript_range.%1.%2.%3")
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
}
