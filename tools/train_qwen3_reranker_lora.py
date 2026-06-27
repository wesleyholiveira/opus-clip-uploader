#!/usr/bin/env python3
"""Fine-tune Qwen3 Reranker with LoRA from exported review pairs.

This is an offline training entrypoint. It does not run inside OBS.
Expected input can be either:
  - grouped rows from export_qwen_reranker_dataset.py
  - pair rows with {query, text, label}

Optional dependencies:
  pip install torch transformers datasets peft accelerate

The script writes a LoRA adapter directory. Merge/convert/quantize for llama.cpp
should be done as a separate model-packaging step after evaluation.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


def load_pairs(path: Path) -> list[dict[str, Any]]:
    pairs: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            row = json.loads(line)
            if "positive" in row or "negative" in row:
                query = str(row.get("query") or "")
                metadata = row.get("metadata") if isinstance(row.get("metadata"), dict) else {}
                for text in row.get("positive") or []:
                    pairs.append({"query": query, "text": str(text), "label": 1.0, "metadata": metadata})
                for text in row.get("negative") or []:
                    pairs.append({"query": query, "text": str(text), "label": 0.0, "metadata": metadata})
            else:
                pairs.append({
                    "query": str(row.get("query") or ""),
                    "text": str(row.get("text") or ""),
                    "label": float(row.get("label", 0.0)),
                    "metadata": row.get("metadata") if isinstance(row.get("metadata"), dict) else {},
                })
    return [item for item in pairs if item["query"].strip() and item["text"].strip()]


def require_training_stack():
    try:
        import torch
        from datasets import Dataset
        from peft import LoraConfig, TaskType, get_peft_model
        from transformers import AutoModelForSequenceClassification, AutoTokenizer, Trainer, TrainingArguments
        return torch, Dataset, LoraConfig, TaskType, get_peft_model, AutoModelForSequenceClassification, AutoTokenizer, Trainer, TrainingArguments
    except Exception as exc:  # pragma: no cover - depends on local tool env
        raise SystemExit(
            "Missing Qwen3 fine-tuning dependencies. Install with:\n"
            "python -m pip install torch transformers datasets peft accelerate\n"
            f"Original import error: {exc}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("dataset_jsonl", type=Path)
    parser.add_argument("-o", "--output-dir", type=Path, required=True)
    parser.add_argument("--base-model", default="Qwen/Qwen3-Reranker-0.6B")
    parser.add_argument("--max-length", type=int, default=2048)
    parser.add_argument("--epochs", type=float, default=1.0)
    parser.add_argument("--batch-size", type=int, default=1)
    parser.add_argument("--gradient-accumulation-steps", type=int, default=16)
    parser.add_argument("--learning-rate", type=float, default=2e-5)
    parser.add_argument("--lora-r", type=int, default=16)
    parser.add_argument("--lora-alpha", type=int, default=32)
    parser.add_argument("--eval-ratio", type=float, default=0.15)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--min-examples", type=int, default=300)
    parser.add_argument("--allow-small-data", action="store_true")
    args = parser.parse_args()

    pairs = load_pairs(args.dataset_jsonl)
    positives = sum(1 for item in pairs if item["label"] >= 0.5)
    negatives = len(pairs) - positives
    if not args.allow_small_data and (len(pairs) < args.min_examples or positives < 80 or negatives < 120):
        print(
            "Not enough Qwen3 reranker fine-tuning data: "
            f"examples={len(pairs)}/{args.min_examples} positives={positives}/80 negatives={negatives}/120. "
            "Use --allow-small-data only for smoke tests."
        )
        return 1

    torch, Dataset, LoraConfig, TaskType, get_peft_model, AutoModelForSequenceClassification, AutoTokenizer, Trainer, TrainingArguments = require_training_stack()
    dataset = Dataset.from_list(pairs).shuffle(seed=args.seed)
    eval_size = int(round(len(dataset) * max(0.0, min(0.45, args.eval_ratio))))
    if eval_size > 0 and eval_size < len(dataset):
        split = dataset.train_test_split(test_size=eval_size, seed=args.seed)
        train_dataset = split["train"]
        eval_dataset = split["test"]
    else:
        train_dataset = dataset
        eval_dataset = None

    tokenizer = AutoTokenizer.from_pretrained(args.base_model, trust_remote_code=True)
    model = AutoModelForSequenceClassification.from_pretrained(
        args.base_model,
        trust_remote_code=True,
        num_labels=1,
    )
    lora = LoraConfig(
        task_type=TaskType.SEQ_CLS,
        r=args.lora_r,
        lora_alpha=args.lora_alpha,
        lora_dropout=0.05,
        target_modules=["q_proj", "k_proj", "v_proj", "o_proj"],
    )
    model = get_peft_model(model, lora)

    def tokenize(batch):
        encoded = tokenizer(
            batch["query"],
            batch["text"],
            max_length=args.max_length,
            truncation=True,
            padding="max_length",
        )
        encoded["labels"] = [float(v) for v in batch["label"]]
        return encoded

    train_dataset = train_dataset.map(tokenize, batched=True, remove_columns=train_dataset.column_names)
    if eval_dataset is not None:
        eval_dataset = eval_dataset.map(tokenize, batched=True, remove_columns=eval_dataset.column_names)

    training_args = TrainingArguments(
        output_dir=str(args.output_dir),
        learning_rate=args.learning_rate,
        per_device_train_batch_size=args.batch_size,
        gradient_accumulation_steps=args.gradient_accumulation_steps,
        num_train_epochs=args.epochs,
        logging_steps=20,
        save_steps=200,
        eval_strategy="steps" if eval_dataset is not None else "no",
        eval_steps=200,
        save_total_limit=2,
        fp16=torch.cuda.is_available(),
        report_to=[],
    )
    trainer = Trainer(model=model, args=training_args, train_dataset=train_dataset, eval_dataset=eval_dataset)
    trainer.train()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    trainer.model.save_pretrained(args.output_dir)
    tokenizer.save_pretrained(args.output_dir)
    (args.output_dir / "clip_cropper_training_meta.json").write_text(
        json.dumps({
            "base_model": args.base_model,
            "examples": len(pairs),
            "positives": positives,
            "negatives": negatives,
            "artifact_type": "qwen3_reranker_lora_adapter",
            "note": "Evaluate before serving or converting for plugin use.",
        }, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    print(f"Wrote LoRA adapter to {args.output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
