/* Guest memory bench (port/tests/test_guest_tlb.c): what the guest's pool costs on
 * this machine, and what the alternatives would cost.
 *
 * The port reserves the guest's PS4 direct memory as a *pagefile-backed section*
 * (port/src/runtime_memory.c, Windows branch: CreateFileMapping2 with
 * INVALID_HANDLE_VALUE and SEC_COMMIT) and maps it twice: once at the guest
 * addresses and once as a "backing view" the host uses to read and write guest
 * memory around the page protection. This bench measures that layout against the
 * alternatives:
 *
 *   1. the page size the pool really uses (fault granularity),
 *   2. whether 2 MiB pages can be obtained here at all (the three documented
 *      routes) and what each one costs,
 *   3. the TLB reach of this CPU (a pointer chase) and what the section
 *      indirection adds on top of it,
 *   4. what a protection change (the GPU tracker's) costs the other threads,
 *   5. what the pagefile backing costs when the working set is trimmed: the
 *      dirty pages go to the pagefile and the next access reads them back.
 *
 * It measures the host only (no game, no GPU, no Vulkan), so it can run next to a
 * session. Build: bash port/tests/run_guest_tlb.sh
 */
#define _GNU_SOURCE
#include <windows.h>
#include <psapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The runtime's pool sizes (runtime_memory.c): PS4 direct memory + the flexible span. */
#define RUNTIME_POOL_MB 5056
#define RUNTIME_FLEX_MB 1024

/* The bench works on a smaller pool so it fits next to a running game; the ratios
 * (page size, fault cost) do not depend on the size. */
#define BENCH_MB 512
#define BENCH_BYTES ((size_t)BENCH_MB * 1024 * 1024)

static double now_ms(void) {
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static uint64_t rng_state = 0x12345678;
static uint64_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static double page_faults(void) {
    PROCESS_MEMORY_COUNTERS pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return -1;
    return (double)pmc.PageFaultCount;
}

static size_t working_set_mb(void) {
    PROCESS_MEMORY_COUNTERS pmc;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
    return (size_t)(pmc.WorkingSetSize >> 20);
}

static size_t available_mb(void) {
    MEMORYSTATUSEX mem;
    mem.dwLength = sizeof(mem);
    GlobalMemoryStatusEx(&mem);
    return (size_t)(mem.ullAvailPhys >> 20);
}

/* ---- the page size the pool really uses ---- */

static void measure_page_size(const char *tag, unsigned char *view, size_t bytes) {
    size_t pages = bytes / 4096;
    double before = page_faults();
    for (size_t i = 0; i < pages; ++i) view[i * 4096] = (unsigned char)i;
    double after = page_faults();
    double per_page = pages ? (after - before) / (double)pages : 0.0;
    printf("  %-28s %7zu pages of 4 KiB -> %8ld faults (%.2f faults/page%s)\n", tag, pages,
           (long)(after - before), per_page, per_page < 0.25 ? " -> 2 MiB pages" : " -> 4 KiB pages");
}

/* ---- the TLB reach of this CPU ---- */

/* Builds a random cycle of one pointer per 4 KiB page inside `base` and returns the
 * first node. Every step of the chase is a dependent load, so the latency of one
 * is the memory latency plus the TLB miss cost when the page is not in the TLB. */
static void *build_chain(unsigned char *base, size_t nodes) {
    size_t *perm = (size_t *)malloc(nodes * sizeof(size_t));
    if (!perm) return NULL;
    for (size_t i = 0; i < nodes; ++i) perm[i] = i;
    for (size_t i = nodes; i > 1; --i) {
        size_t j = (size_t)(rng() % i);
        size_t tmp = perm[i - 1];
        perm[i - 1] = perm[j];
        perm[j] = tmp;
    }
    for (size_t i = 0; i < nodes; ++i) {
        void *following = base + perm[(i + 1) % nodes] * 4096;
        memcpy(base + perm[i] * 4096, &following, sizeof(void *));
    }
    void *start = base + perm[0] * 4096;
    free(perm);
    return start;
}

static double chase(void *start, size_t steps) {
    double t0 = now_ms();
    void *p = start;
    for (size_t i = 0; i < steps; ++i) p = *(void **)p;
    double t1 = now_ms();
    if (p == NULL) printf("%p", p); /* keep the chase alive */
    return (t1 - t0) * 1e6 / (double)steps; /* ns per step */
}

static void chase_section(HANDLE section, size_t bytes) {
    unsigned char *view = (unsigned char *)MapViewOfFile3(section, GetCurrentProcess(), NULL, 0, bytes,
                                                           0, PAGE_READWRITE, NULL, 0);
    if (!view) {
        printf("    (MapViewOfFile3 failed: %lu)\n", GetLastError());
        return;
    }
    void *start = build_chain(view, bytes / 4096);
    if (!start) {
        printf("    (allocation failed)\n");
        UnmapViewOfFile(view);
        return;
    }
    chase(start, 200000);
    double best = 1e9;
    for (int round = 0; round < 3; ++round) {
        double ns = chase(start, 1000000);
        if (ns < best) best = ns;
    }
    printf("    section view: %6.1f ns/access\n", best);
    UnmapViewOfFile(view);
}

static void chase_anonymous(size_t bytes) {
    unsigned char *mem = (unsigned char *)VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT,
                                                        PAGE_READWRITE);
    if (!mem) {
        printf("    (allocation failed)\n");
        return;
    }
    void *start = build_chain(mem, bytes / 4096);
    if (!start) {
        printf("    (allocation failed)\n");
        VirtualFree(mem, 0, MEM_RELEASE);
        return;
    }
    chase(start, 200000);
    double best = 1e9;
    for (int round = 0; round < 3; ++round) {
        double ns = chase(start, 1000000);
        if (ns < best) best = ns;
    }
    printf("    anonymous   : %6.1f ns/access\n", best);
    VirtualFree(mem, 0, MEM_RELEASE);
}

