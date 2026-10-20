// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: the pacing of the presentation thread (the port's flip path).
//
// The bench models the two variants of the port's present thread (the same algorithm as
// port/gpu/shadps4/core/libraries/videoout/driver.cpp, PresentThread):
//
//   old: the flip request is taken at the vblank tick, the present goes to the display
//        engine at the tick and the flip event fires with it.
//   new: the flip request is taken as soon as it arrives (the early present), so the frame
//        reaches the display engine before the vblank that latches it; the flip event still
//        fires at the vblank tick, so the guest's pacing does not change.
//
// The model of the rest of the port:
//   * the display latches a frame at the first vblank after the present reached it. The
//     display runs at 59.94 Hz and the port's tick at 60 Hz, so the phase between them
//     wanders (as it does on a real display).
//   * the pool of presentation frames (3) is the backpressure: the guest cannot be more
//     than that many frames ahead of the presents.
//   * the guest waits for the flip event (the way a game that paces itself with the
//     display does) or runs free (it never waits).
//
// What it measures (per variant):
//   * the flip queue wait: the guest's flip call -> the present thread takes it
//   * the display latency: the guest's flip call -> the display latches the frame
//   * the guest's frame period: the flip event -> the next flip event (the guest's pacing)
//
// The clock is virtual (no sleeping), so the numbers are reproducible.
//
// Usage: test_flip_pacing [--quick]

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

