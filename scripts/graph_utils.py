"""Small dependency-free SVG plotting and CSV validation helpers for Phase 9."""

from __future__ import annotations

import csv
import math
import os
import re
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable, Sequence
from xml.etree import ElementTree
from xml.sax.saxutils import escape


UNAVAILABLE = {"", "NA", "N/A", "UNKNOWN", "-1", "-1.0"}
COLORS = ("#1f4e79", "#b23a48", "#2a7f62", "#8a5a00", "#6a3d9a", "#176b87", "#7f3c8d", "#4d4d4d")


def read_csv(path: Path, required: Iterable[str] = ()) -> tuple[list[dict[str, str]], int, str]:
    """Read a CSV without coercing unavailable values into zero."""
    if not path.is_file():
        return [], 0, "input file is missing"
    try:
        with path.open(newline="", encoding="utf-8") as handle:
            reader = csv.DictReader(handle)
            if reader.fieldnames is None:
                return [], 0, "CSV has no header"
            missing = [name for name in required if name not in reader.fieldnames]
            if missing:
                return [], 0, "missing columns: " + ",".join(missing)
            rows: list[dict[str, str]] = []
            skipped = 0
            for row in reader:
                if row is None or any(value is None for value in row.values()):
                    skipped += 1
                    continue
                rows.append(row)
            return rows, skipped, ""
    except (OSError, csv.Error, UnicodeError) as error:
        return [], 0, f"CSV read error: {error}"


def finite(value: str | int | float | None, unavailable: bool = True) -> float | None:
    if value is None:
        return None
    text = str(value).strip()
    if unavailable and text.upper() in UNAVAILABLE:
        return None
    try:
        number = float(text)
    except (TypeError, ValueError):
        return None
    if not math.isfinite(number):
        return None
    if unavailable and number == -1.0:
        return None
    return number


def label(value: str | None, fallback: str = "UNKNOWN") -> str:
    text = "" if value is None else str(value).strip()
    return text if text else fallback


def safe_filename(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", value).strip("_") or "graph"


def parse_timestamp(value: str | None) -> datetime | None:
    """Parse repository timestamps without inventing a timezone or value."""
    if value is None:
        return None
    text = str(value).strip()
    if not text or text.upper() in UNAVAILABLE:
        return None
    try:
        parsed = datetime.fromisoformat(text.replace("Z", "+00:00"))
    except ValueError:
        return None
    return parsed if parsed.tzinfo is not None else parsed.replace(tzinfo=timezone.utc)


def identifier_value(row: dict[str, str], column: str) -> str | None:
    value = row.get(column)
    if value is None:
        return None
    text = str(value).strip()
    return None if text.upper() in UNAVAILABLE else text


def exact_join(left: Sequence[dict[str, str]], right: Sequence[dict[str, str]], keys: Sequence[str]) -> tuple[list[tuple[dict[str, str], dict[str, str]]], int]:
    """Join only on complete, non-missing identifier tuples.

    Row order is never used as a fallback. Duplicate keys are retained in a
    deterministic Cartesian match because both records carry the same key.
    """
    index: dict[tuple[str, ...], list[dict[str, str]]] = {}
    skipped = 0
    for row in right:
        values = tuple(identifier_value(row, key) for key in keys)
        if any(value is None for value in values):
            skipped += 1
            continue
        index.setdefault(values, []).append(row)
    matches: list[tuple[dict[str, str], dict[str, str]]] = []
    for row in left:
        values = tuple(identifier_value(row, key) for key in keys)
        if any(value is None for value in values):
            skipped += 1
            continue
        for candidate in index.get(values, []):
            matches.append((row, candidate))
    return matches, skipped


def dataset_type(source: str) -> str:
    return "CONTROLLED_TEST" if "tests/" in source.replace("\\", "/") else "REAL"


def _bounds(values: Sequence[float]) -> tuple[float, float]:
    low = min(values)
    high = max(values)
    if low == high:
        pad = abs(low) * 0.1 or 1.0
        return low - pad, high + pad
    pad = (high - low) * 0.08
    return low - pad, high + pad


def _svg_header(title: str, x_label: str, y_label: str, source: str, data_type: str, width: int, height: int) -> list[str]:
    return [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#ffffff"/>',
        '<style>text{font-family:Arial,Helvetica,sans-serif;fill:#20252b} .axis{stroke:#263238;stroke-width:1} .grid{stroke:#dfe5ea;stroke-width:1} .line{fill:none;stroke-width:2.5} .point{stroke:#fff;stroke-width:1} .bar{stroke:#fff;stroke-width:1}</style>',
        f'<text x="70" y="34" font-size="22" font-weight="bold">{escape(title)}</text>',
        f'<text x="70" y="55" font-size="12" fill="#59636e">Source: {escape(source)} | Data: {escape(data_type)}</text>',
    ]


def _plot_frame(svg: list[str], width: int, height: int, x_label: str, y_label: str, low: float, high: float) -> tuple[float, float, float, float]:
    left, top, right, bottom = 78.0, 78.0, width - 34.0, height - 72.0
    for tick in range(6):
        value = low + (high - low) * tick / 5.0
        y = bottom - (bottom - top) * tick / 5.0
        svg.append(f'<line class="grid" x1="{left:.2f}" y1="{y:.2f}" x2="{right:.2f}" y2="{y:.2f}"/>')
        svg.append(f'<text x="{left - 10:.2f}" y="{y + 4:.2f}" text-anchor="end" font-size="11">{value:.3g}</text>')
    svg.append(f'<line class="axis" x1="{left}" y1="{top}" x2="{left}" y2="{bottom}"/>')
    svg.append(f'<line class="axis" x1="{left}" y1="{bottom}" x2="{right}" y2="{bottom}"/>')
    svg.append(f'<text x="{(left + right) / 2:.2f}" y="{height - 24}" text-anchor="middle" font-size="13">{escape(x_label)}</text>')
    svg.append(f'<text x="18" y="{(top + bottom) / 2:.2f}" text-anchor="middle" font-size="13" transform="rotate(-90 18 {(top + bottom) / 2:.2f})">{escape(y_label)}</text>')
    return left, top, right, bottom


def line_chart(path: Path, title: str, x_label: str, y_label: str, series: Sequence[tuple[str, Sequence[tuple[float, float]]]], source: str, data_type: str) -> bool:
    points = [(x, y) for _, values in series for x, y in values if math.isfinite(x) and math.isfinite(y)]
    if not points:
        return False
    width, height = 1100, 650
    x_low, x_high = _bounds([point[0] for point in points])
    y_low, y_high = _bounds([point[1] for point in points])
    svg = _svg_header(title, x_label, y_label, source, data_type, width, height)
    left, top, right, bottom = _plot_frame(svg, width, height, x_label, y_label, y_low, y_high)
    for index, (name, values) in enumerate(series):
        valid = [(x, y) for x, y in values if math.isfinite(x) and math.isfinite(y)]
        if not valid:
            continue
        color = COLORS[index % len(COLORS)]
        coords = []
        for x, y in valid:
            px = left + (right - left) * (x - x_low) / (x_high - x_low)
            py = bottom - (bottom - top) * (y - y_low) / (y_high - y_low)
            coords.append((px, py))
        svg.append(f'<polyline class="line" stroke="{color}" points="{" ".join(f"{x:.2f},{y:.2f}" for x, y in coords)}"/>')
        for px, py in coords:
            svg.append(f'<circle class="point" cx="{px:.2f}" cy="{py:.2f}" r="3.5" fill="{color}"/>')
    legend_y = 76
    for index, (name, values) in enumerate(series):
        if not any(math.isfinite(x) and math.isfinite(y) for x, y in values):
            continue
        x = width - 250
        y = legend_y + index * 20
        color = COLORS[index % len(COLORS)]
        svg.append(f'<line x1="{x}" y1="{y}" x2="{x + 22}" y2="{y}" stroke="{color}" stroke-width="3"/>')
        svg.append(f'<text x="{x + 28}" y="{y + 4}" font-size="12">{escape(name)}</text>')
    svg.append("</svg>")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(svg) + "\n", encoding="utf-8")
    return validate_svg(path)


