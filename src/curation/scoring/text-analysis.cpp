#include "curation/scoring/text-analysis.hpp"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace {

QSet<QString> baseStopWords()
{
	return {
		QStringLiteral("a"), QStringLiteral("an"), QStringLiteral("the"), QStringLiteral("to"),
		QStringLiteral("for"), QStringLiteral("of"), QStringLiteral("with"), QStringLiteral("and"),
		QStringLiteral("or"), QStringLiteral("about"), QStringLiteral("how"), QStringLiteral("your"),
		QStringLiteral("their"), QStringLiteral("his"), QStringLiteral("her"), QStringLiteral("one"),
		QStringLiteral("single"), QStringLiteral("viewer"), QStringLiteral("message"),
		QStringLiteral("response"), QStringLiteral("chat"), QStringLiteral("live"),
		QStringLiteral("uma"), QStringLiteral("um"), QStringLiteral("de"), QStringLiteral("da"),
		QStringLiteral("do"), QStringLiteral("dos"), QStringLiteral("das"), QStringLiteral("para"),
		QStringLiteral("com"), QStringLiteral("como"), QStringLiteral("sobre"), QStringLiteral("por"),
		QStringLiteral("que"), QStringLiteral("voce"), QStringLiteral("voces"), QStringLiteral("vc"),
		QStringLiteral("ce"), QStringLiteral("acho"), QStringLiteral("assim"), QStringLiteral("aqui"),
		QStringLiteral("aquela"), QStringLiteral("aquele"), QStringLiteral("cara"), QStringLiteral("mano"),
		QStringLiteral("tipo"), QStringLiteral("porque"), QStringLiteral("entao"), QStringLiteral("tambem"),
		QStringLiteral("quando"), QStringLiteral("onde"), QStringLiteral("isso"), QStringLiteral("esse"),
		QStringLiteral("essa"), QStringLiteral("esses"), QStringLiteral("essas"), QStringLiteral("pela"),
		QStringLiteral("pelo"), QStringLiteral("tava"), QStringLiteral("estava"), QStringLiteral("agora"),
		QStringLiteral("muito"), QStringLiteral("mesmo"), QStringLiteral("meio"), QStringLiteral("ne"),
		QStringLiteral("ta"), QStringLiteral("pra"), QStringLiteral("pro"), QStringLiteral("num"),
		QStringLiteral("numa"), QStringLiteral("meu"), QStringLiteral("minha"), QStringLiteral("teu"),
		QStringLiteral("sua"), QStringLiteral("seu")
	};
}

bool isNoiseCategory(const QString &category)
{
	return category == QStringLiteral("stream_operation") || category == QStringLiteral("social_meta") ||
	       category == QStringLiteral("greeting") || category == QStringLiteral("topic_transition");
}

bool hasAnyTokenPrefix(const QStringList &words, const QStringList &prefixes);
bool hasAnyPhrase(const QString &normalizedText, const QStringList &phrases);


bool hasAnyTokenPrefix(const QStringList &words, const QStringList &prefixes)
{
	for (const QString &word : words) {
		for (const QString &prefix : prefixes) {
			if (!prefix.isEmpty() && word.startsWith(prefix))
				return true;
		}
	}
	return false;
}


bool hasAnyPhrase(const QString &normalizedText, const QStringList &phrases)
{
	for (const QString &phrase : phrases) {
		if (!phrase.isEmpty() && normalizedText.contains(phrase))
			return true;
	}
	return false;
}

bool startsWithAnyPhrase(const QString &normalizedText, const QStringList &phrases)
{
	for (const QString &phrase : phrases) {
		if (!phrase.isEmpty() && normalizedText.startsWith(phrase))
			return true;
	}
	return false;
}

QStringList greetingPhrases()
{
	return {QStringLiteral("bom dia"), QStringLiteral("boa tarde"), QStringLiteral("boa noite"),
		QStringLiteral("seja bem vindo"), QStringLiteral("seja bem vinda"), QStringLiteral("bem vindo"),
		QStringLiteral("bem vinda"), QStringLiteral("e ai"), QStringLiteral("oi"), QStringLiteral("ola"),
		QStringLiteral("salve"), QStringLiteral("hello"), QStringLiteral("hi"), QStringLiteral("hey"),
		QStringLiteral("welcome")};
}

