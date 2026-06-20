#include "curation/scoring/llama-server-reranker-provider.hpp"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>
#include <QTimer>

#include <utility>

using namespace Curation::Scoring;

namespace {

static constexpr int DEFAULT_TIMEOUT_MS = 20000;
static constexpr int DEFAULT_MAX_TEXT_CHARS = 900;

static QString payloadPreview(const QByteArray &payload)
{
	QString preview = QString::fromUtf8(payload).simplified();
	if (preview.size() > 420)
		preview = preview.left(420) + QStringLiteral("...");
	return preview;
}

static bool jsonNumber(const QJsonValue &value, double *out)
{
	if (!out)
		return false;
	if (value.isDouble()) {
		*out = value.toDouble();
		return true;
	}
	if (value.isString()) {
		bool ok = false;
		const double parsed = value.toString().toDouble(&ok);
		if (ok) {
			*out = parsed;
			return true;
		}
	}
	return false;
}

static int intValue(const QJsonObject &object, const QStringList &keys)
{
	for (const QString &key : keys) {
		const QJsonValue value = object.value(key);
		if (value.isDouble())
			return value.toInt(-1);
		if (value.isString()) {
			bool ok = false;
			const int parsed = value.toString().toInt(&ok);
			if (ok)
				return parsed;
		}
	}
	return -1;
}

static QJsonArray responseResults(const QJsonDocument &document)
{
	if (document.isArray())
		return document.array();
	if (!document.isObject())
		return {};

	const QJsonObject root = document.object();
	for (const QString &key : QStringList{QStringLiteral("results"), QStringLiteral("data"), QStringLiteral("rankings"),
	     QStringLiteral("rerank"), QStringLiteral("scores")}) {
		const QJsonArray array = root.value(key).toArray();
		if (!array.isEmpty())
			return array;
	}
	return {};
}

static double rerankScoreFromItem(const QJsonObject &item, bool *ok)
{
	if (ok)
		*ok = false;

	for (const QString &key : QStringList{QStringLiteral("relevance_score"), QStringLiteral("relevanceScore"),
		     QStringLiteral("score"), QStringLiteral("rank_score")}) {
		double value = 0.0;
		if (jsonNumber(item.value(key), &value)) {
			if (ok)
				*ok = true;
			return value;
		}
	}

	const QJsonObject nested = item.value(QStringLiteral("result")).toObject();
	if (!nested.isEmpty())
		return rerankScoreFromItem(nested, ok);

	return 0.0;
}

static int rerankIndexFromItem(const QJsonObject &item, int fallbackIndex)
{
	int index = intValue(item, QStringList{QStringLiteral("index"), QStringLiteral("document_index"),
		QStringLiteral("documentIndex"), QStringLiteral("corpus_id")});
	if (index >= 0)
		return index;

	const QJsonObject document = item.value(QStringLiteral("document")).toObject();
	if (!document.isEmpty()) {
		index = intValue(document, QStringList{QStringLiteral("index"), QStringLiteral("id")});
		if (index >= 0)
			return index;
	}

	return fallbackIndex;
}

} // namespace

LlamaServerRerankerProvider::LlamaServerRerankerProvider(LlamaServerRerankerProviderOptions options)
	: options_(std::move(options))
{
	options_.endpoint = normalizedEndpoint(options_.endpoint).toString();
	if (options_.timeoutMs <= 0)
		options_.timeoutMs = DEFAULT_TIMEOUT_MS;
	if (options_.maxTextChars <= 0)
		options_.maxTextChars = DEFAULT_MAX_TEXT_CHARS;
	if (options_.modelId.trimmed().isEmpty())
		options_.modelId = QStringLiteral("llama-server-reranker");
}

bool LlamaServerRerankerProvider::isAvailable() const
{
	return options_.enabled && !failed_ && normalizedEndpoint(options_.endpoint).isValid();
}

QString LlamaServerRerankerProvider::modelId() const
{
	return options_.modelId.trimmed();
}

double LlamaServerRerankerProvider::score(const QString &query, const QString &candidateText) const
{
	const QVector<double> scores = scoreBatch(query, QVector<QString>{candidateText});
	return scores.isEmpty() ? 0.0 : scores.first();
}

