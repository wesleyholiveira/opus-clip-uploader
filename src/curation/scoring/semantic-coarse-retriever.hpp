#pragma once

#include "curation/scoring/semantic-model.hpp"
#include "curation/scoring/semantic-prototypes.hpp"
#include "curation/scoring/transcript-index.hpp"

#include <QString>
#include <QStringList>
#include <QVector>

namespace Curation::Scoring {

struct SemanticCoarseRetrievalOptions {
	bool enabled = true;
	ClipDuration searchRange;
	double windowSec = 45.0;
	double strideSec = 30.0;
	double regionPaddingSec = 24.0;
	int maxWindowsToEmbed = 72;
	int maxRegions = 8;
	int maxPrototypeTexts = 8;
	int minWindowTextChars = 40;
	int maxWindowTextChars = 1400;
	double minRegionSpacingSec = 0.0;
};

struct SemanticCoarseRetrievalContext {
	QString presetId;
	QString mainTarget;
	bool reliableMainTarget = false;
};

struct SemanticCoarseRegion {
	ClipDuration range;
	double score = 0.0;
	double targetScore = 0.0;
	double viewerScore = 0.0;
	double directAnswerScore = 0.0;
	double clipValueScore = 0.0;
	double metaNoiseScore = 0.0;
	double noiseScore = 0.0;
	QString textSample;
	QStringList evidence;
};

class SemanticCoarseRetriever {
public:
	QVector<SemanticCoarseRegion> retrieve(const TranscriptIndex &index,
						 const SemanticCoarseRetrievalContext &context,
						 const SemanticCoarseRetrievalOptions &options,
						 const SemanticEmbeddingProvider *provider) const;

private:
	QVector<ClipDuration> buildWindows(const TranscriptIndex &index,
					      const SemanticCoarseRetrievalOptions &options) const;
	QStringList prototypesForContext(const SemanticCoarseRetrievalContext &context,
					       const SemanticPrototypeSet &prototypes,
					       int maxPrototypeTexts) const;
	double maxSimilarity(const SemanticEmbedding &embedding,
				     const QVector<SemanticEmbedding> &prototypes) const;
	double scoreForWindow(double targetScore, double viewerScore, double directAnswerScore, double clipValueScore,
			      double noiseScore, double metaNoiseScore, const SemanticCoarseRetrievalContext &context) const;
	ClipDuration paddedRegion(const ClipDuration &window, const TranscriptIndex &index,
				       const SemanticCoarseRetrievalOptions &options) const;
	bool overlapsOrTooClose(const ClipDuration &left, const ClipDuration &right,
				     double minSpacingSec) const;
};

} // namespace Curation::Scoring
