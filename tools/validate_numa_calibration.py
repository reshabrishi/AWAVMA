#!/usr/bin/env python3
import csv, os, sys
def placement(path, mode):
    with open(path, newline="", encoding="utf-8") as handle: rows=list(csv.DictReader(handle))
    if len(rows) != 1: raise SystemExit("PLACEMENT_MALFORMED")
    row=rows[0]; required=("placement_mode","verification_status","memory_policy_restored","total_pages","queryable_pages","other_pages","unknown_pages")
    if any(not row.get(k) for k in required) or row["placement_mode"] != mode or row["verification_status"] != "PASS" or row["memory_policy_restored"] != "true" or row["total_pages"] != row["queryable_pages"] or row["other_pages"] != "0" or row["unknown_pages"] != "0": raise SystemExit("PLACEMENT_FAILED")
    return row
def raw_row(argv):
    row = placement(argv[0], argv[4]) if argv[10] == "true" else {}
    values = [argv[1], argv[2], "LOCAL_REMOTE" if int(argv[2]) % 2 else "REMOTE_LOCAL", argv[12], "MOVE_MEMORY", argv[3], argv[4], argv[5], argv[6], str(os.sysconf("SC_PAGE_SIZE")), row.get("local_node", ""), row.get("requested_memory_node", ""), row.get("numa_distance", ""), argv[7], argv[8], row.get("verification_status", "FAIL"), row.get("memory_policy_restored", "false"), row.get("total_pages", ""), row.get("queryable_pages", ""), row.get("other_pages", ""), row.get("unknown_pages", ""), argv[0], argv[9], argv[10], argv[11]]
    print(",".join(values))
if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "placement": placement(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 15 and sys.argv[1] == "raw-row": raw_row(sys.argv[2:])
    else: raise SystemExit(2)
