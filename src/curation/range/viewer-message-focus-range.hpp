#pragma once

#include "models/curation-settings.hpp"
#include "models/transcript.hpp"

#include <QString>
#include <QVector>

struct ViewerMessageFocusRangeResult {
	bool applied = false;
	QString reason;
	QString target;
	QString anchorText;
	ClipDuration manualRange;
	ClipDuration focusRange;
	double confidence = 0.0;
	bool startAdjusted = false;
	QVector<ClipDuration> candidateRanges;
};

class ViewerMessageFocusRangeResolver {
public:
	ViewerMessageFocusRangeResult resolve(const RecordingTranscript &transcript,
							 const CurationSettings &settings,
							 const QString &opusPrompt) const;

	CurationSettings applyFocusRange(const CurationSettings &settings,
					       const ViewerMessageFocusRangeResult &result) const;
};
