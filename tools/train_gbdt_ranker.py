#!/usr/bin/env python3
"""Train/export an experimental GBDT feedback ranker.

The generated JSON uses the same runtime file consumed by FeedbackTrainedRanker.
It is intentionally conservative: the script refuses to train by default until the
feedback dataset is large and balanced enough. It prefers group-aware validation
by video/preset so we do not over-trust random splits from a single video.

Optional dependency:
  pip install scikit-learn
"""

from __future__ import annotations

import argparse
import json
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from build_feedback_ranker_dataset import FEATURE_ORDER, build_dataset, load_jsonl
from candidate_snapshot_join import enrich_feedback_rows_with_snapshots, load_candidate_snapshot_index
from training_common import feature_vector, logit, sigmoid, split_by_group


def require_sklearn():
    try:
        import numpy as np  # noqa: F401
        from sklearn.ensemble import GradientBoostingClassifier  # noqa: F401
        from sklearn.metrics import accuracy_score, log_loss, roc_auc_score  # noqa: F401
        return np, GradientBoostingClassifier, accuracy_score, log_loss, roc_auc_score
    except Exception as exc:  # pragma: no cover - depends on local tool env
        raise SystemExit(
            "scikit-learn/numpy are required for GBDT training. Install with: "
            "python -m pip install scikit-learn numpy\n"
            f"Original import error: {exc}"
        )


def class_weights(labels: list[int]) -> list[float]:
    positives = sum(labels)
    negatives = len(labels) - positives
    if positives <= 0 or negatives <= 0:
        return [1.0 for _ in labels]
    pos_weight = len(labels) / (2.0 * positives)
    neg_weight = len(labels) / (2.0 * negatives)
    return [pos_weight if label == 1 else neg_weight for label in labels]


def tree_to_json(tree, feature_order: list[str], learning_rate: float) -> dict[str, Any]:
    nodes: list[dict[str, Any]] = []
    for i in range(tree.node_count):
        feature_index = int(tree.feature[i])
        if feature_index < 0:
            value = float(tree.value[i][0][0])
            nodes.append({"leaf": True, "value": round(value, 10)})
        else:
            nodes.append({
                "leaf": False,
                "feature": feature_order[feature_index],
                "threshold": round(float(tree.threshold[i]), 10),
                "left": int(tree.children_left[i]),
                "right": int(tree.children_right[i]),
            })
    return {"weight": round(float(learning_rate), 10), "nodes": nodes}


def export_model(
    *,
    feedback_jsonl: Path,
    preset: str,
    dataset: list[dict[str, Any]],
    train: list[dict[str, Any]],
    validation: list[dict[str, Any]],
    classifier,
    metrics: dict[str, Any],
) -> dict[str, Any]:
    positives = sum(1 for item in dataset if int(item["label"]) == 1)
    negatives = len(dataset) - positives
    prior = positives / max(1, len(dataset))
    base_score = logit(prior)
    trees = [
        tree_to_json(estimator[0].tree_, list(FEATURE_ORDER), classifier.learning_rate)
        for estimator in classifier.estimators_
    ]
    return {
        "schema_version": 1,
        "model_type": "gbdt_tree_ensemble",
        "preset": preset or "all",
        "trained_at_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "source_feedback": str(feedback_jsonl),
        "records": len(dataset),
        "positives": positives,
        "negatives": negatives,
        "feature_order": list(FEATURE_ORDER),
        "base_score": round(base_score, 10),
        "trees": trees,
        "thresholds": {
            "reject_below": 0.18,
            "accept_above": 0.52,
            "strong_accept_above": 0.80,
        },
        "training": {
            "algorithm": "sklearn.ensemble.GradientBoostingClassifier",
            "train_examples": len(train),
            "validation_examples": len(validation),
            "n_estimators": int(classifier.n_estimators),
            "learning_rate": float(classifier.learning_rate),
            "max_depth": int(classifier.max_depth),
        },
        "evaluation": metrics,
        "note": (
            "Experimental tabular GBDT ranker. Keep hard gates enabled; use shadow diagnostics "
            "until validated by video-level holdout."
        ),
    }


