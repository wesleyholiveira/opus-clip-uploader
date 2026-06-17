#pragma once

#include "models/curation-settings.hpp"

#include <QString>

#include <functional>

class QWidget;

void generate_custom_prompt_for_curation_async(QWidget *parent, const QString &videoPath,
					       const CurationSettings &curationSettings, bool transcribeOnDemand,
					       std::function<void(QString)> finishedCallback);

void generate_custom_prompt_before_review_async(QWidget *parent, const QString &videoPath, bool transcribeOnDemand,
						std::function<void()> finishedCallback);
