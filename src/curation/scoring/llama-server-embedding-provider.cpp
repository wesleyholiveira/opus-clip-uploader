#include "curation/scoring/llama-server-embedding-provider.hpp"

#include <QEventLoop>
#include <QMutexLocker>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <algorithm>
#include <future>
#include <utility>
#include <vector>

using namespace Curation::Scoring;

namespace {

static constexpr int DEFAULT_TIMEOUT_MS = 10000;
static constexpr int DEFAULT_MAX_TEXT_CHARS = 6000;
static constexpr int DEFAULT_MAX_BATCH_SIZE = 64;

} // namespace

LlamaServerEmbeddingProvider::LlamaServerEmbeddingProvider(LlamaServerEmbeddingProviderOptions options)
	: options_(std::move(options))
{
	options_.endpoint = normalizedEndpoint(options_.endpoint).toString();
	if (options_.timeoutMs <= 0)
		options_.timeoutMs = DEFAULT_TIMEOUT_MS;
	if (options_.maxTextChars <= 0)
		options_.maxTextChars = DEFAULT_MAX_TEXT_CHARS;
	if (options_.maxBatchSize <= 0)
		options_.maxBatchSize = DEFAULT_MAX_BATCH_SIZE;
	options_.maxBatchSize = std::clamp(options_.maxBatchSize, 1, 256);
	if (options_.modelId.trimmed().isEmpty())
		options_.modelId = QStringLiteral("llama-server-embedding");
}

