/* Guest pool test (port/tests/test_guest_pool.c): the pool's creation and geometry.
 *
 * It links the real port/src/runtime_memory.c and exercises the paths of the pool:
 *
 *   - the normal one (no flag): the same section as today, 4 KiB pages;
 *   - TLG_HUGE_PAGES=align: the 2 MiB aligned geometry (2 MiB aligned physical
 *     offsets and guest addresses, sizes rounded up to 2 MiB) with 4 KiB pages,
 *     to check that the geometry works without the SeLockMemoryPrivilege that
 *     the large-page route needs;
 *   - TLG_HUGE_PAGES=1: the large-page route. Without the privilege in the token
 *     the runtime says so and keeps the normal pool (the flag changes nothing).
 *
 * The setting is read once per process, so the test runs one mode per run:
 *
 *   bash port/tests/run_guest_pool.sh              # the normal pool
 *   TLG_HUGE_PAGES=align bash port/tests/run_guest_pool.sh
 *   TLG_HUGE_PAGES=1 bash port/tests/run_guest_pool.sh
 */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* The backing view of the pool (runtime_memory.c). */
int runtime_memory_read_backing(uintptr_t address, void *data, uint64_t size);

/* ---- the runtime services runtime_memory.c uses ---- */
void runtime_wait_begin(const char *kind, const void *object, const char *name,
                        const void *const *owner, uintptr_t frame) {
    (void)kind; (void)object; (void)name; (void)owner; (void)frame;
}
void runtime_wait_end(void) {}
static int32_t errno_value;
int32_t *runtime_errno(void) { return &errno_value; }
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) {
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(table[i].name, nid)) return (uintptr_t)table[i].function;
    return 0;
}
void compat_sleep_ns(uint64_t ns) { Sleep((DWORD)(ns / 1000000)); }

/* ---- the guest calls the runtime makes ---- */
typedef int32_t(ABI *alloc_fn)(int64_t low, int64_t high, uint64_t size, uint64_t alignment,
                                int32_t type, int64_t *out);
typedef int32_t(ABI *map_fn)(void **out, uint64_t size, int32_t prot, uint32_t flags,
                              int64_t physical, uint64_t alignment);
typedef int32_t(ABI *release_fn)(uint64_t start, uint64_t size);
typedef int32_t(ABI *mprotect_fn)(const void *address, uint64_t size, int32_t prot);

#define MB (UINT64_C(1024) * 1024)
#define ALLOC_MB 64
#define ALLOC_SIZE (ALLOC_MB * MB + PAGE_ALIGN) /* not a multiple of 2 MiB on purpose */
#define POOL_MB 5056
#define HUGE (UINT64_C(2) * MB)
#define PAGE_ALIGN (UINT64_C(16) * 1024)

static int failures;
static void check(int ok, const char *what) {
    printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) ++failures;
}

int main(void) {
    const char *flag = getenv("TLG_HUGE_PAGES");
    printf("== guest pool test (TLG_HUGE_PAGES=%s) ==\n", flag ? flag : "(unset)");

    alloc_fn allocate = (alloc_fn)runtime_memory_resolve("sceKernelAllocateDirectMemory");
    map_fn map = (map_fn)runtime_memory_resolve("sceKernelMapDirectMemory");
    release_fn release = (release_fn)runtime_memory_resolve("sceKernelReleaseDirectMemory");
    mprotect_fn protect = (mprotect_fn)runtime_memory_resolve("sceKernelMprotect");
    if (!allocate || !map || !release || !protect) {
        printf("  the runtime did not export the direct memory calls\n");
        return 1;
    }

    /* A hint that is 16 KiB aligned but not 2 MiB aligned, and an allocation that
     * starts inside a 2 MiB block: with the aligned geometry the runtime has to move
     * both up to the next 2 MiB boundary. */
    uintptr_t hint = UINT64_C(0x1000000000) + 0x2000;

    int64_t phys = 0;
    int32_t r = allocate(0x1000, (int64_t)(POOL_MB * MB), ALLOC_SIZE, PAGE_ALIGN, 0, &phys);
    check(r == 0, "the direct allocation succeeds");
    if (r) return 1;
    printf("  physical offset: 0x%" PRIx64 "\n", (uint64_t)phys);

    void *address = (void *)hint;
    r = map(&address, ALLOC_SIZE, 0x3, 0, phys, PAGE_ALIGN);
    check(r == 0, "the direct memory maps");
    if (r) return 1;
    printf("  guest address:  %p (hint %p)\n", address, (void *)hint);

    /* The offset and the address are aligned together or not at all: the views of a
     * large-page section need both. */
    const int aligned_geometry = (((uint64_t)phys % HUGE) == 0);
    check(aligned_geometry == (((uintptr_t)address % HUGE) == 0),
          "the offset and the address share the 2 MiB alignment");

    /* The guest writes and the host reads through the backing view: the same bytes. */
    unsigned char *guest = (unsigned char *)address;
    for (uint64_t i = 0; i < ALLOC_SIZE; i += 4096) guest[i] = (unsigned char)(i / 4096);
    unsigned char *backing = malloc(4096);
    int read_ok = runtime_memory_read_backing((uintptr_t)address, backing, 4096) &&
                  backing[0] == 0 && backing[4095] == 0;
    check(read_ok, "the backing view sees the guest's bytes");
    free(backing);

    r = protect(address, aligned_geometry ? HUGE : ALLOC_SIZE, 0x1);
    check(r == 0, "the guest protects the range read-only");
    r = protect(address, aligned_geometry ? HUGE : ALLOC_SIZE, 0x3);
    check(r == 0, "the guest protects the range writable again");
    if (aligned_geometry) {
        r = protect(address, ALLOC_SIZE, 0x3);
        check(r != 0, "an unaligned protection is refused (it would split the views)");
    }

    r = release((uint64_t)phys, ALLOC_SIZE);
    check(r == 0, "the direct memory is released");

    runtime_memory_report();
    printf("== %s ==\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
