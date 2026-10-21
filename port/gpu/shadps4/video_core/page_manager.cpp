#include "tlg_perf_trace.h"
// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef __linux__
#include <ucontext.h>
#endif
#include <vector>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <boost/container/small_vector.hpp>
#include "tlg_toggles.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/div_ceil.h"
#include "common/error.h"
#include "common/range_lock.h"
#include "common/signal_context.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "core/signals.h"
#include "video_core/page_manager.h"
#include "video_core/page_state.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"

#ifndef _WIN64
#include <sys/mman.h>
#include "common/adaptive_mutex.h"
#else
#include <windows.h>
#endif

#ifdef __linux__
#include <thread>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <unordered_set>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#else
#include "common/spin_lock.h"
#endif

namespace VideoCore {

constexpr size_t PM_PAGE_SIZE = 4_KB;
constexpr size_t PM_PAGE_BITS = 12;

namespace {

// tlg: the protection changes asked for while a fault is being handled are applied together,
// at the end of the handler. The invalidation leaves the faulting page accessible and the
// fault also leaves a window around it accessible: two calls, two address space locks and two
// TLB shootdowns. With the game's 91 threads the shootdown is the expensive part (the bench
// in port/tests/writefault measures 116 us per fault with the two calls and 64 us with the
// merged one), and the guest loses that time on every fault.
//
// The ranges are the pages whose protection changed; the newer request wins over the older one
// on the pages they share.
struct PendingRange {
    u64 first_page = 0;
    u64 last_page = 0; ///< exclusive
    Core::MemoryPermission perms = Core::MemoryPermission::ReadWrite;
};

/// The batch of the thread that is handling the fault (the changes are per thread).
struct ProtectionBatch {
    std::vector<PendingRange> ranges;
    bool active = false;
};
thread_local ProtectionBatch tl_batch;

// tlg: the cost of the write faults, printed every 5 s with TLG_FAULT_COST=1: how long the
// handler takes, how much of it is the protection change, and how many calls were merged.
struct FaultCost {
    std::atomic<u64> faults{0};
    std::atomic<u64> handler_ns{0};
    std::atomic<u64> protect_ns{0};
    std::atomic<u64> protect_calls{0};
    std::atomic<u64> merged{0};
    std::atomic<u64> pages{0};
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
};
FaultCost& Cost() {
    static FaultCost cost;
    return cost;
}
bool CostEnabled() {
    static const bool enabled = [] {
        const char* e = std::getenv("TLG_FAULT_COST");
        return e && e[0] == '1';
    }();
    return enabled;
}
void ReportCost() {
    if (!CostEnabled()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    auto& cost = Cost();
    if (now - cost.window < std::chrono::seconds(5)) {
        return;
    }
    cost.window = now;
    const u64 faults = cost.faults.exchange(0);
    const u64 handler = cost.handler_ns.exchange(0);
    const u64 protect = cost.protect_ns.exchange(0);
    const u64 calls = cost.protect_calls.exchange(0);
    const u64 merged = cost.merged.exchange(0);
    const u64 pages = cost.pages.exchange(0);
    if (!faults) {
        return;
    }
    std::printf("Fault cost (5 s): %llu write faults, handler %.2f us/fault, protect %.2f us/fault "
                "(%llu calls, %llu pages, %llu batches)\n",
                static_cast<unsigned long long>(faults), handler / 1000.0 / faults,
                protect / 1000.0 / faults, static_cast<unsigned long long>(calls),
                static_cast<unsigned long long>(pages), static_cast<unsigned long long>(merged));
    std::fflush(stdout);
}

/// Opens the batch of the thread for the time of a fault.
class BatchScope {
public:
    BatchScope() : previous(tl_batch.active) {
        tl_batch.active = true;
    }
    ~BatchScope() {
        tl_batch.active = previous;
    }
    BatchScope(const BatchScope&) = delete;
    BatchScope& operator=(const BatchScope&) = delete;

private:
    bool previous;
};

} // namespace

struct PageManager::Impl {
    using PageState = VideoCore::PageState;

