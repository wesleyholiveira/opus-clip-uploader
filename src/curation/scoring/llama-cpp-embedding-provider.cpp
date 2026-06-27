#include "curation/scoring/llama-cpp-embedding-provider.hpp"

#include "curation/scoring/llama-cpp-model-resolver.hpp"
#include "curation/scoring/llama-cpp-batch-wrapper.hpp"

#include <obs-module.h>

#include <QByteArray>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMutexLocker>
#include <QStringList>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cmath>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

#ifdef CLIP_CROPPER_WITH_LLAMA_CPP
#include "llama.h"
#endif

using namespace Curation::Scoring;

namespace {

static int effectiveThreadCount(int configured)
{
	if (configured > 0)
		return configured;
	const int ideal = QThread::idealThreadCount();
	return std::clamp(ideal > 0 ? ideal - 1 : 4, 1, 12);
}

static QVector<float> normalized(QVector<float> values)
{
	double norm = 0.0;
	for (const float value : values)
		norm += static_cast<double>(value) * static_cast<double>(value);
	if (norm <= 0.0)
		return values;
	const float scale = static_cast<float>(1.0 / std::sqrt(norm));
	for (float &value : values)
		value *= scale;
	return values;
}

#ifdef CLIP_CROPPER_WITH_LLAMA_CPP
static bool llamaAbortCallback(void *data)
{
	auto *callback = static_cast<const std::function<bool()> *>(data);
	return callback && *callback && (*callback)();
}

static void ensureBackendInitialized()
{
	static std::once_flag once;
	std::call_once(once, []() { llama_backend_init(); });
}

static std::atomic<int> nativeLlamaDecodeLogCounter{0};

struct SafeSingleSequenceBatch {
	llama_batch batch = {};
	bool valid = false;

	SafeSingleSequenceBatch(const std::vector<llama_token> &tokens, int32_t capacity)
	{
		if (tokens.empty() || tokens.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max()))
			return;
		const int32_t physicalCapacity = std::max<int32_t>(static_cast<int32_t>(tokens.size()), capacity);
		batch = llama_batch_init(physicalCapacity, 0, 1);
		if (!batch.token || !batch.pos || !batch.n_seq_id || !batch.seq_id)
			return;
		for (int32_t i = 0; i < static_cast<int32_t>(tokens.size()); ++i) {
			batch.token[i] = tokens[static_cast<size_t>(i)];
			batch.pos[i] = i;
			batch.n_seq_id[i] = 1;
			batch.seq_id[i][0] = 0;
			if (batch.logits)
				// llama.cpp's embedding example marks sequence tokens for output.
				// At least one true logit flag is required for pooled sequence output;
				// marking all tokens mirrors common_batch_add(..., true).
				batch.logits[i] = 1;
		}
		batch.n_tokens = static_cast<int32_t>(tokens.size());
		valid = true;
	}

	~SafeSingleSequenceBatch()
	{
		if (valid)
			llama_batch_free(batch);
	}

	SafeSingleSequenceBatch(const SafeSingleSequenceBatch &) = delete;
	SafeSingleSequenceBatch &operator=(const SafeSingleSequenceBatch &) = delete;
};
#endif

} // namespace

class LlamaCppEmbeddingProvider::Engine {
public:
#ifdef CLIP_CROPPER_WITH_LLAMA_CPP
	~Engine()
	{
		if (ctx)
			llama_free(ctx);
		if (model)
			llama_model_free(model);
	}

