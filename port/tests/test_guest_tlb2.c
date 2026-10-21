/* Focused: what does a protection change cost on a shared (section-backed)
 * mapping compared with anonymous memory, and does the cost come from the
 * VirtualProtect itself or from the write fault? */
#define _GNU_SOURCE
#include <windows.h>
#include <psapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double now_ms(void) {
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static LONG WINAPI vec_handler(EXCEPTION_POINTERS *info) {
    const EXCEPTION_RECORD *r = info->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    DWORD old;
    uintptr_t at = (uintptr_t)r->ExceptionInformation[1] & ~(uintptr_t)4095;
    VirtualProtect((void *)at, 4096, PAGE_READWRITE, &old);
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void protect_only(const char *tag, unsigned char *pool, size_t range, size_t bytes, int fresh) {
    const int rounds = 20;
    double total = 0;
    for (int r = 0; r < rounds; ++r) {
        size_t at = fresh ? ((size_t)r * range) % (bytes - range) : 0;
        DWORD old;
        double t0 = now_ms();
        VirtualProtect(pool + at, range, PAGE_READONLY, &old);
        VirtualProtect(pool + at, range, PAGE_READWRITE, &old);
        total += now_ms() - t0;
    }
    printf("  %-24s %7zu KiB range, %-9s: %7.3f ms per protect pair\n", tag, range >> 10,
           fresh ? "fresh" : "same", total / rounds);
}

int main(void) {
    AddVectoredExceptionHandler(1, vec_handler);
    const size_t bytes = 512u * 1024 * 1024;
    HANDLE sec = CreateFileMapping2(INVALID_HANDLE_VALUE, NULL, FILE_MAP_ALL_ACCESS, PAGE_READWRITE,
                                    SEC_COMMIT, bytes, NULL, NULL, 0);
    unsigned char *sv = (unsigned char *)MapViewOfFile3(sec, GetCurrentProcess(), NULL, 0, bytes, 0,
                                                        PAGE_READWRITE, NULL, 0);
    unsigned char *av = (unsigned char *)VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!sv || !av) { printf("allocation failed\n"); return 1; }
    memset(sv, 0, bytes);
    memset(av, 0, bytes);

    printf("== VirtualProtect only (no write fault) ==\n");
    const size_t ranges[] = {64u << 10, 1u << 20, 8u << 20, 32u << 20};
    for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); ++i) {
        protect_only("pagefile section", sv, ranges[i], bytes, 1);
        protect_only("anonymous memory", av, ranges[i], bytes, 1);
    }
    printf("== the same range protected again and again ==\n");
    for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); ++i) {
        protect_only("pagefile section", sv, ranges[i], bytes, 0);
        protect_only("anonymous memory", av, ranges[i], bytes, 0);
    }
    printf("== the write fault itself (page already unprotected) ==\n");
    {
        for (int t = 0; t < 2; ++t) {
            unsigned char *p = t ? sv : av;
            const char *tag = t ? "pagefile section" : "anonymous memory";
            const int rounds = 2000;
            double t0 = now_ms();
            for (int r = 0; r < rounds; ++r) {
                DWORD old;
                VirtualProtect(p, 4096, PAGE_READONLY, &old);
                p[r % (bytes - 4096)] = 1;
            }
            printf("  %-24s protect(4 KiB) + write fault: %6.2f us per fault\n", tag,
                   (now_ms() - t0) * 1000.0 / rounds);
        }
    }
    UnmapViewOfFile(sv);
    UnmapViewOfFile(av);
    VirtualFree(av, 0, MEM_RELEASE);
    CloseHandle(sec);
    return 0;
}
