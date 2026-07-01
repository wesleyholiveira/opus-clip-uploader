#!/usr/bin/env python3
"""Conservatively migrate legacy `auto` feedback rows to a concrete training profile.

The plugin keeps boundary-feedback.jsonl as an append-only source of truth. This
script does not edit that file in-place by default. It writes an audit-friendly
migrated copy and a JSON report so profile fixes can be reviewed before use.
"""

from __future__ import annotations

import argparse
import json
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable

from candidate_snapshot_join import normalize_profile, row_profile

DEFAULT_EXPLANATION_TERMS = (
    "learning",
    "learn",
    "study",
    "studying",
    "german",
    "language",
    "vocabulary",
    "spaced repetition",
    "spaced_repetition",
    "flashcard",
    "flashcards",
    "anki",
    "method",
    "unlock method",
    "dolly",
    "sensei",
    "academic",
    "explain",
    "explanation",
    "lesson",
    "concept",
)

TEXT_FIELDS = (
    "video_file_name",
    "video_path",
    "main_target",
    "topic_keywords",
    "suggestion_summary",
    "candidate_text",
    "transcript_excerpt",
    "timed_text_preview",
    "text_preview",
    "evidence",
)

TRAINING_POSITIVE_DECISIONS = {"accepted", "approved_adjusted", "added_by_user"}
TRAINING_NEGATIVE_DECISIONS = {"rejected", "disliked"}
IGNORED_DECISIONS = {"ignored_diagnostic", "removed_unrated"}


