# Clip scoring MVP

This module prepares the local, deterministic part of the curation pipeline. The goal is to avoid sending the whole transcript to an LLM just to discover where the good clips are.

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

- `TranscriptIndex`: provides indexed access to timestamped transcript segments, text extraction by range, and boundary signals such as silence before/after a range.
- `CandidateGenerator`: creates raw clip candidates from local cues and coarse sliding windows inside the selected/manual range.
- `CheapClipScorer`: assigns deterministic numeric scores for duration, boundary quality, hook strength, emotional/advice/explanation cues, viewer-response likelihood, topic continuity, and keyword-based target evidence.
- `ClipRanker`: sorts candidates, removes weak candidates, and deduplicates overlapping ranges.
- `ClipScoringPipeline`: orchestrates the MVP pipeline and returns structured candidates plus a concise debug summary.

## Current integration

`ViewerMessageFocusRangeResolver` now uses this pipeline for generic viewer-message candidate discovery when there is no reliable explicit target. The existing target-anchor path is preserved so that reliable `main_target` behavior remains stable.

## Next extension points

- Add `EmbeddingSemanticScorer` for real semantic similarity instead of keyword-only target evidence.
- Add `OnnxSentimentClassifier` for model-based emotion/sentiment classification.
- Add audio features such as pause, RMS energy, speech-rate shifts, and laughter markers.
- Add optional `LlmJudge` only for final ambiguous candidates.
