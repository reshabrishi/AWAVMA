CC ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -D_GNU_SOURCE -Iinclude
LDFLAGS ?=
NUMA_LDLIBS := $(shell if test -f /usr/include/numa.h; then printf '%s' '-lnuma'; fi)
LDLIBS ?= -pthread $(NUMA_LDLIBS)

BENCHMARK_TARGET := bin/benchmark
MONITOR_TARGET := bin/monitor
TEST_TARGET := bin/monitor_test_target
CLASSIFIER_TARGET := bin/classifier
DECISION_TARGET := bin/decision
VALIDATION_TARGET := bin/validation
VALIDATION_TEST_TARGET := bin/validation_gate_test
MIGRATION_TARGET := bin/migration
MIGRATION_TEST_TARGET := bin/migration_test
FEEDBACK_TARGET := bin/feedback
FEEDBACK_TEST_TARGET := bin/feedback_test
RUNTIME_TARGET := bin/application_manager
RUNTIME_TEST_TARGET := bin/runtime_manager_test
APPLICATION_DISCOVERY_TARGET := bin/application-discovery
APPLICATION_DISCOVERY_TEST_TARGET := bin/application-discovery-test
APPLICATION_MANAGER_TARGET := bin/application-manager
APPLICATION_MANAGER_TEST_TARGET := bin/application-manager-test
WORKER_POOL_TEST_TARGET := bin/worker-pool-test
APPLICATION_RUNTIME_TEST_TARGET := bin/application-runtime-test
CONTINUOUS_MONITOR_TEST_TARGET := bin/continuous-monitor-test
AWAVMA_RUNTIME_TARGET := bin/awavma-runtime
AWAVMA_RUNTIME_TEST_TARGET := bin/awavma-runtime-test
RUNTIME_TARGET_FILTER_TEST_TARGET := bin/runtime-target-filter-test
DISCOVERY_CADENCE_TEST_TARGET := bin/discovery-cadence-test
DISCOVERY_CADENCE_PROBE_TARGET := bin/discovery-cadence-probe
PROFILE_AWAVMA_RUNTIME_TARGET := bin/profile-awavma-runtime
PROFILE_PHASE46_BIN_DIR := bin/phase46-profile
PROFILE_PHASE46_CLASSIFIER_TARGET := $(PROFILE_PHASE46_BIN_DIR)/classifier
PROFILE_PHASE46_DECISION_TARGET := $(PROFILE_PHASE46_BIN_DIR)/decision
PROFILE_PHASE46_VALIDATION_TARGET := $(PROFILE_PHASE46_BIN_DIR)/validation

