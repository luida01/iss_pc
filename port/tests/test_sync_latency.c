/* Latency and overhead of the port's synchronization primitives (guest mutex, condition
 * variable and semaphore) and of the guest thread mapping (one guest thread = one host
 * pthread). Reproducible bench: no game, no GPU, no Vulkan, no Vulkan driver.
 *
 * What it answers (the numbers the analysis needs):
 *   - how long a lock/unlock of a guest mutex costs, alone and under contention;
 *   - how long a wake-up costs (signal -> the waiter is running again), alone and with
 *     many waiters on the same object;
 *   - how much of that is the port and how much is the host (winpthreads): the same
 *     shapes are measured with raw pthreads;
 *   - how much the port adds to each wait (the wait-slot bookkeeping, the semaphore's
 *     per-wait condition variable);
 *   - how long a guest thread costs to create and to join;
 *   - how much a lowered host priority costs in wake-up latency (the guest priority
 *     mapping of runtime_thread.c lowers threads; the bench measures what that buys).
 *
 * Usage: sync-latency-test [--quick]
 * The numbers are printed as "name: value unit"; they are not thresholds (the machine
 * varies), but the test fails if a primitive is broken (a lost wake-up, a hang).
 */
#define _GNU_SOURCE
#include "runtime.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The settings menu restarts through probe.c, which the tests do not link. */
void runtime_report(void) { }
/* The NID table lives in runtime.c, which needs the GPU library; the bench does not. */
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) {
    (void)table; (void)count; (void)nid;
    return 0;
}

/* ---- host clock ---- */
static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
/* winpthreads' nanosleep() rounds the request to the scheduler tick (and truncates a
 * sub-millisecond request to zero), so the bench sleeps with the port's own
 * high-resolution timer instead. */
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
static uint64_t pick(uint64_t *v, size_t n, unsigned percentile) {
    if (!n) return 0;
    qsort(v, n, sizeof(*v), cmp_u64);
    size_t index = (n * percentile) / 100;
    if (index >= n) index = n - 1;
    return v[index];
}
#define SAMPLES 2000

/* ---- the guest API, as the game sees it ---- */
typedef int32_t (ABI *AttrInit)(void **out);
typedef int32_t (ABI *AttrType)(void **attr, int type);
typedef int32_t (ABI *AttrDestroy)(void **attr);
typedef int32_t (ABI *MutexInit)(void **out, void **attr, const char *name);
typedef int32_t (ABI *MutexOp)(void **mutex);
typedef int32_t (ABI *CondInit)(void **out, void **attr, const char *name);
typedef int32_t (ABI *CondWait)(void **cond, void **mutex);
typedef int32_t (ABI *CondSignal)(void **cond);
typedef int32_t (ABI *CondDestroy)(void **cond);
typedef int32_t (ABI *SemCreate)(uint32_t *out, const char *name, uint32_t attr, int32_t initial,
                                 int32_t maximum, const void *options);
typedef int32_t (ABI *SemWait)(uint32_t id, int32_t need, uint32_t *timeout);
typedef int32_t (ABI *SemSignal)(uint32_t id, int32_t count);
typedef void *(ABI *GuestEntry)(void *);
typedef int32_t (ABI *ThreadCreate)(void **out, void **attr, GuestEntry entry, void *argument,
                                    const char *name);
typedef int32_t (ABI *ThreadJoin)(void *thread, void **result);

static AttrInit attr_init;
static AttrType attr_type;
static AttrDestroy attr_destroy;
static MutexInit mutex_init;
static MutexOp mutex_lock, mutex_unlock;
static CondInit cond_init;
static CondWait cond_wait;
static CondSignal cond_signal, cond_broadcast;
static CondDestroy cond_destroy;
static SemCreate sem_create;
static SemWait sem_wait;
static SemSignal sem_signal;
static ThreadCreate thread_create;
static ThreadJoin thread_join;

