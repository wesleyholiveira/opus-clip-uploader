#!/usr/bin/env python3
"""Build a supervised candidate-ranking dataset from Clip Cropper feedback JSONL.

The output is JSONL with one training example per explicit review decision.  It is
used by tools/train_feedback_ranker.py, but it is also useful for inspection.
"""

from __future__ import annotations

import argparse
import json
import math
from collections import defaultdict
from pathlib import Path
from typing import Any, Iterable

SCORE_FEATURES = [
    "duration", "boundary", "hook", "emotional", "advice", "explanation", "story", "opinion", "tutorial",
    "viewerResponse", "coarseSemantic", "semanticTarget", "embeddingTarget", "semanticViewerMessage",
    "semanticDirectAnswer", "semanticNoise", "semanticTopicShift", "semanticClipValue", "semanticEmpathy",
    "semanticHook", "semanticResolution", "semanticMetaNoise", "semanticOpeningHook", "semanticOpeningMetaNoise",
    "semanticEndingResolution", "semanticEndingMetaNoise", "semanticEndingTopicShift", "topicContinuity",
    "semanticFocusContinuity", "reranker", "rerankerRaw", "rerankerBadClip", "rerankerOpeningDefect",
    "rerankerEndingDefect", "rerankerStructureDefect", "rerankerClipQualityMargin", "qualityGate", "noise",
    "pauseBeforeSec", "pauseAfterSec", "maxInternalPauseSec", "pauseBoundary", "arcOpening", "arcDevelopment",
    "arcConclusion", "arcBoundaryCleanliness", "arcTailRisk", "arcCompleteness", "final",
]

FEEDBACK_FEATURES = [
    "feedbackPositiveRange", "feedbackNegativeRange", "feedbackPositiveText", "feedbackNegativeText",
    "feedbackPositiveScore", "feedbackNegativeScore", "feedbackMargin", "feedbackPositiveDominates",
    "feedbackRawMarginPositive", "feedbackPositiveOverlap", "feedbackNegativeOverlap",
    "feedbackNegativeContamination", "feedbackExplainedByPositive",
]

EVIDENCE_FEATURES = [
    "evidenceViewerResponseCue", "evidenceCleanBoundary", "evidenceQualityPassed", "evidenceArcInvalid",
    "evidenceMissingViewerCue", "evidenceMultipleViewerMessages", "evidenceTopicShift", "evidencePositiveSeed",
    "evidencePatternSearch", "evidenceSemanticResolution", "evidenceSemanticHook", "evidenceRerankerStrongMatch",
    "evidenceMetaPrelude", "evidenceMusicOrIntro", "evidenceHardContextBlocker", "featureCoverage",
]

FEATURE_ORDER = SCORE_FEATURES + FEEDBACK_FEATURES + EVIDENCE_FEATURES

POSITIVE_DECISIONS = {"accepted", "adjusted", "added_by_user"}
NEGATIVE_DECISIONS = {"rejected"}
SKIP_DECISIONS = {"removed_unrated"}


