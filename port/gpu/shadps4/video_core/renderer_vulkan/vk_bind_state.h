// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: the binding state of the command buffer being recorded.
//
// Vulkan keeps the vertex buffers and the index buffer bound until something else binds
// them, and binding the same ones again is a no-op. The driver records the call anyway
// (and the port records one per draw). This is the state of the command buffer: with it,
// the repeated binds can be left out.
//
// The state belongs to the command buffer: a new one starts with nothing bound. The
// generation tells them apart, and it is read when the command is *recorded* (the command
// goes to the command buffer of that generation).
//
// Only the commands recorded for the recording thread (the deferred ones) touch this state:
// they all run on that thread, in order, so the state always describes the command buffer
// they are going into. The commands recorded directly (the copies and the readbacks) do
// not change the vertex bindings, so they do not invalidate it.

#pragma once

#include <cstdint>
#include <cstring>

#include "common/types.h"

namespace Vulkan {

class BindState {
public:
    static constexpr u32 MaxBuffers = 32;

    /// The state is not known (a command was recorded directly, outside the recording
    /// thread): nothing can be skipped until the next bind.
    void Invalidate() {
        generation_ = 0;
    }

    /// True when the command buffer of `generation` already has these vertex buffers bound
    /// (the bind would change nothing).
    /// `with_sizes`: the bind also set the sizes and the strides (the vertex input state
    /// holds them when it does not).
    [[nodiscard]] bool HasVertexBuffers(u64 generation, u32 count, const u64* handles,
                                       const u64* offsets, const u64* sizes,
                                       const u64* strides, bool with_sizes) const {
        if (generation != generation_ || count != vertex_count_ || with_sizes != has_sizes_) {
            return false;
        }
        if (std::memcmp(vertex_handles_, handles, count * sizeof(u64)) != 0 ||
            std::memcmp(vertex_offsets_, offsets, count * sizeof(u64)) != 0) {
            return false;
        }
        if (!with_sizes) {
            return true;
        }
        return std::memcmp(vertex_sizes_, sizes, count * sizeof(u64)) == 0 &&
               std::memcmp(vertex_strides_, strides, count * sizeof(u64)) == 0;
    }

    /// Records the vertex buffers of the command buffer of `generation`.
    void SetVertexBuffers(u64 generation, u32 count, const u64* handles, const u64* offsets,
                          const u64* sizes, const u64* strides, bool with_sizes) {
        generation_ = generation;
        vertex_count_ = count;
        has_sizes_ = with_sizes;
        std::memcpy(vertex_handles_, handles, count * sizeof(u64));
        std::memcpy(vertex_offsets_, offsets, count * sizeof(u64));
        std::memcpy(vertex_sizes_, sizes, count * sizeof(u64));
        std::memcpy(vertex_strides_, strides, count * sizeof(u64));
    }

    /// True when the command buffer of `generation` already has this index buffer bound.
    [[nodiscard]] bool HasIndexBuffer(u64 generation, u64 handle, u64 offset,
                                      u32 type) const {
        return generation == generation_ && index_bound_ && handle == index_handle_ &&
               offset == index_offset_ && type == index_type_;
    }

    /// Records the index buffer of the command buffer of `generation`.
    void SetIndexBuffer(u64 generation, u64 handle, u64 offset, u32 type) {
        generation_ = generation;
        index_bound_ = true;
        index_handle_ = handle;
        index_offset_ = offset;
        index_type_ = type;
    }

private:
    /// The command buffer the state describes (0: none).
    u64 generation_{0};
    u32 vertex_count_{};
    bool has_sizes_{true};
    u64 vertex_handles_[MaxBuffers]{};
    u64 vertex_offsets_[MaxBuffers]{};
    u64 vertex_sizes_[MaxBuffers]{};
    u64 vertex_strides_[MaxBuffers]{};
    bool index_bound_{};
    u64 index_handle_{};
    u64 index_offset_{};
    u32 index_type_{};
};

} // namespace Vulkan
