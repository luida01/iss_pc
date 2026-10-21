// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: TLG_DRAW_CENSUS=1 — what the game draws and dispatches, per site.
//
// The port's counters say how many draws and dispatches a frame has ("1469 draws, 353
// dispatches"), but not what they draw. When an effect is missing (the particles), that
// leaves three possibilities, and they need different fixes:
//   * the compute that fills the particle list never runs, or runs with the wrong
//     dimensions (the dispatch census),
//   * it runs but produces nothing, so the indirect draw that consumes the list has an
//     instance count of 0 (the indirect-arguments census),
//   * it produces the right count, and the draw is there, but nothing shows (blend,
//     depth, the texture): the draw census then only says "the draw was there".
//
// This census answers the first two: the dispatches by their dimensions and the indirect
// draws by the arguments the CPU sees (the count the game computed on the GPU, one frame
// old, which is enough to tell "0 instances" from "N instances").
//
// The arguments are the CPU's view of the guest memory. A slot the GPU wrote and the game
// never read back is stale (often zero) there, so the numbers are to be read as a group:
// if the other sites have instances and one site has none, that one really draws nothing;
// if every site has none, the CPU cannot see the GPU's values and the renderer (not this
// agent's scope) has to say it.
//
// Header-only and free of Vulkan and guest memory: the caller reads the arguments and
// hands them over, so a test can check the accounting alone.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace AmdGpu {

class DrawCensus {
public:
    /// A direct draw with this many instances (PM4_TYPE3_DRAW_NUM_INSTANCES).
    void NoteDirectDraw(std::uint32_t instances) {
        std::scoped_lock lock{mutex};
        ++direct_draws;
        ++direct_instances[instances < kDirectBuckets ? instances : kDirectBuckets];
    }

    /// An indirect draw whose arguments live at `site` (the base address plus the packet's
    /// data offset). `read` is called at most once per site and window: it returns the
    /// (count, instances) pair the CPU sees.
    template <typename Read>
    void NoteIndirectSite(std::uint64_t site, Read&& read) {
        std::scoped_lock lock{mutex};
        auto& entry = indirect[site];
        entry.site = site;
        ++entry.draws;
        if (entry.read) {
            return;
        }
        entry.read = true;
        const auto args = read();
        entry.count = args.first;
        entry.instances = args.second;
        ++indirect_with_args;
        if (args.second == 0) {
            ++indirect_zero_instances;
        }
    }

    /// A compute dispatch of this many workgroups.
    void NoteDispatch(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        std::scoped_lock lock{mutex};
        ++dispatches;
        ++dispatch_groups[x * 1000000u + y * 1000u + z];
    }

    /// The number of indirect draws seen since the last Reset.
    std::uint64_t IndirectDraws() const {
        std::scoped_lock lock{mutex};
        std::uint64_t total = 0;
        for (const auto& [site, entry] : indirect) {
            total += entry.draws;
        }
        return total;
    }

    void Reset() {
        std::scoped_lock lock{mutex};
        direct_draws = 0;
        direct_instances.fill(0);
        indirect.clear();
        indirect_with_args = 0;
        indirect_zero_instances = 0;
        dispatches = 0;
        dispatch_groups.clear();
    }

    /// One line per indirect site and the top dispatches, so a missing effect can be
    /// traced to the draw that should have carried it.
    void Report() {
        std::scoped_lock lock{mutex};
        std::uint64_t indirect_total = 0;
        for (const auto& [site, entry] : indirect) {
            indirect_total += entry.draws;
        }
        std::printf("Draw census: %llu direct draws, %llu indirect draws (%llu sites, %llu with "
                    "0 instances), %llu dispatches\n",
                    static_cast<unsigned long long>(direct_draws),
                    static_cast<unsigned long long>(indirect_total), indirect.size(),
                    static_cast<unsigned long long>(indirect_zero_instances),
                    static_cast<unsigned long long>(dispatches));
        std::printf("  direct draws by instance count:");
        for (std::size_t i = 0; i < kDirectBuckets; ++i) {
            if (direct_instances[i]) {
                std::printf(" %zu:%llu", i, static_cast<unsigned long long>(direct_instances[i]));
            }
        }
        if (direct_instances[kDirectBuckets]) {
            std::printf(" %zu+:%llu", kDirectBuckets,
                        static_cast<unsigned long long>(direct_instances[kDirectBuckets]));
        }
        std::printf("\n");
        // The sites with the most draws first: the particle draws are the ones the game
        // repeats every frame.
        std::vector<std::pair<std::uint64_t, const IndirectEntry*>> sites;
        sites.reserve(indirect.size());
        for (const auto& [site, entry] : indirect) {
            sites.emplace_back(entry.draws, &entry);
        }
        std::sort(sites.begin(), sites.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        const std::size_t shown = std::min<std::size_t>(sites.size(), 12);
        for (std::size_t i = 0; i < shown; ++i) {
            const auto* entry = sites[i].second;
            std::printf("  indirect site %p: %llu draws, count %u, instances %u%s\n",
                        reinterpret_cast<const void*>(entry->site),
                        static_cast<unsigned long long>(entry->draws), entry->count,
                        entry->instances, entry->instances ? "" : "  <-- NOTHING DRAWN");
        }
        if (sites.size() > shown) {
            std::printf("  ... and %zu more indirect sites\n", sites.size() - shown);
        }
        // The dispatches by their dimensions: the particle update is a dispatch of many
        // workgroups; a dispatch of zero groups is a compute that does nothing.
        std::vector<std::pair<std::uint64_t, std::uint32_t>> groups;
        groups.reserve(dispatch_groups.size());
        for (const auto& [key, count] : dispatch_groups) {
            groups.emplace_back(count, key);
        }
        std::sort(groups.begin(), groups.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        std::printf("  dispatches by size:");
        for (std::size_t i = 0; i < std::min<std::size_t>(groups.size(), 10); ++i) {
            const std::uint32_t key = groups[i].second;
            std::printf(" (%u,%u,%u)x%llu", key / 1000000u, (key / 1000u) % 1000u, key % 1000u,
                        static_cast<unsigned long long>(groups[i].first));
        }
        std::printf("\n");
    }

private:
    struct IndirectEntry {
        std::uint64_t site{};
        std::uint64_t draws{};
        std::uint32_t count{};
        std::uint32_t instances{};
        bool read{};
    };

    static constexpr std::size_t kDirectBuckets = 8;

    /// tlg: the PM4 decode of the graphics queue and of the compute queue run on different
    /// threads, so the census is locked.
    mutable std::mutex mutex;
    std::uint64_t direct_draws{};
    std::array<std::uint64_t, kDirectBuckets + 1> direct_instances{};
    std::map<std::uint64_t, IndirectEntry> indirect;
    std::uint64_t indirect_with_args{};
    std::uint64_t indirect_zero_instances{};
    std::uint64_t dispatches{};
    std::map<std::uint32_t, std::uint64_t> dispatch_groups;
};

} // namespace AmdGpu
