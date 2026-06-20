#pragma once

#include "curation/scoring/llama-server-embedding-provider.hpp"

#include <QString>

namespace Curation::Scoring {

inline constexpr const char *CONFIG_LOCAL_EMBEDDING_BACKEND = "local_embedding_backend";
inline constexpr const char *CONFIG_LOCAL_EMBEDDING_ENDPOINT = "local_embedding_endpoint";
inline constexpr const char *CONFIG_LOCAL_EMBEDDING_MODEL_ID = "local_embedding_model_id";
inline constexpr const char *CONFIG_LOCAL_EMBEDDING_TIMEOUT_MS = "local_embedding_timeout_ms";
inline constexpr const char *CONFIG_LOCAL_EMBEDDING_MAX_TEXT_CHARS = "local_embedding_max_text_chars";

inline constexpr const char *LOCAL_EMBEDDING_BACKEND_DISABLED = "disabled";
inline constexpr const char *LOCAL_EMBEDDING_BACKEND_LLAMA_SERVER = "llama_server";

QString localEmbeddingBackendFromConfig();
LlamaServerEmbeddingProviderOptions llamaServerEmbeddingOptionsFromConfig();

} // namespace Curation::Scoring
