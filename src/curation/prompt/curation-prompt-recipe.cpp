#include "curation/prompt/curation-prompt-recipe.hpp"

namespace Curation {

QString renderPromptRecipe(const CurationPromptRecipe &recipe, bool multipleClips)
{
	if (!recipe.isValid())
		return {};

	const QString opening = multipleClips && !recipe.multipleOpeningSentence.trimmed().isEmpty()
					? recipe.multipleOpeningSentence
					: recipe.singleOpeningSentence;

	return QStringLiteral("%1 %2 %3")
		.arg(opening.trimmed(), recipe.prioritySentence.trimmed(), recipe.boundarySentence.trimmed())
		.simplified();
}

QString renderViewerMessagePrompt(bool multipleClips, const QString &targetSuffix, bool discoveryMode)
{
	const QString findPrefix = multipleClips ? QStringLiteral("Find continuous, unbroken clips")
					       : QStringLiteral("Find one continuous, unbroken clip");
	const QString opening = !targetSuffix.trimmed().isEmpty()
					? (multipleClips
						   ? QStringLiteral(
							     "%1, each built from one complete response to a single viewer message specifically %2.")
							     .arg(findPrefix, targetSuffix.trimmed())
						   : QStringLiteral(
							     "%1 built from one complete response to a single viewer message specifically %2.")
							     .arg(findPrefix, targetSuffix.trimmed()))
					: (multipleClips
						   ? QStringLiteral(
							     "%1, each built from one complete response to a single viewer message.")
							     .arg(findPrefix)
						   : QStringLiteral(
							     "%1 built from one complete response to a single viewer message.")
							     .arg(findPrefix));

	const QString priority = discoveryMode && targetSuffix.trimmed().isEmpty()
					 ? QStringLiteral(
						   "Prefer a clearly useful, emotionally consequential viewer message while keeping one exchange, and start with the selected viewer message's first meaningful words; when only the speaker's reaction is audible, start with the first direct reaction.")
					 : QStringLiteral(
						   "Start with the selected viewer message's first meaningful words; when only the speaker's reaction is audible, start with the first direct reaction.");
	const QString boundary = QStringLiteral(
		"Follow only the speaker's direct answer to that same message until its first complete resolution, keeping the clip focused on that one exchange from start to finish.");

	return QStringLiteral("%1 %2 %3").arg(opening, priority, boundary).simplified();
}

} // namespace Curation
