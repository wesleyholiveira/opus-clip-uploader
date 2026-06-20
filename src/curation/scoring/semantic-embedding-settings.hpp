#pragma once

#include "curation/scoring/llama-server-embedding-provider.hpp"
#include "curation/scoring/llama-server-reranker-provider.hpp"

#include <QString>

namespace Curation::Scoring {

inline constexpr const char *CONFIG_LOCAL_EMBEDDING_BACKEND = "local_embedding_backend";
inline constexpr const char *CONFIG_LOCAL_EMBEDDING_ENDPOINT = "local_embedding_endpoint";
inline constexpr const char *CONFIG_LOCAL_EMBEDDING_MODEL_ID = "local_embedding_model_id";
inline constexpr const char *CONFIG_LOCAL_EMBEDDING_TIMEOUT_MS = "local_embedding_timeout_ms";
inline constexpr const char *CONFIG_LOCAL_EMBEDDING_MAX_TEXT_CHARS = "local_embedding_max_text_chars";
inline constexpr const char *CONFIG_LOCAL_RERANKER_BACKEND = "local_reranker_backend";
inline constexpr const char *CONFIG_LOCAL_RERANKER_ENDPOINT = "local_reranker_endpoint";
inline constexpr const char *CONFIG_LOCAL_RERANKER_MODEL_ID = "local_reranker_model_id";
inline constexpr const char *CONFIG_LOCAL_RERANKER_TIMEOUT_MS = "local_reranker_timeout_ms";
inline constexpr const char *CONFIG_LOCAL_RERANKER_MAX_TEXT_CHARS = "local_reranker_max_text_chars";

inline constexpr const char *LOCAL_EMBEDDING_BACKEND_DISABLED = "disabled";
inline constexpr const char *LOCAL_EMBEDDING_BACKEND_LLAMA_SERVER = "llama_server";
inline constexpr const char *LOCAL_RERANKER_BACKEND_DISABLED = "disabled";
inline constexpr const char *LOCAL_RERANKER_BACKEND_LLAMA_SERVER = "llama_server";

QString localEmbeddingBackendFromConfig();
QString localRerankerBackendFromConfig();
LlamaServerEmbeddingProviderOptions llamaServerEmbeddingOptionsFromConfig();
LlamaServerRerankerProviderOptions llamaServerRerankerOptionsFromConfig();

} // namespace Curation::Scoring
