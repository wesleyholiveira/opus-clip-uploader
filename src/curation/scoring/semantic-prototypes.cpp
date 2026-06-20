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
			QStringLiteral("the speaker talks about moderators, spam, bans or live rules"),
			QStringLiteral("o streamer organiza convite lobby fila ou operação da live"),
			QStringLiteral("o streamer agradece seguidores inscrições doações ou moedas"),
			QStringLiteral("o streamer fala sobre moderação spam castigo banimento ou regras da live"),
		},
		{
			QStringLiteral("the speaker moves to another viewer question"),
			QStringLiteral("the conversation changes to a different topic"),
			QStringLiteral("a new unrelated chat message starts"),
			QStringLiteral("o assunto muda para outra pergunta"),
		},
		{
			QStringLiteral("a strong short-form clip with a concrete problem, useful insight, and clear takeaway"),
			QStringLiteral("one focused answer that teaches, advises, or emotionally resolves a real viewer problem"),
			QStringLiteral("a meaningful personal reflection that builds to a practical lesson and conclusion"),
			QStringLiteral("practical advice that helps the viewer decide what to do and why"),
			QStringLiteral("mental health advice with careful reasoning and a responsible conclusion"),
			QStringLiteral("a viewer shares a real dilemma and the speaker gives a complete helpful answer"),
			QStringLiteral("um conselho útil com problema claro, explicação e conclusão"),
			QStringLiteral("uma reflexão profunda que vira aprendizado prático no final"),
			QStringLiteral("um desabafo real com resposta empática útil e fechamento"),
			QStringLiteral("uma dica de saúde mental com começo meio e fim"),
			QStringLiteral("um papo filosófico ou emocional com gancho e conclusão forte"),
		},
		{
			QStringLiteral("the clip opens immediately with a compelling question, conflict, or emotional tension"),
			QStringLiteral("the first sentence creates curiosity before any greeting or setup"),
			QStringLiteral("the opening line makes the viewer want to know the answer"),
			QStringLiteral("a surprising, vulnerable, or philosophical statement starts the clip"),
			QStringLiteral("um gancho inicial direto com pergunta conflito ou tensão emocional"),
			QStringLiteral("o corte começa com uma pergunta interessante antes de qualquer saudação"),
		},
		{
			QStringLiteral("the answer reaches a natural conclusion with no new topic afterwards"),
			QStringLiteral("the speaker resolves the point smoothly before the clip ends"),
			QStringLiteral("a complete thought with a satisfying ending and final takeaway"),
			QStringLiteral("a resposta termina com conclusão natural e sem puxar outro assunto"),
			QStringLiteral("o raciocínio fecha suavemente sem virar saudação moderação ou chat"),
		},
		{
			QStringLiteral("only greeting viewers without useful content"),
			QStringLiteral("a clip starts with a donation thank you follower goal or chat greeting"),
			QStringLiteral("a transcript excerpt mixes several unrelated chat interactions"),
			QStringLiteral("thanking donations followers subscribers or coins"),
			QStringLiteral("live moderation instructions about spam bans timeouts or rules"),
			QStringLiteral("stream status updates follower goals chat backlog or reading chat"),
			QStringLiteral("casual banter with several unrelated viewers"),
			QStringLiteral("a clip that ends one chat topic and starts another unrelated video moment"),
			QStringLiteral("a response with no actionable takeaway, no emotional payoff, and no conclusion"),
			QStringLiteral("apenas cumprimentos agradecimentos ou metas da live"),
			QStringLiteral("agradecimento por doação seguidores moedas inscrições ou subs"),
			QStringLiteral("instruções de moderação spam castigo banimento ou regras da live"),
			QStringLiteral("conversa casual com várias pessoas sem conselho nem conclusão"),
			QStringLiteral("um corte que termina um assunto e começa outro assunto sem relação"),
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
			   << QStringLiteral("one useful response to one viewer problem with clear payoff")
			   << QStringLiteral("uma mensagem de espectador seguida por resposta direta")
			   << QStringLiteral("um conselho útil para uma única pergunta do chat");
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
