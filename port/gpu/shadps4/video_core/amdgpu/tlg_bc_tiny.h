// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: tiny block-compressed textures decoded on the CPU (TLG_BC_TINY / ISS_BC_TINY).
//
// AMD's Windows driver (amdvlk64.dll, RX 580) writes through a null pointer at
// amdvlk64.dll+0xebbbb right after the game's first BC1 1D texture (512x1, iss_pc #16, #31,
// #49), whether that image is created 1D (unsupported) or 2D of height 1 (TLG_BLOCK1D, #36).
// NVIDIA's 617.x driver refuses BC 1D too and faults in the game shortly after one is created
// (docs/analysis/bc1d-auto-fix.md). With this on, BC1-BC5 textures that are 1D or narrower or
// shorter than one 4x4 block become plain RGBA8 / R8 / RG8 images (sRGB and snorm kept) filled
// from a CPU decode of guest memory (texture_cache/tlg_bc_decode.h): the driver never sees a
// block-compressed image of that shape. 1D ones stay 1D (no TLG_BLOCK1D promotion), which
// every driver supports.
//
// Default: automatic. Every driver that refuses BC 1D and was not explicitly given the 2D
// promotion (TLG_BLOCK1D=1) decodes these textures on the CPU; AMD's proprietary driver keeps
// its default because the 2D promotion crashes it too. TLG_BC_TINY=1 / 0 decide otherwise and
// TLG_BC_TINY=auto spells out the automatic decision. vk_instance.cpp sets it once, before any
// image or shader exists, and the shader profile carries it (a change rebuilds the pipeline
// cache).

#pragma once

#include <atomic>

namespace AmdGpu {

inline std::atomic<bool> bc_tiny_decode{false};

inline bool BcTinyDecode() {
    return bc_tiny_decode.load(std::memory_order_relaxed);
}

} // namespace AmdGpu