QStringList gratitudePrefixes()
{
	return {QStringLiteral("obrig"), QStringLiteral("agradec"), QStringLiteral("valeu"),
		QStringLiteral("vlw"), QStringLiteral("thanks"), QStringLiteral("thank"), QStringLiteral("tks")};
}

QStringList supportOrTransactionPrefixes()
{
	return {QStringLiteral("presente"), QStringLiteral("gift"), QStringLiteral("doa"), QStringLiteral("donat"),
		QStringLiteral("sub"), QStringLiteral("inscri"), QStringLiteral("segu"), QStringLiteral("follow"),
		QStringLiteral("moeda"), QStringLiteral("coin"), QStringLiteral("pix"), QStringLiteral("like"),
		QStringLiteral("estrela"), QStringLiteral("ponto"), QStringLiteral("membro"), QStringLiteral("member")};
}

QStringList streamOperationPrefixes()
{
	return {QStringLiteral("convite"), QStringLiteral("fila"), QStringLiteral("lobby"), QStringLiteral("modera"),
		QStringLiteral("spam"), QStringLiteral("ban"), QStringLiteral("timeout"), QStringLiteral("regra"),
		QStringLiteral("live"), QStringLiteral("stream"), QStringLiteral("chat"), QStringLiteral("audio"),
		QStringLiteral("microfone"), QStringLiteral("camera"), QStringLiteral("tela"), QStringLiteral("config"),
		QStringLiteral("link"), QStringLiteral("canal"), QStringLiteral("meta")};
}

QStringList complimentPrefixes()
{
	return {QStringLiteral("elog"), QStringLiteral("lind"), QStringLiteral("bonit"), QStringLiteral("perfeit"),
		QStringLiteral("incrivel"), QStringLiteral("incrivel"), QStringLiteral("maravilh"),
		QStringLiteral("nice"), QStringLiteral("beautiful"), QStringLiteral("great"), QStringLiteral("amazing")};
}

QStringList topicTransitionPhrases()
{
	return {QStringLiteral("mudando de assunto"), QStringLiteral("mudar de assunto"),
		QStringLiteral("proxima pergunta"), QStringLiteral("próxima pergunta"), QStringLiteral("outra pergunta"),
		QStringLiteral("outro assunto"), QStringLiteral("outra coisa"), QStringLiteral("agora vamos"),
		QStringLiteral("anyway"), QStringLiteral("by the way"), QStringLiteral("next question"),
		QStringLiteral("another question")};
}

QStringList questionStartPhrases()
{
	return {QStringLiteral("como"), QStringLiteral("por que"), QStringLiteral("porque"),
		QStringLiteral("o que"), QStringLiteral("qual"), QStringLiteral("quando"), QStringLiteral("onde"),
		QStringLiteral("quem"), QStringLiteral("sera que"), QStringLiteral("será que"), QStringLiteral("devo"),
		QStringLiteral("posso"), QStringLiteral("what"), QStringLiteral("how"), QStringLiteral("why"),
		QStringLiteral("when"), QStringLiteral("where"), QStringLiteral("who"), QStringLiteral("should"),
		QStringLiteral("can i"), QStringLiteral("do i")};
}

QStringList viewerCuePrefixes()
{
	return {QStringLiteral("pergunt"), QStringLiteral("coment"), QStringLiteral("fal"), QStringLiteral("diss"),
		QStringLiteral("mand"), QStringLiteral("escrev"), QStringLiteral("viewer"), QStringLiteral("spectator"),
		QStringLiteral("espectador"), QStringLiteral("chat"), QStringLiteral("asked"), QStringLiteral("comment")};
}

QStringList personalIssuePhrases()
{
	return {QStringLiteral("nao sei se"), QStringLiteral("não sei se"), QStringLiteral("nao sei como"),
		QStringLiteral("não sei como"), QStringLiteral("nao sei o que"), QStringLiteral("não sei o que"),
		QStringLiteral("me sinto"), QStringLiteral("eu sinto"), QStringLiteral("sinto que"),
		QStringLiteral("tenho medo"), QStringLiteral("preciso de ajuda"), QStringLiteral("o que faco"),
		QStringLiteral("o que faço"), QStringLiteral("como faco"), QStringLiteral("como faço"),
		QStringLiteral("i feel"), QStringLiteral("i don't know"), QStringLiteral("what should i"),
		QStringLiteral("how can i")};
}