bool LlamaServerEmbeddingProvider::isAvailable() const
{
	// Availability here means the backend is configured and worth trying.
	// Do not make transient request failures globally disable the provider:
	// semantic boundary refinement and semantic scoring can run in parallel, and a
	// few failed/slow requests must not turn every later candidate into
	// semantic_embedding_unavailable.
	return options_.enabled && normalizedEndpoint(options_.endpoint).isValid();
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
		setLastError(QStringLiteral("embedding_canceled"));
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

QVector<SemanticEmbedding> LlamaServerEmbeddingProvider::embedBatch(const QVector<QString> &texts) const
{
	QVector<SemanticEmbedding> result;
	result.resize(static_cast<long long>(texts.size()));
	if (!isAvailable() || texts.isEmpty())
		return result;

	QVector<QString> pendingTexts;
	QVector<int> pendingIndexes;
	pendingTexts.reserve(std::min(static_cast<int>(texts.size()), options_.maxBatchSize));
	pendingIndexes.reserve(std::min(static_cast<int>(texts.size()), options_.maxBatchSize));

	auto flushPending = [&]() {
		if (pendingTexts.isEmpty())
			return;
		QVector<SemanticEmbedding> embeddings = postEmbeddingBatch(pendingTexts);
		if (embeddings.size() != pendingTexts.size()) {
			embeddings.clear();
			embeddings.resize(static_cast<long long>(pendingTexts.size()));
			// Keep the HTTP fallback conservative: do not parallelize fallback
			// individual calls because llama-server already serializes and can
			// return Operation canceled under concurrent local load.
			for (int i = 0; i < static_cast<int>(pendingTexts.size()); ++i) {
				if (options_.cancellationCallback && options_.cancellationCallback())
					break;
				embeddings[i] = embed(pendingTexts.at(i));
			}
		}
		for (int i = 0; i < static_cast<int>(embeddings.size()) && i < static_cast<int>(pendingIndexes.size()); ++i) {
			const SemanticEmbedding &embedding = embeddings.at(i);
			if (!embedding.isValid())
				continue;
			const int resultIndex = pendingIndexes.at(i);
			if (resultIndex < 0 || resultIndex >= static_cast<int>(result.size()))
				continue;
			result[resultIndex] = embedding;
			if (i < static_cast<int>(pendingTexts.size()))
				cache_.put(modelId(), pendingTexts.at(i), embedding);
		}
		pendingTexts.clear();
		pendingIndexes.clear();
	};

	for (int i = 0; i < static_cast<int>(texts.size()); ++i) {
		if (options_.cancellationCallback && options_.cancellationCallback())
			break;
		const QString input = preparedText(texts.at(i));
		if (input.isEmpty())
			continue;

		SemanticEmbedding cached;
		if (cache_.tryGet(modelId(), input, &cached)) {
			result[i] = cached;
			continue;
		}

		pendingTexts.append(input);
		pendingIndexes.append(i);
		if (pendingTexts.size() >= options_.maxBatchSize)
			flushPending();
	}
	flushPending();
	return result;
}

QString LlamaServerEmbeddingProvider::endpoint() const
{
	return options_.endpoint;
}

QString LlamaServerEmbeddingProvider::lastError() const
{
	QMutexLocker locker(&stateMutex_);
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

QByteArray LlamaServerEmbeddingProvider::buildBatchRequestPayload(const QVector<QString> &texts) const
{
	QJsonObject root;
	QJsonArray input;
	for (const QString &text : texts)
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

QVector<SemanticEmbedding> LlamaServerEmbeddingProvider::parseEmbeddingBatchResponse(const QByteArray &payload,
	qsizetype expectedSize) const
{
	QJsonParseError parseError;
	const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject())
		return {};

	const QJsonObject root = document.object();
	const QJsonArray data = root.value(QStringLiteral("data")).toArray();
	if (data.isEmpty())
		return {};

	QVector<SemanticEmbedding> embeddings;
	embeddings.resize(static_cast<long long>(expectedSize));
	int sequentialIndex = 0;
	int validCount = 0;
	for (const QJsonValue &value : data) {
		const QJsonObject item = value.toObject();
		if (item.isEmpty()) {
			++sequentialIndex;
			continue;
		}
		int index = item.value(QStringLiteral("index")).toInt(sequentialIndex);
		++sequentialIndex;
		if (index < 0 || index >= expectedSize)
			continue;

		const QJsonArray embeddingArray = item.value(QStringLiteral("embedding")).toArray();
		SemanticEmbedding embedding;
		embedding.values.reserve(static_cast<long long>(embeddingArray.size()));
		for (const QJsonValue &number : embeddingArray) {
			if (number.isDouble())
				embedding.values.append(static_cast<float>(number.toDouble()));
		}
		if (embedding.isValid()) {
			embeddings[index] = embedding;
			++validCount;
		}
	}
	return validCount > 0 ? embeddings : QVector<SemanticEmbedding>{};
}

QVector<SemanticEmbedding> LlamaServerEmbeddingProvider::postEmbeddingBatch(const QVector<QString> &preparedTexts) const
{
	if (preparedTexts.isEmpty())
		return {};

	QNetworkAccessManager manager;
	QNetworkRequest request(normalizedEndpoint(options_.endpoint));
	request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
	request.setRawHeader("Accept", "application/json");

	QNetworkReply *reply = manager.post(request, buildBatchRequestPayload(preparedTexts));

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
		setLastError(QStringLiteral("embedding_canceled"));
		return {};
	}

	if (error != QNetworkReply::NoError) {
		QString message = QStringLiteral("llama_server_batch_http_error:%1").arg(errorString);
		if (!response.isEmpty()) {
			QString preview = QString::fromUtf8(response).simplified();
			if (preview.size() > 240)
				preview = preview.left(240) + QStringLiteral("...");
			message += QStringLiteral(":") + preview;
		}
		markFailure(message);
		return {};
	}

	QVector<SemanticEmbedding> embeddings = parseEmbeddingBatchResponse(response, preparedTexts.size());
	if (embeddings.size() != preparedTexts.size()) {
		QString preview = QString::fromUtf8(response).simplified();
		if (preview.size() > 240)
			preview = preview.left(240) + QStringLiteral("...");
		markFailure(preview.isEmpty() ? QStringLiteral("llama_server_invalid_batch_embedding_response")
					: QStringLiteral("llama_server_invalid_batch_embedding_response:%1").arg(preview));
		return {};
	}
	markSuccess();
	return embeddings;
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
	QMutexLocker locker(&stateMutex_);
	lastError_ = message;
	++consecutiveFailures_;
	// Only fatal configuration errors may disable the provider globally. Ordinary
	// HTTP/parse/timeouts are candidate-level failures and should be reported as
	// semantic_embedding_failed, not as semantic_embedding_unavailable for the
	// whole batch.
	if (fatal)
		failed_ = true;
}

void LlamaServerEmbeddingProvider::markSuccess() const
{
	QMutexLocker locker(&stateMutex_);
	consecutiveFailures_ = 0;
	failed_ = false;
	lastError_.clear();
}

void LlamaServerEmbeddingProvider::setLastError(const QString &message) const
{
	QMutexLocker locker(&stateMutex_);
	lastError_ = message;
}
