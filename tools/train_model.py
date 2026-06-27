#!/usr/bin/env python3
"""Unified training/export entrypoint for Clip Cropper feedback models.

This is the high-level command users should run instead of manually chaining
analyze/calibrate/export/train scripts. It intentionally keeps the real work in
small focused tools and orchestrates them with safe defaults.

Typical current-feedback workflow:
  python tools/train_model.py --model current --feedback path/to/boundary-feedback.jsonl

Future workflows:
  python tools/train_model.py --model gbdt --feedback ... --candidate-snapshot-path ...
  python tools/train_model.py --model qwen-reranker --feedback ... --candidate-snapshot-path ... --transcript-path ...
  python tools/train_model.py --model qwen-reranker-lora --feedback ... --transcript-path ... --train-qwen
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable, Sequence

MODEL_CHOICES = (
    "current",
    "analysis",
    "calibration",
    "logistic",
    "gbdt",
    "qwen-reranker",
    "qwen-embedding",
    "qwen-reranker-lora",
    "datasets",
    "all",
)


@dataclass(frozen=True)
class Paths:
    root: Path
    tools: Path
    output_root: Path
    datasets: Path
    models: Path
    reports: Path


def plugin_data_root() -> Path | None:
    appdata = os.environ.get("APPDATA")
    if not appdata:
        return None
    return Path(appdata) / "obs-studio" / "plugin_config" / "clip-cropper"


def default_feedback_path() -> Path | None:
    root = plugin_data_root()
    if not root:
        return None
    return root / "feedback" / "boundary-feedback.jsonl"


def default_snapshot_path() -> Path | None:
    root = plugin_data_root()
    if not root:
        return None
    return root / "feedback" / "candidate-snapshots.jsonl"


def default_transcript_path() -> Path | None:
    root = plugin_data_root()
    if not root:
        return None
    return root / "transcription-cache"


def existing(paths: Iterable[Path | None]) -> list[Path]:
    return [path for path in paths if path and path.exists()]


def make_paths(output_root: Path) -> Paths:
    root = Path(__file__).resolve().parents[1]
    tools = root / "tools"
    datasets = output_root / "datasets"
    models = output_root / "models"
    reports = output_root / "reports"
    for directory in (output_root, datasets, models, reports):
        directory.mkdir(parents=True, exist_ok=True)
    return Paths(root=root, tools=tools, output_root=output_root, datasets=datasets, models=models, reports=reports)


def quote_cmd(cmd: Sequence[str]) -> str:
    return " ".join(f'"{part}"' if any(ch.isspace() for ch in part) else part for part in cmd)


def run_step(name: str, cmd: Sequence[str], *, allow_failure: bool = False) -> int:
    print(f"\n==> {name}", flush=True)
    print(quote_cmd(cmd), flush=True)
    result = subprocess.run(cmd, cwd=Path(__file__).resolve().parents[1])
    if result.returncode != 0 and not allow_failure:
        raise SystemExit(result.returncode)
    if result.returncode != 0:
        print(f"[warn] Step failed but was allowed to continue: {name} rc={result.returncode}", flush=True)
    return int(result.returncode)


def repeated_path_args(flag: str, paths: Sequence[Path]) -> list[str]:
    args: list[str] = []
    for path in paths:
        args.extend([flag, str(path)])
    return args


def script(paths: Paths, filename: str) -> str:
    return str(paths.tools / filename)


def run_analysis(paths: Paths, feedback: Path) -> None:
    run_step("Analyze boundary feedback", [sys.executable, script(paths, "analyze_boundary_feedback.py"), str(feedback)])


def run_calibration(paths: Paths, feedback: Path, *, min_records: int) -> Path:
    output = paths.reports / "boundary-calibration.json"
    run_step(
        "Calibrate boundary thresholds",
        [
            sys.executable,
            script(paths, "calibrate_boundary_thresholds.py"),
            str(feedback),
            "-o",
            str(output),
            "--min-records",
            str(min_records),
        ],
    )
    return output


def run_build_tabular_dataset(paths: Paths, feedback: Path, snapshots: Sequence[Path], *, preset: str, output: Path | None = None) -> Path:
    dataset = output or (paths.datasets / "feedback-ranker-dataset.jsonl")
    cmd = [
        sys.executable,
        script(paths, "build_feedback_ranker_dataset.py"),
        str(feedback),
        "-o",
        str(dataset),
        "--preset",
        preset,
    ]
    cmd.extend(repeated_path_args("--candidate-snapshot-path", snapshots))
    run_step("Build tabular feedback dataset", cmd)
    return dataset


def run_logistic(paths: Paths, feedback: Path, snapshots: Sequence[Path], *, preset: str, allow_failure: bool) -> Path | None:
    # train_feedback_ranker.py intentionally does not support --allow-small-data.
    output = paths.models / "feedback-ranker.json"
    dataset = paths.datasets / "feedback-ranker-dataset.jsonl"
    cmd = [
        sys.executable,
        script(paths, "train_feedback_ranker.py"),
        str(feedback),
        "-o",
        str(output),
        "--dataset-output",
        str(dataset),
        "--preset",
        preset,
    ]
    cmd.extend(repeated_path_args("--candidate-snapshot-path", snapshots))
    rc = run_step("Train logistic feedback ranker", cmd, allow_failure=allow_failure)
    return output if rc == 0 else None


def run_gbdt(paths: Paths, feedback: Path, snapshots: Sequence[Path], *, preset: str, allow_small_data: bool) -> Path | None:
    output = paths.models / "feedback-ranker-gbdt.json"
    dataset = paths.datasets / "feedback-ranker-gbdt-dataset.jsonl"
    cmd = [
        sys.executable,
        script(paths, "train_gbdt_ranker.py"),
        str(feedback),
        "-o",
        str(output),
        "--dataset-output",
        str(dataset),
        "--preset",
        preset,
    ]
    cmd.extend(repeated_path_args("--candidate-snapshot-path", snapshots))
    if allow_small_data:
        cmd.append("--allow-small-data")
    rc = run_step("Train GBDT feedback ranker", cmd, allow_failure=allow_small_data)
    return output if rc == 0 else None


def run_evaluate(paths: Paths, model: Path, dataset: Path | None = None, *, feedback: Path | None = None, preset: str) -> None:
    cmd = [sys.executable, script(paths, "evaluate_rankers.py"), str(model), "--preset", preset]
    if dataset and dataset.exists():
        cmd.extend(["--dataset", str(dataset)])
    elif feedback:
        cmd.extend(["--feedback-jsonl", str(feedback)])
    run_step(f"Evaluate {model.name}", cmd, allow_failure=True)


def run_qwen_reranker_export(
    paths: Paths,
    feedback: Path,
    snapshots: Sequence[Path],
    transcripts: Sequence[Path],
    *,
    output: Path | None = None,
    fmt: str,
    min_negatives: int,
) -> Path | None:
    dataset = output or (paths.datasets / "qwen3-reranker-groups.jsonl")
    cmd = [
        sys.executable,
        script(paths, "export_qwen_reranker_dataset.py"),
        str(feedback),
        "-o",
        str(dataset),
        "--format",
        fmt,
        "--min-negatives",
        str(min_negatives),
    ]
    cmd.extend(repeated_path_args("--candidate-snapshot-path", snapshots))
    cmd.extend(repeated_path_args("--transcript-path", transcripts))
    rc = run_step("Export Qwen3 reranker dataset", cmd, allow_failure=True)
    return dataset if rc == 0 else None


def run_qwen_embedding_export(
    paths: Paths,
    feedback: Path,
    snapshots: Sequence[Path],
    transcripts: Sequence[Path],
    *,
    output: Path | None = None,
) -> Path | None:
    dataset = output or (paths.datasets / "qwen3-embedding-triplets.jsonl")
    cmd = [
        sys.executable,
        script(paths, "export_qwen_embedding_dataset.py"),
        str(feedback),
        "-o",
        str(dataset),
    ]
    cmd.extend(repeated_path_args("--candidate-snapshot-path", snapshots))
    cmd.extend(repeated_path_args("--transcript-path", transcripts))
    rc = run_step("Export Qwen3 embedding dataset", cmd, allow_failure=True)
    return dataset if rc == 0 else None


def run_qwen_reranker_lora(paths: Paths, dataset: Path, args: argparse.Namespace) -> Path | None:
    output_dir = paths.models / "qwen3-reranker-lora"
    cmd = [
        sys.executable,
        script(paths, "train_qwen3_reranker_lora.py"),
        str(dataset),
        "-o",
        str(output_dir),
        "--base-model",
        args.qwen_base_model,
        "--max-length",
        str(args.qwen_max_length),
        "--epochs",
        str(args.qwen_epochs),
        "--batch-size",
        str(args.qwen_batch_size),
        "--gradient-accumulation-steps",
        str(args.qwen_gradient_accumulation_steps),
        "--learning-rate",
        str(args.qwen_learning_rate),
        "--lora-r",
        str(args.qwen_lora_r),
        "--lora-alpha",
        str(args.qwen_lora_alpha),
        "--eval-ratio",
        str(args.qwen_eval_ratio),
        "--seed",
        str(args.seed),
    ]
    if args.allow_small_data:
        cmd.append("--allow-small-data")
    rc = run_step("Train Qwen3 reranker LoRA", cmd, allow_failure=args.allow_small_data)
    return output_dir if rc == 0 else None


def write_manifest(paths: Paths, args: argparse.Namespace, artifacts: dict[str, str | None]) -> Path:
    manifest = paths.output_root / "training-run-manifest.json"
    payload = {
        "schema_version": 1,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "model": args.model,
        "feedback": str(args.feedback),
        "candidate_snapshot_paths": [str(path) for path in args.candidate_snapshot_path],
        "transcript_paths": [str(path) for path in args.transcript_path],
        "output_root": str(paths.output_root),
        "artifacts": artifacts,
        "note": "Generated by tools/train_model.py. This manifest only records orchestration outputs; inspect each model/dataset file before production use.",
    }
    import json

    manifest.write_text(json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return manifest


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Unified Clip Cropper feedback model training/export orchestrator.")
    parser.add_argument("--model", choices=MODEL_CHOICES, required=True,
                        help="What to run: current=analysis+calibration+tabular dataset+safe logistic attempt; datasets=tabular+Qwen exports; all=everything except Qwen LoRA unless --train-qwen is set.")
    parser.add_argument("--feedback", type=Path, default=default_feedback_path(),
                        help="boundary-feedback.jsonl. Defaults to the OBS plugin config path when APPDATA is available.")
    parser.add_argument("--candidate-snapshot-path", type=Path, action="append", default=existing([default_snapshot_path()]),
                        help="candidate-snapshots.jsonl file or feedback directory. Can be repeated.")
    parser.add_argument("--transcript-path", type=Path, action="append", default=existing([default_transcript_path()]),
                        help="Transcript cache file/directory for Qwen text reconstruction. Can be repeated.")
    parser.add_argument("--output-root", type=Path, default=Path("artifacts") / "training",
                        help="Root folder where datasets, models, reports and the manifest are written.")
    parser.add_argument("--preset", default="viewer_message_response")
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--allow-small-data", action="store_true",
                        help="Permissive mode for smoke tests only. Do not use generated models in production.")
    parser.add_argument("--skip-analysis", action="store_true")
    parser.add_argument("--skip-evaluation", action="store_true")
    parser.add_argument("--calibration-min-records", type=int, default=15)

    parser.add_argument("--qwen-format", choices=("grouped", "pairs"), default="grouped")
    parser.add_argument("--qwen-min-negatives", type=int, default=1)
    parser.add_argument("--train-qwen", action="store_true",
                        help="Allow --model all to also run Qwen3 reranker LoRA training. Without this, all only exports Qwen datasets.")
    parser.add_argument("--qwen-base-model", default="Qwen/Qwen3-Reranker-0.6B")
    parser.add_argument("--qwen-max-length", type=int, default=2048)
    parser.add_argument("--qwen-epochs", type=float, default=1.0)
    parser.add_argument("--qwen-batch-size", type=int, default=1)
    parser.add_argument("--qwen-gradient-accumulation-steps", type=int, default=16)
    parser.add_argument("--qwen-learning-rate", type=float, default=2e-5)
    parser.add_argument("--qwen-lora-r", type=int, default=16)
    parser.add_argument("--qwen-lora-alpha", type=int, default=32)
    parser.add_argument("--qwen-eval-ratio", type=float, default=0.15)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not args.feedback:
        print("No feedback path provided and APPDATA default could not be resolved. Pass --feedback path\\to\\boundary-feedback.jsonl", file=sys.stderr)
        return 2
    args.feedback = args.feedback.expanduser().resolve()
    if not args.feedback.exists():
        print(f"Feedback file does not exist: {args.feedback}", file=sys.stderr)
        return 2

    args.candidate_snapshot_path = [path.expanduser().resolve() for path in args.candidate_snapshot_path if path.exists()]
    args.transcript_path = [path.expanduser().resolve() for path in args.transcript_path if path.exists()]
    paths = make_paths(args.output_root.expanduser().resolve())

    artifacts: dict[str, str | None] = {}

    wants_analysis = args.model in {"current", "analysis", "calibration", "all"} and not args.skip_analysis
    wants_calibration = args.model in {"current", "calibration", "all"}
    wants_tabular_dataset = args.model in {"current", "logistic", "gbdt", "datasets", "all"}
    wants_logistic = args.model in {"current", "logistic", "all"}
    wants_gbdt = args.model in {"gbdt", "all"}
    wants_qwen_reranker = args.model in {"qwen-reranker", "qwen-reranker-lora", "datasets", "all"}
    wants_qwen_embedding = args.model in {"qwen-embedding", "datasets", "all"}
    wants_lora = args.model == "qwen-reranker-lora" or (args.model == "all" and args.train_qwen)

    if wants_analysis:
        run_analysis(paths, args.feedback)
        artifacts["analysis"] = "stdout"

    if wants_calibration:
        calibration = run_calibration(paths, args.feedback, min_records=args.calibration_min_records)
        artifacts["boundary_calibration"] = str(calibration)

    tabular_dataset: Path | None = None
    if wants_tabular_dataset:
        tabular_dataset = run_build_tabular_dataset(paths, args.feedback, args.candidate_snapshot_path, preset=args.preset)
        artifacts["tabular_dataset"] = str(tabular_dataset)

    if wants_logistic:
        logistic = run_logistic(paths, args.feedback, args.candidate_snapshot_path, preset=args.preset, allow_failure=(args.allow_small_data or args.model == "current"))
        artifacts["logistic_model"] = str(logistic) if logistic else None
        if logistic and not args.skip_evaluation:
            run_evaluate(paths, logistic, tabular_dataset, feedback=args.feedback, preset=args.preset)

    if wants_gbdt:
        gbdt = run_gbdt(paths, args.feedback, args.candidate_snapshot_path, preset=args.preset, allow_small_data=args.allow_small_data)
        artifacts["gbdt_model"] = str(gbdt) if gbdt else None
        gbdt_dataset = paths.datasets / "feedback-ranker-gbdt-dataset.jsonl"
        if gbdt and not args.skip_evaluation:
            run_evaluate(paths, gbdt, gbdt_dataset if gbdt_dataset.exists() else tabular_dataset, feedback=args.feedback, preset=args.preset)

    qwen_reranker_dataset: Path | None = None
    if wants_qwen_reranker:
        qwen_reranker_dataset = run_qwen_reranker_export(
            paths,
            args.feedback,
            args.candidate_snapshot_path,
            args.transcript_path,
            fmt=args.qwen_format,
            min_negatives=args.qwen_min_negatives,
        )
        artifacts["qwen_reranker_dataset"] = str(qwen_reranker_dataset) if qwen_reranker_dataset else None

    if wants_qwen_embedding:
        qwen_embedding_dataset = run_qwen_embedding_export(paths, args.feedback, args.candidate_snapshot_path, args.transcript_path)
        artifacts["qwen_embedding_dataset"] = str(qwen_embedding_dataset) if qwen_embedding_dataset else None

    if wants_lora:
        if not qwen_reranker_dataset:
            qwen_reranker_dataset = paths.datasets / "qwen3-reranker-groups.jsonl"
        if not qwen_reranker_dataset.exists():
            print("Qwen reranker dataset was not generated; cannot train LoRA.", file=sys.stderr)
            return 1
        qwen_lora = run_qwen_reranker_lora(paths, qwen_reranker_dataset, args)
        artifacts["qwen_reranker_lora"] = str(qwen_lora) if qwen_lora else None

    manifest = write_manifest(paths, args, artifacts)
    print(f"\nWrote manifest: {manifest}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