/* ---- what a protection change costs the other threads ---- */

/* The GPU tracker protects the guest's pages read-only so it can see the guest's
 * writes. A protection change invalidates the TLB entries of the range in every
 * thread, so a thread chasing a chain over that range pays the page walks again.
 * The chain is built over one cache line per page, so the data stays in the cache
 * and the difference is the TLB, not the memory. */
static void measure_protect_tlb(void) {
    const size_t bytes = 1u * 1024 * 1024; /* 256 pages: inside the TLB reach */
    unsigned char *mem = (unsigned char *)VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT,
                                                        PAGE_READWRITE);
    if (!mem) {
        printf("  (allocation failed)\n");
        return;
    }
    void *start = build_chain(mem, bytes / 4096);
    if (!start) {
        printf("  (allocation failed)\n");
        VirtualFree(mem, 0, MEM_RELEASE);
        return;
    }
    chase(start, 100000);
    double before = 0;
    for (int round = 0; round < 5; ++round) before += chase(start, 500000);
    before /= 5;
    DWORD old;
    VirtualProtect(mem, bytes, PAGE_READONLY, &old);
    VirtualProtect(mem, bytes, PAGE_READWRITE, &old);
    double after = 0;
    for (int round = 0; round < 5; ++round) after += chase(start, 500000);
    after /= 5;
    printf("  a chase over 1 MiB (256 pages, TLB resident): %.1f ns/access before a protection "
           "change, %.1f after (the TLB entries of the range were dropped)\n",
           before, after);
    VirtualFree(mem, 0, MEM_RELEASE);
}

/* ---- what the pagefile backing costs when the working set is trimmed ---- */

/* The guest's pool is dirty pagefile-backed memory: when the memory manager
 * trims it, the pages are written to the pagefile and the next access reads them
 * back. This measures that round trip, and the same for anonymous memory (which
 * is also paged out, but only when the system is really short of memory). */
