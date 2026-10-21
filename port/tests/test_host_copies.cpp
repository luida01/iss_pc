// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: the queue of the guest memory copies (the ones the guest's writes and the fences
// wait for).
//
// The copies used to be recorded in the command chunk, so the thread that needed them done
// (the draw recording thread, in a task) had to wait for the Vulkan recording thread to
// record the chunk up to them: 262 us per wait, 12 % of the recording (the profile of
// 2026-10-21). Now the copies are in a queue of their own and the thread that waits for
// them runs them.
//
// The test checks the semantics (the order, the counters, several producer threads) and
// measures the wait of the two designs: the copy in the chunk of another thread (before) and
// the copy in the queue of the waiter (now).
//
// No game, no guest and no Vulkan device: the copies are host memcpys.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace {

using Clock = std::chrono::steady_clock;

/// The commands of a chunk, as the port records them: the draws and, among them, the copy.
struct ChunkState {
    Vulkan::RecordChunk chunk;
    std::atomic<u64> copies_run{0};
    std::atomic<bool> copy_run{false};
};

/// The work of the thread that records the draws (busy, so that a copy that is behind it
/// takes a while to be reached).
void BusyWork(double microseconds) {
    const auto start = Clock::now();
    while (std::chrono::duration<double, std::micro>(Clock::now() - start).count() < microseconds) {
        std::this_thread::yield();
    }
}

/// The "before": the copy is a command in the chunk, and the thread that records the draws
/// is busy with the other commands of the chunk; the waiter needs the copy done.
double MeasureWaitInChunk(double busy_us, u32 commands) {
    std::atomic<bool> copy_run{false};
    std::thread recorder([&] {
        Vulkan::RecordChunk chunk;
        for (uint32_t i = 0; i < commands; ++i) {
            chunk.Push([busy_us, commands](vk::CommandBuffer) { BusyWork(busy_us / commands); });
        }
        // The copy, at the end of the chunk (as it is when the draws that need it are
        // recorded before it).
        chunk.Push([&](vk::CommandBuffer) { copy_run.store(true, std::memory_order_release); });
        chunk.Execute({});
    });
    const auto start = Clock::now();
    while (!copy_run.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    const double ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    recorder.join();
    return ns;
}

/// The "now": the copy is in the queue of the waiter; the waiter runs it and does not
/// depend on the thread that records the draws.
double MeasureWaitInQueue(double busy_us, u32 commands) {
    std::atomic<bool> copy_run{false};
    auto copies = std::make_shared<Vulkan::HostCopyQueue>();
    std::thread recorder([&, copies] {
        // The copy is queued while the draws are recorded (as in the port), and the draws
        // go on: the waiter that needs the copy comes later.
        copies->Push([&] { copy_run.store(true, std::memory_order_release); });
        BusyWork(busy_us);
        (void)commands;
    });
    const auto start = Clock::now();
    while (!copy_run.load(std::memory_order_acquire)) {
        copies->Run();
        std::this_thread::yield();
    }
    const double ns = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    recorder.join();
    return ns;
}
} // namespace

int main() {
    // 1. The copies run in the order they were queued, and the counters add up.
    {
        Vulkan::HostCopyQueue copies;
        std::vector<u32> order;
        for (u32 i = 0; i < 100; ++i) {
            copies.Push([&order, i] { order.push_back(i); });
        }
        if (copies.Issued() != 100 || copies.Done() != 0 || !copies.Pending()) {
            std::printf("FAIL: the counters after the queue: %llu issued, %llu done\n",
                        (unsigned long long)copies.Issued(), (unsigned long long)copies.Done());
            return 1;
        }
        copies.Run();
        if (copies.Done() != 100 || copies.Pending()) {
            std::printf("FAIL: the counters after the run: %llu done\n",
                        (unsigned long long)copies.Done());
            return 1;
        }
        for (u32 i = 0; i < order.size(); ++i) {
            if (order[i] != i) {
                std::printf("FAIL: the copy %u ran out of order\n", i);
                return 1;
            }
        }
    }

    // 2. Several threads queue copies: they all run, and the counters add up.
    {
        Vulkan::HostCopyQueue copies;
        std::atomic<u64> ran{0};
        std::vector<std::thread> producers;
        for (u32 t = 0; t < 4; ++t) {
            producers.emplace_back([&] {
                for (u32 i = 0; i < 500; ++i) {
                    copies.Push([&ran] {
                        ran.fetch_add(1, std::memory_order_relaxed);
                    });
                }
            });
        }
        for (auto& producer : producers) {
            producer.join();
        }
        if (copies.Issued() != 2000) {
            std::printf("FAIL: %llu copies issued, 2000 expected\n",
                        (unsigned long long)copies.Issued());
            return 1;
        }
        while (copies.Pending()) {
            copies.Run();
        }
        if (ran.load() != 2000 || copies.Done() != 2000) {
            std::printf("FAIL: %llu copies ran, %llu done\n", (unsigned long long)ran.load(),
                        (unsigned long long)copies.Done());
            return 1;
        }
    }

    // 3. The wait: the copy in the chunk of another thread against the copy in the queue of
    // the waiter. The commands of the chunk stand for the draws the Vulkan recording thread
    // records before it reaches the copy.
    {
        constexpr u32 kCommands = 40;
        const double in_chunk = MeasureWaitInChunk(2000.0, kCommands);
        const double in_queue = MeasureWaitInQueue(2000.0, kCommands);
        std::printf("the wait for a copy (%u commands before it in the chunk):\n", kCommands);
        std::printf("  in the chunk of another thread (before): %9.0f ns\n", in_chunk);
        std::printf("  in the queue of the waiter (now):      %9.0f ns\n", in_queue);
        if (in_queue >= in_chunk) {
            std::printf("FAIL: the queue of the waiter is not faster\n");
            return 1;
        }
    }

    std::printf("host copies test: ok\n");
    return 0;
}
