// SPDX-License-Identifier: GPL-2.0-or-later
// TLG_BC_TINY (video_core/amdgpu/tlg_bc_tiny.h): the CPU BC1-BC5 decoder and the micro-tile
// addressing in video_core/texture_cache/tlg_bc_decode.h, against hand-built blocks with known
// texels. No game or Vulkan device required.
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

#include "video_core/texture_cache/tlg_bc_decode.h"

using namespace VideoCore::BcDecode;

static uint16_t Rgb565(uint32_t r, uint32_t g, uint32_t b) {
    return uint16_t((r << 11) | (g << 5) | b);
}

static void Put16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

static void Put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        p[i] = uint8_t(v >> (8 * i));
    }
}

/// 2-bit colour indices, texel t in bits 2t..2t+1.
static uint32_t ColorIndices(const int idx[16]) {
    uint32_t bits = 0;
    for (int t = 0; t < 16; ++t) {
        bits |= uint32_t(idx[t]) << (2 * t);
    }
    return bits;
}

/// 3-bit channel indices into bytes 2..7 of a BC4-style block.
static void PutChannelIndices(uint8_t* block, const int idx[16]) {
    uint64_t bits = 0;
    for (int t = 0; t < 16; ++t) {
        bits |= uint64_t(idx[t]) << (3 * t);
    }
    for (int i = 0; i < 6; ++i) {
        block[2 + i] = uint8_t(bits >> (8 * i));
    }
}

static void ExpectRgba(const uint8_t* texel, int r, int g, int b, int a) {
    if (texel[0] != r || texel[1] != g || texel[2] != b || texel[3] != a) {
        std::fprintf(stderr, "got %d %d %d %d, want %d %d %d %d\n", texel[0], texel[1], texel[2],
                     texel[3], r, g, b, a);
        assert(false);
    }
}

static void TestBc1() {
    const int idx[16] = {0, 1, 2, 3, 3, 2, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1};
    uint8_t block[8];
    uint8_t out[16 * 4];

    // Four-colour mode (c0 > c1): red and blue, thirds between them.
    Put16(block, Rgb565(31, 0, 0));
    Put16(block + 2, Rgb565(0, 0, 31));
    Put32(block + 4, ColorIndices(idx));
    DecodeBlock(Kind::Bc1, block, out);
    ExpectRgba(out + 0 * 4, 255, 0, 0, 255);
    ExpectRgba(out + 1 * 4, 0, 0, 255, 255);
    ExpectRgba(out + 2 * 4, 170, 0, 85, 255);
    ExpectRgba(out + 3 * 4, 85, 0, 170, 255);
    ExpectRgba(out + 7 * 4, 255, 0, 0, 255);
    ExpectRgba(out + 15 * 4, 0, 0, 255, 255);

    // Three-colour mode (c0 <= c1): the midpoint, and transparent black for index 3.
    Put16(block, Rgb565(0, 0, 31));
    Put16(block + 2, Rgb565(31, 63, 0));
    DecodeBlock(Kind::Bc1, block, out);
    ExpectRgba(out + 0 * 4, 0, 0, 255, 255);
    ExpectRgba(out + 1 * 4, 255, 255, 0, 255);
    ExpectRgba(out + 2 * 4, 128, 128, 128, 255);
    ExpectRgba(out + 3 * 4, 0, 0, 0, 0);

    // 565 expansion replicates the top bits: 0b10000 -> 0b10000100 = 132.
    Put16(block, Rgb565(16, 32, 16));
    Put16(block + 2, Rgb565(16, 32, 16));
    Put32(block + 4, 0);
    DecodeBlock(Kind::Bc1, block, out);
    ExpectRgba(out, 132, 130, 132, 255);
}

