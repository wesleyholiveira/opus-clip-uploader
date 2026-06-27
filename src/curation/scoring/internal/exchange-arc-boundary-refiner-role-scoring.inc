static void applyExplicitDiscourseSignals(ArcChunk &chunk)
{
	const QString text = foldedLowerText(chunk.text);
	if (text.isEmpty())
		return;

	const bool questionMark = text.contains(QLatin1Char('?'));
	const bool viewerSetup = containsAny(text, {
		"pergunta", "comentario", "comentário", "mensagem", "duvida", "dúvida", "viewer", "espectador",
		"alguem perguntou", "alguem comentou", "voce perguntou", "voces perguntaram", "o cara perguntou",
		"chat perguntou", "chat ta perguntando", "chat esta perguntando", "responder essa", "respondendo essa"
	});
	const bool questionShape = questionMark || containsAny(text, {
		"por que", "porque eu", "como eu", "como que", "o que eu", "qual e", "qual é", "sera que", "será que",
		"e se", "se eu", "quando eu", "devo", "deveria", "o que fazer", "o que voce acha", "o que você acha"
	});
	const bool explicitProblem = containsAny(text, {
		"problema", "dificuldade", "medo", "ansiedade", "depress", "terapia", "clinica", "clínica",
		"sofr", "triste", "cansado", "culpa", "vergonha", "relacionamento", "termino", "término",
		"namorada", "namorado", "dependencia emocional", "dependência emocional", "viciado", "viciada",
		"vicio", "vício", "bet", "aposta", "meu pai", "minha mae", "minha mãe",
		"rejeicao", "rejeição", "nao consigo", "não consigo", "preciso de ajuda", "conselho"
	});
	const bool curiosityHook = containsAny(text, {
		"sabe qual", "o ponto e", "o ponto é", "o problema e", "o problema é", "a questao e", "a questão é",
		"tem uma coisa", "vou te falar", "presta atencao", "presta atenção", "isso aqui", "a verdade e", "a verdade é"
	});
	const bool developmentCue = containsAny(text, {
		"porque", "por que", "tipo", "quando", "se voce", "se você", "na pratica", "na prática", "isso acontece",
		"a questao", "a questão", "o problema", "o ponto", "quer dizer", "significa", "voce tem", "você tem",
		"voce precisa", "você precisa", "eu acho", "eu penso", "faz sentido", "depende", "exemplo"
	});
	const bool conclusionCue = containsAny(text, {
		"entao", "então", "por isso", "por isso que", "no fim", "no final", "resumindo", "em resumo",
		"basicamente", "conclusao", "conclusão", "e isso", "é isso", "esse e o ponto", "esse é o ponto",
		"moral da historia", "moral da história", "ta ligado", "tá ligado", "sacou", "fechou", "fica tranquilo"
	});
	const bool metaCue = containsAny(text, {
		"obrigado", "obrigada", "valeu", "salve", "boa noite", "bom dia", "boa tarde", "tmj",
		"inscrito", "sub", "prime", "doacao", "doação", "donate", "bits", "gift", "presente",
		"moderador", "mod ", "ban", "timeout", "spam", "regra da live", "live", "stream", "overlay",
		"jogo", "game", "partida", "lobby", "fila", "convite", "youtube", "twitch", "discord"
	});
	const bool shiftCue = containsAny(text, {
		"agora", "proximo", "próximo", "outra coisa", "mudando", "vamos para", "vamos falar", "deixa eu ver",
		"deixa eu ler", "seguinte", "enquanto isso", "voltando aqui", "novo assunto"
	});

	chunk.explicitOpeningCue = std::max({chunk.explicitOpeningCue,
		cueScore(viewerSetup, 0.76), cueScore(questionShape, 0.66), cueScore(explicitProblem, 0.58),
		cueScore(curiosityHook, 0.62)});
	chunk.explicitDevelopmentCue = std::max({chunk.explicitDevelopmentCue,
		cueScore(developmentCue, 0.58), cueScore(explicitProblem, 0.42)});
	chunk.explicitConclusionCue = std::max(chunk.explicitConclusionCue, cueScore(conclusionCue, 0.70));
	chunk.explicitMetaCue = std::max(chunk.explicitMetaCue, cueScore(metaCue, 0.72));
	chunk.explicitShiftCue = std::max(chunk.explicitShiftCue, cueScore(shiftCue, 0.68));

	if (chunk.explicitMetaCue > 0.0)
		chunk.meta = std::max(chunk.meta, chunk.explicitMetaCue);
	if (chunk.explicitShiftCue > 0.0)
		chunk.shift = std::max(chunk.shift, chunk.explicitShiftCue);
}

