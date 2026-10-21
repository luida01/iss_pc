/* Probe: what page size does the guest's memory actually use on Windows, and can
 * huge pages be obtained here? It reproduces the runtime's pool creation (a
 * pagefile-backed section) and the alternatives the runtime could use.
 *
 * Build (MSYS2 CLANG64):
 *   clang -std=c11 -O2 -o out/test_guest_tlb_probe.exe tests/test_guest_tlb_probe.c -lpsapi
 */
#include <windows.h>
#include <psapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The runtime's guest pool: PS4 direct memory + the flexible span. */
#define POOL_MB 5056
#define FLEX_MB 1024

static void report(const char *tag) {
    MEMORY_BASIC_INFORMATION mbi;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    if (!VirtualQuery((const void *)&si, &mbi, sizeof(mbi))) return;
    printf("  [%s] system page size=%lu, allocation granularity=%lu\n", tag,
           (unsigned long)si.dwPageSize, (unsigned long)si.dwAllocationGranularity);
}

/* Counts how many 4 KiB pages of a mapping are resident by touching one byte per
 * page and reading the process page fault counter: a large-page mapping faults
 * once per 2 MiB, a small-page mapping once per 4 KiB. */
static void fault_granularity(const char *tag, unsigned char *base, size_t bytes) {
    DWORD before = 0, after = 0;
    PROCESS_MEMORY_COUNTERS pmc;
    size_t probes = bytes / 4096;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) before = pmc.PageFaultCount;
    for (size_t i = 0; i < probes; ++i) base[i * 4096] = (unsigned char)i;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) after = pmc.PageFaultCount;
    printf("  [%s] touched %zu pages of 4 KiB -> %lu faults (%.1f faults/page)\n", tag, probes,
           (unsigned long)(after - before),
           probes ? (double)(after - before) / (double)probes : 0.0);
}

int main(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    printf("== machine ==\n");
    printf("  page size=%lu, allocation granularity=%lu, processors=%lu\n",
           (unsigned long)si.dwPageSize, (unsigned long)si.dwAllocationGranularity,
           (unsigned long)si.dwNumberOfProcessors);
    {
        ULONG_PTR min = GetLargePageMinimum();
        printf("  GetLargePageMinimum=%llu (%s)\n", (unsigned long long)min,
               min ? "huge pages available" : "NO huge pages");
    }
    {
        /* Large pages need the "lock pages in memory" privilege. */
        HANDLE token = NULL;
        OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token);
        if (token) {
            LUID luid;
            if (LookupPrivilegeValueA(NULL, "SeLockMemoryPrivilege", &luid)) {
                TOKEN_PRIVILEGES tp;
                tp.PrivilegeCount = 1;
                tp.Privileges[0].Luid = luid;
                tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                printf("  SeLockMemoryPrivilege enable -> %lu (0=ok, 1314=not held)\n",
                       (unsigned long)AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), NULL, NULL));
            }
            CloseHandle(token);
        }
    }
    report("system");

    printf("== the runtime's current pool (pagefile-backed section) ==\n");
    {
        uint64_t bytes = (uint64_t)(POOL_MB + FLEX_MB) * 1024 * 1024;
        HANDLE section = CreateFileMapping2(INVALID_HANDLE_VALUE, NULL, FILE_MAP_ALL_ACCESS,
                                            PAGE_EXECUTE_READWRITE, SEC_COMMIT, bytes, NULL, NULL, 0);
        if (!section) {
            printf("  CreateFileMapping2 failed: %lu\n", GetLastError());
        } else {
            unsigned char *view = (unsigned char *)MapViewOfFile3(section, GetCurrentProcess(), NULL, 0,
                                                                   64u * 1024 * 1024, 0, PAGE_READWRITE,
                                                                   NULL, 0);
            if (!view) {
                printf("  MapViewOfFile3 failed: %lu\n", GetLastError());
            } else {
                fault_granularity("pagefile section", view, 64u * 1024 * 1024);
                UnmapViewOfFile(view);
            }
            CloseHandle(section);
        }
    }

    printf("== SEC_LARGE_PAGES on the same section ==\n");
    {
        uint64_t bytes = 64u * 1024 * 1024;
        HANDLE section = CreateFileMapping2(INVALID_HANDLE_VALUE, NULL, FILE_MAP_ALL_ACCESS,
                                            PAGE_EXECUTE_READWRITE,
                                            SEC_COMMIT | SEC_LARGE_PAGES, bytes, NULL, NULL, 0);
        if (!section) {
            printf("  CreateFileMapping2(SEC_LARGE_PAGES) failed: %lu (ERROR_INVALID_PARAMETER=87)\n",
                   GetLastError());
        } else {
            unsigned char *view = (unsigned char *)MapViewOfFile3(section, GetCurrentProcess(), NULL, 0,
                                                                   bytes, 0, PAGE_READWRITE, NULL, 0);
            if (!view) {
                printf("  MapViewOfFile3 failed: %lu\n", GetLastError());
            } else {
                fault_granularity("SEC_LARGE_PAGES", view, bytes);
                UnmapViewOfFile(view);
            }
            CloseHandle(section);
        }
    }

    printf("== VirtualAlloc with MEM_LARGE_PAGES ==\n");
    {
        void *p = VirtualAlloc(NULL, 64u * 1024 * 1024, MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES,
                                PAGE_READWRITE);
        if (!p) {
            printf("  VirtualAlloc(MEM_LARGE_PAGES) failed: %lu (8=not enough memory, 1314=privilege)\n",
                   GetLastError());
        } else {
            fault_granularity("MEM_LARGE_PAGES", (unsigned char *)p, 64u * 1024 * 1024);
            VirtualFree(p, 0, MEM_RELEASE);
        }
    }

    printf("== AWE (AllocateUserPhysicalPages + MapUserPhysicalPages) ==\n");
    {
        ULONGLONG frames = 32; /* 32 * 2 MiB = 64 MiB */
        ULONG_PTR *array = (ULONG_PTR *)calloc(frames, sizeof(ULONG_PTR));
        if (AllocateUserPhysicalPages(GetCurrentProcess(), &frames, array)) {
            /* The MinGW header types this as WINBOOL; the real API returns the address. */
            void *p = (void *)(ULONG_PTR)MapUserPhysicalPages(NULL, frames, array);
            if (!p) {
                printf("  MapUserPhysicalPages failed: %lu\n", GetLastError());
            } else {
                fault_granularity("AWE", (unsigned char *)p, (size_t)frames * 2u * 1024 * 1024);
                UnmapViewOfFile(p);
            }
            FreeUserPhysicalPages(GetCurrentProcess(), &frames, array);
        } else {
            printf("  AllocateUserPhysicalPages failed: %lu (1314=privilege not held)\n", GetLastError());
        }
        free(array);
    }
    return 0;
}
