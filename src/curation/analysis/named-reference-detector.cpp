#include "curation/analysis/named-reference-detector.hpp"

#include <QMap>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace {

bool containsAnyPhrase(const QString &text, const QStringList &phrases)
{
	for (const QString &phrase : phrases) {
		if (text.contains(phrase, Qt::CaseInsensitive))
			return true;
	}
	return false;
}

QString joinedText(const RecordingTranscript &transcript)
{
	QString text;
	for (const TranscriptSegment &segment : transcript.segments) {
		if (!segment.text.trimmed().isEmpty()) {
			if (!text.isEmpty())
				text += QLatin1Char(' ');
			text += segment.text.trimmed();
		}
	}
	return text;
}

QStringList namedReferenceWords(const QString &reference)
{
	return reference.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
}

bool sameReferenceWord(const QString &a, const QString &b)
{
	return a.compare(b, Qt::CaseInsensitive) == 0;
}

QString mergedOverlappingReference(const QString &a, const QString &b)
{
	const QStringList aWords = namedReferenceWords(a);
	const QStringList bWords = namedReferenceWords(b);
	if (aWords.isEmpty() || bWords.isEmpty())
		return {};

	const int maxOverlap = std::min(aWords.size(), bWords.size());
	for (int overlap = maxOverlap; overlap >= 1; --overlap) {
		bool aSuffixMatchesBPrefix = true;
		for (int index = 0; index < overlap; ++index) {
			if (!sameReferenceWord(aWords.at(aWords.size() - overlap + index), bWords.at(index))) {
				aSuffixMatchesBPrefix = false;
				break;
			}
		}

		if (aSuffixMatchesBPrefix) {
			QStringList merged = aWords;
			for (int index = overlap; index < bWords.size(); ++index)
				merged.append(bWords.at(index));
			return merged.join(QLatin1Char(' ')).simplified();
		}

		bool bSuffixMatchesAPrefix = true;
		for (int index = 0; index < overlap; ++index) {
			if (!sameReferenceWord(bWords.at(bWords.size() - overlap + index), aWords.at(index))) {
				bSuffixMatchesAPrefix = false;
				break;
			}
		}

		if (bSuffixMatchesAPrefix) {
			QStringList merged = bWords;
			for (int index = overlap; index < aWords.size(); ++index)
				merged.append(aWords.at(index));
			return merged.join(QLatin1Char(' ')).simplified();
		}
	}

	return {};
}

void addOverlappingNamedReferences(QMap<QString, int> &counts)
{
	bool changed = true;
	while (changed) {
		changed = false;
		const QStringList references = counts.keys();

		for (int i = 0; i < references.size(); ++i) {
			for (int j = i + 1; j < references.size(); ++j) {
				const QString merged = mergedOverlappingReference(references.at(i), references.at(j));
				if (merged.isEmpty())
					continue;

				const int mergedWordCount = namedReferenceWords(merged).size();
				const int currentMaxWordCount = std::max(namedReferenceWords(references.at(i)).size(),
									 namedReferenceWords(references.at(j)).size());
				if (mergedWordCount <= currentMaxWordCount || mergedWordCount > 6)
					continue;

				const int mergedCount = counts.value(references.at(i)) + counts.value(references.at(j));
				if (counts.value(merged) < mergedCount) {
					counts[merged] = mergedCount;
					changed = true;
				}
			}
		}
	}
}

} // namespace

