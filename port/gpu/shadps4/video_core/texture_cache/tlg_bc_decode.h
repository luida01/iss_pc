// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: CPU decoder for tiny block-compressed textures (TLG_BC_TINY, video_core/amdgpu/tlg_bc_tiny.h).
//
// BC1-BC5 blocks to RGBA8 / R8 / RG8 texels, and the GCN micro-tile addressing of
// host_shaders/tiling.comp (one sample, non-macro array modes) to find each block in guest
// memory. Header-only and free of Vulkan so tests/test_bc_decode.cpp can check it alone.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace VideoCore::BcDecode {

enum class Kind : uint8_t {
    Bc1,      ///< RGBA8; 3-colour blocks give transparent black for index 3
    Bc2,      ///< RGBA8; explicit 4-bit alpha
    Bc3,      ///< RGBA8; interpolated alpha
    Bc4Unorm, ///< R8
    Bc4Snorm, ///< R8 signed
    Bc5Unorm, ///< RG8
    Bc5Snorm, ///< RG8 signed
};

/// Bytes of one encoded 4x4 block.
constexpr uint32_t BlockBytes(Kind kind) {
    return kind == Kind::Bc1 || kind == Kind::Bc4Unorm || kind == Kind::Bc4Snorm ? 8 : 16;
}

/// Bytes of one decoded texel.
constexpr uint32_t TexelBytes(Kind kind) {
    switch (kind) {
    case Kind::Bc4Unorm:
    case Kind::Bc4Snorm:
        return 1;
    case Kind::Bc5Unorm:
    case Kind::Bc5Snorm:
        return 2;
    default:
        return 4;
    }
}

namespace Detail {

inline uint16_t Load16(const uint8_t* p) {
    return uint16_t(p[0] | (p[1] << 8));
}

inline void Expand565(uint16_t c, uint32_t rgb[3]) {
    const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    rgb[0] = (r << 3) | (r >> 2);
    rgb[1] = (g << 2) | (g >> 4);
    rgb[2] = (b << 3) | (b >> 2);
}

/// Colour part of BC1/BC2/BC3 into 16 RGBA texels (row-major 4x4). BC2/BC3 always use the
/// four-colour mode; only BC1 has the three-colour mode with transparent black.
inline void DecodeColor(const uint8_t* block, bool bc1, uint8_t out[16][4]) {
    const uint16_t c0 = Load16(block), c1 = Load16(block + 2);
    uint32_t p[4][4];
    Expand565(c0, p[0]);
    Expand565(c1, p[1]);
    p[0][3] = p[1][3] = 255;
    const bool four = !bc1 || c0 > c1;
    for (int i = 0; i < 3; ++i) {
        if (four) {
            p[2][i] = (2 * p[0][i] + p[1][i] + 1) / 3;
            p[3][i] = (p[0][i] + 2 * p[1][i] + 1) / 3;
        } else {
            p[2][i] = (p[0][i] + p[1][i] + 1) / 2;
            p[3][i] = 0;
        }
    }
    p[2][3] = 255;
    p[3][3] = four ? 255 : 0;
    uint32_t indices;
    std::memcpy(&indices, block + 4, 4);
    for (int t = 0; t < 16; ++t) {
        const uint32_t idx = (indices >> (2 * t)) & 3;
        for (int c = 0; c < 4; ++c) {
            out[t][c] = uint8_t(p[idx][c]);
        }
    }
}

/// BC4-style 8-byte channel block (also BC3 alpha) into 16 unsigned values.
inline void DecodeChannelUnorm(const uint8_t* block, uint8_t out[16]) {
    const uint32_t e0 = block[0], e1 = block[1];
    uint32_t v[8] = {e0, e1};
    if (e0 > e1) {
        for (uint32_t i = 1; i < 7; ++i) {
            v[i + 1] = ((7 - i) * e0 + i * e1 + 3) / 7;
        }
    } else {
        for (uint32_t i = 1; i < 5; ++i) {
            v[i + 1] = ((5 - i) * e0 + i * e1 + 2) / 5;
        }
        v[6] = 0;
        v[7] = 255;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) {
        bits |= uint64_t(block[2 + i]) << (8 * i);
    }
    for (int t = 0; t < 16; ++t) {
        out[t] = uint8_t(v[(bits >> (3 * t)) & 7]);
    }
}

inline int32_t RoundDiv(int32_t num, int32_t den) {
    return num >= 0 ? (num + den / 2) / den : -((-num + den / 2) / den);
}

/// Signed BC4 channel block into 16 snorm bytes (-128 reads as -127, as in the formats).
inline void DecodeChannelSnorm(const uint8_t* block, int8_t out[16]) {
    const int32_t e0 = std::max<int32_t>(int8_t(block[0]), -127);
    const int32_t e1 = std::max<int32_t>(int8_t(block[1]), -127);
    int32_t v[8] = {e0, e1};
    if (e0 > e1) {
        for (int32_t i = 1; i < 7; ++i) {
            v[i + 1] = RoundDiv((7 - i) * e0 + i * e1, 7);
        }
    } else {
        for (int32_t i = 1; i < 5; ++i) {
            v[i + 1] = RoundDiv((5 - i) * e0 + i * e1, 5);
        }
        v[6] = -127;
        v[7] = 127;
    }
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) {
        bits |= uint64_t(block[2 + i]) << (8 * i);
    }
    for (int t = 0; t < 16; ++t) {
        out[t] = int8_t(v[(bits >> (3 * t)) & 7]);
    }
}

} // namespace Detail

