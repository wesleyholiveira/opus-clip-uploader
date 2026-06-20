#pragma once

#include "models/curation-settings.hpp"
#include "models/transcript.hpp"

#include <QString>
#include <QVector>

#include <memory>

class CurationRangeStrategyResolution {
public:
	enum class Mode {
		Unchanged,
		FocusRange,
		CandidateRanges,
	};

	Mode mode = Mode::Unchanged;
	QString strategyName;
	QString reason;
	QString target;
	QString anchorText;
	QString details;
	ClipDuration manualRange;
	QVector<ClipDuration> ranges;
	double confidence = 0.0;
	bool startAdjusted = false;

	bool applied() const { return mode != Mode::Unchanged && !ranges.isEmpty(); }
};

class CurationRangeStrategy {
public:
	virtual ~CurationRangeStrategy() = default;
	virtual QString name() const = 0;
	virtual CurationRangeStrategyResolution resolve(const RecordingTranscript &transcript,
							      const CurationSettings &settings,
							      const QString &opusPrompt) const = 0;
};

class CurationRangeStrategyResolver {
public:
	CurationRangeStrategyResolver();

	CurationRangeStrategyResolution resolve(const RecordingTranscript &transcript,
						      const CurationSettings &settings,
						      const QString &opusPrompt) const;

	CurationSettings apply(const CurationSettings &settings,
				     const CurationRangeStrategyResolution &resolution) const;

private:
	QVector<std::shared_ptr<CurationRangeStrategy>> strategies;
};