def load_jsonl(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as handle:
        for line_no, line in enumerate(handle, 1):
            line = line.strip()
            if not line:
                continue
            try:
                row = json.loads(line)
                row["__line_no"] = line_no
                rows.append(row)
            except json.JSONDecodeError as exc:
                print(f"warning: skipping invalid JSON at line {line_no}: {exc}")
    return rows


def finite_float(value: Any, default: float = 0.0) -> float:
    if isinstance(value, bool):
        return default
    if isinstance(value, (int, float)) and math.isfinite(float(value)):
        return float(value)
    return default


def clamp(value: float, lo: float = 0.0, hi: float = 1.0) -> float:
    return max(lo, min(hi, value))


def normalise_feature(name: str, value: float) -> float:
    if name == "pauseBeforeSec" or name == "pauseAfterSec":
        return clamp(value / 12.0)
    if name == "maxInternalPauseSec":
        return clamp(value / 16.0)
    if name == "rerankerClipQualityMargin" or name == "feedbackMargin":
        return clamp((value + 1.0) * 0.5)
    return clamp(value)


def is_calibratable(row: dict[str, Any]) -> bool:
    if row.get("decision") in SKIP_DECISIONS:
        return False
    if row.get("event") == "review_rejected" and not row.get("explicit_review_decision"):
        return False
    return True


def label_for_row(row: dict[str, Any]) -> int | None:
    decision = str(row.get("decision", "")).strip()
    if decision in POSITIVE_DECISIONS:
        return 1
    if decision in NEGATIVE_DECISIONS:
        return 0
    return None


def range_for_row(row: dict[str, Any]) -> tuple[float, float] | None:
    start = finite_float(row.get("generated_start_sec"), math.nan)
    end = finite_float(row.get("generated_end_sec"), math.nan)
    if not math.isfinite(start) or not math.isfinite(end) or end <= start:
        return None
    return start, end


def overlap(a: tuple[float, float], b: tuple[float, float]) -> float:
    return max(0.0, min(a[1], b[1]) - max(a[0], b[0]))


def duration(r: tuple[float, float]) -> float:
    return max(0.0, r[1] - r[0])


def range_similarity(a: tuple[float, float], b: tuple[float, float]) -> float:
    da = duration(a)
    db = duration(b)
    if da <= 0 or db <= 0:
        return 0.0
    ov = overlap(a, b)
    union = max(a[1], b[1]) - min(a[0], b[0])
    iou = ov / union if union > 0 else 0.0
    coverage = ov / min(da, db) if min(da, db) > 0 else 0.0
    start_score = max(0.0, 1.0 - abs(a[0] - b[0]) / 18.0)
    end_score = max(0.0, 1.0 - abs(a[1] - b[1]) / 24.0)
    return clamp(iou * 0.48 + coverage * 0.26 + start_score * 0.16 + end_score * 0.10)


def negative_contaminates(candidate: tuple[float, float], negative: tuple[float, float]) -> bool:
    ov = overlap(candidate, negative)
    nd = duration(negative)
    cd = duration(candidate)
    if ov <= 0 or nd <= 0 or cd <= 0:
        return False
    center = negative[0] + nd * 0.5
    covers_core = ov >= min(10.0, nd * 0.55)
    contains_center = candidate[0] <= center <= candidate[1] and ov >= min(6.0, nd * 0.35)
    boundary_same = abs(candidate[0] - negative[0]) <= 4.0 or abs(candidate[1] - negative[1]) <= 5.0
    return covers_core or contains_center or (boundary_same and ov >= 5.0)


def positive_explains(candidate: tuple[float, float], positive: tuple[float, float]) -> bool:
    ov = overlap(candidate, positive)
    pd = duration(positive)
    cd = duration(candidate)
    if ov <= 0 or pd <= 0 or cd <= 0:
        return False
    positive_coverage = ov / pd
    candidate_coverage = ov / cd
    close_start = abs(candidate[0] - positive[0]) <= 5.0
    close_end = abs(candidate[1] - positive[1]) <= 6.0
    return positive_coverage >= 0.72 and candidate_coverage >= 0.58 and (close_start or close_end or cd <= pd * 1.35)


def group_key(row: dict[str, Any]) -> tuple[str, str, str]:
    return (
        str(row.get("preset") or "auto"),
        str(row.get("content_id") or row.get("video_id") or row.get("video_path") or ""),
        str(row.get("transcription_language") or ""),
    )


def evidence_text(row: dict[str, Any]) -> str:
    evidence = row.get("evidence")
    if isinstance(evidence, list):
        return "|".join(str(item) for item in evidence)
    return str(evidence or "")


def evidence_bool(evidence: str, *needles: str) -> float:
    return 1.0 if any(needle in evidence for needle in needles) else 0.0


def text_bool(text: str, *needles: str) -> bool:
    value = (text or "").lower()
    return any(needle in value for needle in needles)


def candidate_text(row: dict[str, Any]) -> str:
    chunks = []
    for key in ("candidate_text", "text", "anchor_text", "transcript_excerpt"):
        value = row.get(key)
        if isinstance(value, str) and value.strip():
            chunks.append(value)
    return " ".join(chunks)


def feature_coverage(features: dict[str, float]) -> float:
    core = [
        "semanticClipValue", "semanticDirectAnswer", "semanticViewerMessage", "semanticHook",
        "semanticResolution", "semanticEmpathy", "rerankerRaw", "rerankerClipQualityMargin",
        "arcCompleteness", "arcOpening", "arcDevelopment", "arcConclusion", "coarseSemantic",
        "viewerResponse", "hook", "advice", "explanation", "emotional",
    ]
    non_zero = sum(1 for name in core if abs(float(features.get(name, 0.0))) > 1e-6)
    return clamp(non_zero / max(1, len(core)))


def build_signals(rows: Iterable[dict[str, Any]]) -> dict[tuple[str, str, str], dict[str, list[tuple[int, tuple[float, float]]]]]:
    grouped: dict[tuple[str, str, str], dict[str, list[tuple[int, tuple[float, float]]]]] = defaultdict(lambda: {"positive": [], "negative": []})
    for row in rows:
        label = label_for_row(row)
        row_range = range_for_row(row)
        if label is None or row_range is None or not is_calibratable(row):
            continue
        bucket = "positive" if label == 1 else "negative"
        grouped[group_key(row)][bucket].append((int(row.get("__line_no", 0)), row_range))
    return grouped


def feedback_features_for_row(row: dict[str, Any], signals: dict[str, list[tuple[int, tuple[float, float]]]]) -> dict[str, float]:
    r = range_for_row(row)
    result = {name: 0.0 for name in FEEDBACK_FEATURES}
    if r is None:
        return result
    line_no = int(row.get("__line_no", 0))
    positives = [rng for idx, rng in signals.get("positive", []) if idx != line_no]
    negatives = [rng for idx, rng in signals.get("negative", []) if idx != line_no]
    pos_range = max((range_similarity(r, p) for p in positives), default=0.0)
    neg_range = max((range_similarity(r, n) for n in negatives), default=0.0)
    pos_overlap = max((overlap(r, p) for p in positives), default=0.0)
    neg_overlap = max((overlap(r, n) for n in negatives), default=0.0)
    neg_cont = any(negative_contaminates(r, n) for n in negatives)
    pos_exp = any(positive_explains(r, p) for p in positives)
    result.update({
        "feedbackPositiveRange": pos_range,
        "feedbackNegativeRange": neg_range,
        "feedbackPositiveText": 0.0,
        "feedbackNegativeText": 0.0,
        "feedbackPositiveScore": pos_range,
        "feedbackNegativeScore": neg_range,
        "feedbackMargin": clamp(((pos_range - neg_range) + 1.0) * 0.5),
        "feedbackPositiveDominates": 1.0 if (pos_range >= 0.26 and (pos_range - neg_range) >= 0.10) else 0.0,
        "feedbackRawMarginPositive": clamp(max(0.0, pos_range - neg_range)),
        "feedbackPositiveOverlap": clamp(pos_overlap / 60.0),
        "feedbackNegativeOverlap": clamp(neg_overlap / 60.0),
        "feedbackNegativeContamination": 1.0 if neg_cont else 0.0,
        "feedbackExplainedByPositive": 1.0 if pos_exp else 0.0,
    })
    return result


def features_for_row(row: dict[str, Any], group_signals: dict[str, list[tuple[int, tuple[float, float]]]]) -> dict[str, float]:
    scores = row.get("scores") if isinstance(row.get("scores"), dict) else {}
    features: dict[str, float] = {}
    for name in SCORE_FEATURES:
        value = scores.get(name, row.get("candidate_final_score") if name == "final" else 0.0)
        features[name] = normalise_feature(name, finite_float(value))
    evidence = evidence_text(row)
    features.update(feedback_features_for_row(row, group_signals))
    text = candidate_text(row)
    meta_prelude = evidence_bool(evidence, "meta_prelude", "drift+block")
    music_or_intro = 1.0 if (evidence_bool(evidence, "Música", "Musica") or text_bool(text, "música", "musica", "acertando o negócio", "acertando o negocio", "configurando", "abrindo live", "testando")) else 0.0
    has_viewer_cue = evidence_bool(evidence, "viewer_response_cue", "semantic_viewer_message_match", "targeted_viewer_message_cue", "flags=origin")
    missing_viewer = evidence_bool(evidence, "missing_viewer_message_cue", "exchange_arc_window_dfs_no_valid_origin_or_answer", "exchange_arc_no_valid_subspan")
    features.update({
        "evidenceViewerResponseCue": has_viewer_cue,
        "evidenceCleanBoundary": evidence_bool(evidence, "clean_boundary"),
        "evidenceQualityPassed": evidence_bool(evidence, "quality_gate_passed"),
        "evidenceArcInvalid": evidence_bool(evidence, "exchange_arc_state_machine:invalid"),
        "evidenceMissingViewerCue": evidence_bool(evidence, "missing_viewer_message_cue"),
        "evidenceMultipleViewerMessages": evidence_bool(evidence, "multiple_viewer_messages_inside_arc"),
        "evidenceTopicShift": evidence_bool(evidence, "topic_shift"),
        "evidencePositiveSeed": evidence_bool(evidence, "feedback_positive_exact_seed", "complete_viewer_arc_gate_passed_by_user_feedback"),
        "evidencePatternSearch": evidence_bool(evidence, "feedback_positive_pattern_search"),
        "evidenceSemanticResolution": evidence_bool(evidence, "semantic_resolution", "semantic_ending_resolution"),
        "evidenceSemanticHook": evidence_bool(evidence, "semantic_hook", "semantic_opening_hook"),
        "evidenceRerankerStrongMatch": evidence_bool(evidence, "reranker_strong_match"),
        "evidenceMetaPrelude": meta_prelude,
        "evidenceMusicOrIntro": music_or_intro,
        "evidenceHardContextBlocker": 1.0 if ((meta_prelude or music_or_intro) and (missing_viewer or not has_viewer_cue)) else 0.0,
    })
    features["featureCoverage"] = feature_coverage(features)
    return {name: float(features.get(name, 0.0)) for name in FEATURE_ORDER}


def build_dataset(rows: list[dict[str, Any]], preset: str | None = None) -> list[dict[str, Any]]:
    selected = [row for row in rows if is_calibratable(row)]
    if preset:
        selected = [row for row in selected if str(row.get("preset") or "auto") == preset]
    signals_by_group = build_signals(selected)
    dataset: list[dict[str, Any]] = []
    for row in selected:
        label = label_for_row(row)
        if label is None:
            continue
        r = range_for_row(row)
        if r is None:
            continue
        features = features_for_row(row, signals_by_group[group_key(row)])
        dataset.append({
            "label": label,
            "weight": 1.35 if label == 1 else 1.0,
            "features": features,
            "metadata": {
                "line_no": row.get("__line_no"),
                "preset": row.get("preset"),
                "decision": row.get("decision"),
                "explicit_review_decision": row.get("explicit_review_decision"),
                "video_file_name": row.get("video_file_name"),
                "start_sec": r[0],
                "end_sec": r[1],
                "candidate_source": row.get("candidate_source"),
            },
        })
    return dataset


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("feedback_jsonl", type=Path)
    parser.add_argument("-o", "--output", type=Path, required=True)
    parser.add_argument("--preset", default="viewer_message_response")
    args = parser.parse_args()

    rows = load_jsonl(args.feedback_jsonl)
    dataset = build_dataset(rows, preset=args.preset or None)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8") as handle:
        for item in dataset:
            handle.write(json.dumps(item, ensure_ascii=False, sort_keys=True) + "\n")
    positives = sum(1 for item in dataset if item["label"] == 1)
    negatives = len(dataset) - positives
    print(f"Wrote {args.output} examples={len(dataset)} positives={positives} negatives={negatives}")
    return 0 if dataset else 1


if __name__ == "__main__":
    raise SystemExit(main())
