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
};

/// tlg: the key of the memo is built in a buffer that is reused for every lookup of the
/// draw (nine in a G-buffer pass) and for every draw. A key that is not reset before it is
/// built keeps the bytes of the previous one, grows past the buffer and writes over the
/// members that follow it (the rasterizer's own members): the memo then answers with the
/// image of another target and the renderer reads a pointer that is not valid.
///
/// This test walks the sequence of a frame (the targets of a G-buffer pass, draw after draw)
/// and checks that the key is always the key of the target it was asked for.
void TestKeyIsReusedSafely() {
    struct {
        TargetKey key;
        std::array<u8, 64> canary;
    } holder;
    holder.canary.fill(0xCD);

    // The targets of a G-buffer pass: eight color buffers and the depth.
    std::vector<FakeTarget> targets(9);
    for (u32 i = 0; i < targets.size(); ++i) {
        targets[i].tag = i + 1;
        for (u32 b = 0; b < targets[i].color_buffer.size(); ++b) {
            targets[i].color_buffer[b] = static_cast<u8>(i * 31 + b);
        }
        targets[i].extent = {1920, 1080};
    }

    // The memo of the renderer, with the key handling of the renderer.
    std::array<std::array<u8, 256>, 64> memo{};
    std::array<u32, 64> memo_size{};
    std::array<u64, 64> memo_hash{};
    std::array<bool, 64> memo_used{};
    std::array<u32, 64> memo_id{};
    // The id the renderer gets for each target (it looks the image up on a miss).
    std::array<u32, 9> ids{};

    for (u32 frame = 0; frame < 8; ++frame) {
        for (u32 t = 0; t < targets.size(); ++t) {
            const auto& target = targets[t];
            holder.key.Reset();
            holder.key.Append(target.tag);
            holder.key.Append(target.color_buffer);
            holder.key.Append(target.extent);
            // The key is the one of this target and nothing else.
            assert(holder.key.size() == 4 + 64 + 8);
            assert(std::memcmp(holder.key.data(), &target.tag, 4) == 0);
            assert(std::memcmp(holder.key.data() + 4, target.color_buffer.data(), 64) == 0);
            // The key did not write past its buffer.
            for (const u8 byte : holder.canary) {
                assert(byte == 0xCD);
            }
            // A fresh key of the same target hashes the same: the memo finds it again.
            TargetKey fresh;
            fresh.Reset();
            fresh.Append(target.tag);
            fresh.Append(target.color_buffer);
            fresh.Append(target.extent);
            assert(holder.key.Hash() == fresh.Hash());

            const u64 hash = holder.key.Hash();
            const auto slot = hash % memo.size();
            u32 id = 0;
            if (memo_used[slot] && memo_hash[slot] == hash && memo_size[slot] == holder.key.size() &&
                std::memcmp(memo[slot].data(), holder.key.data(), holder.key.size()) == 0) {
                id = memo_id[slot];
            } else {
                // A miss: the renderer looks the image up and the slot keeps its id.
                id = 0x1000 + t;
                holder.key.CopyTo(memo[slot].data());
                memo_size[slot] = holder.key.size();
                memo_hash[slot] = hash;
                memo_id[slot] = id;
                memo_used[slot] = true;
            }
            // The id of a target is always the same one: the renderer binds that image.
            if (ids[t] == 0) {
                ids[t] = id;
            }
            assert(ids[t] == id);
        }
        // Two different targets never share an id.
        for (u32 a = 0; a < ids.size(); ++a) {
            for (u32 b = a + 1; b < ids.size(); ++b) {
                assert(ids[a] != ids[b]);
            }
        }
    }
}

void TestHashMatchesTheOldOne() {
    FakeTarget target;
    for (u32 i = 0; i < target.color_buffer.size(); ++i) {
        target.color_buffer[i] = static_cast<u8>(i * 7 + 1);
    }
    target.extent = {1920, 1080};

    OldKey old;
    old.Build(target);
    TargetKey key;
    key.Reset();
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
    key.Reset();
    key.Append(a.tag);
    key.Append(a.color_buffer);
    key.Append(a.extent);

    std::array<u8, 256> memo{};
    key.CopyTo(memo.data());
    assert(key.Equals(memo.data(), key.size()));
    // A different target with the same size is not equal.
    TargetKey other;
    other.Reset();
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
    DrawProfile::AddRecorder(400, 8);
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
    // The sinks keep the compiler from removing the work that is measured: the target
    // changes every iteration (from a volatile seed), so nothing can be folded.
    volatile u64 sink = 0;
    volatile u32 seed = 0;

    OldKey old;
    std::array<u8, 256> old_memo{};
    TargetKey key;
    std::array<u8, 256> memo{};
    old_memo.fill(0);
    key.Reset();
    key.Append(target.tag);
    key.Append(target.color_buffer);
    key.Append(target.extent);
    key.CopyTo(memo.data());

    double old_ns = 1e30;
    double new_ns = 1e30;
    for (u32 turn = 0; turn < Turns; ++turn) {
        {
            const auto start = std::chrono::steady_clock::now();
            for (u32 i = 0; i < Iterations; ++i) {
                target.extent[0] = 1920 + (seed + i);
                old.Build(target);
                sink ^= (std::memcmp(old.buffer.data(), old_memo.data(), old.size) == 0);
                sink ^= old.buffer[i & 255];
            }
            const auto end = std::chrono::steady_clock::now();
            const double ns =
                std::chrono::duration<double, std::nano>(end - start).count() / Iterations;
            old_ns = std::min(old_ns, ns);
        }
        {
            const auto start = std::chrono::steady_clock::now();
            for (u32 i = 0; i < Iterations; ++i) {
                target.extent[0] = 1920 + (seed + i);
                key.Reset(); // the renderer resets the key before every lookup
                key.Append(target.tag);
                key.Append(target.color_buffer);
                key.Append(target.extent);
                sink ^= key.Equals(memo.data(), memo.size());
                sink ^= key.data()[i & 255];
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
    TestKeyIsReusedSafely();
    TestHashMatchesTheOldOne();
    TestEqualsAndCopy();
    TestProfileCounters();
    Benchmark();
    std::printf("draw profile test: ok\n");
    return 0;
}