    static constexpr size_t ADDRESS_BITS = 40;
    static constexpr size_t NUM_ADDRESS_PAGES = 1ULL << (40 - PM_PAGE_BITS);
    static constexpr size_t NUM_ADDRESS_LOCKS = NUM_ADDRESS_PAGES / PAGES_PER_LOCK;
    inline static Vulkan::Rasterizer* rasterizer;
    // tlg: the tracker is a singleton: the fault handler needs it to apply the batch of the
    // protection changes of the fault it is handling.
    inline static Impl* instance;

    Impl() {
        instance = this;
#if defined(__linux__) && defined(MADV_FREE)
        // tlg: cached_pages is one zero counter pair per 4 KiB of the 40-bit address space (512
        // MiB) and is value-initialized, which commits all of it. Hand the untouched part back to
        // the kernel: MADV_FREE only drops the pages under memory pressure, and a page dropped
        // that way faults back in zeroed, which is exactly what the counters hold (they are all
        // zero and any page written afterwards is never dropped). The locks cannot be freed this
        // way (their mutexes are initialized to non-zero bytes).
        const VAddr base = reinterpret_cast<VAddr>(cached_pages.data());
        const VAddr first = Common::AlignUp(base, PM_PAGE_SIZE);
        ::madvise(reinterpret_cast<void*>(first), sizeof(cached_pages) - (first - base), MADV_FREE);
#endif
    }
    virtual ~Impl() = default;

    virtual void OnMap(VAddr address, size_t size) {
        // No-op
    }

    virtual void OnUnmap(VAddr address, size_t size) {
        // No-op
    }

    virtual void Protect(VAddr address, size_t size, Core::MemoryPermission perms) = 0;

    // tlg: the protection change of a range of pages. While a fault is being handled the
    // changes are batched (see ProtectionBatch above) and applied together at the end of the
    // handler: the invalidation and the window the fault leaves loose ask for the same thing
    // (the pages accessible) and two calls cost two address space locks and two TLB
    // shootdowns, which with the game's 91 threads is most of the cost of the fault.
    void ApplyProtect(u64 first_page, u64 last_page, Core::MemoryPermission perms) {
        if (first_page >= last_page) {
            return;
        }
        if (!tl_batch.active) {
            ApplyRange(first_page, last_page, perms);
            return;
        }
        // The newer request wins over the older one on the pages they share.
        std::vector<PendingRange> kept;
        kept.reserve(tl_batch.ranges.size() + 2);
        for (const auto& old : tl_batch.ranges) {
            if (old.last_page <= first_page || old.first_page >= last_page) {
                kept.push_back(old);
                continue;
            }
            if (old.first_page < first_page) {
                kept.push_back({old.first_page, first_page, old.perms});
            }
            if (old.last_page > last_page) {
                kept.push_back({last_page, old.last_page, old.perms});
            }
        }
        kept.push_back({first_page, last_page, perms});
        tl_batch.ranges.swap(kept);
    }

    // tlg: applies the batch of the fault: one call per range of pages that need the same
    // protection. The protection applied is the one the state of the pages asks for *now*, not
    // the one that was asked for when the fault started: the upload may have re-protected the
    // pages while the fault was being handled, and applying the older request would leave them
    // accessible (the next write to them would not be seen and the GPU would read old data).
    void FlushProtectionBatch() {
        if (tl_batch.ranges.empty()) {
            return;
        }
        std::vector<PendingRange> ranges;
        ranges.swap(tl_batch.ranges);
        std::sort(ranges.begin(), ranges.end(),
                  [](const auto& a, const auto& b) { return a.first_page < b.first_page; });
        // The ranges do not overlap; the ones that touch and want the same protection are one.
        std::vector<PendingRange> merged;
        for (const auto& r : ranges) {
            if (!merged.empty() && merged.back().last_page == r.first_page &&
                merged.back().perms == r.perms) {
                merged.back().last_page = r.last_page;
            } else {
                merged.push_back(r);
            }
        }
        Cost().merged.fetch_add(1, std::memory_order_relaxed);
        for (const auto& r : merged) {
            const auto lock_begin = locks.begin() + r.first_page / PAGES_PER_LOCK;
            const auto lock_end = locks.begin() + Common::DivCeil(r.last_page, PAGES_PER_LOCK);
            Common::RangeLockGuard lk(lock_begin, lock_end);
            // The protection of every page, as its state asks now.
            u64 run_start = r.first_page;
            auto run_perms = cached_pages[run_start].Perms();
            for (u64 page = r.first_page + 1; page < r.last_page; ++page) {
                const auto perms = cached_pages[page].Perms();
                if (perms != run_perms) {
                    ApplyRange(run_start, page, run_perms);
                    run_start = page;
                    run_perms = perms;
                }
            }
            ApplyRange(run_start, r.last_page, run_perms);
        }
    }