	bool load(const LlamaCppEmbeddingProviderOptions &options, const QString &path, QString *error)
	{
		ensureBackendInitialized();
		QElapsedTimer timer;
		timer.start();

		llama_model_params modelParams = llama_model_default_params();
		modelParams.n_gpu_layers = options.gpuLayers;
		modelParams.use_mmap = true;

		const QByteArray pathBytes = QFileInfo(path).absoluteFilePath().toUtf8();
		model = llama_model_load_from_file(pathBytes.constData(), modelParams);
		if (!model) {
			if (error)
				*error = QStringLiteral("native_llama_embedding_model_load_failed:%1").arg(path);
			return false;
		}

		llama_context_params ctxParams = llama_context_default_params();
		ctxParams.n_ctx = static_cast<uint32_t>(std::max(256, options.contextSize));
		// Match llama.cpp's embedding server/CLI invariants for Qwen3 embedding models:
		// non-causal embeddings require n_batch == n_ubatch, but large ubatches can
		// crash some Qwen3 GGUF/backends while building KV attention indices. Keep the
		// physical batch conservative and truncate each text to that capacity.
		ctxParams.n_batch = static_cast<uint32_t>(std::clamp(options.batchSize, 64, 512));
		ctxParams.n_ubatch = ctxParams.n_batch;
		// Run one prompt per native context call. This mirrors our usage pattern and
		// avoids exercising llama.cpp multi-slot/unified-KV code paths inside OBS.
		ctxParams.n_seq_max = 1;
		ctxParams.n_outputs_max = ctxParams.n_batch;
		ctxParams.kv_unified = false;
		ctxParams.offload_kqv = false;
		ctxParams.n_threads = effectiveThreadCount(options.threads);
		ctxParams.n_threads_batch = effectiveThreadCount(options.batchThreads);
		ctxParams.embeddings = true;
		// Qwen's llama.cpp usage for Qwen3-Embedding explicitly uses --pooling last.
		// Do not force non-causal attention here; let llama.cpp/model metadata choose
		// the attention path for this decoder-style embedding GGUF.
		ctxParams.pooling_type = LLAMA_POOLING_TYPE_LAST;
		ctxParams.abort_callback = llamaAbortCallback;
		ctxParams.abort_callback_data = const_cast<std::function<bool()> *>(&options.cancellationCallback);

		ctx = llama_init_from_model(model, ctxParams);
		if (!ctx) {
			if (error)
				*error = QStringLiteral("native_llama_embedding_context_init_failed:%1").arg(path);
			return false;
		}

		llama_set_embeddings(ctx, true);
		nEmbd = std::max(0, llama_model_n_embd_out(model));
		if (nEmbd <= 0)
			nEmbd = std::max(0, llama_model_n_embd(model));
		blog(LOG_INFO,
		     "[clip-cropper] Native llama.cpp embedding model loaded. path=%s nEmbd=%d nCtx=%u nBatch=%u nUBatch=%u nSeqMax=%u threads=%d batchThreads=%d gpuLayers=%d pooling=last attention=default offloadKqv=false kvUnified=false elapsedMs=%lld",
		     path.toUtf8().constData(), nEmbd, llama_n_ctx(ctx), llama_n_batch(ctx), llama_n_ubatch(ctx),
		     llama_n_seq_max(ctx), llama_n_threads(ctx), llama_n_threads_batch(ctx), options.gpuLayers,
		     static_cast<long long>(timer.elapsed()));
		return nEmbd > 0;
	}