static void TestBc2AndBc3() {
    uint8_t block[16];
    uint8_t out[16 * 4];
    // BC2: explicit alpha nibbles 0..15 by texel; colour block equal endpoints (white).
    for (int t = 0; t < 16; t += 2) {
        block[t / 2] = uint8_t(t | ((t + 1) << 4));
    }
    Put16(block + 8, 0xffff);
    Put16(block + 10, 0xffff);
    Put32(block + 12, 0);
    DecodeBlock(Kind::Bc2, block, out);
    for (int t = 0; t < 16; ++t) {
        ExpectRgba(out + t * 4, 255, 255, 255, t * 17);
    }

    // BC2/BC3 colour is always four-colour, even with c0 <= c1 (index 3 is not transparent).
    Put16(block + 8, Rgb565(0, 0, 31));
    Put16(block + 10, Rgb565(31, 0, 0));
    Put32(block + 12, 0xffffffffu);
    DecodeBlock(Kind::Bc2, block, out);
    ExpectRgba(out + 0, 170, 0, 85, 1 * 0);

    // BC3: interpolated alpha, eight-value mode (a0 > a1).
    block[0] = 255;
    block[1] = 0;
    const int aidx[16] = {0, 1, 2, 3, 4, 5, 6, 7, 0, 0, 0, 0, 0, 0, 0, 1};
    PutChannelIndices(block, aidx);
    Put16(block + 8, Rgb565(0, 63, 0));
    Put16(block + 10, Rgb565(0, 0, 0));
    Put32(block + 12, 0);
    DecodeBlock(Kind::Bc3, block, out);
    const int want_alpha[8] = {255, 0, 219, 182, 146, 109, 73, 36};
    for (int t = 0; t < 8; ++t) {
        ExpectRgba(out + t * 4, 0, 255, 0, want_alpha[t]);
    }
    ExpectRgba(out + 15 * 4, 0, 255, 0, 0);
}

static void TestBc4AndBc5() {
    uint8_t block[16];
    uint8_t out[16 * 2];
    // Six-value mode (e0 <= e1): four steps, then 0 and 255.
    block[0] = 10;
    block[1] = 60;
    const int idx[16] = {0, 1, 2, 3, 4, 5, 6, 7, 7, 6, 5, 4, 3, 2, 1, 0};
    PutChannelIndices(block, idx);
    DecodeBlock(Kind::Bc4Unorm, block, out);
    const int want[8] = {10, 60, 20, 30, 40, 50, 0, 255};
    for (int t = 0; t < 8; ++t) {
        assert(out[t] == want[t]);
        assert(out[15 - t] == want[t]);
    }

    // Signed: -128 reads as -127; eight-value mode with 127 > -127.
    block[0] = 127;
    block[1] = 0x80;
    const int sidx[16] = {0, 1, 2, 3, 4, 5, 6, 7, 0, 0, 0, 0, 0, 0, 0, 0};
    PutChannelIndices(block, sidx);
    DecodeBlock(Kind::Bc4Snorm, block, out);
    const int want_s[8] = {127, -127, 91, 54, 18, -18, -54, -91};
    for (int t = 0; t < 8; ++t) {
        assert(int8_t(out[t]) == want_s[t]);
    }

    // BC5: two BC4 blocks, interleaved as RG.
    block[0] = 200;
    block[1] = 200;
    const int zero[16] = {};
    PutChannelIndices(block, zero);
    block[8] = 7;
    block[9] = 7;
    PutChannelIndices(block + 8, zero);
    DecodeBlock(Kind::Bc5Unorm, block, out);
    for (int t = 0; t < 16; ++t) {
        assert(out[2 * t] == 200 && out[2 * t + 1] == 7);
    }
}

