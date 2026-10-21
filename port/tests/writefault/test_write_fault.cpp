// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: what a write fault of the GPU page tracker costs on this host, and where the time goes.
//
// The port protects the guest pages the GPU watches, so every guest write to a watched page
// raises an access violation that the loader's VEH turns into a fault handler
// (port/gpu/shadps4/video_core/page_manager.cpp). The handler unprotects the page (and a
// window around it) and lets the store be retried.
//
// This bench reproduces that cycle without the game and without Vulkan: N threads walk a pool
// writing 8 bytes per page (the guest filling its per-frame buffers), a "GPU" thread re-protects
// the pages (the upload), and a vectored exception handler does what the port's does. It
// measures the end-to-end cost per fault and splits it in:
//
//   - dispatch: the kernel's fault handling plus the entry/exit of the handler (measured as the
//     time the writing thread is away from its loop, minus the time inside the handler)
//   - handler: what the handler does besides protecting (the bookkeeping of the tracker)
//   - protect: the VirtualProtect calls (the mprotect of the tracker)
//
// Modes (--mode=):
//   page    unprotect the faulting page only (one VirtualProtect of 4 KiB)
//   window  unprotect the page and a 256 KiB window (two VirtualProtect calls): what the port
//           does today (BufferCache::ExtendWriteFault after the invalidation)
//   merged  unprotect the union of both in one VirtualProtect: the change this bench measures
//   state   like window, but a protection cache skips the calls that would not change anything
//   guard   the pages carry PAGE_GUARD instead of PAGE_NOACCESS: the kernel drops the guard on
//           the first touch, so the handler does not protect anything
//   none    no protection at all: the cost of the writes without any fault
//
// Usage: test_write_fault [--threads=N] [--pool-mib=N] [--window-kib=N] [--mode=M] [--seconds=N]

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

constexpr size_t kPageSize = 4096;

enum Mode {
    kModePage = 0,
    kModeWindow,
    kModeMerged,
    kModeState,
    kModeGuard,
    kModeNone,
    kModeGuardCheck,
};

const char* ModeName(int mode) {
    switch (mode) {
    case kModePage:
        return "page";
    case kModeWindow:
        return "window";
    case kModeMerged:
        return "merged";
    case kModeState:
        return "state";
    case kModeGuard:
        return "guard";
    case kModeGuardCheck:
        return "guardcheck";
    default:
        return "none";
    }
}

struct Options {
    int threads = 8;
    int pool_mib = 256;
    int window_kib = 256;
    int mode = kModeWindow;
    double seconds = 3.0;
    // The port protects the pages the GPU watches as read-only (a write faults, a read does
    // not); --protect=no uses no access, like a readback page.
    bool read_only = true;
    // The guest pool is a view of a section backed by the page file; --section=0 uses plain
    // anonymous memory, to compare the two.
    bool section = true;
};

Options g_opts;

// The pool the writers use, and the protection each page is in (0 = readable/writable,
// 1 = protected). The state cache needs one byte per page.
uint8_t* g_pool = nullptr;
size_t g_pool_size = 0;
std::vector<uint8_t> g_protected;

std::atomic<uint64_t> g_faults{0};
std::atomic<uint64_t> g_handler_tsc{0};
std::atomic<uint64_t> g_protect_tsc{0};
std::atomic<uint64_t> g_protect_calls{0};
std::atomic<uint64_t> g_protect_pages{0};
std::atomic<bool> g_stop{false};

// The handler runs on the faulting thread, so a thread-local counter tells each writer whether
// its own store faulted.
thread_local uint64_t tl_faults = 0;
// The kind of access of the last fault (1 = write, 0 = read).
std::atomic<int> g_last_access{-1};

double g_tsc_hz = 0.0;

inline uint64_t Tsc() { return __rdtsc(); }

// One VirtualProtect over a range of the pool.
void ProtectRange(uintptr_t addr, size_t size, bool guard) {
    DWORD old = 0;
    DWORD prot;
    if (guard) {
        prot = PAGE_READWRITE | PAGE_GUARD;
    } else if (g_opts.read_only) {
        prot = PAGE_READONLY;
    } else {
        prot = PAGE_NOACCESS;
    }
    VirtualProtect(reinterpret_cast<void*>(addr), size, prot, &old);
    g_protect_calls.fetch_add(1, std::memory_order_relaxed);
    g_protect_pages.fetch_add(size / kPageSize, std::memory_order_relaxed);
}