    // tlg: one protection change, with the time it takes (TLG_FAULT_COST=1).
    void ApplyRange(u64 first_page, u64 last_page, Core::MemoryPermission perms) {
        const auto start = std::chrono::steady_clock::now();
        Protect(first_page << PM_PAGE_BITS, (last_page - first_page) << PM_PAGE_BITS, perms);
        if (!CostEnabled()) {
            return;
        }
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        Cost().protect_ns.fetch_add(ns, std::memory_order_relaxed);
        Cost().protect_calls.fetch_add(1, std::memory_order_relaxed);
        Cost().pages.fetch_add(last_page - first_page, std::memory_order_relaxed);
    }

    // tlg: TLG_FAULT_PROFILE=1 prints, every 5 s, the guest instructions whose faults on GPU
    // tracked pages cost the most time (which game code waits on the GPU, and for what).
    struct FaultSite { u64 count = 0, ns = 0; VAddr last = 0; u64 caller = 0; bool write = false; };
    static void ProfileFault(void* context, VAddr addr, bool write, u64 ns) {
#ifdef __linux__
        static const bool enabled = [] { const char* e = std::getenv("TLG_FAULT_PROFILE"); return e && *e == '1'; }();
        if (!enabled) {
            return;
        }
        static std::mutex mutex;
        static std::unordered_map<u64, FaultSite> sites;
        static auto window = std::chrono::steady_clock::now();
        const auto* uc = static_cast<ucontext_t*>(context);
        const u64 rip = static_cast<u64>(uc->uc_mcontext.gregs[REG_RIP]);
        // The return address at the top of the stack (right for leaf routines such as memcpy).
        const u64 caller = *reinterpret_cast<const u64*>(uc->uc_mcontext.gregs[REG_RSP]);
        std::scoped_lock lock{mutex};
        auto& site = sites[rip];
        ++site.count;
        site.ns += ns;
        site.last = addr;
        site.write = write;
        site.caller = caller;
        const auto now = std::chrono::steady_clock::now();
        if (now - window < std::chrono::seconds(5)) {
            return;
        }
        window = now;
        std::vector<std::pair<u64, FaultSite>> top(sites.begin(), sites.end());
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
        std::printf("Fault profile (5 s):");
        for (size_t i = 0; i < top.size() && i < 8; ++i) {
            const u64 at = top[i].first;
            const u64 from = top[i].second.caller;
            std::printf(" [%s %#llx<-%#llx x%llu %.1f ms @%#llx]", top[i].second.write ? "W" : "R",
                        static_cast<unsigned long long>(at >= 0x800000000ull && at < 0x810000000ull ? at - 0x800000000ull : at),
                        static_cast<unsigned long long>(from >= 0x800000000ull && from < 0x810000000ull ? from - 0x800000000ull : from),
                        static_cast<unsigned long long>(top[i].second.count), top[i].second.ns / 1e6,
                        static_cast<unsigned long long>(top[i].second.last));
        }
        std::printf("\n");
        std::fflush(stdout);
        sites.clear();
#else
        (void)context; (void)addr; (void)write; (void)ns;
#endif
    }

