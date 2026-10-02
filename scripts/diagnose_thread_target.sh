#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
    echo "Usage: $0 PID TID DESTINATION_NODE" >&2
    exit 2
fi

pid=$1
tid=$2
destination=$3
cgroup_line=$(awk -F: '$1 == "0" && $2 == "" { print; exit } $2 ~ /(^|,)cpuset(,|$)/ { print; exit }' "/proc/$pid/cgroup")
if [[ -z $cgroup_line ]]; then
    echo "preview_result=PERMITTED_CPUSET_UNAVAILABLE"
    exit 1
fi
IFS=: read -r hierarchy controllers cgroup_path <<<"$cgroup_line"
if [[ $hierarchy == 0 ]]; then
    version=2
    cpuset_file="/sys/fs/cgroup${cgroup_path}/cpuset.cpus.effective"
    [[ -s $cpuset_file ]] || cpuset_file="/sys/fs/cgroup${cgroup_path}/cpuset.cpus"
else
    version=1
    cpuset_file="/sys/fs/cgroup/cpuset${cgroup_path}/cpuset.cpus"
fi
if [[ ! -s $cpuset_file ]]; then
    echo "preview_result=PERMITTED_CPUSET_UNAVAILABLE"
    exit 1
fi

current_affinity=$(taskset -pc "$tid" | sed 's/.*: //')
current_cpu=$(awk '{ print $39 }' "/proc/$tid/stat")
current_node=$(for node in /sys/devices/system/node/node*; do grep -qw "$current_cpu" <(cat "$node/cpulist" | tr ',' ' ') 2>/dev/null && basename "$node" | sed 's/node//'; done)
effective_cpuset=$(tr -d '\n' <"$cpuset_file")
online_cpu_set=$(tr -d '\n' </sys/devices/system/cpu/online)
destination_cpus=$(tr -d '\n' <"/sys/devices/system/node/node${destination}/cpulist")
selected_target_cpu=$(python3 - "$effective_cpuset" "$online_cpu_set" "$destination_cpus" <<'PY'
import sys
def expand(value):
    result=set()
    for part in value.split(','):
        if not part: continue
        bounds=part.split('-', 1)
        start=int(bounds[0]); end=int(bounds[-1])
        result.update(range(start, end + 1))
    return result
allowed=expand(sys.argv[1]) & expand(sys.argv[2]) & expand(sys.argv[3])
print(min(allowed) if allowed else '')
PY
)

printf 'pid=%s\ntid=%s\ncurrent_affinity=%s\ncurrent_cpu=%s\ncurrent_node=%s\ncgroup_path=%s\ncgroup_version=%s\neffective_cpuset=%s\nonline_cpu_set=%s\nfinal_permitted_cpu_set=%s\nrequested_destination_node=%s\nselected_target_cpu=%s\n' \
    "$pid" "$tid" "$current_affinity" "$current_cpu" "$current_node" "$cgroup_path" "$version" "$effective_cpuset" "$online_cpu_set" "$effective_cpuset" "$destination" "$selected_target_cpu"
if [[ -n $selected_target_cpu ]]; then
    echo "preview_result=TARGET_POLICY_SELECTED"
else
    echo "preview_result=TARGET_POLICY_NO_ELIGIBLE_CPUS"
fi