static void resolve(void) {
    attr_init = (AttrInit)(uintptr_t)runtime_mutex_resolve("F8bUHwAG284#libkernel");
    attr_type = (AttrType)(uintptr_t)runtime_mutex_resolve("iMp8QpE+XO4#libkernel");
    attr_destroy = (AttrDestroy)(uintptr_t)runtime_mutex_resolve("smWEktiyyG0#libkernel");
    mutex_init = (MutexInit)(uintptr_t)runtime_mutex_resolve("cmo1RIYva9o#libkernel");
    mutex_lock = (MutexOp)(uintptr_t)runtime_mutex_resolve("9UK1vLZQft4#libkernel");
    mutex_unlock = (MutexOp)(uintptr_t)runtime_mutex_resolve("tn3VlD0hG60#libkernel");
    cond_init = (CondInit)(uintptr_t)runtime_mutex_resolve("2Tb92quprl0#libkernel");
    cond_wait = (CondWait)(uintptr_t)runtime_mutex_resolve("WKAXJ4XBPQ4#libkernel");
    cond_signal = (CondSignal)(uintptr_t)runtime_mutex_resolve("kDh-NfxgMtE#libkernel");
    cond_broadcast = (CondSignal)(uintptr_t)runtime_mutex_resolve("JGgj7Uvrl+A#libkernel");
    cond_destroy = (CondDestroy)(uintptr_t)runtime_mutex_resolve("g+PZd2hiacg#libkernel");
    sem_create = (SemCreate)(uintptr_t)runtime_sema_resolve("188x57JYp0g#libkernel");
    sem_wait = (SemWait)(uintptr_t)runtime_sema_resolve("Zxa0VhQVTsk#libkernel");
    sem_signal = (SemSignal)(uintptr_t)runtime_sema_resolve("4czppHBiriw#libkernel");
    thread_create = (ThreadCreate)(uintptr_t)runtime_thread_resolve("6UgtwV+0zb4#libkernel");
    thread_join = (ThreadJoin)(uintptr_t)runtime_thread_resolve("onNY9Byn-W8#libkernel");
    assert(attr_init && attr_type && attr_destroy && mutex_init && mutex_lock && mutex_unlock);
    assert(cond_init && cond_wait && cond_signal && cond_broadcast && cond_destroy);
    assert(sem_create && sem_wait && sem_signal && thread_create && thread_join);
}

/* ---- M1: an uncontended guest mutex ---- */
static void bench_mutex_alone(unsigned iterations) {
    void *attr = NULL, *mutex = NULL;
    assert(attr_init(&attr) == 0 && attr_type(&attr, 1) == 0);
    assert(mutex_init(&mutex, &attr, "bench") == 0);
    uint64_t t0 = now_ns();
    for (unsigned i = 0; i < iterations; ++i) {
        assert(mutex_lock(&mutex) == 0);
        assert(mutex_unlock(&mutex) == 0);
    }
    uint64_t total = now_ns() - t0;
    printf("mutex lock+unlock alone      : %8.1f ns/pair  (%u pairs in %.1f ms)\n",
           (double)total / iterations, iterations, (double)total / 1e6);
    attr_destroy(&attr);
}

/* ---- M2: a contended guest mutex (the convoy) ---- */
typedef struct {
    void **mutex;
    atomic_int *stop;
    unsigned acquisitions;
    uint64_t total_ns, max_ns;
} Hammer;
static void *hammer(void *argument) {
    Hammer *h = argument;
    runtime_thread_attach_host("bench-hammer");
    while (!atomic_load(h->stop)) {
        uint64_t t0 = now_ns();
        mutex_lock(h->mutex);
        uint64_t t1 = now_ns();
        mutex_unlock(h->mutex);
        uint64_t d = t1 - t0;
        h->total_ns += d;
        if (d > h->max_ns) h->max_ns = d;
        ++h->acquisitions;
    }
    return NULL;
}
static void bench_mutex_contended(unsigned threads, unsigned milliseconds) {
    void *attr = NULL, *mutex = NULL;
    assert(attr_init(&attr) == 0 && attr_type(&attr, 1) == 0);
    assert(mutex_init(&mutex, &attr, "bench") == 0);
    atomic_int stop = 0;
    Hammer *hammers = calloc(threads, sizeof(*hammers));
    pthread_t *ids = calloc(threads, sizeof(*ids));
    for (unsigned i = 0; i < threads; ++i) {
        hammers[i].mutex = &mutex;
        hammers[i].stop = &stop;
        assert(pthread_create(&ids[i], NULL, hammer, &hammers[i]) == 0);
    }
    sleep_ns((uint64_t)milliseconds * 1000000u);
    atomic_store(&stop, 1);
    unsigned acquisitions = 0;
    uint64_t total = 0, worst = 0;
    for (unsigned i = 0; i < threads; ++i) {
        pthread_join(ids[i], NULL);
        acquisitions += hammers[i].acquisitions;
        total += hammers[i].total_ns;
        if (hammers[i].max_ns > worst) worst = hammers[i].max_ns;
    }
    printf("guest mutex type 1 (ERRORCHECK), %u thr: %8.1f ns/acq  max %8.1f ns  (%u acq)\n",
           threads, acquisitions ? (double)total / acquisitions : 0.0, (double)worst, acquisitions);
    free(hammers);
    free(ids);
    attr_destroy(&attr);
}
/* The same with the guest's NORMAL type (the host's SRWLOCK instead of its
 * critical section): the difference is what the host mutex type costs. */
