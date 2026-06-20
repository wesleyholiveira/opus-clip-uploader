#pragma once

#include "models/curation-settings.hpp"

#include <QString>

#include <functional>

class QWidget;

struct ReviewScoringPreparationResult {
	CurationSettings settings;
	bool attempted = false;
	bool applied = false;
	bool canceled = false;
	QString summary;
};

void prepare_review_scoring_async(QWidget *parent, const QString &videoPath, const CurationSettings &baseSettings,
					  std::function<void(ReviewScoringPreparationResult)> finishedCallback);

void prepare_review_scoring_async(QWidget *parent, const QString &videoPath,
					  std::function<void(ReviewScoringPreparationResult)> finishedCallback);
