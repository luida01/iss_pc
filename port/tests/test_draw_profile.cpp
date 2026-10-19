// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: the key of the render target memo (vk_target_memo.h) and the per-phase profile of the
// draw path (vk_draw_profile.h). No game and no Vulkan device: the key is plain bytes and the
// profile is a counter.
//
// The last part is a micro-benchmark of the memo key: the old code built the key in a local
// array that it zeroed (256 bytes) and copied whole (256 bytes) into the memo entry, for
// every render target of every draw (nine times per draw in a G-buffer pass). The new code
// reuses one buffer and copies only the bytes in use.
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "video_core/renderer_vulkan/vk_draw_profile.h"
#include "video_core/renderer_vulkan/vk_target_memo.h"

using Vulkan::DrawPhase;
using Vulkan::DrawProfile;
using Vulkan::TargetKey;

namespace {

/// The shape of a render target key: a tag, the color buffer (64 bytes) and the extent.
struct FakeTarget {
    u32 tag{1};
    std::array<u8, 64> color_buffer{};
    std::array<u32, 2> extent{};
};

/// The hash the memo used before: the key in a zeroed buffer, read eight bytes at a time.
u64 OldHash(const u8* key, u32 key_size) {
    u64 hash = 0xCBF29CE484222325ull;
    for (u32 i = 0; i < key_size; i += 8) {
        u64 word;
        std::memcpy(&word, key + i, 8);
        hash = (hash ^ word) * 0x100000001B3ull;
        hash ^= hash >> 29;
    }
    return hash;
}

/// The old key: a local array that is zeroed and copied whole into the memo entry.
struct OldKey {
    std::array<u8, 256> buffer{};
    u32 size{};

    void Build(const FakeTarget& target) {
        buffer = {}; // the old code cleared the whole array
        size = 0;
        std::memcpy(buffer.data() + size, &target.tag, sizeof(target.tag));
        size += sizeof(target.tag);
        std::memcpy(buffer.data() + size, target.color_buffer.data(), target.color_buffer.size());
        size += static_cast<u32>(target.color_buffer.size());
        std::memcpy(buffer.data() + size, target.extent.data(), sizeof(target.extent));
        size += sizeof(target.extent);
    }
    u64 Hash() const {
        return OldHash(buffer.data(), size);
    }
    void CopyTo(std::array<u8, 256>& destination) const {
        destination = buffer; // the old code copied the whole array
    }
};void TestHashMatchesTheOldOne() {
    FakeTarget target;
    for (u32 i = 0; i < target.color_buffer.size(); ++i) {
        target.color_buffer[i] = static_cast<u8>(i * 7 + 1);
    }
    target.extent = {1920, 1080};

    OldKey old;
    old.Build(target);
    TargetKey key;
    key.Append(target.tag);
    key.Append(target.color_buffer);
    key.Append(target.extent);

    // The bytes in use are the same...
    assert(key.size() == old.size);
    assert(std::memcmp(key.data(), old.buffer.data(), key.size()) == 0);
    // ...and so is the hash: the bytes past the key are zeros in both.
    assert(key.Hash() == old.Hash());
}

void TestEqualsAndCopy() {
    FakeTarget a;
    a.extent = {1920, 1080};
    FakeTarget b = a;
    b.extent = {1280, 720};

    TargetKey key;
    key.Append(a.tag);
    key.Append(a.color_buffer);
    key.Append(a.extent);

    std::array<u8, 256> memo{};
    key.CopyTo(memo.data());
    assert(key.Equals(memo.data(), key.size()));
    // A different target with the same size is not equal.
    TargetKey other;
    other.Append(b.tag);
    other.Append(b.color_buffer);
    other.Append(b.extent);
    assert(!other.Equals(memo.data(), memo.size()));
    // A different size is not equal either.
    assert(!key.Equals(memo.data(), memo.size() + 1));

    // Only the bytes in use are compared: the rest of the entry keeps what it had.
    std::array<u8, 256> entry{};
    entry.fill(0xAB);
    key.CopyTo(entry.data());
    for (u32 i = 0; i < key.size(); ++i) {
        assert(entry[i] == key.data()[i]);
    }
}

void TestProfileCounters() {
    DrawProfile::Add(DrawPhase::Bind, 100);
    DrawProfile::Add(DrawPhase::Bind, 200);
    DrawProfile::Add(DrawPhase::State, 50);
    DrawProfile::AddRecorder(400);
    // The report clears the counters (and prints them: the test output shows the numbers).
    DrawProfile::Report(1000, 0.001, 2);
    assert(DrawProfile::Phase(DrawPhase::Bind).cycles.load() == 0);
    assert(DrawProfile::Phase(DrawPhase::Bind).calls.load() == 0);
    assert(DrawProfile::Phase(DrawPhase::State).cycles.load() == 0);
    assert(DrawProfile::Recorder().cycles.load() == 0);
}

/// The cost of the hot path of the memo: the key of the render target is built and compared
/// with the one of the last target (the same target is drawn again and again, so the
/// comparison hits and nothing is copied). The old code built the key in a local array that
/// it zeroed (256 bytes) for every target of every draw; the new code reuses one buffer.
double Benchmark() {
    constexpr u32 Iterations = 500'000;
    constexpr u32 Turns = 4;
    FakeTarget target;
    target.extent = {1920, 1080};
    // The sinks keep the compiler from removing the work that is measured.
    volatile u64 sink = 0;

    OldKey old;
    std::array<u8, 256> old_memo{};
    TargetKey key;
    std::array<u8, 256> memo{};
    old_memo.fill(0);
    key.CopyTo(memo.data());

    double old_ns = 1e30;
    double new_ns = 1e30;
    for (u32 turn = 0; turn < Turns; ++turn) {
        {
            const auto start = std::chrono::steady_clock::now();
            for (u32 i = 0; i < Iterations; ++i) {
                target.extent[0] = 1920 + (i & 1);
                old.Build(target);
                sink ^= (std::memcmp(old.buffer.data(), old_memo.data(), old.size) == 0);
            }
            const auto end = std::chrono::steady_clock::now();
            const double ns =
                std::chrono::duration<double, std::nano>(end - start).count() / Iterations;
            old_ns = std::min(old_ns, ns);
        }
        {
            const auto start = std::chrono::steady_clock::now();
            for (u32 i = 0; i < Iterations; ++i) {
                target.extent[0] = 1920 + (i & 1);
                key.Append(target.tag);
                key.Append(target.color_buffer);
                key.Append(target.extent);
                sink ^= key.Equals(memo.data(), memo.size());
            }
            const auto end = std::chrono::steady_clock::now();
            const double ns =
                std::chrono::duration<double, std::nano>(end - start).count() / Iterations;
            new_ns = std::min(new_ns, ns);
        }
    }

    std::fprintf(stderr,
                 "Memo key (build and compare): %.1f ns before, %.1f ns after (%.0f%% less)\n",
                 old_ns, new_ns, old_ns > 0 ? 100.0 * (old_ns - new_ns) / old_ns : 0.0);
    std::fflush(stderr);
    // The new handling must not be slower (it does strictly less work).
    assert(new_ns <= old_ns * 1.05);
    return new_ns;
}

} // namespace

int main() {
    TestHashMatchesTheOldOne();
    TestEqualsAndCopy();
    TestProfileCounters();
    Benchmark();
    std::printf("draw profile test: ok\n");
    return 0;
}
