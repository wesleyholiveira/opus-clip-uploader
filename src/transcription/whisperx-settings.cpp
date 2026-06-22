#include "transcription/whisperx-settings.hpp"

#include "utils/config.hpp"

#ifdef __cplusplus
extern "C" {
#endif
#include <obs-module.h>
#ifdef __cplusplus
}
#endif

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <algorithm>

namespace Transcription {
namespace {

QString obsModuleFilePath(const QString &relativePath)
{
	const QByteArray relativePathBytes = relativePath.toUtf8();
	char *modulePath = obs_module_file(relativePathBytes.constData());
	if (!modulePath)
		return {};
	const QString result = QString::fromUtf8(modulePath);
	bfree(modulePath);
	return result;
}

QString configuredOrEnv(const char *configKey, const char *envKey)
{
	const QString configured = PluginConfig::getValue(QString::fromLatin1(configKey)).trimmed();
	if (!configured.isEmpty())
		return configured;
	return QString::fromLocal8Bit(qgetenv(envKey)).trimmed();
}

} // namespace

bool WhisperXSettings::enabled() const
{
	return (alignmentOnly() || primaryTranscription()) && !pythonPath.trimmed().isEmpty();
}

bool WhisperXSettings::alignmentOnly() const
{
	return backend == QString::fromLatin1(WHISPERX_BACKEND_ALIGNMENT);
}

bool WhisperXSettings::primaryTranscription() const
{
	return backend == QString::fromLatin1(WHISPERX_BACKEND_PRIMARY);
}

QString normalizeWhisperXDevice(QString value)
{
	value = value.trimmed().toLower();
	if (value == QStringLiteral("cuda") || value == QStringLiteral("cpu"))
		return value;
	return QStringLiteral("cuda");
}

QString normalizeWhisperXComputeType(QString value)
{
	value = value.trimmed().toLower();
	if (value == QStringLiteral("float16") || value == QStringLiteral("float32") || value == QStringLiteral("int8"))
		return value;
	return QStringLiteral("float16");
}

QString defaultWhisperXWorkerPath()
{
	const QString bundled = obsModuleFilePath(QStringLiteral("tools/whisperx_align_worker.py"));
	if (QFileInfo::exists(bundled))
		return bundled;

	const QString appDirCandidate = QDir(QCoreApplication::applicationDirPath())
		.filePath(QStringLiteral("tools/whisperx_align_worker.py"));
	if (QFileInfo::exists(appDirCandidate))
		return appDirCandidate;

	return QStringLiteral("tools/whisperx_align_worker.py");
}

WhisperXSettings whisperXSettingsFromConfig()
{
	WhisperXSettings settings;
	settings.backend = PluginConfig::getValue(QString::fromLatin1(CONFIG_WHISPERX_BACKEND),
		QString::fromLatin1(WHISPERX_BACKEND_DISABLED)).trimmed().toLower();
	if (settings.backend == QString::fromLatin1(WHISPERX_BACKEND_PYTHON))
		settings.backend = QString::fromLatin1(WHISPERX_BACKEND_ALIGNMENT);
	if (settings.backend != QString::fromLatin1(WHISPERX_BACKEND_ALIGNMENT) &&
	    settings.backend != QString::fromLatin1(WHISPERX_BACKEND_PRIMARY))
		settings.backend = QString::fromLatin1(WHISPERX_BACKEND_DISABLED);

	settings.pythonPath = configuredOrEnv(CONFIG_WHISPERX_PYTHON_PATH, "CLIP_CROPPER_WHISPERX_PYTHON");
	settings.workerPath = configuredOrEnv(CONFIG_WHISPERX_WORKER_PATH, "CLIP_CROPPER_WHISPERX_WORKER");
	if (settings.workerPath.trimmed().isEmpty())
		settings.workerPath = defaultWhisperXWorkerPath();
	settings.device = normalizeWhisperXDevice(PluginConfig::getValue(QString::fromLatin1(CONFIG_WHISPERX_DEVICE),
		QStringLiteral("cuda")));
	settings.ffmpegPath = configuredOrEnv(CONFIG_WHISPERX_FFMPEG_PATH, "CLIP_CROPPER_FFMPEG_PATH");
	settings.model = PluginConfig::getValue(QString::fromLatin1(CONFIG_WHISPERX_MODEL), QStringLiteral("large-v3"))
		.trimmed();
	if (settings.model.isEmpty())
		settings.model = QStringLiteral("large-v3");
	settings.computeType = normalizeWhisperXComputeType(PluginConfig::getValue(
		QString::fromLatin1(CONFIG_WHISPERX_COMPUTE_TYPE), QStringLiteral("float16")));
	bool batchOk = false;
	settings.batchSize = PluginConfig::getValue(QString::fromLatin1(CONFIG_WHISPERX_BATCH_SIZE),
		QStringLiteral("8")).toInt(&batchOk);
	if (!batchOk || settings.batchSize < 1)
		settings.batchSize = 8;
	settings.batchSize = std::min(settings.batchSize, 128);
	return settings;
}

} // namespace Transcription
