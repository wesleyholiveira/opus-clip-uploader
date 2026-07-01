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
import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable, Sequence

from candidate_snapshot_join import normalize_profile, row_profile

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
    profile: str
    profile_root: Path
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


def make_paths(output_root: Path, profile: str) -> Paths:
    root = Path(__file__).resolve().parents[1]
    tools = root / "tools"
    normalized_profile = normalize_profile(profile)
    profile_root = output_root / normalized_profile
    datasets = profile_root / "datasets"
    models = profile_root / "models"
    reports = profile_root / "reports"
    for directory in (output_root, profile_root, datasets, models, reports):
        directory.mkdir(parents=True, exist_ok=True)
    return Paths(root=root, tools=tools, output_root=output_root, profile=normalized_profile, profile_root=profile_root, datasets=datasets, models=models, reports=reports)



def _iter_jsonl(path: Path) -> list[dict]:
    rows: list[dict] = []
    if not path.exists():
        return rows
    with path.open("r", encoding="utf-8") as handle:
        for line_no, line in enumerate(handle, 1):
            line = line.strip()
            if not line:
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                print(f"[warn] Ignoring invalid JSON at {path}:{line_no}", flush=True)
                continue
            if isinstance(row, dict):
                rows.append(row)
    return rows


def _write_jsonl(path: Path, rows: Sequence[dict]) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as handle:
        for row in rows:
            handle.write(json.dumps(row, ensure_ascii=False, sort_keys=True) + "\n")
    return path


def _profile_matches(row: dict, profile: str) -> bool:
    return row_profile(row) == normalize_profile(profile)


def write_profile_scoped_inputs(paths: Paths, feedback: Path, snapshot_paths: Sequence[Path]) -> tuple[Path, list[Path], dict[str, str | int | list[str]]]:
    """Create audit-friendly inputs filtered to the selected training profile.

    Runtime append-only files intentionally remain global so a single review log can
    contain multiple presets. Training should not be ambiguous, so the orchestrator
    materializes profile-scoped feedback/snapshot JSONL files under reports/ and uses
    those files for all downstream scripts.
    """
    feedback_rows = _iter_jsonl(feedback)
    filtered_feedback = [row for row in feedback_rows if _profile_matches(row, paths.profile)]
    feedback_out = paths.reports / "boundary-feedback.filtered.jsonl"
    _write_jsonl(feedback_out, filtered_feedback)

    referenced_snapshot_ids = {
        str(row.get("candidate_snapshot_id") or "").strip()
        for row in filtered_feedback
        if str(row.get("candidate_snapshot_id") or "").strip()
    }

    filtered_snapshot_outputs: list[Path] = []
    total_snapshot_rows = 0
    total_filtered_snapshot_rows = 0
    source_snapshot_files: list[str] = []
    for index, snapshot_path in enumerate(snapshot_paths, 1):
        rows = _iter_jsonl(snapshot_path)
        total_snapshot_rows += len(rows)
        source_snapshot_files.append(str(snapshot_path))
        scoped_rows = []
        for row in rows:
            snapshot_id = str(row.get("candidate_snapshot_id") or "").strip()
            if _profile_matches(row, paths.profile) or (snapshot_id and snapshot_id in referenced_snapshot_ids):
                scoped_rows.append(row)
        if not scoped_rows:
            continue
        output = paths.reports / ("candidate-snapshots.filtered.jsonl" if len(snapshot_paths) == 1 else f"candidate-snapshots.{index}.filtered.jsonl")
        _write_jsonl(output, scoped_rows)
        filtered_snapshot_outputs.append(output)
        total_filtered_snapshot_rows += len(scoped_rows)

    stats: dict[str, str | int | list[str]] = {
        "source_feedback": str(feedback),
        "filtered_feedback": str(feedback_out),
        "source_feedback_rows": len(feedback_rows),
        "filtered_feedback_rows": len(filtered_feedback),
        "source_candidate_snapshot_paths": source_snapshot_files,
        "filtered_candidate_snapshot_paths": [str(path) for path in filtered_snapshot_outputs],
        "source_candidate_snapshot_rows": total_snapshot_rows,
        "filtered_candidate_snapshot_rows": total_filtered_snapshot_rows,
    }
    print(
        f"Profile-scoped inputs: feedback {len(filtered_feedback)}/{len(feedback_rows)} rows, "
        f"snapshots {total_filtered_snapshot_rows}/{total_snapshot_rows} rows for profile={paths.profile}",
        flush=True,
    )
    return feedback_out, filtered_snapshot_outputs, stats


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
            "--profile",
            paths.profile,
        ],
    )
    return output