def evaluate_split(name: str, rows: list[dict[str, Any]], classifier, np, accuracy_score, log_loss, roc_auc_score) -> dict[str, Any]:
    if not rows:
        return {}
    x = np.asarray([feature_vector(item) for item in rows], dtype=float)
    y = np.asarray([int(item["label"]) for item in rows], dtype=int)
    probs = classifier.predict_proba(x)[:, 1]
    pred = (probs >= 0.5).astype(int)
    result: dict[str, Any] = {
        "examples": int(len(rows)),
        "positives": int(y.sum()),
        "negatives": int(len(rows) - y.sum()),
        "accuracy_at_0_5": round(float(accuracy_score(y, pred)), 4),
        "log_loss": round(float(log_loss(y, probs, labels=[0, 1])), 6),
        "positive_mean_score": round(float(probs[y == 1].mean()), 4) if int(y.sum()) else None,
        "negative_mean_score": round(float(probs[y == 0].mean()), 4) if int((y == 0).sum()) else None,
    }
    if len(set(y.tolist())) == 2:
        result["roc_auc"] = round(float(roc_auc_score(y, probs)), 4)
    return {name: result}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("feedback_jsonl", type=Path)
    parser.add_argument("-o", "--output", type=Path, required=True)
    parser.add_argument("--dataset-output", type=Path)
    parser.add_argument("--preset", default="viewer_message_response")
    parser.add_argument("--candidate-snapshot-path", type=Path, action="append", default=[],
                        help="candidate-snapshots.jsonl file or feedback directory used to enrich rows with original text/features/scores.")
    parser.add_argument("--min-examples", type=int, default=400)
    parser.add_argument("--min-positives", type=int, default=120)
    parser.add_argument("--min-negatives", type=int, default=160)
    parser.add_argument("--validation-ratio", type=float, default=0.2)
    parser.add_argument("--n-estimators", type=int, default=160)
    parser.add_argument("--learning-rate", type=float, default=0.045)
    parser.add_argument("--max-depth", type=int, default=3)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--allow-small-data", action="store_true", help="Only for local smoke tests; do not use for production models.")
    args = parser.parse_args()

    rows = load_jsonl(args.feedback_jsonl)
    snapshot_hits = 0
    if args.candidate_snapshot_path:
        snapshot_index = load_candidate_snapshot_index(args.candidate_snapshot_path)
        rows, snapshot_hits = enrich_feedback_rows_with_snapshots(rows, snapshot_index)
    dataset = build_dataset(rows, preset=args.preset or None)
    positives = sum(1 for item in dataset if int(item["label"]) == 1)
    negatives = len(dataset) - positives
    if not args.allow_small_data and (
        len(dataset) < args.min_examples or positives < args.min_positives or negatives < args.min_negatives
    ):
        print(
            "Not enough balanced data to train GBDT safely: "
            f"examples={len(dataset)}/{args.min_examples} "
            f"positives={positives}/{args.min_positives} "
            f"negatives={negatives}/{args.min_negatives} snapshot_hits={snapshot_hits}. "
            "Use --allow-small-data only for smoke tests."
        )
        return 1

    if args.dataset_output:
        args.dataset_output.parent.mkdir(parents=True, exist_ok=True)
        with args.dataset_output.open("w", encoding="utf-8") as handle:
            for item in dataset:
                handle.write(json.dumps(item, ensure_ascii=False, sort_keys=True) + "\n")

    train, validation = split_by_group(dataset, args.validation_ratio, args.seed)
    if len({int(item["label"]) for item in train}) < 2:
        print("Training split has only one class; collect more balanced feedback first.")
        return 1

    np, GradientBoostingClassifier, accuracy_score, log_loss, roc_auc_score = require_sklearn()
    x_train = np.asarray([feature_vector(item) for item in train], dtype=float)
    y_train = np.asarray([int(item["label"]) for item in train], dtype=int)
    weights = np.asarray(class_weights(y_train.tolist()), dtype=float)
    classifier = GradientBoostingClassifier(
        n_estimators=args.n_estimators,
        learning_rate=args.learning_rate,
        max_depth=args.max_depth,
        random_state=args.seed,
    )
    classifier.fit(x_train, y_train, sample_weight=weights)
    metrics: dict[str, Any] = {}
    metrics.update(evaluate_split("train", train, classifier, np, accuracy_score, log_loss, roc_auc_score))
    metrics.update(evaluate_split("validation", validation, classifier, np, accuracy_score, log_loss, roc_auc_score))
    model = export_model(
        feedback_jsonl=args.feedback_jsonl,
        preset=args.preset or "all",
        dataset=dataset,
        train=train,
        validation=validation,
        classifier=classifier,
        metrics=metrics,
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(model, indent=2, ensure_ascii=False, sort_keys=True) + "\n", encoding="utf-8")
    print(f"Wrote {args.output} examples={len(dataset)} positives={positives} negatives={negatives} snapshot_hits={snapshot_hits}")
    if validation:
        print(f"Validation: {metrics.get('validation')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
