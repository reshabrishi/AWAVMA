#include "migration_safety_manager.h"
#include "thread_target_policy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    MigrationTargetTopology topology;
    ThreadTargetPolicyInput policy_input;
    unsigned target_calls;
    unsigned executor_calls;
    unsigned feedback_calls;
} policy_fixture_t;

static void report(const char *id, bool passed)
{
    printf("%s: %s\n", id, passed ? "PASS" : "FAIL");
}

static MigrationTargetTopology topology_for(unsigned nodes)
{
    MigrationTargetTopology topology;

    memset(&topology, 0, sizeof(topology));
    topology.available = true;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++)
        topology.cpu_node[cpu] = -1;
    for (unsigned node = 0; node < nodes; node++) {
        topology.node_present[node] = true;
        for (unsigned cpu = node * 2; cpu < node * 2 + 2; cpu++) {
            topology.cpu_node[cpu] = (int)node;
            CPU_SET((int)cpu, &topology.online_cpus);
        }
    }
    return topology;
}

static ThreadTargetPolicyInput input_for(void)
{
    ThreadTargetPolicyInput input;

    memset(&input, 0, sizeof(input));
    input.pid = 42;
    input.start_time_ticks = 99;
    input.attempt_id = "thread-policy-attempt";
    input.action = VALIDATION_ACTION_MOVE_THREAD;
    input.migration_intent_approved = true;
    input.identity_match = true;
    input.safety_state_available = true;
    input.source_cpu_available = true;
    input.source_cpu = 0;
    input.allowed_affinity_available = true;
    CPU_SET(0, &input.allowed_affinity);
    CPU_SET(1, &input.allowed_affinity);
    CPU_SET(2, &input.allowed_affinity);
    CPU_SET(3, &input.allowed_affinity);
    return input;
}

static bool always_identity(void *context, pid_t pid, uint64_t ticks)
{
    (void)context;
    return pid == 42 && ticks == 99;
}

static MigrationTargetResult policy_target(void *context, const MigrationSafetyRequest *request,
                                           const char *attempt, MigrationTarget *target)
{
    policy_fixture_t *fixture = context;
    ThreadTargetPolicyInput input = fixture->policy_input;

    fixture->target_calls++;
    input.attempt_id = attempt;
    input.migration_intent_approved = request->target_policy_migration_intent_approved;
    return thread_target_policy_select(&input, &fixture->topology, target);
}

static MigrationResultCode executor(void *context, const MigrationRequest *request,
                                    MigrationReport *report)
{
    (void)request;
    (void)report;
    ((policy_fixture_t *)context)->executor_calls++;
    return MIGRATION_SYSTEM_ERROR;
}

static bool feedback(void *context, const FeedbackEvent *event, FeedbackResult *result)
{
    (void)event;
    (void)result;
    ((policy_fixture_t *)context)->feedback_calls++;
    return true;
}

static bool selected_execution_disabled(void)
{
    policy_fixture_t fixture = {.topology = topology_for(2), .policy_input = input_for()};
    MigrationSafetyConfig config;
    MigrationSafetyRequest request;
    MigrationSafetyResult result;
    MigrationSafetyManager *manager = migration_safety_manager_create();
    bool passed;

    if (manager == NULL)
        return false;
    memset(&request, 0, sizeof(request));
    snprintf(request.app_id, sizeof(request.app_id), "policy-test");
    request.pid = 42;
    request.start_time_ticks = 99;
    request.action = VALIDATION_ACTION_MOVE_THREAD;
    request.system_safe = true;
    request.migration_request.pid = 42;
    request.migration_request.start_time_ticks = 99;
    request.migration_request.start_time_ticks_available = true;
    request.migration_request.phase5_decision.action = VALIDATION_ACTION_MOVE_THREAD;
    snprintf(request.migration_request.phase6_validation.final_decision,
             sizeof(request.migration_request.phase6_validation.final_decision), "APPROVED");
    migration_safety_config_default(&config);
    config.enabled = true;
    config.execution_enabled = false;
    config.identity_fn = always_identity;
    config.target_fn = policy_target;
    config.execute_fn = executor;
    config.feedback_fn = feedback;
    config.callback_context = &fixture;
    passed = migration_safety_manager_init(manager, &config) &&
             migration_safety_manager_attempt(manager, &request, &result) &&
             result.target_result == MIGRATION_TARGET_AVAILABLE &&
             result.state == MIGRATION_SAFETY_EXECUTION_DISABLED && fixture.target_calls == 1 &&
             fixture.executor_calls == 0 && fixture.feedback_calls == 1;
    migration_safety_manager_destroy(manager);
    return passed;
}