def run_build_tabular_dataset(paths: Paths, feedback: Path, snapshots: Sequence[Path], *, preset: str, output: Path | None = None, include_weak_negatives: bool = False) -> Path:
    dataset = output or (paths.datasets / "feedback-ranker-dataset.jsonl")
    cmd = [
        sys.executable,
        script(paths, "build_feedback_ranker_dataset.py"),
        str(feedback),
        "-o",
        str(dataset),
        "--profile",
        preset,
    ]
    cmd.extend(repeated_path_args("--candidate-snapshot-path", snapshots))
    if include_weak_negatives:
        cmd.append("--include-weak-negatives")
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
        "--profile",
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
        "--profile",
        preset,
    ]
    cmd.extend(repeated_path_args("--candidate-snapshot-path", snapshots))
    if allow_small_data:
        cmd.append("--allow-small-data")
    rc = run_step("Train GBDT feedback ranker", cmd, allow_failure=allow_small_data)
    return output if rc == 0 else None


def run_evaluate(paths: Paths, model: Path, dataset: Path | None = None, *, feedback: Path | None = None, preset: str) -> None:
    cmd = [sys.executable, script(paths, "evaluate_rankers.py"), str(model), "--profile", preset]
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
        "--profile",
        paths.profile,
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
        "--profile",
        paths.profile,
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


def default_plugin_profile_dir(profile: str) -> Path | None:
    root = plugin_data_root()
    if not root:
        return None
    return root / "feedback" / "profiles" / normalize_profile(profile)


def install_runtime_artifacts(paths: Paths, args: argparse.Namespace, artifacts: dict[str, str | None]) -> dict[str, str]:
    target = args.plugin_profile_dir or default_plugin_profile_dir(paths.profile)
    if target is None:
        print("[warn] Could not resolve plugin profile directory; skipping runtime install.", flush=True)
        return {}
    target.mkdir(parents=True, exist_ok=True)
    installed: dict[str, str] = {}

    def copy_artifact(key: str, filename: str) -> None:
        value = artifacts.get(key)
        if not value:
            return
        source = Path(value)
        if not source.exists():
            return
        destination = target / filename
        shutil.copy2(source, destination)
        installed[key] = str(destination)
        print(f"Installed {key}: {destination}", flush=True)

    copy_artifact("boundary_calibration", "boundary-calibration.json")
    copy_artifact("logistic_model", "feedback-ranker.json")
    copy_artifact("gbdt_model", "feedback-ranker-gbdt.json")
    return installed


