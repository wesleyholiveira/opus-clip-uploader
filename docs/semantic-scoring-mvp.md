# Clip scoring MVP: heuristics + embeddings + optional reranker

This refactor prepares the scorer for the first four MVP phases without moving the GPT prompt client/store into the new architecture yet.

## Phase 1: cheap candidate discovery

`CandidateGenerator`, `CheapClipScorer`, `TranscriptIndex` and `TextAnalysis` still own only cheap signals:

- candidate windows and cue-anchored ranges;
- duration and pause/boundary scores;
- obvious stream-operation/noise hints;
- weak lexical hints used only as fallback/debug evidence.

This layer should not be trusted as the semantic judge. It only narrows the search space.

## Phase 2: embedding semantic scorer

`SemanticEmbeddingProvider` is the boundary for local embedding models.

`SemanticClipScorer` consumes that interface and upgrades a `ClipCandidate` with semantic scores:

- `embeddingTarget` / `semanticTarget`;
- `semanticViewerMessage`;
- `semanticDirectAnswer`;
- `semanticNoise`;
- `semanticTopicShift`;
- embedding-based topic continuity.

When no provider is available, the pipeline remains deterministic and adds `semantic_embedding_unavailable` evidence instead of pretending that lexical NLP solved the semantic task.

## Phase 3: optional reranker

`SemanticReranker` is a second interface for query-vs-candidate ranking.

`SemanticRerankerStage` is optional and updates only final candidates when a reranker backend exists. It is intentionally separate from embeddings so a future Qwen3 reranker or llama.cpp-backed reranker can be plugged in without rewriting candidate generation.

## Phase 4: candidate quality gate

`CandidateQualityGate` decides whether a scored candidate is viable before final ranking.

It checks:

- minimum duration;
- minimum text size;
- noise/stream-management score;
- final score;
- viewer-response strength for `viewer_message_response`;
- semantic target match when semantic scoring is available and a reliable main target exists.

This is intentionally separate from `PromptQualityGate`. `PromptQualityGate` validates the final Opus prompt. `CandidateQualityGate` validates the clip candidate itself.

## Dependencies

`.gitmodules` now declares:

```ini
[submodule "deps/llama.cpp"]
	path = deps/llama.cpp
	url = https://github.com/ggml-org/llama.cpp
```

CMake recognizes this dependency through `CLIP_CROPPER_USE_LLAMA_CPP` and `CLIP_CROPPER_LLAMA_CPP_DIR`. The option is disabled by default because the concrete llama.cpp embedding/reranker provider is not wired in this step. Enable it only after initializing the submodule and adding the concrete backend.

## Intended next step

Add a concrete provider, likely backed by llama.cpp and a GGUF embedding model such as Qwen3-Embedding. The provider should implement:

```cpp
class SemanticEmbeddingProvider {
public:
    virtual bool isAvailable() const = 0;
    virtual QString modelId() const = 0;
    virtual SemanticEmbedding embed(const QString &text) const = 0;
};
```

After that, the existing scoring pipeline starts using real semantic scores without changing `CandidateGenerator`, `CheapClipScorer`, `CandidateQualityGate`, or the Opus prompt compiler.
