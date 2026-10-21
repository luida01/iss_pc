// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: the scene proxy (the reduced-size copy of the guest's targets that the port draws
// into when the resolution preset is not Native AA).
//
// The first part needs no device: it checks which guest sizes can have a proxy and how big
// it is. The scene is rendered at the preset's size and the guest's targets keep their
// fraction of it: a 1920x1080 target is the scene, a 960x540 one (the game's
// post-processing chain) is half of it, a 480x270 one a quarter. A size that is not a
// fraction of the guest's frame (a shadow map, a texture) never has one.
//
// The second part needs a Vulkan device (the copies): it draws into the proxy, resolves it
// back and checks that the image is the same, and that a native write invalidates the
// proxy (the coherence). It only runs with TLG_SCENE_PROXY_GPU=1: the port's own Vulkan
// instance cannot be created in a test binary on Windows (the layer settings of the
// instance's pNext chain are always there, but the extension that defines them is only
// enabled when a layer provides it: vk_platform.cpp).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <setjmp.h>
#include "common/slot_vector.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_scene_resolution.h"
#include "video_core/texture_cache/blit_helper.h"
#include <vk_mem_alloc.h>
#include "vulkan_test_driver.h"

#if defined(_WIN32)
// The GPU library needs two symbols of the loader that a test without a guest process
// does not have: the setjmp of the rasterizer's fault recovery (the loader's own is in
// src/compat_win.c) and the file mapping of the shader cache (a Win11 API that the import
// library of this build host does not have).
#if defined(__clang__)
// The header declares the file mapping as an import that the library does not have.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winconsistent-dllimport"
#endif
extern "C" __attribute__((returns_twice)) int tlg_setjmp(sigjmp_buf buffer) {
    // The loader's own version (src/compat_win.c) saves the xmm registers too; the test
    // only needs a recovery point that exists (the rasterizer never faults here).
    jmp_buf local;
    std::memset(local, 0, sizeof(local));
    const int result = setjmp(local);
    if (result == 0) {
        std::memcpy(buffer, local, sizeof(local));
    }
    return result;
}
extern "C" HANDLE WINAPI CreateFileMapping2(HANDLE file, SECURITY_ATTRIBUTES* attributes,
                                             ULONG access, ULONG protection, ULONG allocation,
                                             ULONG64 size, PCWSTR name,
                                             MEM_EXTENDED_PARAMETER* extended, ULONG count) {
    (void)access;
    (void)allocation;
    (void)extended;
    (void)count;
    return CreateFileMappingW(file, attributes, protection, static_cast<DWORD>(size >> 32),
                              static_cast<DWORD>(size), name);
}
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif

namespace {

using Vulkan::SceneResolution::Size;
using Vulkan::SceneTargets;

// The scene at the Quality preset: 1920/1.5 = 1280, 1080/1.5 = 720.
constexpr Size kScene{1280, 720};

VideoCore::ImageInfo MakeInfo(u32 w, u32 h, u32 levels = 1) {
    VideoCore::ImageInfo ci;
    ci.size = {w, h, 1};
    ci.resources = {levels, 1};
    ci.type = AmdGpu::ImageType::Color2D;
    ci.pixel_format = vk::Format::eR8G8B8A8Unorm;
    ci.num_bits = 32;
    return ci;
}

// Which guest sizes can have a proxy, and how big it is.
void TestDivisors() {
    // The scene itself.
    assert((SceneTargets::ProxySizeFor(MakeInfo(1920, 1080), kScene) == kScene));
    // The guest's half-resolution chain (the game's post-processing).
    assert((SceneTargets::ProxySizeFor(MakeInfo(960, 540), kScene) == Size{640, 360}));
    // A quarter of the guest's frame.
    assert((SceneTargets::ProxySizeFor(MakeInfo(480, 270), kScene) == Size{320, 180}));
    // An eighth.
    assert((SceneTargets::ProxySizeFor(MakeInfo(240, 135), kScene) == Size{160, 90}));
    // Any size that is a view of the guest's frame scales with it: 1024x576 (the game's
    // half-resolution buffer is not necessarily 960x540) and 1280x720 too.
    assert((SceneTargets::ProxySizeFor(MakeInfo(1024, 576), kScene) == Size{682, 384}));
    assert((SceneTargets::ProxySizeFor(MakeInfo(1280, 720), kScene) == Size{852, 480}));
    // What is not a view of the guest's frame never has a proxy: the shadow maps and the
    // textures (another aspect ratio) stay at their own size.
    assert(!SceneTargets::ScalesWithTheScene(MakeInfo(1024, 1024)));
    assert(!SceneTargets::ScalesWithTheScene(MakeInfo(512, 512)));
    assert(!SceneTargets::ScalesWithTheScene(MakeInfo(0, 0)));
    assert(SceneTargets::ScalesWithTheScene(MakeInfo(1920, 1080)));
    assert(SceneTargets::ScalesWithTheScene(MakeInfo(960, 540)));
    // The mip level of the proxy.
    assert((SceneTargets::ProxySizeFor(MakeInfo(960, 540), kScene, 1) == Size{320, 180}));
    assert((SceneTargets::ProxySizeFor(MakeInfo(960, 540), kScene, 2) == Size{160, 90}));
    // A bigger scene than the guest's frame (the output at 4K): the proxy of the guest's
    // targets is bigger than them, but the factor is the same.
    assert((SceneTargets::ProxySizeFor(MakeInfo(1920, 1080), Size{2560, 1440}) ==
            Size{2560, 1440}));
    assert((SceneTargets::ProxySizeFor(MakeInfo(960, 540), Size{2560, 1440}) ==
            Size{1280, 720}));
    // The scene at the Balanced preset (1130x636): the factor is not exact and the proxy
    // is rounded down to an even size.
    assert((SceneTargets::ProxySizeFor(MakeInfo(1920, 1080), Size{1130, 636}) ==
            Size{1130, 636}));
    assert((SceneTargets::ProxySizeFor(MakeInfo(960, 540), Size{1130, 636}) ==
            Size{564, 318}));
}

// The proxy on a real device: draw into it, resolve it back and check the coherence.
void TestProxyOnDevice() {
    using namespace Vulkan;
    Instance instance(false, false);
    // The calls of this executable use a local dispatcher, and the GPU library (a static
    // library here, a shared one on Linux) has no initializer of its own: the default
    // dispatcher of the Vulkan headers has to be ready before the Instance is created.
    static vk::detail::DynamicLoader loader;
    vk::detail::DispatchLoaderDynamic dispatch;
    dispatch.init(
        loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));
    dispatch.init(instance.GetInstance());
    dispatch.init(instance.GetDevice());
    Scheduler scheduler(instance);
    Runtime runtime(instance, scheduler);
    Common::SlotVector<VideoCore::ImageView> views;
    Common::SlotVector<VideoCore::Image> images;

    const auto scene_id = images.insert(instance, runtime, views, MakeInfo(1920, 1080));
    const auto half_id = images.insert(instance, runtime, views, MakeInfo(960, 540, 4));
    SceneTargets targets(instance, scheduler, runtime, [&](VideoCore::ImageId id, u64 uid) {
        if (!images.is_allocated(id)) return static_cast<VideoCore::Image*>(nullptr);
        return !uid || images[id].image_uid == uid ? &images[id] : nullptr;
    });
    targets.SetSize(kScene);

    // The eligible sizes and their proxies.
    assert(targets.Eligible(images[scene_id]));
    assert(targets.EligibleScene(images[scene_id]));
    assert(!targets.EligibleScene(images[half_id]));
    assert((targets.ProxySize(images[scene_id], 0) == kScene));
    assert(targets.Eligible(images[half_id]));
    assert((targets.ProxySize(images[half_id], 0) == Size{640, 360}));
    assert((targets.ProxySize(images[half_id], 2) == Size{160, 90}));