static void measure_trim_cost(const char *tag, unsigned char *view, size_t bytes) {
    size_t pages = bytes / 4096;
    for (size_t i = 0; i < pages; ++i) view[i * 4096] = (unsigned char)i;

    SIZE_T min = 0, max = 0;
    GetProcessWorkingSetSize(GetCurrentProcess(), &min, &max);
    SetProcessWorkingSetSize(GetCurrentProcess(), 1, 1); /* trim */

    double t0 = now_ms();
    volatile unsigned long sum = 0;
    for (size_t i = 0; i < pages; ++i) sum += view[i * 4096];
    double t1 = now_ms();
    SetProcessWorkingSetSize(GetCurrentProcess(), min, max);
    printf("  %-28s re-read of %7zu pages after the trim: %7.1f ms (%.2f us/page)\n", tag, pages,
           t1 - t0, pages ? (t1 - t0) * 1000.0 / (double)pages : 0.0);
    (void)sum;
}

/* The same, but with the system really short of memory: then the pages have to
 * travel to the pagefile (on D:, a spinning disk) instead of staying in the
 * standby list. The pressure is bounded so the bench does not destabilize the
 * machine. */
static void measure_eviction_cost(const char *tag, unsigned char *view, size_t bytes) {
    size_t pages = bytes / 4096;
    for (size_t i = 0; i < pages; ++i) view[i * 4096] = (unsigned char)i;

    size_t available = available_mb();
    size_t pressure_mb = available > 2048 ? available - 1536 : 0;
    if (pressure_mb > 4096) pressure_mb = 4096;
    unsigned char *pressure = NULL;
    if (pressure_mb) {
        pressure = (unsigned char *)VirtualAlloc(NULL, pressure_mb << 20, MEM_RESERVE | MEM_COMMIT,
                                                 PAGE_READWRITE);
        if (pressure) memset(pressure, 1, pressure_mb << 20);
    }
    SIZE_T min = 0, max = 0;
    GetProcessWorkingSetSize(GetCurrentProcess(), &min, &max);
    SetProcessWorkingSetSize(GetCurrentProcess(), 1, 1); /* trim under pressure */

    double t0 = now_ms();
    volatile unsigned long sum = 0;
    for (size_t i = 0; i < pages; ++i) sum += view[i * 4096];
    double t1 = now_ms();
    SetProcessWorkingSetSize(GetCurrentProcess(), min, max);
    if (pressure) VirtualFree(pressure, 0, MEM_RELEASE);
    printf("  %-28s re-read of %7zu pages with %4lu MiB of pressure: %8.1f ms (%.2f us/page)\n",
           tag, pages, (unsigned long)pressure_mb, t1 - t0,
           pages ? (t1 - t0) * 1000.0 / (double)pages : 0.0);
    (void)sum;
}

/* ---- what the tracker's protection cycle costs ---- */

/* The GPU tracker protects the guest's pages read-only and unprotects them on
 * every write fault. The log shows the ranges it protects ("protect calls 10
 * (8192 pages)"), so this measures one full cycle over a fresh 32 MiB range:
 * protect, write (fault), unprotect. On a section-backed mapping the protection
 * change has to split the section's control area, so it is compared with the same
 * cycle over anonymous memory. */
static void measure_protect_cycle(const char *tag, unsigned char *pool, size_t bytes) {
    const size_t range = 32u * 1024 * 1024; /* 8192 pages, the tracker's size */
    const int rounds = 20;
    double total = 0;
    for (int r = 0; r < rounds; ++r) {
        size_t at = ((size_t)r * range) % (bytes - range);
        DWORD old;
        double t0 = now_ms();
        VirtualProtect(pool + at, range, PAGE_READONLY, &old);
        pool[at] = 1; /* the write faults */
        VirtualProtect(pool + at, range, PAGE_READWRITE, &old);
        total += now_ms() - t0;
    }
    printf("  %-28s protect+write fault+unprotect over 32 MiB: %6.2f ms per cycle\n", tag,
           total / rounds);
}