static double maxPrototypeSimilarityForEmbedding(const SemanticEmbeddingProvider &provider,
	const SemanticEmbedding &embedding, const QStringList &prototypes)
{
	if (!embedding.isValid() || prototypes.isEmpty())
		return 0.0;

	double best = 0.0;
	for (const QString &prototype : prototypes) {
		const QString text = prototype.simplified();
		if (text.isEmpty())
			continue;
		best = std::max(best, cosineSimilarity(embedding, provider.embed(text)));
	}
	return boundedScore(best);
}


static QString clippedRoleText(const QString &text, int maxChars = 360)
{
	QString value = text.simplified();
	if (value.size() <= maxChars)
		return value;
	return value.left(maxChars).trimmed();
}

static QString contextualRoleWindowText(const QVector<ArcChunk> &chunks, int index,
	const ExchangeArcBoundaryRefinementOptions &options)
{
	const int first = std::max(0, index - 2);
	const int last = std::min(static_cast<int>(chunks.size()) - 1, index + 3);
	QStringList window;
	for (int i = first; i <= last; ++i) {
		const QString label = i == index ? QStringLiteral("CURRENT") :
			(i < index ? QStringLiteral("PREVIOUS") : QStringLiteral("NEXT"));
		window.append(QStringLiteral("%1[%2 %3-%4]: %5")
			.arg(label)
			.arg(i - index)
			.arg(QString::number(chunks.at(i).startSec, 'f', 1))
			.arg(QString::number(chunks.at(i).endSec, 'f', 1))
			.arg(clippedRoleText(chunks.at(i).text, i == index ? 520 : 260)));
	}

	QString target = options.scoring.mainTarget.simplified();
	if (target.size() > 320)
		target = target.left(320).trimmed();
	return QStringLiteral(
		"Boundary role classification context. Classify the discourse role of CURRENT using PREVIOUS and NEXT blocks, "
		"not isolated keywords. A valid clip is one continuous viewer response with opening/context, development, and conclusion. "
		"Target/topic: %1. %2")
		.arg(target.isEmpty() ? QStringLiteral("none") : target,
		     window.join(QStringLiteral(" | ")));
}

static QStringList contextualOpeningRolePrototypes(bool portuguese)
{
	if (portuguese) {
		return {
			QStringLiteral("CURRENT é o começo real de um novo corte autossuficiente: apresenta a pergunta, dilema, problema ou contexto do viewer e não depende do bloco anterior."),
			QStringLiteral("CURRENT funciona como gancho inicial: cria curiosidade ou tensão emocional e inicia a resposta a uma única mensagem do chat."),
			QStringLiteral("CURRENT contém contexto inicial suficiente para um espectador entender o assunto sem ouvir os blocos anteriores."),
			QStringLiteral("CURRENT abre um assunto humano de conselho, relacionamento, terapia, saúde mental, empatia ou resposta útil antes da explicação."),
		};
	}
	return {
		QStringLiteral("CURRENT is the real start of a self-contained clip: it introduces the viewer question, dilemma, problem, or context and does not depend on the previous block."),
		QStringLiteral("CURRENT works as the opening hook: it creates curiosity or emotional tension and starts the response to one chat message."),
		QStringLiteral("CURRENT contains enough initial context for an outside viewer to understand the subject without the previous blocks."),
		QStringLiteral("CURRENT opens a human advice, relationship, therapy, mental health, empathy, or useful answer topic before the explanation."),
	};
}

