#!/usr/bin/env python3
"""Export Qwen3 Embedding contrastive/triplet data from review feedback.

Output JSONL rows:
  {"anchor": str, "positive": str, "negative": str, "metadata": {...}}

Use this after there is enough feedback diversity. For the current low-data stage,
this exporter is mainly a schema/staging tool.
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
    return " ".join((value or "").split())[:max_chars].strip()


def anchor_for_row(row: dict[str, Any]) -> str:
    preset = str(row.get("preset") or "viewer_message_response")
    target = str(row.get("main_target") or row.get("review_main_target") or "").strip()
    return (
        f"Good clip for preset '{preset}'. "
        f"Target: {target or 'active preset intent'}. "
        "The clip should have one clear subject, strong opening context, complete answer, and natural ending."
    )


def row_meta(row: dict[str, Any]) -> dict[str, Any]:
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


def build_triplets(rows: list[dict[str, Any]], *, min_text_chars: int, max_text_chars: int, max_triplets_per_group: int, transcripts: dict[str, list[dict[str, Any]]] | None = None) -> list[dict[str, Any]]:
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
        row["__embedding_text"] = text
        grouped[group_key(row)].append(row)

    triplets: list[dict[str, Any]] = []
    for key, group_rows in grouped.items():
        positives = [row for row in group_rows if str(row.get("decision") or "").strip() in POSITIVE_DECISIONS]
        negatives = [row for row in group_rows if str(row.get("decision") or "").strip() in NEGATIVE_DECISIONS]
        if not positives or not negatives:
            continue
        count = 0
        for positive in positives:
            for negative in negatives:
                if positive["__embedding_text"] == negative["__embedding_text"]:
                    continue
                triplets.append({
                    "anchor": anchor_for_row(positive),
                    "positive": positive["__embedding_text"],
                    "negative": negative["__embedding_text"],
                    "metadata": {
                        "group_key": list(key),
                        "positive_row": row_meta(positive),
                        "negative_row": row_meta(negative),
                    },
                })
                count += 1
                if count >= max_triplets_per_group:
                    break
            if count >= max_triplets_per_group:
                break
    return triplets


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("feedback_jsonl", type=Path)
    parser.add_argument("-o", "--output", type=Path, required=True)
    parser.add_argument("--min-text-chars", type=int, default=40)
    parser.add_argument("--max-text-chars", type=int, default=4096)
    parser.add_argument("--max-triplets-per-group", type=int, default=256)
    parser.add_argument("--transcript-path", type=Path, action="append", default=[], help="Transcript JSON/JSONL file or directory used to reconstruct candidate text when feedback rows do not contain text.")
    parser.add_argument("--candidate-snapshot-path", type=Path, action="append", default=[],
                        help="candidate-snapshots.jsonl file or feedback directory used to enrich rows with original text/features/scores before transcript fallback.")
    args = parser.parse_args()

    rows = load_jsonl(args.feedback_jsonl)
    snapshot_hits = 0
    if args.candidate_snapshot_path:
        snapshot_index = load_candidate_snapshot_index(args.candidate_snapshot_path)
        rows, snapshot_hits = enrich_feedback_rows_with_snapshots(rows, snapshot_index)

    triplets = build_triplets(
        rows,
        min_text_chars=args.min_text_chars,
        max_text_chars=args.max_text_chars,
        max_triplets_per_group=args.max_triplets_per_group,
        transcripts=load_transcript_indexes(args.transcript_path) if args.transcript_path else {},
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8") as handle:
        for row in triplets:
            handle.write(json.dumps(row, ensure_ascii=False, sort_keys=True) + "\n")
    print(f"Wrote {args.output} triplets={len(triplets)} snapshot_hits={snapshot_hits}")
    return 0 if triplets else 1


if __name__ == "__main__":
    raise SystemExit(main())