/* The port resolves a write to a protected page in a vectored exception handler
 * (port/src/probe.c: vectored_fault), which restores the write protection and
 * lets the write continue. The bench does the same, so the measured cost is the
 * one the guest pays. */
static LONG WINAPI vec_handler(EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *record = info->ExceptionRecord;
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    DWORD old;
    /* The faulting data address is in the exception information, not in the
     * exception address (which is the instruction that faulted). */
    uintptr_t at = (uintptr_t)record->ExceptionInformation[1] & ~(uintptr_t)4095;
    VirtualProtect((void *)at, 4096, PAGE_READWRITE, &old);
    return EXCEPTION_CONTINUE_EXECUTION;
}

int main(int argc, char **argv) {
    const int evict = argc > 1 && !strcmp(argv[1], "evict");
    /* The write to a protected page faults; the port's exception handler resolves
     * it, and so does this bench's (installed before anything else). */
    AddVectoredExceptionHandler(1, vec_handler);
    SYSTEM_INFO si;
    MEMORYSTATUSEX mem;
    GetSystemInfo(&si);
    mem.dwLength = sizeof(mem);
    GlobalMemoryStatusEx(&mem);
    printf("== machine ==\n");
    printf("  page size=%lu, allocation granularity=%lu, logical processors=%lu\n",
           (unsigned long)si.dwPageSize, (unsigned long)si.dwAllocationGranularity,
           (unsigned long)si.dwNumberOfProcessors);
    printf("  physical memory: %lu MiB total, %lu MiB available\n",
           (unsigned long)(mem.ullTotalPhys >> 20), (unsigned long)(mem.ullAvailPhys >> 20));
    {
        ULONG_PTR min = GetLargePageMinimum();
        printf("  GetLargePageMinimum=%llu (%s)\n", (unsigned long long)min,
               min ? "the OS knows 2 MiB pages" : "no huge pages");
    }
    {
        HANDLE token = NULL;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
            LUID luid;
            if (LookupPrivilegeValueA(NULL, "SeLockMemoryPrivilege", &luid)) {
                TOKEN_PRIVILEGES tp;
                tp.PrivilegeCount = 1;
                tp.Privileges[0].Luid = luid;
                tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                SetLastError(0);
                AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), NULL, NULL);
                DWORD err = GetLastError();
                printf("  SeLockMemoryPrivilege: %s (error %lu; 0=assigned, 1300=not in the token)\n",
                       err == 0 ? "assigned" : "NOT available", (unsigned long)err);
            }
            CloseHandle(token);
        }
    }

    printf("== the runtime's pool, as it is today (pagefile-backed section) ==\n");
    HANDLE section = CreateFileMapping2(INVALID_HANDLE_VALUE, NULL, FILE_MAP_ALL_ACCESS,
                                        PAGE_EXECUTE_READWRITE, SEC_COMMIT,
                                        (uint64_t)BENCH_BYTES, NULL, NULL, 0);
    if (!section) {
        printf("  CreateFileMapping2 failed: %lu\n", GetLastError());
        return 1;
    }
    unsigned char *view = (unsigned char *)MapViewOfFile3(section, GetCurrentProcess(), NULL, 0,
                                                           BENCH_BYTES, 0, PAGE_READWRITE, NULL, 0);
    if (!view) {
        printf("  MapViewOfFile3 failed: %lu\n", GetLastError());
        return 1;
    }
    measure_page_size("pagefile section", view, BENCH_BYTES);

    printf("== the alternatives for 2 MiB pages ==\n");
    {
        HANDLE s = CreateFileMapping2(INVALID_HANDLE_VALUE, NULL, FILE_MAP_ALL_ACCESS,
                                       PAGE_EXECUTE_READWRITE, SEC_COMMIT | SEC_LARGE_PAGES,
                                       (uint64_t)BENCH_BYTES, NULL, NULL, 0);
        if (!s) {
            printf("  SEC_LARGE_PAGES on the section: failed with %lu (87=invalid parameter, "
                   "1314=privilege not held)\n",
                   GetLastError());
        } else {
            unsigned char *v = (unsigned char *)MapViewOfFile3(s, GetCurrentProcess(), NULL, 0,
                                                               BENCH_BYTES, 0, PAGE_READWRITE, NULL, 0);
            if (v) {
                measure_page_size("SEC_LARGE_PAGES", v, BENCH_BYTES);
                UnmapViewOfFile(v);
            }
            CloseHandle(s);
        }
    }
    {
        void *p = VirtualAlloc(NULL, BENCH_BYTES, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES,
                                PAGE_READWRITE);
        if (!p) {
            printf("  VirtualAlloc(MEM_LARGE_PAGES): failed with %lu (8=not enough memory, "
                   "1314=privilege not held)\n",
                   GetLastError());
        } else {
            measure_page_size("MEM_LARGE_PAGES", (unsigned char *)p, BENCH_BYTES);
            VirtualFree(p, 0, MEM_RELEASE);
        }
    }
    {
        ULONGLONG frames = BENCH_BYTES / (2 * 1024 * 1024);
        ULONG_PTR *array = (ULONG_PTR *)calloc((size_t)frames, sizeof(ULONG_PTR));
        if (AllocateUserPhysicalPages(GetCurrentProcess(), &frames, array)) {
            void *p = (void *)(ULONG_PTR)MapUserPhysicalPages(NULL, frames, array);
            if (!p) {
                printf("  AWE MapUserPhysicalPages: failed with %lu\n", GetLastError());
            } else {
                measure_page_size("AWE (2 MiB frames)", (unsigned char *)p,
                                  (size_t)frames * 2 * 1024 * 1024);
                UnmapViewOfFile(p);
            }
            FreeUserPhysicalPages(GetCurrentProcess(), &frames, array);
        } else {
            printf("  AWE AllocateUserPhysicalPages: failed with %lu (1314=privilege not held)\n",
                   GetLastError());
        }
        free(array);
    }

    printf("== the TLB reach and the section indirection (pointer chase) ==\n");
    {
        const size_t sizes[] = {1, 4, 8, 16, 64, 256}; /* MiB */
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
            size_t bytes = sizes[i] << 20;
            printf("  %4lu MiB working set (%7lu pages)\n", (unsigned long)sizes[i],
                   (unsigned long)(bytes / 4096));
            chase_section(section, bytes);
            chase_anonymous(bytes);
        }
    }

    printf("== what a protection change (the GPU tracker's) costs ==\n");
    measure_protect_tlb();
    {
        unsigned char *anon = (unsigned char *)VirtualAlloc(NULL, BENCH_BYTES, MEM_RESERVE | MEM_COMMIT,
                                                            PAGE_READWRITE);
        if (anon) {
            measure_protect_cycle("pagefile section", view, BENCH_BYTES);
            measure_protect_cycle("anonymous memory", anon, BENCH_BYTES);
            VirtualFree(anon, 0, MEM_RELEASE);
        }
    }

    printf("== what the pagefile backing costs ==\n");
    measure_trim_cost("pagefile section", view, BENCH_BYTES);
    {
        unsigned char *anon = (unsigned char *)VirtualAlloc(NULL, BENCH_BYTES, MEM_RESERVE | MEM_COMMIT,
                                                            PAGE_READWRITE);
        if (anon) {
            measure_trim_cost("anonymous memory", anon, BENCH_BYTES);
            if (evict) {
                measure_eviction_cost("pagefile section", view, BENCH_BYTES);
                measure_eviction_cost("anonymous memory", anon, BENCH_BYTES);
            }
            VirtualFree(anon, 0, MEM_RELEASE);
        }
    }
    printf("  working set now: %lu MiB\n", (unsigned long)working_set_mb());
    printf("  (the runtime's real pool is %d MiB direct + %d MiB flexible)\n", RUNTIME_POOL_MB,
           RUNTIME_FLEX_MB);
    UnmapViewOfFile(view);
    CloseHandle(section);
    return 0;
}