def bar_chart(path: Path, title: str, x_label: str, y_label: str, categories: Sequence[str], series: Sequence[tuple[str, Sequence[float]]], source: str, data_type: str) -> bool:
    values = [value for _, row in series for value in row if math.isfinite(value)]
    if not categories or not values:
        return False
    width, height = 1100, 650
    low, high = _bounds([0.0] + values)
    low = min(0.0, low)
    svg = _svg_header(title, x_label, y_label, source, data_type, width, height)
    left, top, right, bottom = _plot_frame(svg, width, height, x_label, y_label, low, high)
    group_width = (right - left) / len(categories)
    bar_width = group_width * 0.72 / max(1, len(series))
    zero_y = bottom - (bottom - top) * (0.0 - low) / (high - low)
    for category_index, category in enumerate(categories):
        center = left + group_width * (category_index + 0.5)
        svg.append(f'<text x="{center:.2f}" y="{bottom + 20:.2f}" text-anchor="middle" font-size="11" transform="rotate(-25 {center:.2f} {bottom + 20:.2f})">{escape(category)}</text>')
        for series_index, (_, row) in enumerate(series):
            if category_index >= len(row) or not math.isfinite(row[category_index]):
                continue
            value = row[category_index]
            x = center - group_width * 0.36 + series_index * bar_width
            y = zero_y if value >= 0 else zero_y - (bottom - top) * (value - 0.0) / (high - low)
            h = abs((bottom - top) * value / (high - low))
            svg.append(f'<rect class="bar" x="{x:.2f}" y="{min(y, zero_y):.2f}" width="{bar_width:.2f}" height="{h:.2f}" fill="{COLORS[series_index % len(COLORS)]}"/>')
    for index, (name, _) in enumerate(series):
        x = width - 250
        y = 76 + index * 20
        color = COLORS[index % len(COLORS)]
        svg.append(f'<rect x="{x}" y="{y - 9}" width="14" height="14" fill="{color}"/>')
        svg.append(f'<text x="{x + 21}" y="{y + 3}" font-size="12">{escape(name)}</text>')
    svg.append("</svg>")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(svg) + "\n", encoding="utf-8")
    return validate_svg(path)


def validate_svg(path: Path) -> bool:
    if not path.is_file() or path.stat().st_size == 0:
        return False
    try:
        root = ElementTree.parse(path).getroot()
        width = float(root.attrib["width"])
        height = float(root.attrib["height"])
        if width <= 0 or height <= 0:
            return False
        content = path.read_text(encoding="utf-8").lower()
        # Do not reject legitimate words such as "insufficient"; only reject
        # standalone non-finite numeric tokens.
        return re.search(r"(?<![a-z])(nan|inf(?:inity)?)(?![a-z])", content) is None
    except (OSError, ValueError, KeyError, ElementTree.ParseError):
        return False