static QStringList contextualDevelopmentRolePrototypes(bool portuguese)
{
	if (portuguese) {
		return {
			QStringLiteral("CURRENT desenvolve o mesmo assunto iniciado antes: explica o raciocínio, dá exemplo, aprofunda o conselho ou constrói a resposta."),
			QStringLiteral("CURRENT é meio do corte: continua a mesma resposta de viewer sem iniciar novo assunto e sem fechar definitivamente."),
			QStringLiteral("CURRENT contém desenvolvimento útil, motivo, consequência, nuance ou orientação prática ligada ao mesmo problema."),
		};
	}
	return {
		QStringLiteral("CURRENT develops the same topic that started before: it explains reasoning, gives an example, deepens the advice, or builds the answer."),
		QStringLiteral("CURRENT is the middle of the clip: it continues the same viewer answer without starting a new topic or fully ending it."),
		QStringLiteral("CURRENT contains useful development, reason, consequence, nuance, or practical guidance tied to the same problem."),
	};
}

static QStringList contextualConclusionRolePrototypes(bool portuguese)
{
	if (portuguese) {
		return {
			QStringLiteral("CURRENT é a conclusão natural da mesma resposta: fecha o raciocínio com resolução, takeaway ou frase final satisfatória."),
			QStringLiteral("CURRENT pode ser o fim limpo do corte porque resolve o ponto e o próximo bloco não é necessário para entender a resposta."),
			QStringLiteral("CURRENT encerra o conselho ou resposta do viewer antes de outro assunto, meta-chat, agradecimento ou nova pergunta começar."),
		};
	}
	return {
		QStringLiteral("CURRENT is the natural conclusion of the same answer: it closes the reasoning with a resolution, takeaway, or satisfying final line."),
		QStringLiteral("CURRENT can be the clean ending of the clip because it resolves the point and the next block is not needed to understand the answer."),
		QStringLiteral("CURRENT ends the advice or viewer response before another topic, meta chat, thank you, or new question begins."),
	};
}

static QStringList contextualPreviousTopicRolePrototypes(bool portuguese)
{
	if (portuguese) {
		return {
			QStringLiteral("CURRENT é sobra do assunto anterior: fecha uma ideia já iniciada antes e não funciona como começo de um novo corte."),
			QStringLiteral("CURRENT depende dos blocos anteriores para fazer sentido e parece continuação ou conclusão de algo que veio antes."),
			QStringLiteral("CURRENT é uma resolução antiga antes do verdadeiro início da próxima resposta."),
		};
	}
	return {
		QStringLiteral("CURRENT is leftover from the previous topic: it closes an idea that started earlier and is not a valid start of a new clip."),
		QStringLiteral("CURRENT depends on previous blocks to make sense and looks like continuation or conclusion of something before it."),
		QStringLiteral("CURRENT is an old resolution before the real start of the next answer."),
	};
}

static QStringList contextualNewTopicRolePrototypes(bool portuguese)
{
	if (portuguese) {
		return {
			QStringLiteral("CURRENT inicia outro assunto, outra pergunta do chat ou outra interação não relacionada; o corte anterior deveria terminar antes dele."),
			QStringLiteral("CURRENT muda de tópico, começa novo viewer ou quebra a continuidade da resposta anterior."),
			QStringLiteral("CURRENT é transição para outro tema, outra mensagem, outro jogo, outro comentário ou outro momento da live."),
		};
	}
	return {
		QStringLiteral("CURRENT starts another topic, another chat question, or an unrelated interaction; the previous clip should end before it."),
		QStringLiteral("CURRENT changes topic, starts a new viewer, or breaks continuity from the previous answer."),
		QStringLiteral("CURRENT is a transition to another theme, message, game, comment, or stream moment."),
	};
}