BENCHMARK_SOURCES := src/benchmark.c
MONITOR_SOURCES := src/monitor.c src/monitor_main.c
TEST_SOURCES := src/monitor_test_target.c
CLASSIFIER_SOURCES := src/classifier.c src/classifier_main.c
DECISION_SOURCES := src/decision.c src/decision_main.c
VALIDATION_SOURCES := src/confidence.c src/roi.c src/safety.c src/validation.c src/validation_log.c src/validation_main.c
VALIDATION_TEST_SOURCES := tests/validation_gate_test.c src/confidence.c src/roi.c src/safety.c src/validation.c src/validation_log.c
MIGRATION_SOURCES := src/migration.c src/migration_log.c src/migration_main.c
MIGRATION_TEST_SOURCES := tests/migration_test.c src/migration.c src/migration_log.c
FEEDBACK_SOURCES := src/feedback.c src/feedback_log.c src/feedback_main.c src/decision.c
FEEDBACK_TEST_SOURCES := tests/feedback_test.c src/feedback.c src/feedback_log.c src/decision.c
RUNTIME_SOURCES := src/application_manager.c src/application_manager_main.c src/application_discovery.c
RUNTIME_TEST_SOURCES := tests/runtime_manager_test.c src/application_manager.c src/worker_pool.c
APPLICATION_DISCOVERY_SOURCES := src/application_discovery.c src/application_discovery_main.c
APPLICATION_DISCOVERY_TEST_SOURCES := tests/application_discovery_test.c src/application_discovery.c
APPLICATION_MANAGER_SOURCES := src/application_manager.c src/application_manager_main.c src/application_discovery.c
APPLICATION_MANAGER_TEST_SOURCES := tests/application_manager_test.c src/application_manager.c src/application_discovery.c
WORKER_POOL_SOURCES := src/worker_pool.c
WORKER_POOL_TEST_SOURCES := tests/worker_pool_test.c src/worker_pool.c
APPLICATION_RUNTIME_SOURCES := src/application_runtime.c src/application_manager.c src/application_discovery.c src/worker_pool.c
APPLICATION_RUNTIME_TEST_SOURCES := tests/application_runtime_test.c $(APPLICATION_RUNTIME_SOURCES)
CONTINUOUS_MONITOR_SOURCES := src/runtime_monitor.c src/application_manager.c src/application_discovery.c src/worker_pool.c src/monitor.c
CONTINUOUS_MONITOR_TEST_SOURCES := tests/continuous_monitoring_test.c $(CONTINUOUS_MONITOR_SOURCES)
AWAVMA_RUNTIME_SOURCES := src/awavma_runtime.c src/awavma_runtime_main.c src/runtime_target_filter.c src/classifier.c $(CONTINUOUS_MONITOR_SOURCES)
AWAVMA_RUNTIME_TEST_SOURCES := tests/awavma_runtime_test.c src/awavma_runtime.c src/classifier.c $(CONTINUOUS_MONITOR_SOURCES)
RUNTIME_TARGET_FILTER_TEST_SOURCES := tests/runtime_target_filter_test.c src/runtime_target_filter.c
DISCOVERY_CADENCE_TEST_SOURCES := tests/discovery_cadence_test.c $(CONTINUOUS_MONITOR_SOURCES)
DISCOVERY_CADENCE_PROBE_SOURCES := tests/discovery_cadence_probe.c $(CONTINUOUS_MONITOR_SOURCES)
PROFILE_AWAVMA_RUNTIME_SOURCES := src/awavma_runtime.c src/awavma_runtime_main.c src/runtime_target_filter.c src/classifier.c $(CONTINUOUS_MONITOR_SOURCES) src/monitor_profile.c
HEADERS := include/benchmark.h include/monitor.h include/monitor_profile.h include/classifier.h include/decision.h include/validation.h include/validation_types.h include/validation_log.h include/confidence.h include/roi.h include/safety.h include/migration.h include/migration_types.h include/migration_log.h include/feedback.h include/feedback_types.h include/feedback_log.h include/application_manager.h include/application_manager_types.h include/application_types.h include/worker_pool.h include/worker_types.h include/application_discovery.h include/application_runtime.h include/runtime_monitor.h include/awavma_runtime.h include/runtime_target_filter.h

.PHONY: all benchmark monitor monitor-test-target classifier decision validation migration feedback runtime awavma-runtime test-awavma-runtime test-runtime-target-filter test-discovery-cadence discovery-cadence-probe discovery-cadence-performance multi-application-performance test-multi-application-graphs profile-phase46-binaries phase46-pipeline-profile test-phase46-pipeline test-phase46-pipeline-graphs application-discovery test-application-discovery application-manager test-application-manager worker-pool test-worker-pool test-application-worker continuous-monitor test-continuous-monitor test-final-integration test-system-regression phase10 test-phase10 profile-monitor profile-application-discovery-test profile-continuous-monitor-test profile-awavma-runtime profile-monitoring full-system-performance test-monitoring-profile graphs test-validation test-migration test-feedback test-runtime test-graphs clean

all: bin results logs scripts state history benchmark monitor monitor-test-target classifier decision validation migration feedback runtime graphs

bin results logs scripts state history:
	mkdir -p $@

$(PROFILE_PHASE46_BIN_DIR): | bin
	mkdir -p $@

benchmark: $(BENCHMARK_TARGET)

monitor: $(MONITOR_TARGET)

monitor-test-target: $(TEST_TARGET)

classifier: $(CLASSIFIER_TARGET)

decision: $(DECISION_TARGET)

validation: $(VALIDATION_TARGET)

migration: $(MIGRATION_TARGET)

feedback: $(FEEDBACK_TARGET)

runtime: $(RUNTIME_TARGET)

awavma-runtime: classifier decision validation $(AWAVMA_RUNTIME_TARGET)

test-awavma-runtime: awavma-runtime $(AWAVMA_RUNTIME_TEST_TARGET)
	./$(AWAVMA_RUNTIME_TEST_TARGET)

test-runtime-target-filter: awavma-runtime $(RUNTIME_TARGET_FILTER_TEST_TARGET)
	./$(RUNTIME_TARGET_FILTER_TEST_TARGET)
	python3 tests/runtime_target_filter_cli_test.py