/// Decodes one block into 16 texels of TexelBytes(kind) each, row-major (texel = y * 4 + x).
inline void DecodeBlock(Kind kind, const uint8_t* block, uint8_t* out) {
    switch (kind) {
    case Kind::Bc1:
    case Kind::Bc2:
    case Kind::Bc3: {
        const uint8_t* color = kind == Kind::Bc1 ? block : block + 8;
        uint8_t rgba[16][4];
        Detail::DecodeColor(color, kind == Kind::Bc1, rgba);
        if (kind == Kind::Bc2) {
            for (int t = 0; t < 16; ++t) {
                const uint32_t a = (block[t / 2] >> (4 * (t & 1))) & 15;
                rgba[t][3] = uint8_t(a * 17);
            }
        } else if (kind == Kind::Bc3) {
            uint8_t alpha[16];
            Detail::DecodeChannelUnorm(block, alpha);
            for (int t = 0; t < 16; ++t) {
                rgba[t][3] = alpha[t];
            }
        }
        std::memcpy(out, rgba, sizeof(rgba));
        return;
    }
    case Kind::Bc4Unorm:
        Detail::DecodeChannelUnorm(block, out);
        return;
    case Kind::Bc4Snorm:
        Detail::DecodeChannelSnorm(block, reinterpret_cast<int8_t*>(out));
        return;
    case Kind::Bc5Unorm:
    case Kind::Bc5Snorm: {
        uint8_t r[16], g[16];
        if (kind == Kind::Bc5Unorm) {
            Detail::DecodeChannelUnorm(block, r);
            Detail::DecodeChannelUnorm(block + 8, g);
        } else {
            Detail::DecodeChannelSnorm(block, reinterpret_cast<int8_t*>(r));
            Detail::DecodeChannelSnorm(block + 8, reinterpret_cast<int8_t*>(g));
        }
        for (int t = 0; t < 16; ++t) {
            out[2 * t] = r[t];
            out[2 * t + 1] = g[t];
        }
        return;
    }
    }
}

/// Micro tile modes (AmdGpu::MicroTileMode).
enum : uint32_t {
    MicroDisplay = 0,
    MicroThin = 1,
    MicroDepth = 2,
    MicroRotated = 3,
};

/// Index of element (x, y, z) inside its 8x8 micro tile: ComputePixelIndexWithinMicroTile of
/// host_shaders/tiling.comp for 64- and 128-bit elements (the BC block sizes).
inline uint32_t MicroTileElementIndex(uint32_t x, uint32_t y, uint32_t z, uint32_t micro_mode,
                                      uint32_t thickness, uint32_t bits) {
    const uint32_t x0 = x & 1, x1 = (x >> 1) & 1, x2 = (x >> 2) & 1;
    const uint32_t y0 = y & 1, y1 = (y >> 1) & 1, y2 = (y >> 2) & 1;
    const uint32_t z0 = z & 1, z1 = (z >> 1) & 1, z2 = (z >> 2) & 1;
    uint32_t p[9] = {};
    if (micro_mode == MicroDisplay) {
        if (bits == 64) {
            p[0] = x0, p[1] = y0, p[2] = x1, p[3] = x2, p[4] = y1, p[5] = y2;
        } else {
            p[0] = y0, p[1] = x0, p[2] = x1, p[3] = x2, p[4] = y1, p[5] = y2;
        }
    } else if (micro_mode == MicroThin || micro_mode == MicroDepth) {
        p[0] = x0, p[1] = y0, p[2] = x1, p[3] = y1, p[4] = x2, p[5] = y2;
    } else {
        p[0] = x0, p[1] = y0, p[2] = z0, p[3] = x1, p[4] = y1, p[5] = z1;
        p[6] = x2, p[7] = y2;
        if (thickness == 8) {
            p[8] = z2;
        }
    }
    uint32_t index = 0;
    for (uint32_t i = 0; i < 9; ++i) {
        index |= p[i] << i;
    }
    return index;
}

