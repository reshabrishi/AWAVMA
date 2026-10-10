#define _POSIX_C_SOURCE 200809L

#include "awavma_runtime.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int contains(const char *path, const char *text)
{
    char line[2048];
    FILE *file = fopen(path, "r");
    int found = 0;

    if (file == NULL)
        return 0;
    while (fgets(line, sizeof(line), file) != NULL)
        if (strstr(line, text) != NULL)
            found = 1;
    fclose(file);
    return found;
}

static int invalid_execution_case(void)
{
    char root[] = "/tmp/awavma-profile-invalid-XXXXXX";
    char profile[PATH_MAX];
    awavma_runtime_config_t config;
    awavma_runtime_t *runtime;
    int passed;

    if (mkdtemp(root) == NULL)
        return 0;
    awavma_runtime_config_default(&config);
    config.root_dir = root;
    config.execution_profile.migration_execution_requested = true;
    runtime = awavma_runtime_create();
    passed = runtime != NULL && awavma_runtime_init(runtime, &config) != 0;
    if (snprintf(profile, sizeof(profile), "%s/runtime_execution_profile.csv", root) >=
        (int)sizeof(profile)) {
        awavma_runtime_destroy(runtime);
        return 0;
    }
    passed = passed && contains(profile, "INCOMPATIBLE,migration execution requires migration safety");
    awavma_runtime_destroy(runtime);
    return passed;
}

static int invalid_registration_case(void)
{
    char root[] = "/tmp/awavma-profile-registration-XXXXXX";
    char profile[PATH_MAX];
    awavma_runtime_config_t config;
    awavma_runtime_t *runtime;
    int passed;

    if (mkdtemp(root) == NULL)
        return 0;
    awavma_runtime_config_default(&config);
    config.root_dir = root;
    config.execution_profile.page_registration_requested = true;
    config.execution_profile.page_registration_ttl_ms = 0;
    runtime = awavma_runtime_create();
    passed = runtime != NULL && awavma_runtime_init(runtime, &config) != 0;
    snprintf(profile, sizeof(profile), "%s/runtime_execution_profile.csv", root);
    passed = passed && contains(profile, "INCOMPATIBLE,page registration requires a positive TTL");
    awavma_runtime_destroy(runtime);
    return passed;
}

static int incomplete_production_case(void)
{
    char root[] = "/tmp/awavma-profile-incomplete-XXXXXX";
    char profile[PATH_MAX];
    awavma_runtime_config_t config;
    awavma_runtime_t *runtime;
    int passed;

    if (mkdtemp(root) == NULL)
        return 0;
    awavma_runtime_config_default(&config);
    config.root_dir = root;
    config.execution_profile.requested_mode = AWAVMA_RUNTIME_EXECUTION_PRODUCTION_REAL_MIGRATION;
    config.execution_profile.migration_safety_requested = true;
    runtime = awavma_runtime_create();
    passed = runtime != NULL && awavma_runtime_init(runtime, &config) != 0;
    snprintf(profile, sizeof(profile), "%s/runtime_execution_profile.csv", root);
    passed = passed && contains(profile, "INCOMPATIBLE,production mode requires safety execution and page registration");
    awavma_runtime_destroy(runtime);
    return passed;
}

static int production_case(void)
{
    char root[] = "/tmp/awavma-profile-production-XXXXXX";
    char cwd[PATH_MAX], bin_dir[PATH_MAX + 32], config_path[PATH_MAX + 32], profile[PATH_MAX];
    EnvironmentCapabilities capabilities;
    awavma_runtime_config_t config;
    awavma_runtime_t *runtime;
    int passed;

    if (mkdtemp(root) == NULL || getcwd(cwd, sizeof(cwd)) == NULL ||
        !environment_capabilities_detect(&capabilities))
        return 0;
    snprintf(bin_dir, sizeof(bin_dir), "%s/bin", cwd);
    snprintf(config_path, sizeof(config_path), "%s/config/awavma.conf", cwd);
    snprintf(profile, sizeof(profile), "%s/runtime_execution_profile.csv", root);
    awavma_runtime_config_default(&config);
    config.root_dir = root;
    config.bin_dir = bin_dir;
    config.phase_config_path = config_path;
    config.execution_profile.requested_mode = AWAVMA_RUNTIME_EXECUTION_PRODUCTION_REAL_MIGRATION;
    config.execution_profile.migration_safety_requested = true;
    config.execution_profile.migration_execution_requested = true;
    config.execution_profile.page_registration_requested = true;
    runtime = awavma_runtime_create();
    passed = runtime != NULL;
    if (capabilities.production_real_migration_ready) {
        passed = passed && awavma_runtime_init(runtime, &config) != 0 &&
                 contains(profile, ",CALIBRATION_UNTRUSTED,");
    } else {
        passed = passed && awavma_runtime_init(runtime, &config) != 0 &&
                 contains(profile, ",ENV_LIMITED,production real-migration capability profile is unavailable");
    }
    awavma_runtime_destroy(runtime);
    return passed;
}

static int long_root_socket_case(void)
{
    char base[] = "/tmp/awavma-long-root-XXXXXX", root[PATH_MAX], profile[PATH_MAX];
    awavma_runtime_config_t config;
    awavma_runtime_t *runtime;
    int passed;

    if (mkdtemp(base) == NULL)
        return 0;
    snprintf(root, sizeof(root), "%s/%080d", base, 1);
    awavma_runtime_config_default(&config);
    config.root_dir = root;
    config.execution_profile.page_registration_requested = true;
    runtime = awavma_runtime_create();
    passed = runtime != NULL && awavma_runtime_init(runtime, &config) == 0;
    if (snprintf(profile, sizeof(profile), "%s/runtime_execution_profile.csv", root) >=
        (int)sizeof(profile)) {
        awavma_runtime_destroy(runtime);
        return 0;
    }
    passed = passed && contains(profile, "page_registration_socket,worker_evidence_socket") &&
             contains(profile, "/page-registration.sock,/" );
    awavma_runtime_destroy(runtime);
    return passed;
}

int main(void)
{
    int passed = invalid_execution_case() && invalid_registration_case() &&
                 incomplete_production_case() && production_case() && long_root_socket_case();
    printf("runtime_execution_profile_test: %s\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
