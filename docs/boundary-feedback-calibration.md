# Boundary feedback calibration

The review dialog now writes structured JSONL feedback whenever semantic clip suggestions are reviewed.

Default runtime paths:

```text
<OBS plugin config>/feedback/boundary-feedback.jsonl
<OBS plugin config>/feedback/boundary-calibration.json
```

## What is saved

Each suggested range becomes one JSONL record with:

- generated start/end;
- user start/end when the suggestion was kept or adjusted;
- `start_error_type`: `good`, `starts_too_late`, `starts_too_early`, `rejected`, or `missed_candidate`;
- `end_error_type`: `good`, `ends_too_early`, `overextended_after_resolution`, `rejected`, or `missed_candidate`;
- preset, topic keywords, language, model and review settings key;
- candidate scores/evidence when available from semantic scoring.

The user does not need to type JSON. Moving markers, accepting, cancelling, or removing suggestions is enough.

## Analyze feedback

```bash
python tools/analyze_boundary_feedback.py path/to/boundary-feedback.jsonl
```

## Generate calibration

```bash
python tools/calibrate_boundary_thresholds.py path/to/boundary-feedback.jsonl -o path/to/boundary-calibration.json
```

Copy the generated `boundary-calibration.json` into the plugin feedback directory. The boundary DP refiner loads it automatically and applies conservative adjustments for:

- question/context lookback;
- lookahead for endings;
- context/hook/development/resolution weights;
- target weight;
- defect/tail penalty;
- minimum arc confidence.

This is calibration, not model training. It is safe to start using after ~15 records, but it becomes more useful after 50–100 real corrections.