static void bench_mutex_contended_normal(unsigned threads, unsigned milliseconds) {
    void *attr = NULL, *mutex = NULL;
    assert(attr_init(&attr) == 0 && attr_type(&attr, 3) == 0);
    assert(mutex_init(&mutex, &attr, "bench") == 0);
    atomic_int stop = 0;
    Hammer *hammers = calloc(threads, sizeof(*hammers));
    pthread_t *ids = calloc(threads, sizeof(*ids));
    for (unsigned i = 0; i < threads; ++i) {
        hammers[i].mutex = &mutex;
        hammers[i].stop = &stop;
        assert(pthread_create(&ids[i], NULL, hammer, &hammers[i]) == 0);
    }
    sleep_ns((uint64_t)milliseconds * 1000000u);
    atomic_store(&stop, 1);
    unsigned acquisitions = 0;
    uint64_t total = 0, worst = 0;
    for (unsigned i = 0; i < threads; ++i) {
        pthread_join(ids[i], NULL);
        acquisitions += hammers[i].acquisitions;
        total += hammers[i].total_ns;
        if (hammers[i].max_ns > worst) worst = hammers[i].max_ns;
    }
    printf("guest mutex type 3 (NORMAL),    %u thr: %8.1f ns/acq  max %8.1f ns  (%u acq)\n",
           threads, acquisitions ? (double)total / acquisitions : 0.0, (double)worst, acquisitions);
    free(hammers);
    free(ids);
    attr_destroy(&attr);
}

/* ---- M3: the wake-up latency of a guest condition variable ---- */
typedef struct {
    void **cond, **mutex;
    atomic_int *armed;      /* the waiter is inside the wait */
    atomic_int *done;
    uint64_t *wait_ns;       /* time blocked in the wait */
    uint64_t *wake_ns;       /* signal -> the waiter runs again */
    unsigned count;
    uint64_t signal_at;
} Waiter;
static void *cond_waiter(void *argument) {
    Waiter *w = argument;
    runtime_thread_attach_host("bench-waiter");
    for (unsigned i = 0; i < w->count; ++i) {
        assert(mutex_lock(w->mutex) == 0);
        uint64_t t0 = now_ns();
        atomic_store(w->armed, 1);
        cond_wait(w->cond, w->mutex);
        uint64_t t1 = now_ns();
        atomic_store(w->armed, 0);
        assert(mutex_unlock(w->mutex) == 0);
        w->wait_ns[i] = t1 - t0;
        w->wake_ns[i] = t1 > w->signal_at ? t1 - w->signal_at : 0;
        atomic_fetch_add(w->done, 1);
    }
    return NULL;
}
/* One waiter, one signal at a time: the classic "the game waits for a worker" shape. */
static void bench_cond_wake(unsigned waiters, unsigned rounds) {
    void *attr = NULL, *mutex = NULL, *cond = NULL;
    assert(attr_init(&attr) == 0 && attr_type(&attr, 1) == 0);
    assert(mutex_init(&mutex, &attr, "bench") == 0);
    assert(cond_init(&cond, NULL, "bench") == 0);
    atomic_int armed = 0, done = 0;
    uint64_t *wait_ns = calloc(rounds, sizeof(*wait_ns));
    uint64_t *wake_ns = calloc(rounds, sizeof(*wake_ns));
    Waiter w = {.cond = &cond, .mutex = &mutex, .armed = &armed, .done = &done,
                .wait_ns = wait_ns, .wake_ns = wake_ns, .count = rounds};
    pthread_t *ids = calloc(waiters, sizeof(*ids));
    for (unsigned i = 0; i < waiters; ++i)
        assert(pthread_create(&ids[i], NULL, cond_waiter, &w) == 0);
    for (unsigned round = 0; round < rounds; ++round) {
        /* Wait until every waiter is inside the wait, then signal them all. */
        int ready = 0;
        for (int spin = 0; spin < 20000 && !ready; ++spin) {
            ready = atomic_load(&armed);
            if (!ready) sleep_ns(50000);
        }
        assert(ready);
        sleep_ns(50000);
        assert(mutex_lock(&mutex) == 0);
        w.signal_at = now_ns();
        cond_broadcast(&cond);
        assert(mutex_unlock(&mutex) == 0);
        while (atomic_load(&done) < (int)(round + 1)) {
            sleep_ns(50000);
        }
    }
    for (unsigned i = 0; i < waiters; ++i) pthread_join(ids[i], NULL);
    uint64_t blocked = pick(wait_ns, rounds, 50);
    uint64_t woken = pick(wake_ns, rounds, 50);
    uint64_t woken95 = pick(wake_ns, rounds, 95);
    printf("cond wait, %u waiter(s)       : blocked %8.1f ns (median)  wake %8.1f ns  p95 %8.1f ns\n",
           waiters, (double)blocked, (double)woken, (double)woken95);
    free(wait_ns);
    free(wake_ns);
    free(ids);
    attr_destroy(&attr);
}