    static bool GuestFaultSignalHandler(void* context, void* fault_address) {
        const auto start = std::chrono::steady_clock::now();
        const bool write = Common::IsWriteError(context);
        const auto rip = TlgPerf::Enabled() ? reinterpret_cast<std::uintptr_t>(Common::GetRip(context)) : 0;
        const auto site = rip >= 0x800000000ULL && rip < 0x810000000ULL ? rip - 0x800000000ULL : 0;
        TlgPerf::Span perf_fault{TlgPerf::Kind::FaultBegin, TlgPerf::Kind::FaultEnd, site, write};
        const auto addr = reinterpret_cast<VAddr>(fault_address);
        if (perf_fault.Id()) {
            TlgPerf::Event e;e.kind=TlgPerf::Kind::FaultContext;e.operation=perf_fault.Id();
            e.object=TlgPerf::Hash(addr);e.a=site;e.b=write;
            TlgPerf::Emit(e);
        }
        // tlg: the protection changes of this fault are applied together, at the end (the
        // invalidation and the window the fault leaves loose ask for the same thing).
        bool handled = false;
        {
            BatchScope batch;
            handled = HandleGuestFault(context, addr);
        }
        if (instance) {
            instance->FlushProtectionBatch();
        }
        if (CostEnabled()) {
            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();
            Cost().handler_ns.fetch_add(ns, std::memory_order_relaxed);
            Cost().faults.fetch_add(1, std::memory_order_relaxed);
            ReportCost();
        }
        ProfileFault(context, addr, write,
                     static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now() - start).count()));
        return handled;
    }

    static bool HandleGuestFault(void* context, VAddr addr) {
        // tlg: the draw recording thread handles its faults inline too (vk_draw_pipe.h).
        const auto is_gpu_thread = rasterizer->IsGpuSideThread();
        if (is_gpu_thread) {
            TlgStats::gpu_signal_faults.fetch_add(1, std::memory_order_relaxed);
        }
        if (Common::IsWriteError(context)) {
            TlgStats::Timer timer{TlgStats::t_write_faults};
            return rasterizer->OnWriteFault(addr, is_gpu_thread);
        } else {
            TlgStats::read_faults.fetch_add(1, std::memory_order_relaxed);
            TlgStats::Timer timer{TlgStats::t_read_faults};
            return rasterizer->ReadMemory(addr, 8, is_gpu_thread);
        }
        return false;
    }

    template <bool track, bool is_read>
    void UpdatePageWatchers(VAddr addr, u64 size) {
        RENDERER_TRACE;

        size_t page = addr >> PM_PAGE_BITS;
        const u64 page_end = Common::DivCeil(addr + size, PM_PAGE_SIZE);

        // Acquire locks for the range of pages
        const auto lock_start = locks.begin() + (page / PAGES_PER_LOCK);
        const auto lock_end = locks.begin() + Common::DivCeil(page_end, PAGES_PER_LOCK);
        Common::RangeLockGuard lk(lock_start, lock_end);

        auto perms = cached_pages[page].Perms();
        u64 range_begin = 0;
        u64 range_bytes = 0;
        u64 potential_range_bytes = 0;

        const auto release_pending = [&] {
            if (range_bytes > 0) {
                RENDERER_TRACE;
                // Perform pending (un)protect action
                ApplyProtect(range_begin, range_begin + (range_bytes >> PM_PAGE_BITS), perms);
                range_bytes = 0;
                potential_range_bytes = 0;
            }
        };

        // Iterate requested pages
        const u64 aligned_addr = page << PM_PAGE_BITS;
        const u64 aligned_end = page_end << PM_PAGE_BITS;
        if (!rasterizer->IsMapped(aligned_addr, aligned_end - aligned_addr)) {
            LOG_WARNING(Render,
                        "Tracking memory region {:#x} - {:#x} which is not fully GPU mapped.",
                        aligned_addr, aligned_end);
        }

        for (; page != page_end; ++page) {
            PageState& state = cached_pages[page];

            // Apply the change to the page state
            const u8 new_count = state.AddDelta<track ? 1 : -1, is_read>();

            if (auto new_perms = state.Perms(); new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_bytes != 0) {
                // If the protection did not change, extend the potential range
                potential_range_bytes += PM_PAGE_SIZE;
            }

            // Only start a new range if the page must be (un)protected
            if ((new_count == 0 && !track) || (new_count == 1 && track)) {
                if (range_bytes == 0) {
                    // Start a new potential range
                    range_begin = page;
                    potential_range_bytes = PM_PAGE_SIZE;
                }
                // Extend current range up to potential range
                range_bytes = potential_range_bytes;
            }
        }

        // Add pending (un)protect action
        release_pending();
    }

