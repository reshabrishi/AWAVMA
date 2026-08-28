# AWAVMA Test Fixtures

These CSV files are synthetic algorithm fixtures only. Their `access_value` field is an explicit test signal and is not claimed to be measured by the current Phase 3 monitor. The fixtures exercise high, moderate, low, hysteresis, recent-activity, decay, and rolling-window behavior.

Phase 7 controlled migration fixtures use dedicated child processes for thread affinity tests. They do not represent remote NUMA optimization. Page migration tests never invent addresses; missing or unverified page metadata is rejected.
