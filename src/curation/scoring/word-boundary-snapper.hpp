#pragma once

#include "models/curation-settings.hpp"
#include "models/transcript.hpp"

#include <QVector>

namespace Curation::Scoring {

class WordBoundarySnapper {
public:
	ClipDuration snap(const ClipDuration &range, const ClipDuration &bounds,
			  const QVector<WordTiming> &words) const;
};

} // namespace Curation::Scoring