/* ---- M4: the same shape with raw pthreads (the host's own cost) ---- */
typedef struct {
    pthread_cond_t *cond;
    pthread_mutex_t *mutex;
    atomic_int *armed, *done;
    uint64_t *wait_ns, *wake_ns;
    unsigned count;
    uint64_t signal_at;
} RawWaiter;
static void *raw_waiter(void *argument) {
    RawWaiter *w = argument;
    for (unsigned i = 0; i < w->count; ++i) {
        pthread_mutex_lock(w->mutex);
        uint64_t t0 = now_ns();
        atomic_store(w->armed, 1);
        pthread_cond_wait(w->cond, w->mutex);
        uint64_t t1 = now_ns();
        atomic_store(w->armed, 0);
        pthread_mutex_unlock(w->mutex);
        w->wait_ns[i] = t1 - t0;
        w->wake_ns[i] = t1 > w->signal_at ? t1 - w->signal_at : 0;
        atomic_fetch_add(w->done, 1);
    }
    return NULL;
}
static void bench_raw_cond_wake(unsigned rounds) {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
    atomic_int armed = 0, done = 0;
    uint64_t *wait_ns = calloc(rounds, sizeof(*wait_ns));
    uint64_t *wake_ns = calloc(rounds, sizeof(*wake_ns));
    RawWaiter w = {.cond = &cond, .mutex = &mutex, .armed = &armed, .done = &done,
                   .wait_ns = wait_ns, .wake_ns = wake_ns, .count = rounds};
    pthread_t id;
    assert(pthread_create(&id, NULL, raw_waiter, &w) == 0);
    for (unsigned round = 0; round < rounds; ++round) {
        int ready = 0;
        for (int spin = 0; spin < 20000 && !ready; ++spin) {
            ready = atomic_load(&armed);
            if (!ready) sleep_ns(50000);
        }
        assert(ready);
        sleep_ns(50000);
        pthread_mutex_lock(&mutex);
        w.signal_at = now_ns();
        pthread_cond_broadcast(&cond);
        pthread_mutex_unlock(&mutex);
        while (atomic_load(&done) < (int)(round + 1)) {
            sleep_ns(50000);
        }
    }
    pthread_join(id, NULL);
    printf("raw pthread_cond_wait         : blocked %8.1f ns (median)  wake %8.1f ns  p95 %8.1f ns\n",
           (double)pick(wait_ns, rounds, 50), (double)pick(wake_ns, rounds, 50),
           (double)pick(wake_ns, rounds, 95));
    free(wait_ns);
    free(wake_ns);
}