QVector<double> LlamaServerRerankerProvider::scoreBatch(const QString &query, const QVector<QString> &candidateTexts) const
{
	if (!isAvailable() || query.trimmed().isEmpty() || candidateTexts.isEmpty())
		return {};

	if (options_.cancellationCallback && options_.cancellationCallback())
		return {};

	QVector<QString> preparedTexts;
	preparedTexts.reserve(static_cast<long long>(candidateTexts.size()));
	for (const QString &candidateText : candidateTexts) {
		const QString prepared = preparedText(candidateText);
		if (prepared.isEmpty())
			return {};
		preparedTexts.append(prepared);
	}

	QNetworkAccessManager manager;
	QNetworkRequest request(normalizedEndpoint(options_.endpoint));
	request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
	request.setRawHeader("Accept", "application/json");

	QNetworkReply *reply = manager.post(request, buildRequestPayload(preparedText(query), preparedTexts));

	QEventLoop loop;
	QTimer timer;
	QTimer cancelTimer;
	timer.setSingleShot(true);
	cancelTimer.setInterval(100);

	QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
	QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
	QObject::connect(&cancelTimer, &QTimer::timeout, &loop, [&loop, reply, this]() {
		if (!options_.cancellationCallback || !options_.cancellationCallback())
			return;
		if (reply && !reply->isFinished())
			reply->abort();
		loop.quit();
	});
	timer.start(options_.timeoutMs);
	if (options_.cancellationCallback)
		cancelTimer.start();
	loop.exec();

	if (timer.isActive())
		timer.stop();
	if (cancelTimer.isActive())
		cancelTimer.stop();
	if (!reply->isFinished())
		reply->abort();

	const QNetworkReply::NetworkError error = reply->error();
	const QString errorString = reply->errorString();
	QByteArray response;
	if (reply->isOpen() && reply->isReadable())
		response = reply->readAll();
	reply->deleteLater();

	if (options_.cancellationCallback && options_.cancellationCallback()) {
		lastError_ = QStringLiteral("reranker_canceled");
		return {};
	}

	if (error != QNetworkReply::NoError) {
		const QString responsePreview = payloadPreview(response);
		QString message = QStringLiteral("llama_server_rerank_http_error:%1").arg(errorString);
		if (!responsePreview.isEmpty())
			message += QStringLiteral(":") + responsePreview;
		markFailure(message);
		return {};
	}

	QVector<double> scores = parseRerankResponse(response, preparedTexts.size());
	if (scores.size() != preparedTexts.size()) {
		markFailure(QStringLiteral("llama_server_invalid_rerank_response:%1").arg(payloadPreview(response)));
		return {};
	}
	return scores;
}

QString LlamaServerRerankerProvider::endpoint() const
{
	return options_.endpoint;
}

QString LlamaServerRerankerProvider::lastError() const
{
	return lastError_;
}

QUrl LlamaServerRerankerProvider::normalizedEndpoint(const QString &value) const
{
	QString endpoint = value.trimmed();
	if (endpoint.isEmpty())
		endpoint = QStringLiteral("http://127.0.0.1:8081/v1/rerank");

	QUrl url(endpoint);
	if (!url.isValid() || url.scheme().isEmpty())
		url = QUrl(QStringLiteral("http://") + endpoint);

	QString path = url.path();
	while (path.size() > 1 && path.endsWith(QLatin1Char('/')))
		path.chop(1);
	if (path.isEmpty() || path == QStringLiteral("/"))
		path = QStringLiteral("/v1/rerank");
	else if (!path.endsWith(QStringLiteral("/v1/rerank")))
		path = path + QStringLiteral("/v1/rerank");
	url.setPath(path);
	return url;
}

QByteArray LlamaServerRerankerProvider::buildRequestPayload(const QString &query,
	const QVector<QString> &candidateTexts) const
{
	QJsonObject root;
	root.insert(QStringLiteral("query"), query);
	QJsonArray documents;
	for (const QString &candidateText : candidateTexts)
		documents.append(candidateText);
	root.insert(QStringLiteral("documents"), documents);

	// llama-server's native reranking endpoint only documents `query` and `documents`.
	// Some builds accept OpenAI/Jina-style optional fields, but others return HTTP 500
	// when extra keys such as `model`, `top_n`, or `return_documents` are present.
	// The model is already selected by the llama-server process launched on the reranker port.
	return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QVector<double> LlamaServerRerankerProvider::parseRerankResponse(const QByteArray &payload, qsizetype expectedSize) const
{
	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
	if (parseError.error != QJsonParseError::NoError)
		return {};

	const QJsonArray results = responseResults(document);
	if (results.isEmpty())
		return {};

	QVector<double> scores;
	scores.fill(0.0, static_cast<long long>(expectedSize));
	QVector<bool> seen;
	seen.fill(false, static_cast<long long>(expectedSize));

	int seenCount = 0;
	int sequentialIndex = 0;
	for (const QJsonValue &value : results) {
		int index = sequentialIndex;
		double score = 0.0;
		bool scoreOk = false;

		if (jsonNumber(value, &score)) {
			scoreOk = true;
		} else {
			const QJsonObject item = value.toObject();
			if (item.isEmpty()) {
				++sequentialIndex;
				continue;
			}

			index = rerankIndexFromItem(item, sequentialIndex);
			score = rerankScoreFromItem(item, &scoreOk);
		}

		++sequentialIndex;
		if (index < 0 || index >= expectedSize || !scoreOk)
			continue;

		scores[index] = score;
		if (!seen[index]) {
			seen[index] = true;
			++seenCount;
		}
	}

	if (seenCount <= 0)
		return {};
	return scores;
}

QString LlamaServerRerankerProvider::preparedText(const QString &text) const
{
	QString value = text.simplified();
	if (value.size() > options_.maxTextChars)
		value = value.left(options_.maxTextChars);
	return value;
}

void LlamaServerRerankerProvider::markFailure(const QString &message) const
{
	failed_ = true;
	lastError_ = message;
}
