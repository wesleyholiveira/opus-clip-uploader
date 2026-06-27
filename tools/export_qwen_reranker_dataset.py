#!/usr/bin/env python3
"""Export grouped Qwen3 Reranker fine-tuning data from review feedback.

Output format is intentionally simple JSONL:
  {"query": str, "positive": [str], "negative": [str], "metadata": {...}}

This is an offline training/export step. The OBS plugin should only consume the
fine-tuned model artifact after it is trained and evaluated.
"""

from __future__ import annotations

import argparse
import json
from collections import defaultdict
from pathlib import Path
from typing import Any

from build_feedback_ranker_dataset import (
    candidate_text,
    diagnostic_rejection_reason,
    group_key,
    is_calibratable,
    is_default_no_marker_placeholder,
    is_ignored_or_weak_training_signal,
    load_jsonl,
)

from training_common import load_transcript_indexes, range_text_from_transcripts
from candidate_snapshot_join import enrich_feedback_rows_with_snapshots, load_candidate_snapshot_index

POSITIVE_DECISIONS = {"accepted", "approved_adjusted", "added_by_user"}
NEGATIVE_DECISIONS = {"rejected", "adjusted"}


def clean_text(value: str, max_chars: int) -> str:
    text = " ".join((value or "").split())
    return text[:max_chars].strip()


def query_for_group(rows: list[dict[str, Any]]) -> str:
    first = rows[0]
    preset = str(first.get("preset") or "viewer_message_response")
    target = str(first.get("main_target") or first.get("review_main_target") or "").strip()
    language = str(first.get("transcription_language") or first.get("source_language") or "auto")
    return (
        f"Preset: {preset}\n"
        f"Target: {target or 'use the active preset intent'}\n"
        f"Language: {language}\n"
        "Editorial criteria: rank self-contained short-form clips with a clear beginning, "
        "one subject, useful development, natural resolution, smooth ending, and no topic mixing. "
        "Prefer the corrected/accepted clip over candidates that start late, start too early, end early, "
        "overextend after the resolution, include meta/setup noise, or mix another viewer message."
    )


def row_metadata(row: dict[str, Any]) -> dict[str, Any]:
    return {
        "line_no": row.get("__line_no"),
        "decision": row.get("decision"),
        "preset": row.get("preset"),
        "video_file_name": row.get("video_file_name"),
        "content_id": row.get("content_id"),
        "candidate_source": row.get("candidate_source"),
        "generated_feedback_class": row.get("generated_feedback_class"),
        "diagnostic_rejection_reason": diagnostic_rejection_reason(row),
    }


def export_groups(rows: list[dict[str, Any]], *, min_text_chars: int, max_text_chars: int, min_negatives: int, transcripts: dict[str, list[dict[str, Any]]] | None = None) -> list[dict[str, Any]]:
    grouped: dict[tuple[str, str, str], list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        if not is_calibratable(row) or is_ignored_or_weak_training_signal(row) or is_default_no_marker_placeholder(row):
            continue
        text = clean_text(candidate_text(row), max_text_chars)
        if len(text) < min_text_chars:
            text = clean_text(range_text_from_transcripts(row, transcripts or {}), max_text_chars)
        if len(text) < min_text_chars:
            continue
        row = dict(row)
        row["__qwen_text"] = text
        grouped[group_key(row)].append(row)

    output: list[dict[str, Any]] = []
    for key, group_rows in grouped.items():
        positives: list[dict[str, Any]] = []
        negatives: list[dict[str, Any]] = []
        seen_positive: set[str] = set()
        seen_negative: set[str] = set()
        for row in group_rows:
            decision = str(row.get("decision") or "").strip()
            text = row["__qwen_text"]
            if decision in POSITIVE_DECISIONS and text not in seen_positive:
                positives.append(row)
                seen_positive.add(text)
            elif decision in NEGATIVE_DECISIONS and text not in seen_negative:
                negatives.append(row)
                seen_negative.add(text)
        if not positives or len(negatives) < min_negatives:
            continue
        output.append({
            "query": query_for_group(group_rows),
            "positive": [row["__qwen_text"] for row in positives],
            "negative": [row["__qwen_text"] for row in negatives],
            "metadata": {
                "group_key": list(key),
                "preset": group_rows[0].get("preset"),
                "video_file_name": group_rows[0].get("video_file_name"),
                "content_id": group_rows[0].get("content_id"),
                "positive_count": len(positives),
                "negative_count": len(negatives),
                "positive_rows": [row_metadata(row) for row in positives],
                "negative_rows": [row_metadata(row) for row in negatives],
            },
        })
    return output


def export_pairs(groups: list[dict[str, Any]]) -> list[dict[str, Any]]:
    pairs: list[dict[str, Any]] = []
    for group in groups:
        for text in group["positive"]:
            pairs.append({"query": group["query"], "text": text, "label": 1, "metadata": group["metadata"]})
        for text in group["negative"]:
            pairs.append({"query": group["query"], "text": text, "label": 0, "metadata": group["metadata"]})
    return pairs


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("feedback_jsonl", type=Path)
    parser.add_argument("-o", "--output", type=Path, required=True)
    parser.add_argument("--format", choices=("grouped", "pairs"), default="grouped")
    parser.add_argument("--min-text-chars", type=int, default=40)
    parser.add_argument("--max-text-chars", type=int, default=4096)
    parser.add_argument("--min-negatives", type=int, default=1)
    parser.add_argument("--transcript-path", type=Path, action="append", default=[], help="Transcript JSON/JSONL file or directory used to reconstruct candidate text when feedback rows do not contain text.")
    parser.add_argument("--candidate-snapshot-path", type=Path, action="append", default=[],
                        help="candidate-snapshots.jsonl file or feedback directory used to enrich rows with original text/features/scores before transcript fallback.")
    args = parser.parse_args()

    rows = load_jsonl(args.feedback_jsonl)
    snapshot_hits = 0
    if args.candidate_snapshot_path:
        snapshot_index = load_candidate_snapshot_index(args.candidate_snapshot_path)
        rows, snapshot_hits = enrich_feedback_rows_with_snapshots(rows, snapshot_index)
    transcripts = load_transcript_indexes(args.transcript_path) if args.transcript_path else {}
    groups = export_groups(
        rows,
        min_text_chars=args.min_text_chars,
        max_text_chars=args.max_text_chars,
        min_negatives=args.min_negatives,
        transcripts=transcripts,
    )
    records = export_pairs(groups) if args.format == "pairs" else groups
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8") as handle:
        for row in records:
            handle.write(json.dumps(row, ensure_ascii=False, sort_keys=True) + "\n")
    positives = sum(len(group["positive"]) for group in groups)
    negatives = sum(len(group["negative"]) for group in groups)
    print(f"Wrote {args.output} groups={len(groups)} positives={positives} negatives={negatives} format={args.format} snapshot_hits={snapshot_hits}")
    return 0 if groups else 1


if __name__ == "__main__":
    raise SystemExit(main())
