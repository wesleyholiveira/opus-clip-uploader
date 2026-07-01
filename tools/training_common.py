#!/usr/bin/env python3
"""Shared helpers for offline Clip Cropper training tools.

These helpers intentionally depend only on the existing feedback dataset builder.
They are used by optional offline scripts and are not imported by the OBS plugin.
"""

from __future__ import annotations

import json
import math
import random
from pathlib import Path
from typing import Any, Iterable

from build_feedback_ranker_dataset import FEATURE_ORDER


def load_jsonl(path: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as handle:
        for line_no, line in enumerate(handle, 1):
            line = line.strip()
            if not line:
                continue
            row = json.loads(line)
            row.setdefault("__line_no", line_no)
            rows.append(row)
    return rows


def write_jsonl(path: Path, rows: Iterable[dict[str, Any]]) -> int:
    path.parent.mkdir(parents=True, exist_ok=True)
    count = 0
    with path.open("w", encoding="utf-8") as handle:
        for row in rows:
            handle.write(json.dumps(row, ensure_ascii=False, sort_keys=True) + "\n")
            count += 1
    return count


def feature_vector(item: dict[str, Any], feature_order: list[str] | tuple[str, ...] = FEATURE_ORDER) -> list[float]:
    features = item.get("features") if isinstance(item.get("features"), dict) else {}
    result: list[float] = []
    for name in feature_order:
        value = features.get(name, 0.0)
        if isinstance(value, bool):
            value = 1.0 if value else 0.0
        try:
            value = float(value)
        except (TypeError, ValueError):
            value = 0.0
        if not math.isfinite(value):
            value = 0.0
        result.append(value)
    return result


def group_id_for_item(item: dict[str, Any]) -> str:
    metadata = item.get("metadata") if isinstance(item.get("metadata"), dict) else {}
    return "::".join(
        str(metadata.get(key) or "")
        for key in ("video_file_name", "training_profile")
    )


def split_by_group(dataset: list[dict[str, Any]], validation_ratio: float, seed: int) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    if not dataset:
        return [], []
    grouped: dict[str, list[dict[str, Any]]] = {}
    for item in dataset:
        grouped.setdefault(group_id_for_item(item), []).append(item)
    groups = list(grouped.items())
    rng = random.Random(seed)
    rng.shuffle(groups)
    validation_size = int(round(len(groups) * max(0.0, min(0.45, validation_ratio))))
    if validation_size <= 0 and len(groups) > 1:
        validation_size = 1
    validation_groups = set(group for group, _ in groups[:validation_size])
    train: list[dict[str, Any]] = []
    validation: list[dict[str, Any]] = []
    for group, items in groups:
        (validation if group in validation_groups else train).extend(items)
    if not train:
        return dataset, []
    return train, validation


def sigmoid(value: float) -> float:
    if value >= 36.0:
        return 1.0
    if value <= -36.0:
        return 0.0
    return 1.0 / (1.0 + math.exp(-value))


def logit(probability: float) -> float:
    p = min(0.999999, max(0.000001, probability))
    return math.log(p / (1.0 - p))


def _transcript_segments_from_json_object(root: dict[str, Any]) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    segments = root.get("segments") if isinstance(root.get("segments"), list) else []
    meta = {
        "videoFileName": root.get("videoFileName"),
        "videoPath": root.get("videoPath"),
        "transcriptionLanguage": root.get("transcriptionLanguage"),
    }
    return meta, [seg for seg in segments if isinstance(seg, dict)]


def _transcript_segments_from_jsonl(path: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    meta: dict[str, Any] = {}
    segments: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            try:
                item = json.loads(line)
            except json.JSONDecodeError:
                continue
            if not isinstance(item, dict):
                continue
            if item.get("type") == "meta":
                meta.update(item)
            elif item.get("type") == "segment" or {"startSec", "endSec", "text"}.issubset(item.keys()):
                segments.append(item)
    return meta, segments


def load_transcript_indexes(paths: list[Path]) -> dict[str, list[dict[str, Any]]]:
    """Load transcript files and index them by video path/name.

    Supports the plugin v4 JSONL cache and compact JSON objects with a segments array.
    Directory inputs are scanned recursively for *.jsonl and *.json files.
    """
    files: list[Path] = []
    for path in paths:
        if path.is_dir():
            files.extend(path.rglob("*.jsonl"))
            files.extend(path.rglob("*.json"))
        elif path.exists():
            files.append(path)
    index: dict[str, list[dict[str, Any]]] = {}
    for path in files:
        try:
            if path.suffix.lower() == ".jsonl":
                meta, segments = _transcript_segments_from_jsonl(path)
            else:
                root = json.loads(path.read_text(encoding="utf-8"))
                if not isinstance(root, dict):
                    continue
                meta, segments = _transcript_segments_from_json_object(root)
        except Exception:
            continue
        normalized: list[dict[str, Any]] = []
        for segment in segments:
            text = str(segment.get("text") or "").strip()
            try:
                start = float(segment.get("startSec", segment.get("start", 0.0)))
                end = float(segment.get("endSec", segment.get("end", 0.0)))
            except (TypeError, ValueError):
                continue
            if text and end >= start:
                normalized.append({"startSec": start, "endSec": end, "text": text})
        if not normalized:
            continue
        keys = {
            str(meta.get("videoFileName") or ""),
            str(meta.get("videoPath") or ""),
            Path(str(meta.get("videoPath") or "")).name,
            path.name,
        }
        for key in keys:
            if key:
                index[key] = normalized
    return index


def range_text_from_transcripts(row: dict[str, Any], transcripts: dict[str, list[dict[str, Any]]]) -> str:
    if not transcripts:
        return ""
    keys = [
        str(row.get("video_file_name") or ""),
        str(row.get("video_path") or ""),
        Path(str(row.get("video_path") or "")).name,
    ]
    segments: list[dict[str, Any]] | None = None
    for key in keys:
        if key and key in transcripts:
            segments = transcripts[key]
            break
    if not segments:
        return ""
    start = row.get("generated_start_sec", row.get("user_start_sec"))
    end = row.get("generated_end_sec", row.get("user_end_sec"))
    try:
        start_f = float(start)
        end_f = float(end)
    except (TypeError, ValueError):
        return ""
    if end_f <= start_f:
        return ""
    selected = [
        str(segment.get("text") or "").strip()
        for segment in segments
        if float(segment.get("endSec", 0.0)) >= start_f and float(segment.get("startSec", 0.0)) <= end_f
    ]
    return " ".join(text for text in selected if text)
