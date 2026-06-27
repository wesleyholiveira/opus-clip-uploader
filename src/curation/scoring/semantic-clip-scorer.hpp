#pragma once

#include "curation/scoring/clip-candidate.hpp"
#include "curation/scoring/semantic-model.hpp"
#include "curation/scoring/transcript-index.hpp"

#include <QString>
#include <QStringList>

namespace Curation::Scoring {

struct SemanticClipScoringOptions {
	bool enabled = true;
	int minTextChars = 40;
	int maxPrototypeTexts = 8;
	bool enablePairwiseTopicContinuity = false;
	double highConfidenceMatch = 0.72;
};

struct SemanticScoringContext {
	QString presetId;
	QString mainTarget;
	QString transcriptionLanguage;
	QString sourceLanguage;
	bool reliableMainTarget = false;
};

class SemanticClipScorer {
public:
	ClipCandidate score(const TranscriptIndex &index, const ClipCandidate &candidate,
			    const SemanticScoringContext &context, const SemanticClipScoringOptions &options,
			    const SemanticEmbeddingProvider *provider) const;
	QVector<ClipCandidate> scoreBatch(const TranscriptIndex &index, const QVector<ClipCandidate> &candidates,
					  const SemanticScoringContext &context,
					  const SemanticClipScoringOptions &options,
					  const SemanticEmbeddingProvider *provider) const;

private:
	double maxPrototypeSimilarity(const SemanticEmbeddingProvider &provider, const SemanticEmbedding &textEmbedding,
				      const QStringList &prototypes) const;
	double topicContinuityByEmbedding(const TranscriptIndex &index, const ClipCandidate &candidate,
					  const SemanticEmbeddingProvider &provider) const;
	void scoreOpeningAndEnding(const TranscriptIndex &index, ClipCandidate &candidate,
				   const SemanticEmbeddingProvider &provider, const QStringList &hookPrototypes,
				   const QStringList &resolutionPrototypes, const QStringList &metaNoisePrototypes,
				   const QStringList &topicShiftPrototypes) const;
	double combineFinalScore(const ClipCandidate &candidate, const SemanticScoringContext &context) const;
	QString firstHalfText(const TranscriptIndex &index, const ClipCandidate &candidate) const;
	QString secondHalfText(const TranscriptIndex &index, const ClipCandidate &candidate) const;
	QString openingText(const TranscriptIndex &index, const ClipCandidate &candidate) const;
	QString endingText(const TranscriptIndex &index, const ClipCandidate &candidate) const;
};

} // namespace Curation::Scoring
