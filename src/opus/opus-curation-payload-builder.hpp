#pragma once

#include "models/curation-settings.hpp"

#include <QJsonObject>

class OpusCurationPayloadBuilder {
public:
	QJsonObject build(const ClipDuration &range, const CurationSettings &settings) const;
};