def load_jsonl(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as handle:
        for line_no, line in enumerate(handle, 1):
            line = line.strip()
            if not line:
                continue
            try:
                item = json.loads(line)
            except json.JSONDecodeError as exc:
                raise SystemExit(f"Invalid JSON at {path}:{line_no}: {exc}") from exc
            if not isinstance(item, dict):
                raise SystemExit(f"Expected JSON object at {path}:{line_no}")
            item.setdefault("__line_no", line_no)
            rows.append(item)
    return rows


def write_jsonl(path: Path, rows: Iterable[dict[str, Any]]) -> int:
    path.parent.mkdir(parents=True, exist_ok=True)
    count = 0
    with path.open("w", encoding="utf-8") as handle:
        for row in rows:
            clean = {key: value for key, value in row.items() if not key.startswith("__")}
            handle.write(json.dumps(clean, ensure_ascii=False, sort_keys=True) + "\n")
            count += 1
    return count


def _flatten(value: Any) -> str:
    if value is None:
        return ""
    if isinstance(value, str):
        return value
    if isinstance(value, (int, float, bool)):
        return str(value)
    if isinstance(value, list):
        return " ".join(_flatten(item) for item in value)
    if isinstance(value, dict):
        return " ".join(f"{key} {_flatten(val)}" for key, val in value.items())
    return str(value)


def row_text(row: dict[str, Any]) -> str:
    return "\n".join(_flatten(row.get(field)) for field in TEXT_FIELDS).lower()


def decision_group(decision: str) -> str:
    if decision in TRAINING_POSITIVE_DECISIONS:
        return "training_positive"
    if decision in TRAINING_NEGATIVE_DECISIONS:
        return "training_negative"
    if decision in IGNORED_DECISIONS:
        return "ignored_or_unrated"
    return "other"


def confidence_for_row(row: dict[str, Any], *, terms: list[str], include_video_names: list[str]) -> tuple[bool, list[str]]:
    reasons: list[str] = []
    text = row_text(row)

    for name in include_video_names:
        if name and name.lower() in text:
            reasons.append(f"video_name:{name}")

    term_hits = [term for term in terms if term and term.lower() in text]
    if term_hits:
        reasons.append("terms:" + ",".join(term_hits[:8]))

    # Rows explicitly rejected by the viewer-message gate are good migration
    # candidates for explanation only when the surrounding content is educational.
    # Do not let these strings block migration; record them as context instead.
    if "missing_viewer_message_cue" in text or "incomplete_viewer_arc" in text:
        reasons.append("wrong_viewer_gate_context")

    return bool([reason for reason in reasons if reason.startswith("video_name:") or reason.startswith("terms:")]), reasons



def rewrite_wrong_viewer_gate_for_target(row: dict[str, Any], *, target_profile: str, reasons: list[str]) -> None:
    """Preserve original gate metadata but stop explanation rows from training as viewer-arc failures."""
    if normalize_profile(target_profile) == "viewer_message_response":
        return
    if "wrong_viewer_gate_context" not in reasons:
        return

    feedback = row.get("explicit_structured_feedback")
    if isinstance(feedback, dict):
        original = str(feedback.get("diagnostic_rejection_reason") or "")
        if "incomplete_viewer_arc" in original or "missing_viewer_message_cue" in original:
            row.setdefault("profile_migration_original_diagnostic_rejection_reason", original)
            feedback = dict(feedback)
            feedback["diagnostic_rejection_reason"] = "wrong_profile_viewer_gate"
            row["explicit_structured_feedback"] = feedback

    original_top = str(row.get("diagnostic_rejection_reason") or "")
    if "incomplete_viewer_arc" in original_top or "missing_viewer_message_cue" in original_top:
        row.setdefault("profile_migration_original_top_level_diagnostic_rejection_reason", original_top)
        row["diagnostic_rejection_reason"] = "wrong_profile_viewer_gate"

    feedback_class = str(row.get("generated_feedback_class") or "")
    if feedback_class == "incomplete_viewer_arc":
        row.setdefault("profile_migration_original_generated_feedback_class", feedback_class)
        decision = str(row.get("decision") or "")
        if decision in {"adjusted", "approved_adjusted"}:
            row["generated_feedback_class"] = "good_topic_bad_boundary"
        elif decision == "ignored_diagnostic":
            row["generated_feedback_class"] = "ignored_diagnostic"


def migrate_rows(
    rows: list[dict[str, Any]],
    *,
    from_profile: str,
    target_profile: str,
    terms: list[str],
    include_video_names: list[str],
    positive_only: bool,
    migrate_ignored: bool,
) -> tuple[list[dict[str, Any]], dict[str, Any], set[str]]:
    now = datetime.now(timezone.utc).isoformat(timespec="seconds")
    normalized_from = normalize_profile(from_profile)
    normalized_target = normalize_profile(target_profile)
    migrated: list[dict[str, Any]] = []
    migrated_snapshot_ids: set[str] = set()
    counters: Counter[str] = Counter()
    decision_counts: Counter[str] = Counter()
    original_profile_counts: Counter[str] = Counter(row_profile(row) for row in rows)
    migrated_by_decision: Counter[str] = Counter()
    migration_reasons: Counter[str] = Counter()

    for row in rows:
        current_profile = row_profile(row)
        decision = str(row.get("decision") or "")
        decision_counts[decision] += 1
        should_migrate = False
        reasons: list[str] = []

        if current_profile != normalized_from:
            counters["kept_not_from_profile"] += 1
        elif positive_only and decision not in TRAINING_POSITIVE_DECISIONS:
            counters["kept_not_positive"] += 1
        elif decision in IGNORED_DECISIONS and not migrate_ignored:
            counters["kept_ignored_or_unrated"] += 1
        else:
            should_migrate, reasons = confidence_for_row(row, terms=terms, include_video_names=include_video_names)
            if should_migrate:
                counters["migrated"] += 1
                migrated_by_decision[decision] += 1
                for reason in reasons:
                    migration_reasons[reason] += 1
            else:
                counters["kept_low_confidence"] += 1

        if not should_migrate:
            migrated.append(row)
            continue

        new_row = dict(row)
        original_preset = new_row.get("preset")
        original_training_profile = new_row.get("training_profile")
        new_row["profile_migration_version"] = 1
        new_row["profile_migration_timestamp_utc"] = now
        new_row["profile_migration_from_profile"] = current_profile
        new_row["profile_migration_to_profile"] = normalized_target
        new_row["profile_migration_original_preset"] = original_preset
        new_row["profile_migration_original_training_profile"] = original_training_profile
        new_row["profile_migration_reason"] = ";".join(reasons)
        rewrite_wrong_viewer_gate_for_target(new_row, target_profile=normalized_target, reasons=reasons)
        new_row["preset"] = normalized_target
        new_row["training_profile"] = normalized_target
        snapshot_id = str(new_row.get("candidate_snapshot_id") or "").strip()
        if snapshot_id:
            migrated_snapshot_ids.add(snapshot_id)
        migrated.append(new_row)

    report: dict[str, Any] = {
        "schema_version": 1,
        "generated_at_utc": now,
        "from_profile": normalized_from,
        "target_profile": normalized_target,
        "positive_only": positive_only,
        "migrate_ignored": migrate_ignored,
        "terms": terms,
        "include_video_names": include_video_names,
        "total_rows": len(rows),
        "original_profile_counts": dict(original_profile_counts),
        "decision_counts": dict(decision_counts),
        "counters": dict(counters),
        "migrated_by_decision": dict(migrated_by_decision),
        "migration_reasons": dict(migration_reasons),
        "migrated_candidate_snapshot_ids": len(migrated_snapshot_ids),
        "notes": [
            "The source JSONL is not edited in-place unless the caller explicitly overwrites it.",
            "Rows are migrated only when they are from the requested source profile and match educational/explanation evidence.",
            "Ignored diagnostics remain ignored for training; migrating them keeps profile auditability without making them positive labels.",
        ],
    }
    return migrated, report, migrated_snapshot_ids


def migrate_snapshots(
    snapshot_path: Path,
    *,
    migrated_snapshot_ids: set[str],
    target_profile: str,
    terms: list[str],
    include_video_names: list[str],
) -> tuple[list[dict[str, Any]], dict[str, Any]]:
    rows = load_jsonl(snapshot_path)
    normalized_target = normalize_profile(target_profile)
    migrated: list[dict[str, Any]] = []
    counters: Counter[str] = Counter()
    for row in rows:
        snapshot_id = str(row.get("candidate_snapshot_id") or "").strip()
        should_migrate = bool(snapshot_id and snapshot_id in migrated_snapshot_ids)
        reasons: list[str] = []
        if not should_migrate and row_profile(row) == "auto":
            should_migrate, reasons = confidence_for_row(row, terms=terms, include_video_names=include_video_names)
        if not should_migrate:
            counters["kept"] += 1
            migrated.append(row)
            continue
        new_row = dict(row)
        new_row["profile_migration_version"] = 1
        new_row["profile_migration_to_profile"] = normalized_target
        new_row["profile_migration_original_preset"] = new_row.get("preset")
        new_row["profile_migration_original_training_profile"] = new_row.get("training_profile")
        new_row["profile_migration_reason"] = ";".join(reasons or ["referenced_by_migrated_feedback"])
        new_row["preset"] = normalized_target
        new_row["training_profile"] = normalized_target
        counters["migrated"] += 1
        migrated.append(new_row)
    report = {
        "snapshot_path": str(snapshot_path),
        "total_rows": len(rows),
        "counters": dict(counters),
    }
    return migrated, report


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Migrate confident legacy auto feedback rows to a concrete profile.")
    parser.add_argument("feedback", type=Path, help="Input boundary-feedback.jsonl")
    parser.add_argument("-o", "--output", type=Path, help="Output migrated feedback JSONL. Defaults to <input>.profile-migrated.jsonl")
    parser.add_argument("--report", type=Path, help="Output JSON report. Defaults to <output>.report.json")
    parser.add_argument("--from-profile", default="auto", help="Source profile to migrate from. Default: auto")
    parser.add_argument("--target-profile", default="explanation", help="Target profile. Default: explanation")
    parser.add_argument("--term", action="append", default=[], help="Additional term that marks a row as target-profile content")
    parser.add_argument("--include-video-name", action="append", default=[], help="Video-name substring to migrate from auto to target profile")
    parser.add_argument("--positive-only", action="store_true", help="Only migrate positive training decisions")
    parser.add_argument("--no-migrate-ignored", action="store_true", help="Keep ignored diagnostics/unrated rows as auto")
    parser.add_argument("--candidate-snapshot-path", action="append", type=Path, default=[], help="Optional candidate-snapshots JSONL to migrate alongside feedback")
    parser.add_argument("--snapshot-output", action="append", type=Path, default=[], help="Output path for each migrated snapshot file")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    feedback = args.feedback
    output = args.output or feedback.with_name(feedback.stem + ".profile-migrated" + feedback.suffix)
    report_path = args.report or output.with_suffix(output.suffix + ".report.json")
    terms = list(dict.fromkeys([*DEFAULT_EXPLANATION_TERMS, *args.term]))
    rows = load_jsonl(feedback)
    migrated_rows, report, migrated_snapshot_ids = migrate_rows(
        rows,
        from_profile=args.from_profile,
        target_profile=args.target_profile,
        terms=terms,
        include_video_names=args.include_video_name,
        positive_only=args.positive_only,
        migrate_ignored=not args.no_migrate_ignored,
    )
    count = write_jsonl(output, migrated_rows)

    snapshot_reports: list[dict[str, Any]] = []
    for index, snapshot_path in enumerate(args.candidate_snapshot_path):
        if index < len(args.snapshot_output):
            snapshot_output = args.snapshot_output[index]
        else:
            snapshot_output = snapshot_path.with_name(snapshot_path.stem + ".profile-migrated" + snapshot_path.suffix)
        snapshot_rows, snapshot_report = migrate_snapshots(
            snapshot_path,
            migrated_snapshot_ids=migrated_snapshot_ids,
            target_profile=args.target_profile,
            terms=terms,
            include_video_names=args.include_video_name,
        )
        write_jsonl(snapshot_output, snapshot_rows)
        snapshot_report["output"] = str(snapshot_output)
        snapshot_reports.append(snapshot_report)

    report.update({
        "input": str(feedback),
        "output": str(output),
        "written_rows": count,
        "snapshot_reports": snapshot_reports,
    })
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    print(f"Wrote migrated feedback: {output} rows={count}")
    print(f"Wrote migration report: {report_path}")
    if snapshot_reports:
        print(f"Migrated snapshot files: {len(snapshot_reports)}")
    print(json.dumps({"migrated": report["counters"].get("migrated", 0), "target_profile": report["target_profile"]}, ensure_ascii=False))


if __name__ == "__main__":
    main()
