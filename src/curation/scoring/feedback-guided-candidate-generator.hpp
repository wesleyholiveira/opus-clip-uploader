#pragma once

#include "curation/feedback/curation-feedback-store.hpp"
#include "curation/scoring/candidate-generator.hpp"
#include "curation/scoring/feedback-similarity-scorer.hpp"
#include "curation/scoring/transcript-index.hpp"

#include <QString>
#include <QStringList>
#include <QVector>

namespace Curation::Scoring {

struct FeedbackGuidedCandidateSeed {
	ClipDuration range;
	QString source;
	QStringList evidence;
	double priorScore = 0.0;
};

struct FeedbackGuidedCandidateGenerationOptions {
	CandidateGenerationOptions generation;
	int maxSeeds = 96;
	int maxSemanticPrototypeSeeds = 48;
	int maxPatternSeeds = 48;
	// Direct replay of approved ranges is useful only during cold start.
	// Once the same video has many positives, exact/boundary seeds become
	// repetition traps and should give budget back to discovery scans.
	int maxExactPositiveSeeds = 6;
	int maxBoundaryVariantSeeds = 8;
};

class FeedbackGuidedCandidateGenerator {
public:
	QVector<FeedbackGuidedCandidateSeed> generate(const TranscriptIndex &index,
		const Curation::Feedback::FeedbackRangeMemory &memory,
		const FeedbackGuidedCandidateGenerationOptions &options) const;

private:
	void appendPositiveRangeSeeds(QVector<FeedbackGuidedCandidateSeed> &seeds,
		const TranscriptIndex &index,
		const Curation::Feedback::FeedbackRangeMemory &memory,
		const FeedbackGuidedCandidateGenerationOptions &options) const;
	void appendPrototypeSimilaritySeeds(QVector<FeedbackGuidedCandidateSeed> &seeds,
		const TranscriptIndex &index,
		const Curation::Feedback::FeedbackRangeMemory &memory,
		const FeedbackGuidedCandidateGenerationOptions &options) const;
	void appendPatternSearchSeeds(QVector<FeedbackGuidedCandidateSeed> &seeds,
		const TranscriptIndex &index,
		const Curation::Feedback::FeedbackRangeMemory &memory,
		const FeedbackGuidedCandidateGenerationOptions &options) const;
	bool appendSeed(QVector<FeedbackGuidedCandidateSeed> &seeds, const TranscriptIndex &index,
		const Curation::Feedback::FeedbackRangeMemory &memory,
		const FeedbackGuidedCandidateGenerationOptions &options,
		FeedbackGuidedCandidateSeed seed) const;
	bool similarSeedExists(const QVector<FeedbackGuidedCandidateSeed> &seeds, const ClipDuration &range) const;
};

} // namespace Curation::Scoring