/* ---- M5: a guest semaphore (the port's own waiting path) ---- */
typedef struct {
    uint32_t id;
    uint32_t timeout;
    uint64_t wait_ns, wake_ns;
    uint64_t signal_at;
    atomic_int *armed, *done;
} SemaTask;
static void *sema_waiter(void *argument) {
    SemaTask *t = argument;
    runtime_thread_attach_host("bench-sema");
    uint64_t t0 = now_ns();
    atomic_store(t->armed, 1);
    sem_wait(t->id, 1, &t->timeout);
    uint64_t t1 = now_ns();
    atomic_store(t->armed, 0);
    t->wait_ns = t1 - t0;
    t->wake_ns = t1 > t->signal_at ? t1 - t->signal_at : 0;
    atomic_fetch_add(t->done, 1);
    return NULL;
}
static void bench_sema_wake(unsigned rounds) {
    uint32_t id = 0;
    assert(sem_create(&id, "bench", 1, 0, 1, NULL) == 0);
    atomic_int armed = 0, done = 0;
    uint64_t blocked_total = 0, wake_total = 0, wake_max = 0;
    for (unsigned round = 0; round < rounds; ++round) {
        SemaTask task = {.id = id, .timeout = 5000000, .armed = &armed, .done = &done};
        pthread_t handle;
        assert(pthread_create(&handle, NULL, sema_waiter, &task) == 0);
        int ready = 0;
        for (int spin = 0; spin < 20000 && !ready; ++spin) {
            ready = atomic_load(&armed);
            if (!ready) sleep_ns(50000);
        }
        assert(ready);
        sleep_ns(50000);
        task.signal_at = now_ns();
        assert(sem_signal(id, 1) == 0);
        pthread_join(handle, NULL);
        blocked_total += task.wait_ns;
        wake_total += task.wake_ns;
        if (task.wake_ns > wake_max) wake_max = task.wake_ns;
    }
    printf("semaphore wait -> signal      : blocked %8.1f ns (mean)  wake %8.1f ns  max %8.1f ns\n",
           (double)blocked_total / rounds, (double)wake_total / rounds, (double)wake_max);
}

/* ---- M6: the port's bookkeeping per wait ---- */
static void bench_wait_bookkeeping(unsigned iterations) {
    /* A guest thread (the game's): the wait slot is in its own record (fast path). */
    runtime_thread_attach_host("bench-book");
    uint64_t t0 = now_ns();
    for (unsigned i = 0; i < iterations; ++i) {
        runtime_wait_begin("bench", NULL, "bench", NULL, 0);
        runtime_wait_end();
    }
    uint64_t guest = now_ns() - t0;
    printf("runtime_wait_begin/end (guest) : %8.1f ns/pair\n", (double)guest / iterations);
}

/* ---- M7: a guest thread: create and join ---- */
static void *ABI guest_entry(void *argument) {
    return argument;
}
static void bench_thread_lifecycle(unsigned iterations) {
    uint64_t create_total = 0, join_total = 0;
    for (unsigned i = 0; i < iterations; ++i) {
        void *thread = NULL;
        uint64_t t0 = now_ns();
        assert(thread_create(&thread, NULL, guest_entry, NULL, "bench") == 0);
        uint64_t t1 = now_ns();
        assert(thread_join(thread, NULL) == 0);
        uint64_t t2 = now_ns();
        create_total += t1 - t0;
        join_total += t2 - t1;
    }
    printf("guest thread create+join       : create %8.1f us  join %8.1f us\n",
           (double)create_total / iterations / 1000.0, (double)join_total / iterations / 1000.0);
}

