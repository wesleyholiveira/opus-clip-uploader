#include "curation/scoring/text-analysis.hpp"

#include <QRegularExpression>

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
		QStringLiteral("que"), QStringLiteral("voce"), QStringLiteral("voces"), QStringLiteral("acho"),
		QStringLiteral("assim"), QStringLiteral("aqui"), QStringLiteral("aquela"), QStringLiteral("aquele"),
		QStringLiteral("cara"), QStringLiteral("mano"), QStringLiteral("tipo"), QStringLiteral("porque"),
		QStringLiteral("entao"), QStringLiteral("tambem"), QStringLiteral("quando"), QStringLiteral("onde"),
		QStringLiteral("isso"), QStringLiteral("esse"), QStringLiteral("essa"), QStringLiteral("esses"),
		QStringLiteral("essas"), QStringLiteral("pela"), QStringLiteral("pelo"), QStringLiteral("tava"),
		QStringLiteral("estava"), QStringLiteral("agora"), QStringLiteral("muito"), QStringLiteral("mesmo"),
		QStringLiteral("meio"), QStringLiteral("ne"), QStringLiteral("ta")
	};
}

bool isNoiseCategory(const QString &category)
{
	return category == QStringLiteral("stream_operation") || category == QStringLiteral("greeting") ||
	       category == QStringLiteral("gameplay");
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
	for (const QString &marker : markers) {
		if (normalizedText.contains(marker))
			return true;
	}
	return false;
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

	if (normalizedContainsAny(value,
			{QStringLiteral("depress"), QStringLiteral("ansiedade"), QStringLiteral("ansioso"),
			 QStringLiteral("saude mental"), QStringLiteral("psicolog"), QStringLiteral("terapia"),
			 QStringLiteral("crise"), QStringLiteral("fobia social"), QStringLiteral("tdah"),
			 QStringLiteral("transtorno"), QStringLiteral("diagnostic")}))
		return true;

	return normalizedContainsAny(value,
		{QStringLiteral("nao sei se"), QStringLiteral("acho que tenho"), QStringLiteral("sera que eu tenho"),
		 QStringLiteral("eu acho que"), QStringLiteral("eu tenho medo"), QStringLiteral("eu sinto que"),
		 QStringLiteral("sinto que"), QStringLiteral("me sinto"), QStringLiteral("eu to com"),
		 QStringLiteral("to com"), QStringLiteral("estou com")});
}

bool looksLikeGamblingContext(const QString &text)
{
	const QString value = normalized(text);
	return normalizedContainsAny(value,
		{QStringLiteral("aposta"), QStringLiteral("apostam"), QStringLiteral("apostar"),
		 QStringLiteral("vicio"), QStringLiteral("cassino"), QStringLiteral("jogo de azar"),
		 QStringLiteral("bet"), QStringLiteral("publicidade"), QStringLiteral("influencia"),
		 QStringLiteral("influenciador"), QStringLiteral("receber um valor"), QStringLiteral("moralmente"),
		 QStringLiteral("legalmente"), QStringLiteral("felipe")});
}

