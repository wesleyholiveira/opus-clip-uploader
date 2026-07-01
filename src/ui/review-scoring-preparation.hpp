#pragma once

#include "models/curation-settings.hpp"

#include <QString>
#include <QJsonArray>
#include <QStringList>

#include <functional>

class QWidget;

struct ReviewScoringPreparationResult {
	CurationSettings settings;
	bool attempted = false;
	bool applied = false;
	bool canceled = false;
	QString summary;
	QString contentId;
	QStringList contentIdAliases;
	QString resolvedProfileId;
	QJsonArray candidateDiagnostics;
};

struct ReviewScoringProgressUpdate {
	QString message;
	int value = 0;
	int maximum = 100;
};

using ReviewScoringProgressCallback = std::function<void(ReviewScoringProgressUpdate)>;

void prepare_review_scoring_async(QWidget *parent, const QString &videoPath, const CurationSettings &baseSettings,
				  std::function<void(ReviewScoringPreparationResult)> finishedCallback,
				  ReviewScoringProgressCallback progressCallback = {});

void prepare_review_scoring_async(QWidget *parent, const QString &videoPath,
				  std::function<void(ReviewScoringPreparationResult)> finishedCallback,
				  ReviewScoringProgressCallback progressCallback = {});
