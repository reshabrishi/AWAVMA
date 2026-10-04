#!/usr/bin/env bash
set -Eeuo pipefail

skip_install=false
check_only=false
for argument in "$@"; do
    case "$argument" in
        --skip-install) skip_install=true ;;
        --check-only) check_only=true ;;
        *) echo "ERROR: unknown option: $argument" >&2; exit 2 ;;
    esac
done

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root_dir"

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "ERROR: setup_cloudlab_node.sh requires Linux" >&2
    exit 1
fi
for required in Makefile src/awavma_runtime.c include/awavma_runtime.h; do
    [[ -e "$required" ]] || { echo "ERROR: not an AWAVMA repository root (missing $required)" >&2; exit 1; }
done

packages=(build-essential gcc make numactl libnuma-dev python3 python3-pip)
verify_packages() {
    local package
    for package in "${packages[@]}"; do
        dpkg-query -W -f='${Status}' "$package" 2>/dev/null | grep -q 'install ok installed' || {
            echo "ERROR: required package is not installed: $package" >&2
            return 1
        }
    done
}

if ! $check_only && ! $skip_install; then
    sudo apt update
    sudo apt install -y "${packages[@]}"
fi
verify_packages

echo "Git branch: $(git branch --show-current)"
echo "Git revision: $(git rev-parse --short HEAD)"
echo "Git latest: $(git log -1 --oneline)"

topology="$(numactl --hardware)"
printf '%s\n' "$topology"
numa_nodes="$(awk '/available:/ { print $2; exit }' <<<"$topology")"
[[ "$numa_nodes" =~ ^[0-9]+$ ]] || { echo "ERROR: unable to determine NUMA node count" >&2; exit 1; }

if ! $check_only; then
    make benchmark classifier decision validation awavma-runtime environment-check migration-cost-calibration
    mkdir -p results/cloudlab/migration-cost results/cloudlab/calibration
fi

binaries=(bin/benchmark bin/classifier bin/decision bin/validation bin/awavma-runtime bin/environment-check bin/migration-cost-calibration)
for binary in "${binaries[@]}"; do
    [[ -x "$binary" ]] || { echo "ERROR: required binary is missing or not executable: $binary" >&2; exit 1; }
done

./bin/environment-check

echo "===== AWAVMA CLOUDLAB SETUP SUMMARY ====="
echo "Git branch: $(git branch --show-current)"
echo "Git revision: $(git rev-parse --short HEAD)"
echo "NUMA nodes: $numa_nodes"
echo "Environment check: PASS"
echo "Required binaries: READY"
echo "Calibration: NOT RUN"
if (( numa_nodes < 2 )); then
    echo "ENVIRONMENT_LIMITED: cross-NUMA validation unavailable"
    echo "Status: ENVIRONMENT_LIMITED"
else
    echo "Status: READY_FOR_CALIBRATION"
fi
