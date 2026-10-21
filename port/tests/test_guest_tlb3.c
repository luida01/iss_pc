/* Why a protection change on the guest's pool costs more than on plain memory.
 *
 * The guest memory bench (tests/test_guest_tlb.c) measured, on this machine:
 *
 *   pagefile section   protect+write fault+unprotect over 32 MiB: 1.26 ms
 *   anonymous memory   protect+write fault+unprotect over 32 MiB: 0.12 ms
 *
 * The write fault itself costs the same in both (4.2 us), so the difference looks
 * like it is in VirtualProtect. This bench isolates the cause:
 *
 *   1. where the section's data lives: pagefile (D:, an HDD), a file on the NVMe
 *      (C:), a file on the HDD (D:), or anonymous memory (no backing store at all);
 *   2. whether the pages of the range are present (already written) or not: a
 *      protection change over pages that are not there has nothing to update;
 *   3. whether the range moves: the first protection change of a range inside a
 *      view splits that view's page tables, the next ones only update them;
 *   4. how many views of the same section exist (the runtime maps the pool once per
 *      guest mapping, plus one "backing view");
 *   5. the first-touch write fault and the re-read after the working set is trimmed
 *      for each backing store (what the pool costs when the machine is short of RAM).
 *
 * Host only (no game, no GPU, no Vulkan). Build: bash port/tests/run_guest_tlb.sh
 * (the same script builds this file; run it directly with out/test_guest_tlb3.exe).
 */
#define _GNU_SOURCE
#include <windows.h>
#include <psapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define BENCH_MB 512
#define BENCH_BYTES ((size_t)BENCH_MB * 1024 * 1024)

/* The NVMe (C:) and the HDD with the pagefile (D:). */
#define NVME_DIR L"C:\\Users\\luisd\\AppData\\Local\\Temp"
#define HDD_DIR L"D:\\"

