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


def is_calibratable_feedback(row: dict[str, Any]) -> bool:
    if row.get("decision") == "removed_unrated":
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
    decisions = Counter(str(row.get("decision", "unknown")) for row in rows)

    adjusted_or_accepted = [row for row in rows_for_stats if row.get("decision") in {"accepted", "adjusted"}]
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
    start_late_rate = starts["starts_too_late"] / total
    ends_early_rate = ends["ends_too_early"] / total
    overextended_rate = ends["overextended_after_resolution"] / total
    reject_rate = decisions["rejected"] / total

    lookback = 60.0
    if start_late_errors:
        lookback = clamp(max(60.0, median(start_late_errors) + 35.0), 45.0, 120.0)

    lookahead = 42.0
    if ends_early_errors:
        lookahead = clamp(max(42.0, median(ends_early_errors) + 28.0), 24.0, 90.0)

    context_weight = clamp(1.0 + (start_late_rate * 0.55), 0.85, 1.70)
    hook_weight = clamp(1.0 + (start_late_rate * 0.18), 0.85, 1.35)
    development_weight = 1.0
    resolution_weight = clamp(1.0 + (ends_early_rate * 0.35) + (overextended_rate * 0.20), 0.85, 1.70)
    target_weight = clamp(1.0 + (reject_rate * 0.30), 0.90, 1.50)
    defect_penalty = clamp(1.0 + (overextended_rate * 0.65) + (reject_rate * 0.25), 0.95, 2.20)
    min_arc_confidence = clamp(0.30 + (reject_rate * 0.08) - (start_late_rate * 0.04), 0.24, 0.48)

    return {
        "records": len(rows),
        "calibratable_records": len(rows_for_stats),
        "decision_counts": dict(decisions),
        "start_error_counts": dict(starts),
        "end_error_counts": dict(ends),
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
    args = parser.parse_args()

    rows = load_jsonl(args.feedback_jsonl)
    by_preset: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        preset = str(row.get("preset") or "auto")
        by_preset[preset].append(row)

    calibration: dict[str, Any] = {
        "schema_version": 1,
        "source_feedback": str(args.feedback_jsonl),
        "total_records": len(rows),
        "note": "Generated by tools/calibrate_boundary_thresholds.py. Conservative threshold calibration; not ML training.",
    }

    if len(rows) >= args.min_records:
        calibration["default"] = profile_for_rows(rows)

    for preset, preset_rows in sorted(by_preset.items()):
        if len(preset_rows) >= args.min_records:
            calibration[preset] = profile_for_rows(preset_rows)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(calibration, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"Wrote {args.output} with {len(calibration) - 4} profile(s).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