	SemanticEmbedding embedPrepared(const QString &text, const LlamaCppEmbeddingProviderOptions &options, QString *error)
	{
		SemanticEmbedding result;
		if (!ctx || !model || text.trimmed().isEmpty() || nEmbd <= 0)
			return result;
		if (options.cancellationCallback && options.cancellationCallback())
			return result;

		const QByteArray bytes = text.toUtf8();
		const llama_vocab *vocab = llama_model_get_vocab(model);
		int tokenCount = llama_tokenize(vocab, bytes.constData(), static_cast<int32_t>(bytes.size()), nullptr, 0,
						 true, true);
		if (tokenCount < 0)
			tokenCount = -tokenCount;
		if (tokenCount <= 0)
			return result;

		std::vector<llama_token> tokens(static_cast<size_t>(tokenCount));
		int actual = llama_tokenize(vocab, bytes.constData(), static_cast<int32_t>(bytes.size()), tokens.data(),
						 static_cast<int32_t>(tokens.size()), true, true);
		if (actual < 0)
			actual = -actual;
		if (actual <= 0)
			return result;
		tokens.resize(static_cast<size_t>(actual));

		const uint32_t nBatch = std::max<uint32_t>(1, llama_n_batch(ctx));
		if (tokens.size() > static_cast<size_t>(nBatch)) {
			// Keep the opening and tail; for review candidates the first and last
			// parts are the most important for hook/resolution similarity. The cap
			// must not exceed llama_n_batch(ctx), otherwise llama_decode may receive
			// a larger logical batch than the context was initialized to handle.
			const size_t keepHead = static_cast<size_t>(nBatch / 2);
			const size_t keepTail = static_cast<size_t>(nBatch) - keepHead;
			std::vector<llama_token> clipped;
			clipped.reserve(static_cast<size_t>(nBatch));
			clipped.insert(clipped.end(), tokens.begin(), tokens.begin() + static_cast<long long>(keepHead));
			clipped.insert(clipped.end(), tokens.end() - static_cast<long long>(keepTail), tokens.end());
			tokens = std::move(clipped);
		}

		SafeSingleSequenceBatch batch(tokens, static_cast<int32_t>(llama_n_batch(ctx)));
		if (!batch.valid) {
			if (error)
				*error = QStringLiteral("native_llama_embedding_batch_init_failed");
			return result;
		}
		// Clear previous sequence/KV state before embedding. This mirrors the
		// upstream llama.cpp embedding example and avoids stale KV positions when the
		// same context is reused for many independent review candidates.
		llama_memory_clear(llama_get_memory(ctx), true);

		const int decodeLogIndex = nativeLlamaDecodeLogCounter.fetch_add(1, std::memory_order_relaxed);
		if (decodeLogIndex < 8) {
			blog(LOG_INFO,
			     "[clip-cropper] Native llama.cpp embedding decode starting. tokens=%d nCtx=%u nBatch=%u nUBatch=%u nSeqMax=%u pooling=last attention=default kvUnified=false offloadKqv=false",
			     batch.batch.n_tokens, llama_n_ctx(ctx), llama_n_batch(ctx), llama_n_ubatch(ctx),
			     llama_n_seq_max(ctx));
		}

		// Use llama_decode, matching llama.cpp's embedding example. With Qwen3
		// embedding we use decoder-style last pooling and leave attention on the
		// model/default path instead of forcing non-causal attention.
		const int rc = llama_decode(ctx, batch.batch);
		if (rc != 0) {
			if (error)
				*error = QStringLiteral("native_llama_embedding_decode_failed:%1").arg(rc);
			return result;
		}

		llama_synchronize(ctx);
		float *embedding = llama_get_embeddings_seq(ctx, 0);
		if (!embedding)
			embedding = llama_get_embeddings_ith(ctx, -1);
		if (!embedding) {
			if (error)
				*error = QStringLiteral("native_llama_embedding_missing_output");
			return result;
		}

		result.values.resize(nEmbd);
		for (int i = 0; i < nEmbd; ++i)
			result.values[i] = embedding[i];
		result.values = normalized(std::move(result.values));
		return result;
	}

	llama_model *model = nullptr;
	llama_context *ctx = nullptr;
	int nEmbd = 0;
#else
	bool load(const LlamaCppEmbeddingProviderOptions &, const QString &, QString *error)
	{
		if (error)
			*error = QStringLiteral("native_llama_cpp_backend_not_built");
		return false;
	}
	SemanticEmbedding embedPrepared(const QString &, const LlamaCppEmbeddingProviderOptions &, QString *error)
	{
		if (error)
			*error = QStringLiteral("native_llama_cpp_backend_not_built");
		return {};
	}
#endif
};

LlamaCppEmbeddingProvider::LlamaCppEmbeddingProvider(LlamaCppEmbeddingProviderOptions options)
	: options_(std::move(options))
{
}

LlamaCppEmbeddingProvider::~LlamaCppEmbeddingProvider() = default;

bool LlamaCppEmbeddingProvider::isAvailable() const
{
	return options_.enabled && ensureLoaded();
}

QString LlamaCppEmbeddingProvider::modelId() const
{
	if (!resolvedModelPath_.isEmpty())
		return QStringLiteral("llama.cpp:%1").arg(QFileInfo(resolvedModelPath_).fileName());
	return QStringLiteral("llama.cpp:%1").arg(options_.modelPathOrId);
}

QString LlamaCppEmbeddingProvider::resolvedModelPath() const
{
	QMutexLocker locker(&mutex_);
	return resolvedModelPath_;
}

QString LlamaCppEmbeddingProvider::lastError() const
{
	QMutexLocker locker(&mutex_);
	return lastError_;
}

