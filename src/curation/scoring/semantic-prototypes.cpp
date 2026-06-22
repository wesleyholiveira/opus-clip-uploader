#include "curation/scoring/semantic-prototypes.hpp"

using namespace Curation::Scoring;

namespace {

static QString normalizedLanguageToken(const QString &value)
{
	QString language = value.trimmed().toLower();
	language.replace(QLatin1Char('_'), QLatin1Char('-'));
	if (language.isEmpty() || language == QStringLiteral("auto") || language == QStringLiteral("default"))
		return {};
	return language;
}

static bool isPortugueseToken(const QString &language)
{
	return language == QStringLiteral("pt") || language.startsWith(QStringLiteral("pt-")) ||
		language == QStringLiteral("portuguese") || language == QStringLiteral("portugues") ||
		language == QStringLiteral("português");
}

static QStringList uniqueTexts(QStringList values)
{
	QStringList result;
	for (const QString &value : values) {
		const QString text = value.simplified();
		if (!text.isEmpty() && !result.contains(text))
			result.append(text);
	}
	return result;
}

} // namespace

QString Curation::Scoring::normalizedSemanticLanguageCode(const QString &transcriptionLanguage,
	const QString &sourceLanguage)
{
	const QString transcription = normalizedLanguageToken(transcriptionLanguage);
	if (!transcription.isEmpty()) {
		if (isPortugueseToken(transcription))
			return QStringLiteral("pt");
		if (transcription == QStringLiteral("en") || transcription.startsWith(QStringLiteral("en-")) ||
		    transcription == QStringLiteral("english"))
			return QStringLiteral("en");
		return transcription.left(16);
	}

	const QString source = normalizedLanguageToken(sourceLanguage);
	if (!source.isEmpty()) {
		if (isPortugueseToken(source))
			return QStringLiteral("pt");
		if (source == QStringLiteral("en") || source.startsWith(QStringLiteral("en-")) ||
		    source == QStringLiteral("english"))
			return QStringLiteral("en");
		return source.left(16);
	}

	return QStringLiteral("auto");
}

QString Curation::Scoring::semanticLanguageDisplayName(const QString &languageCode)
{
	const QString language = normalizedSemanticLanguageCode(languageCode);
	if (language == QStringLiteral("pt"))
		return QStringLiteral("português do Brasil");
	if (language == QStringLiteral("en"))
		return QStringLiteral("English");
	if (language == QStringLiteral("auto"))
		return QStringLiteral("the transcript language");
	return language;
}

bool Curation::Scoring::isPortugueseSemanticLanguage(const QString &languageCode)
{
	return normalizedSemanticLanguageCode(languageCode) == QStringLiteral("pt");
}

QString Curation::Scoring::semanticLanguageInstruction(const QString &languageCode)
{
	const QString language = normalizedSemanticLanguageCode(languageCode);
	if (language == QStringLiteral("pt")) {
		return QStringLiteral(
			"Avalie trechos de transcrição em português do Brasil. Dê preferência a linguagem natural de live/chat brasileiro. ");
	}
	if (language == QStringLiteral("en"))
		return QStringLiteral("Evaluate transcript excerpts in English. ");
	if (language == QStringLiteral("auto"))
		return QStringLiteral("Evaluate transcript excerpts in their original language. ");
	return QStringLiteral("Evaluate transcript excerpts in %1. ").arg(semanticLanguageDisplayName(language));
}

