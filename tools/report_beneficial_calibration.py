#!/usr/bin/env python3
"""Classify every calibration row using its conservative benefit and cost."""
from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path

from calibration_manifest_trust import TrustError, verify_production_pair

REQUIRED = ("collection_experiment_id", "calibration_status", "expected_recoverable_gain_pct",
            "uncertainty_pct", "safety_margin_pct", "estimated_cost_pct")
OUTPUT = ("effective_cost_pct", "effective_benefit_pct", "conservative_roi", "benefit_status", "benefit_reason")


def finite(row: dict[str, str], field: str) -> float:
    value = float(row[field])
    if not math.isfinite(value):
        raise ValueError(field)
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--calibration", type=Path, required=True)
    parser.add_argument("--production-manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        verified_rows = verify_production_pair(args.calibration, args.production_manifest)
        with args.calibration.open(newline="", encoding="utf-8") as handle:
            reader = csv.DictReader(handle)
            if not reader.fieldnames or any(field not in reader.fieldnames for field in REQUIRED):
                raise ValueError("calibration schema is malformed")
            rows = list(reader)
        if not rows:
            raise ValueError("calibration has no rows")
        if rows != verified_rows:
            raise ValueError("calibration changed during verification")
        output = []
        identities = set()
        for row in rows:
            result = dict(row)
            try:
                gain = finite(row, "expected_recoverable_gain_pct")
                uncertainty = finite(row, "uncertainty_pct")
                margin = finite(row, "safety_margin_pct")
                cost = finite(row, "estimated_cost_pct")
                if (min(gain, uncertainty, margin, cost) < 0 or max(gain, uncertainty, margin, cost) > 100 or
                        row["calibration_status"] != "VALIDATED_PRODUCTION"):
                    raise ValueError("invalid calibration values or status")
                identity = row.get("calibration_id", "")
                if identity and identity in identities:
                    raise ValueError("duplicate calibration identity")
                identities.add(identity)
                effective_cost = cost + uncertainty + margin
                benefit = gain - effective_cost
                roi = benefit / effective_cost if effective_cost > 0 else (math.inf if benefit > 0 else 0.0)
                result.update(effective_cost_pct=f"{effective_cost:.12g}", effective_benefit_pct=f"{benefit:.12g}",
                              conservative_roi="inf" if math.isinf(roi) else f"{roi:.12g}",
                              benefit_status="BENEFICIAL" if benefit > 0 else "NOT_BENEFICIAL",
                              benefit_reason="positive_conservative_benefit" if benefit > 0 else "non_positive_conservative_benefit")
            except (KeyError, ValueError) as error:
                raise ValueError(f"invalid trusted calibration row: {error}") from error
            output.append(result)
        output.sort(key=lambda row: float(row["conservative_roi"]) if row["conservative_roi"] else -math.inf,
                    reverse=True)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("w", newline="", encoding="utf-8") as handle:
            writer = csv.DictWriter(handle, fieldnames=[*reader.fieldnames, *OUTPUT])
            writer.writeheader()
            writer.writerows(output)
    except (OSError, csv.Error, json.JSONDecodeError, TrustError, ValueError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