static QStringList contextualMetaRolePrototypes(bool portuguese)
{
	if (portuguese) {
		return {
			QStringLiteral("CURRENT é meta-chat da live: agradecimento, moderação, jogo, lobby, inscrição, doação, spam, regra, cumprimento ou logística."),
			QStringLiteral("CURRENT é conversa operacional ou casual de stream sem conselho útil, sem arco emocional e sem resposta completa."),
			QStringLiteral("CURRENT fala sobre jogo, moderador, ban, convite, live, Twitch, YouTube, doação, sub, obrigado ou salve."),
		};
	}
	return {
		QStringLiteral("CURRENT is stream meta-chat: thank you, moderation, game, lobby, subscription, donation, spam, rule, greeting, or logistics."),
		QStringLiteral("CURRENT is operational or casual stream talk without useful advice, emotional arc, or complete response."),
		QStringLiteral("CURRENT talks about game, moderator, ban, invite, live, Twitch, YouTube, donation, sub, thanks, or greeting."),
	};
}

static void scoreContextualBoundaryRoles(QVector<ArcChunk> &chunks, const ExchangeArcBoundaryRefinementOptions &options)
{
	if (!options.embeddingProvider || !options.embeddingProvider->isAvailable() || chunks.isEmpty())
		return;

	const QString languageCode = normalizedSemanticLanguageCode(options.scoring.transcriptionLanguage,
		options.scoring.sourceLanguage);
	const bool portuguese = isPortugueseSemanticLanguage(languageCode);
	const QStringList openingPrototypes = contextualOpeningRolePrototypes(portuguese);
	const QStringList developmentPrototypes = contextualDevelopmentRolePrototypes(portuguese);
	const QStringList conclusionPrototypes = contextualConclusionRolePrototypes(portuguese);
	const QStringList previousTopicPrototypes = contextualPreviousTopicRolePrototypes(portuguese);
	const QStringList newTopicPrototypes = contextualNewTopicRolePrototypes(portuguese);
	const QStringList metaPrototypes = contextualMetaRolePrototypes(portuguese);

	for (int i = 0; i < static_cast<int>(chunks.size()); ++i) {
		ArcChunk &chunk = chunks[i];
		const SemanticEmbedding contextEmbedding = options.embeddingProvider->embed(contextualRoleWindowText(chunks, i, options));
		if (!contextEmbedding.isValid())
			continue;

		chunk.contextOpeningScore = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, contextEmbedding, openingPrototypes);
		chunk.contextDevelopmentScore = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, contextEmbedding, developmentPrototypes);
		chunk.contextConclusionScore = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, contextEmbedding, conclusionPrototypes);
		chunk.contextPreviousTopicScore = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, contextEmbedding, previousTopicPrototypes);
		chunk.contextNewTopicScore = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, contextEmbedding, newTopicPrototypes);
		chunk.contextMetaScore = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, contextEmbedding, metaPrototypes);
		if (i > 0)
			chunk.contextCoherenceScore = std::max(chunk.contextCoherenceScore,
				cosineSimilarity(chunks.at(i - 1).embedding, chunk.embedding));
		if (i + 1 < static_cast<int>(chunks.size()))
			chunk.contextCoherenceScore = std::max(chunk.contextCoherenceScore,
				cosineSimilarity(chunk.embedding, chunks.at(i + 1).embedding));

		const double contextPositive = std::max({chunk.contextOpeningScore, chunk.contextDevelopmentScore,
			chunk.contextConclusionScore});
		const double contextDefect = std::max(chunk.contextMetaScore, chunk.contextNewTopicScore);
		chunk.openingScore = boundedScore(std::max(chunk.openingScore,
			(chunk.contextOpeningScore * 0.58) + (chunk.hook * 0.20) + (chunk.target * 0.12) +
			(chunk.explicitOpeningCue * 0.16) - (contextDefect * 0.20)));
		chunk.developmentScore = boundedScore(std::max(chunk.developmentScore,
			(chunk.contextDevelopmentScore * 0.60) + (chunk.value * 0.20) + (chunk.target * 0.08) +
			(chunk.explicitDevelopmentCue * 0.14) - (contextDefect * 0.16)));
		chunk.conclusionScore = boundedScore(std::max(chunk.conclusionScore,
			(chunk.contextConclusionScore * 0.62) + (chunk.resolution * 0.20) +
			(chunk.explicitConclusionCue * 0.18) - (contextDefect * 0.14)));
		chunk.defectScore = boundedScore(std::max(chunk.defectScore,
			(contextDefect * 0.62) + (chunk.explicitMetaCue * 0.24) + (chunk.explicitShiftCue * 0.22) -
			(contextPositive * 0.18)));
		chunk.meta = std::max(chunk.meta, boundedScore((chunk.contextMetaScore * 0.72) + (chunk.explicitMetaCue * 0.20)));
		chunk.shift = std::max(chunk.shift, boundedScore((chunk.contextNewTopicScore * 0.72) + (chunk.explicitShiftCue * 0.20)));
	}
}

