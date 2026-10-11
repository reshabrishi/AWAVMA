#!/usr/bin/env python3
"""Parse and fingerprint the stable structural subset of ``numactl --hardware``."""
from __future__ import annotations

import argparse
import hashlib
import re
from decimal import Decimal, InvalidOperation
from pathlib import Path

TOPOLOGY_FINGERPRINT_VERSION = "numactl-structural-v1"
AVAILABLE = re.compile(r"available:\s*(\d+)\s+nodes\s*\(([^)]*)\)\s*$")
CPUS = re.compile(r"node\s+(\d+)\s+cpus:\s*(.*)$")
SIZE = re.compile(r"node\s+(\d+)\s+size:\s*([^\s]+)\s+([^\s]+)\s*$")
FREE = re.compile(r"node\s+\d+\s+free:\s*.*$")
DISTANCE_ROW = re.compile(r"(\d+)\s*:\s*(.*)$")


def node_list(value: str) -> tuple[int, ...]:
    nodes = []
    for part in re.split(r"[\s,]+", value.strip()):
        if not part:
            continue
        match = re.fullmatch(r"(\d+)(?:-(\d+))?", part)
        if match is None:
            raise ValueError("NUMA_TOPOLOGY_MALFORMED")
        start, end = int(match.group(1)), int(match.group(2) or match.group(1))
        if end < start:
            raise ValueError("NUMA_TOPOLOGY_MALFORMED")
        nodes.extend(range(start, end + 1))
    if len(nodes) != len(set(nodes)):
        raise ValueError("NUMA_TOPOLOGY_MALFORMED")
    return tuple(sorted(nodes))


def decimal(value: str) -> str:
    try:
        parsed = Decimal(value)
    except InvalidOperation as error:
        raise ValueError("NUMA_TOPOLOGY_MALFORMED") from error
    if not parsed.is_finite() or parsed <= 0:
        raise ValueError("NUMA_TOPOLOGY_MALFORMED")
    return format(parsed.normalize(), "f")


def canonical(text: str) -> tuple[str, dict[tuple[int, int], int]]:
    available = None
    cpus: dict[int, tuple[int, ...]] = {}
    sizes: dict[int, tuple[str, str]] = {}
    distance_header: tuple[int, ...] | None = None
    distances: dict[tuple[int, int], int] = {}
    in_distances = False
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        match = AVAILABLE.fullmatch(line)
        if match:
            if available is not None:
                raise ValueError("NUMA_TOPOLOGY_MALFORMED")
            available = int(match.group(1)), node_list(match.group(2))
            continue
        match = CPUS.fullmatch(line)
        if match:
            node = int(match.group(1))
            if node in cpus:
                raise ValueError("NUMA_TOPOLOGY_MALFORMED")
            cpus[node] = node_list(match.group(2)) if match.group(2) else ()
            continue
        match = SIZE.fullmatch(line)
        if match:
            node = int(match.group(1))
            if node in sizes:
                raise ValueError("NUMA_TOPOLOGY_MALFORMED")
            sizes[node] = decimal(match.group(2)), match.group(3).upper()
            continue
        if FREE.fullmatch(line):
            continue
        if line == "node distances:":
            if in_distances:
                raise ValueError("NUMA_TOPOLOGY_MALFORMED")
            in_distances = True
            continue
        if in_distances and distance_header is None:
            if not line.startswith("node"):
                raise ValueError("NUMA_TOPOLOGY_MALFORMED")
            distance_header = node_list(line[4:])
            continue
        match = DISTANCE_ROW.fullmatch(line) if in_distances else None
        if match and distance_header is not None:
            row, values = int(match.group(1)), match.group(2).split()
            if len(values) != len(distance_header) or row not in distance_header:
                raise ValueError("NUMA_TOPOLOGY_MALFORMED")
            for column, value in zip(distance_header, values):
                if not value.isdigit():
                    raise ValueError("NUMA_TOPOLOGY_MALFORMED")
                distances[row, column] = int(value)
            continue
        raise ValueError("NUMA_TOPOLOGY_MALFORMED")
    if available is None or distance_header is None:
        raise ValueError("NUMA_TOPOLOGY_MALFORMED")
    count, nodes = available
    if count != len(nodes) or tuple(sorted(cpus)) != nodes or tuple(sorted(sizes)) != nodes or distance_header != nodes:
        raise ValueError("NUMA_TOPOLOGY_MALFORMED")
    if {(row, column) for row in nodes for column in nodes} != set(distances):
        raise ValueError("NUMA_TOPOLOGY_MALFORMED")
    lines = [f"version={TOPOLOGY_FINGERPRINT_VERSION}", f"nodes={','.join(map(str, nodes))}"]
    lines.extend(f"node={node};cpus={','.join(map(str, cpus[node]))};size={sizes[node][0]}{sizes[node][1]}" for node in nodes)
    lines.extend(f"distance={row}:{','.join(str(distances[row, column]) for column in nodes)}" for row in nodes)
    return "\n".join(lines) + "\n", distances


def fingerprint_path(path: Path) -> str:
    try:
        canonical_text, _ = canonical(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError) as error:
        raise ValueError("NUMA_TOPOLOGY_EVIDENCE_UNAVAILABLE") from error
    return hashlib.sha256(canonical_text.encode("utf-8")).hexdigest()


def distance_path(path: Path, local: int, remote: int) -> int:
    try:
        _, distances = canonical(path.read_text(encoding="utf-8"))
        return distances[local, remote]
    except (OSError, UnicodeError, KeyError) as error:
        raise ValueError("NUMA_TOPOLOGY_EVIDENCE_UNAVAILABLE") from error


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("topology", type=Path)
    parser.add_argument("--distance", nargs=2, type=int, metavar=("LOCAL", "REMOTE"))
    args = parser.parse_args()
    try:
        print(distance_path(args.topology, *args.distance) if args.distance else fingerprint_path(args.topology))
    except ValueError as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