QSet<QString> semanticCategories(const QString &text)
{
	const QString value = normalized(text);
	QSet<QString> categories;
	if (value.isEmpty())
		return categories;

	if (looksLikeMentalHealthContext(value))
		categories.insert(QStringLiteral("mental_health"));
	if (looksLikeGamblingContext(value))
		categories.insert(QStringLiteral("gambling"));
	if (normalizedContainsAny(value,
			{QStringLiteral("esquecer"), QStringLiteral("garota"), QStringLiteral("relacionamento"),
			 QStringLiteral("namor"), QStringLiteral("gostei"), QStringLiteral("termin"),
			 QStringLiteral("ficante")}))
		categories.insert(QStringLiteral("relationship"));
	if (normalizedContainsAny(value,
			{QStringLiteral("aumento"), QStringLiteral("chefe"), QStringLiteral("empresa"),
			 QStringLiteral("trabalho"), QStringLiteral("salario"), QStringLiteral("merecedor")}))
		categories.insert(QStringLiteral("career"));
	if (normalizedContainsAny(value,
			{QStringLiteral("usado"), QStringLiteral("me sinto"), QStringLiteral("sinto que"),
			 QStringLiteral("valor"), QStringLiteral("amizade"), QStringLiteral("amigo")}))
		categories.insert(QStringLiteral("self_worth"));
	if (normalizedContainsAny(value,
			{QStringLiteral("perdi meu pai"), QStringLiteral("pai pra aposta"), QStringLiteral("minha mae"),
			 QStringLiteral("padrasto"), QStringLiteral("familia")}))
		categories.insert(QStringLiteral("family"));
	if (normalizedContainsAny(value,
			{QStringLiteral("convite"), QStringLiteral("lobby"), QStringLiteral("alice"),
			 QStringLiteral("aceita esse"), QStringLiteral("cancela"), QStringLiteral("entrar na sala")}))
		categories.insert(QStringLiteral("stream_operation"));
	if (normalizedContainsAny(value,
			{QStringLiteral("boa noite"), QStringLiteral("bom dia"), QStringLiteral("boa tarde"),
			 QStringLiteral("seja bem vindo"), QStringLiteral("bem vindo"), QStringLiteral("salve"),
			 QStringLiteral("e ai "), QStringLiteral("oi ")}))
		categories.insert(QStringLiteral("greeting"));
	if (normalizedContainsAny(value,
			{QStringLiteral("block blast"), QStringLiteral("sand bricks"), QStringLiteral("jogo parecido"),
			 QStringLiteral("partida"), QStringLiteral("personagem"), QStringLiteral("tela de jogo")}))
		categories.insert(QStringLiteral("gameplay"));

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

	for (const QString &marker : {QStringLiteral("mas "), QStringLiteral("porque "), QStringLiteral("imagina"),
		     QStringLiteral("uma coisa"), QStringLiteral("outra coisa"), QStringLiteral("por exemplo"),
		     QStringLiteral("entao assim"), QStringLiteral("e se"), QStringLiteral("so que"),
		     QStringLiteral("olha so"), QStringLiteral("tipo assim")}) {
		if (value.startsWith(marker))
			return true;
	}

	return normalizedContainsAny(value,
		{QStringLiteral("ja parou pra pensar"), QStringLiteral("voce ja parou pra pensar"),
		 QStringLiteral("ce ja parou pra pensar"), QStringLiteral("ja parou para pensar"),
		 QStringLiteral("parou pra pensar"), QStringLiteral("parou para pensar"),
		 QStringLiteral("na minha opiniao"), QStringLiteral("moralmente incorreto"),
		 QStringLiteral("legalmente correto"), QStringLiteral("felipe"), QStringLiteral("entendendo"),
		 QStringLiteral("entendeu")});
}

bool looksLikeSameTopicContinuation(const QString &text, const QString &contextText)
{
	const QString value = normalized(text);
	const QString context = normalized(contextText);
	if (value.isEmpty() || context.isEmpty())
		return false;

	if (hasSharedSemanticTopic(value, context))
		return true;
	if (looksLikeGamblingContext(context) && looksLikeGamblingContext(value))
		return true;
	if (looksLikeMentalHealthContext(context) && looksLikeMentalHealthContext(value))
		return true;

	return false;
}

bool looksLikeQuestionOrViewerMessage(const QString &text)
{
	if (text.contains(QLatin1Char('?')))
		return true;

	const QString value = normalized(text);
	const bool explicitQuestion = normalizedContainsAny(value,
		{QStringLiteral("como eu"), QStringLiteral("como posso"), QStringLiteral("por que"),
		 QStringLiteral("porque eu"), QStringLiteral("o que eu"), QStringLiteral("what should"),
		 QStringLiteral("how can"), QStringLiteral("why do"), QStringLiteral("should i")});
	const bool viewerCue = normalizedContainsAny(value,
		{QStringLiteral("mandou"), QStringLiteral("perguntou"), QStringLiteral("comentou"),
		 QStringLiteral("falou"), QStringLiteral("disse"), QStringLiteral("asked"),
		 QStringLiteral("commented"), QStringLiteral("viewer"), QStringLiteral("chat")});
	const bool firstPersonViewerIssue = normalizedContainsAny(value,
		{QStringLiteral("nao sei se"), QStringLiteral("nao sei como"), QStringLiteral("me sinto"),
		 QStringLiteral("sinto falta"), QStringLiteral("tenho medo"), QStringLiteral("como faco"),
		 QStringLiteral("o que faco")});
	return explicitQuestion || viewerCue || firstPersonViewerIssue;
}