static double now_ms(void) {
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

/* ---- backing stores ---- */

typedef struct {
    const char *tag;
    HANDLE section;
    HANDLE file; /* the file behind the section, if any */
    unsigned char *view;
} Backing;

static int backing_pagefile(Backing *b) {
    b->tag = "pagefile section (today)";
    b->file = NULL;
    b->section = CreateFileMapping2(INVALID_HANDLE_VALUE, NULL, FILE_MAP_ALL_ACCESS,
                                     PAGE_EXECUTE_READWRITE, SEC_COMMIT, (uint64_t)BENCH_BYTES, NULL,
                                     NULL, 0);
    if (!b->section) return -1;
    b->view = (unsigned char *)MapViewOfFile3(b->section, GetCurrentProcess(), NULL, 0, BENCH_BYTES, 0,
                                              PAGE_READWRITE, NULL, 0);
    return b->view ? 0 : -1;
}

/* A file-backed section: the data lives in that file, not in the pagefile. The file
 * is sparse (only the written pages take space) and is deleted at the end. */
static int backing_file(Backing *b, const char *tag, const wchar_t *dir) {
    wchar_t path[MAX_PATH];
    wcscpy(path, dir);
    wcscat(path, L"\\tlg-guest-tlb-bench.bin");
    b->tag = tag;
    b->file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (b->file == INVALID_HANDLE_VALUE) {
        b->file = NULL;
        return -1;
    }
    LARGE_INTEGER size;
    size.QuadPart = (LONGLONG)BENCH_BYTES;
    if (!SetFilePointerEx(b->file, size, NULL, FILE_BEGIN) || !SetEndOfFile(b->file)) return -1;
    b->section = CreateFileMappingW(b->file, NULL, PAGE_READWRITE, 0, 0, NULL);
    if (!b->section) return -1;
    b->view = (unsigned char *)MapViewOfFile3(b->section, GetCurrentProcess(), NULL, 0, BENCH_BYTES, 0,
                                              PAGE_READWRITE, NULL, 0);
    return b->view ? 0 : -1;
}

static int backing_anonymous(Backing *b) {
    b->tag = "anonymous memory";
    b->file = NULL;
    b->section = NULL;
    b->view = (unsigned char *)VirtualAlloc(NULL, BENCH_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return b->view ? 0 : -1;
}

static void backing_free(Backing *b) {
    if (b->view) {
        if (b->section) UnmapViewOfFile(b->view);
        else VirtualFree(b->view, 0, MEM_RELEASE);
    }
    if (b->section) CloseHandle(b->section);
    if (b->file) CloseHandle(b->file);
}

/* ---- the measurements ---- */

/* One protect/unprotect pair over a range: what the GPU tracker pays every time it
 * takes the guest's pages read-only and gives them back. */
static double protect_pair(unsigned char *view, size_t at, size_t range, int rounds) {
    DWORD old;
    double total = 0;
    for (int r = 0; r < rounds; ++r) {
        double t0 = now_ms();
        if (!VirtualProtect(view + at, range, PAGE_READONLY, &old)) return -1;
        if (!VirtualProtect(view + at, range, PAGE_READWRITE, &old)) return -1;
        total += now_ms() - t0;
    }
    return total / rounds;
}

/* The same, but every round a different range: the first protection change of a range
 * inside a view has to split that view's page tables, the following ones only update
 * them. This is what the GPU tracker does (it protects many different ranges). */
static double protect_pair_moving(unsigned char *view, size_t range, int rounds) {
    DWORD old;
    double total = 0;
    size_t step = (BENCH_BYTES / (size_t)rounds) & ~((size_t)4095);
    for (int r = 0; r < rounds; ++r) {
        size_t at = (step * (size_t)r) % (BENCH_BYTES - range);
        double t0 = now_ms();
        if (!VirtualProtect(view + at, range, PAGE_READONLY, &old)) return -1;
        if (!VirtualProtect(view + at, range, PAGE_READWRITE, &old)) return -1;
        total += now_ms() - t0;
    }
    return total / rounds;
}

/* The first touch of every page: the commit of the section (or of the anonymous page). */
static double first_touch(unsigned char *view, size_t bytes) {
    size_t pages = bytes / 4096;
    double t0 = now_ms();
    for (size_t i = 0; i < pages; ++i) view[i * 4096] = (unsigned char)i;
    return (now_ms() - t0) * 1000.0 / (double)pages; /* us/page */
}

/* The re-read after the working set is trimmed: the pages come back from the pagefile
 * (or from the file) when the machine is short of RAM. */
static double reread_after_trim(unsigned char *view, size_t bytes) {
    size_t pages = bytes / 4096;
    SIZE_T min = 0, max = 0;
    GetProcessWorkingSetSize(GetCurrentProcess(), &min, &max);
    SetProcessWorkingSetSize(GetCurrentProcess(), 1, 1);
    double t0 = now_ms();
    volatile unsigned long sum = 0;
    for (size_t i = 0; i < pages; ++i) sum += view[i * 4096];
    double us = (now_ms() - t0) * 1000.0 / (double)pages;
    SetProcessWorkingSetSize(GetCurrentProcess(), min, max);
    (void)sum;
    return us;
}

/* The port resolves a write to a protected page in a vectored exception handler
 * (port/src/probe.c: vectored_fault), which restores the write protection and lets
 * the write continue. The bench does the same, so the measured cost is the one the
 * guest pays. */
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

static void measure_backing(const char *label, int kind, const wchar_t *dir) {
    Backing b;
    memset(&b, 0, sizeof(b));
    int failed = kind == 0 ? backing_pagefile(&b)
                           : kind == 1 ? backing_file(&b, label, dir) : backing_anonymous(&b);
    if (failed) {
        printf("  %-28s (not available here: error %lu)\n", label, GetLastError());
        backing_free(&b);
        return;
    }
    printf("  %s\n", b.tag);
    printf("    first touch      : %6.3f us/page\n", first_touch(b.view, BENCH_BYTES));
    const size_t ranges[] = {1u << 20, 8u << 20, 32u << 20};
    for (size_t i = 0; i < 3; ++i) {
        size_t range = ranges[i];
        double ms = protect_pair(b.view, 0, range, 20);
        printf("    protect pair     : %6.3f ms over %2lu MiB (%6.1f ns/page), pages present\n", ms,
               (unsigned long)(range >> 20), ms * 1e6 / (double)(range / 4096));
    }
    printf("    protect pair, new range each time (32 MiB): %6.3f ms\n",
           protect_pair_moving(b.view, 32u << 20, 16));
    printf("    re-read trimmed  : %6.3f us/page\n", reread_after_trim(b.view, BENCH_BYTES));
    backing_free(&b);
}

/* The same protect pair, but over pages that were never written: they are not there,
 * so there is nothing to update. */
static void measure_absent(void) {
    const size_t range = 32u << 20;
    const double per_page = 1e6 / (double)(range / 4096); /* ns per page of a pair */
    printf("  %-28s\n", "pages not present");
    {
        HANDLE section = CreateFileMapping2(INVALID_HANDLE_VALUE, NULL, FILE_MAP_ALL_ACCESS,
                                             PAGE_EXECUTE_READWRITE, SEC_COMMIT, (uint64_t)BENCH_BYTES,
                                             NULL, NULL, 0);
        unsigned char *view = (unsigned char *)MapViewOfFile3(section, GetCurrentProcess(), NULL, 0,
                                                              BENCH_BYTES, 0, PAGE_READWRITE, NULL, 0);
        if (view) {
            double ms = protect_pair(view, 0, range, 20);
            printf("    pagefile section : %6.3f ms per pair over 32 MiB (%6.1f ns/page)\n", ms,
                   ms * per_page);
            UnmapViewOfFile(view);
        }
        CloseHandle(section);
    }
    {
        unsigned char *anon = (unsigned char *)VirtualAlloc(NULL, BENCH_BYTES, MEM_RESERVE | MEM_COMMIT,
                                                             PAGE_READWRITE);
        if (anon) {
            double ms = protect_pair(anon, 0, range, 20);
            printf("    anonymous memory : %6.3f ms per pair over 32 MiB (%6.1f ns/page)\n", ms,
                   ms * per_page);
            VirtualFree(anon, 0, MEM_RELEASE);
        }
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    /* The write to a protected page faults; the port's exception handler resolves
     * it, and so does this bench's (installed before anything else). */
    AddVectoredExceptionHandler(1, vec_handler);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    printf("== machine ==\n");
    printf("  page size=%lu, logical processors=%lu\n", (unsigned long)si.dwPageSize,
           (unsigned long)si.dwNumberOfProcessors);
    MEMORYSTATUSEX mem;
    mem.dwLength = sizeof(mem);
    GlobalMemoryStatusEx(&mem);
    printf("  physical memory: %lu MiB total, %lu MiB available\n", (unsigned long)(mem.ullTotalPhys >> 20),
           (unsigned long)(mem.ullAvailPhys >> 20));

    printf("== 1. where the section's data lives ==\n");
    measure_backing("file on the NVMe (C:)", 1, NVME_DIR);
    measure_backing("file on the HDD (D:)", 1, HDD_DIR);
    measure_backing("pagefile section (D: pagefile)", 0, NULL);
    measure_backing("anonymous memory", 2, NULL);

    printf("== 2. the same, over pages that were never written ==\n");
    measure_absent();

    printf("== 3. how many views of the same section exist ==\n");
    /* The runtime maps the pool once per guest mapping (the game maps its direct
     * memory in several chunks) plus one "backing view" the host writes through. */
    for (int views = 1; views <= 8; views *= 2) {
        HANDLE section = CreateFileMapping2(INVALID_HANDLE_VALUE, NULL, FILE_MAP_ALL_ACCESS,
                                             PAGE_EXECUTE_READWRITE, SEC_COMMIT, (uint64_t)BENCH_BYTES,
                                             NULL, NULL, 0);
        if (!section) break;
        unsigned char *first = NULL;
        for (int v = 0; v < views; ++v) {
            unsigned char *p = (unsigned char *)MapViewOfFile3(section, GetCurrentProcess(), NULL, 0,
                                                              BENCH_BYTES, 0, PAGE_READWRITE, NULL, 0);
            if (!p) break;
            if (v == 0) first = p;
            else UnmapViewOfFile(p); /* only the number of views matters */
        }
        double ms = protect_pair(first, 0, 32u << 20, 20);
        printf("  %d view(s) of the section: protect pair over 32 MiB: %6.3f ms (%6.1f ns/page)\n",
               views, ms, ms * 1e6 / (double)((32u << 20) / 4096));
        UnmapViewOfFile(first);
        CloseHandle(section);
    }

    printf("== 4. what the write fault costs (protect, write, unprotect) ==\n");
    {
        HANDLE section = CreateFileMapping2(INVALID_HANDLE_VALUE, NULL, FILE_MAP_ALL_ACCESS,
                                             PAGE_EXECUTE_READWRITE, SEC_COMMIT, (uint64_t)BENCH_BYTES,
                                             NULL, NULL, 0);
        unsigned char *view = (unsigned char *)MapViewOfFile3(section, GetCurrentProcess(), NULL, 0,
                                                              BENCH_BYTES, 0, PAGE_READWRITE, NULL, 0);
        if (view) {
            DWORD old;
            const size_t pages = BENCH_BYTES / 4096;
            double t0 = now_ms();
            for (size_t i = 0; i < pages; ++i) {
                VirtualProtect(view + i * 4096, 4096, PAGE_READONLY, &old);
                view[i * 4096] = 1; /* the write faults */
                VirtualProtect(view + i * 4096, 4096, PAGE_READWRITE, &old);
            }
            double ms = now_ms() - t0;
            printf("  pagefile section: %6.3f us per page (%6.1f ns/page)\n",
                   ms * 1000.0 / (double)pages, ms * 1e6 / (double)pages);
            UnmapViewOfFile(view);
        }
        CloseHandle(section);
    }
    {
        unsigned char *anon = (unsigned char *)VirtualAlloc(NULL, BENCH_BYTES, MEM_RESERVE | MEM_COMMIT,
                                                             PAGE_READWRITE);
        if (anon) {
            DWORD old;
            const size_t pages = BENCH_BYTES / 4096;
            double t0 = now_ms();
            for (size_t i = 0; i < pages; ++i) {
                VirtualProtect(anon + i * 4096, 4096, PAGE_READONLY, &old);
                anon[i * 4096] = 1;
                VirtualProtect(anon + i * 4096, 4096, PAGE_READWRITE, &old);
            }
            double ms = now_ms() - t0;
            printf("  anonymous      : %6.3f us per page (%6.1f ns/page)\n",
                   ms * 1000.0 / (double)pages, ms * 1e6 / (double)pages);
            VirtualFree(anon, 0, MEM_RELEASE);
        }
    }
    return 0;
}