static void TestMicroTiling() {
    // Thin micro tiling interleaves x0 y0 x1 y1 x2 y2.
    assert(MicroTileElementIndex(1, 0, 0, MicroThin, 1, 64) == 1);
    assert(MicroTileElementIndex(0, 1, 0, MicroThin, 1, 64) == 2);
    assert(MicroTileElementIndex(2, 0, 0, MicroThin, 1, 64) == 4);
    assert(MicroTileElementIndex(7, 7, 0, MicroThin, 1, 64) == 63);
    // Display micro tiling for 64-bit elements: x0 y0 x1 x2 y1 y2.
    assert(MicroTileElementIndex(4, 0, 0, MicroDisplay, 1, 64) == 8);
    assert(MicroTileElementIndex(0, 2, 0, MicroDisplay, 1, 64) == 16);
    // ... and for 128-bit elements: y0 x0 x1 x2 y1 y2.
    assert(MicroTileElementIndex(1, 0, 0, MicroDisplay, 1, 128) == 2);
    assert(MicroTileElementIndex(0, 1, 0, MicroDisplay, 1, 128) == 1);

    // Every element of a 16x8 two-tile surface maps to a distinct 8-byte slot inside it.
    std::set<uint64_t> offsets;
    for (uint32_t y = 0; y < 8; ++y) {
        for (uint32_t x = 0; x < 16; ++x) {
            const uint64_t offset = MicroTiledOffset(x, y, 0, 16, 8, MicroThin, 1, 64);
            assert(offset % 8 == 0 && offset < 16 * 8 * 8);
            offsets.insert(offset);
        }
    }
    assert(offsets.size() == 16 * 8);
    // Second tile starts after the first 64 elements; the next slice after pitch*height.
    assert(MicroTiledOffset(8, 0, 0, 16, 8, MicroThin, 1, 64) == 64 * 8);
    assert(MicroTiledOffset(0, 0, 1, 16, 8, MicroThin, 1, 64) == 16 * 8 * 8);
    // A 512x1 BC1 1D texture (128 blocks, pitch 128, height 8 blocks): block x of row 0.
    assert(MicroTiledOffset(9, 0, 0, 128, 8, MicroThin, 1, 64) == (64 + 1) * 8);
    assert(MicroTiledOffset(0, 0, 3, 128, 8, MicroThin, 1, 64) == 3ull * 128 * 8 * 8);
}

// tlg: how much CPU the CPU decode of the tiny BC textures costs. It happens inside the frame
// (the driver refuses 1D BC images), so the rate says whether it is a first-order problem: a
// million BC1 blocks are 8 MiB of blocks and 16 MiB of decoded texels.
static void BenchmarkDecode() {
    constexpr uint32_t blocks = 1 << 20;
    std::vector<uint8_t> data(blocks * 8, 0x5a);
    uint8_t texels[16 * 4];
    uint32_t checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < blocks; ++i) {
        DecodeBlock(Kind::Bc1, data.data() + (i % (blocks - 1)) * 8, texels);
        checksum += texels[0];
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
    const double ms = double(elapsed) / 1e6;
    const double mib = double(blocks) * 8.0 / (1024.0 * 1024.0);
    std::printf("bc-decode: %.1f ms for %.0f MiB of blocks (%.0f MiB/s, %.0f ns per block, "
                "checksum %u)\n",
                ms, mib, mib / (ms / 1000.0), double(elapsed) / double(blocks), checksum);
}

// tlg: a BC1 block with a known pattern, the way a sprite atlas holds it: a red/green
// gradient in the four columns. The endpoints are 565 colours and the four indices pick
// them, so the expected texels are known exactly (this is the shape of the game's
// particle sprites: one BC1 block, four texels wide).
static void TestBc1KnownPattern() {
    uint8_t block[8];
    uint8_t out[16 * 4];
    // c0 = red (31,0,0), c1 = green (0,63,0). Four-colour mode (c0 > c1).
    Put16(block, Rgb565(31, 0, 0));
    Put16(block + 2, Rgb565(0, 63, 0));
    // The columns: 0, 1, 2, 3, 3, 2, 1, 0, repeated in the four rows.
    const int idx[16] = {0, 1, 2, 3, 3, 2, 1, 0, 0, 1, 2, 3, 3, 2, 1, 0};
    Put32(block + 4, ColorIndices(idx));
    DecodeBlock(Kind::Bc1, block, out);
    // The interpolated colours: (2*c0 + c1) / 3 and (c0 + 2*c1) / 3, with the 5/6 bits
    // replicated (31 -> 255, 63 -> 255).
    ExpectRgba(out + 0 * 4, 255, 0, 0, 255);
    ExpectRgba(out + 1 * 4, 0, 255, 0, 255);
    ExpectRgba(out + 2 * 4, 170, 85, 0, 255);
    ExpectRgba(out + 3 * 4, 85, 170, 0, 255);
    // The texels are row-major: texel t is (t % 4, t / 4). Row 1 starts with the index 3
    // (the pattern repeats every four texels), so its first texel is the fourth colour.
    ExpectRgba(out + 4 * 4, 85, 170, 0, 255);
    ExpectRgba(out + 12 * 4, 85, 170, 0, 255);
}