test-discovery-cadence: awavma-runtime $(DISCOVERY_CADENCE_TEST_TARGET)
	./$(DISCOVERY_CADENCE_TEST_TARGET)
	python3 tests/discovery_cadence_cli_test.py

discovery-cadence-probe: $(DISCOVERY_CADENCE_PROBE_TARGET)

application-discovery: $(APPLICATION_DISCOVERY_TARGET)

test-application-discovery: $(APPLICATION_DISCOVERY_TEST_TARGET)
	./$(APPLICATION_DISCOVERY_TEST_TARGET)

application-manager: $(APPLICATION_MANAGER_TARGET)

test-application-manager: $(APPLICATION_MANAGER_TEST_TARGET)
	./$(APPLICATION_MANAGER_TEST_TARGET)

worker-pool: $(WORKER_POOL_TEST_TARGET)

test-worker-pool: $(WORKER_POOL_TEST_TARGET)
	./$(WORKER_POOL_TEST_TARGET)

test-application-worker: $(APPLICATION_RUNTIME_TEST_TARGET) $(APPLICATION_DISCOVERY_TEST_TARGET) $(APPLICATION_MANAGER_TEST_TARGET) $(WORKER_POOL_TEST_TARGET)
	make test-application-discovery
	make test-application-manager
	make test-worker-pool
	AWAVMA_REGRESSION_VERIFIED=1 ./$(APPLICATION_RUNTIME_TEST_TARGET)

continuous-monitor: $(CONTINUOUS_MONITOR_TEST_TARGET)

test-continuous-monitor: $(CONTINUOUS_MONITOR_TEST_TARGET)
	make test-application-discovery
	make test-application-manager
	make test-worker-pool
	AWAVMA_REGRESSION_VERIFIED=1 ./$(CONTINUOUS_MONITOR_TEST_TARGET)

test-final-integration:
	python3 tests/final_integration_test.py

test-system-regression:
	python3 tests/system_regression_test.py

phase10:
	python3 tests/phase10_test.py

test-phase10: phase10

profile-monitor: | bin
	$(CC) $(CFLAGS) -DAWAVMA_PROFILE -o bin/profile-monitor $(MONITOR_SOURCES) src/monitor_profile.c $(LDLIBS)

profile-application-discovery-test: | bin
	$(CC) $(CFLAGS) -DAWAVMA_PROFILE -o bin/profile-application-discovery-test tests/application_discovery_test.c src/application_discovery.c src/monitor_profile.c -pthread

profile-continuous-monitor-test: | bin
	$(CC) $(CFLAGS) -DAWAVMA_PROFILE -o bin/profile-continuous-monitor-test tests/continuous_monitoring_test.c $(CONTINUOUS_MONITOR_SOURCES) src/monitor_profile.c -pthread $(NUMA_LDLIBS)

profile-awavma-runtime: classifier decision validation | bin
	$(CC) $(CFLAGS) -DAWAVMA_PROFILE -o $(PROFILE_AWAVMA_RUNTIME_TARGET) $(PROFILE_AWAVMA_RUNTIME_SOURCES) -pthread $(NUMA_LDLIBS) -lm

profile-phase46-binaries: $(PROFILE_PHASE46_CLASSIFIER_TARGET) $(PROFILE_PHASE46_DECISION_TARGET) $(PROFILE_PHASE46_VALIDATION_TARGET)

$(PROFILE_PHASE46_CLASSIFIER_TARGET): src/classifier.c src/classifier_main.c src/monitor_profile.c | $(PROFILE_PHASE46_BIN_DIR)
	$(CC) $(CFLAGS) -DAWAVMA_PROFILE -o $@ src/classifier.c src/classifier_main.c src/monitor_profile.c -lm

$(PROFILE_PHASE46_DECISION_TARGET): src/decision.c src/decision_main.c src/monitor_profile.c | $(PROFILE_PHASE46_BIN_DIR)
	$(CC) $(CFLAGS) -DAWAVMA_PROFILE -o $@ src/decision.c src/decision_main.c src/monitor_profile.c -lm

$(PROFILE_PHASE46_VALIDATION_TARGET): src/confidence.c src/roi.c src/safety.c src/validation.c src/validation_log.c src/validation_main.c src/monitor_profile.c | $(PROFILE_PHASE46_BIN_DIR)
	$(CC) $(CFLAGS) -DAWAVMA_PROFILE -o $@ src/confidence.c src/roi.c src/safety.c src/validation.c src/validation_log.c src/validation_main.c src/monitor_profile.c -lm

