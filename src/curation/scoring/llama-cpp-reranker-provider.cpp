#include "curation/scoring/llama-cpp-reranker-provider.hpp"

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

class LlamaCppRerankerProvider::Engine {
public:
#ifdef CLIP_CROPPER_WITH_LLAMA_CPP
	~Engine()
	{
		if (ctx)
			llama_free(ctx);
		if (model)
			llama_model_free(model);
	}

	bool load(const LlamaCppRerankerProviderOptions &options, const QString &path, QString *error)
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
				*error = QStringLiteral("native_llama_reranker_model_load_failed:%1").arg(path);
			return false;
		}

		llama_context_params ctxParams = llama_context_default_params();
		ctxParams.n_ctx = static_cast<uint32_t>(std::max(512, options.contextSize));
		// Match llama.cpp's embedding server/CLI invariants for Qwen3 reranker models:
		// non-causal rank pooling requires n_batch == n_ubatch. Keep the physical
		// batch conservative to avoid crashes while building KV attention indices.
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
		// Rank pooling is required for Qwen3 reranker. Do not force non-causal
		// attention; let llama.cpp/model metadata choose the stable path.
		ctxParams.pooling_type = LLAMA_POOLING_TYPE_RANK;
		ctxParams.abort_callback = llamaAbortCallback;
		ctxParams.abort_callback_data = const_cast<std::function<bool()> *>(&options.cancellationCallback);

		ctx = llama_init_from_model(model, ctxParams);
		if (!ctx) {
			if (error)
				*error = QStringLiteral("native_llama_reranker_context_init_failed:%1").arg(path);
			return false;
		}
		llama_set_embeddings(ctx, true);
		nClassOutputs = std::max<uint32_t>(1, llama_model_n_cls_out(model));
		blog(LOG_INFO,
		     "[clip-cropper] Native llama.cpp reranker model loaded. path=%s clsOut=%u nCtx=%u nBatch=%u nUBatch=%u nSeqMax=%u threads=%d batchThreads=%d gpuLayers=%d pooling=rank attention=default offloadKqv=false kvUnified=false elapsedMs=%lld",
		     path.toUtf8().constData(), nClassOutputs, llama_n_ctx(ctx), llama_n_batch(ctx),
		     llama_n_ubatch(ctx), llama_n_seq_max(ctx), llama_n_threads(ctx), llama_n_threads_batch(ctx),
		     options.gpuLayers, static_cast<long long>(timer.elapsed()));
		return true;
	}

	double scorePrepared(const QString &pairText, const LlamaCppRerankerProviderOptions &options, QString *error)
	{
		if (!ctx || !model || pairText.trimmed().isEmpty())
			return 0.0;
		if (options.cancellationCallback && options.cancellationCallback())
			return 0.0;

		const QByteArray bytes = pairText.toUtf8();
		const llama_vocab *vocab = llama_model_get_vocab(model);
		int tokenCount = llama_tokenize(vocab, bytes.constData(), static_cast<int32_t>(bytes.size()), nullptr,
						0, true, true);
		if (tokenCount < 0)
			tokenCount = -tokenCount;
		if (tokenCount <= 0)
			return 0.0;

		std::vector<llama_token> tokens(static_cast<size_t>(tokenCount));
		int actual = llama_tokenize(vocab, bytes.constData(), static_cast<int32_t>(bytes.size()), tokens.data(),
					    static_cast<int32_t>(tokens.size()), true, true);
		if (actual < 0)
			actual = -actual;
		if (actual <= 0)
			return 0.0;
		tokens.resize(static_cast<size_t>(actual));
		const uint32_t nBatch = std::max<uint32_t>(1, llama_n_batch(ctx));
		if (tokens.size() > static_cast<size_t>(nBatch))
			tokens.resize(static_cast<size_t>(nBatch));

		SafeSingleSequenceBatch batch(tokens, static_cast<int32_t>(llama_n_batch(ctx)));
		if (!batch.valid) {
			if (error)
				*error = QStringLiteral("native_llama_reranker_batch_init_failed");
			return 0.0;
		}
		// Clear previous sequence/KV state before embedding. This mirrors the
		// upstream llama.cpp embedding example and avoids stale KV positions when the
		// same context is reused for many independent review candidates.
		llama_memory_clear(llama_get_memory(ctx), true);

		const int decodeLogIndex = nativeLlamaDecodeLogCounter.fetch_add(1, std::memory_order_relaxed);
		if (decodeLogIndex < 8) {
			blog(LOG_INFO,
			     "[clip-cropper] Native llama.cpp reranker decode starting. tokens=%d nCtx=%u nBatch=%u nUBatch=%u nSeqMax=%u pooling=rank attention=default kvUnified=false offloadKqv=false",
			     batch.batch.n_tokens, llama_n_ctx(ctx), llama_n_batch(ctx), llama_n_ubatch(ctx),
			     llama_n_seq_max(ctx));
		}

		// Use llama_decode, matching llama.cpp's embedding/rank example. With Qwen3
		// reranker we use rank pooling and leave attention on the model/default path
		// instead of forcing non-causal attention.
		const int rc = llama_decode(ctx, batch.batch);
		if (rc != 0) {
			if (error)
				*error = QStringLiteral("native_llama_reranker_decode_failed:%1").arg(rc);
			return 0.0;
		}
		llama_synchronize(ctx);
		float *rank = llama_get_embeddings_seq(ctx, 0);
		if (!rank)
			rank = llama_get_embeddings_ith(ctx, -1);
		if (!rank) {
			if (error)
				*error = QStringLiteral("native_llama_reranker_missing_rank_output");
			return 0.0;
		}
		return rank[0];
	}

	llama_model *model = nullptr;
	llama_context *ctx = nullptr;
	uint32_t nClassOutputs = 1;
