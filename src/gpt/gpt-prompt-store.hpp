#pragma once

#include <QString>

class GptPromptStore {
public:
	static QString keyForVideoPath(const QString &videoPath);
	static QString keyForInput(const QString &model, const QString &inputText);
	static QString loadForVideoPath(const QString &videoPath);
	static QString loadForInput(const QString &model, const QString &inputText);
	static void saveForVideoPath(const QString &videoPath, const QString &prompt);
	static void saveForInput(const QString &model, const QString &inputText, const QString &prompt);
	static bool isPendingForVideoPath(const QString &videoPath);
	static void markPendingForVideoPath(const QString &videoPath);
	static void clearPendingForVideoPath(const QString &videoPath);
	static void removeForVideoPath(const QString &videoPath);

private:
	static QString safeFileKey(const QString &videoPath);
};
