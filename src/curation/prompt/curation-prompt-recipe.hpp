#pragma once

#include <QString>

namespace Curation {

struct CurationPromptRecipe {
	QString presetId;
	QString singleOpeningSentence;
	QString multipleOpeningSentence;
	QString prioritySentence;
	QString boundarySentence;

	bool isValid() const
	{
		return !singleOpeningSentence.trimmed().isEmpty() && !prioritySentence.trimmed().isEmpty() &&
		       !boundarySentence.trimmed().isEmpty();
	}
};

QString renderPromptRecipe(const CurationPromptRecipe &recipe, bool multipleClips);
QString renderViewerMessagePrompt(bool multipleClips, const QString &targetSuffix = QString(),
				     bool discoveryMode = true);

} // namespace Curation
