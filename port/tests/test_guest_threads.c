/* The guest's job system on the port's own primitives: where the frame's time goes.
 *
 * The port maps the guest's threads one to one onto host threads and the guest's job
 * workers all park on ONE condition variable (the freeze reports of 2026-10-19 show
 * JobWorker1..5 waiting on the same condvar for 8-11 s while the main thread spins in
 * guest code). This bench reproduces that shape with the real runtime code:
 *
 *   - N guest threads (created through scePthreadCreate) pop jobs from a queue protected
 *     by a guest mutex and park on a guest condition variable;
 *   - the main thread posts a job and waits for it;
 *   - the round trip (post -> job done) is measured, and so is the wake-up latency (the
 *     post -> the moment the woken thread runs) that runtime_thread.c now accounts for.
 *
 * It also measures the two things the mission asks about that are not visible in the log:
 * the cost of scePthreadYield (the guest's spin) and the effect of the number of runnable
 * threads on the wake-up latency (the machine has 12 logical CPUs and the process has ~77
 * threads in the game).
 *
 * Usage: guest-threads-test [--quick]
 * It needs no game, no GPU and no Vulkan.
 */
#define _GNU_SOURCE
#include "runtime.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* The runtime files the bench links do not need the loader or the GPU library. */
void runtime_report(void) { }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) {
    (void)table; (void)count; (void)nid;
    return 0;
}

/* ---- the guest ABI, reached through the same resolve tables the loader uses ---- */
typedef void *(ABI *GuestEntry)(void *);
typedef int32_t (ABI *ThreadCreate)(void **out, void **attr, GuestEntry entry, void *argument, const char *name);
typedef int32_t (ABI *ThreadAttrInit)(void **out);
typedef int32_t (ABI *ThreadJoin)(void *thread, void **result);
typedef int32_t (ABI *ThreadYield)(void);
typedef int32_t (ABI *MutexInit)(void **out, void **attr, const char *name);
typedef int32_t (ABI *MutexLock)(void **mutex);
typedef int32_t (ABI *MutexUnlock)(void **mutex);
typedef int32_t (ABI *CondInit)(void **out, void **attr, const char *name);
typedef int32_t (ABI *CondWait)(void **cond, void **mutex);
typedef int32_t (ABI *CondSignal)(void **cond);
typedef int32_t (ABI *CondBroadcast)(void **cond);
typedef int32_t (ABI *SemCreate)(uint32_t *out, const char *name, uint32_t attr, int32_t initial, int32_t maximum, const void *options);
typedef int32_t (ABI *SemWait)(uint32_t id, int32_t need, uint32_t *timeout);
typedef int32_t (ABI *SemSignal)(uint32_t id, int32_t count);

static uintptr_t resolve(uintptr_t (*table)(const char *), const char *nid) {
    uintptr_t fn = table(nid);
    if (!fn) { fprintf(stderr, "STOP: %s not resolved\n", nid); exit(2); }
    return fn;
}

/* ---- clock ---- */
static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t) t.tv_sec * 1000000000u + (uint64_t) t.tv_nsec;
}
static void sleep_ns(uint64_t ns) {
#ifdef _WIN32
    compat_sleep_ns(ns);
#else
    struct timespec t = {(time_t)(ns / 1000000000u), (long)(ns % 1000000000u)};
    nanosleep(&t, NULL);
#endif
}
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}
static unsigned logical_cpus(void) {
#ifdef _WIN32
    return GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (unsigned) n : 1u;
#endif
}

/* ---- the guest's job system ---- */
typedef struct JobSystem {
    void *mutex, *cond, *done_cond;
    int jobs, done, stop, workers;
    uint64_t *round_trip;      /* post -> job done, one per round */
    int rounds;
} JobSystem;

static MutexLock g_mutex_lock;
static MutexUnlock g_mutex_unlock;
static CondWait g_cond_wait;
static CondSignal g_cond_signal;
static CondBroadcast g_cond_broadcast;

/* Background threads that burn CPU, like the port's recorder does in the game: they are the
 * reason a wake-up takes tens of microseconds there. */
typedef struct Spinner { volatile int *stop; } Spinner;
static void *(ABI spinner_entry)(void *argument) {
    Spinner *s = (Spinner *) argument;
    volatile uint64_t sink = 0;
    while (!*s->stop) {
        for (int i = 0; i < 1000; ++i) sink += (uint64_t) i * 2654435761u;
    }
    (void) sink;
    return NULL;
}

