#!/usr/bin/env python3
"""Generate boundary-calibration.json from Clip Cropper feedback JSONL.

This is intentionally conservative. It does not train a model; it produces calibration
weights and time windows that the C++ boundary refiner can load at runtime.
"""

from __future__ import annotations

import argparse
import json
import math
from collections import Counter, defaultdict
from pathlib import Path
from statistics import median
from typing import Any

from candidate_snapshot_join import normalize_profile, row_profile


def load_jsonl(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def finite_number(value: Any) -> float | None:
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)) and math.isfinite(float(value)):
        return float(value)
    return None


def clamp(value: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, value))


def range_from_keys(row: dict[str, Any], start_key: str, end_key: str) -> tuple[float, float] | None:
    start = finite_number(row.get(start_key))
    end = finite_number(row.get(end_key))
    if start is None or end is None or end <= start:
        return None
    return start, end


def is_default_no_marker_placeholder(row: dict[str, Any]) -> bool:
    if str(row.get("decision", "")).strip() != "added_by_user":
        return False
    review_key = str(row.get("review_settings_key", ""))
    if not review_key.endswith(".no_markers") and ".no_markers" not in review_key:
        return False
    user_range = range_from_keys(row, "user_start_sec", "user_end_sec")
    if user_range is None:
        return False
    return abs(user_range[0]) <= 0.05 and abs(user_range[1] - 90.0) <= 0.25


def structured_feedback(row: dict[str, Any]) -> dict[str, Any]:
    feedback = row.get("explicit_structured_feedback")
    return feedback if isinstance(feedback, dict) else {}


def feedback_bool(row: dict[str, Any], key: str) -> bool:
    feedback = structured_feedback(row)
    value = feedback.get(key, row.get(f"feedback_{key}"))
    return value is True


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


def is_ignored_or_weak_training_signal(row: dict[str, Any]) -> bool:
    feedback_class = str(row.get("generated_feedback_class", ""))
    return (
        row.get("decision") == "ignored_diagnostic"
        or row.get("explicit_review_decision") == "ignored_diagnostic"
        or feedback_class in {"ignored_diagnostic", "weak_negative"}
        or feedback_bool(row, "ignore_for_training")
        or feedback_bool(row, "weak_negative")
    )


def is_calibratable_feedback(row: dict[str, Any]) -> bool:
    if row.get("decision") in {"removed_unrated", "ignored_diagnostic"}:
        return False
    if is_ignored_or_weak_training_signal(row):
        return False
    if is_default_no_marker_placeholder(row):
        return False
    # Older builds wrote event=review_rejected when the user merely canceled the dialog.
    # Without an explicit thumbs decision, those rows are ambiguous and should not
    # influence weights or thresholds.
    if row.get("event") == "review_rejected" and not row.get("explicit_review_decision"):
        return False
    return True


