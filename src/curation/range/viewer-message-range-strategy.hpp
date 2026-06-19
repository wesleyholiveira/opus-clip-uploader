#pragma once

#include "curation/range/curation-range-strategy.hpp"

class ViewerMessageResponseRangeStrategy final : public CurationRangeStrategy {
public:
	QString name() const override;
	CurationRangeStrategyResolution resolve(const RecordingTranscript &transcript,
							      const CurationSettings &settings,
							      const QString &opusPrompt) const override;
};
