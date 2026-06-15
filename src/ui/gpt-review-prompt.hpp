#pragma once

#include <functional>

class QString;
class QWidget;

void generate_custom_prompt_before_review_async(QWidget *parent, const QString &videoPath, bool transcribeOnDemand,
						std::function<void()> finishedCallback);
