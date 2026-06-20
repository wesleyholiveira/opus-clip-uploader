#include "curation/scoring/semantic-prototypes.hpp"

using namespace Curation::Scoring;

const SemanticPrototypeSet &Curation::Scoring::defaultSemanticPrototypes()
{
	static const SemanticPrototypeSet prototypes = {
		{
			QStringLiteral("a viewer asks a personal question in chat"),
			QStringLiteral("a chat message asks the speaker for advice"),
			QStringLiteral("a viewer explains a problem and asks what to do"),
			QStringLiteral("the speaker reads a viewer's question before answering"),
			QStringLiteral("uma pergunta de espectador pedindo conselho"),
			QStringLiteral("uma mensagem do chat contando um problema pessoal"),
		},
		{
			QStringLiteral("the speaker gives a direct answer to the viewer's question"),
			QStringLiteral("the speaker explains what the viewer should do"),
			QStringLiteral("a complete practical response to the same viewer message"),
			QStringLiteral("uma resposta direta para a pergunta do espectador"),
			QStringLiteral("um conselho completo sobre a mesma mensagem do chat"),
		},
		{
			QStringLiteral("the speaker greets the audience"),
			QStringLiteral("the speaker says hello or welcomes someone"),
			QStringLiteral("the speaker thanks someone for joining"),
			QStringLiteral("uma saudação rápida para o chat"),
			QStringLiteral("o streamer dá boa noite ou boas vindas"),
		},
		{
			QStringLiteral("the speaker manages stream logistics"),
			QStringLiteral("the speaker talks about invites, lobby, queue, donations or subscriptions"),
			QStringLiteral("chat backlog or stream management without a meaningful answer"),
			QStringLiteral("o streamer organiza convite lobby fila ou operação da live"),
		},
		{
			QStringLiteral("the speaker moves to another viewer question"),
			QStringLiteral("the conversation changes to a different topic"),
			QStringLiteral("a new unrelated chat message starts"),
			QStringLiteral("o assunto muda para outra pergunta"),
		}
	};
	return prototypes;
}

QStringList Curation::Scoring::targetPrototypesForPreset(const QString &presetId, const QString &mainTarget)
{
	QStringList prototypes;
	const QString target = mainTarget.trimmed();
	if (!target.isEmpty()) {
		prototypes << QStringLiteral("a self-contained clip specifically about %1").arg(target)
			   << QStringLiteral("the speaker stays on the topic of %1").arg(target)
			   << QStringLiteral("a complete answer about %1").arg(target);
	}

	if (presetId == QStringLiteral("viewer_message_response")) {
		prototypes << QStringLiteral("a viewer message followed by the speaker's direct response")
			   << QStringLiteral("one complete answer to a single viewer question")
			   << QStringLiteral("uma mensagem de espectador seguida por resposta direta");
	} else if (presetId == QStringLiteral("advice_answer")) {
		prototypes << QStringLiteral("practical advice with a clear recommendation")
			   << QStringLiteral("a useful answer explaining what someone should do");
	} else if (presetId == QStringLiteral("emotional_reaction")) {
		prototypes << QStringLiteral("a strong emotional reaction with a clear payoff")
			   << QStringLiteral("the speaker reacts with surprise, humor, anger, sadness or excitement");
	} else if (presetId == QStringLiteral("explanation")) {
		prototypes << QStringLiteral("a clear explanation of one idea")
			   << QStringLiteral("the speaker explains a concept from setup to resolution");
	}

	prototypes.removeDuplicates();
	return prototypes;
}