static void assignContextualBoundaryRoles(QVector<ArcChunk> &chunks, const ExchangeArcBoundaryRefinementOptions &options)
{
	if (options.scoring.presetId != QStringLiteral("viewer_message_response") || chunks.isEmpty())
		return;

	for (int i = 0; i < static_cast<int>(chunks.size()); ++i) {
		ArcChunk &chunk = chunks[i];
		const double positive = std::max({chunk.contextOpeningScore, chunk.contextDevelopmentScore,
			chunk.contextConclusionScore, chunk.openingScore, chunk.developmentScore, chunk.conclusionScore});
		const double hardDefect = std::max({chunk.contextMetaScore, chunk.contextNewTopicScore,
			chunk.explicitMetaCue, chunk.explicitShiftCue, chunk.meta, chunk.shift});
		const double previousScore = std::max(chunk.contextPreviousTopicScore,
			(i > 0 ? boundedScore((chunks.at(i - 1).contextConclusionScore * 0.42) +
				(chunk.contextDevelopmentScore * 0.20)) : 0.0));

		if (hardDefect >= positive + 0.045 && hardDefect >= 0.54) {
			chunk.role = chunk.contextNewTopicScore >= chunk.contextMetaScore + 0.03 || chunk.explicitShiftCue >= 0.64
				? ArcChunkRole::TailOrNewTurn
				: ArcChunkRole::SocialOrMetaPrelude;
			chunk.roleReason = QStringLiteral("context_defect");
			continue;
		}

		if (looksLikeTargetedViewerMessageCue(chunk) && hardDefect <= positive + 0.16) {
			chunk.role = ArcChunkRole::OpeningCandidate;
			chunk.roleReason = QStringLiteral("targeted_viewer_message_cue");
			continue;
		}

		if (previousScore >= std::max({chunk.contextOpeningScore, chunk.openingScore}) + 0.040 &&
		    previousScore >= 0.54 && i + 1 < static_cast<int>(chunks.size()) &&
		    std::max(chunks.at(i + 1).contextOpeningScore, chunks.at(i + 1).contextDevelopmentScore) >= previousScore - 0.05) {
			chunk.role = ArcChunkRole::PreviousConclusion;
			chunk.roleReason = QStringLiteral("context_previous_topic");
			continue;
		}

		const double opening = std::max(chunk.contextOpeningScore, chunk.openingScore);
		const double development = std::max(chunk.contextDevelopmentScore, chunk.developmentScore);
		const double conclusion = std::max(chunk.contextConclusionScore, chunk.conclusionScore);
		const double roleMax = std::max({opening, development, conclusion});
		const double roleMin = std::min({opening, development, conclusion});
		const bool viewerPresetTie = options.scoring.presetId == QStringLiteral("viewer_message_response") &&
			roleMax - roleMin <= 0.085 && chunk.explicitConclusionCue < 0.48 &&
			hardDefect <= roleMax + 0.05;
		if (viewerPresetTie) {
			// Multilingual embedding role prompts frequently produce near-identical
			// opening/development/conclusion scores for the same answer. When the
			// scores are tied, do not default to conclusion: that creates the
			// all-resolution plateau seen in dale.mp4. Use local discourse structure
			// and pauses as a tie-breaker instead.
			const bool likelyStart = i == 0 || chunk.pauseBeforeSec >= 1.20 ||
				chunk.explicitOpeningCue >= 0.38 ||
				(i > 0 && chunk.pauseBeforeSec >= 2.40);
			const bool likelyEnd = i + 1 >= static_cast<int>(chunks.size()) ||
				chunk.pauseAfterSec >= 1.20 || chunk.resolution >= 0.62;
			if (likelyStart && chunk.explicitMetaCue < 0.62 && chunk.explicitShiftCue < 0.62) {
				chunk.role = ArcChunkRole::OpeningCandidate;
				chunk.roleReason = QStringLiteral("context_tie_opening");
				continue;
			}
			if (!likelyEnd || development >= conclusion - 0.055 || chunk.explicitDevelopmentCue >= 0.34) {
				chunk.role = ArcChunkRole::Development;
				chunk.roleReason = QStringLiteral("context_tie_development");
				continue;
			}
		}

		if (opening >= development - 0.020 && opening >= conclusion - 0.010 && opening >= hardDefect - 0.02 &&
		    (chunk.explicitOpeningCue >= 0.42 || chunk.contextOpeningScore >= 0.58 || chunk.hook >= 0.64)) {
			chunk.role = ArcChunkRole::OpeningCandidate;
			chunk.roleReason = QStringLiteral("context_opening");
			continue;
		}
		if (development >= hardDefect - 0.04 &&
		    (development >= 0.42 || chunk.value >= 0.54 || chunk.contextDevelopmentScore >= 0.52) &&
		    (chunk.explicitConclusionCue < 0.48 || development >= conclusion - 0.045)) {
			chunk.role = ArcChunkRole::Development;
			chunk.roleReason = QStringLiteral("context_development");
			continue;
		}
		if (conclusion >= development - 0.010 && conclusion >= opening - 0.025 && conclusion >= hardDefect - 0.02 &&
		    (chunk.explicitConclusionCue >= 0.48 || chunk.contextConclusionScore >= 0.58 || chunk.resolution >= 0.58)) {
			chunk.role = ArcChunkRole::LocalResolution;
			chunk.roleReason = QStringLiteral("context_conclusion");
			continue;
		}
	}
}

