// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include "video_core/renderer_vulkan/scene_resolution.h"
#include "video_core/texture_cache/image.h"

namespace VideoCore { class TextureCache; }
namespace Vulkan {
class Instance;
class Runtime;
class Scheduler;

/// Why a guest image cannot have a reduced-size proxy. The counters of the frame statistics
/// add them up: an image that is not eligible is the reason its pass is not reduced.
enum class ProxySkip : u32 {
    None = 0,
    Size,    ///< the size is not a view of the guest's frame (1920x1080) or the switch of
             ///< the smaller targets is off
    Samples, ///< multisampled
    Layers,  ///< an array or an array-layered image
    Volume,  ///< 3D or block-compressed
    Mips,    ///< a mip chain the proxy cannot mirror
    Format,  ///< the format cannot be blitted nor resampled by a shader
    Count,
};

/// The name of a reason, for the frame statistics.
inline const char* ProxySkipName(ProxySkip skip) {
    switch (skip) {
    case ProxySkip::Size: return "size";
    case ProxySkip::Samples: return "samples";
    case ProxySkip::Layers: return "layers";
    case ProxySkip::Volume: return "volume";
    case ProxySkip::Mips: return "mips";
    case ProxySkip::Format: return "format";
    default: return "none";
    }
}

// Reduced raster targets, with native-size images retained for unmodified guest compute,
// integer texture loads, copies and CPU readbacks. All native access passes Runtime::Transit.
class SceneTargets {
public:
    SceneTargets(const Instance&, Scheduler&, Runtime&, VideoCore::TextureCache&);
    using Lookup = std::function<VideoCore::Image*(VideoCore::ImageId, u64)>;
    SceneTargets(const Instance&, Scheduler&, Runtime&, Lookup);
    ~SceneTargets();
    bool SetSize(SceneResolution::Size);
    SceneResolution::Size Size() const { return size; }
    bool Reduced() const { return size != SceneResolution::Size{}; }
    /// A reduced proxy can stand in for the image: a 1920x1080 scene target or, scaled by
    /// the same factor, a smaller one (960x540, 480x270...; toggle SceneHalfRes).
    bool Eligible(const VideoCore::Image&) const;
    /// Whether the size of an image is a view of the guest's frame (the same aspect ratio
    /// of 1920x1080): the targets of the scene scale with it, the shadow maps and the
    /// textures do not.
    static bool ScalesWithTheScene(const VideoCore::ImageInfo&);
    /// The proxy size of an image whose size scales with the guest's frame: its size with
    /// the scene's factor (the scene's size over the guest's frame), of the mip level
    /// (level 0 without one). The size zero when the image does not scale with it.
    static SceneResolution::Size ProxySizeFor(const VideoCore::ImageInfo&, SceneResolution::Size,
                                              u32 level = 0);
    /// Eligible at the full scene size (1920x1080): the upscaler's inputs, the scene start.
    bool EligibleScene(const VideoCore::Image&) const;
    /// The proxy size of an eligible image: its size with the scene's factor.
    SceneResolution::Size ProxySize(const VideoCore::Image&, u32 level = 0) const;
    struct Target {
        vk::Image image;
        vk::ImageView view;
        vk::ImageLayout layout;
        vk::ImageUsageFlags usage;
    };
    Target Attachment(VideoCore::ImageId, const VideoCore::ImageViewInfo&);
    Target Read(VideoCore::ImageId, const VideoCore::ImageViewInfo&,
                vk::PipelineStageFlags2 = vk::PipelineStageFlagBits2::eComputeShader,
                vk::AccessFlags2 = vk::AccessFlagBits2::eShaderRead);
    void NativeAccess(VideoCore::Image&, vk::AccessFlags2);
    /// The proxy of `image` for sampling when it holds the current content (else nullopt: the
    /// native image is current). Transitions only after other use (attachment, copy).
    /// Mipmapped targets have one proxy per level: views of one level only.
    std::optional<Target> SampleProxy(const VideoCore::Image& image,
                                      const VideoCore::ImageViewInfo& info);
    /// Whether SampleProxy would return the proxy of `image` for these levels.
    [[nodiscard]] bool ProxyCurrent(const VideoCore::Image& image, u32 level = 0,
                                    u32 levels = 1) const;
    /// SampleProxy for a view obtained before (valid while Generation() is unchanged): the
    /// layout to sample it in.
    vk::ImageLayout PrepareSample(const VideoCore::Image& image, u32 level = 0);
    /// Changes when proxies and their views are destroyed (SetSize).
    [[nodiscard]] u64 Generation() const noexcept {
        return generation;
    }
    /// Whether NativeAccess has work for this image (a reduced-size proxy exists).
    [[nodiscard]] bool Tracks(u64 image_uid) const {
        return !copying && tracked.contains(image_uid);
    }
    void ResolveAll();

