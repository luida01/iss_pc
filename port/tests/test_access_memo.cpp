// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: the memo of the barrier tracker's queries (vk_access_memo.h) and the cost of the walk
// it avoids (vk_barrier_tracker.h). No game and no Vulkan device: the tracker is a tree of
// ranges and the memo is a table.
//
// The renderer asks the tracker, for every buffer of every draw, whether the range has a
// pending access. The walk of the tree is a chain of cache misses (the nodes are scattered),
// and the same range is asked again and again. The memo answers the repeated question.
#include <cassert>
#include <chrono>
#include <cstdio>
#include <vector>

#include "video_core/renderer_vulkan/vk_access_memo.h"
#include "video_core/renderer_vulkan/vk_barrier_tracker.h"

using Vulkan::Access;
using Vulkan::AccessQueryMemo;
using Vulkan::AddressRange;
using Vulkan::BarrierTracker;

namespace {

/// The ranges of a frame: the vertex and index buffers of a few thousand draws, plus the
/// constant buffers, each a different range of a few buffers.
std::vector<AddressRange> FrameRanges() {
    std::vector<AddressRange> ranges;
    for (u64 buffer = 1; buffer <= 24; ++buffer) {
        for (u64 slot = 0; slot < 16; ++slot) {
            ranges.push_back({
                .resource = buffer * 0x1000,
                .range_start = slot * 4096,
                .range_end = slot * 4096 + 2047,
            });
        }
    }
    return ranges;
}

void TestMemoAnswers() {
    AccessQueryMemo memo;
    const auto range = AddressRange{.resource = 7, .range_start = 0, .range_end = 4095};
    // Nothing known yet.
    assert(memo.Find(range.resource, range.range_start, range.range_end) == 0);
    // A "yes" is remembered.
    memo.Remember(range.resource, range.range_start, range.range_end,
                  AccessQueryMemo::WriteFlag);
    assert((memo.Find(range.resource, range.range_start, range.range_end) &
            AccessQueryMemo::WriteFlag) != 0);
    // Another range is not answered with it.
    assert(memo.Find(range.resource, range.range_start + 4096, range.range_end + 4096) == 0);
    // A new epoch (the tracker was cleared) forgets it.
    memo.NewEpoch();
    assert(memo.Find(range.resource, range.range_start, range.range_end) == 0);
    // A "no" is not remembered (it can become a "yes" with the next insert).
    memo.Remember(range.resource, range.range_start, range.range_end, 0);
    assert(memo.Find(range.resource, range.range_start, range.range_end) == 0);
}

double Benchmark() {
    constexpr u32 Iterations = 2'000'000;
    const auto ranges = FrameRanges();

    // The tracker with the ranges of the frame in it.
    BarrierTracker tracker;
    for (const auto& range : ranges) {
        tracker.InsertRange(range, Access::Write);
    }

    // Before: the walk of the tree, every time.
    volatile u64 sink = 0;
    const auto walk_start = std::chrono::steady_clock::now();
    for (u32 i = 0; i < Iterations; ++i) {
        const auto& range = ranges[i % ranges.size()];
        sink ^= tracker.FindRange(range, Access::Write);
    }
    const auto walk_end = std::chrono::steady_clock::now();
    const double walk_ns =
        std::chrono::duration<double, std::nano>(walk_end - walk_start).count() / Iterations;

    // After: the memo answers the repeated question.
    AccessQueryMemo memo;
    for (const auto& range : ranges) {
        memo.Remember(range.resource, range.range_start, range.range_end,
                      AccessQueryMemo::WriteFlag);
    }
    const auto memo_start = std::chrono::steady_clock::now();
    for (u32 i = 0; i < Iterations; ++i) {
        const auto& range = ranges[i % ranges.size()];
        const u32 flags = memo.Find(range.resource, range.range_start, range.range_end);
        sink ^= flags;
    }
    const auto memo_end = std::chrono::steady_clock::now();
    const double memo_ns =
        std::chrono::duration<double, std::nano>(memo_end - memo_start).count() / Iterations;

    std::fprintf(stderr,
                 "Barrier query: %.1f ns with the walk, %.1f ns with the memo (%.0f%% less)\n",
                 walk_ns, memo_ns, walk_ns > 0 ? 100.0 * (walk_ns - memo_ns) / walk_ns : 0.0);
    std::fflush(stderr);
    assert(memo_ns <= walk_ns);
    return memo_ns;
}

} // namespace

int main() {
    TestMemoAnswers();
    Benchmark();
    std::printf("access memo test: ok\n");
    return 0;
}