QStringList adviceOrResolutionPrefixes()
{
	return {QStringLiteral("recomend"), QStringLiteral("aconselh"), QStringLiteral("dever"),
		QStringLiteral("faria"), QStringLiteral("precisa"), QStringLiteral("entend"), QStringLiteral("conclu"),
		QStringLiteral("resum"), QStringLiteral("portanto"), QStringLiteral("entao"), QStringLiteral("so que"),
		QStringLiteral("mas"), QStringLiteral("because"), QStringLiteral("therefore"), QStringLiteral("recommend"),
		QStringLiteral("advice"), QStringLiteral("should")};
}

bool hasGreeting(const QString &normalizedText)
{
	if (startsWithAnyPhrase(normalizedText,
			{QStringLiteral("bom dia"), QStringLiteral("boa tarde"), QStringLiteral("boa noite"),
			 QStringLiteral("seja bem vindo"), QStringLiteral("seja bem vinda"), QStringLiteral("bem vindo"),
			 QStringLiteral("bem vinda"), QStringLiteral("e ai"), QStringLiteral("hello"), QStringLiteral("welcome")}))
		return true;

	const QStringList words = normalizedText.split(QLatin1Char(' '), Qt::SkipEmptyParts);
	const int limit = std::min(4, static_cast<int>(words.size()));
	for (int i = 0; i < limit; ++i) {
		const QString &word = words.at(i);
		if (word == QStringLiteral("oi") || word == QStringLiteral("ola") ||
		    word == QStringLiteral("salve") || word == QStringLiteral("hi") || word == QStringLiteral("hey"))
			return true;
	}

	return false;
}

bool hasGratitude(const QStringList &words)
{
	return hasAnyTokenPrefix(words, gratitudePrefixes());
}

bool hasSupportOrTransactionCue(const QStringList &words)
{
	return hasAnyTokenPrefix(words, supportOrTransactionPrefixes());
}

bool hasStreamOperationCue(const QStringList &words)
{
	return hasAnyTokenPrefix(words, streamOperationPrefixes());
}

bool hasComplimentCue(const QStringList &words)
{
	return hasAnyTokenPrefix(words, complimentPrefixes());
}

int contentTokenCount(const QString &text)
{
	return Curation::Scoring::TextAnalysis::contentTokens(text).size();
}

bool looksLikeGenericSocialMeta(const QString &text)
{
	const QString value = Curation::Scoring::TextAnalysis::normalized(text);
	if (value.isEmpty())
		return false;

	const QStringList words = value.split(QLatin1Char(' '), Qt::SkipEmptyParts);
	const int contentCount = contentTokenCount(text);
	const bool greeting = hasGreeting(value);
	const bool gratitude = hasGratitude(words);
	const bool support = hasSupportOrTransactionCue(words);
	const bool streamOps = hasStreamOperationCue(words);
	const bool compliment = hasComplimentCue(words);
	const bool question = text.contains(QLatin1Char('?')) || startsWithAnyPhrase(value, questionStartPhrases());
	const bool personalIssue = hasAnyPhrase(value, personalIssuePhrases());

	if (question || personalIssue)
		return false;
	if (greeting && (words.size() <= 12 || contentCount <= 4))
		return true;
	if (gratitude && (support || compliment || words.size() <= 14 || contentCount <= 5))
		return true;
	if (streamOps && (contentCount <= 6 || support || gratitude))
		return true;
	if (support && contentCount <= 5)
		return true;
	return false;
}

bool looksLikeLikelyVocativeTurn(const QString &text)
{
	const QString trimmed = text.simplified();
	if (trimmed.isEmpty() || trimmed.size() > 140)
		return false;

	if (trimmed.contains(QLatin1Char('?')))
		return true;

	static const QRegularExpression trailingCommaName(
		QStringLiteral(",\\s*([\\p{L}][\\p{L}0-9_]{2,24})\\s*[!?.]?$"),
		QRegularExpression::UseUnicodePropertiesOption);
	const QRegularExpressionMatch match = trailingCommaName.match(trimmed);
	if (!match.hasMatch())
		return false;

	const QString possibleName = Curation::Scoring::TextAnalysis::normalized(match.captured(1));
	if (possibleName.isEmpty())
		return false;
	return !baseStopWords().contains(possibleName);
}

} // namespace

