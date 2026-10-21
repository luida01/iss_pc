// SPDX-License-Identifier: GPL-2.0-or-later
// The draw census (video_core/amdgpu/tlg_draw_census.h): the accounting the port prints with
// TLG_DRAW_CENSUS=1, so a missing effect can be told apart from a draw that asks for 0
// instances.
#include <cassert>
#include <cstdio>
#include <utility>

#include "video_core/amdgpu/tlg_draw_census.h"

using AmdGpu::DrawCensus;

int main() {
    DrawCensus census;

    // Two indirect sites: one with instances, one without (the particle list empty).
    census.NoteIndirectSite(0x1000, [] { return std::pair<uint32_t, uint32_t>{6, 120}; });
    census.NoteIndirectSite(0x2000, [] { return std::pair<uint32_t, uint32_t>{6, 0}; });
    // The same site again: the arguments are read once per site and window.
    census.NoteIndirectSite(0x1000, [] { return std::pair<uint32_t, uint32_t>{6, 0}; });
    assert(census.IndirectDraws() == 3);

    // The direct draws, by instance count.
    census.NoteDirectDraw(1);
    census.NoteDirectDraw(1);
    census.NoteDirectDraw(4);

    // The dispatches, by their size.
    census.NoteDispatch(64, 1, 1);
    census.NoteDispatch(64, 1, 1);
    census.NoteDispatch(256, 1, 1);

    census.Report();

    // The window is reset: the counters start again.
    census.Reset();
    assert(census.IndirectDraws() == 0);
    census.NoteDirectDraw(2);
    census.Report();

    std::puts("draw-census-test: ok");
    return 0;
}