    template <bool track, bool is_read>
    void UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) {
        RENDERER_TRACE;
        auto start_range = mask.FirstRange();
        auto end_range = mask.LastRange();

        if (start_range.second == end_range.second) {
            // if all pages are contiguous, use the regular UpdatePageWatchers
            const VAddr start_addr = base_addr + (start_range.first << PM_PAGE_BITS);
            const u64 size = (start_range.second - start_range.first) << PM_PAGE_BITS;
            return UpdatePageWatchers<track, is_read>(start_addr, size);
        }

        size_t base_page = (base_addr >> PM_PAGE_BITS);
        ASSERT(base_page % PAGES_PER_LOCK == 0);
        std::scoped_lock lk(locks[base_page / PAGES_PER_LOCK]);
        auto perms = cached_pages[base_page + start_range.first].Perms();
        u64 range_begin = 0;
        u64 range_bytes = 0;
        u64 potential_range_bytes = 0;

        const auto release_pending = [&] {
            if (range_bytes > 0) {
                RENDERER_TRACE;
                // Perform pending (un)protect action
                ApplyProtect(range_begin, range_begin + (range_bytes >> PM_PAGE_BITS), perms);
                range_bytes = 0;
                potential_range_bytes = 0;
            }
        };

        // Iterate pages
        for (size_t page = start_range.first; page < end_range.second; ++page) {
            PageState& state = cached_pages[base_page + page];
            const bool update = mask.Get(page);

            // Apply the change to the page state
            const u8 new_count =
                update ? state.AddDelta<track ? 1 : -1, is_read>() : state.AddDelta<0, is_read>();

            if (auto new_perms = state.Perms(); new_perms != perms) [[unlikely]] {
                // If the protection changed add pending (un)protect action
                release_pending();
                perms = new_perms;
            } else if (range_bytes != 0) {
                // If the protection did not change, extend the potential range
                potential_range_bytes += PM_PAGE_SIZE;
            }

            // If the page is not being updated, skip it
            if (!update) {
                continue;
            }

            // If the page must be (un)protected
            if ((new_count == 0 && !track) || (new_count == 1 && track)) {
                if (range_bytes == 0) {
                    // Start a new potential range
                    range_begin = base_page + page;
                    potential_range_bytes = PM_PAGE_SIZE;
                }
                // Extend current rango up to potential range
                range_bytes = potential_range_bytes;
            }
        }

        // Add pending (un)protect action
        release_pending();
    }

    std::array<PageState, NUM_ADDRESS_PAGES> cached_pages{};
#ifdef PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
    using LockType = Common::AdaptiveMutex;
#else
    using LockType = Common::SpinLock;
#endif
    std::array<LockType, NUM_ADDRESS_LOCKS> locks{};
};