namespace Curation::Scoring::TextAnalysis {

QString stripDiacritics(const QString &value)
{
	const QString normalizedValue = value.normalized(QString::NormalizationForm_D);
	QString output;
	output.reserve(normalizedValue.size());

	for (const QChar ch : normalizedValue) {
		if (ch.category() == QChar::Mark_NonSpacing)
			continue;
		output.append(ch);
	}

	return output;
}

QString normalized(QString value)
{
	value = stripDiacritics(value).toLower();
	value.replace(QRegularExpression(QStringLiteral("[^a-z0-9\\s]+")), QStringLiteral(" "));
	value.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
	return value.trimmed();
}

QString sampleForLog(QString text, int maxChars)
{
	text = text.simplified();
	if (maxChars <= 0 || text.size() <= maxChars)
		return text;
	return text.left(std::max(0, maxChars - 3)).trimmed() + QStringLiteral("...");
}

bool containsAny(const QString &text, const QStringList &phrases)
{
	for (const QString &phrase : phrases) {
		if (!phrase.trimmed().isEmpty() && text.contains(phrase, Qt::CaseInsensitive))
			return true;
	}
	return false;
}

bool normalizedContainsAny(const QString &normalizedText, const QStringList &markers)
{
	return hasAnyPhrase(normalizedText, markers);
}

QStringList meaningfulTerms(const QString &text, const QSet<QString> &extraStopWords)
{
	QSet<QString> stopWords = baseStopWords();
	for (const QString &word : extraStopWords)
		stopWords.insert(word);

	QStringList words = normalized(text).split(QLatin1Char(' '), Qt::SkipEmptyParts);
	words.erase(std::remove_if(words.begin(), words.end(), [&stopWords](const QString &word) {
		return word.size() < 3 || stopWords.contains(word);
	}), words.end());
	words.removeDuplicates();
	return words;
}

QSet<QString> contentTokens(const QString &text)
{
	QSet<QString> tokens;
	const QSet<QString> stopWords = baseStopWords();
	for (const QString &token : normalized(text).split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
		if (token.size() < 4 || stopWords.contains(token))
			continue;
		tokens.insert(token);
	}
	return tokens;
}

bool looksLikeMentalHealthContext(const QString &text)
{
	const QString value = normalized(text);
	if (value.isEmpty())
		return false;

	const QStringList words = value.split(QLatin1Char(' '), Qt::SkipEmptyParts);
	const bool personalStruggle = hasAnyPhrase(value, personalIssuePhrases()) ||
		hasAnyPhrase(value, {QStringLiteral("estou com"), QStringLiteral("eu estou com"), QStringLiteral("eu to com"),
			QStringLiteral("eu tô com"), QStringLiteral("sinto falta"), QStringLiteral("me incomoda"),
			QStringLiteral("i am struggling"), QStringLiteral("i need help")});
	const bool adviceContext = hasAnyTokenPrefix(words,
		{QStringLiteral("ajud"), QStringLiteral("terap"), QStringLiteral("crise"), QStringLiteral("medo"),
		 QStringLiteral("ansiedad"), QStringLiteral("transtorn"), QStringLiteral("diagnostic"), QStringLiteral("health"),
		 QStringLiteral("therapy")});
	return personalStruggle || adviceContext;
}

bool looksLikeGamblingContext(const QString &text)
{
	const QString value = normalized(text);
	const QStringList words = value.split(QLatin1Char(' '), Qt::SkipEmptyParts);
	return hasAnyTokenPrefix(words,
		{QStringLiteral("apost"), QStringLiteral("cassino"), QStringLiteral("bet"), QStringLiteral("vici"),
		 QStringLiteral("gambl")});
}

QSet<QString> semanticCategories(const QString &text)
{
	const QString value = normalized(text);
	const QStringList words = value.split(QLatin1Char(' '), Qt::SkipEmptyParts);
	QSet<QString> categories;
	if (value.isEmpty())
		return categories;

	if (hasGreeting(value))
		categories.insert(QStringLiteral("greeting"));
	if (looksLikeGenericSocialMeta(text))
		categories.insert(QStringLiteral("social_meta"));
	if (hasAnyPhrase(value, topicTransitionPhrases()))
		categories.insert(QStringLiteral("topic_transition"));
	if (hasStreamOperationCue(words) && !looksLikeQuestionOrViewerMessage(text))
		categories.insert(QStringLiteral("stream_operation"));
	if (looksLikeQuestionOrViewerMessage(text) || looksLikeMentalHealthContext(text))
		categories.insert(QStringLiteral("viewer_issue"));
	if (hasAnyTokenPrefix(words, adviceOrResolutionPrefixes()))
		categories.insert(QStringLiteral("advice_or_resolution"));
	if (looksLikeGamblingContext(text))
		categories.insert(QStringLiteral("risk_or_habit"));

	return categories;
}

QSet<QString> contentCategories(const QString &text)
{
	QSet<QString> categories = semanticCategories(text);
	for (auto it = categories.begin(); it != categories.end();) {
		if (isNoiseCategory(*it))
			it = categories.erase(it);
		else
			++it;
	}
	return categories;
}

double lexicalTopicOverlap(const QString &text, const QString &contextText)
{
	const QSet<QString> left = contentTokens(text);
	const QSet<QString> right = contentTokens(contextText);
	if (left.isEmpty() || right.isEmpty())
		return 0.0;

	int intersection = 0;
	for (const QString &token : left) {
		if (right.contains(token))
			++intersection;
	}

	const int denominator = std::max(1, std::min(static_cast<int>(left.size()), static_cast<int>(right.size())));
	return static_cast<double>(intersection) / static_cast<double>(denominator);
}

bool hasSharedSemanticTopic(const QString &text, const QString &contextText)
{
	const QSet<QString> textCategories = contentCategories(text);
	const QSet<QString> contextCategories = contentCategories(contextText);
	for (const QString &category : textCategories) {
		if (contextCategories.contains(category))
			return true;
	}

	return lexicalTopicOverlap(text, contextText) >= 0.25;
}

bool hasNoiseOnlySemanticTopic(const QString &text)
{
	if (isSocialOrStreamMetaText(text))
		return true;

	const QSet<QString> categories = semanticCategories(text);
	if (categories.isEmpty())
		return false;

	for (const QString &category : categories) {
		if (!isNoiseCategory(category))
			return false;
	}
	return true;
}

bool looksLikeSameExchangeContinuation(const QString &text)
{
	const QString value = normalized(text);
	if (value.isEmpty())
		return false;

	if (startsWithAnyPhrase(value,
			{QStringLiteral("mas"), QStringLiteral("porque"), QStringLiteral("por exemplo"),
			 QStringLiteral("entao"), QStringLiteral("então"), QStringLiteral("so que"),
			 QStringLiteral("só que"), QStringLiteral("olha so"), QStringLiteral("olha só"),
			 QStringLiteral("tipo assim"), QStringLiteral("a questao"), QStringLiteral("a questão"),
			 QStringLiteral("o ponto"), QStringLiteral("but"), QStringLiteral("because"), QStringLiteral("for example"),
			 QStringLiteral("the point"), QStringLiteral("so") }))
		return true;

	return hasAnyPhrase(value,
		{QStringLiteral("na minha opiniao"), QStringLiteral("na minha opinião"), QStringLiteral("vamos pensar"),
		 QStringLiteral("parou pra pensar"), QStringLiteral("parou para pensar"), QStringLiteral("entendeu"),
		 QStringLiteral("faz sentido"), QStringLiteral("in my opinion"), QStringLiteral("does that make sense")});
}

bool looksLikeSameTopicContinuation(const QString &text, const QString &contextText)
{
	const QString value = normalized(text);
	const QString context = normalized(contextText);
	if (value.isEmpty() || context.isEmpty())
		return false;

	if (hasSharedSemanticTopic(value, context))
		return true;
	if (looksLikeSameExchangeContinuation(text))
		return true;
	if ((looksLikeMentalHealthContext(contextText) && looksLikeMentalHealthContext(text)) ||
	    (looksLikeGamblingContext(contextText) && looksLikeGamblingContext(text)))
		return true;

	return false;
}

bool looksLikeQuestionOrViewerMessage(const QString &text)
{
	if (text.contains(QLatin1Char('?')))
		return true;

	const QString value = normalized(text);
	if (value.isEmpty())
		return false;

	const QStringList words = value.split(QLatin1Char(' '), Qt::SkipEmptyParts);
	const bool explicitQuestion = startsWithAnyPhrase(value, questionStartPhrases()) ||
		hasAnyPhrase(value,
			{QStringLiteral("o que eu"), QStringLiteral("o que voce"), QStringLiteral("o que você"),
			 QStringLiteral("qual sua"), QStringLiteral("qual a sua"), QStringLiteral("voce acha"),
			 QStringLiteral("você acha"), QStringLiteral("vc acha"), QStringLiteral("what should"),
			 QStringLiteral("how can"), QStringLiteral("why do")});
	const bool viewerCue = hasAnyTokenPrefix(words, viewerCuePrefixes());
	const bool firstPersonViewerIssue = hasAnyPhrase(value, personalIssuePhrases());
	return explicitQuestion || viewerCue || firstPersonViewerIssue;
}

bool looksLikeHardTopicShift(const QString &text, const QString &contextText)
{
	const QString value = normalized(text);
	if (value.isEmpty())
		return false;

	if (hasAnyPhrase(value, topicTransitionPhrases()))
		return true;
	if (hasNoiseOnlySemanticTopic(text) && !hasSharedSemanticTopic(text, contextText))
		return true;

	const QSet<QString> textCategories = contentCategories(text);
	const QSet<QString> contextCategories = contentCategories(contextText);
	if (!textCategories.isEmpty() && !contextCategories.isEmpty() && !hasSharedSemanticTopic(text, contextText))
		return true;

	if ((looksLikeQuestionOrViewerMessage(text) || looksLikeLikelyVocativeTurn(text)) &&
	    !hasSharedSemanticTopic(text, contextText) && !looksLikeSameExchangeContinuation(text))
		return true;

	return false;
}

bool looksLikeViewerContextPrelude(const QString &previousText, const QString &anchorText)
{
	const QString previous = normalized(previousText);
	const QString anchor = normalized(anchorText);
	if (previous.isEmpty() || anchor.isEmpty())
		return false;

	if (isSocialOrStreamMetaText(previousText))
		return false;
	if (looksLikeQuestionOrViewerMessage(previousText) && !hasNoiseOnlySemanticTopic(previousText))
		return hasSharedSemanticTopic(previousText, anchorText) || looksLikeQuestionOrViewerMessage(anchorText);

	return hasSharedSemanticTopic(previousText, anchorText) && contentTokenCount(previousText) >= 3;
}

bool isBacklogOrGreetingText(const QString &text)
{
	return looksLikeGenericSocialMeta(text);
}

bool isSocialOrStreamMetaText(const QString &text)
{
	return looksLikeGenericSocialMeta(text);
}

bool looksLikeNewViewerTurnAfterPause(const QString &text)
{
	const QString value = normalized(text);
	if (value.isEmpty())
		return false;

	if (isSocialOrStreamMetaText(text))
		return true;
	if (looksLikeQuestionOrViewerMessage(text))
		return true;
	return looksLikeLikelyVocativeTurn(text);
}

bool hasConcreteViewerQuestion(const QString &text)
{
	const QString value = normalized(text);
	if (value.isEmpty())
		return false;

	if (text.contains(QLatin1Char('?')))
		return true;
	return startsWithAnyPhrase(value, questionStartPhrases()) || hasAnyPhrase(value, personalIssuePhrases()) ||
	       hasAnyPhrase(value,
		       {QStringLiteral("o que eu"), QStringLiteral("o que voce"), QStringLiteral("o que você"),
		        QStringLiteral("qual sua"), QStringLiteral("qual a sua"), QStringLiteral("voce acha"),
		        QStringLiteral("você acha"), QStringLiteral("vc acha"), QStringLiteral("devo fazer"),
		        QStringLiteral("what should"), QStringLiteral("how can")});
}

bool isGenericFocusTarget(const QString &target)
{
	const QString value = normalized(target);
	if (value.isEmpty())
		return true;

	if (value.startsWith(QStringLiteral("one clear idea")) || value.startsWith(QStringLiteral("the selected idea")) ||
	    value.contains(QStringLiteral("selected range")) || value.contains(QStringLiteral("one exchange")) ||
	    value.contains(QStringLiteral("same message")))
		return true;

	for (const QString &phrase : {QStringLiteral("em uma conversa de live"), QStringLiteral("em uma live"),
		     QStringLiteral("conversa de live"), QStringLiteral("live conversation"),
		     QStringLiteral("conversation in a live"), QStringLiteral("live stream conversation"),
		     QStringLiteral("stream conversation"), QStringLiteral("viewer interaction"),
		     QStringLiteral("viewer message"), QStringLiteral("viewer question"),
		     QStringLiteral("viewer comment"), QStringLiteral("chat message"), QStringLiteral("live chat"),
		     QStringLiteral("general chat"), QStringLiteral("stream chat")}) {
		if (value == phrase || value.contains(phrase))
			return true;
	}

	return false;
}

} // namespace Curation::Scoring::TextAnalysis
