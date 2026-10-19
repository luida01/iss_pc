// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: how much of the VRAM budget the texture cache may keep, and when the collector frees.
//
// The collector used to compare the *driver's* memory usage with the budget. That number also
// holds the buffer cache's arenas (the guest memory the GPU reads: host memory the driver
// accounts for in the same heap), the staging pool and the driver's own allocations, none of
// which the collector can free: the usage stayed above the critical mark for the whole session
// and the collector evicted the textures the game was drawing with, over and over (the detail
// the player sees missing).
//
// Worse, on this NVIDIA driver the declared total (4757 MiB) is *above* the card itself
// (4096 MiB) while nvidia-smi reports 3848 MB in use: the accounting includes host memory and
// cannot be compared with a VRAM budget. When the total does not fit in the card it is not
// usable, and the collector decides with its own bytes and the budget alone.
//
// This header holds the policy alone (no Vulkan, no cache): it is what the unit test checks.

#pragma once

#include "common/types.h"

#include <algorithm>

namespace VideoCore::GcBudget {

/// The share of the budget the cache keeps at least, in percent: the working set the game needs
/// right now. Below it the collector does not free, whatever the driver says.
inline constexpr u64 DefaultFloorShare = 25;
/// The share of the card's memory above which the driver itself is in trouble, in percent: it
/// starts moving allocations out of the card and into system memory (slow, and the game's data
/// stops fitting). 93% of 4 GiB is 3809 MiB. Only used when the driver's total fits in the card.
inline constexpr u64 DefaultHardShare = 93;
/// The ticks (submissions) the working set decays over: about half a second at 60 FPS.
inline constexpr u64 WorkingSetTicks = 32;

/// What the collector knows when it decides.
struct Inputs {
    u64 budget{};       ///< the bytes the port may use on the GPU (the launcher's VRAM budget)
    u64 cache_bytes{};  ///< the bytes the texture cache holds now (its own images)
    u64 other_bytes{};  ///< the bytes everything else in the port holds (only when the driver's
                        ///< total is reliable: it is the total minus the cache's own bytes)
    u64 driver_bytes{}; ///< the bytes the driver says it uses
    u64 physical{};     ///< the card's device local memory (0: unknown)
    u64 working_set{};  ///< the bytes the game is using right now
    u64 floor_share{DefaultFloorShare};
    u64 hard_share{DefaultHardShare};
};

/// What the collector does with them.
struct Plan {
    u64 target{};        ///< the bytes the cache should hold; free while above it
    u64 floor{};         ///< the bytes it never goes below (the working set)
    u64 critical{};      ///< above this the collector frees faster
    u64 free_per_pass{}; ///< the bytes a single pass may free (smooths the trimming)
    bool hard{};         ///< the driver is over the card: it is already spilling
    bool over_budget{};  ///< the rest of the port alone takes more than the budget
    bool driver_reliable{}; ///< the driver's total fits in the card and can be used
};

inline u64 Percent(u64 value, u64 percent) {
    return (value * percent) / 100;
}

/// The plan for the current state. `budget` is the cap the cache may not pass, the working set
/// the floor it does not go below, and the driver the reason to hurry (only when its number is
/// comparable with the card's).
inline Plan MakePlan(const Inputs& in) {
    Plan plan{};
    const u64 budget = in.budget;

    // The floor: the working set the game needs right now, and at least a share of the budget.
    u64 floor = std::max(in.working_set, Percent(budget, in.floor_share));
    floor = std::min(floor, budget);
    plan.floor = floor;

    // The driver's total is only usable when it fits in the card: on this driver it counts the
    // host memory the buffer cache's arenas use, and it is above the card's own size.
    plan.driver_reliable = in.physical == 0 || in.driver_bytes <= in.physical;

    // The soft target: what is left of the budget once the rest of the port took its share.
    if (plan.driver_reliable) {
        plan.over_budget = in.other_bytes > budget;
        u64 target = budget > in.other_bytes ? budget - in.other_bytes : 0;
        target = std::clamp(target, floor, budget);
        plan.target = target;
    } else {
        // The number is not comparable with the budget: the cache may use the whole budget and
        // the pressure is decided with its own bytes alone.
        plan.over_budget = false;
        plan.target = budget;
    }

    const u64 hard_limit = in.physical != 0 ? Percent(in.physical, in.hard_share) : 0;
    if (plan.driver_reliable && hard_limit != 0 && in.driver_bytes > hard_limit) {
        // The driver is over the card: it moves allocations out to system memory. The cache gives
        // up what it can above its floor; what is left is the rest of the port's (the buffer
        // cache's arenas), which the collector cannot touch.
        plan.hard = true;
        const u64 excess = in.cache_bytes > floor ? in.cache_bytes - floor : 0;
        const u64 give = std::min(in.driver_bytes - hard_limit, excess);
        u64 target = in.cache_bytes > give ? in.cache_bytes - give : floor;
        plan.target = std::max(std::min(target, budget), floor);
    }

    // The cache never aims past 90% of the card: the rest is for the desktop and the driver.
    // A working set that does not fit is trimmed anyway: the card is too small for it.
    if (in.physical != 0) {
        plan.target = std::min(plan.target, Percent(in.physical, 90));
    }

    // Five percent over the target: the collector then frees the older images faster.
    plan.critical = Percent(plan.target, 105);
    // A pass frees at most an eighth of the target: a mass eviction in one frame is a hitch.
    plan.free_per_pass = std::max<u64>(Percent(plan.target, 12), 16_MB);
    return plan;
}

/// The working set estimate: an exponential moving average of the bytes the game touched per
/// tick. `touched` are the bytes touched since the last call, `ticks` the window.
inline u64 UpdateWorkingSet(u64 working_set, u64 touched, u64 ticks = WorkingSetTicks) {
    if (ticks == 0) {
        return working_set + touched;
    }
    return working_set - working_set / ticks + touched;
}

} // namespace VideoCore::GcBudget