static QVector<ArcChunk> chunksForCandidate(const TranscriptIndex &index, const ClipCandidate &candidate,
	const ExchangeArcBoundaryRefinementOptions &options)
{
	QVector<ArcChunk> chunks;
	if (!options.embeddingProvider || !options.embeddingProvider->isAvailable())
		return chunks;

	const QVector<int> indices = index.segmentIndicesForRange(candidate.range);
	if (indices.isEmpty())
		return chunks;

	ArcChunk current;
	double previousEndSec = -1.0;
	auto flushCurrent = [&]() {
		const QString text = current.text.simplified();
		const int minChunkChars = options.scoring.presetId == QStringLiteral("viewer_message_response") ? 8 : 20;
		if (current.firstIndex >= 0 && current.lastIndex >= current.firstIndex && text.size() >= minChunkChars) {
			current.text = text;
			current.pauseBeforeSec = index.silenceBeforeSegment(current.firstIndex);
			current.pauseAfterSec = index.silenceAfterSegment(current.lastIndex);
			current.embedding = options.embeddingProvider->embed(current.text);
			if (current.embedding.isValid())
				chunks.append(current);
		}
		current = {};
	};

	for (const int segmentIndex : indices) {
		const TranscriptSegment *segment = index.segmentAt(segmentIndex);
		if (!segment || segment->text.trimmed().isEmpty())
			continue;

		const QString text = segment->text.trimmed();
		const bool startsNewChunk = current.firstIndex < 0;
		const double gapSec = previousEndSec >= 0.0 ? std::max(0.0, segment->startSec - previousEndSec) : 0.0;
		const double nextDurationSec = startsNewChunk ? std::max(0.0, segment->endSec - segment->startSec)
						 : std::max(0.0, segment->endSec - current.startSec);
		const int nextChars = current.text.size() + text.size() + 1;
		const bool viewerPreset = options.scoring.presetId == QStringLiteral("viewer_message_response");
		const double maxChunkDurationSec = viewerPreset ? 2.6 : 3.75;
		const int maxChunkChars = viewerPreset ? 118 : 190;
		const bool startsNewViewerCueSegment = viewerPreset && !startsNewChunk &&
			looksLikeViewerMessageCueText(foldedLowerText(text));
		const bool shouldFlush = !startsNewChunk &&
			(startsNewViewerCueSegment || gapSec > 0.95 || nextDurationSec > maxChunkDurationSec ||
			 nextChars > maxChunkChars);
		if (shouldFlush)
			flushCurrent();

		if (current.firstIndex < 0) {
			current.firstIndex = segmentIndex;
			current.startSec = segment->startSec;
		}
		current.lastIndex = segmentIndex;
		current.endSec = segment->endSec;
		if (!current.text.isEmpty())
			current.text.append(QLatin1Char(' '));
		current.text.append(text);
		previousEndSec = segment->endSec;
	}
	flushCurrent();
	return chunks;
}