void UnprotectRange(uintptr_t addr, size_t size) {
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(addr), size, PAGE_READWRITE, &old);
    g_protect_calls.fetch_add(1, std::memory_order_relaxed);
    g_protect_pages.fetch_add(size / kPageSize, std::memory_order_relaxed);
}

LONG CALLBACK FaultHandler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    const bool guard = rec->ExceptionCode == STATUS_GUARD_PAGE_VIOLATION;
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION && !guard) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (rec->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const uintptr_t addr = reinterpret_cast<uintptr_t>(rec->ExceptionInformation[1]);
    if (g_opts.mode == kModeGuardCheck) {
        g_last_access.store(rec->ExceptionInformation[0], std::memory_order_relaxed);
        g_faults.fetch_add(1, std::memory_order_relaxed);
        ++tl_faults;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (addr < reinterpret_cast<uintptr_t>(g_pool) ||
        addr >= reinterpret_cast<uintptr_t>(g_pool) + g_pool_size) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const uint64_t t0 = Tsc();
    g_faults.fetch_add(1, std::memory_order_relaxed);
    ++tl_faults;

    const uintptr_t page = addr & ~(kPageSize - 1);
    const uint64_t t_protect0 = Tsc();

    switch (g_opts.mode) {
    case kModeNone:
        // Nothing is protected: the reference (the cost of the writes without any fault).
        break;
    case kModePage:
        UnprotectRange(page, kPageSize);
        break;
    case kModeWindow:
    case kModeState: {
        const size_t window = static_cast<size_t>(g_opts.window_kib) * 1024;
        const uintptr_t start = page & ~(window - 1);
        if (g_opts.mode == kModeState) {
            // The state cache: skip the calls that would not change anything.
            const size_t pages = window / kPageSize;
            const size_t base = (start - reinterpret_cast<uintptr_t>(g_pool)) / kPageSize;
            size_t first = 0;
            size_t run = 0;
            for (size_t i = 0; i <= pages; ++i) {
                const bool prot = i < pages && g_protected[base + i] != 0;
                if (prot) {
                    if (run == 0) {
                        first = i;
                    }
                    ++run;
                } else if (run) {
                    UnprotectRange(start + first * kPageSize, run * kPageSize);
                    for (size_t k = first; k < first + run; ++k) {
                        g_protected[base + k] = 0;
                    }
                    run = 0;
                }
            }
        } else {
            // What the port does: the page, then the window around it (two calls).
            UnprotectRange(page, kPageSize);
            UnprotectRange(start, window);
        }
        break;
    }
    case kModeMerged: {
        // The change: one call for the union of the page and the window.
        const size_t window = static_cast<size_t>(g_opts.window_kib) * 1024;
        const uintptr_t start = page & ~(window - 1);
        UnprotectRange(start, window);
        break;
    }
    case kModeGuard:
        // The kernel already dropped the guard: nothing to protect.
        break;
    }
    const uint64_t t_protect1 = Tsc();
    g_protect_tsc.fetch_add(t_protect1 - t_protect0, std::memory_order_relaxed);
    g_handler_tsc.fetch_add(Tsc() - t0, std::memory_order_relaxed);
    return EXCEPTION_CONTINUE_EXECUTION;
}

// The writers: they walk the pool writing 8 bytes per page, like the guest filling its
// per-frame buffers. Each buffer is window_kib long (the page the fault unprotects).
struct WriterStats {
    uint64_t stores = 0;
    uint64_t faults = 0;
    uint64_t fault_tsc_min = ~0ull;
    uint64_t fault_tsc_max = 0;
    uint64_t fault_tsc_sum = 0;
    std::vector<uint64_t> samples;
};

void WriterThread(int id, WriterStats* out, double seconds) {
    const size_t window = static_cast<size_t>(g_opts.window_kib) * 1024;
    const size_t buffer_pages = window / kPageSize;
    const size_t total_pages = g_pool_size / kPageSize;
    const size_t buffers = total_pages / buffer_pages;
    const uint64_t deadline = Tsc() + static_cast<uint64_t>(seconds * g_tsc_hz);

    size_t buffer = id % buffers;
    uint64_t before = tl_faults;
    while (!g_stop.load(std::memory_order_relaxed) && Tsc() < deadline) {
        for (size_t p = 0; p < buffer_pages; ++p) {
            const size_t index = buffer * buffer_pages + p;
            uint8_t* addr = g_pool + index * kPageSize;
            const uint64_t t0 = Tsc();
            *reinterpret_cast<volatile uint64_t*>(addr) = 0x1234567890abcdefull;
            const uint64_t t1 = Tsc();
            ++out->stores;
            if (tl_faults != before) {
                before = tl_faults;
                ++out->faults;
                const uint64_t delta = t1 - t0;
                out->fault_tsc_min = std::min(out->fault_tsc_min, delta);
                out->fault_tsc_max = std::max(out->fault_tsc_max, delta);
                out->fault_tsc_sum += delta;
                if (out->samples.size() < 200000) {
                    out->samples.push_back(delta);
                }
            }
        }
        buffer = (buffer + 1) % buffers;
    }
}

// The "GPU" thread: it re-protects the pages the writers went through, like the upload does
// when it takes the modified pages back.
void GpuThread(double seconds) {
    const size_t window = static_cast<size_t>(g_opts.window_kib) * 1024;
    const size_t total_pages = g_pool_size / kPageSize;
    const size_t buffers = total_pages / (window / kPageSize);
    const uint64_t deadline = Tsc() + static_cast<uint64_t>(seconds * g_tsc_hz);
    size_t buffer = 0;
    while (!g_stop.load(std::memory_order_relaxed) && Tsc() < deadline) {
        if (g_opts.mode == kModeNone) {
            break;
        }
        const uintptr_t start = reinterpret_cast<uintptr_t>(g_pool) + buffer * window;
        ProtectRange(start, window, g_opts.mode == kModeGuard);
        if (g_opts.mode == kModeState) {
            const size_t base = buffer * (window / kPageSize);
            for (size_t i = 0; i < window / kPageSize; ++i) {
                g_protected[base + i] = 1;
            }
        }
        buffer = (buffer + 1) % buffers;
        // The GPU is not as fast as the writers: a short pause between the uploads.
        Sleep(1);
    }
}

double MeasureTscHz() {
    LARGE_INTEGER freq, c0, c1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c0);
    const uint64_t t0 = Tsc();
    Sleep(200);
    QueryPerformanceCounter(&c1);
    const uint64_t t1 = Tsc();
    const double seconds = static_cast<double>(c1.QuadPart - c0.QuadPart) / freq.QuadPart;
    return static_cast<double>(t1 - t0) / seconds;
}