    // Drawing into the proxy and resolving it back gives the same image.
    VkBuffer readback{};
    VmaAllocation allocation{};
    VmaAllocationInfo ai{};
    const VkBufferCreateInfo bi{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size=16, .usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo ac{.flags=VMA_ALLOCATION_CREATE_MAPPED_BIT |
        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT, .usage=VMA_MEMORY_USAGE_AUTO,
        .requiredFlags=VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    assert(vmaCreateBuffer(instance.GetAllocator(), &bi, &ac, &readback, &allocation, &ai) ==
           VK_SUCCESS);
    const auto pixel = [&](vk::Image image, u32 w, u32 h) {
        scheduler.EndRendering();
        const auto cmd = scheduler.CommandBuffer();
        const vk::MemoryBarrier2 b{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask=vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask=vk::PipelineStageFlagBits2::eTransfer,
            .dstAccessMask=vk::AccessFlagBits2::eTransferRead};
        cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&b}, dispatch);
        const vk::BufferImageCopy region{
            .imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
            .imageOffset={s32(w-1),s32(h-1),0},.imageExtent={1,1,1}};
        cmd.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, readback, region,
                              dispatch);
        const vk::MemoryBarrier2 host{.srcStageMask=vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask=vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask=vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask=vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&host}, dispatch);
        scheduler.Finish();
        u32 result = 0;
        std::memcpy(&result, ai.pMappedData, 4);
        return result;
    };
    VideoCore::ImageViewInfo view_info;
    view_info.format = vk::Format::eR8G8B8A8Unorm;
    const auto proxy = targets.Attachment(scene_id, view_info);
    {
        auto cmd = scheduler.CommandBuffer();
        const vk::RenderingAttachmentInfo color{.imageView=proxy.view,.imageLayout=proxy.layout,
            .loadOp=vk::AttachmentLoadOp::eClear,.storeOp=vk::AttachmentStoreOp::eStore,
            .clearValue=vk::ClearValue{.color={.float32=std::array{0.f,0.5f,0.25f,1.f}}}};
        cmd.beginRendering({.renderArea={{0,0},{kScene.width,kScene.height}},.layerCount=1,
            .colorAttachmentCount=1,.pColorAttachments=&color}, dispatch);
        cmd.endRendering(dispatch);
    }
    // The native image is resolved lazily: reading it copies the proxy into it.
    runtime.Transit(&images[scene_id], vk::ImageLayout::eTransferSrcOptimal,
                    vk::PipelineStageFlagBits2::eTransfer,
                    vk::AccessFlagBits2::eTransferRead);
    runtime.FlushBarriers();
    assert(pixel(images[scene_id].GetImage(), 1920, 1080) == 0xff400080u);

    // The coherence: a native write invalidates the proxy, so the next sampler reads the
    // native image instead of the stale proxy.
    assert(targets.ProxyCurrent(images[scene_id], 0, 1));
    {
        vk::ClearValue clear{};
        clear.color.float32 = std::array{1.f, 0.f, 0.f, 1.f};
        runtime.ClearImage(&images[scene_id], {}, clear);
    }
    assert(!targets.ProxyCurrent(images[scene_id], 0, 1));
    // The next draw into the proxy fills it from the native again.
    const auto proxy2 = targets.Attachment(scene_id, view_info);
    assert(targets.ProxyCurrent(images[scene_id], 0, 1));
    assert(proxy2.image == proxy.image);

    // The counters of the frame: what the scene offered and what the proxy took.
    const auto stats = targets.TakeStats();
    assert(stats.eligible > 0);
    assert(stats.writes >= 2);
    assert(stats.proxies >= 1);
    std::printf("scene proxy: eligible %llu, drawn %llu, proxies %llu, fills %llu, "
                "resolves %llu, samples %llu\n",
                static_cast<unsigned long long>(stats.eligible),
                static_cast<unsigned long long>(stats.writes),
                static_cast<unsigned long long>(stats.proxies),
                static_cast<unsigned long long>(stats.fills),
                static_cast<unsigned long long>(stats.resolves),
                static_cast<unsigned long long>(stats.samples));
    scheduler.Finish();
    vmaDestroyBuffer(instance.GetAllocator(), readback, allocation);
}

} // namespace

int main() {
    TestDivisors();
    const char* gpu = std::getenv("TLG_SCENE_PROXY_GPU");
    if (gpu && gpu[0] == '1') {
        if (SkipWithoutVulkanDriver()) {
            return 0;
        }
        TestProxyOnDevice();
    }
    std::puts("scene proxy test: ok");
    return 0;
}