static void classifyChunk(ArcChunk &chunk)
{
	applyExplicitDiscourseSignals(chunk);

	const double positive = std::max({chunk.target, chunk.value, chunk.hook, chunk.resolution,
		chunk.explicitOpeningCue, chunk.explicitDevelopmentCue, chunk.explicitConclusionCue});
	const double defect = std::max({chunk.meta, chunk.shift, chunk.explicitMetaCue, chunk.explicitShiftCue});
	chunk.openingScore = boundedScore((chunk.hook * 0.36) + (chunk.target * 0.24) + (chunk.value * 0.14) +
		(chunk.explicitOpeningCue * 0.34) - (chunk.meta * 0.30) - (chunk.shift * 0.16));
	chunk.developmentScore = boundedScore((chunk.value * 0.38) + (chunk.target * 0.16) +
		(chunk.resolution * 0.12) + (chunk.hook * 0.08) + (chunk.explicitDevelopmentCue * 0.28) -
		(chunk.meta * 0.24) - (chunk.shift * 0.14));
	const double strongestEndingDefect = std::max({chunk.meta, chunk.shift, chunk.explicitMetaCue, chunk.explicitShiftCue});
	const double strictConclusion = boundedScore((chunk.resolution * 0.48) + (chunk.value * 0.18) +
		(chunk.target * 0.06) + (chunk.explicitConclusionCue * 0.34) - (chunk.meta * 0.28) -
		(chunk.shift * 0.26));
	const double semanticConclusionFallback = boundedScore((chunk.resolution * 0.56) + (chunk.value * 0.14) +
		(chunk.target * 0.08) + (chunk.explicitConclusionCue * 0.24) - (strongestEndingDefect * 0.20));
	chunk.conclusionScore = std::max(strictConclusion, semanticConclusionFallback);
	chunk.defectScore = boundedScore((defect * 0.76) + (chunk.shift * 0.18) - (positive * 0.22));

	const bool metaCompetesWithContent = defect >= positive - 0.05;
	const bool explicitMetaDominates = chunk.explicitMetaCue >= 0.70 && chunk.explicitOpeningCue < 0.50 &&
		chunk.explicitDevelopmentCue < 0.48 && chunk.explicitConclusionCue < 0.55;
	const bool weakSelfContainedOpening = chunk.openingScore < 0.48 && chunk.target < 0.58 &&
		chunk.resolution < 0.55 && chunk.explicitOpeningCue < 0.50;
	if (explicitMetaDominates || (chunk.defectScore >= 0.54 && metaCompetesWithContent) ||
	    (weakSelfContainedOpening && chunk.meta >= 0.54 && chunk.meta >= chunk.hook - 0.03)) {
		chunk.role = chunk.shift >= chunk.meta + 0.03 || chunk.explicitShiftCue >= 0.66
			? ArcChunkRole::TailOrNewTurn
			: ArcChunkRole::SocialOrMetaPrelude;
		return;
	}

	const bool explicitOpening = chunk.explicitOpeningCue >= 0.50;
	const bool strongSemanticOpening = chunk.openingScore >= 0.48 && chunk.hook >= 0.62 &&
		chunk.openingScore >= chunk.conclusionScore - 0.04 && chunk.meta < chunk.openingScore + 0.08;
	if (explicitOpening || strongSemanticOpening) {
		chunk.role = ArcChunkRole::OpeningCandidate;
		return;
	}

	const bool explicitConclusion = chunk.explicitConclusionCue >= 0.58;
	const bool strongConclusion = (explicitConclusion || chunk.resolution >= 0.58) &&
		chunk.conclusionScore >= std::max(chunk.openingScore, chunk.developmentScore) - 0.01;
	if (strongConclusion) {
		chunk.role = ArcChunkRole::LocalResolution;
		return;
	}

	const bool explicitDevelopment = chunk.explicitDevelopmentCue >= 0.48;
	const bool strongDevelopment = chunk.developmentScore >= 0.40 &&
		(explicitDevelopment || chunk.value >= 0.52 || positive >= 0.60 ||
		 chunk.developmentScore >= chunk.openingScore - 0.03) &&
		chunk.developmentScore >= chunk.conclusionScore - 0.06;
	if (strongDevelopment) {
		chunk.role = ArcChunkRole::Development;
		return;
	}

	if (chunk.openingScore >= chunk.developmentScore + 0.04 && chunk.openingScore >= 0.38 &&
	    chunk.explicitOpeningCue >= 0.38) {
		chunk.role = ArcChunkRole::OpeningCandidate;
		return;
	}
	if (chunk.developmentScore >= 0.36 || (positive >= 0.60 && chunk.meta < positive + 0.08)) {
		chunk.role = ArcChunkRole::Development;
		return;
	}
	chunk.role = ArcChunkRole::Unknown;
}

