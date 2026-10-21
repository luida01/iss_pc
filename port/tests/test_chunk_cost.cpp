// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: what the port's own recording chunk costs the Vulkan recording thread.
//
// The draw profile (TLG_DRAW_PROFILE=1) says that the Vulkan recording thread spends
// 20.6 % of the window inside the chunk's execution (the "vk-record" phase). The bench
// tests/vkbench measures what the *driver* charges for the commands the port records:
// a few tens of nanoseconds each. The rest is the port: walking the chunk (a linked
// list of closures that the producer wrote on another core and the recorder reads
// cold) plus the payload every closure carries.
//
// This test measures the walk with the port's own chunk (vk_scheduler.h) and compares:
//
//   1. the chunk as it was: a linked list of closures (the reference, kept here),
//   2. the chunk as it is now: the commands one after the other with their offsets in
//      an array, so the reader walks them without chasing pointers and the hardware
//      prefetcher can work on the bodies,
//   3. the same, with a compact payload: the viewport command carries only the
//      viewports in use instead of the whole array of 16 (not implemented yet: the
//      numbers say what it would give).
//
// The producer and the consumer are two threads, like the port (the draw recording
// thread and the Vulkan recording thread), and the chunk is handed over from one to the
// other, so the consumer reads the memory the producer wrote (cold, on another core).
//
// No game, no guest and no Vulkan device: the closures do not touch the command
// buffer, so what is measured is the port's own work.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include <array>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace {

using Clock = std::chrono::steady_clock;

/// The commands of one draw, as the port records them (the counts are the ones the
/// profile measured: about 6 dynamic state commands, a vertex input, a push
/// constants, a descriptor push, a draw, a barrier and a render pass per draw).
constexpr uint32_t kDrawsPerChunk = 15; ///< the profile measured about 15 draws per chunk
constexpr uint32_t kCommandsPerDraw = 13;
constexpr uint32_t kIterations = 3000;

/// A byte the compiler cannot remove: the payloads have to stay alive.
volatile u8 sink = 0;

template <typename T>
void Touch(const T& value) {
    sink = reinterpret_cast<const u8*>(&value)[0];
}

/// The viewport state the port keeps: an array of 16 (AmdGpu::NUM_VIEWPORTS).
using Viewports = boost::container::static_vector<vk::Viewport, 16>;
using Scissors = boost::container::static_vector<vk::Rect2D, 16>;

/// The payload of the viewport command, as the port copies it today: the whole array.
struct WidePayload {
    Viewports viewports;
    Scissors scissors;
};

/// The same, with only the viewports in use.
struct NarrowPayload {
    std::array<vk::Viewport, 4> viewports;
    std::array<vk::Rect2D, 4> scissors;
    uint32_t num_viewports;
    uint32_t num_scissors;
};

/// The chunk as it was: a linked list of closures (the reference of the "before").
class LinkedChunk {
public:
    static constexpr size_t Capacity = 128 * 1024;

    template <typename Func>
    bool Push(Func&& func) {
        using Command = TypedCommand<std::decay_t<Func>>;
        const size_t offset = (used + alignof(Command) - 1) & ~(alignof(Command) - 1);
        if (offset + sizeof(Command) > Capacity) {
            return false;
        }
        auto* command = new (storage + offset) Command(std::forward<Func>(func));
        if (last) {
            last->next = command;
        } else {
            first = command;
        }
        last = command;
        used = offset + sizeof(Command);
        return true;
    }

    void* Allocate(size_t bytes, size_t align) {
        const size_t offset = (used + align - 1) & ~(align - 1);
        if (offset + bytes > Capacity) {
            return nullptr;
        }
        used = offset + bytes;
        return storage + offset;
    }

    size_t Execute(vk::CommandBuffer cmdbuf) {
        size_t count = 0;
        for (CommandBase* command = first; command;) {
            CommandBase* const next = command->next;
            command->Execute(cmdbuf);
            command->~CommandBase();
            command = next;
            ++count;
        }
        first = last = nullptr;
        used = 0;
        return count;
    }

private:
    struct CommandBase {
        virtual ~CommandBase() = default;
        virtual void Execute(vk::CommandBuffer cmdbuf) = 0;
        CommandBase* next{};
    };
    template <typename Func>
    struct TypedCommand final : CommandBase {
        explicit TypedCommand(Func&& func_) : func{std::move(func_)} {}
        explicit TypedCommand(const Func& func_) : func{func_} {}
        void Execute(vk::CommandBuffer cmdbuf) override {
            func(cmdbuf);
        }
        Func func;
    };

    alignas(64) std::byte storage[Capacity];
    size_t used = 0;
    CommandBase* first{};
    CommandBase* last{};
};

