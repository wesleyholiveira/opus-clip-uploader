#!/usr/bin/env python3
"""Train a lightweight feedback ranker from Clip Cropper boundary feedback.

This intentionally avoids heavy ML dependencies. It trains a balanced logistic
regression over the same scalar features available inside the C++ scorer and
writes feedback-ranker.json, which the plugin loads at runtime from:

  %APPDATA%/obs-studio/plugin_config/clip-cropper/feedback/feedback-ranker.json
"""

from __future__ import annotations

import argparse
import json
import math
import random
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from build_feedback_ranker_dataset import FEATURE_ORDER, build_dataset, load_jsonl
from candidate_snapshot_join import enrich_feedback_rows_with_snapshots, load_candidate_snapshot_index


def sigmoid(value: float) -> float:
    if value >= 36.0:
        return 1.0
    if value <= -36.0:
        return 0.0
    return 1.0 / (1.0 + math.exp(-value))


def dot(weights: dict[str, float], features: dict[str, float], intercept: float) -> float:
    return intercept + sum(weights.get(name, 0.0) * float(features.get(name, 0.0)) for name in FEATURE_ORDER)


def log_loss(dataset: list[dict[str, Any]], weights: dict[str, float], intercept: float) -> float:
    total = 0.0
    weight_sum = 0.0
    for item in dataset:
        y = float(item["label"])
        w = float(item.get("weight", 1.0))
        p = min(0.999999, max(0.000001, sigmoid(dot(weights, item["features"], intercept))))
        total += w * (-(y * math.log(p)) - ((1.0 - y) * math.log(1.0 - p)))
        weight_sum += w
    return total / max(1e-9, weight_sum)


def split_dataset(dataset: list[dict[str, Any]], validation_ratio: float, seed: int) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    rng = random.Random(seed)
    shuffled = list(dataset)
    rng.shuffle(shuffled)
    validation_size = int(round(len(shuffled) * validation_ratio))
    if validation_size <= 0 or validation_size >= len(shuffled):
        return shuffled, []
    return shuffled[validation_size:], shuffled[:validation_size]


def class_balanced_weights(dataset: list[dict[str, Any]]) -> None:
    positives = sum(1 for item in dataset if item["label"] == 1)
    negatives = len(dataset) - positives
    if positives <= 0 or negatives <= 0:
        return
    pos_weight = len(dataset) / (2.0 * positives)
    neg_weight = len(dataset) / (2.0 * negatives)
    for item in dataset:
        base = float(item.get("weight", 1.0))
        item["weight"] = base * (pos_weight if item["label"] == 1 else neg_weight)


def train_logistic_regression(
    dataset: list[dict[str, Any]],
    *,
    epochs: int,
    learning_rate: float,
    l2: float,
    validation_ratio: float,
    seed: int,
) -> tuple[dict[str, float], float, dict[str, Any]]:
    train, validation = split_dataset(dataset, validation_ratio, seed)
    class_balanced_weights(train)
    weights = {name: 0.0 for name in FEATURE_ORDER}
    labels = [float(item["label"]) for item in train]
    mean_label = sum(labels) / max(1, len(labels))
    intercept = math.log(max(1e-5, mean_label) / max(1e-5, 1.0 - mean_label)) if 0.0 < mean_label < 1.0 else 0.0

    best_weights = dict(weights)
    best_intercept = intercept
    best_loss = float("inf")
    patience = 0

    rng = random.Random(seed)
    for epoch in range(max(1, epochs)):
        rng.shuffle(train)
        for item in train:
            features = item["features"]
            y = float(item["label"])
            sample_weight = float(item.get("weight", 1.0))
            p = sigmoid(dot(weights, features, intercept))
            err = (p - y) * sample_weight
            intercept -= learning_rate * err
            for name in FEATURE_ORDER:
                x = float(features.get(name, 0.0))
                if x == 0.0 and weights[name] == 0.0:
                    continue
                grad = (err * x) + (l2 * weights[name])
                weights[name] -= learning_rate * grad

        eval_set = validation or train
        current_loss = log_loss(eval_set, weights, intercept)
        if current_loss + 1e-6 < best_loss:
            best_loss = current_loss
            best_weights = dict(weights)
            best_intercept = intercept
            patience = 0
        else:
            patience += 1
        if patience >= 30:
            break
        learning_rate *= 0.995

    stats = {
        "train_examples": len(train),
        "validation_examples": len(validation),
        "train_log_loss": round(log_loss(train, best_weights, best_intercept), 6) if train else None,
        "validation_log_loss": round(log_loss(validation, best_weights, best_intercept), 6) if validation else None,
        "epochs_run": epoch + 1,
    }
    return best_weights, best_intercept, stats


def evaluate(dataset: list[dict[str, Any]], weights: dict[str, float], intercept: float) -> dict[str, Any]:
    if not dataset:
        return {}
    scored = [(sigmoid(dot(weights, item["features"], intercept)), int(item["label"])) for item in dataset]
    positives = sum(label for _, label in scored)
    negatives = len(scored) - positives
    correct = sum(1 for score, label in scored if (score >= 0.5) == bool(label))
    pos_scores = [score for score, label in scored if label == 1]
    neg_scores = [score for score, label in scored if label == 0]
    return {
        "examples": len(scored),
        "positives": positives,
        "negatives": negatives,
        "accuracy_at_0_5": round(correct / len(scored), 4),
        "positive_mean_score": round(sum(pos_scores) / len(pos_scores), 4) if pos_scores else None,
        "negative_mean_score": round(sum(neg_scores) / len(neg_scores), 4) if neg_scores else None,
    }


