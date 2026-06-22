#!/usr/bin/env python3
"""Clip Cropper WhisperX worker.

Modes:
- align: reads existing whisper.cpp transcript segments as JSON Lines and writes word-aligned JSON Lines.
- transcribe: uses WhisperX as the primary transcription backend and writes word-aligned JSON Lines.

The C++ plugin consumes the output as JSONL so it never needs to load a large WhisperX JSON document at once.
"""

from __future__ import annotations

import argparse
import gc
import json
import os
import shutil
import sys
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional

import torch
import whisperx


def iter_jsonl(path: Path) -> Iterable[Dict[str, Any]]:
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            yield json.loads(line)


def load_segments(path: Path) -> List[Dict[str, Any]]:
    segments: List[Dict[str, Any]] = []
    for item in iter_jsonl(path):
        if item.get("type") != "segment":
            continue
        text = str(item.get("text") or "").strip()
        if not text:
            continue
        start = float(item.get("start", 0.0))
        end = float(item.get("end", start))
        if end <= start:
            continue
        segments.append(
            {
                "segment_index": int(item.get("segment_index", len(segments))),
                "start": start,
                "end": end,
                "text": text,
            }
        )
    return segments


def cleanup_cuda(device: str) -> None:
    gc.collect()
    if device == "cuda" and torch.cuda.is_available():
        torch.cuda.empty_cache()


def normalize_language(language: str) -> str:
    language = (language or "").strip().lower()
    if not language or language == "auto":
        return "auto"
    if language in {"pt-br", "portuguese"}:
        return "pt"
    if language in {"en-us", "english"}:
        return "en"
    return language


def emit_progress(progress: int, message: str) -> None:
    progress = max(0, min(int(progress), 100))
    print(
        json.dumps(
            {"type": "progress", "progress": progress, "message": message},
            ensure_ascii=False,
            separators=(",", ":"),
        ),
        flush=True,
    )


def configure_ffmpeg(ffmpeg_path: Optional[str]) -> None:
    """Make ffmpeg discoverable for whisperx.load_audio().

    WhisperX invokes an executable named `ffmpeg` internally. On Windows, the
    plugin may know the bundled ffmpeg path even when it is not on PATH, so the
    worker prepends that directory here as a second line of defense.
    """

    candidates = [
        ffmpeg_path,
        os.environ.get("CLIP_CROPPER_FFMPEG_PATH"),
        os.environ.get("FFMPEG_BINARY"),
    ]
    for candidate in candidates:
        candidate = str(candidate or "").strip().strip('"')
        if not candidate:
            continue
        candidate_path = Path(candidate)
        directory = candidate_path if candidate_path.is_dir() else candidate_path.parent
        if str(directory):
            os.environ["PATH"] = str(directory) + os.pathsep + os.environ.get("PATH", "")
        if candidate_path.is_file():
            os.environ["CLIP_CROPPER_FFMPEG_PATH"] = str(candidate_path)
            os.environ["FFMPEG_BINARY"] = str(candidate_path)
            break

    resolved = shutil.which("ffmpeg") or shutil.which("ffmpeg.exe")
    if resolved:
        print(f"Using ffmpeg for WhisperX: {resolved}", flush=True)
        return

    print(
        "ffmpeg was not found for WhisperX. Set WhisperX FFmpeg path in the plugin settings "
        "or CLIP_CROPPER_FFMPEG_PATH to ffmpeg.exe / the ffmpeg bin directory.",
        file=sys.stderr,
        flush=True,
    )


def load_audio(path: str):
    print(f"Loading audio for WhisperX: {path}", flush=True)
    return whisperx.load_audio(path)


def align_segments(
    audio: Any,
    segments: List[Dict[str, Any]],
    language: str,
    device: str,
    load_progress: int,
    align_progress: int,
) -> Dict[str, Any]:
    if language == "auto":
        language = "pt"
    emit_progress(load_progress, f"Loading WhisperX alignment model ({language})...")
    print(f"Loading WhisperX alignment model language={language} device={device}", flush=True)
    model_a, metadata = whisperx.load_align_model(language_code=language, device=device)
    emit_progress(align_progress, f"Aligning {len(segments)} transcript segment(s) with WhisperX...")
    print(f"Aligning {len(segments)} segment(s)", flush=True)
    aligned = whisperx.align(
        segments,
        model_a,
        metadata,
        audio,
        device,
        return_char_alignments=False,
    )
    del model_a
    cleanup_cuda(device)
    return aligned


