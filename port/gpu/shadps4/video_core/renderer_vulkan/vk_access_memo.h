// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: the memo of the barrier tracker's queries (vk_runtime.cpp).
//
// Every draw that binds a buffer asks the tracker whether that range has a pending access
// (Runtime::IsBufferAccessed). The tracker is a tree of scattered nodes, so the walk is a
// chain of cache misses, and the same range is asked again for every draw that binds it.
//
// The memo answers the repeated question. Only the "yes" answers are kept: the ranges of
// the tracker only grow (an insert merges the ranges it overlaps) until it is cleared, so a
// "yes" stays a "yes"; a "no" can become a "yes" with the next insert.

#pragma once

#include <array>
#include <cstdint>

#include "common/types.h"

namespace Vulkan {

class AccessQueryMemo {
public:
    /// The flags of a memoized answer: bit 0, a write access is pending; bit 1, a read one.
    static constexpr u32 WriteFlag = 1u;
    static constexpr u32 ReadFlag = 2u;

    /// A new epoch invalidates the answers (the tracker was cleared).
    void NewEpoch() {
        ++epoch;
    }

    /// The answer for the range, if it is known: 0 when it is not (the caller asks the
    /// tracker), otherwise the flags of the pending accesses.
    [[nodiscard]] u32 Find(u64 resource, u64 start, u64 end) const {
        const auto& entry = entries[Index(resource, start, end)];
        if (entry.epoch != epoch || entry.resource != resource || entry.start != start ||
            entry.end != end) {
            return 0;
        }
        return entry.flags;
    }

    /// Remembers the accesses that are pending for the range (only the "yes" ones).
    void Remember(u64 resource, u64 start, u64 end, u32 flags) {
        if (flags == 0) {
            return;
        }
        auto& entry = entries[Index(resource, start, end)];
        entry = {resource, start, end, epoch, flags};
    }

private:
    struct Entry {
        u64 resource{};
        u64 start{};
        u64 end{};
        u32 epoch{};
        u32 flags{};
    };
    static constexpr size_t Size = 512;

    static size_t Index(u64 resource, u64 start, u64 end) {
        u64 hash = resource ^ start * 0x9E3779B97F4A7C15ull ^ end << 13;
        hash = (hash ^ hash >> 31) * 0xFF51AFD7ED558CCDull;
        return (hash ^ hash >> 29) % Size;
    }

    std::array<Entry, Size> entries{};
    u32 epoch{1};
};

} // namespace Vulkan
