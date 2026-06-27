#pragma once

#include "models/curation-settings.hpp"

#include <QJsonObject>
#include <QString>

#include <optional>

class QWidget;

namespace ClipCropper::Review {

void showMarkerDiagnosticsDialog(const ClipDuration &range, const QJsonObject &diagnostic, const QString &startLabel,
				 const QString &endLabel, QWidget *parent);

std::optional<QJsonObject> collectStructuredFeedbackDialog(const QString &decision, const ClipDuration &range,
							   const QJsonObject &diagnostic, const QString &startLabel,
							   const QString &endLabel, QWidget *parent);

} // namespace ClipCropper::Review
