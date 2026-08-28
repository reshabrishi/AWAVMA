#ifndef AWAVMA_MONITOR_PROFILE_H
#define AWAVMA_MONITOR_PROFILE_H

#include <stdint.h>
#include <sys/types.h>

typedef struct {
    const char *level;
    const char *component;
    const char *application;
    pid_t pid;
    uint64_t job_id;
    uint64_t start_ns;
} monitor_profile_scope_t;

#ifdef AWAVMA_PROFILE

uint64_t monitor_profile_now_ns(void);
void monitor_profile_scope_begin(monitor_profile_scope_t *scope,
                                 const char *level, const char *component,
                                 const char *application, pid_t pid,
                                 uint64_t job_id);
void monitor_profile_scope_end(monitor_profile_scope_t *scope,
                               const char *status);
void monitor_profile_event(const char *level, const char *component,
                           const char *application, pid_t pid,
                            uint64_t job_id, uint64_t start_ns,
                            uint64_t end_ns, const char *status);
void monitor_profile_counter(const char *level, const char *component,
                             const char *application, pid_t pid,
                             uint64_t job_id, uint64_t value,
                             const char *status);

#else

static inline uint64_t monitor_profile_now_ns(void)
{
    return 0;
}

static inline void monitor_profile_scope_begin(monitor_profile_scope_t *scope,
                                               const char *level,
                                               const char *component,
                                               const char *application,
                                               pid_t pid, uint64_t job_id)
{
    (void)scope;
    (void)level;
    (void)component;
    (void)application;
    (void)pid;
    (void)job_id;
}

static inline void monitor_profile_scope_end(monitor_profile_scope_t *scope,
                                             const char *status)
{
    (void)scope;
    (void)status;
}

static inline void monitor_profile_event(const char *level, const char *component,
                                         const char *application, pid_t pid,
                                         uint64_t job_id, uint64_t start_ns,
                                         uint64_t end_ns, const char *status)
{
    (void)level;
    (void)component;
    (void)application;
    (void)pid;
    (void)job_id;
    (void)start_ns;
    (void)end_ns;
    (void)status;
}

static inline void monitor_profile_counter(const char *level, const char *component,
                                           const char *application, pid_t pid,
                                           uint64_t job_id, uint64_t value,
                                           const char *status)
{
    (void)level;
    (void)component;
    (void)application;
    (void)pid;
    (void)job_id;
    (void)value;
    (void)status;
}

#endif

#endif