def boundary_correction_profile(dataset: list[dict[str, Any]], raw_rows: list[dict[str, Any]] | None = None) -> dict[str, Any]:
    # Kept intentionally in the model file as training metadata for the next
    # boundary-correction stage. Runtime ranking does not blindly apply these
    # global deltas because that would move otherwise good clips incorrectly.
    return {
        "enabled": False,
        "note": "Boundary correction is exported as metadata first. Use adjusted rows to train a separate start/end model before applying at runtime.",
    }


def model_json(
    *,
    feedback_jsonl: Path,
    preset: str,
    dataset: list[dict[str, Any]],
    weights: dict[str, float],
    intercept: float,
    training_stats: dict[str, Any],
) -> dict[str, Any]:
    positives = sum(1 for item in dataset if item["label"] == 1)
    negatives = len(dataset) - positives
    # Keep thresholds conservative. The model should rank first; it should only
    # hard-reject when it is very confident and the feedback gate also sees a
    # negative pattern.
    # Hard blockers remain authoritative at runtime; the model should not
    # become a broad override for meta/music/opening-less spans.
    return {
        "schema_version": 1,
        "model_type": "logistic_regression",
        "preset": preset or "all",
        "trained_at_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "source_feedback": str(feedback_jsonl),
        "records": len(dataset),
        "positives": positives,
        "negatives": negatives,
        "feature_order": FEATURE_ORDER,
        "intercept": round(intercept, 8),
        "weights": {name: round(float(weights.get(name, 0.0)), 8) for name in FEATURE_ORDER},
        "thresholds": {
            "reject_below": 0.22,
            "accept_above": 0.50,
            "strong_accept_above": 0.78,
        },
        "boundary_correction": boundary_correction_profile(dataset),
        "training": training_stats,
        "evaluation": evaluate(dataset, weights, intercept),
        "note": "Generated by tools/train_feedback_ranker.py. Lightweight ranker; keep feedback hard gates for safety.",
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("feedback_jsonl", type=Path)
    parser.add_argument("-o", "--output", type=Path, required=True)
    parser.add_argument("--preset", default="viewer_message_response")
    parser.add_argument("--dataset-output", type=Path)
    parser.add_argument("--candidate-snapshot-path", type=Path, action="append", default=[],
                        help="candidate-snapshots.jsonl file or feedback directory used to enrich rows with original text/features/scores.")
    parser.add_argument("--min-examples", type=int, default=40)
    parser.add_argument("--min-positives", type=int, default=30)
    parser.add_argument("--min-negatives", type=int, default=60)
    parser.add_argument("--epochs", type=int, default=260)
    parser.add_argument("--learning-rate", type=float, default=0.055)
    parser.add_argument("--l2", type=float, default=0.018)
    parser.add_argument("--validation-ratio", type=float, default=0.2)
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()

    rows = load_jsonl(args.feedback_jsonl)
    snapshot_hits = 0
    if args.candidate_snapshot_path:
        snapshot_index = load_candidate_snapshot_index(args.candidate_snapshot_path)
        rows, snapshot_hits = enrich_feedback_rows_with_snapshots(rows, snapshot_index)
    dataset = build_dataset(rows, preset=args.preset or None)
    positives = sum(1 for item in dataset if item["label"] == 1)
    negatives = len(dataset) - positives
    if len(dataset) < args.min_examples or positives < args.min_positives or negatives < args.min_negatives:
        print(
            "Not enough balanced data to train: "
            f"examples={len(dataset)}/{args.min_examples} "
            f"positives={positives}/{args.min_positives} "
            f"negatives={negatives}/{args.min_negatives} "
            f"snapshot_hits={snapshot_hits}"
        )
        print(
            "Keep calibrating boundary feedback first. Override --min-positives/--min-negatives only "
            "for inspection experiments, not production runtime models."
        )
        return 1

    if args.dataset_output:
        args.dataset_output.parent.mkdir(parents=True, exist_ok=True)
        with args.dataset_output.open("w", encoding="utf-8") as handle:
            for item in dataset:
                handle.write(json.dumps(item, ensure_ascii=False, sort_keys=True) + "\n")

    weights, intercept, stats = train_logistic_regression(
        dataset,
        epochs=args.epochs,
        learning_rate=args.learning_rate,
        l2=args.l2,
        validation_ratio=max(0.0, min(0.45, args.validation_ratio)),
        seed=args.seed,
    )
    model = model_json(
        feedback_jsonl=args.feedback_jsonl,
        preset=args.preset or "all",
        dataset=dataset,
        weights=weights,
        intercept=intercept,
        training_stats=stats,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(model, indent=2, ensure_ascii=False, sort_keys=True) + "\n", encoding="utf-8")
    print(
        f"Wrote {args.output} examples={len(dataset)} positives={positives} negatives={negatives} "
        f"snapshot_hits={snapshot_hits} "
        f"accuracy={model['evaluation'].get('accuracy_at_0_5')}"
    )
    if args.dataset_output:
        print(f"Wrote dataset {args.dataset_output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
