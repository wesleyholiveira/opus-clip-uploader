#include "transcription/transcript-store.hpp"

#include "utils/config.hpp"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

QString TranscriptStore::safeFileKey(const QString &videoPath)
{
	const QFileInfo info(videoPath);
	const QString stableName = info.fileName().trimmed().isEmpty() ? videoPath : info.fileName();
	const QByteArray hash = QCryptographicHash::hash(stableName.toUtf8(), QCryptographicHash::Sha256).toHex();
	return QString::fromLatin1(hash.left(24));
}

QString TranscriptStore::keyForVideoPath(const QString &videoPath)
{
	return QStringLiteral("video_transcript.%1").arg(safeFileKey(videoPath));
}

void TranscriptStore::saveForVideoPath(const QString &videoPath, const RecordingTranscript &transcript)
{
	QJsonObject root;
	root.insert(QStringLiteral("videoFileName"), QFileInfo(videoPath).fileName());
	root.insert(QStringLiteral("videoPath"), videoPath);

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
	PluginConfig::setValue(keyForVideoPath(videoPath), QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
}

RecordingTranscript TranscriptStore::loadForVideoPath(const QString &videoPath)
{
	RecordingTranscript transcript;
	transcript.videoFileName = QFileInfo(videoPath).fileName();
	transcript.videoPath = videoPath;

	const QString raw = PluginConfig::getValue(keyForVideoPath(videoPath));
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
	PluginConfig::removeValue(keyForVideoPath(videoPath));
}
