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

from candidate_snapshot_join import enrich_feedback_rows_with_snapshots, load_candidate_snapshot_index

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

STRUCTURED_FEEDBACK_FEATURES = [
    "userSaysHasBeginning", "userSaysHasDevelopment", "userSaysHasConclusion", "userSaysSingleTopic",
    "userSaysSmoothEnding", "userSaysGoodHook", "userSaysViewerCue", "userSaysGoodTopicBadBoundary",
    "userSaysIncompleteRecoverable", "userSaysBoundaryRecoverable", "userSaysBadTopic", "userSaysTopicShift",
    "userSaysStartsTooLate", "userSaysStartsTooEarly", "userSaysEndsTooEarly",
    "userSaysOverextended", "userSaysMetaNoise",
]

FEATURE_ORDER = SCORE_FEATURES + FEEDBACK_FEATURES + EVIDENCE_FEATURES + STRUCTURED_FEEDBACK_FEATURES

# The supervised ranker learns from generated candidate features.  An adjusted
# generated range is the range the user had to fix, so it must be a negative
# example for ranking.  The corrected user range is still used as a positive
# feedback signal for range-memory features, but it is not emitted as a positive
# supervised row here because it does not carry candidate/reranker features.
POSITIVE_DECISIONS = {"accepted", "approved_adjusted"}
NEGATIVE_DECISIONS = {"rejected", "adjusted"}
RANGE_MEMORY_POSITIVE_DECISIONS = {"accepted", "approved_adjusted", "added_by_user"}
SKIP_DECISIONS = {"removed_unrated", "ignored_diagnostic"}


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


def range_from_keys(row: dict[str, Any], start_key: str, end_key: str) -> tuple[float, float] | None:
    start = finite_float(row.get(start_key), math.nan)
    end = finite_float(row.get(end_key), math.nan)
    if not math.isfinite(start) or not math.isfinite(end) or end <= start:
        return None
    return start, end




def structured_feedback(row: dict[str, Any]) -> dict[str, Any]:
    feedback = row.get("explicit_structured_feedback")
    return feedback if isinstance(feedback, dict) else {}


def diagnostic_original_range_for_row(row: dict[str, Any]) -> tuple[float, float] | None:
    feedback = structured_feedback(row)
    # New runtime stores these at the top level and inside explicit_structured_feedback;
    # older diagnostic-feedback rows may only contain the nested copy.
    return (
        range_from_keys(row, "diagnostic_original_start_sec", "diagnostic_original_end_sec")
        or range_from_keys(feedback, "diagnostic_original_start_sec", "diagnostic_original_end_sec")
    )


def structured_reviewed_range_for_row(row: dict[str, Any]) -> tuple[float, float] | None:
    feedback = structured_feedback(row)
    return range_from_keys(feedback, "range_start_sec", "range_end_sec")


def feedback_bool(row: dict[str, Any], key: str) -> bool:
    feedback = structured_feedback(row)
    return feedback.get(key, row.get(f"feedback_{key}")) is True


def diagnostic_rejection_reason(row: dict[str, Any]) -> str:
    feedback = structured_feedback(row)
    return str(feedback.get("diagnostic_rejection_reason") or row.get("diagnostic_rejection_reason") or "")


def is_incomplete_viewer_arc(row: dict[str, Any]) -> bool:
    return "incomplete_viewer_arc" in diagnostic_rejection_reason(row)


def is_bad_topic_feedback(row: dict[str, Any]) -> bool:
    return str(row.get("generated_feedback_class", "")) == "bad_topic" or feedback_bool(row, "bad_topic")


def is_boundary_recoverable_feedback(row: dict[str, Any]) -> bool:
    if is_bad_topic_feedback(row):
        return False
    if str(row.get("generated_feedback_class", "")) == "good_topic_bad_boundary":
        return True
    if feedback_bool(row, "boundary_recoverable") or feedback_bool(row, "good_topic_bad_boundary"):
        return True
    if feedback_bool(row, "incomplete_but_recoverable"):
        return True
    return row.get("decision") in {"adjusted", "approved_adjusted"} and is_incomplete_viewer_arc(row)


def is_boundary_recoverable_only(row: dict[str, Any]) -> bool:
    decision = str(row.get("decision", "")).strip()
    return decision in {"rejected", "adjusted", "approved_adjusted"} and is_boundary_recoverable_feedback(row)


def is_ignored_or_weak_training_signal(row: dict[str, Any]) -> bool:
    feedback_class = str(row.get("generated_feedback_class", ""))
    return (
        str(row.get("decision", "")).strip() == "ignored_diagnostic"
        or str(row.get("explicit_review_decision", "")).strip() == "ignored_diagnostic"
        or feedback_class in {"ignored_diagnostic", "weak_negative"}
        or feedback_bool(row, "ignore_for_training")
        or feedback_bool(row, "weak_negative")
    )


def is_edited_approved_adjustment(row: dict[str, Any]) -> bool:
    if str(row.get("decision", "")).strip() != "approved_adjusted":
        return False
    return range_meaningfully_edited(diagnostic_original_range_for_row(row), user_range_for_row(row))