    /// What the scene offered and what the proxy could take, since the last report. The
    /// numbers answer why a pass is not reduced: an attachment that is not eligible (with
    /// its reason), a proxy size that changes between attachments, and the life of the
    /// proxies (created, filled, resolved, sampled, drawn into).
    struct Stats {
        u64 eligible = 0;    ///< attachment checks that passed
        std::array<u64, u32(ProxySkip::Count)> skipped{}; ///< by reason
        u64 size_changes = 0; ///< the proxy size changed between two checks
        u64 proxies = 0;      ///< proxy images created
        u64 fills = 0;        ///< copies from the native image into the proxy
        u64 resolves = 0;     ///< copies from the proxy into the native image
        u64 samples = 0;      ///< bindings that read the proxy
        u64 writes = 0;       ///< attachments drawn into the proxy
    };
    /// The counters since the last call, and resets them.
    Stats TakeStats();

    bool debug = false; ///< TLG_SCENE_DEBUG frame: print resolves and fills
private:
    struct Entry {
        VideoCore::ImageId source{};
        u64 uid = 0;
        u32 level = 0; ///< mip level of the native image this proxy stands for
        VideoCore::UniqueImage image;
        std::vector<std::pair<VideoCore::ImageViewInfo, vk::UniqueImageView>> views;
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
        SceneResolution::Coherence state;
    };
    static constexpr u64 Key(u64 uid, u32 level) {
        return uid << 4 | level;
    }
    Entry& Get(VideoCore::ImageId, u32 level = 0);
    vk::ImageView View(Entry&, const VideoCore::Image&, const VideoCore::ImageViewInfo&);
    void Copy(Entry&, VideoCore::Image&, bool to_native);
    /// The eligibility of an image and, when `count` is set, why it was refused.
    bool EligibleInternal(const VideoCore::Image&, bool count) const;
    /// Counts a reason (the frame statistics print them).
    void CountSkip(ProxySkip) const;
    /// Depth/stencil formats without blit support (D32S8 on RADV) are resampled by a
    /// fullscreen draw writing gl_FragDepth and, with stencil export, the stencil value.
    vk::FormatFeatureFlags Features(vk::Format) const;
    bool Blittable(vk::Format) const;
    /// Whether the format can be filtered while it is copied (colour formats only).
    bool Filterable(vk::Format) const;
    bool ShaderResampled(const VideoCore::Image&) const;
    void Resample(vk::Image src, vk::Image dst, const VideoCore::Image& original,
                  vk::Extent2D dst_size);
    vk::Pipeline ResamplePipeline(vk::Format, bool stencil);
    void CreateResampleResources();
    void Transition(Entry&, vk::ImageAspectFlags, vk::ImageLayout,
                    vk::PipelineStageFlags2, vk::AccessFlags2);
    /// Prints the counters of the window (the frame statistics) and resets them.
    void ReportStats();
    const Instance& instance;
    Scheduler& scheduler;
    Runtime& runtime;
    Lookup lookup;
    SceneResolution::Size size;
    std::unordered_map<u64, std::unique_ptr<Entry>> entries; ///< by Key(uid, level)
    std::unordered_set<u64> tracked; ///< uids with proxies
    bool copying = false;
    bool force_stencil_bits = false; ///< test the portable resampler on any driver
    u64 generation = 0;
    mutable std::unordered_map<vk::Format, vk::FormatFeatureFlags> format_features;
    mutable std::array<vk::FormatFeatureFlags, 256> format_table{};
    mutable std::array<bool, 256> format_known{};
    std::array<std::pair<u64, Entry*>, 8> recent{}; ///< last entries by Key
    u32 recent_next = 0;
    vk::UniqueShaderModule fs_tri_vert, depth_frag, depth_stencil_frag, stencil_bits_frag;
    vk::UniqueDescriptorSetLayout resample_set_layout;
    vk::UniquePipelineLayout resample_layout;
    std::vector<std::pair<std::pair<vk::Format, bool>, vk::UniquePipeline>> resample_pipelines;
    /// The counters of the frame (the statistics are read from another thread).
    struct Counters {
        std::atomic<u64> eligible{0};
        std::array<std::atomic<u64>, u32(ProxySkip::Count)> skipped{};
        std::atomic<u64> size_changes{0};
        std::atomic<u64> proxies{0};
        std::atomic<u64> fills{0};
        std::atomic<u64> resolves{0};
        std::atomic<u64> samples{0};
        std::atomic<u64> writes{0};
    };
    mutable Counters counters;
    /// The proxy size of the last check: the rasterizer asks for the size of every
    /// attachment of a pass and keeps the first one; a change means the pass mixes sizes.
    mutable std::optional<SceneResolution::Size> last_proxy_size;
    /// The counters are only collected with the frame statistics on (TLG_FRAME_STATS=1).
    bool stats_on = false;
    /// When the counters were last printed (the clock of the statistics).
    std::chrono::steady_clock::time_point stats_window{};
};
} // namespace Vulkan
