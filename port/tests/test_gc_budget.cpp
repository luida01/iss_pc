// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: the texture cache's budget policy (video_core/texture_cache/gc_budget.h).
//
// The numbers come from the measured session of 2026-10-19 (RTX 3050 Laptop, 4 GB, 720p,
// ISS_GC_BUDGET_MB=3500): the driver said it used 4757 MiB while the card has 4096 MiB and
// nvidia-smi reported 3848 MB in use, and the collector evicted the textures the game was
// drawing with, over and over. The policy must keep the cache inside its share of the budget
// without giving up the working set, and it must not believe a total that does not fit in the
// card.
#include <cassert>
#include <cstdint>

#include "video_core/texture_cache/gc_budget.h"

using VideoCore::GcBudget::Inputs;
using VideoCore::GcBudget::MakePlan;
using VideoCore::GcBudget::Percent;
using VideoCore::GcBudget::UpdateWorkingSet;

namespace {
constexpr u64 MiB = 1024 * 1024;
constexpr u64 GiB = 1024 * MiB;
} // namespace

int main() {
    // The card of the measurement: 4 GiB, the launcher's budget 3500 MB (in MiB: 3500).
    const u64 card = 4 * GiB;
    const u64 budget = 3500 * MiB;

    // 1. The driver's total is above the card (this driver counts the host memory of the
    //    buffer cache's arenas): it is not comparable with a VRAM budget, so the pressure is
    //    decided with the cache's own bytes and the budget alone. The cache (3078 MiB in the
    //    measured session) is inside the budget: nothing is freed.
    {
        const auto plan = MakePlan({.budget = budget,
                                    .cache_bytes = 3078 * MiB,
                                    .other_bytes = 0,
                                    .driver_bytes = 4757 * MiB,
                                    .physical = card,
                                    .working_set = 2900 * MiB});
        assert(!plan.driver_reliable);       // the total does not fit in the card
        assert(!plan.hard);                   // and it is not a pressure signal
        assert(!plan.over_budget);
        assert(plan.target == budget);        // the cache may use the whole budget
        assert(plan.target >= plan.floor);
        // The floor is the working set when it is bigger than the share of the budget.
        assert(plan.floor == 2900 * MiB);
    }

    // 2. The driver's total fits in the card: it can be compared with the budget. The rest of
    //    the port takes its share and the cache gets what is left.
    {
        const auto plan = MakePlan({.budget = budget,
                                    .cache_bytes = 1200 * MiB,
                                    .other_bytes = 500 * MiB,
                                    .driver_bytes = 1700 * MiB,
                                    .physical = card,
                                    .working_set = 300 * MiB});
        assert(plan.driver_reliable);
        assert(!plan.hard);
        assert(!plan.over_budget);
        // What is left of the budget after the rest of the port: 3500 - 500 = 3000 MiB.
        assert(plan.target == 3000 * MiB);
        // The free mark is above the target: the collector frees faster only when the cache is
        // more than 5% over its target.
        assert(plan.critical > plan.target);
        // A pass frees at most an eighth of the target: no mass eviction in one frame.
        assert(plan.free_per_pass > 0);
        assert(plan.free_per_pass <= plan.target / 4);
    }

    // 3. The rest of the port alone is over the budget: the cache keeps its floor, and the
    //    report says so.
    {
        const auto plan = MakePlan({.budget = budget,
                                    .cache_bytes = 2000 * MiB,
                                    .other_bytes = 3900 * MiB,
                                    .driver_bytes = 5900 * MiB,
                                    .physical = card,
                                    .working_set = 1500 * MiB});
        assert(!plan.driver_reliable);        // 5900 MiB does not fit in a 4096 MiB card
        assert(!plan.hard);
        assert(!plan.over_budget);            // the total is not usable, so it is not judged
        assert(plan.target == budget);
    }

    // 4. The driver's total fits in the card but is over it: the cache gives up exactly what the
    //    driver is over the hard mark by (the rest is the buffer cache's, which it cannot free).
    {
        const u64 hard_mark = Percent(card, 93);
        const auto plan = MakePlan({.budget = budget,
                                    .cache_bytes = 2000 * MiB,
                                    .other_bytes = 2000 * MiB,
                                    .driver_bytes = 4000 * MiB,
                                    .physical = card,
                                    .working_set = 1500 * MiB});
        assert(plan.driver_reliable);
        // 4000 MiB is over the hard mark (93% of 4096 = 3809 MiB).
        assert(plan.hard);
        assert(plan.floor == 1500 * MiB);
        // The cache gives up what the driver is over the mark by, and never goes below its floor.
        assert(plan.target == 2000 * MiB - (4000 * MiB - hard_mark));
        assert(plan.target > plan.floor);
    }

    // 5. A card big enough for everything: the cache may use the budget's share.
    {
        const auto plan = MakePlan({.budget = 8500 * MiB,      // the launcher's rule for 12 GB
                                    .cache_bytes = 2000 * MiB,
                                    .other_bytes = 500 * MiB,
                                    .driver_bytes = 2500 * MiB,
                                    .physical = 12 * GiB,
                                    .working_set = 300 * MiB});
        assert(plan.driver_reliable);
        assert(!plan.hard);
        assert(!plan.over_budget);
        assert(plan.target == 8500 * MiB - 500 * MiB);
        assert(plan.floor == Percent(8500 * MiB, 25));
    }

    // 6. Without the card's memory (the driver does not say): the total is taken at face value.
    {
        const auto plan = MakePlan({.budget = budget,
                                    .cache_bytes = 2000 * MiB,
                                    .other_bytes = 500 * MiB,
                                    .driver_bytes = 2500 * MiB,
                                    .physical = 0,
                                    .working_set = 300 * MiB});
        assert(plan.driver_reliable);
        assert(!plan.hard);
        assert(plan.target == 3000 * MiB);
    }

    // 7. The cache never aims past 90% of the card: the rest is for the desktop and the driver.
    {
        const auto plan = MakePlan({.budget = 4096 * MiB,
                                    .cache_bytes = 2000 * MiB,
                                    .other_bytes = 0,
                                    .driver_bytes = 2000 * MiB,
                                    .physical = card,
                                    .working_set = 300 * MiB});
        assert(plan.target == Percent(card, 90));
    }

    // 8. The working set: an exponential average of what the game touches per tick.
    {
        u64 working_set = 0;
        for (int i = 0; i < 1000; ++i) {
            working_set = UpdateWorkingSet(working_set, 10 * MiB);
        }
        // The steady state is the touched bytes times the window.
        assert(working_set > 300 * MiB && working_set < 330 * MiB);
        // A tick with nothing touched lets it fade.
        const u64 before = working_set;
        for (int i = 0; i < 1000; ++i) {
            working_set = UpdateWorkingSet(working_set, 0);
        }
        assert(working_set < before);
    }

    // 9. Percentages do not lose the small values.
    assert(Percent(100, 25) == 25);
    assert(Percent(0, 93) == 0);
    assert(Percent(4096 * MiB, 93) == (4096 * MiB * 93) / 100);
    // The hard mark of a 4 GB card: 93% of 4096 MiB.
    assert(Percent(4096 * MiB, 93) / MiB == 3809);
    return 0;
}