bool looksLikeHardTopicShift(const QString &text, const QString &contextText)
{
	const QString value = normalized(text);
	if (value.isEmpty())
		return false;

	if (hasNoiseOnlySemanticTopic(text) && !hasSharedSemanticTopic(text, contextText))
		return true;

	const QSet<QString> textCategories = contentCategories(text);
	const QSet<QString> contextCategories = contentCategories(contextText);
	if (!textCategories.isEmpty() && !contextCategories.isEmpty() && !hasSharedSemanticTopic(text, contextText))
		return true;

	if (looksLikeQuestionOrViewerMessage(text) && !hasSharedSemanticTopic(text, contextText) &&
	    !looksLikeSameExchangeContinuation(text))
		return true;

	return normalizedContainsAny(value,
		{QStringLiteral("mudando de assunto"), QStringLiteral("proxima pergunta"),
		 QStringLiteral("outra coisa"), QStringLiteral("agora vamos"), QStringLiteral("next question"),
		 QStringLiteral("by the way")});
}

bool looksLikeViewerContextPrelude(const QString &previousText, const QString &anchorText)
{
	const QString previous = normalized(previousText);
	const QString anchor = normalized(anchorText);
	if (previous.isEmpty() || anchor.isEmpty())
		return false;

	if (normalizedContainsAny(previous, {QStringLiteral("seja bem vindo"), QStringLiteral("bem vindo"), QStringLiteral("salve")}))
		return false;

	if (looksLikeMentalHealthContext(anchorText) && looksLikeMentalHealthContext(previousText))
		return true;

	const bool anchorIsRelationship = normalizedContainsAny(anchor,
		{QStringLiteral("esquecer"), QStringLiteral("garota"), QStringLiteral("relacionamento")});
	if (anchorIsRelationship && normalizedContainsAny(previous,
			{QStringLiteral("gostei"), QStringLiteral("relacionamento"), QStringLiteral("garota")}))
		return true;

	return hasSharedSemanticTopic(previousText, anchorText) && looksLikeQuestionOrViewerMessage(previousText);
}

bool isBacklogOrGreetingText(const QString &text)
{
	const QString value = normalized(text);
	if (value.isEmpty())
		return false;

	if (normalizedContainsAny(value,
			{QStringLiteral("nao sei como e que ele ta"), QStringLiteral("nao sei como ele ta"),
			 QStringLiteral("j2 tambem"), QStringLiteral("o j2"), QStringLiteral("ta indo ai"),
			 QStringLiteral("nao sei nem se ele"), QStringLiteral("seja bem vindo"),
			 QStringLiteral("bem vindo"), QStringLiteral("pessoa sem nick"), QStringLiteral("sem nick"),
			 QStringLiteral("estamos entendidos"), QStringLiteral("tambem seguir"),
			 QStringLiteral("ok estamos"), QStringLiteral("salve"), QStringLiteral("boa noite"),
			 QStringLiteral("bom dia"), QStringLiteral("boa tarde"), QStringLiteral("convite errado"),
			 QStringLiteral("mandei o convite"), QStringLiteral("nao aceita esse"),
			 QStringLiteral("cancela cancela"), QStringLiteral("alice"), QStringLiteral("block blast"),
			 QStringLiteral("sand bricks"), QStringLiteral("jogo parecido")}))
		return true;

	return value.startsWith(QStringLiteral("e ai ")) || value == QStringLiteral("e ai") || value.startsWith(QStringLiteral("ok "));
}

bool hasConcreteViewerQuestion(const QString &text)
{
	const QString value = normalized(text);
	return normalizedContainsAny(value,
		{QStringLiteral("como posso"), QStringLiteral("como eu posso"), QStringLiteral("o que eu faco"),
		 QStringLiteral("o que faco"), QStringLiteral("por que"), QStringLiteral("ansiedade"),
		 QStringLiteral("depress"), QStringLiteral("aumento"), QStringLiteral("chefe"),
		 QStringLiteral("esquecer"), QStringLiteral("relacionamento"), QStringLiteral("conselho"),
		 QStringLiteral("me sinto"), QStringLiteral("tenho"), QStringLiteral("perdi"),
		 QStringLiteral("lembra"), QStringLiteral("nao sei se"), QStringLiteral("nao sei o que"),
		 QStringLiteral("nao sei como fazer"), QStringLiteral("na ansiedade")});
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