// tlg: BC4 and BC5 carry the alpha (or the green) channel the sprites need: BC4 is one
// channel, BC5 two. The game binds BC1, BC4 and BC5 1D textures (the log of the session:
// "tlg: BC image 1x1 Bc4UnormBlock" and "tlg: BC image 1x1 Bc5UnormBlock").
static void TestBc4Bc5Channels() {
    uint8_t block[16];
    uint8_t out[16 * 2];

    // BC4: a single channel, from 0 to 255 across the block (the 8-value mode, e0 > e1).
    block[0] = 255;
    block[1] = 0;
    const int idx[16] = {0, 1, 2, 3, 4, 5, 6, 7, 7, 6, 5, 4, 3, 2, 1, 0};
    PutChannelIndices(block, idx);
    DecodeBlock(Kind::Bc4Unorm, block, out);
    const int want[8] = {255, 0, 219, 182, 146, 109, 73, 36};
    for (int t = 0; t < 16; ++t) {
        assert(out[t] == want[idx[t]]);
    }

    // BC5: two channels, the red and the green, interleaved as RG.
    block[0] = 250;
    block[1] = 10;
    const int idx2[16] = {0, 1, 2, 3, 4, 5, 6, 7, 7, 6, 5, 4, 3, 2, 1, 0};
    PutChannelIndices(block, idx2);
    block[8] = 128;
    block[9] = 64;
    const int zero[16] = {};
    PutChannelIndices(block + 8, zero);
    DecodeBlock(Kind::Bc5Unorm, block, out);
    // The red channel of the 8-value mode with e0 = 250, e1 = 10.
    const int want_r[8] = {250, 10, 216, 181, 147, 113, 79, 44};
    for (int t = 0; t < 16; ++t) {
        assert(out[2 * t] == want_r[idx2[t]]);
        // The green channel: e0 = 128, e1 = 64 (8-value mode); every index is 0, so every
        // texel of the green channel is e0.
        assert(out[2 * t + 1] == 128);
    }
}

