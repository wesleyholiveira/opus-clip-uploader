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
						   "Prefer the clearest emotionally consequential viewer issue and start at the self-contained sentence that states it.")
					 : QStringLiteral(
						   "Start with the viewer message's clearest self-contained sentence that states the issue.");
	const QString boundary = QStringLiteral(
		"Follow the speaker's direct answer through its first local resolution, ending as soon as the speaker leaves that exchange.");

	return QStringLiteral("%1 %2 %3").arg(opening, priority, boundary).simplified();
}

} // namespace Curation
