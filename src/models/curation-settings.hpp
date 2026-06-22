#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

struct ClipDuration {
	double startSec = 0;
	double endSec = 0;
};

struct CurationSettings {
	double rangeStartSec = 0;
	double rangeEndSec = 0;
	double originalVideoDurationSec = 0;
	QVector<ClipDuration> clipDurations;
	QStringList topicKeywords;
	QString genre = "Auto";
	QString curationPreset = "auto";
	bool skipCurate = false;
	QString model = "ClipAnything";
	QString clipLengthPreset = "Medium";
	QString sourceLanguage = "auto";
	QString transcriptionLanguage = "auto";
	// Optional manual ClipAnything prompt sent directly to Opus.
	// This is user-provided guidance for Opus curation.
	QString aiPrompt;
	QString reviewSettingsKey;
	bool uploadClipRangesIndependently = false;
};