/// The commands of one draw, as closures that do not touch the command buffer (the
/// driver is measured by tests/vkbench).
template <typename Chunk, typename Payload>
void FillChunk(Chunk& chunk, const Payload& payload) {
    // The dynamic state: the viewport and the scissor carry the payload, the rest of
    // the state commands are small.
    chunk.Push([p = payload](vk::CommandBuffer) { Touch(p); });
    chunk.Push([p = payload](vk::CommandBuffer) { Touch(p); });
    chunk.Push([](vk::CommandBuffer) { sink = sink + 1; });
    chunk.Push([](vk::CommandBuffer) { sink = sink + 1; });
    chunk.Push([](vk::CommandBuffer) { sink = sink + 1; });
    chunk.Push([](vk::CommandBuffer) { sink = sink + 1; });
    // The push constants (the user data of the draw, up to 128 bytes).
    chunk.Push([data = std::array<char, 128>{}](vk::CommandBuffer) { Touch(data); });
    // The descriptor push: its data lives in the chunk's own storage (the port copies
    // the descriptor writes there).
    {
        auto* data = static_cast<char*>(chunk.Allocate(64, 8));
        if (data) {
            std::memset(data, 1, 64);
        }
        chunk.Push([info = std::array<char, 64>{}](vk::CommandBuffer) { Touch(info); });
    }
    // The vertex input and the vertex buffers.
    chunk.Push([info = std::array<char, 48>{}](vk::CommandBuffer) { Touch(info); });
    chunk.Push([info = std::array<char, 32>{}](vk::CommandBuffer) { Touch(info); });
    // The draw.
    chunk.Push([info = std::array<char, 24>{}](vk::CommandBuffer) { Touch(info); });
    // The barrier (the port's barriers carry the image barriers in the chunk's data).
    chunk.Push([info = std::array<char, 16>{}](vk::CommandBuffer) { Touch(info); });
    // The render pass.
    chunk.Push([info = std::array<char, 40>{}](vk::CommandBuffer) { Touch(info); });
}

struct Result {
    double ns_per_chunk{};
    double ns_per_draw{};
    double ns_per_command{};
    uint64_t commands{};
};

/// Fills a chunk on one thread and executes it on another (the port's two threads).
/// The chunk is handed over from one to the other, so the consumer reads the memory
/// the producer wrote (cold, on another core).
template <typename Chunk, typename Payload>
Result Measure(const Payload& payload) {
    std::mutex mutex;
    std::condition_variable producer_cv;
    std::condition_variable consumer_cv;
    Chunk chunk_a;
    Chunk chunk_b;
    Chunk* ready = nullptr;  ///< the chunk the producer filled
    Chunk* spare = &chunk_a; ///< the chunk the producer may fill

    std::atomic<bool> stop{false};
    std::thread producer([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            Chunk* mine = nullptr;
            {
                std::unique_lock lock{mutex};
                producer_cv.wait(lock, [&] { return spare != nullptr; });
                mine = spare;
                spare = nullptr;
            }
            for (uint32_t d = 0; d < kDrawsPerChunk; ++d) {
                FillChunk(*mine, payload);
            }
            {
                std::scoped_lock lock{mutex};
                ready = mine;
            }
            consumer_cv.notify_one();
        }
    });

    Result result;
    double total_ns = 0.0;
    uint64_t commands = 0;
    for (uint32_t i = 0; i < kIterations; ++i) {
        Chunk* chunk = nullptr;
        {
            std::unique_lock lock{mutex};
            consumer_cv.wait(lock, [&] { return ready != nullptr; });
            chunk = ready;
            ready = nullptr;
        }
        const auto start = Clock::now();
        chunk->Execute({});
        const auto end = Clock::now();
        total_ns += std::chrono::duration<double, std::nano>(end - start).count();
        commands += kDrawsPerChunk * kCommandsPerDraw;
        {
            std::scoped_lock lock{mutex};
            spare = chunk;
        }
        producer_cv.notify_one();
    }
    stop.store(true);
    {
        std::scoped_lock lock{mutex};
        spare = &chunk_a;
    }
    producer_cv.notify_one();
    producer.join();

    result.ns_per_chunk = total_ns / kIterations;
    result.ns_per_draw = result.ns_per_chunk / kDrawsPerChunk;
    result.ns_per_command = result.ns_per_draw / kCommandsPerDraw;
    result.commands = commands;
    return result;
}

void Print(const char* name, const Result& result) {
    std::printf("%-42s %9.0f ns/chunk %8.0f ns/draw %8.0f ns/command\n", name,
                result.ns_per_chunk, result.ns_per_draw, result.ns_per_command);
}

} // namespace

int main() {
    Viewports viewports;
    viewports.emplace_back(vk::Viewport{0.0f, 0.0f, 1280.0f, 720.0f, 0.0f, 1.0f});
    Scissors scissors;
    scissors.emplace_back(vk::Rect2D{{0, 0}, {1280, 720}});
    const WidePayload wide{viewports, scissors};

    NarrowPayload narrow{};
    narrow.viewports[0] = viewports[0];
    narrow.scissors[0] = scissors[0];
    narrow.num_viewports = 1;
    narrow.num_scissors = 1;

    std::printf("tlg: the cost of the port's recording chunk (the port's own work)\n");
    std::printf("     %u draws per chunk, %u commands per draw\n", kDrawsPerChunk,
                kCommandsPerDraw);

    // 1. The chunk as it was (a linked list) with the wide payload.
    const Result linked_wide = Measure<LinkedChunk>(wide);
    Print("linked list (before), wide payload", linked_wide);
    // 2. The chunk as it is now (the port's), with the same payload.
    const Result packed_wide = Measure<Vulkan::RecordChunk>(wide);
    Print("packed (the port's now), wide payload", packed_wide);
    // 3. The same, with the compact payload (not implemented: what it would give).
    const Result packed_narrow = Measure<Vulkan::RecordChunk>(narrow);
    Print("packed, compact payload", packed_narrow);

    std::printf("\nchunk cost test: ok\n");
    return 0;
}