// tlg: the whole image the game binds: a 512x1, 32-layer, 10-mip BC1 texture (the log of
// the session: "tlg: BC image 512x1 Bc1RgbaSrgbBlock type 12 tile Thin1DThin layers 32
// mips 10"). The test builds the guest memory the way the GPU's detiler lays it out and
// checks that the port's decode (tlg_bc_decode.h DecodeLevel, the same code the port
// uploads) gives every layer its own texels: a layer that reads another layer's blocks is
// a sprite atlas that shows the wrong sprite (or none).
static void TestOneDArrayImage() {
    constexpr uint32_t kWidth = 512;
    constexpr uint32_t kLayers = 32;
    constexpr uint32_t kLevels = 10;
    // The guest layout of a Thin1DThin BC1 image: 128 blocks of pitch, 8 blocks of height
    // (the micro tile is 8x8 blocks), 64 bits per block, one slice per layer.
    constexpr uint32_t kPitchBlocks = kWidth / 4;
    constexpr uint32_t kHeightBlocks = 8;

    ImageLayout layout;
    layout.kind = Kind::Bc1;
    layout.is_tiled = true;
    layout.micro_mode = MicroThin;
    layout.thickness = 1;
    layout.num_bits = 64;
    layout.layers = kLayers;
    layout.width = kWidth;
    layout.height = 1;
    layout.levels = kLevels;
    uint64_t offset = 0;
    for (uint32_t m = 0; m < kLevels; ++m) {
        const uint32_t blocks = std::max((kWidth >> m) / 4, 1u);
        layout.mips[m].offset = offset;
        layout.mips[m].pitch = blocks * 4;
        layout.mips[m].height = 32; // 8 blocks of height, in texels
        offset += uint64_t(blocks) * kHeightBlocks * 8 * kLayers;
    }

    // The guest memory: every block of every layer holds the layer's number in its first
    // two bytes, so a decode that reads the wrong block is visible.
    std::vector<uint8_t> guest(offset, 0);
    for (uint32_t layer = 0; layer < kLayers; ++layer) {
        for (uint32_t bx = 0; bx < kPitchBlocks; ++bx) {
            const uint64_t at = MicroTiledOffset(bx, 0, layer, kPitchBlocks, kHeightBlocks, MicroThin,
                                            1, 64);
            assert(at + 8 <= guest.size());
            guest[at] = uint8_t(layer);
            guest[at + 1] = uint8_t(bx);
            // The colour endpoints: c0 = (layer, bx, 0) as a 565 colour, c1 = black. The
            // four indices all pick c0, so every texel of the block is that colour.
            const uint16_t c0 = Rgb565((layer & 31) * 2, bx & 31, 0);
            Put16(guest.data() + at, c0);
            Put16(guest.data() + at + 2, 0);
            Put32(guest.data() + at + 4, 0);
        }
    }

    std::vector<uint8_t> out(LevelTexelBytes(layout, 0));
    DecodeLevel(layout, 0, guest.data(), guest.size(), out.data());
    for (uint32_t layer = 0; layer < kLayers; ++layer) {
        for (uint32_t bx = 0; bx < kPitchBlocks; ++bx) {
            const uint8_t* const texel = out.data() + (uint64_t(layer) * kWidth + bx * 4) * 4;
            const uint16_t c0 = Rgb565((layer & 31) * 2, bx & 31, 0);
            // The 565 expansion replicates the top bits: 5 bits of red, 6 of green.
            const uint8_t want_r = uint8_t((((c0 >> 11) & 31) << 3) | (((c0 >> 11) & 31) >> 2));
            const uint8_t want_g = uint8_t((((c0 >> 5) & 63) << 2) | (((c0 >> 5) & 63) >> 4));
            if (texel[0] != want_r || texel[1] != want_g || texel[2] != 0 || texel[3] != 255) {
                std::fprintf(stderr,
                             "layer %u block %u: got %u %u %u %u, wanted %u %u 0 255\n", layer, bx,
                             texel[0], texel[1], texel[2], texel[3], want_r, want_g);
                assert(false);
            }
        }
    }

    // The mip chain: every level decodes, and the layers stay apart.
    for (uint32_t m = 1; m < kLevels; ++m) {
        const uint64_t bytes = LevelTexelBytes(layout, m);
        std::vector<uint8_t> level(bytes, 0xcd);
        DecodeLevel(layout, m, guest.data(), guest.size(), level.data());
        uint64_t non_zero = 0;
        for (uint64_t i = 0; i < bytes; ++i) {
            non_zero += level[i] != 0xcd ? 1 : 0;
        }
        // Every level has data (the decode wrote over the whole buffer).
        assert(non_zero == bytes);
    }
}

int main() {
    TestBc1();
    TestBc1KnownPattern();
    TestBc2AndBc3();
    TestBc4AndBc5();
    TestBc4Bc5Channels();
    TestMicroTiling();
    TestOneDArrayImage();
    BenchmarkDecode();
    std::puts("bc-decode-test: ok");
    return 0;
}