static void *(ABI worker_entry)(void *argument) {
    JobSystem *js = (JobSystem *) argument;
    for (;;) {
        g_mutex_lock(&js->mutex);
        while (!js->jobs && !js->stop) g_cond_wait(&js->cond, &js->mutex);
        if (js->stop) { g_mutex_unlock(&js->mutex); break; }
        js->jobs--;
        g_mutex_unlock(&js->mutex);
        /* The job itself (the guest's work would be here). */
        g_mutex_lock(&js->mutex);
        js->done++;
        g_cond_signal(&js->done_cond);
        g_mutex_unlock(&js->mutex);
    }
    return NULL;
}

/* The job system with N workers: posts `rounds` jobs one at a time and measures the round
 * trip of each one. `spinners` background threads burn CPU meanwhile, as the port's
 * recorder and the driver do in the game (the process has ~77 threads there). */
static void job_system_round(uintptr_t thread_create, uintptr_t thread_attr_init,
                             uintptr_t thread_join, unsigned workers, int rounds,
                             unsigned spinners, uint64_t *out_p50, uint64_t *out_p95,
                             uint64_t *out_max) {
    ThreadCreate create = (ThreadCreate) thread_create;
    ThreadAttrInit attr_init = (ThreadAttrInit) thread_attr_init;
    ThreadJoin join = (ThreadJoin) thread_join;
    MutexInit mutex_init = (MutexInit) resolve(runtime_mutex_resolve, "cmo1RIYva9o#libkernel");
    CondInit cond_init = (CondInit) resolve(runtime_mutex_resolve, "2Tb92quprl0#libkernel");
    JobSystem js;
    memset(&js, 0, sizeof(js));
    js.workers = (int) workers;
    js.rounds = rounds;
    js.round_trip = (uint64_t *) calloc((size_t) rounds, sizeof(uint64_t));
    if (mutex_init(&js.mutex, NULL, "jobMutex")) { fprintf(stderr, "STOP: mutex_init\n"); exit(2); }
    if (cond_init(&js.cond, NULL, "jobCond")) { fprintf(stderr, "STOP: cond_init\n"); exit(2); }
    if (cond_init(&js.done_cond, NULL, "jobDone")) { fprintf(stderr, "STOP: cond_init\n"); exit(2); }
    void *attr = NULL;
    attr_init(&attr);
    void *threads[64];
    for (unsigned i = 0; i < workers && i < 64; ++i) {
        if (create(&threads[i], &attr, worker_entry, &js, "benchWorker")) {
            fprintf(stderr, "STOP: thread_create\n");
            exit(2);
        }
    }
    /* The workers park on the condvar; give them time to get there. */
    sleep_ns(50 * 1000 * 1000);
    /* The background load, as in the game. */
    int stop = 0;
    Spinner spin;
    spin.stop = &stop;
    void *spinner_threads[64];
    for (unsigned i = 0; i < spinners && i < 64; ++i)
        create(&spinner_threads[i], &attr, spinner_entry, &spin, "benchSpinner");
    for (int r = 0; r < rounds; ++r) {
        const uint64_t posted = now_ns();
        g_mutex_lock(&js.mutex);
        js.jobs++;
        g_cond_signal(&js.cond);
        while (js.done <= r) g_cond_wait(&js.done_cond, &js.mutex);
        g_mutex_unlock(&js.mutex);
        js.round_trip[r] = now_ns() - posted;
    }
    stop = 1;
    for (unsigned i = 0; i < spinners && i < 64; ++i) join(spinner_threads[i], NULL);
    g_mutex_lock(&js.mutex);
    js.stop = 1;
    g_cond_broadcast(&js.cond);
    g_mutex_unlock(&js.mutex);
    for (unsigned i = 0; i < workers && i < 64; ++i) join(threads[i], NULL);
    qsort(js.round_trip, (size_t) rounds, sizeof(uint64_t), cmp_u64);
    if (out_p50) *out_p50 = js.round_trip[rounds / 2];
    if (out_p95) *out_p95 = js.round_trip[(size_t) (rounds * 95 / 100)];
    if (out_max) *out_max = js.round_trip[rounds - 1];
    free(js.round_trip);
}

/* ---- a contended guest mutex, the way the guest's job queue uses it ---- */
typedef struct MutexBench {
    void *mutex;
    int iterations;
    uint64_t *samples;          /* the time each acquisition took */
    volatile int *counter;
    volatile int *stop;
    int critical_ns;             /* how long the critical section lasts */
} MutexBench;

/* A busy loop of about `ns` nanoseconds (the critical section of the job queue). */
static void burn_ns(int ns) {
    if (ns <= 0) return;
    const uint64_t end = now_ns() + (uint64_t) ns;
    volatile uint64_t sink = 0;
    while (now_ns() < end) sink++;
    (void) sink;
}