/* ---- M8: the guest priority mapping (runtime_thread.c lowers the host priority) ---- */
#ifdef _WIN32
#include <windows.h>
static void bench_priority(void) {
    /* The same wake-up latency with the waking thread at NORMAL and at BELOW_NORMAL. */
    for (int level = 0; level < 3; ++level) {
        int priority = level == 0 ? THREAD_PRIORITY_NORMAL
                                  : (level == 1 ? THREAD_PRIORITY_BELOW_NORMAL
                                                : THREAD_PRIORITY_LOWEST);
        HANDLE handle = OpenThread(THREAD_SET_INFORMATION, FALSE, (DWORD)GetCurrentThreadId());
        if (handle) {
            SetThreadPriority(handle, priority);
            CloseHandle(handle);
        }
        atomic_int armed = 0, done = 0;
        uint64_t *wait_ns = calloc(200, sizeof(*wait_ns));
        uint64_t *wake_ns = calloc(200, sizeof(*wake_ns));
        void *attr = NULL, *mutex = NULL, *cond = NULL;
        assert(attr_init(&attr) == 0 && attr_type(&attr, 1) == 0);
        assert(mutex_init(&mutex, &attr, "bench") == 0);
        assert(cond_init(&cond, NULL, "bench") == 0);
        Waiter w = {.cond = &cond, .mutex = &mutex, .armed = &armed, .done = &done,
                    .wait_ns = wait_ns, .wake_ns = wake_ns, .count = 200};
        pthread_t id;
        assert(pthread_create(&id, NULL, cond_waiter, &w) == 0);
        for (unsigned round = 0; round < 200; ++round) {
            int ready = 0;
            for (int spin = 0; spin < 20000 && !ready; ++spin) {
                ready = atomic_load(&armed);
                if (!ready) sleep_ns(50000);
            }
            assert(ready);
            sleep_ns(50000);
            assert(mutex_lock(&mutex) == 0);
            w.signal_at = now_ns();
            cond_broadcast(&cond);
            assert(mutex_unlock(&mutex) == 0);
            while (atomic_load(&done) < (int)(round + 1)) {
                sleep_ns(50000);
            }
        }
        pthread_join(id, NULL);
        static const char *names[] = {"NORMAL     ", "BELOW_NORMAL", "LOWEST     "};
        printf("wake latency, signer at %s : blocked %8.1f ns  wake %8.1f ns  p95 %8.1f ns\n",
               names[level], (double)pick(wait_ns, 200, 50), (double)pick(wake_ns, 200, 50),
               (double)pick(wake_ns, 200, 95));
        free(wait_ns);
        free(wake_ns);
        attr_destroy(&attr);
    }
    /* Back to normal for the rest of the bench. */
    HANDLE handle = OpenThread(THREAD_SET_INFORMATION, FALSE, (DWORD)GetCurrentThreadId());
    if (handle) {
        SetThreadPriority(handle, THREAD_PRIORITY_NORMAL);
        CloseHandle(handle);
    }
}
#else
static void bench_priority(void) { }
#endif

/* ---- M9: the accuracy of the port's sleep (the guest's nanosleep/usleep) ---- */
static void bench_sleep_accuracy(void) {
    static const uint64_t requests[] = {100000, 500000, 1000000, 2000000, 5000000, 16600000};
    printf("== sleep accuracy ==\n");
    for (unsigned i = 0; i < sizeof(requests) / sizeof(*requests); ++i) {
        uint64_t ns = requests[i];
        unsigned n = ns >= 5000000 ? 20 : 100;
        uint64_t t0 = now_ns();
        for (unsigned k = 0; k < n; ++k) sleep_ns(ns);
        uint64_t total = now_ns() - t0;
        printf("compat_sleep_ns(%7.1f us)      : actual %8.1f us  (%.2fx the request)\n",
               (double)ns / 1000.0, (double)total / n / 1000.0, (double)total / n / (double)ns);
    }
    /* The host's own nanosleep, for comparison (winpthreads rounds to the tick). */
    uint64_t ns = 1000000;
    unsigned n = 50;
    uint64_t t0 = now_ns();
    for (unsigned k = 0; k < n; ++k) {
        struct timespec t = {0, (long)ns};
        nanosleep(&t, NULL);
    }
    uint64_t total = now_ns() - t0;
    printf("host nanosleep(%7.1f us)       : actual %8.1f us  (%.2fx the request)\n",
           (double)ns / 1000.0, (double)total / n / 1000.0, (double)total / n / (double)ns);
}

/* ---- M10: the cost of the condition variable the semaphore creates per wait ---- */
static void bench_condvar_lifecycle(unsigned iterations) {
    uint64_t t0 = now_ns();
    for (unsigned i = 0; i < iterations; ++i) {
        pthread_condattr_t attr;
        pthread_condattr_init(&attr);
        pthread_condattr_setclock(&attr, CLOCK_REALTIME);
        pthread_cond_t cond;
        pthread_cond_init(&cond, &attr);
        pthread_condattr_destroy(&attr);
        pthread_cond_destroy(&cond);
    }
    uint64_t total = now_ns() - t0;
    printf("cond create+destroy (per wait) : %8.1f ns\n", (double)total / iterations);
}

