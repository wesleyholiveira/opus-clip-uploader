#!/usr/bin/env python3
"""Join boundary-feedback rows with candidate snapshot JSONL rows.

The feedback JSONL is the source of truth for human labels. Candidate snapshots
carry the text/features/scores/evidence captured when the candidate was shown or
reviewed. Exporters should merge snapshots into feedback rows before building ML
training examples.
"""

from __future__ import annotations

import json
import math
from pathlib import Path
from typing import Any, Iterable


def _iter_jsonl_files(paths: Iterable[Path]) -> list[Path]:
    files: list[Path] = []
    for path in paths:
        if path.is_dir():
            files.extend(path.rglob("candidate-snapshots*.jsonl"))
            files.extend(path.rglob("*.candidate-snapshots.jsonl"))
        elif path.exists():
            files.append(path)
    return sorted(set(files))


def _load_jsonl(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as handle:
        for line_no, line in enumerate(handle, 1):
            line = line.strip()
            if not line:
                continue
            try:
                item = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(item, dict):
                item.setdefault("__snapshot_line_no", line_no)
                item.setdefault("__snapshot_file", str(path))
                rows.append(item)
    return rows


def finite_float(value: Any, default: float = math.nan) -> float:
    if isinstance(value, bool):
        return default
    try:
        number = float(value)
    except (TypeError, ValueError):
        return default
    return number if math.isfinite(number) else default


def _range_from(row: dict[str, Any], *pairs: tuple[str, str]) -> tuple[float, float] | None:
    for start_key, end_key in pairs:
        start = finite_float(row.get(start_key))
        end = finite_float(row.get(end_key))
        if math.isfinite(start) and math.isfinite(end) and end > start:
            return start, end
    return None


def _duration(r: tuple[float, float]) -> float:
    return max(0.0, r[1] - r[0])


def _range_similarity(a: tuple[float, float] | None, b: tuple[float, float] | None) -> float:
    if a is None or b is None:
        return 0.0
    da = _duration(a)
    db = _duration(b)
    if da <= 0 or db <= 0:
        return 0.0
    overlap = max(0.0, min(a[1], b[1]) - max(a[0], b[0]))
    union = max(a[1], b[1]) - min(a[0], b[0])
    iou = overlap / union if union > 0 else 0.0
    coverage = overlap / min(da, db) if min(da, db) > 0 else 0.0
    start_score = max(0.0, 1.0 - abs(a[0] - b[0]) / 18.0)
    end_score = max(0.0, 1.0 - abs(a[1] - b[1]) / 24.0)
    return max(0.0, min(1.0, iou * 0.48 + coverage * 0.26 + start_score * 0.16 + end_score * 0.10))


def normalize_profile(value: Any) -> str:
    text = str(value or "auto").strip().lower().replace("-", "_").replace(" ", "_")
    aliases = {
        "": "auto",
        "viewer": "viewer_message_response",
        "chat": "viewer_message_response",
        "q&a": "viewer_message_response",
        "qa": "viewer_message_response",
        "viewer_response": "viewer_message_response",
        "viewer_message": "viewer_message_response",
        "advice": "advice_answer",
        "emotional": "emotional_reaction",
        "story": "story_arc",
        "hot_take": "opinion",
        "tutorial": "tutorial_step",
    }
    return aliases.get(text, text or "auto")


def row_profile(row: dict[str, Any]) -> str:
    return normalize_profile(row.get("training_profile") or row.get("preset") or "auto")


def _snapshot_index_key(row: dict[str, Any]) -> tuple[str, str, int] | None:
    video_id = str(row.get("video_id") or "")
    preset = row_profile(row)
    suggested = row.get("suggested_index")
    if suggested is None:
        suggested = row.get("matched_user_index")
    try:
        suggested_i = int(suggested)
    except (TypeError, ValueError):
        return None
    if not video_id:
        return None
    return video_id, preset, suggested_i


class CandidateSnapshotIndex:
    def __init__(self) -> None:
        self.by_id: dict[str, dict[str, Any]] = {}
        self.by_key: dict[tuple[str, str, int], list[dict[str, Any]]] = {}
        self.count = 0

    def add(self, snapshot: dict[str, Any]) -> None:
        snapshot_id = str(snapshot.get("candidate_snapshot_id") or "").strip()
        if not snapshot_id:
            return
        self.by_id[snapshot_id] = snapshot
        key = _snapshot_index_key(snapshot)
        if key is not None:
            self.by_key.setdefault(key, []).append(snapshot)
        self.count += 1

    def find_for_feedback(self, row: dict[str, Any]) -> dict[str, Any] | None:
        snapshot_id = str(row.get("candidate_snapshot_id") or "").strip()
        if snapshot_id and snapshot_id in self.by_id:
            return self.by_id[snapshot_id]
        key = _snapshot_index_key(row)
        if key is None:
            return None
        candidates = self.by_key.get(key, [])
        if not candidates:
            return None
        row_range = _range_from(
            row,
            ("generated_start_sec", "generated_end_sec"),
            ("user_start_sec", "user_end_sec"),
        )
        best: dict[str, Any] | None = None
        best_score = 0.0
        for snapshot in candidates:
            snapshot_range = _range_from(snapshot, ("start_sec", "end_sec"), ("generated_start_sec", "generated_end_sec"))
            score = _range_similarity(row_range, snapshot_range)
            if score > best_score:
                best = snapshot
                best_score = score
        return best if best is not None and best_score >= 0.50 else None


def load_candidate_snapshot_index(paths: Iterable[Path]) -> CandidateSnapshotIndex:
    index = CandidateSnapshotIndex()
    for path in _iter_jsonl_files(paths):
        for row in _load_jsonl(path):
            if row.get("record_type") not in {None, "candidate_snapshot"}:
                continue
            if not row.get("candidate_snapshot_id"):
                continue
            index.add(row)
    return index


def merge_snapshot_into_feedback(row: dict[str, Any], snapshot: dict[str, Any]) -> dict[str, Any]:
    merged = dict(row)
    merged["candidate_snapshot_loaded"] = True
    merged.setdefault("candidate_snapshot_file", snapshot.get("__snapshot_file"))
    merged.setdefault("candidate_snapshot_line_no", snapshot.get("__snapshot_line_no"))

    passthrough_if_missing = [
        "training_profile", "scores", "classifier_labels", "evidence", "candidate_source", "candidate_final_score",
        "final_score", "rejection_reason", "diagnostic_kind", "selected_rank", "snapshot_kind",
        "text_preview", "timed_text_preview", "candidate_text",
    ]
    for key in passthrough_if_missing:
        if key not in merged or merged.get(key) in (None, "", [], {}):
            if key in snapshot:
                merged[key] = snapshot[key]

    if not merged.get("candidate_source") and snapshot.get("source"):
        merged["candidate_source"] = snapshot.get("source")
    if not merged.get("candidate_final_score"):
        merged["candidate_final_score"] = snapshot.get("candidate_final_score", snapshot.get("final_score"))
    if not merged.get("candidate_text"):
        text = snapshot.get("candidate_text") or snapshot.get("text_preview") or snapshot.get("text")
        if isinstance(text, str) and text.strip():
            merged["candidate_text"] = text
    if not merged.get("transcript_excerpt"):
        timed = snapshot.get("timed_text_preview") or snapshot.get("timed_text")
        if isinstance(timed, str) and timed.strip():
            merged["transcript_excerpt"] = timed
    return merged


def enrich_feedback_rows_with_snapshots(rows: list[dict[str, Any]], index: CandidateSnapshotIndex) -> tuple[list[dict[str, Any]], int]:
    if index.count <= 0:
        return rows, 0
    enriched: list[dict[str, Any]] = []
    hits = 0
    for row in rows:
        snapshot = index.find_for_feedback(row)
        if snapshot is None:
            enriched.append(row)
            continue
        enriched.append(merge_snapshot_into_feedback(row, snapshot))
        hits += 1
    return enriched, hits
