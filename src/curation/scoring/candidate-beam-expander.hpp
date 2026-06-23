#pragma once

#include "curation/scoring/candidate-builder.hpp"

namespace Curation::Scoring {

struct CandidateBeamExpansionOptions {
	CandidateGenerationOptions generation;
	CheapScoringContext scoring;
	CandidateQualityGateOptions qualityGate;
	int finalBudget = 128;
	bool viewerMessagePreset = false;
};

class CandidateBeamExpander {
public:
	QVector<ClipCandidate> expand(const TranscriptIndex &index,
		QVector<ClipCandidate> candidates,
		const CandidateBeamExpansionOptions &options) const;

private:
	QVector<ClipCandidate> expandVariants(const TranscriptIndex &index,
		const ClipCandidate &candidate,
		const CandidateBeamExpansionOptions &options) const;
};

} // namespace Curation::Scoring