#ifdef __linux__
// tlg: write tracking with userfaultfd write-protection instead of mprotect. mprotect takes
// the address space lock for writing and splits mappings; while guest threads fault on
// per-frame buffers and the GPU thread re-protects uploaded pages, every page fault in the
// process (copy threads included) waits for it. UFFDIO_WRITEPROTECT changes page table bits
// under the lock for reading. Read protection (readbacks) still uses mprotect: userfaultfd
// write-protection cannot deny reads. TLG_UFFD=1.
struct UffdImpl : public PageManager::Impl {
private:
    std::jthread ufd_thread;
    int uffd;
    std::mutex read_revoked_mutex;
    std::unordered_set<u64> read_revoked_pages; ///< 4 KiB pages denied reads with mprotect
    std::atomic<size_t> num_read_revoked{0};

public:
    UffdImpl(Vulkan::Rasterizer* rasterizer_) : Impl() {
        rasterizer = rasterizer_;
        uffd = syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
        if (uffd == -1) {
            LOG_ERROR(Common_Memory,
                      "userfaultfd syscall failed: {}, falling back to signal implementation",
                      Common::GetLastErrorMsg());
            throw std::runtime_error("userfaultfd");
        }

        // Guest memory is a shared memfd mapping: write-protection there needs the shmem
        // feature, and unpopulated pages must be protectable too.
        uffdio_api api{};
        api.api = UFFD_API;
        api.features =
            UFFD_FEATURE_THREAD_ID | UFFD_FEATURE_WP_HUGETLBFS_SHMEM | UFFD_FEATURE_WP_UNPOPULATED;
        if (ioctl(uffd, UFFDIO_API, &api) != 0) {
            LOG_ERROR(Common_Memory,
                      "uffdio_api call failed: {}, falling back to signal implementation",
                      Common::GetLastErrorMsg());
            close(uffd);
            throw std::runtime_error("uffdio_api");
        }

        // Read faults (readbacks) still arrive as signals.
        Core::Signals::Instance()->RegisterAccessViolationHandler(
            GuestFaultSignalHandler, std::numeric_limits<u32>::min());

        ufd_thread = std::jthread([this](std::stop_token token) { UffdHandler(token); });
        std::printf("GPU: memory tracking with userfaultfd write-protection\n");
    }

    ~UffdImpl() = default;

    void OnMap(VAddr address, size_t size) override {
        uffdio_register reg{};
        reg.range.start = address;
        reg.range.len = size;
        reg.mode = UFFDIO_REGISTER_MODE_WP;
        if (ioctl(uffd, UFFDIO_REGISTER, &reg) == -1) {
            LOG_ERROR(Common_Memory, "Uffdio register {:#x}+{:#x} failed: {}", address, size,
                      Common::GetLastErrorMsg());
        }
    }

    void OnUnmap(VAddr address, size_t size) override {
        uffdio_range range{};
        range.start = address;
        range.len = size;
        ioctl(uffd, UFFDIO_UNREGISTER, &range);
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) override {
        auto& address_space = Core::Memory::Instance()->GetAddressSpace();
        const bool allow_read = True(perms & Core::MemoryPermission::Read);
        const bool allow_write = True(perms & Core::MemoryPermission::Write);
        const u64 first = address >> 12, last = (address + size + 4095) >> 12;
        if (!allow_read) {
            // Readbacks: deny all access with mprotect.
            address_space.Protect(address, size, Core::MemoryPermission::None);
            std::scoped_lock lk{read_revoked_mutex};
            for (u64 page = first; page < last; ++page) {
                read_revoked_pages.insert(page);
            }
            num_read_revoked.store(read_revoked_pages.size(), std::memory_order_release);
            return;
        }
        if (num_read_revoked.load(std::memory_order_acquire) != 0) {
            std::scoped_lock lk{read_revoked_mutex};
            bool restore = false;
            for (u64 page = first; page < last; ++page) {
                restore |= read_revoked_pages.erase(page) != 0;
            }
            num_read_revoked.store(read_revoked_pages.size(), std::memory_order_release);
            if (restore) {
                address_space.Protect(address, size, Core::MemoryPermission::ReadWrite);
            }
        }
        uffdio_writeprotect wp{};
        wp.range.start = address;
        wp.range.len = size;
        wp.mode = allow_write ? UFFDIO_WRITEPROTECT_MODE_DONTWAKE : UFFDIO_WRITEPROTECT_MODE_WP;
        if (ioctl(uffd, UFFDIO_WRITEPROTECT, &wp) == -1) {
            LOG_ERROR(Common_Memory, "Uffdio writeprotect {:#x}+{:#x} failed: {}", address, size,
                      Common::GetLastErrorMsg());
        }
    }