/* ---- M11: the port's shared counters (the cache line they ping-pong) ---- */
static _Atomic size_t shared_counter;
typedef struct {
    atomic_int *stop;
    unsigned increments;
    size_t local; /* the per-thread counter (in memory, like a thread record field) */
} Counter;
static void *counter_thread(void *argument) {
    Counter *c = argument;
    while (!atomic_load(c->stop)) {
        for (unsigned i = 0; i < 64; ++i) atomic_fetch_add_explicit(&shared_counter, 1, memory_order_relaxed);
        c->increments += 64;
    }
    return NULL;
}
static void *private_thread(void *argument) {
    Counter *c = argument;
    while (!atomic_load(c->stop)) {
        for (unsigned i = 0; i < 64; ++i) ++c->local;
        c->increments += 64;
    }
    return NULL;
}
static void bench_counters(unsigned threads, unsigned milliseconds) {
    for (int mode = 0; mode < 2; ++mode) {
        atomic_int stop = 0;
        Counter *counters = calloc(threads, sizeof(*counters));
        pthread_t *ids = calloc(threads, sizeof(*ids));
        for (unsigned i = 0; i < threads; ++i) {
            counters[i].stop = &stop;
            assert(pthread_create(&ids[i], NULL, mode ? private_thread : counter_thread, &counters[i]) == 0);
        }
        sleep_ns((uint64_t)milliseconds * 1000000u);
        atomic_store(&stop, 1);
        unsigned increments = 0;
        for (unsigned i = 0; i < threads; ++i) {
            pthread_join(ids[i], NULL);
            increments += counters[i].increments;
        }
        printf("%s counter, %u threads      : %8.1f ns/increment  (%u increments)\n",
               mode ? "private" : "shared ", threads,
               increments ? (double)milliseconds * 1e6 / increments : 0.0, increments);
        free(counters);
        free(ids);
    }
}

/* ---- M12: the false sharing with the guest's stack canary ---- */
/* The runtime counts every libc memcpy in a global counter that sits next to the
 * guest's stack canary (runtime.c): the canary is read at every guest function
 * entry. This measures what a reader pays when a neighbouring counter is hammered. */
typedef struct {
    volatile uint64_t canary;
    _Atomic size_t counter;
    char pad[64];
} Canary;
static Canary canary_line;
static Canary canary_alone;
static atomic_int counter_stop;
static void *counter_hammer(void *argument) {
    Canary *line = argument;
    while (!atomic_load(&counter_stop))
        for (unsigned i = 0; i < 64; ++i)
            atomic_fetch_add_explicit(&line->counter, 1, memory_order_relaxed);
    return NULL;
}
static uint64_t read_canary(Canary *line, unsigned iterations) {
    uint64_t sum = 0;
    for (unsigned i = 0; i < iterations; ++i) sum += line->canary;
    return sum;
}
static void bench_false_sharing(unsigned milliseconds) {
    for (int mode = 0; mode < 2; ++mode) {
        Canary *line = mode ? &canary_alone : &canary_line;
        line->canary = 0x123456789abcdefULL;
        atomic_store(&counter_stop, 0);
        pthread_t id;
        assert(pthread_create(&id, NULL, counter_hammer, line) == 0);
        sleep_ns((uint64_t)milliseconds * 1000000u);
        unsigned iterations = 2000000;
        uint64_t t0 = now_ns();
        uint64_t sum = read_canary(line, iterations);
        uint64_t elapsed = now_ns() - t0;
        atomic_store(&counter_stop, 1);
        pthread_join(id, NULL);
        printf("canary read, %-14s: %8.2f ns/read (sum %llx)\n",
               mode ? "own cache line" : "next to the counter", (double)elapsed / iterations,
               (unsigned long long)sum);
    }
}

int main(int argc, char **argv) {
    int quick = argc > 1 && !strcmp(argv[1], "--quick");
    resolve();
    printf("== port sync bench (guest primitives) ==\n");
    bench_mutex_alone(quick ? 20000 : 200000);
    bench_mutex_contended(2, quick ? 200 : 500);
    bench_mutex_contended(8, quick ? 200 : 500);
    bench_mutex_contended_normal(8, quick ? 200 : 500);
    bench_cond_wake(1, quick ? 200 : SAMPLES);
    bench_cond_wake(8, quick ? 100 : 500);
    bench_raw_cond_wake(quick ? 200 : SAMPLES);
    bench_sema_wake(quick ? 200 : 1000);
    bench_wait_bookkeeping(quick ? 20000 : 200000);
    bench_thread_lifecycle(quick ? 20 : 100);
    bench_condvar_lifecycle(quick ? 2000 : 20000);
    bench_counters(8, quick ? 200 : 500);
    bench_false_sharing(quick ? 100 : 300);
    bench_sleep_accuracy();
    bench_priority();
    puts("PASS: port sync bench");
    return 0;
}
