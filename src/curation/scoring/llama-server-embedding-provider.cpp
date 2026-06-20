#include "curation/scoring/llama-server-embedding-provider.hpp"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <algorithm>
#include <utility>

using namespace Curation::Scoring;

namespace {

static constexpr int DEFAULT_TIMEOUT_MS = 10000;
static constexpr int DEFAULT_MAX_TEXT_CHARS = 6000;
static constexpr int MAX_CONSECUTIVE_FAILURES_BEFORE_UNAVAILABLE = 3;

} // namespace

LlamaServerEmbeddingProvider::LlamaServerEmbeddingProvider(LlamaServerEmbeddingProviderOptions options)
	: options_(std::move(options))
{
	options_.endpoint = normalizedEndpoint(options_.endpoint).toString();
	if (options_.timeoutMs <= 0)
		options_.timeoutMs = DEFAULT_TIMEOUT_MS;
	if (options_.maxTextChars <= 0)
		options_.maxTextChars = DEFAULT_MAX_TEXT_CHARS;
	if (options_.modelId.trimmed().isEmpty())
		options_.modelId = QStringLiteral("llama-server-embedding");
}

bool LlamaServerEmbeddingProvider::isAvailable() const
{
	return options_.enabled && !failed_ && normalizedEndpoint(options_.endpoint).isValid();
}

QString LlamaServerEmbeddingProvider::modelId() const
{
	return options_.modelId.trimmed();
}

SemanticEmbedding LlamaServerEmbeddingProvider::embed(const QString &text) const
{
	if (!isAvailable())
		return {};

	const QString input = preparedText(text);
	if (input.isEmpty())
		return {};
	if (options_.cancellationCallback && options_.cancellationCallback())
		return {};

	SemanticEmbedding cached;
	if (cache_.tryGet(modelId(), input, &cached))
		return cached;

	QNetworkAccessManager manager;
	QNetworkRequest request(normalizedEndpoint(options_.endpoint));
	request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
	request.setRawHeader("Accept", "application/json");

	QNetworkReply *reply = manager.post(request, buildRequestPayload(input));

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
		lastError_ = QStringLiteral("embedding_canceled");
		return {};
	}

	if (error != QNetworkReply::NoError) {
		QString message = QStringLiteral("llama_server_http_error:%1").arg(errorString);
		if (!response.isEmpty()) {
			QString preview = QString::fromUtf8(response).simplified();
			if (preview.size() > 240)
				preview = preview.left(240) + QStringLiteral("...");
			message += QStringLiteral(":") + preview;
		}
		markFailure(message);
		return {};
	}

	SemanticEmbedding embedding = parseEmbeddingResponse(response);
	if (!embedding.isValid()) {
		QString preview = QString::fromUtf8(response).simplified();
		if (preview.size() > 240)
			preview = preview.left(240) + QStringLiteral("...");
		markFailure(preview.isEmpty() ? QStringLiteral("llama_server_invalid_embedding_response")
					: QStringLiteral("llama_server_invalid_embedding_response:%1").arg(preview));
		return {};
	}

	markSuccess();
	cache_.put(modelId(), input, embedding);
	return embedding;
}

QString LlamaServerEmbeddingProvider::endpoint() const
{
	return options_.endpoint;
}

QString LlamaServerEmbeddingProvider::lastError() const
{
	return lastError_;
}

QUrl LlamaServerEmbeddingProvider::normalizedEndpoint(const QString &value) const
{
	QString endpoint = value.trimmed();
	if (endpoint.isEmpty())
		endpoint = QStringLiteral("http://127.0.0.1:8080/v1/embeddings");

	QUrl url(endpoint);
	if (!url.isValid() || url.scheme().isEmpty())
		url = QUrl(QStringLiteral("http://") + endpoint);

	QString path = url.path();
	while (path.size() > 1 && path.endsWith(QLatin1Char('/')))
		path.chop(1);
	if (path.isEmpty() || path == QStringLiteral("/"))
		path = QStringLiteral("/v1/embeddings");
	else if (!path.endsWith(QStringLiteral("/v1/embeddings")))
		path = path + QStringLiteral("/v1/embeddings");
	url.setPath(path);
	return url;
}

QByteArray LlamaServerEmbeddingProvider::buildRequestPayload(const QString &text) const
{
	QJsonObject root;
	QJsonArray input;
	input.append(text);
	root.insert(QStringLiteral("input"), input);
	if (!options_.modelId.trimmed().isEmpty())
		root.insert(QStringLiteral("model"), options_.modelId.trimmed());
	return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

SemanticEmbedding LlamaServerEmbeddingProvider::parseEmbeddingResponse(const QByteArray &payload) const
{
	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject())
		return {};

	const QJsonObject root = document.object();
	const QJsonArray data = root.value(QStringLiteral("data")).toArray();
	if (data.isEmpty())
		return {};

	QJsonArray embeddingArray;
	for (const QJsonValue &value : data) {
		const QJsonObject item = value.toObject();
		if (item.value(QStringLiteral("index")).toInt(0) == 0 || embeddingArray.isEmpty()) {
			embeddingArray = item.value(QStringLiteral("embedding")).toArray();
			if (!embeddingArray.isEmpty())
				break;
		}
	}

	SemanticEmbedding embedding;
	embedding.values.reserve(static_cast<long long>(embeddingArray.size()));
	for (const QJsonValue &value : embeddingArray) {
		if (!value.isDouble())
			continue;
		embedding.values.append(static_cast<float>(value.toDouble()));
	}
	return embedding;
}

QString LlamaServerEmbeddingProvider::preparedText(const QString &text) const
{
	QString value = text.simplified();
	if (value.size() > options_.maxTextChars)
		value = value.left(options_.maxTextChars);
	return value;
}

void LlamaServerEmbeddingProvider::markFailure(const QString &message, bool fatal) const
{
	lastError_ = message;
	++consecutiveFailures_;
	if (fatal || consecutiveFailures_ >= MAX_CONSECUTIVE_FAILURES_BEFORE_UNAVAILABLE)
		failed_ = true;
}

void LlamaServerEmbeddingProvider::markSuccess() const
{
	consecutiveFailures_ = 0;
	failed_ = false;
	lastError_.clear();
}