static void scoreChunks(QVector<ArcChunk> &chunks, const ExchangeArcBoundaryRefinementOptions &options)
{
	if (!options.embeddingProvider || !options.embeddingProvider->isAvailable())
		return;

	const QString languageCode = normalizedSemanticLanguageCode(options.scoring.transcriptionLanguage,
		options.scoring.sourceLanguage);
	const SemanticPrototypeSet &defaults = semanticPrototypesForLanguage(languageCode);
	const QStringList targetPrototypes = targetPrototypesForPreset(options.scoring.presetId,
		options.scoring.mainTarget, languageCode);
	const QStringList valuePrototypes = uniqueTexts(QStringList(defaults.clipValue) + defaults.empathy + targetPrototypes);
	const QStringList hookPrototypes = uniqueTexts(QStringList(defaults.hook) + defaults.viewerMessage + targetPrototypes);
	const QStringList resolutionPrototypes = uniqueTexts(QStringList(defaults.resolution) + defaults.directAnswer + targetPrototypes);
	const QStringList metaPrototypes = uniqueTexts(QStringList(defaults.metaNoise) + defaults.greetingNoise + defaults.streamManagement);
	const QStringList shiftPrototypes = uniqueTexts(QStringList(defaults.topicShift) + defaults.streamManagement);

	for (ArcChunk &chunk : chunks) {
		chunk.target = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, chunk.embedding, targetPrototypes);
		chunk.value = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, chunk.embedding, valuePrototypes);
		chunk.hook = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, chunk.embedding, hookPrototypes);
		chunk.resolution = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, chunk.embedding, resolutionPrototypes);
		chunk.meta = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, chunk.embedding, metaPrototypes);
		chunk.shift = maxPrototypeSimilarityForEmbedding(*options.embeddingProvider, chunk.embedding, shiftPrototypes);
		classifyChunk(chunk);
	}
	scoreContextualBoundaryRoles(chunks, options);
	assignContextualBoundaryRoles(chunks, options);
}