def write_manifest(paths: Paths, args: argparse.Namespace, artifacts: dict[str, str | None], scoped_inputs: dict[str, str | int | list[str]]) -> Path:
    manifest = paths.profile_root / "training-run-manifest.json"
    payload = {
        "schema_version": 1,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "model": args.model,
        "profile": args.profile,
        "feedback": scoped_inputs.get("source_feedback", str(args.feedback)),
        "filtered_feedback": scoped_inputs.get("filtered_feedback"),
        "candidate_snapshot_paths": scoped_inputs.get("filtered_candidate_snapshot_paths", [str(path) for path in args.candidate_snapshot_path]),
        "source_candidate_snapshot_paths": scoped_inputs.get("source_candidate_snapshot_paths", [str(path) for path in args.candidate_snapshot_path]),
        "transcript_paths": [str(path) for path in args.transcript_path],
        "profile_input_stats": {
            "source_feedback_rows": scoped_inputs.get("source_feedback_rows"),
            "filtered_feedback_rows": scoped_inputs.get("filtered_feedback_rows"),
            "source_candidate_snapshot_rows": scoped_inputs.get("source_candidate_snapshot_rows"),
            "filtered_candidate_snapshot_rows": scoped_inputs.get("filtered_candidate_snapshot_rows"),
        },
        "output_root": str(paths.output_root),
        "profile_root": str(paths.profile_root),
        "artifacts": artifacts,
        "note": "Generated by tools/train_model.py. This manifest only records orchestration outputs; inspect each model/dataset file before production use.",
    }
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
                        help="Root folder where datasets, models, reports and the manifest are written. The profile id is appended below this root.")
    parser.add_argument("--install-runtime-artifacts", action="store_true",
                        help="Copy generated calibration/model artifacts into the OBS plugin feedback/profiles/<profile> runtime directory.")
    parser.add_argument("--plugin-profile-dir", type=Path,
                        help="Override runtime install directory for --install-runtime-artifacts.")
    parser.add_argument("--preset", default="viewer_message_response", help="Legacy alias for --profile.")
    parser.add_argument("--profile", help="Training profile/preset id. Defaults to --preset.")
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
    if args.plugin_profile_dir:
        args.plugin_profile_dir = args.plugin_profile_dir.expanduser().resolve()
    args.profile = normalize_profile(args.profile or args.preset)
    args.preset = args.profile
    paths = make_paths(args.output_root.expanduser().resolve(), args.profile)
    scoped_feedback, scoped_snapshots, scoped_inputs = write_profile_scoped_inputs(paths, args.feedback, args.candidate_snapshot_path)

    artifacts: dict[str, str | None] = {
        "filtered_feedback": str(scoped_feedback),
    }
    if scoped_snapshots:
        artifacts["filtered_candidate_snapshots"] = ",".join(str(path) for path in scoped_snapshots)

    feedback_for_training = scoped_feedback
    snapshots_for_training = scoped_snapshots

    wants_analysis = args.model in {"current", "analysis", "calibration", "all"} and not args.skip_analysis
    wants_calibration = args.model in {"current", "calibration", "all"}
    wants_tabular_dataset = args.model in {"current", "logistic", "gbdt", "datasets", "all"}
    wants_logistic = args.model in {"current", "logistic", "all"}
    wants_gbdt = args.model in {"gbdt", "all"}
    wants_qwen_reranker = args.model in {"qwen-reranker", "qwen-reranker-lora", "datasets", "all"}
    wants_qwen_embedding = args.model in {"qwen-embedding", "datasets", "all"}
    wants_lora = args.model == "qwen-reranker-lora" or (args.model == "all" and args.train_qwen)

    if wants_analysis:
        run_analysis(paths, feedback_for_training)
        artifacts["analysis"] = "stdout"

    if wants_calibration:
        calibration = run_calibration(paths, feedback_for_training, min_records=args.calibration_min_records)
        artifacts["boundary_calibration"] = str(calibration)

    tabular_dataset: Path | None = None
    if wants_tabular_dataset:
        tabular_dataset = run_build_tabular_dataset(
            paths,
            feedback_for_training,
            snapshots_for_training,
            preset=args.preset,
            include_weak_negatives=(args.model == "gbdt"),
        )
        artifacts["tabular_dataset"] = str(tabular_dataset)

    if wants_logistic:
        logistic = run_logistic(paths, feedback_for_training, snapshots_for_training, preset=args.preset, allow_failure=(args.allow_small_data or args.model == "current"))
        artifacts["logistic_model"] = str(logistic) if logistic else None
        if logistic and not args.skip_evaluation:
            run_evaluate(paths, logistic, tabular_dataset, feedback=feedback_for_training, preset=args.preset)

    if wants_gbdt:
        gbdt = run_gbdt(paths, feedback_for_training, snapshots_for_training, preset=args.preset, allow_small_data=args.allow_small_data)
        artifacts["gbdt_model"] = str(gbdt) if gbdt else None
        gbdt_dataset = paths.datasets / "feedback-ranker-gbdt-dataset.jsonl"
        if gbdt and not args.skip_evaluation:
            run_evaluate(paths, gbdt, gbdt_dataset if gbdt_dataset.exists() else tabular_dataset, feedback=feedback_for_training, preset=args.preset)

    qwen_reranker_dataset: Path | None = None
    if wants_qwen_reranker:
        qwen_reranker_dataset = run_qwen_reranker_export(
            paths,
            feedback_for_training,
            snapshots_for_training,
            args.transcript_path,
            fmt=args.qwen_format,
            min_negatives=args.qwen_min_negatives,
        )
        artifacts["qwen_reranker_dataset"] = str(qwen_reranker_dataset) if qwen_reranker_dataset else None

    if wants_qwen_embedding:
        qwen_embedding_dataset = run_qwen_embedding_export(paths, feedback_for_training, snapshots_for_training, args.transcript_path)
        artifacts["qwen_embedding_dataset"] = str(qwen_embedding_dataset) if qwen_embedding_dataset else None

    if wants_lora:
        if not qwen_reranker_dataset:
            qwen_reranker_dataset = paths.datasets / "qwen3-reranker-groups.jsonl"
        if not qwen_reranker_dataset.exists():
            print("Qwen reranker dataset was not generated; cannot train LoRA.", file=sys.stderr)
            return 1
        qwen_lora = run_qwen_reranker_lora(paths, qwen_reranker_dataset, args)
        artifacts["qwen_reranker_lora"] = str(qwen_lora) if qwen_lora else None

    if args.install_runtime_artifacts:
        installed = install_runtime_artifacts(paths, args, artifacts)
        artifacts["installed_runtime_artifacts"] = ",".join(installed.values()) if installed else None

    manifest = write_manifest(paths, args, artifacts, scoped_inputs)
    print(f"\nWrote manifest: {manifest}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
