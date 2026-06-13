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
	QVector<ClipDuration> clipDurations;
	QStringList topicKeywords;
	QString genre = "Auto";
	bool skipCurate = false;
	QString model = "ClipAnything";
	QString aiPrompt;
};