static bool terminal_policy_once(const ThreadTargetPolicyInput *input,
                                 const MigrationTargetTopology *topology,
                                 MigrationTargetResult expected)
{
    policy_fixture_t fixture = {.topology = *topology, .policy_input = *input};
    MigrationSafetyConfig config;
    MigrationSafetyRequest request;
    MigrationSafetyResult result;
    MigrationSafetyManager *manager = migration_safety_manager_create();
    char history_path[] = "/tmp/awavma-phase5-policy-history-XXXXXX";
    char line[1024];
    FILE *history;
    int history_fd;
    unsigned rows = 0;
    bool passed;

    if (manager == NULL)
        return false;
    history_fd = mkstemp(history_path);
    if (history_fd < 0) {
        migration_safety_manager_destroy(manager);
        return false;
    }
    close(history_fd);
    memset(&request, 0, sizeof(request));
    snprintf(request.app_id, sizeof(request.app_id), "policy-terminal-test");
    request.pid = 42;
    request.start_time_ticks = 99;
    request.action = input->action;
    request.system_safe = true;
    request.migration_request.pid = 42;
    request.migration_request.start_time_ticks = 99;
    request.migration_request.start_time_ticks_available = true;
    request.migration_request.phase5_decision.action =
        expected == MIGRATION_TARGET_NO_MIGRATION_INTENT ? VALIDATION_ACTION_INSUFFICIENT : input->action;
    snprintf(request.migration_request.phase6_validation.final_decision,
             sizeof(request.migration_request.phase6_validation.final_decision), "APPROVED");
    migration_safety_config_default(&config);
    config.enabled = true;
    config.execution_enabled = false;
    config.identity_fn = always_identity;
    config.target_fn = policy_target;
    config.execute_fn = executor;
    config.feedback_fn = feedback;
    config.callback_context = &fixture;
    config.history_path = history_path;
    passed = migration_safety_manager_init(manager, &config) &&
             migration_safety_manager_attempt(manager, &request, &result) &&
             result.target_result == expected && result.state == MIGRATION_SAFETY_TARGET_UNAVAILABLE &&
             fixture.target_calls == 1 && fixture.executor_calls == 0 && fixture.feedback_calls == 1;
    history = fopen(history_path, "r");
    while (history != NULL && fgets(line, sizeof(line), history) != NULL)
        if (strstr(line, result.attempt_id) != NULL)
            rows++;
    if (history != NULL)
        fclose(history);
    passed = passed && result.persistence_recorded && rows == 1;
    migration_safety_manager_destroy(manager);
    unlink(history_path);
    return passed;
}

