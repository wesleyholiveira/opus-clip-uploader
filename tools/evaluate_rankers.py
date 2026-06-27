#!/usr/bin/env python3
"""Evaluate exported feedback ranker artifacts against a dataset JSONL.

Supports model_type:
  - logistic_regression
  - gbdt_tree_ensemble

This script is intentionally dependency-free so it can run in CI/lightweight envs.
"""

from __future__ import annotations

import argparse
import json
import math
from collections import defaultdict
from pathlib import Path
from typing import Any

from build_feedback_ranker_dataset import FEATURE_ORDER, build_dataset, load_jsonl
from training_common import feature_vector, group_id_for_item, sigmoid


def load_dataset(path: Path | None, feedback_jsonl: Path | None, preset: str | None) -> list[dict[str, Any]]:
    if path is not None:
        rows: list[dict[str, Any]] = []
        with path.open("r", encoding="utf-8") as handle:
            for line in handle:
                line = line.strip()
                if line:
                    rows.append(json.loads(line))
        return rows
    if feedback_jsonl is None:
        raise SystemExit("Provide either --dataset or --feedback-jsonl")
    return build_dataset(load_jsonl(feedback_jsonl), preset=preset or None)


def feature_value(features: dict[str, Any], name: str) -> float:
    try:
        value = float(features.get(name, 0.0))
    except (TypeError, ValueError):
        value = 0.0
    return value if math.isfinite(value) else 0.0


def score_logistic(model: dict[str, Any], features: dict[str, Any]) -> float:
    raw = float(model.get("intercept", 0.0))
    weights = model.get("weights") if isinstance(model.get("weights"), dict) else {}
    for name in model.get("feature_order") or FEATURE_ORDER:
        raw += float(weights.get(name, 0.0)) * feature_value(features, name)
    return sigmoid(raw)


def score_tree(tree: dict[str, Any], features: dict[str, Any]) -> float:
    nodes = tree.get("nodes") if isinstance(tree.get("nodes"), list) else []
    index = 0
    guard = 0
    while 0 <= index < len(nodes) and guard < 128:
        guard += 1
        node = nodes[index]
        if node.get("leaf"):
            return float(node.get("value", 0.0))
        value = feature_value(features, str(node.get("feature") or ""))
        threshold = float(node.get("threshold", 0.0))
        index = int(node.get("left" if value <= threshold else "right", -1))
    return 0.0


def score_gbdt(model: dict[str, Any], features: dict[str, Any]) -> float:
    raw = float(model.get("base_score", model.get("intercept", 0.0)))
    for tree in model.get("trees") or []:
        raw += float(tree.get("weight", 1.0)) * score_tree(tree, features)
    return sigmoid(raw)


def score_model(model: dict[str, Any], item: dict[str, Any]) -> float:
    features = item.get("features") if isinstance(item.get("features"), dict) else {}
    model_type = model.get("model_type")
    if model_type == "gbdt_tree_ensemble":
        return score_gbdt(model, features)
    if model_type == "logistic_regression":
        return score_logistic(model, features)
    raise SystemExit(f"Unsupported model_type: {model_type}")


def precision_at_k(scored: list[tuple[float, int]], k: int) -> float | None:
    if not scored:
        return None
    top = sorted(scored, key=lambda item: item[0], reverse=True)[:k]
    return sum(label for _, label in top) / max(1, len(top))


def evaluate(model: dict[str, Any], dataset: list[dict[str, Any]]) -> dict[str, Any]:
    scored = [(score_model(model, item), int(item["label"]), item) for item in dataset]
    if not scored:
        return {"examples": 0}
    correct = sum(1 for score, label, _ in scored if (score >= 0.5) == bool(label))
    positives = sum(label for _, label, _ in scored)
    negatives = len(scored) - positives
    grouped: dict[str, list[tuple[float, int]]] = defaultdict(list)
    for score, label, item in scored:
        grouped[group_id_for_item(item)].append((score, label))
    p_at_5_values = [value for value in (precision_at_k(values, 5) for values in grouped.values()) if value is not None]
    p_at_10_values = [value for value in (precision_at_k(values, 10) for values in grouped.values()) if value is not None]
    pos_scores = [score for score, label, _ in scored if label == 1]
    neg_scores = [score for score, label, _ in scored if label == 0]
    return {
        "examples": len(scored),
        "positives": positives,
        "negatives": negatives,
        "accuracy_at_0_5": round(correct / len(scored), 4),
        "positive_mean_score": round(sum(pos_scores) / len(pos_scores), 4) if pos_scores else None,
        "negative_mean_score": round(sum(neg_scores) / len(neg_scores), 4) if neg_scores else None,
        "groups": len(grouped),
        "mean_precision_at_5": round(sum(p_at_5_values) / len(p_at_5_values), 4) if p_at_5_values else None,
        "mean_precision_at_10": round(sum(p_at_10_values) / len(p_at_10_values), 4) if p_at_10_values else None,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("model_json", type=Path)
    parser.add_argument("--dataset", type=Path)
    parser.add_argument("--feedback-jsonl", type=Path)
    parser.add_argument("--preset", default="viewer_message_response")
    args = parser.parse_args()

    model = json.loads(args.model_json.read_text(encoding="utf-8"))
    dataset = load_dataset(args.dataset, args.feedback_jsonl, args.preset)
    result = evaluate(model, dataset)
    print(json.dumps(result, indent=2, ensure_ascii=False, sort_keys=True))
    return 0 if result.get("examples", 0) else 1


if __name__ == "__main__":
    raise SystemExit(main())
