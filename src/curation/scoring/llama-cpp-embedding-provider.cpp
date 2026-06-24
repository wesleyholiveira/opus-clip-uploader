#include "curation/scoring/llama-cpp-embedding-provider.hpp"

#include "curation/scoring/llama-cpp-model-resolver.hpp"

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

#if defined(_MSC_VER)
#include <excpt.h>
#endif

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

#if defined(_MSC_VER)
static int safeLlamaDecodeRaw(llama_context *ctx, llama_batch *batch)
{
	__try {
		return llama_decode(ctx, *batch);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return INT_MIN;
	}
}
#endif

static int safeLlamaDecode(llama_context *ctx, llama_batch &batch, QString *error)
{
#if defined(_MSC_VER)
	const int rc = safeLlamaDecodeRaw(ctx, &batch);
	if (rc == INT_MIN && error)
		*error = QStringLiteral("native_llama_embedding_decode_access_violation");
	return rc;
#else
	(void)error;
	return llama_decode(ctx, batch);
#endif
}

struct NativeEmbeddingSequence {
	std::vector<llama_token> tokens;
	int resultIndex = -1;
};

struct SafeMultiSequenceBatch {
	llama_batch batch = {};
	bool valid = false;

	SafeMultiSequenceBatch(const QVector<NativeEmbeddingSequence> &sequences, int32_t capacity)
	{
		if (sequences.isEmpty())
			return;
		int64_t totalTokens = 0;
		for (const NativeEmbeddingSequence &sequence : sequences) {
			if (sequence.tokens.empty())
				return;
			totalTokens += static_cast<int64_t>(sequence.tokens.size());
			if (totalTokens > std::numeric_limits<int32_t>::max())
				return;
		}
		const int32_t physicalCapacity = std::max<int32_t>(static_cast<int32_t>(totalTokens), capacity);
		const int32_t sequenceCapacity = std::max<int32_t>(1, static_cast<int32_t>(sequences.size()));
		batch = llama_batch_init(physicalCapacity, 0, sequenceCapacity);
		if (!batch.token || !batch.pos || !batch.n_seq_id || !batch.seq_id)
			return;

		int32_t cursor = 0;
		for (int32_t seq = 0; seq < static_cast<int32_t>(sequences.size()); ++seq) {
			const std::vector<llama_token> &tokens = sequences.at(seq).tokens;
			for (int32_t pos = 0; pos < static_cast<int32_t>(tokens.size()); ++pos) {
				batch.token[cursor] = tokens[static_cast<size_t>(pos)];
				batch.pos[cursor] = pos;
				batch.n_seq_id[cursor] = 1;
				batch.seq_id[cursor][0] = seq;
				if (batch.logits)
					// For pooled embedding output we only need one output slot per
					// sequence. Marking only the last token avoids asking llama.cpp to
					// materialize per-token embeddings for every token in every text.
					batch.logits[cursor] = (pos + 1 == static_cast<int32_t>(tokens.size())) ? 1 : 0;
				++cursor;
			}
		}
		batch.n_tokens = cursor;
		valid = cursor > 0;
	}

	~SafeMultiSequenceBatch()
	{
		if (valid)
			llama_batch_free(batch);
	}