def transcribe_segments(
    audio: Any,
    language: str,
    device: str,
    model_name: str,
    compute_type: str,
    batch_size: int,
) -> tuple[List[Dict[str, Any]], str]:
    emit_progress(12, f"Loading WhisperX ASR model {model_name}...")
    print(
        f"Loading WhisperX ASR model model={model_name} language={language} device={device} compute={compute_type}",
        flush=True,
    )
    load_kwargs: Dict[str, Any] = {"compute_type": compute_type}
    if language != "auto":
        load_kwargs["language"] = language
    model = whisperx.load_model(model_name, device, **load_kwargs)
    emit_progress(22, "Transcribing audio with WhisperX. This can take a while...")
    print(f"Transcribing audio with batch_size={batch_size}", flush=True)
    result = model.transcribe(audio, batch_size=batch_size)
    del model
    cleanup_cuda(device)

    detected_language = normalize_language(str(result.get("language") or language))
    segments: List[Dict[str, Any]] = []
    for index, segment in enumerate(result.get("segments") or []):
        text = str(segment.get("text") or "").strip()
        start = float(segment.get("start", 0.0))
        end = float(segment.get("end", start))
        if not text or end <= start:
            continue
        segments.append(
            {
                "segment_index": index,
                "start": start,
                "end": end,
                "text": text,
            }
        )
    return segments, detected_language


def write_aligned_jsonl(
    output_path: Path,
    aligned_segments: List[Dict[str, Any]],
    original_indices: List[int],
    language: str,
    schema: str,
    progress: int,
) -> None:
    emit_progress(progress, "Writing WhisperX word-aligned transcript...")
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8") as handle:
        meta = {
            "type": "meta",
            "schema": schema,
            "language": language,
            "segments": len(aligned_segments),
        }
        handle.write(json.dumps(meta, ensure_ascii=False, separators=(",", ":")) + "\n")

        for i, segment in enumerate(aligned_segments):
            segment_index = int(segment.get("segment_index", original_indices[i] if i < len(original_indices) else i))
            words = []
            for word in segment.get("words") or []:
                if "start" not in word or "end" not in word:
                    continue
                word_text = str(word.get("word") or "").strip()
                if not word_text:
                    continue
                words.append(
                    {
                        "word": word_text,
                        "start": float(word["start"]),
                        "end": float(word["end"]),
                        "score": float(word.get("score") or 0.0),
                    }
                )

            fallback = aligned_segments[i] if i < len(aligned_segments) else {}
            item = {
                "type": "segment",
                "segment_index": segment_index,
                "start": float(segment.get("start", fallback.get("start", 0.0))),
                "end": float(segment.get("end", fallback.get("end", 0.0))),
                "text": str(segment.get("text") or "").strip(),
                "words": words,
            }
            handle.write(json.dumps(item, ensure_ascii=False, separators=(",", ":")) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", default="align", choices=("align", "transcribe"))
    parser.add_argument("--audio", required=True)
    parser.add_argument("--segments-jsonl")
    parser.add_argument("--output-jsonl", required=True)
    parser.add_argument("--language", default="pt")
    parser.add_argument("--device", default="cuda", choices=("cuda", "cpu"))
    parser.add_argument("--model", default="large-v3")
    parser.add_argument("--compute-type", default="float16")
    parser.add_argument("--batch-size", type=int, default=8)
    parser.add_argument("--ffmpeg-path")
    args = parser.parse_args()

    configure_ffmpeg(args.ffmpeg_path)
    language = normalize_language(args.language)
    batch_size = max(1, min(int(args.batch_size), 128))
    emit_progress(5 if args.mode == "transcribe" else 90, "Loading audio for WhisperX...")
    try:
        audio = load_audio(args.audio)
    except FileNotFoundError as exc:
        print(
            "WhisperX failed to load audio because ffmpeg could not be started. "
            "Configure WhisperX FFmpeg path to ffmpeg.exe or to the ffmpeg bin directory.",
            file=sys.stderr,
            flush=True,
        )
        print(str(exc), file=sys.stderr, flush=True)
        return 4

    if args.mode == "align":
        if not args.segments_jsonl:
            print("--segments-jsonl is required in align mode", file=sys.stderr)
            return 2
        emit_progress(91, "Loading transcript segments for WhisperX alignment...")
        segments = load_segments(Path(args.segments_jsonl))
        if not segments:
            print("No valid segments to align", file=sys.stderr)
            return 2
        aligned = align_segments(audio, segments, language, args.device, 92, 94)
        schema = "clip-cropper-whisperx-align-v1-jsonl"
        write_progress = 97
    else:
        segments, detected_language = transcribe_segments(
            audio,
            language,
            args.device,
            args.model,
            args.compute_type,
            batch_size,
        )
        if not segments:
            print("WhisperX primary transcription produced no segments", file=sys.stderr)
            return 3
        language = detected_language if detected_language != "auto" else language
        aligned = align_segments(audio, segments, language, args.device, 68, 80)
        schema = "clip-cropper-whisperx-primary-v1-jsonl"
        write_progress = 94

    aligned_segments = aligned.get("segments") or []
    original_indices = [int(seg.get("segment_index", i)) for i, seg in enumerate(segments)]
    write_aligned_jsonl(Path(args.output_jsonl), aligned_segments, original_indices, language, schema, write_progress)
    emit_progress(98, "WhisperX transcript is ready.")
    print(f"Wrote WhisperX JSONL: {args.output_jsonl}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
