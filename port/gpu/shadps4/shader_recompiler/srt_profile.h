// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: what the SRT walker costs in the game (TLG_SRT_PROFILE=1).
//
// The SRT walker is the machine code the recompiler generates to copy the resource table
// of a shader out of the guest's memory into the flattened user data. The GPU command
// thread runs it once per stage of every draw it decodes, to check that the data the
// draw preparation workers prepared still matches the guest's memory, and the workers
// run it too.
//
// The bench in port/tests/srt_bench.cpp measures the walkers of a real session outside
// the game (16.4 cycles per walk, 47.5 entries). This profile measures them inside the
// game, where the guest's memory is where it is.

#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include "common/arch.h"
#include "common/types.h"

namespace Shader::SrtProfile {

inline bool& EnabledFlag() {
    static bool on = [] {
        const char* value = std::getenv("TLG_SRT_PROFILE");
        return value && value[0] == '1';
    }();
    return on;
}

inline bool Enabled() {
    return EnabledFlag();
}

/// Turns the profile on or off (the test does; the game uses the environment).
inline void SetEnabled(bool on) {
    EnabledFlag() = on;
}

/// The clock of the walker: the raw TSC, like the PM4 profile uses (a fenced read would
/// cost more than the walk itself).
inline u64 Now() {
#if defined(ARCH_X86_64)
    u32 low = 0, high = 0;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return (static_cast<u64>(high) << 32) | low;
#else
    return 0;
#endif
}

struct Counters {
    std::atomic<u64> walks{0};   ///< the times the SRT was walked
    std::atomic<u64> entries{0}; ///< the entries the walks copied
    std::atomic<u64> cycles{0};  ///< the cycles the walks took
    std::atomic<u64> skipped{0}; ///< the times a shader had no SRT to walk
};

inline Counters& Get() {
    static Counters counters;
    return counters;
}

/// One walk: `entries` words copied out of the guest's memory, in `cycles` cycles.
inline void Add(u32 entries, u64 cycles) {
    auto& counters = Get();
    counters.walks.fetch_add(1, std::memory_order_relaxed);
    counters.entries.fetch_add(entries, std::memory_order_relaxed);
    counters.cycles.fetch_add(cycles, std::memory_order_relaxed);
}

/// A shader with no SRT: nothing to walk (its resources are in the user data).
inline void AddSkipped() {
    Get().skipped.fetch_add(1, std::memory_order_relaxed);
}

/// Prints the profile every five seconds. Only one thread prints at a time.
inline void Report() {
    auto& counters = Get();
    static std::atomic<u64> last{0};
    const u64 now = Now();
    const u64 previous = last.load(std::memory_order_relaxed);
    if (previous == 0) {
        // The first call only starts the clock.
        last.store(now, std::memory_order_relaxed);
        return;
    }
    if (now - previous < 5'000'000'000ull || now < previous) {
        return;
    }
    u64 expected = previous;
    if (!last.compare_exchange_strong(expected, now, std::memory_order_relaxed)) {
        return;
    }
    const u64 walks = counters.walks.load(std::memory_order_relaxed);
    const u64 entries = counters.entries.load(std::memory_order_relaxed);
    const u64 cycles = counters.cycles.load(std::memory_order_relaxed);
    const u64 skipped = counters.skipped.load(std::memory_order_relaxed);
    counters.walks.store(0, std::memory_order_relaxed);
    counters.entries.store(0, std::memory_order_relaxed);
    counters.cycles.store(0, std::memory_order_relaxed);
    counters.skipped.store(0, std::memory_order_relaxed);
    std::printf(
        "SRT walker: %llu walks, %llu entries, %llu cycles (%.1f per walk, %.2f per entry), "
        "%llu shaders without SRT\n",
        static_cast<unsigned long long>(walks), static_cast<unsigned long long>(entries),
        static_cast<unsigned long long>(cycles), walks ? double(cycles) / double(walks) : 0.0,
        entries ? double(cycles) / double(entries) : 0.0,
        static_cast<unsigned long long>(skipped));
}

} // namespace Shader::SrtProfile