#else
	bool load(const LlamaCppRerankerProviderOptions &, const QString &, QString *error)
	{
		if (error)
			*error = QStringLiteral("native_llama_cpp_backend_not_built");
		return false;
	}
	double scorePrepared(const QString &, const LlamaCppRerankerProviderOptions &, QString *error)
	{
		if (error)
			*error = QStringLiteral("native_llama_cpp_backend_not_built");
		return 0.0;
	}
#endif
};

LlamaCppRerankerProvider::LlamaCppRerankerProvider(LlamaCppRerankerProviderOptions options)
	: options_(std::move(options))
{
}

LlamaCppRerankerProvider::~LlamaCppRerankerProvider() = default;

bool LlamaCppRerankerProvider::isAvailable() const
{
	return options_.enabled && ensureLoaded();
}

QString LlamaCppRerankerProvider::modelId() const
{
	if (!resolvedModelPath_.isEmpty())
		return QStringLiteral("llama.cpp:%1").arg(QFileInfo(resolvedModelPath_).fileName());
	return QStringLiteral("llama.cpp:%1").arg(options_.modelPathOrId);
}

QString LlamaCppRerankerProvider::resolvedModelPath() const
{
	QMutexLocker locker(&mutex_);
	return resolvedModelPath_;
}

QString LlamaCppRerankerProvider::lastError() const
{
	QMutexLocker locker(&mutex_);
	return lastError_;
}

double LlamaCppRerankerProvider::score(const QString &query, const QString &candidateText) const
{
	if (!isAvailable())
		return 0.0;

	const QString pair = preparedPairText(query, candidateText);
	if (pair.isEmpty())
		return 0.0;
	QMutexLocker locker(&mutex_);
	QString error;
	const double raw = engine_ ? engine_->scorePrepared(pair, options_, &error) : 0.0;
	if (!error.isEmpty())
		lastError_ = error;
	return normalizeRankScore(raw);
}

