// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: the key of the render target memo (vk_rasterizer.cpp): the raw register bytes a
// target's description is built from.
//
// The key is written and read only up to its size, so its buffer is reused between the
// lookups and never cleared: the previous version built it in a local array that was zeroed
// (256 bytes) and copied whole (256 bytes) for every render target of every draw, which is
// nine times per draw in a G-buffer pass.
//
// The hash is the same as the one of the zeroed buffer (the bytes past the key are zeros),
// so the memo table keeps its distribution.
//
// tlg: the buffer is reused, so the key must be reset before it is built again: a key that
// kept growing wrote past the buffer (into the rasterizer's members) and left a corrupted
// image id behind, which crashed in PrepareRenderState.

#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>

#include "common/types.h"

namespace Vulkan {

class TargetKey {
public:
    static constexpr size_t Capacity = 256;

    /// Starts a new key: the bytes of the previous one are forgotten.
    void Reset() noexcept {
        size_ = 0;
    }

    /// Appends the bytes of `part` to the key.
    template <typename T>
    void Append(const T& part) {
        static_assert(sizeof(T) <= Capacity, "target key part too large");
        // tlg: the key must fit: a caller that forgets Reset() would write past the buffer.
        // (The renderer resets it, and the parts of a key are bounded by a static_assert.)
        assert(size_ + sizeof(T) <= Capacity);
        std::memcpy(buffer_.data() + size_, &part, sizeof(part));
        size_ += sizeof(part);
    }

    [[nodiscard]] const u8* data() const noexcept {
        return buffer_.data();
    }
    [[nodiscard]] u32 size() const noexcept {
        return size_;
    }

    /// The hash of the key. The bytes past the size are read as zeros, never as garbage.
    [[nodiscard]] u64 Hash() const noexcept {
        u64 hash = 0xCBF29CE484222325ull;
        u32 i = 0;
        for (; i + sizeof(u64) <= size_; i += sizeof(u64)) {
            u64 word;
            std::memcpy(&word, buffer_.data() + i, sizeof(word));
            hash = (hash ^ word) * 0x100000001B3ull;
            hash ^= hash >> 29;
        }
        if (i < size_) {
            u64 word = 0;
            std::memcpy(&word, buffer_.data() + i, size_ - i);
            hash = (hash ^ word) * 0x100000001B3ull;
            hash ^= hash >> 29;
        }
        return hash;
    }

    /// True when the first size() bytes of `other` are the same and its size matches.
    [[nodiscard]] bool Equals(const u8* other, u32 other_size) const noexcept {
        return other_size == size_ && std::memcmp(buffer_.data(), other, size_) == 0;
    }

    /// Copies the key into a memo entry. The whole buffer is copied (a fixed size, so the
    /// compiler inlines it): the bytes past the key are never compared, and copying them
    /// costs less than a call to memcpy with a size it cannot know.
    void CopyTo(u8* destination) const noexcept {
        std::memcpy(destination, buffer_.data(), Capacity);
    }

private:
    std::array<u8, Capacity> buffer_{};
    u32 size_{};
};

} // namespace Vulkan