QString Curation::Scoring::semanticDocumentPrefix(const QString &languageCode)
{
	const QString language = normalizedSemanticLanguageCode(languageCode);
	if (language == QStringLiteral("pt"))
		return QStringLiteral("Idioma da transcrição: português do Brasil. ");
	if (language == QStringLiteral("en"))
		return QStringLiteral("Transcript language: English. ");
	if (language == QStringLiteral("auto"))
		return QStringLiteral("Transcript language: original language. ");
	return QStringLiteral("Transcript language: %1. ").arg(semanticLanguageDisplayName(language));
}

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
			QStringLiteral("the speaker looks for, checks, or reads chat before the actual useful topic starts"),
			QStringLiteral("the speaker says they are checking a message before the real answer begins"),
			QStringLiteral("technical troubleshooting about a viewer connection, stream setup, audio, video, lag or internet before useful content"),
			QStringLiteral("the speaker helps someone adjust connection settings or live technical setup instead of answering the main topic"),
			QStringLiteral("the speaker talks about moderators, spam, bans or live rules"),
			QStringLiteral("the speaker is setting up the stream, background game, overlay, live start, or stream context without a useful answer"),
			QStringLiteral("the speaker scolds chat about greetings, good night messages, or asking the background game name instead of giving a useful answer"),
			QStringLiteral("light social check-in, small talk, body status, or casual availability question before any useful topic"),
			QStringLiteral("brief trivia answer followed by a topic change without a complete arc"),
			QStringLiteral("o streamer organiza convite lobby fila ou operação da live"),
			QStringLiteral("o streamer agradece apoio do chat, inscrições, doações ou itens virtuais"),
			QStringLiteral("o streamer agradece apoio do chat, itens virtuais, inscrições, doações ou elogios"),
			QStringLiteral("the speaker thanks gifts compliments donations subs virtual gifts coins badges or follower messages"),
			QStringLiteral("the opening is only a virtual gift thank you before the real subject starts"),
			QStringLiteral("o streamer fala sobre moderação spam castigo banimento ou regras da live"),
		},
		{
			QStringLiteral("the speaker moves to another viewer question"),
			QStringLiteral("the conversation changes to a different topic"),
			QStringLiteral("a new unrelated chat message starts"),
			QStringLiteral("a different viewer compliment or comment starts after the answer"),
			QStringLiteral("o assunto muda para outra pergunta"),
			QStringLiteral("começa outro comentário de espectador depois da conclusão"),
		},
		{
			QStringLiteral("a strong short-form clip with a concrete problem, useful insight, and clear takeaway"),
			QStringLiteral("one focused answer that teaches, advises, or emotionally resolves a real viewer problem"),
			QStringLiteral("a meaningful personal reflection that builds to a practical lesson and conclusion"),
			QStringLiteral("a curiosity-driven standalone moment with setup, development, and payoff"),
			QStringLiteral("an interesting story, opinion, or reflection that creates curiosity and resolves locally"),
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
			QStringLiteral("a viewer asks for relationship advice after a breakup, blame, rejection, or romantic conflict"),
			QStringLiteral("a viewer shares vulnerability, trauma, depression, treatment, recovery, or a difficult personal situation"),
			QStringLiteral("an empathetic response to someone describing mental health, therapy, a clinic, isolation, grief, or life struggle"),
			QStringLiteral("the speaker gives careful emotional support or responsible advice to a vulnerable viewer"),
			QStringLiteral("context about a viewer spending time in a clinic, struggling emotionally, or asking for mental health advice"),
			QStringLiteral("viewer vulnerability creates empathy and the answer develops a helpful point"),
			QStringLiteral("um viewer compartilha vulnerabilidade, depressão, terapia, clínica, sofrimento ou dificuldade pessoal"),
			QStringLiteral("resposta empática a uma pessoa em sofrimento emocional ou problema de saúde mental"),
			QStringLiteral("conselho cuidadoso sobre saúde mental com contexto humano e fechamento responsável"),
		},
		{
			QStringLiteral("the clip opens immediately with a compelling question, conflict, or emotional tension"),
			QStringLiteral("the first sentence creates curiosity before any greeting or setup"),
			QStringLiteral("the opening creates curiosity even without an explicit question"),
			QStringLiteral("the opening line makes the viewer want to know the answer"),
			QStringLiteral("a surprising, vulnerable, or philosophical statement starts the clip"),
			QStringLiteral("um gancho inicial direto com pergunta conflito ou tensão emocional"),
			QStringLiteral("o corte começa com uma pergunta interessante antes de qualquer saudação"),
		},
		{
			QStringLiteral("the answer reaches a natural conclusion with no new topic afterwards"),
			QStringLiteral("the speaker resolves the point smoothly before the clip ends"),
			QStringLiteral("a complete thought with a satisfying ending and final takeaway"),
			QStringLiteral("a curiosity or story arc reaches its local payoff and stops cleanly"),
			QStringLiteral("a resposta termina com conclusão natural e sem puxar outro assunto"),
			QStringLiteral("o raciocínio fecha suavemente sem virar saudação moderação ou chat"),
		},
		{
			QStringLiteral("only greeting viewers without useful content"),
			QStringLiteral("a clip starts with a donation thank you follower goal or chat greeting"),
			QStringLiteral("a clip starts with thanks for chat support, virtual items, gifts, subscriptions, donations or compliments"),
			QStringLiteral("a clip begins with a virtual gift name or gift thank you before the actual topic"),
			QStringLiteral("a transcript excerpt mixes several unrelated chat interactions"),
			QStringLiteral("thanking donations followers subscribers or coins"),
			QStringLiteral("live moderation instructions about spam bans timeouts or rules"),
			QStringLiteral("stream status updates follower goals chat backlog or reading chat"),
			QStringLiteral("procedural prelude where the speaker looks for a chat message before the real hook"),
			QStringLiteral("technical support or connection troubleshooting before the actual clip topic"),
			QStringLiteral("uninteresting setup about checking or reading chat before the answer"),
			QStringLiteral("casual banter with several unrelated viewers"),
			QStringLiteral("a clip that ends one chat topic and starts another unrelated video moment"),
			QStringLiteral("a casual check-in or trivia reply with no actionable takeaway, no emotional payoff, and no conclusion"),
			QStringLiteral("stream setup or background game naming discussion with no advice, empathy, curiosity arc, or conclusion"),
			QStringLiteral("ranting or scolding chat for not greeting or for asking the game name before any useful topic"),
			QStringLiteral("asking what someone is doing, whether they are online, or mentioning body status before the real hook"),
			QStringLiteral("a response with no actionable takeaway, no emotional payoff, and no conclusion"),
			QStringLiteral("apenas cumprimentos agradecimentos ou metas da live"),
			QStringLiteral("agradecimento por apoio do chat, item virtual, doação, inscrição, seguidor ou elogio"),
			QStringLiteral("agradecimento por apoio do chat, doações, inscrições ou itens virtuais"),
			QStringLiteral("instruções de moderação spam castigo banimento ou regras da live"),
			QStringLiteral("conversa casual com várias pessoas sem conselho nem conclusão"),
			QStringLiteral("um corte que termina um assunto e começa outro assunto sem relação"),
		}
	};
	return prototypes;
}