    void UffdHandler(std::stop_token token) {
        Common::SetCurrentThreadName("tlg:Uffd");
        while (!token.stop_requested()) {
            pollfd pollfd{};
            pollfd.fd = uffd;
            pollfd.events = POLLIN;
            // Short timeout so the stop request is seen.
            const int pollres = poll(&pollfd, 1, 100);
            if (pollres <= 0 || !(pollfd.revents & POLLIN)) {
                continue;
            }
            uffd_msg msg;
            const int readret = read(uffd, &msg, sizeof(msg));
            if (readret != sizeof(msg)) {
                continue;
            }
            if (msg.event != UFFD_EVENT_PAGEFAULT) {
                continue;
            }
            const VAddr addr = msg.arg.pagefault.address;
            const auto ptid = msg.arg.pagefault.feat.ptid;
            {
                TlgStats::Timer timer{TlgStats::t_write_faults};
                // tlg: the protection changes of this fault are applied together, at the end.
                BatchScope batch;
                rasterizer->OnWriteFault(addr, rasterizer->IsGpuSideThreadId(ptid));
            }
            if (instance) {
                instance->FlushProtectionBatch();
            }
            // Protect() clears with DONTWAKE (it may run for pages nobody waits on).
            uffdio_range wake{};
            wake.start = addr & ~u64(PM_PAGE_SIZE - 1);
            wake.len = PM_PAGE_SIZE;
            ioctl(uffd, UFFDIO_WAKE, &wake);
        }
    }
};
#endif // __linux__

struct SignalImpl : public PageManager::Impl {
    SignalImpl(Vulkan::Rasterizer* rasterizer_) : Impl() {
        rasterizer = rasterizer_;

        // Should be called first.
        constexpr auto priority = std::numeric_limits<u32>::min();
        Core::Signals::Instance()->RegisterAccessViolationHandler(GuestFaultSignalHandler,
                                                                  priority);
    }

    void Protect(VAddr address, size_t size, Core::MemoryPermission perms) override {
        RENDERER_TRACE;
        auto* memory = Core::Memory::Instance();
        auto& impl = memory->GetAddressSpace();
        ASSERT_MSG(perms != Core::MemoryPermission::Write,
                   "Attempted to protect region as write-only which is not a valid permission");
        impl.Protect(address, size, perms);
    }

};

PageManager::PageManager(Vulkan::Rasterizer* rasterizer_) {
#ifdef __linux__
    if (std::getenv("TLG_UFFD") && std::getenv("TLG_UFFD")[0] == '1') {
        try {
            impl = std::make_unique<UffdImpl>(rasterizer_);
            LOG_INFO(Config, "Memory tracking method: userfaultfd");
            return;
        } catch (const std::runtime_error& e) {
            // if uffd is unsupported, falls back to SignalImpl
        }
    }
    LOG_INFO(Config, "Memory tracking method: signals");
#endif
    impl = std::make_unique<SignalImpl>(rasterizer_);
}

PageManager::~PageManager() = default;

void PageManager::OnGpuMap(VAddr address, size_t size) {
    impl->OnMap(address, size);
}

void PageManager::OnGpuUnmap(VAddr address, size_t size) {
    impl->OnUnmap(address, size);
}

template <bool track, bool is_read>
void PageManager::UpdatePageWatchers(VAddr addr, u64 size) const {
    impl->UpdatePageWatchers<track, is_read>(addr, size);
}

template <bool track, bool is_read>
void PageManager::UpdatePageWatchersForRegion(VAddr base_addr, RegionBits& mask) const {
    impl->UpdatePageWatchersForRegion<track, is_read>(base_addr, mask);
}

template void PageManager::UpdatePageWatchers<true>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchers<false>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchers<true, true>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchers<false, true>(VAddr addr, u64 size) const;
template void PageManager::UpdatePageWatchersForRegion<true, true>(VAddr base_addr,
                                                                   RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<true, false>(VAddr base_addr,
                                                                    RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<false, true>(VAddr base_addr,
                                                                    RegionBits& mask) const;
template void PageManager::UpdatePageWatchersForRegion<false, false>(VAddr base_addr,
                                                                     RegionBits& mask) const;

} // namespace VideoCore