static void *(ABI hammer_entry)(void *argument) {
    MutexBench *mb = (MutexBench *) argument;
    for (int i = 0; i < mb->iterations; ++i) {
        const uint64_t before = now_ns();
        g_mutex_lock(&mb->mutex);
        const uint64_t after = now_ns();
        /* The critical section: the guest's job queue pop is short, but the dispatch can
         * hold the lock a little longer. */
        ++*mb->counter;
        burn_ns(mb->critical_ns);
        g_mutex_unlock(&mb->mutex);
        mb->samples[i] = after - before;
    }
    return NULL;
}

/* Background threads that burn CPU, like the port's recorder does in the game: they are the
 * reason a wake-up takes tens of microseconds there. */

/* N threads on one guest mutex: the acquisition latency and the throughput. */
static void mutex_contention(uintptr_t thread_create, uintptr_t thread_attr_init,
                              uintptr_t thread_join, unsigned workers, int iterations,
                              unsigned spinners, int critical_ns, uint64_t *out_p50,
                              uint64_t *out_p95, uint64_t *out_max, double *out_ms) {
    ThreadCreate create = (ThreadCreate) thread_create;
    ThreadAttrInit attr_init = (ThreadAttrInit) thread_attr_init;
    ThreadJoin join = (ThreadJoin) thread_join;
    MutexInit mutex_init = (MutexInit) resolve(runtime_mutex_resolve, "cmo1RIYva9o#libkernel");
    MutexBench mb;
    memset(&mb, 0, sizeof(mb));
    mb.iterations = iterations;
    mb.critical_ns = critical_ns;
    int counter = 0, stop = 0;
    mb.counter = &counter;
    mb.stop = &stop;
    mb.samples = (uint64_t *) calloc((size_t) workers * (size_t) iterations, sizeof(uint64_t));
    if (mutex_init(&mb.mutex, NULL, "benchMutex")) { fprintf(stderr, "STOP: mutex_init\n"); exit(2); }
    void *attr = NULL;
    attr_init(&attr);
    void *threads[64];
    for (unsigned i = 0; i < workers && i < 64; ++i)
        create(&threads[i], &attr, hammer_entry, &mb, "benchHammer");
    Spinner spin;
    spin.stop = &stop;
    void *spinner_threads[64];
    for (unsigned i = 0; i < spinners && i < 64; ++i)
        create(&spinner_threads[i], &attr, spinner_entry, &spin, "benchSpinner");
    const uint64_t start = now_ns();
    for (unsigned i = 0; i < workers && i < 64; ++i) join(threads[i], NULL);
    const uint64_t spent = now_ns() - start;
    stop = 1;
    for (unsigned i = 0; i < spinners && i < 64; ++i) join(spinner_threads[i], NULL);
    const size_t total = (size_t) workers * (size_t) iterations;
    qsort(mb.samples, total, sizeof(uint64_t), cmp_u64);
    if (out_p50) *out_p50 = mb.samples[total / 2];
    if (out_p95) *out_p95 = mb.samples[total * 95 / 100];
    if (out_max) *out_max = mb.samples[total - 1];
    if (out_ms) *out_ms = (double) spent / 1e6;
    free(mb.samples);
}

/* ---- the cost of scePthreadYield (the guest's spin) ---- */
static void yield_cost(uintptr_t thread_yield) {
    ThreadYield yields = (ThreadYield) thread_yield;
    const int rounds = 20000;
    const uint64_t start = now_ns();
    for (int i = 0; i < rounds; ++i) yields();
    const uint64_t spent = now_ns() - start;
    /* And what a plain pause would cost, for the comparison. */
    const uint64_t start2 = now_ns();
    for (int i = 0; i < rounds; ++i) {
#ifdef _WIN32
        YieldProcessor();
#else
        __asm__ __volatile__("pause" ::: "memory");
#endif
    }
    const uint64_t spent2 = now_ns() - start2;
    printf("  scePthreadYield            : %8.0f ns/call  (%lld calls in %.1f ms)\n",
           (double) spent / rounds, (long long) rounds, (double) spent / 1e6);
    printf("  YieldProcessor (no syscall): %8.0f ns/call\n", (double) spent2 / rounds);
}

/* ---- the wait map of the runtime (the instrumentation this agent added) ---- */
static void print_wait_map(void) {
    RuntimeThreadInfo infos[64];
    const size_t count = runtime_thread_snapshot(infos, 64);
    printf("  threads in the map: %zu\n", count);
    for (size_t i = 0; i < count; ++i) {
        const RuntimeWait *w = &infos[i].wait;
        if (!w->wait_count && !w->wake_count) continue;
        printf("    %s %s (tid %llu): %u waits, %.1f ms blocked, %u wakes, mean wake %.1f us, "
               "max %.1f us, spurious %u\n",
               infos[i].record ? "guest" : "host ", infos[i].record ? infos[i].name : "thread",
               (unsigned long long) infos[i].os_tid, w->wait_count, (double) w->wait_ns / 1e6,
               w->wake_count,
               (double) (w->wake_count ? w->wake_ns / w->wake_count : 0) / 1e3,
               (double) w->wake_max_ns / 1e3, w->spurious);
    }
}