const SemanticPrototypeSet &Curation::Scoring::semanticPrototypesForLanguage(const QString &languageCode)
{
	if (!isPortugueseSemanticLanguage(languageCode))
		return defaultSemanticPrototypes();

	static const SemanticPrototypeSet portuguesePrototypes = {
		{
			QStringLiteral("pergunta de espectador no chat sobre um tema específico"),
			QStringLiteral("mensagem do chat pedindo opinião, conselho ou explicação"),
			QStringLiteral("espectador faz uma pergunta direta para o streamer responder"),
			QStringLiteral("o streamer lê uma pergunta do espectador antes de responder"),
			QStringLiteral("mensagem do viewer com pergunta específica, dilema pessoal ou pedido de opinião"),
			QStringLiteral("mensagem do viewer com problema real e pedido de resposta"),
		},
		{
			QStringLiteral("resposta direta do streamer para a pergunta do espectador"),
			QStringLiteral("o streamer desenvolve uma resposta completa para a mesma mensagem"),
			QStringLiteral("conselho ou opinião com começo, explicação e conclusão"),
			QStringLiteral("resposta útil que permanece no mesmo assunto do chat"),
			QStringLiteral("fala do streamer respondendo o que ele acha do tema perguntado"),
		},
		{
			QStringLiteral("boa noite, salve, e aí, bem-vindo, obrigado por estar na live"),
			QStringLiteral("cumprimento rápido para o chat sem conteúdo principal"),
			QStringLiteral("o streamer dá oi, saúda alguém ou agradece presença"),
		},
		{
			QStringLiteral("agradecimento por apoio do chat, item virtual, presente, inscrição, seguidor ou doação"),
			QStringLiteral("o início é apenas agradecimento por presente ou item virtual antes do assunto útil"),
			QStringLiteral("agradecimento curto por apoio, item virtual, pagamento, inscrição ou participação"),
			QStringLiteral("elogio casual sobre aparência, voz, desempenho ou estilo antes da pergunta principal"),
			QStringLiteral("meta da live, fila, lobby, convite, moderador, spam, banimento ou regra do chat"),
			QStringLiteral("comentário operacional da live que não faz parte da resposta principal"),
			QStringLiteral("conversa casual perguntando o que alguém tem feito, se está online, disponibilidade ou estado físico antes do tema útil"),
			QStringLiteral("resposta curta de curiosidade trivial seguida de troca de assunto sem arco completo"),
			QStringLiteral("setup da live, começo da transmissão, jogo de fundo, overlay ou preparação da live sem resposta útil"),
			QStringLiteral("sermão no chat por não dar boa noite ou por perguntar só o nome do jogo de fundo"),
			QStringLiteral("suporte técnico da live ou do viewer sobre internet, conexão, áudio, vídeo, delay ou configuração antes do conteúdo útil"),
			QStringLiteral("o streamer ajuda alguém a ajustar conexão, internet ou configuração técnica em vez de entrar no assunto principal"),
			QStringLiteral("o streamer procura ou confere uma mensagem do chat antes do assunto útil começar"),
			QStringLiteral("preparação operacional para ler mensagem do chat antes da resposta principal"),
		},
		{
			QStringLiteral("começa outra pergunta de outro espectador depois da conclusão"),
			QStringLiteral("novo comentário do chat muda o assunto da resposta"),
			QStringLiteral("depois do fechamento aparece elogio, pergunta ou observação sem relação"),
			QStringLiteral("o streamer deixa a pergunta original e passa para outro tema"),
			QStringLiteral("novo comentário elogioso, observação ou pergunta de outro viewer depois da conclusão"),
		},
		{
			QStringLiteral("corte forte com problema claro, desenvolvimento e conclusão útil"),
			QStringLiteral("uma única resposta contínua para uma única mensagem do chat"),
			QStringLiteral("opinião emocional, filosófica ou pessoal com raciocínio completo"),
			QStringLiteral("resposta sobre o tema perguntado com desenvolvimento e fechamento natural"),
			QStringLiteral("dilema real do espectador com resposta empática e conclusão"),
			QStringLiteral("trecho que ensina, aconselha ou resolve um ponto sem trocar de assunto"),
			QStringLiteral("momento interessante que gera curiosidade, desenvolve a ideia e fecha localmente"),
			QStringLiteral("história, opinião ou reflexão autossuficiente com começo, meio e fim"),
		},
		{
			QStringLiteral("viewer pede conselho amoroso depois de término, culpa, rejeição ou conflito de relacionamento"),
			QStringLiteral("viewer compartilha que está desenvolvendo traços de depressão e recebe orientação cuidadosa"),
			QStringLiteral("mensagem vulnerável sobre depressão, saúde mental, terapia, clínica ou sofrimento emocional"),
			QStringLiteral("alguém conta que ficou ou morou em clínica e o streamer responde com empatia"),
			QStringLiteral("relato humano que gera empatia antes de um conselho ou reflexão útil"),
			QStringLiteral("resposta empática com conselho responsável para uma pessoa em sofrimento"),
			QStringLiteral("contexto emocional essencial do viewer antes do desenvolvimento do conselho"),
		},
		{
			QStringLiteral("o corte começa direto na pergunta ou frase que apresenta o assunto"),
			QStringLiteral("gancho inicial com pergunta interessante antes de agradecimento ou saudação"),
			QStringLiteral("primeira frase autossuficiente que explica o tema da resposta"),
			QStringLiteral("começo que gera curiosidade mesmo sem ser uma pergunta explícita"),
			QStringLiteral("abertura de história, opinião ou reflexão que prende atenção sem social/meta antes"),
			QStringLiteral("começo no momento em que o viewer pergunta o que ele acha do assunto"),
		},
		{
			QStringLiteral("a resposta chega na primeira conclusão local e para antes de outro comentário"),
			QStringLiteral("o raciocínio fecha sem puxar outra pergunta do chat"),
			QStringLiteral("conclusão natural com takeaway antes de saudação, elogio ou assunto novo"),
			QStringLiteral("termina quando a resposta sobre o mesmo tema já está completa"),
			QStringLiteral("o momento de curiosidade ou história chega ao payoff local e para antes da próxima interação"),
		},
		{
			QStringLiteral("agradecimento por apoio do chat, item virtual, presente, doação, inscrição ou seguidor"),
			QStringLiteral("corte começa com agradecimento por presente ou item virtual antes do gancho real"),
			QStringLiteral("elogio casual sobre aparência, voz, desempenho ou estilo, ou agradecimento pelo elogio"),
			QStringLiteral("cumprimentos, salves, boa noite, obrigado, tamo junto sem conteúdo principal"),
			QStringLiteral("trecho mistura várias mensagens de espectadores sem um único assunto"),
			QStringLiteral("o corte começa antes da pergunta real ou termina depois com outro assunto"),
			QStringLiteral("começa com social da live e só depois entra no tema importante"),
			QStringLiteral("começa com o streamer procurando ou conferindo uma mensagem antes do gancho real"),
			QStringLiteral("começa com preparação para ler o chat e só depois entra na resposta útil"),
			QStringLiteral("começa com suporte técnico sobre internet conexão áudio vídeo delay ou configuração antes do gancho real"),
			QStringLiteral("começa com conversa casual perguntando o que a pessoa fez ou se está online antes do tema principal"),
			QStringLiteral("comentário sobre nariz voz corpo sono horário ou estado físico antes do gancho real"),
			QStringLiteral("resposta muito curta sobre faculdade nome do jogo ou curiosidade trivial seguida de troca de assunto"),
			QStringLiteral("setup da live ou conversa sobre nome do jogo de fundo sem conselho empatia curiosidade real ou conclusão"),
			QStringLiteral("sermão sobre etiqueta do chat, boa noite, salve, ou pessoas perguntando o jogo de fundo"),
			QStringLiteral("termina a resposta e continua para comentário novo do chat"),
			QStringLiteral("fala sobre moderação, spam, castigo, banimento, regras, fila ou lobby"),
		}
	};
	return portuguesePrototypes;
}