SemanticEmbedding LlamaCppEmbeddingProvider::embed(const QString &text) const
{
	const QString input = preparedText(text);
	if (input.isEmpty() || !isAvailable())
		return {};

	SemanticEmbedding cached;
	if (cache_.tryGet(modelId(), input, &cached))
		return cached;

	QMutexLocker locker(&mutex_);
	if (!engine_)
		return {};
	QString error;
	SemanticEmbedding embedding = engine_->embedPrepared(input, options_, &error);
	if (!error.isEmpty())
		lastError_ = error;
	if (embedding.isValid())
		cache_.put(modelId(), input, embedding);
	return embedding;
}

QVector<SemanticEmbedding> LlamaCppEmbeddingProvider::embedBatch(const QVector<QString> &texts) const
{
	QVector<SemanticEmbedding> result;
	result.resize(texts.size());
	if (texts.isEmpty() || !isAvailable())
		return result;

	const QString cacheModelId = modelId();
	QVector<QString> pendingTexts;
	QVector<int> pendingIndexes;
	pendingTexts.reserve(static_cast<long long>(texts.size()));
	pendingIndexes.reserve(static_cast<long long>(texts.size()));

	for (int i = 0; i < static_cast<int>(texts.size()); ++i) {
		if (options_.cancellationCallback && options_.cancellationCallback())
			break;
		const QString input = preparedText(texts.at(i));
		if (input.isEmpty())
			continue;
		SemanticEmbedding cached;
		if (cache_.tryGet(cacheModelId, input, &cached)) {
			result[i] = cached;
			continue;
		}
		pendingTexts.append(input);
		pendingIndexes.append(i);
	}

	if (pendingTexts.isEmpty())
		return result;

	QVector<SemanticEmbedding> computed;
	QString error;
	{
		QMutexLocker locker(&mutex_);
		if (!engine_)
			return result;
		computed = runSafeLlamaCppBatch<QString, SemanticEmbedding>(
			pendingTexts, options_.maxBatchSize, options_.cancellationCallback,
			[this, &error](const QString &input, int) {
				if (!error.isEmpty())
					return SemanticEmbedding{};
				SemanticEmbedding embedding = engine_->embedPrepared(input, options_, &error);
				if (!error.isEmpty())
					lastError_ = error;
				return embedding;
			});
	}

	for (int i = 0; i < static_cast<int>(computed.size()) && i < static_cast<int>(pendingIndexes.size()); ++i) {
		const SemanticEmbedding &embedding = computed.at(i);
		if (!embedding.isValid())
			continue;
		const int resultIndex = pendingIndexes.at(i);
		if (resultIndex < 0 || resultIndex >= static_cast<int>(result.size()))
			continue;
		result[resultIndex] = embedding;
		cache_.put(cacheModelId, pendingTexts.at(i), embedding);
	}
	return result;
}

bool LlamaCppEmbeddingProvider::ensureLoaded() const
{
	QMutexLocker locker(&mutex_);
	if (available_)
		return true;
	if (loadAttempted_)
		return false;
	loadAttempted_ = true;

	resolvedModelPath_ = resolveLlamaCppModelPath(options_.modelPathOrId);
	if (resolvedModelPath_.isEmpty()) {
		lastError_ = QStringLiteral("native_llama_embedding_model_not_found:%1").arg(options_.modelPathOrId);
		const QStringList paths = llamaCppModelSearchPaths(options_.modelPathOrId);
		for (const QString &path : paths) {
			blog(LOG_INFO, "[clip-cropper] Native llama.cpp embedding model search path: %s",
			     path.toUtf8().constData());
		}
		blog(LOG_ERROR, "[clip-cropper] Native llama.cpp embedding model not found. model=%s",
		     options_.modelPathOrId.toUtf8().constData());
		return false;
	}

	engine_ = std::make_unique<Engine>();
	QString error;
	available_ = engine_->load(options_, resolvedModelPath_, &error);
	if (!available_) {
		lastError_ = error;
		blog(LOG_ERROR, "[clip-cropper] Native llama.cpp embedding backend unavailable. error=%s",
		     lastError_.toUtf8().constData());
		engine_.reset();
	}
	return available_;
}

QString LlamaCppEmbeddingProvider::preparedText(const QString &text) const
{
	QString value = text.simplified();
	if (options_.maxTextChars > 0 && value.size() > options_.maxTextChars)
		value = value.left(options_.maxTextChars);
	return value;
}

void LlamaCppEmbeddingProvider::setLastError(const QString &message) const
{
	QMutexLocker locker(&mutex_);
	lastError_ = message;
}
