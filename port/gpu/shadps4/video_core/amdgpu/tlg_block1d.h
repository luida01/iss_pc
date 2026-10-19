// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: block-compressed 1D textures created as 2D images of height 1 (AmdGpu::Image::IsBlock1D).
//
// inFAMOUS binds BC1/BC4/BC5 1D textures, and drivers may refuse block-compressed 1D images.
// This promotion is a fallback, not the safe default: sampling a compressed 1D texture through
// a 2D image repeats HUD/fonts (measured on NVIDIA), and AMD's Windows driver (amdvlk64.dll,
// RX 580) crashes whether the image is 1D or promoted (iss_pc #16, #31, #36, #49). Since
// 2026-10-19 the safe default for every refusing driver is the CPU decode of TLG_BC_TINY
// (video_core/amdgpu/tlg_bc_tiny.h, docs/analysis/bc1d-auto-fix.md); BC1-BC5 1D textures never
// take this path while BcTinyDecode() is on. The promotion stays on by default for refusing
// drivers (except NVIDIA, kept off since the streaks) as the fallback for block formats the
// CPU decoder does not handle, and TLG_BLOCK1D=1 forces it for experiments.
//
// TLG_BLOCK1D=1 or 0 decides otherwise; vk_instance.cpp sets the value once, before any image or
// shader exists, and the shader profile carries it (a change rebuilds the pipeline cache).

#pragma once

#include <atomic>

namespace AmdGpu {

inline std::atomic<bool> block1d_as_2d{false};

inline bool Block1DAs2D() {
    return block1d_as_2d.load(std::memory_order_relaxed);
}

// tlg: BP09 (CTR d26bcb2), TLG_BP09=1: every 1D image, block-coded or not, is a 2D image of
// height 1 on the host (Vulkan cannot view a 2D render target as 1D, and the game builds
// colour-grading LUTs from 64x1 / 1024x1 targets it samples as 1D). Set once by vk_instance.cpp
// before any image or shader exists; part of the shader cache key through the backports salt.
inline std::atomic<bool> all_1d_as_2d{false};

inline bool All1DAs2D() {
    return all_1d_as_2d.load(std::memory_order_relaxed);
}

} // namespace AmdGpu
