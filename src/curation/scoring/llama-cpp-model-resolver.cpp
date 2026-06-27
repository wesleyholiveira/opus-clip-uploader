#include "curation/scoring/llama-cpp-model-resolver.hpp"

#include <obs-module.h>

#include <QDir>
#include <QFileInfo>

#include <memory>

namespace {
using ObsCharPtr = std::unique_ptr<char, decltype(&bfree)>;

QString obsModuleModelPath(const QString &fileName)
{
	const QByteArray relativePath = QStringLiteral("models/%1").arg(fileName).toUtf8();
	ObsCharPtr modulePath(obs_module_file(relativePath.constData()), bfree);
	if (!modulePath || !modulePath.get() || modulePath.get()[0] == '\0')
		return {};
	return QDir::fromNativeSeparators(QString::fromUtf8(modulePath.get()));
}

QString legacyPluginDir()
{
	const QString appData = QString::fromLocal8Bit(qgetenv("APPDATA"));
	if (!appData.trimmed().isEmpty())
		return QDir::fromNativeSeparators(appData) + QStringLiteral("/obs-studio/plugins/clip-cropper");
	return QDir::homePath() + QStringLiteral("/AppData/Roaming/obs-studio/plugins/clip-cropper");
}

QStringList candidateFileNames(const QString &configuredModel)
{
	QString value = QDir::fromNativeSeparators(configuredModel.trimmed());
	if (value.isEmpty())
		return {};

	const QFileInfo info(value);
	QStringList names;
	names.append(value);

	// The old setting was named "model id" because it targeted llama-server. For
	// native llama.cpp, accept both a complete path and the same bare id, looking
	// for a local GGUF with a conventional extension.
	if (info.suffix().isEmpty()) {
		names.append(value + QStringLiteral(".gguf"));
		names.append(value + QStringLiteral(".GGUF"));
	}
	names.removeDuplicates();
	return names;
}

QString normalizedLookupKey(const QString &value)
{
	QString key = QFileInfo(QDir::fromNativeSeparators(value.trimmed())).fileName().toLower();
	if (key.endsWith(QStringLiteral(".gguf")))
		key.chop(5);
	key.remove(QChar('_'));
	key.remove(QChar('-'));
	key.remove(QChar('.'));
	key.remove(QChar(' '));
	return key;
}

QStringList modelDirectories()
{
	QStringList dirs;
	const QString legacy = legacyPluginDir();
	if (!legacy.trimmed().isEmpty()) {
		dirs.append(QDir(legacy).filePath(QStringLiteral("models")));
		dirs.append(QDir(legacy).filePath(QStringLiteral("data/models")));
	}
	dirs.append(QDir::current().filePath(QStringLiteral("models")));
	dirs.append(QDir::currentPath());
	dirs.removeDuplicates();
	return dirs;
}

QString findCaseInsensitiveGgufMatch(const QString &configuredModel)
{
	const QString wanted = normalizedLookupKey(configuredModel);
	if (wanted.isEmpty())
		return {};
	QString bestPartial;
	for (const QString &dirPath : modelDirectories()) {
		QDir dir(dirPath);
		if (!dir.exists())
			continue;
		const QFileInfoList files =
			dir.entryInfoList(QStringList{QStringLiteral("*.gguf"), QStringLiteral("*.GGUF")},
					  QDir::Files | QDir::Readable, QDir::Name);
		for (const QFileInfo &file : files) {
			const QString key = normalizedLookupKey(file.fileName());
			if (key == wanted)
				return QDir::fromNativeSeparators(file.absoluteFilePath());
			if (bestPartial.isEmpty() && (key.contains(wanted) || wanted.contains(key)))
				bestPartial = QDir::fromNativeSeparators(file.absoluteFilePath());
		}
	}
	return bestPartial;
}
} // namespace

QStringList Curation::Scoring::llamaCppModelSearchPaths(const QString &configuredModel)
{
	QStringList result;
	const QStringList names = candidateFileNames(configuredModel);
	for (const QString &name : names) {
		const QFileInfo info(name);
		if (info.isAbsolute()) {
			result.append(info.absoluteFilePath());
			continue;
		}

		const QString moduleModel = obsModuleModelPath(name);
		if (!moduleModel.trimmed().isEmpty())
			result.append(moduleModel);

		const QString legacy = legacyPluginDir();
		if (!legacy.trimmed().isEmpty()) {
			result.append(QDir(legacy).filePath(QStringLiteral("models/%1").arg(name)));
			result.append(QDir(legacy).filePath(QStringLiteral("data/models/%1").arg(name)));
		}

		result.append(QDir::current().filePath(QStringLiteral("models/%1").arg(name)));
		result.append(QDir::current().filePath(name));
	}
	result.removeDuplicates();
	return result;
}

QString Curation::Scoring::resolveLlamaCppModelPath(const QString &configuredModel)
{
	const QStringList paths = llamaCppModelSearchPaths(configuredModel);
	for (const QString &path : paths) {
		if (QFileInfo(path).isFile())
			return QDir::fromNativeSeparators(path);
	}

	const QString scanned = findCaseInsensitiveGgufMatch(configuredModel);
	if (!scanned.isEmpty()) {
		blog(LOG_INFO,
		     "[clip-cropper] Native llama.cpp model resolved by case-insensitive GGUF scan. requested=%s path=%s",
		     configuredModel.toUtf8().constData(), scanned.toUtf8().constData());
		return scanned;
	}
	return {};
}