int main(int argc, char **argv) {
    const int quick = (argc > 1 && !strcmp(argv[1], "--quick"));
    const unsigned cpus = logical_cpus();
    const int rounds = quick ? 200 : 2000;

    /* The loader does this: it gives the main thread its guest record and starts the
     * monitor of the wait map (TLG_WAIT_STATS=1). */
    runtime_thread_attach_main();

    uintptr_t thread_create = resolve(runtime_thread_resolve, "6UgtwV+0zb4#libkernel");
    uintptr_t thread_attr_init = resolve(runtime_thread_resolve, "nsYoNRywwNg#libkernel");
    uintptr_t thread_join = resolve(runtime_thread_resolve, "onNY9Byn-W8#libkernel");
    uintptr_t thread_yield = resolve(runtime_thread_resolve, "T72hz6ffq08#libkernel");
    g_mutex_lock = (MutexLock) resolve(runtime_mutex_resolve, "9UK1vLZQft4#libkernel");
    g_mutex_unlock = (MutexUnlock) resolve(runtime_mutex_resolve, "tn3VlD0hG60#libkernel");
    g_cond_wait = (CondWait) resolve(runtime_mutex_resolve, "WKAXJ4XBPQ4#libkernel");
    g_cond_signal = (CondSignal) resolve(runtime_mutex_resolve, "kDh-NfxgMtE#libkernel");
    g_cond_broadcast = (CondBroadcast) resolve(runtime_mutex_resolve, "JGgj7Uvrl+A#libkernel");

    printf("== the guest's job system on the port's primitives (host: %u logical CPUs) ==\n", cpus);
    printf("  the workers park on ONE condition variable, as in the game's freeze reports\n");
    printf("  the round trip is post -> the job is done: it is two wake-ups (the worker and\n"
           "  the main thread) plus the queue mutex\n");
    const unsigned worker_counts[] = {1, 5};
    const unsigned spinner_counts[] = {0, 12, 36};
    for (unsigned s = 0; s < sizeof(spinner_counts) / sizeof(*spinner_counts); ++s) {
        for (unsigned i = 0; i < sizeof(worker_counts) / sizeof(*worker_counts); ++i) {
            uint64_t p50 = 0, p95 = 0, max = 0;
            job_system_round(thread_create, thread_attr_init, thread_join, worker_counts[i], rounds,
                             spinner_counts[s], &p50, &p95, &max);
            printf("  %u workers, %2u spinning threads: round trip p50 %8.1f us, p95 %8.1f us, "
                   "max %8.1f us\n",
                   worker_counts[i], spinner_counts[s], (double) p50 / 1e3, (double) p95 / 1e3,
                   (double) max / 1e3);
        }
    }

    printf("== a contended guest mutex (the guest's job queue) ==\n");
    printf("  TLG_MUTEX_SPIN=%s (the adaptive spin of the contended acquire)\n",
           getenv("TLG_MUTEX_SPIN") ? getenv("TLG_MUTEX_SPIN") : "off");
    const int criticals[] = {0, 200, 2000};
    for (unsigned c = 0; c < sizeof(criticals) / sizeof(*criticals); ++c) {
        uint64_t p50 = 0, p95 = 0, max = 0;
        double ms = 0;
        mutex_contention(thread_create, thread_attr_init, thread_join, 8, quick ? 2000 : 10000,
                         0, criticals[c], &p50, &p95, &max, &ms);
        printf("  8 threads, critical section %5d ns: acquisition p50 %8.1f us, p95 %8.1f us, "
               "max %8.1f us, %.0f ms total\n",
               criticals[c], (double) p50 / 1e3, (double) p95 / 1e3, (double) max / 1e3, ms);
    }

    printf("== the guest's spin (scePthreadYield) ==\n");
    yield_cost(thread_yield);

    printf("== the wait map of the bench (TLG_WAIT_STATS=1) ==\n");
    print_wait_map();
    /* The monitor of runtime_thread.c prints the map of every 5 s window; with this variable
     * the bench stays alive long enough to see two of them. */
    if (getenv("TLG_GUEST_THREADS_MAP")) {
        printf("== the windows of the monitor (5 s each) ==\n");
        fflush(stdout);
        sleep_ns(12ull * 1000 * 1000 * 1000);
    }
    printf("PASS: guest threads bench\n");
    return 0;
}