/// Byte offset of block (x, y) of slice z in a micro-tiled mip whose pitch and height are in
/// blocks: ComputeSurfaceAddrFromCoordMicroTiled of host_shaders/tiling.comp, one sample.
inline uint64_t MicroTiledOffset(uint32_t x, uint32_t y, uint32_t z, uint32_t pitch,
                                 uint32_t height, uint32_t micro_mode, uint32_t thickness,
                                 uint32_t bits) {
    const uint64_t slice_bytes = uint64_t(pitch) * height * thickness * bits / 8;
    const uint64_t micro_tile_bytes = 64ull * thickness * bits / 8;
    const uint64_t tile = uint64_t(y / 8) * (pitch / 8) + x / 8;
    return (z / thickness) * slice_bytes + tile * micro_tile_bytes +
           uint64_t(MicroTileElementIndex(x, y, z, micro_mode, thickness, bits)) * bits / 8;
}

/// One mip level of a guest image: where it starts in the guest memory and how big it is.
/// The pitch and the height are in texels (a block covers 4x4 texels).
struct MipLayout {
    uint64_t offset = 0;
    uint32_t pitch = 0;
    uint32_t height = 0;
};

/// The guest layout of a block-coded image, enough to decode all its blocks. The port's
/// upload (video_core/texture_cache/texture_cache.cpp) and the tests
/// (port/tests/test_bc_decode.cpp) use the same code, so a test says what the port does.
struct ImageLayout {
    Kind kind = Kind::Bc1;
    bool is_tiled = true;
    uint32_t micro_mode = MicroThin;
    uint32_t thickness = 1;
    uint32_t num_bits = 64; ///< the bits of one block (64 for BC1/BC4, 128 for BC2/BC3/BC5)
    uint32_t layers = 1;
    uint32_t width = 0;  ///< level 0 width in texels
    uint32_t height = 0; ///< level 0 height in texels
    uint32_t levels = 1;
    MipLayout mips[16]{};
};

/// The size of one level, in texels.
inline void LevelExtent(const ImageLayout& layout, uint32_t level, uint32_t& width,
                        uint32_t& height) {
    width = std::max(layout.width >> level, 1u);
    height = std::max(layout.height >> level, 1u);
}

/// The bytes the decoded texels of one level take (the layers one after another).
inline uint64_t LevelTexelBytes(const ImageLayout& layout, uint32_t level) {
    uint32_t width = 0, height = 0;
    LevelExtent(layout, level, width, height);
    return uint64_t(width) * height * layout.layers * TexelBytes(layout.kind);
}

/// Decodes one level of a guest image into `out` (the layers one after another, each with
/// its rows of texels, row-major). The blocks are read from the guest memory the way the
/// GPU's detiler lays them out (micro tiling, or linear for a linear surface), and a
/// block that falls outside the guest memory decodes as zeroes.
inline void DecodeLevel(const ImageLayout& layout, uint32_t level, const uint8_t* guest,
                        size_t guest_size, uint8_t* out) {
    const uint32_t block_bytes = BlockBytes(layout.kind);
    const uint32_t texel_bytes = TexelBytes(layout.kind);
    uint32_t width = 0, height = 0;
    LevelExtent(layout, level, width, height);
    const MipLayout& mip = layout.mips[level];
    const uint32_t pitch_blocks = std::max((mip.pitch + 3) / 4, 1u);
    const uint32_t height_blocks = std::max((mip.height + 3) / 4, 1u);
    uint8_t texels[16 * 4];
    for (uint32_t layer = 0; layer < layout.layers; ++layer) {
        for (uint32_t by = 0; by < (height + 3) / 4; ++by) {
            for (uint32_t bx = 0; bx < (width + 3) / 4; ++bx) {
                const uint64_t offset =
                    mip.offset +
                    (layout.is_tiled
                         ? MicroTiledOffset(bx, by, layer, pitch_blocks, height_blocks,
                                            layout.micro_mode, layout.thickness, layout.num_bits)
                         : ((uint64_t(layer) * height_blocks + by) * pitch_blocks + bx) *
                               block_bytes);
                if (offset + block_bytes <= guest_size) {
                    DecodeBlock(layout.kind, guest + offset, texels);
                } else {
                    std::memset(texels, 0, sizeof(texels));
                }
                for (uint32_t ty = 0; ty < 4 && by * 4 + ty < height; ++ty) {
                    const uint32_t y = by * 4 + ty;
                    const uint32_t x = bx * 4;
                    const uint32_t count = std::min(4u, width - x);
                    std::memcpy(out + ((uint64_t(layer) * height + y) * width + x) * texel_bytes,
                                texels + ty * 4 * texel_bytes, count * texel_bytes);
                }
            }
        }
    }
}

} // namespace VideoCore::BcDecode
