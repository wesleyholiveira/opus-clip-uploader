#pragma once

#include <QString>
#include <QStringView>

#include <algorithm>

namespace Curation::Scoring::EvidenceView {

inline bool equals(QStringView evidence, QStringView value)
{
	return evidence == value;
}

inline bool contains(QStringView evidence, QStringView fragment)
{
	return evidence.contains(fragment);
}

inline bool startsWith(QStringView evidence, QStringView prefix)
{
	return evidence.startsWith(prefix);
}

template <typename EvidenceRange>
inline bool anyEquals(const EvidenceRange &evidence, QStringView value)
{
	return std::any_of(evidence.cbegin(), evidence.cend(), [value](const QString &entry) {
		return equals(QStringView{entry}, value);
	});
}

template <typename EvidenceRange>
inline bool anyContains(const EvidenceRange &evidence, QStringView fragment)
{
	return std::any_of(evidence.cbegin(), evidence.cend(), [fragment](const QString &entry) {
		return contains(QStringView{entry}, fragment);
	});
}

template <typename EvidenceRange>
inline bool anyStartsWith(const EvidenceRange &evidence, QStringView prefix)
{
	return std::any_of(evidence.cbegin(), evidence.cend(), [prefix](const QString &entry) {
		return startsWith(QStringView{entry}, prefix);
	});
}

} // namespace Curation::Scoring::EvidenceView