namespace Curation {

QStringList importantNamedReferences(const RecordingTranscript &transcript)
{
	QMap<QString, int> counts;
	static const QSet<QString> ignored = {QStringLiteral("The"),      QStringLiteral("This"),
					      QStringLiteral("That"),     QStringLiteral("Then"),
					      QStringLiteral("And"),      QStringLiteral("But"),
					      QStringLiteral("So"),       QStringLiteral("If"),
					      QStringLiteral("When"),     QStringLiteral("For"),
					      QStringLiteral("Week"),     QStringLiteral("German"),
					      QStringLiteral("Japanese"), QStringLiteral("Brazilian"),
					      QStringLiteral("Brazil"),   QStringLiteral("UTC")};
	static const QSet<QString> ignoredLowerPrefixes = {
		QStringLiteral("and"),     QStringLiteral("but"),    QStringLiteral("the"),
		QStringLiteral("this"),    QStringLiteral("that"),   QStringLiteral("then"),
		QStringLiteral("when"),    QStringLiteral("with"),   QStringLiteral("from"),
		QStringLiteral("through"), QStringLiteral("using"),  QStringLiteral("like"),
		QStringLiteral("about"),   QStringLiteral("after"),  QStringLiteral("before"),
		QStringLiteral("into"),    QStringLiteral("only"),   QStringLiteral("also"),
		QStringLiteral("real"),    QStringLiteral("first"),  QStringLiteral("second"),
		QStringLiteral("third"),   QStringLiteral("week"),   QStringLiteral("what"),
		QStringLiteral("where"),   QStringLiteral("which"),  QStringLiteral("while"),
		QStringLiteral("because"), QStringLiteral("there"),  QStringLiteral("their"),
		QStringLiteral("your"),    QStringLiteral("some"),   QStringLiteral("many"),
		QStringLiteral("very"),    QStringLiteral("just"),   QStringLiteral("kind"),
		QStringLiteral("sort"),    QStringLiteral("will"),   QStringLiteral("would"),
		QStringLiteral("could"),   QStringLiteral("should"), QStringLiteral("have"),
		QStringLiteral("been"),    QStringLiteral("were"),   QStringLiteral("they"),
		QStringLiteral("them"),    QStringLiteral("than"),   QStringLiteral("how"),
		QStringLiteral("why"),     QStringLiteral("who"),    QStringLiteral("whom"),
		QStringLiteral("whose")};

	const QRegularExpression namedPhrasePattern(
		QStringLiteral("\\b([A-Z][\\p{L}0-9]*(?:\\s+[A-Z][\\p{L}0-9]*){1,3})\\b"),
		QRegularExpression::UseUnicodePropertiesOption);
	const QRegularExpression prefixedNamedPhrasePattern(
		QStringLiteral("\\b([a-z][\\p{L}0-9]{2,})\\s+([A-Z][\\p{L}0-9]*(?:\\s+[A-Z][\\p{L}0-9]*){1,3})\\b"),
		QRegularExpression::UseUnicodePropertiesOption);

	for (const TranscriptSegment &segment : transcript.segments) {
		const QString text = segment.text.trimmed();
		if (text.isEmpty())
			continue;

		QRegularExpressionMatchIterator prefixedIterator = prefixedNamedPhrasePattern.globalMatch(text);
		while (prefixedIterator.hasNext()) {
			const QRegularExpressionMatch match = prefixedIterator.next();
			const QString prefix = match.captured(1).simplified();
			QString reference = match.captured(2).simplified();
			if (prefix.isEmpty() || reference.isEmpty())
				continue;

			if (ignoredLowerPrefixes.contains(prefix.toLower()))
				continue;

			const QString firstReferenceWord = reference.section(QLatin1Char(' '), 0, 0).trimmed();
			if (ignored.contains(reference) || ignored.contains(firstReferenceWord))
				continue;

			QString normalizedPrefix = prefix.toLower();
			normalizedPrefix.replace(0, 1, normalizedPrefix.left(1).toUpper());
			reference = QStringList{normalizedPrefix, reference}.join(QLatin1Char(' ')).simplified();
			counts[reference] += 2;
		}

		QRegularExpressionMatchIterator iterator = namedPhrasePattern.globalMatch(text);
		while (iterator.hasNext()) {
			QString reference = iterator.next().captured(1).simplified();
			if (reference.isEmpty())
				continue;

			const QString firstWord = reference.section(QLatin1Char(' '), 0, 0).trimmed();
			if (ignored.contains(reference) || ignored.contains(firstWord))
				continue;

			counts[reference] += 1;
		}
	}

	addOverlappingNamedReferences(counts);

	QStringList references = counts.keys();
	std::sort(references.begin(), references.end(), [&counts](const QString &a, const QString &b) {
		const int wordCountA = namedReferenceWords(a).size();
		const int wordCountB = namedReferenceWords(b).size();
		if (wordCountA != wordCountB)
			return wordCountA > wordCountB;

		if (a.size() != b.size())
			return a.size() > b.size();

		const int countA = counts.value(a);
		const int countB = counts.value(b);
		if (countA != countB)
			return countA > countB;

		return a.localeAwareCompare(b) < 0;
	});

	QStringList output;
	for (const QString &reference : references) {
		bool coveredByLongerReference = false;
		for (const QString &existing : output) {
			if (existing.contains(reference, Qt::CaseInsensitive) ||
			    reference.contains(existing, Qt::CaseInsensitive)) {
				coveredByLongerReference = true;
				break;
			}
		}

		if (!coveredByLongerReference)
			output.append(reference);
	}

	std::sort(output.begin(), output.end(), [&counts](const QString &a, const QString &b) {
		const int wordCountA = namedReferenceWords(a).size();
		const int wordCountB = namedReferenceWords(b).size();
		if (wordCountA != wordCountB)
			return wordCountA > wordCountB;

		if (a.size() != b.size())
			return a.size() > b.size();

		const int countA = counts.value(a);
		const int countB = counts.value(b);
		if (countA != countB)
			return countA > countB;

		return a.localeAwareCompare(b) < 0;
	});

	if (output.size() > 6)
		output = output.mid(0, 6);

	return output;
}

QString formatImportantNamedReferences(const RecordingTranscript &transcript)
{
	const QStringList references = importantNamedReferences(transcript);
	if (references.isEmpty())
		return QStringLiteral("No strong named reference detected in the selected range.");

	return references.join(QStringLiteral(", "));
}

bool transcriptHasPronounDependentNamedReference(const RecordingTranscript &transcript)
{
	if (importantNamedReferences(transcript).isEmpty())
		return false;

	const QString text = joinedText(transcript).toLower();
	return containsAnyPhrase(text, {QStringLiteral("the way she"), QStringLiteral("the way he"),
					QStringLiteral("the way they"), QStringLiteral("she teaches"),
					QStringLiteral("he teaches"), QStringLiteral("they teach"),
					QStringLiteral("this method"), QStringLiteral("that approach"),
					QStringLiteral("the way it works")});
}

bool transcriptHasReferenceBackedUnlockMethod(const RecordingTranscript &transcript)
{
	if (importantNamedReferences(transcript).isEmpty())
		return false;

	const QString lower = joinedText(transcript).toLower();
	const bool unlockMethod = containsAnyPhrase(lower, {QStringLiteral("unlock"), QStringLiteral("key clicks"),
							    QStringLiteral("one key"), QStringLiteral("one logic"),
							    QStringLiteral("core logic")});
	if (!unlockMethod)
		return false;

	const bool methodReferenceSignal = containsAnyPhrase(
		lower, {QStringLiteral("method"), QStringLiteral("approach"), QStringLiteral("framework"),
			QStringLiteral("style"), QStringLiteral("language-learning"), QStringLiteral("the way"),
			QStringLiteral("teaches"), QStringLiteral("teach")});

	return methodReferenceSignal && (transcriptHasPronounDependentNamedReference(transcript) ||
					 !importantNamedReferences(transcript).isEmpty());
}

bool promptContainsAnyNamedReference(const QString &prompt, const RecordingTranscript &transcript)
{
	const QString lowerPrompt = prompt.toLower();
	for (const QString &reference : importantNamedReferences(transcript)) {
		const QString lowerReference = reference.toLower();
		if (lowerPrompt.contains(lowerReference))
			return true;

		const QStringList parts = reference.split(QLatin1Char(' '), Qt::SkipEmptyParts);
		if (parts.size() >= 2) {
			const QString shortReference =
				QStringList{parts.at(0), parts.at(1)}.join(QStringLiteral(" ")).toLower();
			if (lowerPrompt.contains(shortReference))
				return true;
		}
	}

	return false;
}

} // namespace Curation