def is_default_no_marker_placeholder(row: dict[str, Any]) -> bool:
    decision = str(row.get("decision", "")).strip()
    if decision != "added_by_user":
        return False
    review_key = str(row.get("review_settings_key", ""))
    if not review_key.endswith(".no_markers") and ".no_markers" not in review_key:
        return False
    user_range = range_from_keys(row, "user_start_sec", "user_end_sec")
    if user_range is None:
        return False
    return abs(user_range[0]) <= 0.05 and abs(user_range[1] - 90.0) <= 0.25


def is_calibratable(row: dict[str, Any]) -> bool:
    if row.get("decision") in SKIP_DECISIONS:
        return False
    if is_ignored_or_weak_training_signal(row):
        return False
    if is_default_no_marker_placeholder(row):
        return False
    if row.get("event") == "review_rejected" and not row.get("explicit_review_decision"):
        return False
    return True


def label_for_row(row: dict[str, Any]) -> int | None:
    decision = str(row.get("decision", "")).strip()
    # A recoverable incomplete arc means the topic can be useful while the generated
    # boundaries are bad. Do not emit it as a pure ranker negative/positive row; keep
    # it for range memory and boundary calibration instead.
    if is_ignored_or_weak_training_signal(row):
        return None
    if is_boundary_recoverable_only(row):
        return None
    if decision == "approved_adjusted" and is_edited_approved_adjustment(row):
        return None
    if decision in POSITIVE_DECISIONS:
        return 1
    if decision in NEGATIVE_DECISIONS:
        return 0
    return None


def range_for_row(row: dict[str, Any]) -> tuple[float, float] | None:
    # Supervised rows represent generated candidates.  For diagnostic-feedback
    # rows where the user edited the review range, keep the generated/original
    # range as the supervised candidate and treat the edited range as user_range.
    decision = str(row.get("decision", "")).strip()
    if decision in {"adjusted", "approved_adjusted"}:
        original = diagnostic_original_range_for_row(row)
        if original is not None:
            return original
    return range_from_keys(row, "generated_start_sec", "generated_end_sec")


def user_range_for_row(row: dict[str, Any]) -> tuple[float, float] | None:
    explicit_user = range_from_keys(row, "user_start_sec", "user_end_sec")
    if explicit_user is not None:
        return explicit_user
    decision = str(row.get("decision", "")).strip()
    if decision in {"adjusted", "approved_adjusted"}:
        return structured_reviewed_range_for_row(row)
    return None


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


def range_meaningfully_edited(generated: tuple[float, float] | None, user: tuple[float, float] | None) -> bool:
    if generated is None or user is None:
        return False
    return (
        abs(generated[0] - user[0]) > 0.25
        or abs(generated[1] - user[1]) > 0.25
        or abs(duration(generated) - duration(user)) > 0.25
    )


def rounded_range_key(r: tuple[float, float], bucket_sec: float = 2.0) -> tuple[int, int]:
    return round(r[0] / bucket_sec), round(r[1] / bucket_sec)


def positive_semantic_dedupe_key(row: dict[str, Any]) -> tuple[str, str, str, tuple[int, int]] | None:
    decision = str(row.get("decision", "")).strip()
    if decision != "approved_adjusted":
        return None
    corrected = user_range_for_row(row)
    if corrected is None:
        return None
    return (*group_key(row), rounded_range_key(corrected))


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
        if not is_calibratable(row):
            continue
        line_no = int(row.get("__line_no", 0))
        decision = str(row.get("decision", "")).strip()
        generated = range_for_row(row)
        user = user_range_for_row(row)
        group = grouped[group_key(row)]

        if decision == "adjusted":
            # Runtime memory treats the generated range as the bad boundary and
            # the corrected user range as the positive target. Recoverable incomplete
            # arcs are not topic negatives; avoid using them as negative memory that
            # suppresses nearby corrected positives.
            if generated is not None and not is_boundary_recoverable_feedback(row):
                group["negative"].append((line_no, generated))
            if range_meaningfully_edited(generated, user):
                group["positive"].append((line_no, user))
            continue

        if decision in NEGATIVE_DECISIONS and generated is not None:
            if not is_boundary_recoverable_feedback(row):
                group["negative"].append((line_no, generated))
            continue

        if decision in RANGE_MEMORY_POSITIVE_DECISIONS:
            positive_range = user if user is not None else generated
            if positive_range is not None:
                group["positive"].append((line_no, positive_range))
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


def bool_feature(value: Any) -> float:
    return 1.0 if value is True else 0.0


