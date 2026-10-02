#!/usr/bin/env python3
"""Run the production Migration_Execute thread operation for 20 warmups and 100 samples."""
import argparse
from pathlib import Path
import subprocess

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--source-node", required=True, type=int)
    parser.add_argument("--target-node", required=True, type=int)
    parser.add_argument("--binary", type=Path, default=Path("bin/migration-cost-calibration"))
    args = parser.parse_args()
    if args.source_node < 0 or args.target_node < 0 or args.source_node == args.target_node:
        parser.error("source and target must be distinct non-negative NUMA nodes")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        subprocess.run([str(args.binary), str(args.source_node), str(args.target_node), str(args.output)], check=True)
    except FileNotFoundError:
        parser.error("calibration binary not found; run make migration-cost-calibration")
    except subprocess.CalledProcessError as error:
        parser.error("production-equivalent calibration failed: " + str(error.returncode))
    print(args.output)

if __name__ == "__main__": main()
