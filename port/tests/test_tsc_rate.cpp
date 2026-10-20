// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: the clock the port's statistics use (rdtsc) on this machine.
//
// The frame statistics mix the cycles of several threads (the GPU command thread, the draw
// recording thread, the Vulkan recording thread) and divide them by the cycles of the
// window: that is only valid if the clock ticks at the same rate everywhere. This test
// measures the rate of the clock on two threads at the same time.
#include <chrono>
#include <cstdio>
#include <thread>

#include <x86intrin.h>

#include "common/types.h"

namespace {

struct Sample {
    u64 cycles{};
    double seconds{};
};

Sample Measure() {
    static volatile u64 sink;
    const auto start = std::chrono::steady_clock::now();
    const u64 tsc_start = __rdtsc();
    for (u32 i = 0; i < 40'000'000; ++i) {
        sink = sink + i;
    }
    const u64 tsc_end = __rdtsc();
    const auto end = std::chrono::steady_clock::now();
    return {tsc_end - tsc_start, std::chrono::duration<double>(end - start).count()};
}

} // namespace

int main() {
    // Both threads run at the same time.
    Sample other{};
    std::thread worker([&] { other = Measure(); });
    const Sample main_thread = Measure();
    worker.join();

    const double main_rate = double(main_thread.cycles) / main_thread.seconds;
    const double other_rate = double(other.cycles) / other.seconds;
    std::printf("main: %llu cycles in %.3f s -> %.0f Hz\n",
                static_cast<unsigned long long>(main_thread.cycles), main_thread.seconds,
                main_rate);
    std::printf("other: %llu cycles in %.3f s -> %.0f Hz\n",
                static_cast<unsigned long long>(other.cycles), other.seconds, other_rate);
    const double apart = 100.0 * (main_rate - other_rate) / main_rate;
    std::printf("%.2f%% apart: %s\n", apart,
                (apart > 2.0 || apart < -2.0)
                    ? "the clock VARIES between cores: the percentages that mix threads are not "
                      "comparable"
                    : "the clock is the same on both cores");
    return 0;
}