def profile_for_rows(rows: list[dict[str, Any]]) -> dict[str, Any]:
    calibratable_rows = [row for row in rows if is_calibratable_feedback(row)]
    rows_for_stats = calibratable_rows or rows
    starts = Counter(str(row.get("start_error_type", "unknown")) for row in rows_for_stats)
    ends = Counter(str(row.get("end_error_type", "unknown")) for row in rows_for_stats)
    decisions = Counter(str(row.get("decision", "unknown")) for row in rows_for_stats)

    adjusted_or_accepted = [row for row in rows_for_stats if row.get("decision") in {"accepted", "adjusted", "approved_adjusted"}]
    start_late_errors = [
        abs(float(row["start_error_sec"]))
        for row in adjusted_or_accepted
        if row.get("start_error_type") == "starts_too_late" and finite_number(row.get("start_error_sec")) is not None
    ]
    ends_early_errors = [
        abs(float(row["end_error_sec"]))
        for row in adjusted_or_accepted
        if row.get("end_error_type") == "ends_too_early" and finite_number(row.get("end_error_sec")) is not None
    ]
    overextended_errors = [
        abs(float(row["end_error_sec"]))
        for row in adjusted_or_accepted
        if row.get("end_error_type") == "overextended_after_resolution" and finite_number(row.get("end_error_sec")) is not None
    ]

    total = max(1, len(rows_for_stats))
    recoverable_boundary_rows = [row for row in rows_for_stats if is_boundary_recoverable_feedback(row)]
    bad_topic_rows = [row for row in rows_for_stats if is_bad_topic_feedback(row)]
    incomplete_viewer_arc_rows = [row for row in rows_for_stats if is_incomplete_viewer_arc(row)]
    ignored_or_weak_rows = [row for row in rows if is_ignored_or_weak_training_signal(row)]
    pure_rejected_rows = [
        row for row in rows_for_stats
        if row.get("decision") == "rejected" and not is_boundary_recoverable_feedback(row)
    ]

    start_late_rate = starts["starts_too_late"] / total
    ends_early_rate = ends["ends_too_early"] / total
    overextended_rate = ends["overextended_after_resolution"] / total
    recoverable_boundary_rate = len(recoverable_boundary_rows) / total
    reject_rate = len(pure_rejected_rows) / total

    lookback = 60.0
    if start_late_errors:
        lookback = clamp(max(60.0, median(start_late_errors) + 35.0), 45.0, 120.0)

    lookahead = 42.0
    if ends_early_errors:
        lookahead = clamp(max(42.0, median(ends_early_errors) + 28.0), 24.0, 90.0)

    context_weight = clamp(1.0 + (start_late_rate * 0.55) + (recoverable_boundary_rate * 0.18), 0.85, 1.70)
    hook_weight = clamp(1.0 + (start_late_rate * 0.18), 0.85, 1.35)
    development_weight = 1.0
    resolution_weight = clamp(1.0 + (ends_early_rate * 0.35) + (overextended_rate * 0.20), 0.85, 1.70)
    target_weight = clamp(1.0 + (reject_rate * 0.30), 0.90, 1.50)
    defect_penalty = clamp(1.0 + (overextended_rate * 0.65) + (reject_rate * 0.25), 0.95, 2.20)
    min_arc_confidence = clamp(0.30 + (reject_rate * 0.08) - (start_late_rate * 0.04) - (recoverable_boundary_rate * 0.06), 0.24, 0.48)

    return {
        "records": len(rows),
        "calibratable_records": len(rows_for_stats),
        "decision_counts": dict(decisions),
        "start_error_counts": dict(starts),
        "end_error_counts": dict(ends),
        "recoverable_boundary_records": len(recoverable_boundary_rows),
        "ignored_or_weak_training_records": len(ignored_or_weak_rows),
        "bad_topic_records": len(bad_topic_rows),
        "incomplete_viewer_arc_records": len(incomplete_viewer_arc_rows),
        "pure_rejected_records": len(pure_rejected_rows),
        "max_question_lookback_sec": round(lookback, 2),
        "lookahead_sec": round(lookahead, 2),
        "context_weight": round(context_weight, 4),
        "hook_weight": round(hook_weight, 4),
        "development_weight": round(development_weight, 4),
        "resolution_weight": round(resolution_weight, 4),
        "target_weight": round(target_weight, 4),
        "defect_penalty": round(defect_penalty, 4),
        "min_arc_confidence": round(min_arc_confidence, 4),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("feedback_jsonl", type=Path)
    parser.add_argument("-o", "--output", type=Path, required=True)
    parser.add_argument("--min-records", type=int, default=15)
    parser.add_argument("--profile", help="Optional training profile/preset id. When provided, write a profile-local calibration with only that profile's rows.")
    args = parser.parse_args()

    rows = load_jsonl(args.feedback_jsonl)
    usable_rows = [row for row in rows if not is_default_no_marker_placeholder(row)]
    requested_profile = normalize_profile(args.profile) if args.profile else None
    if requested_profile:
        usable_rows = [row for row in usable_rows if row_profile(row) == requested_profile]

    by_preset: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in usable_rows:
        preset = row_profile(row)
        by_preset[preset].append(row)

    calibration: dict[str, Any] = {
        "schema_version": 1,
        "source_feedback": str(args.feedback_jsonl),
        "total_records": len(rows),
        "selected_records": len(usable_rows),
        "profile": requested_profile,
        "note": "Generated by tools/calibrate_boundary_thresholds.py. Conservative threshold calibration; not ML training.",
    }

    if len(usable_rows) >= args.min_records:
        default_profile = profile_for_rows(usable_rows)
        calibration["default"] = default_profile
        if requested_profile:
            calibration["profile"] = default_profile

    for preset, preset_rows in sorted(by_preset.items()):
        if len(preset_rows) >= args.min_records:
            calibration[preset] = profile_for_rows(preset_rows)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(calibration, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    profile_keys = [key for key, value in calibration.items() if isinstance(value, dict)]
    print(f"Wrote {args.output} with {len(profile_keys)} profile object(s).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
