// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: the wait of the draw recording thread (vk_draw_pipe.h): how much CPU it burns
// spinning for a packet and how much latency the sleep costs.
//
// The recording thread waits for the packets the GPU command thread posts. While it waits
// it can spin (a core burned, but the packet is picked up at once) or sleep (the core goes
// back to the game, but the packet waits for the wake-up). This test measures both sides
// of the trade with a producer that posts bursts of packets, like the GPU thread does
// while it decodes a batch of draws.
//
// The metrics are the CPU time of the recording thread (what the machine loses) and the
// latency of the packets (what the frame pays), over a window while the producer runs.
//
// No game and no Vulkan device: the pipe is a ring of packets and a thread.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <time.h>
#endif

#include "tlg_threads.h"
#include "video_core/renderer_vulkan/vk_draw_pipe.h"

using Clock = std::chrono::steady_clock;

namespace {

struct Context {
    std::atomic<u64> packets{0};
    std::atomic<u64> posted_ns{0};
    std::atomic<u64> latency_ns{0};
    std::atomic<u64> max_latency_ns{0};
};

u64 NowNs() {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());
}

/// The CPU time of the recording thread, in nanoseconds (0 when it cannot be read).
u64 RecordingThreadCpuNs(const Vulkan::DrawPipe& pipe) {
#ifdef _WIN32
    const u32 tid = pipe.cpu_tid.load();
    if (!tid) {
        return 0;
    }
    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (!thread) {
        return 0;
    }
    FILETIME created, exited, kernel, user;
    const bool ok = GetThreadTimes(thread, &created, &exited, &kernel, &user) != 0;
    CloseHandle(thread);
    if (!ok) {
        return 0;
    }
    const auto to_ns = [](const FILETIME& t) {
        return (static_cast<u64>(t.dwHighDateTime) << 32 | t.dwLowDateTime) * 100;
    };
    return to_ns(kernel) + to_ns(user);
#else
    const int clock = pipe.cpu_clock.load();
    if (clock < 0) {
        return 0;
    }
    timespec ts{};
    if (clock_gettime(clock, &ts) != 0) {
        return 0;
    }
    return static_cast<u64>(ts.tv_sec) * 1000000000ull + static_cast<u64>(ts.tv_nsec);
#endif
}

void Handler(void* context, const u8* data, u32 size) {
    (void)data;
    (void)size;
    auto& ctx = *static_cast<Context*>(context);
    const u64 now = NowNs();
    const u64 latency = now - ctx.posted_ns.load(std::memory_order_relaxed);
    ctx.latency_ns.fetch_add(latency, std::memory_order_relaxed);
    u64 previous = ctx.max_latency_ns.load(std::memory_order_relaxed);
    while (latency > previous &&
           !ctx.max_latency_ns.compare_exchange_weak(previous, latency)) {
    }
    ctx.packets.fetch_add(1, std::memory_order_relaxed);
}

struct Result {
    double cpu_us_per_packet{};
    double spin_us_per_packet{};
    double sleep_us_per_packet{};
    double mean_latency_us{};
    double max_latency_us{};
    u64 packets{};
    u64 sleeps{};
};

Result Measure(int interval_us, int burst_size) {
    Context context;
    Vulkan::DrawPipe pipe(&Handler, &context);

    std::vector<u8> payload(64);
    std::atomic<bool> stop{false};
    std::thread producer([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            for (int i = 0; i < burst_size; ++i) {
                u8* out = pipe.Begin(static_cast<u32>(payload.size()));
                std::memcpy(out, payload.data(), payload.size());
                context.posted_ns.store(NowNs(), std::memory_order_relaxed);
                pipe.Commit(static_cast<u32>(payload.size()));
            }
            std::this_thread::sleep_for(std::chrono::microseconds(interval_us));
        }
    });

    // The window: the producer is in full swing.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const u64 cpu_before = RecordingThreadCpuNs(pipe);
    const u64 spin_before = pipe.spin_ns.load();
    const u64 sleep_before = pipe.sleep_ns.load();
    const u64 packets_before = context.packets.load();
    const u64 latency_before = context.latency_ns.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const u64 cpu_after = RecordingThreadCpuNs(pipe);
    const u64 spin_after = pipe.spin_ns.load();
    const u64 sleep_after = pipe.sleep_ns.load();
    const u64 packets_after = context.packets.load();
    const u64 latency_after = context.latency_ns.load();
    stop.store(true);
    producer.join();

    const u64 packets = packets_after - packets_before;
    const double per_packet = packets ? double(packets) : 1.0;
    Result result;
    result.packets = packets;
    result.sleeps = pipe.sleeps.load();
    result.cpu_us_per_packet = double(cpu_after - cpu_before) / 1000.0 / per_packet;
    result.spin_us_per_packet = double(spin_after - spin_before) / 1000.0 / per_packet;
    result.sleep_us_per_packet = double(sleep_after - sleep_before) / 1000.0 / per_packet;
    result.mean_latency_us =
        double(latency_after - latency_before) / 1000.0 / per_packet;
    result.max_latency_us = double(context.max_latency_ns.load()) / 1000.0;
    return result;
}

} // namespace

int main() {
    // The producer is close (the packets follow each other, like a batch of draws) and far
    // (the GPU thread is decoding), which are the two cases the spin has to serve.
    struct Case {
        const char* name;
        int interval_us;
        int burst_size;
    };
    constexpr Case cases[] = {{"close (burst 20)", 20, 20}, {"far (burst 1)", 200, 1}};

    // tlg: the spin budget of the recording thread is a policy of the pipe (200 us with
    // 12 threads or more, 50 us otherwise); TLG_PIPE_SPIN_US overrides it. The pipe reads
    // it once, so a run measures one budget: the runner varies the variable.
    int budget = TlgThreads::SizingCount() >= 12 ? 200 : 50;
    if (const char* env = std::getenv("TLG_PIPE_SPIN_US")) {
        budget = std::max(0, std::atoi(env));
    }
    std::printf("spin budget: %d us\n", budget);
    std::printf("%-18s %8s %13s %13s %13s %12s %10s %8s\n", "producer", "spin us",
                "spin/pkt us", "sleep/pkt us", "cpu/pkt us", "latency us", "max us", "sleeps");
    for (const auto& c : cases) {
        const auto r = Measure(c.interval_us, c.burst_size);
        std::printf("%-18s %8d %13.2f %13.2f %13.2f %12.2f %10.2f %8llu\n", c.name, budget,
                    r.spin_us_per_packet, r.sleep_us_per_packet, r.cpu_us_per_packet,
                    r.mean_latency_us, r.max_latency_us,
                    static_cast<unsigned long long>(r.sleeps));
    }
    std::printf("draw pipe test: ok\n");
    return 0;
}