QVector<double> LlamaCppRerankerProvider::scoreBatch(const QString &query, const QVector<QString> &candidateTexts) const
{
	QVector<double> scores;
	scores.resize(candidateTexts.size());
	if (candidateTexts.isEmpty() || !isAvailable())
		return scores;

	QVector<QString> pairs;
	pairs.reserve(candidateTexts.size());
	QVector<int> resultIndexes;
	resultIndexes.reserve(candidateTexts.size());
	for (int i = 0; i < static_cast<int>(candidateTexts.size()); ++i) {
		if (options_.cancellationCallback && options_.cancellationCallback())
			break;
		const QString pair = preparedPairText(query, candidateTexts.at(i));
		if (pair.isEmpty())
			continue;
		pairs.append(pair);
		resultIndexes.append(i);
	}

	QString error;
	QVector<double> computed;
	{
		QMutexLocker locker(&mutex_);
		if (!engine_)
			return scores;
		computed = runSafeLlamaCppBatch<QString, double>(
			pairs, options_.maxBatchSize, options_.cancellationCallback,
			[this, &error](const QString &pair, int) {
				if (!error.isEmpty())
					return 0.0;
				const double raw = engine_->scorePrepared(pair, options_, &error);
				if (!error.isEmpty()) {
					lastError_ = error;
					return 0.0;
				}
				return normalizeRankScore(raw);
			});
	}

	for (int i = 0; i < computed.size() && i < resultIndexes.size(); ++i) {
		const int resultIndex = resultIndexes.at(i);
		if (resultIndex >= 0 && resultIndex < scores.size())
			scores[resultIndex] = computed.at(i);
	}
	return scores;
}

bool LlamaCppRerankerProvider::ensureLoaded() const
{
	QMutexLocker locker(&mutex_);
	if (available_)
		return true;
	if (loadAttempted_)
		return false;
	loadAttempted_ = true;

	resolvedModelPath_ = resolveLlamaCppModelPath(options_.modelPathOrId);
	if (resolvedModelPath_.isEmpty()) {
		lastError_ = QStringLiteral("native_llama_reranker_model_not_found:%1").arg(options_.modelPathOrId);
		const QStringList paths = llamaCppModelSearchPaths(options_.modelPathOrId);
		for (const QString &path : paths) {
			blog(LOG_INFO, "[clip-cropper] Native llama.cpp reranker model search path: %s",
			     path.toUtf8().constData());
		}
		blog(LOG_ERROR, "[clip-cropper] Native llama.cpp reranker model not found. model=%s",
		     options_.modelPathOrId.toUtf8().constData());
		return false;
	}

	engine_ = std::make_unique<Engine>();
	QString error;
	available_ = engine_->load(options_, resolvedModelPath_, &error);
	if (!available_) {
		lastError_ = error;
		blog(LOG_ERROR, "[clip-cropper] Native llama.cpp reranker backend unavailable. error=%s",
		     lastError_.toUtf8().constData());
		engine_.reset();
	}
	return available_;
}

QString LlamaCppRerankerProvider::preparedPairText(const QString &query, const QString &candidateText) const
{
	QString candidate = candidateText.simplified();
	if (options_.maxTextChars > 0 && candidate.size() > options_.maxTextChars)
		candidate = candidate.left(options_.maxTextChars);
	QString prompt = QStringLiteral("Query: %1\nDocument: %2").arg(query.simplified(), candidate);
	if (options_.maxTextChars > 0 && prompt.size() > options_.maxTextChars + query.size() + 32)
		prompt = prompt.left(options_.maxTextChars + query.size() + 32);
	return prompt;
}

double LlamaCppRerankerProvider::normalizeRankScore(double rawScore)
{
	if (rawScore >= 0.0 && rawScore <= 1.0)
		return std::clamp(rawScore, 0.0, 1.0);
	return std::clamp(1.0 / (1.0 + std::exp(-rawScore)), 0.0, 1.0);
}
