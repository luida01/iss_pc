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

void runtime_report(void) { }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) {
    (void)table; (void)count; (void)nid; return 0;
}
static void sleep_ns(uint64_t ns) {
#ifdef _WIN32
    compat_sleep_ns(ns);
#else
    struct timespec t = {(time_t)(ns / 1000000000u), (long)(ns % 1000000000u)};
    nanosleep(&t, NULL);
#endif
}
static unsigned lost, rounds_done;

/* ---- raw pthread ---- */
static pthread_mutex_t raw_mutex;
static pthread_cond_t raw_cond = PTHREAD_COND_INITIALIZER;
static atomic_int raw_armed, raw_done;
static void *raw_waiter(void *p) {
    (void)p;
    pthread_mutex_lock(&raw_mutex);
    atomic_store(&raw_armed, 1);
    pthread_cond_wait(&raw_cond, &raw_mutex);
    pthread_mutex_unlock(&raw_mutex);
    atomic_fetch_add(&raw_done, 1);
    return NULL;
}
static void raw_trial(const char *label, int type, unsigned rounds) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, type);
    pthread_mutex_init(&raw_mutex, &attr);
    lost = 0;
    for (unsigned round = 0; round < rounds; ++round) {
        atomic_store(&raw_armed, 0);
        atomic_store(&raw_done, 0);
        pthread_t id;
        pthread_create(&id, NULL, raw_waiter, NULL);
        for (int spin = 0; spin < 100000 && !atomic_load(&raw_armed); ++spin) sleep_ns(1000);
        pthread_mutex_lock(&raw_mutex);
        pthread_cond_signal(&raw_cond);
        pthread_mutex_unlock(&raw_mutex);
        int ok = 0;
        for (int spin = 0; spin < 3000 && !ok; ++spin) {
            ok = atomic_load(&raw_done);
            if (!ok) sleep_ns(1000);
        }
        if (!ok) {
            ++lost;
            pthread_mutex_lock(&raw_mutex);
            pthread_cond_broadcast(&raw_cond);
            pthread_mutex_unlock(&raw_mutex);
            for (int spin = 0; spin < 3000 && !atomic_load(&raw_done); ++spin) sleep_ns(1000);
        }
        pthread_join(id, NULL);
    }
    pthread_mutex_destroy(&raw_mutex);
    printf("%-32s: %u lost wake-ups of %u\n", label, lost, rounds);
    fflush(stdout);
}

/* ---- the port's guest condition variable ---- */
typedef int32_t (ABI *AttrInit)(void **out);
typedef int32_t (ABI *AttrType)(void **attr, int type);
typedef int32_t (ABI *MutexInit)(void **out, void **attr, const char *name);
typedef int32_t (ABI *MutexOp)(void **mutex);
typedef int32_t (ABI *CondInit)(void **out, void **attr, const char *name);
typedef int32_t (ABI *CondWait)(void **cond, void **mutex);
typedef int32_t (ABI *CondSignal)(void **cond);
static MutexOp mutex_lock, mutex_unlock;
static CondWait cond_wait;
static CondSignal cond_signal;
static void *g_mutex, *g_cond;
static atomic_int g_armed, g_done;
static void *guest_waiter(void *p) {
    (void)p;
    fprintf(stderr,"[waiter] lock\n"); fflush(stderr);
    mutex_lock(&g_mutex);
    fprintf(stderr,"[waiter] armed\n"); fflush(stderr);
    atomic_store(&g_armed, 1);
    cond_wait(&g_cond, &g_mutex);
    fprintf(stderr,"[waiter] woke\n"); fflush(stderr);
    mutex_unlock(&g_mutex);
    atomic_fetch_add(&g_done, 1);
    return NULL;
}
static void guest_trial(const char *label, int mutex_type, unsigned rounds) {
    void *attr = NULL;
    AttrInit ai = (AttrInit)(uintptr_t)runtime_mutex_resolve("F8bUHwAG284#libkernel");
    AttrType at = (AttrType)(uintptr_t)runtime_mutex_resolve("iMp8QpE+XO4#libkernel");
    MutexInit mi = (MutexInit)(uintptr_t)runtime_mutex_resolve("cmo1RIYva9o#libkernel");
    CondInit ci = (CondInit)(uintptr_t)runtime_mutex_resolve("2Tb92quprl0#libkernel");
    ai(&attr);
    at(&attr, mutex_type);
    mi(&g_mutex, &attr, "t");
    ci(&g_cond, NULL, "t");
    lost = 0;
    for (unsigned round = 0; round < rounds; ++round) {
        fprintf(stderr,"[main] round %u\n",round); fflush(stderr);
        atomic_store(&g_armed, 0);
        atomic_store(&g_done, 0);
        pthread_t id;
        pthread_create(&id, NULL, guest_waiter, NULL);
        for (int spin = 0; spin < 100000 && !atomic_load(&g_armed); ++spin) sleep_ns(1000);
        mutex_lock(&g_mutex);
        cond_signal(&g_cond);
        mutex_unlock(&g_mutex);
        int ok = 0;
        for (int spin = 0; spin < 3000 && !ok; ++spin) {
            ok = atomic_load(&g_done);
            if (!ok) sleep_ns(1000);
        }
        if (!ok) {
            ++lost;
            mutex_lock(&g_mutex);
            cond_signal(&g_cond);
            mutex_unlock(&g_mutex);
            for (int spin = 0; spin < 3000 && !atomic_load(&g_done); ++spin) sleep_ns(1000);
        }
        pthread_join(id, NULL);
    }
    printf("%-32s: %u lost wake-ups of %u\n", label, lost, rounds);
    fflush(stdout);
}

int main(int argc, char **argv) {
    unsigned rounds = argc > 1 ? (unsigned)atoi(argv[1]) : 500;
    mutex_lock = (MutexOp)(uintptr_t)runtime_mutex_resolve("9UK1vLZQft4#libkernel");
    mutex_unlock = (MutexOp)(uintptr_t)runtime_mutex_resolve("tn3VlD0hG60#libkernel");
    cond_wait = (CondWait)(uintptr_t)runtime_mutex_resolve("WKAXJ4XBPQ4#libkernel");
    cond_signal = (CondSignal)(uintptr_t)runtime_mutex_resolve("kDh-NfxgMtE#libkernel");
    printf("rounds=%u\n", rounds);
    fflush(stdout);
    raw_trial("raw pthread, NORMAL mutex", PTHREAD_MUTEX_NORMAL, rounds);
    raw_trial("raw pthread, ERRORCHECK mutex", PTHREAD_MUTEX_ERRORCHECK, rounds);
    raw_trial("raw pthread, RECURSIVE mutex", PTHREAD_MUTEX_RECURSIVE, rounds);
    guest_trial("port guest mutex type 1", 1, rounds);
    guest_trial("port guest mutex type 3", 3, rounds);
    return 0;
}