profile-monitoring: profile-monitor profile-application-discovery-test profile-continuous-monitor-test
	python3 tests/monitoring_profile_test.py

test-monitoring-profile: profile-monitoring

full-system-performance: profile-awavma-runtime
	python3 tests/full_system_performance_test.py

discovery-cadence-performance: profile-awavma-runtime discovery-cadence-probe
	python3 tests/discovery_cadence_performance_test.py

multi-application-performance: profile-awavma-runtime
	python3 scripts/run_multi_application_performance.py

test-multi-application-graphs:
	python3 tests/multi_application_graph_test.py

phase46-pipeline-profile: profile-awavma-runtime profile-phase46-binaries
	python3 scripts/run_phase46_pipeline_profile.py
	python3 scripts/summarize_phase46_pipeline.py
	python3 scripts/generate_phase46_pipeline_graphs.py --root $(CURDIR)

test-phase46-pipeline: profile-awavma-runtime
	python3 tests/phase46_pipeline_test.py

test-phase46-pipeline-graphs:
	python3 tests/phase46_pipeline_graph_test.py

phase56-subprocess-profile: profile-awavma-runtime profile-phase46-binaries
	python3 scripts/run_phase56_subprocess_profile.py
	python3 scripts/summarize_phase56_investigation.py
	python3 scripts/generate_phase56_investigation_graphs.py --root $(CURDIR)

test-phase56-subprocess-profile:
	python3 scripts/generate_phase56_investigation_graphs.py --root $(CURDIR)
	python3 tests/phase56_subprocess_profile_test.py

final-integrated-performance: profile-awavma-runtime profile-phase46-binaries
	python3 scripts/run_final_integrated_performance.py
	python3 scripts/generate_final_integrated_reports.py

test-final-integrated-performance:
	python3 tests/final_integrated_performance_test.py

graphs:
	python3 scripts/generate_graphs.py

test-validation: $(VALIDATION_TARGET) $(VALIDATION_TEST_TARGET)
	sh tests/run_validation_fixtures.sh

test-migration: $(MIGRATION_TEST_TARGET)
	./$(MIGRATION_TEST_TARGET)
	sh tests/run_migration_cli.sh

test-feedback: $(FEEDBACK_TEST_TARGET)
	./$(FEEDBACK_TEST_TARGET)
	sh tests/run_feedback_cli.sh

test-runtime: test-awavma-runtime test-runtime-target-filter

test-graphs:
	python3 tests/graph_test.py

