#include "sequencer/flags.h"

ABSL_FLAG(int, raft_election_timeout_ms, 5, "");
ABSL_FLAG(int, raft_heartbeat_timeout_ms, 2, "");
ABSL_FLAG(int, raft_snapshot_threshold, 1024, "");
ABSL_FLAG(int, raft_snapshot_trailing, 128, "");
ABSL_FLAG(bool, raft_pre_vote, false, "");

ABSL_FLAG(bool, enable_raft_leader_fuzzer, false, "");
ABSL_FLAG(int, raft_leader_fuzz_interval_ms, 2000, "");

ABSL_FLAG(bool, enable_view_reconfig_fuzzer, false, "");
ABSL_FLAG(int, view_reconfig_fuzz_interval_ms, 2000, "");