int main(void)
{
    MigrationTargetTopology two = topology_for(2);
    MigrationTargetTopology three = topology_for(3);
    MigrationTargetTopology single = topology_for(1);
    ThreadTargetPolicyInput input;
    MigrationTarget target;
    MigrationTargetTopology host;
    bool passed = true;
    unsigned host_nodes = 0;
    int host_cpu = -1;

    if (!migration_target_topology_read(&host))
        passed = false;
    else {
        for (unsigned node = 0; node < MIGRATION_TARGET_MAX_NODES; node++)
            if (host.node_present[node])
                host_nodes++;
        for (int cpu = 0; cpu < CPU_SETSIZE; cpu++)
            if (CPU_ISSET(cpu, &host.online_cpus)) {
                host_cpu = cpu;
                break;
            }
        input = input_for();
        CPU_ZERO(&input.allowed_affinity);
        CPU_SET(host_cpu, &input.allowed_affinity);
        input.source_cpu = host_cpu;
        passed = host_nodes == 1 && host_cpu >= 0 &&
                 thread_target_policy_select(&input, &host, &target) == MIGRATION_TARGET_NO_ALTERNATE_TARGET &&
                 target.candidate_count == 0 &&
                 terminal_policy_once(&input, &host, MIGRATION_TARGET_NO_ALTERNATE_TARGET);
    }
    report("TS01_SINGLE_NODE_NO_ALTERNATE", passed);

    input = input_for();
    passed = passed && thread_target_policy_select(&input, &two, &target) == MIGRATION_TARGET_AVAILABLE &&
             target.source_node_known && target.source_numa_node == 0 && target.target_numa_node == 1 &&
             CPU_ISSET(2, &target.target_cpu_mask) && CPU_ISSET(3, &target.target_cpu_mask) &&
             target.candidate_count == 1;
    report("TS02_SYNTHETIC_TWO_NODE_SINGLE_CANDIDATE", passed);

    input = input_for();
    CPU_SET(4, &input.allowed_affinity);
    CPU_SET(5, &input.allowed_affinity);
    passed = passed && thread_target_policy_select(&input, &three, &target) == MIGRATION_TARGET_AMBIGUOUS &&
             target.candidate_count == 2 && terminal_policy_once(&input, &three, MIGRATION_TARGET_AMBIGUOUS);
    report("TS03_SYNTHETIC_THREE_NODE_AMBIGUOUS", passed);

    input = input_for();
    CPU_ZERO(&input.allowed_affinity);
    CPU_SET(0, &input.allowed_affinity);
    CPU_SET(1, &input.allowed_affinity);
    CPU_SET(3, &input.allowed_affinity);
    passed = passed && thread_target_policy_select(&input, &two, &target) == MIGRATION_TARGET_AVAILABLE &&
             CPU_COUNT(&target.target_cpu_mask) == 1 && CPU_ISSET(3, &target.target_cpu_mask);
    report("TS04_ALLOWED_CPU_INTERSECTION", passed);

    input = input_for();
    CPU_ZERO(&input.allowed_affinity);
    CPU_SET(0, &input.allowed_affinity);
    CPU_SET(1, &input.allowed_affinity);
    passed = passed && thread_target_policy_select(&input, &two, &target) == MIGRATION_TARGET_NO_ELIGIBLE_CPUS &&
              terminal_policy_once(&input, &two, MIGRATION_TARGET_NO_ELIGIBLE_CPUS);
    {
        MigrationTargetTopology offline_two = topology_for(2);

        CPU_CLR(2, &offline_two.online_cpus);
        CPU_CLR(3, &offline_two.online_cpus);
        passed = passed && thread_target_policy_select(&input, &offline_two, &target) ==
                  MIGRATION_TARGET_NO_ALTERNATE_TARGET;
    }
    report("TS05_EMPTY_CPU_INTERSECTION", passed);

    input = input_for();
    input.allowed_affinity_available = false;
    passed = passed && thread_target_policy_select(&input, &two, &target) == MIGRATION_TARGET_SOURCE_UNKNOWN &&
             terminal_policy_once(&input, &two, MIGRATION_TARGET_SOURCE_UNKNOWN);
    ThreadTargetPolicyInput no_intent = input_for();
    no_intent.migration_intent_approved = false;
    passed = passed && thread_target_policy_select(&no_intent, &two, &target) ==
             MIGRATION_TARGET_NO_MIGRATION_INTENT &&
             terminal_policy_once(&no_intent, &two, MIGRATION_TARGET_NO_MIGRATION_INTENT);
    report("TS06_SOURCE_UNKNOWN", passed);

    input = input_for();
    input.source_cpu_available = false;
    passed = passed && thread_target_policy_select(&input, &two, &target) == MIGRATION_TARGET_SOURCE_AMBIGUOUS &&
             terminal_policy_once(&input, &two, MIGRATION_TARGET_SOURCE_AMBIGUOUS);
    report("TS07_SOURCE_MULTI_NODE_AMBIGUOUS", passed);

    input = input_for();
    input.history_available = true;
    input.recent_equivalent_failure = true;
    input.previous_action = VALIDATION_ACTION_MOVE_THREAD;
    input.previous_source_node = 0;
    input.previous_target_node = 1;
    passed = passed && thread_target_policy_select(&input, &two, &target) == MIGRATION_TARGET_SUPPRESSED_BY_HISTORY &&
             terminal_policy_once(&input, &two, MIGRATION_TARGET_SUPPRESSED_BY_HISTORY);
    report("TS08_HISTORY_SUPPRESSION", passed);

    input = input_for();
    input.quarantined = true;
    passed = passed && thread_target_policy_select(&input, &two, &target) == MIGRATION_TARGET_QUARANTINED &&
             terminal_policy_once(&input, &two, MIGRATION_TARGET_QUARANTINED);
    input = input_for();
    input.cooldown_active = true;
    passed = passed && thread_target_policy_select(&input, &two, &target) == MIGRATION_TARGET_COOLDOWN &&
             terminal_policy_once(&input, &two, MIGRATION_TARGET_COOLDOWN);
    report("TS09_QUARANTINE", passed);

    input = input_for();
    input.identity_match = false;
    passed = passed && thread_target_policy_select(&input, &two, &target) == MIGRATION_TARGET_IDENTITY_CHANGED &&
             terminal_policy_once(&input, &two, MIGRATION_TARGET_IDENTITY_CHANGED);
    report("TS10_IDENTITY_MISMATCH", passed);

    input = input_for();
    input.action = VALIDATION_ACTION_MOVE_MEMORY;
    passed = passed && thread_target_policy_select(&input, &two, &target) ==
             MIGRATION_TARGET_PAGE_RECOVERY_UNAVAILABLE &&
             terminal_policy_once(&input, &two, MIGRATION_TARGET_PAGE_RECOVERY_UNAVAILABLE);
    report("TS11_PAGE_ACTION_RECOVERY_UNAVAILABLE", passed);

    passed = passed && selected_execution_disabled();
    report("TS12_SELECTED_TARGET_EXECUTION_DISABLED", passed);
    (void)single;
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