	SafeMultiSequenceBatch(const SafeMultiSequenceBatch &) = delete;
	SafeMultiSequenceBatch &operator=(const SafeMultiSequenceBatch &) = delete;
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
		// Run a small real native batch per decode. This keeps the crash-prone
		// unified-KV/multi-slot paths disabled, but allows several independent
		// sequences to share one llama_decode call when their tokens fit into
		// n_batch. Keep the cap conservative inside OBS.
		// Qwen3 embedding is stable in OBS with single-sequence decode. Multi-sequence
		// decode is useful, but crash-prone in some llama.cpp/CUDA builds when the
		// batch fans out too far. Start with a conservative native micro-batch of two
		// sequences and use unified KV for the multi-sequence path.
		const int nativeSequenceBatch = std::clamp(options.maxBatchSize, 1, 2);
		const bool unifiedKvForNativeBatch = nativeSequenceBatch > 1;
		ctxParams.n_seq_max = static_cast<uint32_t>(nativeSequenceBatch);
		ctxParams.n_outputs_max = ctxParams.n_seq_max;
		ctxParams.kv_unified = unifiedKvForNativeBatch;
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
		     "[clip-cropper] Native llama.cpp embedding model loaded. path=%s nEmbd=%d nCtx=%u nBatch=%u nUBatch=%u nSeqMax=%u threads=%d batchThreads=%d gpuLayers=%d pooling=last attention=default offloadKqv=false kvUnified=%s nativeBatchSequences=%d elapsedMs=%lld",
		     path.toUtf8().constData(), nEmbd, llama_n_ctx(ctx), llama_n_batch(ctx), llama_n_ubatch(ctx),
		     llama_n_seq_max(ctx), llama_n_threads(ctx), llama_n_threads_batch(ctx), options.gpuLayers,
		     unifiedKvForNativeBatch ? "true" : "false", nativeSequenceBatch,
		     static_cast<long long>(timer.elapsed()));
		return nEmbd > 0;
	}

	std::vector<llama_token> tokenizePrepared(const QString &text) const
	{
		std::vector<llama_token> tokens;
		if (!model || text.trimmed().isEmpty())
			return tokens;
		const QByteArray bytes = text.toUtf8();
		const llama_vocab *vocab = llama_model_get_vocab(model);
		int tokenCount = llama_tokenize(vocab, bytes.constData(), static_cast<int32_t>(bytes.size()), nullptr,
						0, true, true);
		if (tokenCount < 0)
			tokenCount = -tokenCount;
		if (tokenCount <= 0)
			return tokens;

		tokens.resize(static_cast<size_t>(tokenCount));
		int actual = llama_tokenize(vocab, bytes.constData(), static_cast<int32_t>(bytes.size()), tokens.data(),
					    static_cast<int32_t>(tokens.size()), true, true);
		if (actual < 0)
			actual = -actual;
		if (actual <= 0) {
			tokens.clear();
			return tokens;
		}
		tokens.resize(static_cast<size_t>(actual));
		return tokens;
	}

	void clipTokensToBatch(std::vector<llama_token> &tokens) const
	{
		const uint32_t nBatch = std::max<uint32_t>(1, llama_n_batch(ctx));
		if (tokens.size() <= static_cast<size_t>(nBatch))
			return;
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

	void decodeSequenceBatch(const QVector<NativeEmbeddingSequence> &sequences, QVector<SemanticEmbedding> &result,
				 const LlamaCppEmbeddingProviderOptions &options, QString *error)
	{
		if (sequences.isEmpty() || !ctx || fatalDecodeError)
			return;
		SafeMultiSequenceBatch batch(sequences, static_cast<int32_t>(llama_n_batch(ctx)));
		if (!batch.valid) {
			if (error)
				*error = QStringLiteral("native_llama_embedding_batch_init_failed");
			return;
		}

		// Clear previous sequence/KV state before embedding. This mirrors the
		// upstream llama.cpp embedding example and avoids stale KV positions when the
		// same context is reused for many independent review candidates.
		llama_memory_clear(llama_get_memory(ctx), true);

		const int decodeLogIndex = nativeLlamaDecodeLogCounter.fetch_add(1, std::memory_order_relaxed);
		if (decodeLogIndex < 12) {
			blog(LOG_INFO,
			     "[clip-cropper] Native llama.cpp embedding batch decode starting. texts=%d tokens=%d nCtx=%u nBatch=%u nUBatch=%u nSeqMax=%u pooling=last attention=default kvUnified=%s offloadKqv=false",
			     static_cast<int>(sequences.size()), batch.batch.n_tokens, llama_n_ctx(ctx),
			     llama_n_batch(ctx), llama_n_ubatch(ctx), llama_n_seq_max(ctx),
			     llama_n_seq_max(ctx) > 1 ? "true" : "false");
		}

		// Use llama_decode, matching llama.cpp's embedding example. With Qwen3
		// embedding we use decoder-style last pooling and leave attention on the
		// model/default path instead of forcing non-causal attention.
		QString decodeError;
		const int rc = safeLlamaDecode(ctx, batch.batch, &decodeError);
		if (rc == std::numeric_limits<int>::min()) {
			fatalDecodeError = true;
			const QString message =
				decodeError.isEmpty() ? QStringLiteral("native_llama_embedding_decode_access_violation")
						      : decodeError;
			if (error)
				*error = message;
			blog(LOG_ERROR,
			     "[clip-cropper] Native llama.cpp embedding decode trapped a fatal native access violation. texts=%d tokens=%d. Disabling native embedding for this run to keep OBS alive. error=%s",
			     static_cast<int>(sequences.size()), batch.batch.n_tokens, message.toUtf8().constData());
			return;
		}
		if (rc != 0) {
			if (error)
				*error = QStringLiteral("native_llama_embedding_decode_failed:%1").arg(rc);
			return;
		}

		llama_synchronize(ctx);
		for (int seq = 0; seq < static_cast<int>(sequences.size()); ++seq) {
			const int resultIndex = sequences.at(seq).resultIndex;
			if (resultIndex < 0 || resultIndex >= static_cast<int>(result.size()))
				continue;
			float *embedding = llama_get_embeddings_seq(ctx, seq);
			if (!embedding)
				embedding = llama_get_embeddings_ith(ctx, seq);
			if (!embedding && sequences.size() == 1)
				embedding = llama_get_embeddings_ith(ctx, -1);
			if (!embedding)
				continue;
			SemanticEmbedding value;
			value.values.resize(nEmbd);
			for (int i = 0; i < nEmbd; ++i)
				value.values[i] = embedding[i];
			value.values = normalized(std::move(value.values));
			result[resultIndex] = value;
		}
	}

	QVector<SemanticEmbedding> embedPreparedBatch(const QVector<QString> &texts,
						      const LlamaCppEmbeddingProviderOptions &options, QString *error)
	{
		QVector<SemanticEmbedding> result;
		result.resize(texts.size());
		if (!ctx || !model || texts.isEmpty() || nEmbd <= 0)
			return result;
		if (options.cancellationCallback && options.cancellationCallback())
			return result;

		const uint32_t maxBatchTokens = std::max<uint32_t>(1, llama_n_batch(ctx));
		const int maxBatchSequences = std::max(1, std::min(static_cast<int>(llama_n_seq_max(ctx)),
								   std::clamp(options.maxBatchSize, 1, 8)));

		QVector<NativeEmbeddingSequence> pending;
		pending.reserve(maxBatchSequences);
		int pendingTokens = 0;

		auto flushPending = [&]() {
			if (pending.isEmpty())
				return;
			decodeSequenceBatch(pending, result, options, error);
			pending.clear();
			pendingTokens = 0;
		};

		for (int i = 0; i < static_cast<int>(texts.size()); ++i) {
			if (options.cancellationCallback && options.cancellationCallback())
				break;
			std::vector<llama_token> tokens = tokenizePrepared(texts.at(i));
			if (tokens.empty())
				continue;
			clipTokensToBatch(tokens);
			if (tokens.empty())
				continue;
			const int tokenCount = static_cast<int>(tokens.size());
			if (!pending.isEmpty() && (pending.size() >= maxBatchSequences ||
						   pendingTokens + tokenCount > static_cast<int>(maxBatchTokens))) {
				flushPending();
				if (error && !error->isEmpty())
					return result;
			}

			NativeEmbeddingSequence sequence;
			sequence.tokens = std::move(tokens);
			sequence.resultIndex = i;
			pending.append(std::move(sequence));
			pendingTokens += tokenCount;
		}
		flushPending();
		return result;
	}

	SemanticEmbedding embedPrepared(const QString &text, const LlamaCppEmbeddingProviderOptions &options,
					QString *error)
	{
		QVector<QString> texts;
		texts.append(text);
		const QVector<SemanticEmbedding> batch = embedPreparedBatch(texts, options, error);
		return batch.isEmpty() ? SemanticEmbedding{} : batch.first();
	}

	llama_model *model = nullptr;
	llama_context *ctx = nullptr;
	int nEmbd = 0;
	bool fatalDecodeError = false;
#else
	bool load(const LlamaCppEmbeddingProviderOptions &, const QString &, QString *error)
	{
		if (error)
			*error = QStringLiteral("native_llama_cpp_backend_not_built");
		return false;
	}
	QVector<SemanticEmbedding> embedPreparedBatch(const QVector<QString> &texts,
						      const LlamaCppEmbeddingProviderOptions &, QString *error)
	{
		if (error)
			*error = QStringLiteral("native_llama_cpp_backend_not_built");
		QVector<SemanticEmbedding> result;
		result.resize(texts.size());
		return result;
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

	QVector<SemanticEmbedding> embeddings;
	QString error;
	{
		QMutexLocker locker(&mutex_);
		if (!engine_)
			return result;
		embeddings = engine_->embedPreparedBatch(pendingTexts, options_, &error);
		if (!error.isEmpty())
			lastError_ = error;
	}

	for (int i = 0; i < static_cast<int>(embeddings.size()) && i < static_cast<int>(pendingIndexes.size()); ++i) {
		const SemanticEmbedding &embedding = embeddings.at(i);
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
