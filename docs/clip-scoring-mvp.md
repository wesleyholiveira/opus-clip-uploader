# Clip scoring MVP

This module is the deterministic/NLP-ready foundation for local clip discovery. It keeps the expensive model layer out of the first pass: the plugin first builds structured candidates, scores them with cheap features, removes obvious noise, and only then should the next step plug embeddings/reranking/LLM judgement into the remaining finalists.

## Pipeline

```text
RecordingTranscript
  -> TranscriptIndex
  -> CandidateGenerator
  -> CheapClipScorer
  -> ClipRanker
  -> optional embedding/reranker/LLM judge later
```

## Responsibilities

- `TranscriptIndex`: indexed access to timestamped transcript segments, range text extraction, segment windows, and boundary signals such as silence before/after a range.
- `TextAnalysis`: shared lightweight NLP utilities: normalization, diacritic stripping, semantic buckets, lexical topic overlap, topic-shift detection, continuation detection, greeting/backlog noise detection, and viewer-question detection.
- `CandidateGenerator`: creates raw clip candidates from local cues, target anchors, context prelude, coarse sliding windows, explanation extension, and topic-shift trimming.
- `CheapClipScorer`: assigns deterministic numeric scores for duration, boundary quality, hook strength, emotional/advice/explanation/story/opinion/tutorial cues, viewer-response likelihood, topic continuity, noise, and keyword-based target evidence.
- `ClipRanker`: removes noise/weak candidates, sorts by final score, and deduplicates overlapping ranges.
- `ClipScoringPipeline`: orchestrates the MVP pipeline and returns structured candidates plus a concise debug summary.

## Current integration

`ViewerMessageFocusRangeResolver` is now intentionally small. It only:

1. resolves whether the active preset is `viewer_message_response`;
2. preserves the reliable `main_target` anchor path so a confident target still beats emotion;
3. delegates generic discovery/fallback candidate generation to `ClipScoringPipeline`.

This removes the old duplicate discovery engine from `viewer-message-focus-range.cpp` and makes candidate generation reusable by the next MVP step.

## Behavior preserved

Reliable target behavior is still conservative:

```text
explicit reliable target
  -> find target anchor
  -> focus selected range around target
  -> target wins over emotional cues
```

If the target is missing, generic, not explicitly requested, not found, or too weak, the resolver now falls back to structured candidate discovery:

```text
no/weak target
  -> ClipScoringPipeline
  -> candidate ranges for viewer-message discovery
```

## Next extension points

- Add `EmbeddingSemanticScorer` after `CheapClipScorer` and before `ClipRanker` to replace keyword-only target evidence with cosine similarity.
- Add `OnnxSentimentClassifier` inside `EmotionScorer` or as a new scorer beside `CheapClipScorer`.
- Add audio features such as pause, RMS energy, speech-rate shifts, and laughter markers to `ClipCandidateScores`.
- Add optional `LlmJudge` only for final ambiguous candidates.
- Add persisted per-segment feature cache if scoring becomes expensive for long transcripts.
