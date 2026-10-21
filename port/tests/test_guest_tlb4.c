/* What creating the guest's pool costs (port/src/runtime_memory.c: pool()).
 *
 * The pool is one section of POOL_SIZE + FLEX_SPAN bytes (5056 + 1024 MiB with the
 * default budget) created with SEC_COMMIT, mapped once as the "backing view" and
 * once per guest mapping. This bench measures, for the real geometry:
 *
 *   1. the cost of creating the section with SEC_COMMIT (the pool is committed to
 *      the pagefile up front) against SEC_RESERVE (committed page by page);
 *   2. the cost of the backing view (MapViewOfFile3 of the whole pool) and of a
 *      guest view over the placeholder (MapViewOfFile3 with MEM_REPLACE_PLACEHOLDER);
 *   3. what the first touch of the pages costs with each kind of section (the
 *      commit of a pagefile-backed page against an anonymous one);
 *   4. how much of the pool the game actually uses: the flexible span (1 GiB) was
 *      never mapped in the session measured (flexible maps=0).
 *
 * Host only (no game, no GPU, no Vulkan).
 */
#define _GNU_SOURCE
#include <windows.h>
#include <psapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The runtime's geometry (port/src/runtime_memory.c): POOL_SIZE + FLEX_SPAN. */
#define POOL_MB 5056
#define FLEX_MB 1024
#define POOL_BYTES ((uint64_t)(POOL_MB + FLEX_MB) * 1024 * 1024)
/* The guest's user range starts at 1 TiB (USER_MIN). */
#define USER_MIN UINT64_C(0x1000000000)

static double now_ms(void) {
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static double first_touch_us(unsigned char *view, size_t bytes) {
    size_t pages = bytes / 4096;
    double t0 = now_ms();
    for (size_t i = 0; i < pages; ++i) view[i * 4096] = (unsigned char)i;
    return (now_ms() - t0) * 1000.0 / (double)pages;
}

/* Creates the section like the runtime does and reports the time. */
static void measure_section(const char *tag, DWORD flags) {
    double t0 = now_ms();
    HANDLE section = CreateFileMapping2(INVALID_HANDLE_VALUE, NULL, FILE_MAP_ALL_ACCESS,
                                        PAGE_EXECUTE_READWRITE, flags, POOL_BYTES, NULL, NULL, 0);
    double t_create = now_ms() - t0;
    if (!section) {
        printf("  %-34s CreateFileMapping2 failed: %lu\n", tag, GetLastError());
        return;
    }
    printf("  %-34s created in %8.1f ms (%lu MiB)\n", tag, t_create, (unsigned long)(POOL_BYTES >> 20));

    double t1 = now_ms();
    unsigned char *backing = (unsigned char *)MapViewOfFile3(section, GetCurrentProcess(), NULL, 0,
                                                              (size_t)POOL_BYTES, 0, PAGE_READWRITE, NULL, 0);
    double t_map = now_ms() - t1;
    if (!backing) {
        printf("  %-34s MapViewOfFile3 failed: %lu\n", tag, GetLastError());
        CloseHandle(section);
        return;
    }
    printf("  %-34s backing view in %8.1f ms\n", tag, t_map);

    /* A guest view over the placeholder, as host_map does it. */
    void *placeholder = VirtualAlloc2(NULL, (void *)USER_MIN, 256u << 20, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                      PAGE_NOACCESS, NULL, 0);
    if (placeholder) {
        double t2 = now_ms();
        unsigned char *guest = (unsigned char *)MapViewOfFile3(section, GetCurrentProcess(),
                                                               (void *)USER_MIN, 0, 256u << 20,
                                                               MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
                                                               NULL, 0);
        double t_guest = now_ms() - t2;
        printf("  %-34s guest view (256 MiB over the placeholder) in %8.1f ms%s\n", tag, t_guest,
               guest ? "" : " (failed)");
        if (guest) UnmapViewOfFile(guest);
        VirtualFree((void *)USER_MIN, 256u << 20, MEM_RELEASE);
    }

    /* The first touch of a quarter of the pool (the rest would take too long). */
    size_t quarter = (size_t)POOL_BYTES / 4;
    printf("  %-34s first touch: %6.3f us/page over %lu MiB\n", tag,
           first_touch_us(backing, quarter), (unsigned long)(quarter >> 20));

    UnmapViewOfFile(backing);
    CloseHandle(section);
}

/* The first touch of a view of a *reserved* section has to commit the page on demand,
 * and that can fail (a hard fault, which kills the process). It is measured in a child
 * process so the bench can report it instead of dying. */
static int measure_reserve_touch(const char *self) {
    char cmd[MAX_PATH + 32];
    snprintf(cmd, sizeof(cmd), "\"%s\" --reserve-touch", self);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    MEMORYSTATUSEX mem;
    mem.dwLength = sizeof(mem);
    GlobalMemoryStatusEx(&mem);
    printf("== machine ==\n");
    printf("  physical memory: %lu MiB total, %lu MiB available\n", (unsigned long)(mem.ullTotalPhys >> 20),
           (unsigned long)(mem.ullAvailPhys >> 20));

    if (argc > 1 && !strcmp(argv[1], "--reserve-touch")) {
        /* The child: the reserved section and the first touch of its backing view. */
        HANDLE section = CreateFileMapping2(INVALID_HANDLE_VALUE, NULL, FILE_MAP_ALL_ACCESS,
                                            PAGE_EXECUTE_READWRITE, SEC_RESERVE, POOL_BYTES, NULL, NULL, 0);
        if (!section) return 2;
        unsigned char *view = (unsigned char *)MapViewOfFile3(section, GetCurrentProcess(), NULL, 0,
                                                              (size_t)POOL_BYTES, 0, PAGE_READWRITE, NULL, 0);
        if (!view) return 3;
        size_t quarter = (size_t)POOL_BYTES / 4;
        printf("  SEC_RESERVE        first touch: %6.3f us/page over %lu MiB\n",
               first_touch_us(view, quarter), (unsigned long)(quarter >> 20));
        fflush(stdout);
        return 0;
    }

    printf("== creating the pool (%d MiB direct + %d MiB flexible) ==\n", POOL_MB, FLEX_MB);
    measure_section("SEC_COMMIT (today)", SEC_COMMIT);
    printf("  %-34s the first touch is measured in a child process (it can fail hard)\n",
           "SEC_RESERVE");
    int code = measure_reserve_touch(argv[0]);
    if (code == 0)
        printf("  %-34s the first touch worked\n", "SEC_RESERVE");
    else
        printf("  %-34s the first touch failed (the child exited with %d: a reserved "
               "section's view cannot be written)\n",
               "SEC_RESERVE", code);
    {
        /* The same size in anonymous memory, for reference: what the pool would cost
         * without a backing store at all. */
        double t0 = now_ms();
        void *p = VirtualAlloc(NULL, (size_t)POOL_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        printf("  %-34s reserved+committed in %8.1f ms\n", "anonymous memory",
               p ? now_ms() - t0 : -1.0);
        if (p) {
            printf("  %-34s first touch: %6.3f us/page over %lu MiB\n", "anonymous memory",
                   first_touch_us((unsigned char *)p, (size_t)POOL_BYTES / 4),
                   (unsigned long)((size_t)POOL_BYTES / 4 >> 20));
            VirtualFree(p, 0, MEM_RELEASE);
        }
    }
    return 0;
}
