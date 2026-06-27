#pragma once

#include "models/curation-settings.hpp"

#include <QSet>
#include <QString>
#include <QVector>

#include <utility>

namespace Curation::Scoring::CandidateRangeUtils {
QString rangeKey(const ClipDuration &range);
}

namespace Curation::Scoring::CandidateDedupe {

inline bool markSeen(QSet<QString> &seen, const ClipDuration &range)
{
	const QString key = CandidateRangeUtils::rangeKey(range);
	if (seen.contains(key))
		return false;
	seen.insert(key);
	return true;
}

template<typename CandidateLike> inline bool markSeen(QSet<QString> &seen, const CandidateLike &candidate)
{
	return markSeen(seen, candidate.range);
}

template<typename CandidateLike>
inline void appendUniqueRangeMoved(QVector<CandidateLike> &target, QSet<QString> &seen, CandidateLike candidate)
{
	if (markSeen(seen, candidate))
		target.append(std::move(candidate));
}

} // namespace Curation::Scoring::CandidateDedupe