namespace {

constexpr double kPortTickMs = 1000.0 / 60.0;   // the port's vblank tick
constexpr double kDisplayVblankMs = 1000.0 / 59.94; // the display (its phase wanders)
constexpr int kFramePool = 3;                  // the presentation frames the port has
constexpr int kSwapchainImages = 3;            // the images of the swapchain

/// tlg: how the guest paces itself.
enum class GuestMode {
    Free,      ///< it never waits: it produces frames as fast as it can
    FlipEvent, ///< it waits for the flip event of the port
    Vblank,    ///< it waits for the vblank event of the port (the frame is ready just
               ///< after the vblank, so the flip waits a whole vblank for the next one)
};

struct Sample {
    double queue_wait_ms{};   // the guest's flip -> the present thread takes it
    double swap_wait_ms{};    // the present thread takes it -> the display engine has it
    double display_ms{};      // the guest's flip -> the display latches the frame
    double guest_period_ms{}; // the flip event -> the next flip event (the guest's pacing)
    double latch_wait_ms{};   // the present -> the display latches it
    double fps{};             // the guest's frames per second
    unsigned frames{};
};

/// The next display vblank strictly after 'now' (the display latches the frame there).
double NextLatch(double now) {
    return now + (kDisplayVblankMs - std::fmod(now, kDisplayVblankMs));
}

/// The port's vblank tick: the present thread wakes up on this grid.
double NextTick(double now) {
    return now + (kPortTickMs - std::fmod(now, kPortTickMs));
}

/// The model of the present thread, of the guest and of the pool of frames.
///
/// 'early': the new variant (the request is taken as soon as it arrives).
Sample RunModel(double guest_work_ms, GuestMode mode, bool early, unsigned frames) {
    struct Request {
        double submit_ms{}; // the guest's flip call
        double taken_ms{};  // the present thread took it
    };

    Sample out{};
    double now = 0.0;
    double tick = 0.0;       // the next tick of the present thread
    double guest_work_until = 0.0; // when the guest finishes its work
    double next_display_vblank = 0.0; // the display releases an image at every vblank
    bool guest_waiting = false; // the guest waits for the flip event
    bool guest_blocked = false; // the guest waits for a free frame (the pool is empty)
    bool have_event = false;
    double last_event_ms = 0.0;
    int pool = kFramePool;
    int images_in_flight = 0; // the images of the swapchain that the display has not released
    std::deque<Request> requests;
    std::deque<Request> pending;  // the presents that went to the display, waiting for the tick
    std::deque<Request> swap_queue; // the presents waiting for the swap thread

    const auto fire_event = [&](double at) {
        if (have_event) {
            out.guest_period_ms += at - last_event_ms;
        }
        last_event_ms = at;
        have_event = true;
        if (mode == GuestMode::FlipEvent) {
            guest_waiting = false; // the guest may work again
        }
    };

    // The swap thread: the present reaches the display engine here (it waits for an image
    // of the swapchain: the display releases one at every vblank).
    const auto dispatch = [&](double at) {
        if (swap_queue.empty() || images_in_flight >= kSwapchainImages) {
            return;
        }
        const auto req = swap_queue.front();
        swap_queue.pop_front();
        const auto latch = NextLatch(at);
        out.queue_wait_ms += req.taken_ms - req.submit_ms;
        out.swap_wait_ms += at - req.taken_ms;
        out.display_ms += latch - req.submit_ms;
        out.latch_wait_ms += latch - at;
        out.frames++;
        images_in_flight++;
        pool++; // the frame comes back to the pool
        if (early) {
            // The present went to the display engine: the flip event waits for the tick.
            pending.push_back(req);
        }
    };

    // The scene varies a little (the same scene at a different moment costs a bit more),
    // so the phase of the guest against the vblank wanders as it does in the game.
    const auto guest_work = [&] {
        return guest_work_ms * (1.0 + 0.05 * std::sin(static_cast<double>(out.frames) * 0.37));
    };

    // The present thread takes a request: it hands it to the swap thread.
    const auto take = [&](double at) {
        auto req = requests.front();
        requests.pop_front();
        req.taken_ms = at;
        swap_queue.push_back(req);
        return req;
    };

    unsigned long long iterations = 0;
    while (out.frames < frames) {
        if (++iterations > frames * 10000ULL + 10000ULL) {
            break; // the model cannot make progress (a bug): do not hang the bench
        }

        // The present thread wakes up at the tick.
        if (now >= tick) {
            if (early) {
                if (!pending.empty()) {
                    // The flip event of the frame presented since the last tick.
                    pending.pop_front();
                    fire_event(now);
                } else if (!requests.empty()) {
                    // Nothing pending: a request that is already waiting is taken and
                    // presented now (its flip event fires with it, as before).
                    take(now);
                    dispatch(now);
                    fire_event(now);
                }
            } else if (!requests.empty()) {
                take(now);
                dispatch(now);
                fire_event(now);
            }
            tick = NextTick(now);
            if (mode == GuestMode::Vblank && guest_waiting) {
                guest_waiting = false; // the vblank releases the guest
                guest_work_until = now + guest_work(); // and it works from here
            }
        }

        // The guest: it works and submits a flip (it needs a free frame for it).
        // The vblank paced guest is released at the tick and submits its flip after its
        // work; the other two submit as soon as they are done.
        if (!guest_waiting && !guest_blocked && now >= guest_work_until) {
            if (pool > 0) {
                pool--;
                requests.push_back({.submit_ms = now});
                if (mode == GuestMode::FlipEvent) {
                    guest_waiting = true; // it waits for the flip event
                    guest_work_until = now + guest_work();
                } else if (mode == GuestMode::Vblank) {
                    guest_waiting = true; // it waits for the next vblank
                } else {
                    guest_work_until = now + guest_work();
                }
                if (early) {
                    // The early present: the request is taken the moment it arrives.
                    take(now);
                    dispatch(now);
                }
            } else {
                // The pool is empty: the guest waits for the present to free a frame.
                guest_blocked = true;
            }
        }
        if (guest_blocked && pool > 0) {
            guest_blocked = false;
        }

        // The swap thread: the present goes to the display engine when the swapchain has
        // an image free.
        dispatch(now);

        // Advance the clock: the next event is the earliest of the tick, the guest's work
        // and the display's vblank (which releases an image of the swapchain).
        while (next_display_vblank <= now) {
            next_display_vblank += kDisplayVblankMs;
            if (images_in_flight > 0) {
                images_in_flight--;
            }
        }
        double next = std::min(tick, next_display_vblank);
        if (!guest_waiting && !guest_blocked) {
            next = std::min(next, guest_work_until);
        }
        if (next <= now) {
            next = now + 0.0001;
        }
        now = next;
    }

    const auto n = static_cast<double>(std::max<unsigned>(1, frames));
    out.queue_wait_ms /= n;
    out.swap_wait_ms /= n;
    out.display_ms /= n;
    out.latch_wait_ms /= n;
    out.guest_period_ms /= std::max(1.0, n - 1.0);
    out.fps = out.guest_period_ms > 0.0 ? 1000.0 / out.guest_period_ms : 0.0;
    return out;
}

void Report(const char* title, double guest_work_ms, GuestMode mode, unsigned frames) {
    const char* mode_name = mode == GuestMode::Free ? "the guest runs free"
                              : mode == GuestMode::FlipEvent
                                  ? "the guest waits for the flip event"
                                  : "the guest waits for the vblank event";
    const auto old_sample = RunModel(guest_work_ms, mode, false, frames);
    const auto new_sample = RunModel(guest_work_ms, mode, true, frames);
    std::printf("%-28s work %6.2f ms, %s\n", title, guest_work_ms, mode_name);
    std::printf("   old: queue %6.2f ms, swap %5.2f ms, display %6.2f ms (latch wait %5.2f ms), "
                "guest period %7.2f ms (%.1f FPS)\n",
                old_sample.queue_wait_ms, old_sample.swap_wait_ms, old_sample.display_ms,
                old_sample.latch_wait_ms, old_sample.guest_period_ms, old_sample.fps);
    std::printf("   new: queue %6.2f ms, swap %5.2f ms, display %6.2f ms (latch wait %5.2f ms), "
                "guest period %7.2f ms (%.1f FPS)\n",
                new_sample.queue_wait_ms, new_sample.swap_wait_ms, new_sample.display_ms,
                new_sample.latch_wait_ms, new_sample.guest_period_ms, new_sample.fps);
    std::printf("   change: display latency %+6.2f ms, the guest's pacing %+6.2f ms\n\n",
                new_sample.display_ms - old_sample.display_ms,
                new_sample.guest_period_ms - old_sample.guest_period_ms);
}

} // namespace

int main(int argc, char** argv) {
    bool quick = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string{argv[i]} == "--quick") {
            quick = true;
        }
    }
    const unsigned frames = quick ? 300 : 2000;

    std::printf("The present thread's pacing (the port's flip path), %u frames per case.\n",
                frames);
    std::printf("The display latches a frame at the first vblank after the present reached "
                "it.\n\n");

    // The scene of the session of the user: the guest spends ~135 ms per frame (the frame
    // takes 150 ms with the present path) and the present path is not the bottleneck.
    Report("heavy scene, vblank paced", 135.0, GuestMode::Vblank, frames);
    Report("heavy scene, flip paced", 135.0, GuestMode::FlipEvent, frames);
    // The light scene (the title): the guest is faster than the display.
    Report("light scene, vblank paced", 0.6, GuestMode::Vblank, frames);
    Report("light scene, flip paced", 0.6, GuestMode::FlipEvent, frames);
    // The guest that does not wait for the flip event (the port cannot pace it).
    Report("heavy scene, guest free", 135.0, GuestMode::Free, frames);
    Report("light scene, guest free", 0.6, GuestMode::Free, frames);
    return 0;
}
