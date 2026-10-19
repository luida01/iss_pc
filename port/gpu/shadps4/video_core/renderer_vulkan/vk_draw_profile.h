// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: per-phase CPU profile of the draw path (TLG_DRAW_PROFILE=1).
//
// The frame statistics say how much the draw recording thread (stage B) is busy, but not what
// it is busy with: a draw costs several microseconds and the phases of it (resolving the
// render targets, binding the resources, building the render pass, the dynamic state, the
// descriptor writes) are spread over the texture cache, the buffer cache and the driver.
// This profile attributes the time of one recorded draw to those phases, so the next
// optimization can aim at the one that actually costs.
//
// The counters are TSC cycles (the same clock the draw pipe statistics use), not wall time:
// reading the clock twice per phase would cost more than the phases themselves. They are
// accumulated with relaxed atomics because the phase counters are written by the thread that
// records the draws and read by the thread that prints them.
//
// TLG_DRAW_PROFILE=1 turns it on. It costs two clock reads and a few atomics per phase, so
// it is a diagnostic, not a measurement of the shipping build.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <x86intrin.h>

#include "common/types.h"

namespace Vulkan {

/// tlg: the phases of a recorded draw, in the order they run.
enum class DrawPhase : u32 {
    Targets = 0,     ///< PrepareRenderState: the render targets of the draw
    Bind,            ///< BindResources: the buffers, the textures and the samplers
    State,           ///< BeginRendering: the render pass and its attachments
    Inputs,          ///< the vertex and index buffers
    Dynamic,         ///< the dynamic state (viewport, scissor, depth, blend)
    Descriptors,     ///< the descriptor writes and the push constants
    Draw,            ///< the draw command itself
    Reset,           ///< the per-draw bookkeeping (the bindings are released)
    Submit,          ///< the submission of the work that piled up (a wait for the recorder)
    Count,
};

class DrawProfile {
public:
    static constexpr u32 PhaseCount = static_cast<u32>(DrawPhase::Count);

    /// True when TLG_DRAW_PROFILE=1.
    [[nodiscard]] static bool Enabled() {
        static const bool enabled = [] {
            const char* value = std::getenv("TLG_DRAW_PROFILE");
            return value && value[0] == '1';
        }();
        return enabled;
    }

    struct Counters {
        std::atomic<u64> cycles{0};
        std::atomic<u64> calls{0};
    };

    /// The counters of the phases of a draw (the thread that records the draws writes them).
    [[nodiscard]] static Counters& Phase(DrawPhase phase) {
        return Get().phases[static_cast<u32>(phase)];
    }

    /// The cycles the Vulkan recording thread spent in the driver's command recording.
    [[nodiscard]] static Counters& Recorder() {
        return Get().recorder;
    }

    /// Adds the cycles of one phase and counts the call.
    static void Add(DrawPhase phase, u64 cycles) {
        auto& counters = Phase(phase);
        counters.cycles.fetch_add(cycles, std::memory_order_relaxed);
        counters.calls.fetch_add(1, std::memory_order_relaxed);
    }

    /// Adds the cycles the Vulkan recording thread spent recording a chunk.
    static void AddRecorder(u64 cycles) {
        auto& counters = Recorder();
        counters.cycles.fetch_add(cycles, std::memory_order_relaxed);
        counters.calls.fetch_add(1, std::memory_order_relaxed);
    }

    /// Times one phase. The cost when the profile is off is a branch per phase.
    class Scope {
    public:
        explicit Scope(DrawPhase phase) : phase{phase}, enabled{Enabled()} {
            if (enabled) {
                start = __rdtsc();
            }
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        ~Scope() {
            if (enabled) {
                Add(phase, __rdtsc() - start);
            }
        }

    private:
        DrawPhase phase;
        bool enabled;
        u64 start{};
    };

    /// Prints the profile and clears the counters. `window_cycles` are the TSC cycles of the
    /// window (the same clock the draw pipe statistics use), `window_seconds` its length.
    static void Report(u64 window_cycles, double window_seconds, u64 draws) {
        auto& self = Get();
        std::printf("Draw profile: %.1f ms window, %llu draws", window_seconds * 1e3,
                    static_cast<unsigned long long>(draws));
        u64 total = 0;
        for (u32 i = 0; i < PhaseCount; ++i) {
            total += self.phases[i].cycles.load(std::memory_order_relaxed);
        }
        for (u32 i = 0; i < PhaseCount; ++i) {
            const u64 cycles = self.phases[i].cycles.exchange(0, std::memory_order_relaxed);
            const u64 calls = self.phases[i].calls.exchange(0, std::memory_order_relaxed);
            if (!cycles) {
                continue;
            }
            const double share = window_cycles ? 100.0 * double(cycles) / double(window_cycles) : 0.0;
            std::printf("\n  %-12s %7.2f%% of the window, %8.0f cycles/call, %llu calls",
                        PhaseName(static_cast<DrawPhase>(i)), share,
                        calls ? double(cycles) / double(calls) : 0.0,
                        static_cast<unsigned long long>(calls));
        }
        const u64 rec_cycles = self.recorder.cycles.exchange(0, std::memory_order_relaxed);
        const u64 rec_calls = self.recorder.calls.exchange(0, std::memory_order_relaxed);
        if (rec_cycles) {
            const double share =
                window_cycles ? 100.0 * double(rec_cycles) / double(window_cycles) : 0.0;
            std::printf("\n  %-12s %7.2f%% of the window, %8.0f cycles/call, %llu calls",
                        "vk-record", share,
                        rec_calls ? double(rec_cycles) / double(rec_calls) : 0.0,
                        static_cast<unsigned long long>(rec_calls));
        }
        if (total) {
            std::printf("\n  %-12s %7.2f%% of the window, %8.0f cycles/draw", "draw total",
                        window_cycles ? 100.0 * double(total) / double(window_cycles) : 0.0,
                        draws ? double(total) / double(draws) : 0.0);
        }
        std::printf("\n");
    }

    [[nodiscard]] static const char* PhaseName(DrawPhase phase) {
        switch (phase) {
        case DrawPhase::Targets:
            return "targets";
        case DrawPhase::Bind:
            return "bind";
        case DrawPhase::State:
            return "state";
        case DrawPhase::Inputs:
            return "inputs";
        case DrawPhase::Dynamic:
            return "dynamic";
        case DrawPhase::Descriptors:
            return "descriptors";
        case DrawPhase::Draw:
            return "draw";
        case DrawPhase::Reset:
            return "reset";
        case DrawPhase::Submit:
            return "submit";
        default:
            return "other";
        }
    }

private:
    static DrawProfile& Get() {
        static DrawProfile instance;
        return instance;
    }

    std::array<Counters, PhaseCount> phases{};
    Counters recorder{};
};

} // namespace Vulkan