def structured_feedback_features_for_row(row: dict[str, Any]) -> dict[str, float]:
    feedback = row.get("explicit_structured_feedback")
    if not isinstance(feedback, dict):
        feedback = {}
    return {
        "userSaysHasBeginning": bool_feature(row.get("feedback_has_beginning", feedback.get("has_beginning"))),
        "userSaysHasDevelopment": bool_feature(row.get("feedback_has_development", feedback.get("has_development"))),
        "userSaysHasConclusion": bool_feature(row.get("feedback_has_conclusion", feedback.get("has_conclusion"))),
        "userSaysSingleTopic": bool_feature(row.get("feedback_has_single_topic", feedback.get("has_single_topic"))),
        "userSaysSmoothEnding": bool_feature(row.get("feedback_has_smooth_ending", feedback.get("has_smooth_ending"))),
        "userSaysGoodHook": bool_feature(row.get("feedback_has_good_hook", feedback.get("has_good_hook"))),
        "userSaysViewerCue": bool_feature(row.get("feedback_has_viewer_cue", feedback.get("has_viewer_cue"))),
        "userSaysGoodTopicBadBoundary": bool_feature(row.get("feedback_good_topic_bad_boundary", feedback.get("good_topic_bad_boundary"))),
        "userSaysIncompleteRecoverable": bool_feature(row.get("feedback_incomplete_but_recoverable", feedback.get("incomplete_but_recoverable"))),
        "userSaysBoundaryRecoverable": bool_feature(row.get("feedback_boundary_recoverable", feedback.get("boundary_recoverable"))),
        "userSaysBadTopic": bool_feature(row.get("feedback_bad_topic", feedback.get("bad_topic"))),
        "userSaysTopicShift": bool_feature(row.get("feedback_has_topic_shift", feedback.get("has_topic_shift"))),
        "userSaysStartsTooLate": bool_feature(row.get("feedback_starts_too_late", feedback.get("starts_too_late"))),
        "userSaysStartsTooEarly": bool_feature(row.get("feedback_starts_too_early", feedback.get("starts_too_early"))),
        "userSaysEndsTooEarly": bool_feature(row.get("feedback_ends_too_early", feedback.get("ends_too_early"))),
        "userSaysOverextended": bool_feature(row.get("feedback_overextended_after_resolution", feedback.get("overextended_after_resolution"))),
        "userSaysMetaNoise": bool_feature(row.get("feedback_has_meta_noise", feedback.get("has_meta_noise"))),
    }


def features_for_row(row: dict[str, Any], group_signals: dict[str, list[tuple[int, tuple[float, float]]]]) -> dict[str, float]:
    scores = row.get("scores") if isinstance(row.get("scores"), dict) else {}
    features: dict[str, float] = {}
    for name in SCORE_FEATURES:
        value = scores.get(name, row.get("candidate_final_score") if name == "final" else 0.0)
        features[name] = normalise_feature(name, finite_float(value))
    evidence = evidence_text(row)
    features.update(feedback_features_for_row(row, group_signals))
    features.update(structured_feedback_features_for_row(row))
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
    emitted_positive_semantic_keys: set[tuple[str, str, str, tuple[int, int]]] = set()
    for row in selected:
        label = label_for_row(row)
        if label is None:
            continue
        dedupe_key = positive_semantic_dedupe_key(row)
        if label == 1 and dedupe_key is not None:
            if dedupe_key in emitted_positive_semantic_keys:
                continue
            emitted_positive_semantic_keys.add(dedupe_key)
        r = range_for_row(row)
        if r is None:
            continue
        features = features_for_row(row, signals_by_group[group_key(row)])
        dataset.append({
            "label": label,
            "weight": 1.35 if label == 1 else 1.10 if str(row.get("decision", "")).strip() == "adjusted" else 1.0,
            "features": features,
            "metadata": {
                "line_no": row.get("__line_no"),
                "preset": row.get("preset"),
                "decision": row.get("decision"),
                "label_reason": "adjusted_generated_range_is_negative" if row.get("decision") == "adjusted" else None,
                "generated_feedback_class": row.get("generated_feedback_class"),
                "diagnostic_rejection_reason": diagnostic_rejection_reason(row),
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
    parser.add_argument("--candidate-snapshot-path", type=Path, action="append", default=[],
                        help="candidate-snapshots.jsonl file or feedback directory used to enrich rows with original text/features/scores.")
    args = parser.parse_args()

    rows = load_jsonl(args.feedback_jsonl)
    snapshot_hits = 0
    if args.candidate_snapshot_path:
        snapshot_index = load_candidate_snapshot_index(args.candidate_snapshot_path)
        rows, snapshot_hits = enrich_feedback_rows_with_snapshots(rows, snapshot_index)
    dataset = build_dataset(rows, preset=args.preset or None)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8") as handle:
        for item in dataset:
            handle.write(json.dumps(item, ensure_ascii=False, sort_keys=True) + "\n")
    positives = sum(1 for item in dataset if item["label"] == 1)
    negatives = len(dataset) - positives
    selected = [row for row in rows if is_calibratable(row) and (not args.preset or str(row.get("preset") or "auto") == args.preset)]
    recoverable_excluded = sum(1 for row in selected if is_boundary_recoverable_only(row))
    ignored_or_weak_excluded = sum(1 for row in selected if is_ignored_or_weak_training_signal(row))
    print(
        f"Wrote {args.output} examples={len(dataset)} positives={positives} negatives={negatives} "
        f"recoverable_boundary_excluded={recoverable_excluded} "
        f"ignored_or_weak_training_excluded={ignored_or_weak_excluded} "
        f"snapshot_hits={snapshot_hits}"
    )
    return 0 if dataset else 1


if __name__ == "__main__":
    raise SystemExit(main())