$(BENCHMARK_TARGET): $(BENCHMARK_SOURCES) $(HEADERS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(BENCHMARK_SOURCES) $(LDLIBS)

$(MONITOR_TARGET): $(MONITOR_SOURCES) $(HEADERS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MONITOR_SOURCES) $(LDLIBS)

$(TEST_TARGET): $(TEST_SOURCES) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(TEST_SOURCES) -pthread

$(CLASSIFIER_TARGET): $(CLASSIFIER_SOURCES) $(HEADERS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(CLASSIFIER_SOURCES) -lm

$(DECISION_TARGET): $(DECISION_SOURCES) $(HEADERS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(DECISION_SOURCES) -lm

$(VALIDATION_TARGET): $(VALIDATION_SOURCES) $(HEADERS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(VALIDATION_SOURCES) -lm

$(VALIDATION_TEST_TARGET): $(VALIDATION_TEST_SOURCES) $(HEADERS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(VALIDATION_TEST_SOURCES) -lm

$(MIGRATION_TARGET): $(MIGRATION_SOURCES) $(HEADERS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MIGRATION_SOURCES) -pthread -lm

$(MIGRATION_TEST_TARGET): $(MIGRATION_TEST_SOURCES) $(HEADERS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(MIGRATION_TEST_SOURCES) -pthread -lm

$(FEEDBACK_TARGET): $(FEEDBACK_SOURCES) $(HEADERS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FEEDBACK_SOURCES) -lm

$(FEEDBACK_TEST_TARGET): $(FEEDBACK_TEST_SOURCES) $(HEADERS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(FEEDBACK_TEST_SOURCES) -lm

$(RUNTIME_TARGET): $(RUNTIME_SOURCES) $(HEADERS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(RUNTIME_SOURCES) -pthread

$(RUNTIME_TEST_TARGET): $(RUNTIME_TEST_SOURCES) $(HEADERS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(RUNTIME_TEST_SOURCES) -pthread

$(APPLICATION_DISCOVERY_TARGET): $(APPLICATION_DISCOVERY_SOURCES) $(HEADERS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(APPLICATION_DISCOVERY_SOURCES)

$(APPLICATION_DISCOVERY_TEST_TARGET): $(APPLICATION_DISCOVERY_TEST_SOURCES) $(HEADERS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(APPLICATION_DISCOVERY_TEST_SOURCES)

$(APPLICATION_MANAGER_TARGET): $(APPLICATION_MANAGER_SOURCES) $(HEADERS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(APPLICATION_MANAGER_SOURCES) -pthread

$(APPLICATION_MANAGER_TEST_TARGET): $(APPLICATION_MANAGER_TEST_SOURCES) $(HEADERS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(APPLICATION_MANAGER_TEST_SOURCES) -pthread

$(WORKER_POOL_TEST_TARGET): $(WORKER_POOL_TEST_SOURCES) $(HEADERS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(WORKER_POOL_TEST_SOURCES) -pthread

$(APPLICATION_RUNTIME_TEST_TARGET): $(APPLICATION_RUNTIME_TEST_SOURCES) $(HEADERS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(APPLICATION_RUNTIME_TEST_SOURCES) -pthread

$(CONTINUOUS_MONITOR_TEST_TARGET): $(CONTINUOUS_MONITOR_TEST_SOURCES) $(HEADERS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(CONTINUOUS_MONITOR_TEST_SOURCES) -pthread $(NUMA_LDLIBS)

$(AWAVMA_RUNTIME_TARGET): $(AWAVMA_RUNTIME_SOURCES) $(HEADERS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(AWAVMA_RUNTIME_SOURCES) -pthread $(NUMA_LDLIBS) -lm

$(AWAVMA_RUNTIME_TEST_TARGET): $(AWAVMA_RUNTIME_TEST_SOURCES) $(HEADERS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(AWAVMA_RUNTIME_TEST_SOURCES) -pthread $(NUMA_LDLIBS) -lm

$(RUNTIME_TARGET_FILTER_TEST_TARGET): $(RUNTIME_TARGET_FILTER_TEST_SOURCES) $(HEADERS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(RUNTIME_TARGET_FILTER_TEST_SOURCES)

$(DISCOVERY_CADENCE_TEST_TARGET): $(DISCOVERY_CADENCE_TEST_SOURCES) $(HEADERS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(DISCOVERY_CADENCE_TEST_SOURCES) -pthread $(NUMA_LDLIBS)

$(DISCOVERY_CADENCE_PROBE_TARGET): $(DISCOVERY_CADENCE_PROBE_SOURCES) $(HEADERS) | bin
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(DISCOVERY_CADENCE_PROBE_SOURCES) -pthread $(NUMA_LDLIBS)

clean:
	rm -f $(BENCHMARK_TARGET) $(MONITOR_TARGET) $(TEST_TARGET) $(CLASSIFIER_TARGET) $(DECISION_TARGET) $(VALIDATION_TARGET) $(VALIDATION_TEST_TARGET) $(MIGRATION_TARGET) $(MIGRATION_TEST_TARGET) $(FEEDBACK_TARGET) $(FEEDBACK_TEST_TARGET) $(RUNTIME_TARGET) $(RUNTIME_TEST_TARGET) $(APPLICATION_DISCOVERY_TARGET) $(APPLICATION_DISCOVERY_TEST_TARGET) $(APPLICATION_MANAGER_TARGET) $(APPLICATION_MANAGER_TEST_TARGET) $(WORKER_POOL_TEST_TARGET) $(APPLICATION_RUNTIME_TEST_TARGET) $(CONTINUOUS_MONITOR_TEST_TARGET) $(AWAVMA_RUNTIME_TARGET) $(AWAVMA_RUNTIME_TEST_TARGET) $(RUNTIME_TARGET_FILTER_TEST_TARGET) $(DISCOVERY_CADENCE_TEST_TARGET) $(DISCOVERY_CADENCE_PROBE_TARGET) $(PROFILE_AWAVMA_RUNTIME_TARGET) bin/profile-monitor bin/profile-application-discovery-test bin/profile-continuous-monitor-test