QStringList Curation::Scoring::targetPrototypesForPreset(const QString &presetId, const QString &mainTarget)
{
	return targetPrototypesForPreset(presetId, mainTarget, QStringLiteral("auto"));
}

QStringList Curation::Scoring::targetPrototypesForPreset(const QString &presetId, const QString &mainTarget,
	const QString &languageCode)
{
	QStringList prototypes;
	const QString target = mainTarget.trimmed();
	const bool portuguese = isPortugueseSemanticLanguage(languageCode);
	if (!target.isEmpty()) {
		if (portuguese) {
			prototypes << QStringLiteral("um corte autossuficiente especificamente sobre %1").arg(target)
				   << QStringLiteral("o streamer permanece no assunto %1").arg(target)
				   << QStringLiteral("uma resposta completa sobre %1").arg(target)
				   << QStringLiteral("o início, meio e fim tratam apenas de %1").arg(target);
		} else {
			prototypes << QStringLiteral("a self-contained clip specifically about %1").arg(target)
				   << QStringLiteral("the speaker stays on the topic of %1").arg(target)
				   << QStringLiteral("a complete answer about %1").arg(target);
		}
	}

	if (presetId == QStringLiteral("viewer_message_response")) {
		if (portuguese) {
			prototypes << QStringLiteral("uma mensagem de espectador seguida por resposta direta")
				   << QStringLiteral("uma resposta completa para uma única pergunta do chat")
				   << QStringLiteral("um conselho ou opinião útil com começo, desenvolvimento e conclusão")
				   << QStringLiteral("um momento interessante que gera curiosidade e fecha localmente")
				   << QStringLiteral("um conselho amoroso ou emocional com contexto completo, desenvolvimento e conclusão")
				   << QStringLiteral("uma história opinião ou reflexão autossuficiente com começo meio e fim")
				   << QStringLiteral("um corte contínuo sem agradecimento, saudação ou troca de assunto");
		} else {
			prototypes << QStringLiteral("a viewer message followed by the speaker's direct response")
				   << QStringLiteral("one complete answer to a single viewer question")
				   << QStringLiteral("one useful response to one viewer problem with clear payoff")
				   << QStringLiteral("a curiosity-driven standalone story or opinion with a local payoff")
				   << QStringLiteral("uma mensagem de espectador seguida por resposta direta")
				   << QStringLiteral("um conselho útil para uma única pergunta do chat");
		}
	} else if (presetId == QStringLiteral("advice_answer")) {
		prototypes << (portuguese ? QStringLiteral("conselho prático com recomendação clara")
				      : QStringLiteral("practical advice with a clear recommendation"))
			   << (portuguese ? QStringLiteral("resposta útil explicando o que alguém deveria fazer")
				      : QStringLiteral("a useful answer explaining what someone should do"));
	} else if (presetId == QStringLiteral("emotional_reaction")) {
		prototypes << (portuguese ? QStringLiteral("reação emocional forte com fechamento claro")
				      : QStringLiteral("a strong emotional reaction with a clear payoff"))
			   << (portuguese ? QStringLiteral("o streamer reage com surpresa, humor, raiva, tristeza ou empolgação")
				      : QStringLiteral("the speaker reacts with surprise, humor, anger, sadness or excitement"));
	} else if (presetId == QStringLiteral("explanation")) {
		prototypes << (portuguese ? QStringLiteral("explicação clara de uma ideia")
				      : QStringLiteral("a clear explanation of one idea"))
			   << (portuguese ? QStringLiteral("o streamer explica um conceito do começo até a resolução")
				      : QStringLiteral("the speaker explains a concept from setup to resolution"));
	}

	return uniqueTexts(prototypes);
}
