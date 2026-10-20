// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: the binding state of the command buffer (vk_bind_state.h): when a repeated bind can
// be left out.
//
// Vulkan keeps the vertex buffers and the index buffer bound until something else binds
// them: binding the same ones again changes nothing. The driver records the call anyway, and
// the port records one per draw. This test proves when the bind is redundant and when it is
// not, so the port can leave the redundant ones out without breaking the geometry.
//
// No game and no Vulkan device: the state is a table of handles.
#include <cassert>
#include <cstdio>

#include "video_core/renderer_vulkan/vk_bind_state.h"

using Vulkan::BindState;

namespace {

constexpr u64 Generation = 7; ///< the command buffer the state belongs to

void TestSameStateIsSkipped() {
    BindState state;
    const u64 handles[2] = {0xAAAA, 0xBBBB};
    const u64 offsets[2] = {0, 256};
    const u64 sizes[2] = {256, 512};
    const u64 strides[2] = {16, 32};

    // Nothing is bound yet: the first bind is needed.
    assert(!state.HasVertexBuffers(Generation, 2, handles, offsets, sizes, strides, true));
    state.SetVertexBuffers(Generation, 2, handles, offsets, sizes, strides, true);
    // The same bind: it would change nothing.
    assert(state.HasVertexBuffers(Generation, 2, handles, offsets, sizes, strides, true));
    // Another offset: the bind is needed.
    const u64 other_offsets[2] = {0, 512};
    assert(!state.HasVertexBuffers(Generation, 2, handles, other_offsets, sizes, strides, true));
    // Another size or stride: the bind is needed.
    const u64 other_sizes[2] = {256, 1024};
    assert(!state.HasVertexBuffers(Generation, 2, handles, offsets, other_sizes, strides, true));
    const u64 other_strides[2] = {16, 64};
    assert(!state.HasVertexBuffers(Generation, 2, handles, offsets, sizes, other_strides, true));
    // Another buffer: the bind is needed.
    const u64 other_handles[2] = {0xAAAA, 0xCCCC};
    assert(!state.HasVertexBuffers(Generation, 2, other_handles, offsets, sizes, strides, true));
    // Fewer buffers: the bind is needed (the state of the others is not the same).
    assert(!state.HasVertexBuffers(Generation, 1, handles, offsets, sizes, strides, true));
}

void TestAnotherCommandBufferIsNotSkipped() {
    BindState state;
    const u64 handles[1] = {0xAAAA};
    const u64 offsets[1] = {0};
    const u64 sizes[1] = {256};
    const u64 strides[1] = {16};
    state.SetVertexBuffers(Generation, 1, handles, offsets, sizes, strides, true);
    assert(state.HasVertexBuffers(Generation, 1, handles, offsets, sizes, strides, true));
    // A new command buffer has nothing bound: the bind is needed again.
    assert(!state.HasVertexBuffers(Generation + 1, 1, handles, offsets, sizes, strides, true));
    // And the state of the new command buffer is the new one.
    state.SetVertexBuffers(Generation + 1, 1, handles, offsets, sizes, strides, true);
    assert(state.HasVertexBuffers(Generation + 1, 1, handles, offsets, sizes, strides, true));
    assert(!state.HasVertexBuffers(Generation, 1, handles, offsets, sizes, strides, true));
}

void TestIndexBuffer() {
    BindState state;
    assert(!state.HasIndexBuffer(Generation, 0xAAAA, 0, 16));
    state.SetIndexBuffer(Generation, 0xAAAA, 0, 16);
    assert(state.HasIndexBuffer(Generation, 0xAAAA, 0, 16));
    // Another buffer, offset or type: the bind is needed.
    assert(!state.HasIndexBuffer(Generation, 0xBBBB, 0, 16));
    assert(!state.HasIndexBuffer(Generation, 0xAAAA, 64, 16));
    assert(!state.HasIndexBuffer(Generation, 0xAAAA, 0, 32));
    // Another command buffer: the bind is needed.
    assert(!state.HasIndexBuffer(Generation + 1, 0xAAAA, 0, 16));
}

void TestSizesFlag() {
    BindState state;
    const u64 handles[1] = {0xAAAA};
    const u64 offsets[1] = {0};
    const u64 sizes[1] = {256};
    const u64 strides[1] = {16};
    // The dynamic vertex input: the bind does not set the sizes and the strides.
    state.SetVertexBuffers(Generation, 1, handles, offsets, sizes, strides, false);
    assert(state.HasVertexBuffers(Generation, 1, handles, offsets, sizes, strides, false));
    // The other kind of bind is a different state.
    assert(!state.HasVertexBuffers(Generation, 1, handles, offsets, sizes, strides, true));
}

void TestInvalidate() {
    BindState state;
    const u64 handles[1] = {0xAAAA};
    const u64 offsets[1] = {0};
    const u64 sizes[1] = {256};
    const u64 strides[1] = {16};
    state.SetVertexBuffers(Generation, 1, handles, offsets, sizes, strides, true);
    assert(state.HasVertexBuffers(Generation, 1, handles, offsets, sizes, strides, true));
    // A command recorded outside the recording thread: the state is not known any more.
    state.Invalidate();
    assert(!state.HasVertexBuffers(Generation, 1, handles, offsets, sizes, strides, true));
}

} // namespace

int main() {
    TestSameStateIsSkipped();
    TestAnotherCommandBufferIsNotSkipped();
    TestIndexBuffer();
    TestSizesFlag();
    TestInvalidate();
    std::printf("bind state test: ok\n");
    return 0;
}