void PrintUsage() {
    std::printf("usage: test_write_fault [--threads=N] [--pool-mib=N] [--window-kib=N] "
                "[--protect=ro|no] [--section=0|1] "
                "[--mode=page|window|merged|state|guard|none|guardcheck] [--seconds=N]\n");
}

// The semantics of PAGE_GUARD that the port would rely on: the kernel drops the guard of the
// page that faulted (so the rest of the range keeps it) and reports the kind of access.
void GuardCheck() {
    constexpr size_t kPages = 16;
    uint8_t* region = static_cast<uint8_t*>(
        VirtualAlloc(nullptr, kPages * kPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!region) {
        std::printf("VirtualAlloc failed\n");
        return;
    }
    std::memset(region, 0, kPages * kPageSize);

    DWORD old = 0;
    const bool ok = VirtualProtect(region, kPages * kPageSize, PAGE_READWRITE | PAGE_GUARD, &old) != 0;
    std::printf("PAGE_GUARD on %zu pages: %s\n", kPages, ok ? "ok" : "failed");
    if (!ok) {
        return;
    }

    const uint64_t before = g_faults.load();
    *reinterpret_cast<volatile uint64_t*>(region + 0 * kPageSize) = 1;
    const uint64_t after0 = g_faults.load();
    *reinterpret_cast<volatile uint64_t*>(region + 1 * kPageSize) = 1;
    const uint64_t after1 = g_faults.load();
    *reinterpret_cast<volatile uint64_t*>(region + 1 * kPageSize) = 2;
    const uint64_t after1b = g_faults.load();
    const uint64_t read0 = *reinterpret_cast<volatile uint64_t*>(region + 0 * kPageSize);
    const uint64_t after_read0 = g_faults.load();
    *reinterpret_cast<volatile uint64_t*>(region + 2 * kPageSize) = 3;
    const uint64_t after2 = g_faults.load();

    std::printf("  write page 0: %s\n", after0 > before ? "fault" : "no fault");
    std::printf("  write page 1: %s (the guard of the range survived the first fault: %s)\n",
                after1 > after0 ? "fault" : "no fault", after1 > after0 ? "per page" : "per range");
    std::printf("  write page 1 again: %s\n", after1b > after1 ? "fault" : "no fault");
    std::printf("  read page 0: %s (value %" PRIu64 ")\n", after_read0 > after1b ? "fault" : "no fault",
                read0);
    std::printf("  write page 2: %s\n", after2 > after_read0 ? "fault" : "no fault");
    std::printf("  access type of the last fault: %s\n",
                g_last_access == 1 ? "write" : (g_last_access == 0 ? "read" : "unknown"));
    VirtualFree(region, 0, MEM_RELEASE);
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strncmp(arg, "--threads=", 10) == 0) {
            g_opts.threads = std::atoi(arg + 10);
        } else if (std::strncmp(arg, "--pool-mib=", 11) == 0) {
            g_opts.pool_mib = std::atoi(arg + 11);
        } else if (std::strncmp(arg, "--window-kib=", 13) == 0) {
            g_opts.window_kib = std::atoi(arg + 13);
        } else if (std::strncmp(arg, "--seconds=", 10) == 0) {
            g_opts.seconds = std::atof(arg + 10);
        } else if (std::strncmp(arg, "--protect=", 10) == 0) {
            const char* kind = arg + 10;
            if (std::strcmp(kind, "ro") == 0) {
                g_opts.read_only = true;
            } else if (std::strcmp(kind, "no") == 0) {
                g_opts.read_only = false;
            } else {
                PrintUsage();
                return 2;
            }
        } else if (std::strncmp(arg, "--section=", 10) == 0) {
            g_opts.section = std::atoi(arg + 10) != 0;
        } else if (std::strncmp(arg, "--mode=", 7) == 0) {
            const char* mode = arg + 7;
            if (std::strcmp(mode, "page") == 0) {
                g_opts.mode = kModePage;
            } else if (std::strcmp(mode, "window") == 0) {
                g_opts.mode = kModeWindow;
            } else if (std::strcmp(mode, "merged") == 0) {
                g_opts.mode = kModeMerged;
            } else if (std::strcmp(mode, "state") == 0) {
                g_opts.mode = kModeState;
            } else if (std::strcmp(mode, "guard") == 0) {
                g_opts.mode = kModeGuard;
            } else if (std::strcmp(mode, "none") == 0) {
                g_opts.mode = kModeNone;
            } else if (std::strcmp(mode, "guardcheck") == 0) {
                g_opts.mode = kModeGuardCheck;
            } else {
                PrintUsage();
                return 2;
            }
        } else {
            PrintUsage();
            return 2;
        }
    }
    if (g_opts.window_kib < 4) {
        g_opts.window_kib = 4;
    }

    g_tsc_hz = MeasureTscHz();

    if (g_opts.mode == kModeGuardCheck) {
        AddVectoredExceptionHandler(1, FaultHandler);
        GuardCheck();
        return 0;
    }

    g_pool_size = static_cast<size_t>(g_opts.pool_mib) * 1024 * 1024;
    if (g_opts.section) {
        // The guest pool: a view of a section backed by the page file.
        HANDLE file = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                         g_pool_size >> 32, g_pool_size & 0xffffffffu, nullptr);
        if (!file) {
            std::printf("CreateFileMapping failed (%lu)\n", GetLastError());
            return 1;
        }
        g_pool = static_cast<uint8_t*>(
            MapViewOfFile(file, FILE_MAP_ALL_ACCESS, 0, 0, g_pool_size));
        if (!g_pool) {
            std::printf("MapViewOfFile failed (%lu)\n", GetLastError());
            return 1;
        }
    } else {
        g_pool = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, g_pool_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    }
    if (!g_pool) {
        std::printf("VirtualAlloc failed\n");
        return 1;
    }
    // Touch every page so the pool is resident (the bench measures the protection, not the
    // first touch of a page).
    std::memset(g_pool, 0, g_pool_size);
    g_protected.assign(g_pool_size / kPageSize, 0);

    // Everything starts protected (the tracker protects the pages the GPU watches).
    for (size_t i = 0; i < g_pool_size && g_opts.mode != kModeNone; i += kPageSize) {
        ProtectRange(reinterpret_cast<uintptr_t>(g_pool) + i, kPageSize, g_opts.mode == kModeGuard);
        g_protected[i / kPageSize] = 1;
    }
    g_protect_calls.store(0);
    g_protect_pages.store(0);

    AddVectoredExceptionHandler(1, FaultHandler);

    std::vector<WriterStats> stats(g_opts.threads);
    std::vector<std::thread> writers;
    writers.reserve(g_opts.threads);
    for (int i = 0; i < g_opts.threads; ++i) {
        writers.emplace_back(WriterThread, i, &stats[i], g_opts.seconds);
    }
    std::thread gpu(GpuThread, g_opts.seconds);

    for (auto& w : writers) {
        w.join();
    }
    gpu.join();
    g_stop.store(true);

    const uint64_t faults = g_faults.load();
    const uint64_t handler_tsc = g_handler_tsc.load();
    const uint64_t protect_tsc = g_protect_tsc.load();
    const uint64_t protect_calls = g_protect_calls.load();
    const uint64_t protect_pages = g_protect_pages.load();

    uint64_t stores = 0, w_faults = 0, sum = 0, min = ~0ull, max = 0;
    std::vector<uint64_t> samples;
    for (const auto& s : stats) {
        stores += s.stores;
        w_faults += s.faults;
        sum += s.fault_tsc_sum;
        min = std::min(min, s.fault_tsc_min);
        max = std::max(max, s.fault_tsc_max);
        samples.insert(samples.end(), s.samples.begin(), s.samples.end());
    }
    std::sort(samples.begin(), samples.end());
    const double us_per_tsc = 1e6 / g_tsc_hz;

    const double handler_us = static_cast<double>(handler_tsc) * us_per_tsc;
    const double protect_us = static_cast<double>(protect_tsc) * us_per_tsc;
    const double faults_f = static_cast<double>(faults ? faults : 1);

    std::printf("mode=%s threads=%d pool=%d MiB window=%d KiB protect=%s backing=%s seconds=%.1f\n",
                ModeName(g_opts.mode), g_opts.threads, g_opts.pool_mib, g_opts.window_kib,
                g_opts.read_only ? "read-only" : "no-access",
                g_opts.section ? "section" : "anonymous", g_opts.seconds);
    std::printf("  stores %" PRIu64 ", faults %" PRIu64 " (%.1f/s), protect calls %" PRIu64
                " (%" PRIu64 " pages)\n",
                stores, faults, faults_f / g_opts.seconds, protect_calls, protect_pages);
    std::printf("  handler total %.1f ms (%.2f us/fault), protect %.1f ms (%.2f us/fault), "
                "other %.1f ms (%.2f us/fault)\n",
                handler_us / 1000.0, handler_us / faults_f, protect_us / 1000.0,
                protect_us / faults_f, (handler_us - protect_us) / 1000.0,
                (handler_us - protect_us) / faults_f);
    if (!samples.empty()) {
        const double med = static_cast<double>(samples[samples.size() / 2]) * us_per_tsc;
        const double p95 = static_cast<double>(samples[(samples.size() * 95) / 100]) * us_per_tsc;
        const double mean = static_cast<double>(sum) * us_per_tsc / static_cast<double>(w_faults);
        std::printf("  end-to-end per fault (the thread is away from its loop): "
                    "min %.2f us, median %.2f us, p95 %.2f us, max %.2f us\n",
                    static_cast<double>(min) * us_per_tsc, med, p95,
                    static_cast<double>(max) * us_per_tsc);
        std::printf("  mean end-to-end %.2f us/fault (%.0f faults/s seen by the writers), "
                    "handler %.2f us, dispatch %.2f us\n",
                    mean, static_cast<double>(w_faults) / g_opts.seconds, handler_us / faults_f,
                    mean - handler_us / faults_f);
    }
    return 0;
}
