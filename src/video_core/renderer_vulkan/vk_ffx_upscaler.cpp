// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// AMD FSR 4 (through AMD's FidelityFX API) in place of a game's own upscale pass.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "common/dynamic_library.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "video_core/renderer_vulkan/vk_ffx_upscaler.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

// After the project's Vulkan headers, which set the platform defines vulkan.h needs.
#include <ffx_api.h>
#include <ffx_upscale.h>
#include <vk/ffx_api_vk.h>

namespace Vulkan {
namespace {

constexpr VkFormat OUTPUT_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkImageUsageFlags OUTPUT_USAGE = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT;
constexpr const char* LOADER_DLL = "amd_fidelityfx_loader_vk.dll";

struct Api {
    Common::DynamicLibrary library;
    PfnFfxCreateContext create_context{};
    PfnFfxDestroyContext destroy_context{};
    PfnFfxQuery query{};
    PfnFfxDispatch dispatch{};
    u64 version_id{};
    std::string version_name;
    bool available{};
};

Api& GetApi() {
    static Api api;
    return api;
}

VkDeviceContext DeviceContext(const Device& device) {
    return VkDeviceContext{
        .vkDevice = *device.GetLogical(),
        .vkPhysicalDevice = device.GetPhysical(),
        .vkDeviceProcAddr = device.GetDispatchLoader().vkGetDeviceProcAddr,
    };
}

void OnMessage(uint32_t type, const wchar_t* message) {
    const std::string text = message ? Common::UTF16ToUTF8(std::wstring_view{message}) : "";
    if (type == FFX_API_MESSAGE_TYPE_ERROR) {
        LOG_ERROR(Render_Vulkan, "AMD FSR: {}", text);
    } else {
        LOG_WARNING(Render_Vulkan, "AMD FSR: {}", text);
    }
}

/// Loads AMD's loader and picks the upscaler version: the first one whose name starts with
/// EDEN_NX2_FSR_VERSION (default "4."), the newest the loader offers for this device.
bool LoadApi(const Device& device) {
    Api& api = GetApi();
    if (!api.library.Open(LOADER_DLL)) {
        LOG_INFO(Render_Vulkan, "AMD FSR 4: {} not found next to the executable", LOADER_DLL);
        return false;
    }
    if (!api.library.GetSymbol("ffxCreateContext", &api.create_context) ||
        !api.library.GetSymbol("ffxDestroyContext", &api.destroy_context) ||
        !api.library.GetSymbol("ffxQuery", &api.query) ||
        !api.library.GetSymbol("ffxDispatch", &api.dispatch)) {
        LOG_ERROR(Render_Vulkan, "AMD FSR 4: {} lacks the FidelityFX API entry points", LOADER_DLL);
        return false;
    }
    VkDeviceContext device_context = DeviceContext(device);
    u64 count = 0;
    ffxQueryDescGetVersions versions{};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    versions.device = &device_context;
    versions.outputCount = &count;
    if (api.query(nullptr, &versions.header) != FFX_API_RETURN_OK || count == 0) {
        LOG_WARNING(Render_Vulkan, "AMD FSR 4: the loader offers no upscaler for this device");
        return false;
    }
    std::vector<u64> ids(count);
    std::vector<const char*> names(count);
    versions.versionIds = ids.data();
    versions.versionNames = names.data();
    if (api.query(nullptr, &versions.header) != FFX_API_RETURN_OK) {
        return false;
    }
    const char* const wanted_env = std::getenv("EDEN_NX2_FSR_VERSION");
    const std::string_view wanted = wanted_env ? wanted_env : "4.";
    std::string offered;
    std::optional<size_t> pick;
    for (size_t i = 0; i < count; ++i) {
        const std::string_view name = names[i] ? names[i] : "";
        offered += fmt::format("{}{}", offered.empty() ? "" : ", ", name);
        if (!pick && name.starts_with(wanted)) {
            pick = i;
        }
    }
    if (!pick) {
        LOG_WARNING(Render_Vulkan, "AMD FSR 4: no upscaler version {}* for this device (offered: {})",
                    wanted, offered);
        return false;
    }
    api.version_id = ids[*pick];
    api.version_name = names[*pick];
    LOG_INFO(Render_Vulkan, "AMD FSR: upscaler {} (offered: {})", api.version_name, offered);
    return true;
}

FfxApiResource Resource(VkImage image, VkFormat format, VkExtent2D size, VkImageUsageFlags usage,
                        uint32_t state) {
    const VkImageCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {size.width, size.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
    };
    return ffxApiGetResourceVK(image, ffxApiGetImageResourceDescriptionVK(image, info, 0), state);
}

VkImageMemoryBarrier Barrier(const TemporalUpscaler::Image& image, VkAccessFlags src_access,
                             VkAccessFlags dst_access, VkImageLayout old_layout,
                             VkImageLayout new_layout) {
    VkImageSubresourceRange range = image.range;
    range.aspectMask = image.barrier_aspect;
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image.image,
        .subresourceRange = range,
    };
}

/// FSR's RCAS from the sharpening slider (stops x 100, 200 = off), as the FSR 3.1 path reads it.
/// EDEN_NX2_FSR4_SHARPNESS=<0..1>|off overrides it.
std::optional<f32> Sharpness() {
    if (const char* const value = std::getenv("EDEN_NX2_FSR4_SHARPNESS")) {
        if (std::string_view{value} == "off") {
            return std::nullopt;
        }
        return std::clamp(static_cast<f32>(std::strtod(value, nullptr)), 0.0f, 1.0f);
    }
    const int slider = Settings::values.fsr_sharpening_slider.GetValue();
    if (slider >= 200) {
        return std::nullopt;
    }
    return std::clamp(1.0f - static_cast<f32>(slider) / 200.0f, 0.0f, 1.0f);
}

} // Anonymous namespace

bool FfxUpscaler::IsAvailable(const Device& device) {
    static std::once_flag once;
    std::call_once(once, [&] { GetApi().available = LoadApi(device); });
    return GetApi().available;
}

FfxUpscaler::FfxUpscaler(const Device& device_, Scheduler& scheduler_,
                         MemoryAllocator& memory_allocator_)
    : device{device_}, scheduler{scheduler_}, memory_allocator{memory_allocator_} {
    if (!IsAvailable(device)) {
        throw vk::Exception(VK_ERROR_FEATURE_NOT_PRESENT);
    }
    point_sampler = device.GetLogical().CreateSampler(VkSamplerCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxAnisotropy = 1.0f,
        .compareOp = VK_COMPARE_OP_NEVER,
        .maxLod = VK_LOD_CLAMP_NONE,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
    });
}

FfxUpscaler::~FfxUpscaler() {
    // As StreamlineDlssUpscaler: this runs after the game's channels are gone, so no submit.
    void(device.GetLogical().WaitIdle());
    DestroyContext();
}

void FfxUpscaler::DestroyContext() {
    if (context) {
        GetApi().destroy_context(&context, nullptr);
        context = nullptr;
    }
}

void FfxUpscaler::CreateContext(const Inputs& in) {
    Api& api = GetApi();
    if (context) {
        scheduler.Finish(); // recorded dispatches still use the old context
        DestroyContext();
    }
    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.deviceContext = DeviceContext(device);
    ffxOverrideVersion version{};
    version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    version.versionId = api.version_id;
    backend.header.pNext = &version.header;

    ffxCreateContextDescUpscale desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.header.pNext = &backend.header;
    desc.flags = (in.hdr ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE : 0u) |
                 (in.depth_inverted ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0u) |
                 (in.hdr ? FFX_UPSCALE_ENABLE_AUTO_EXPOSURE : 0u);
    desc.maxRenderSize = {in.render_size.width, in.render_size.height};
    desc.maxUpscaleSize = {in.upscale_size.width, in.upscale_size.height};
    desc.fpMessage = OnMessage;
    void* created{};
    const ffxReturnCode_t result = api.create_context(&created, &desc.header, nullptr);
    if (result != FFX_API_RETURN_OK) {
        LOG_ERROR(Render_Vulkan, "AMD FSR {}: context creation failed ({})", api.version_name,
                  result);
        throw vk::Exception(VK_ERROR_INITIALIZATION_FAILED);
    }
    context = created;
    ffxQueryGetProviderVersion provider{};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    const bool known = api.query(&context, &provider.header) == FFX_API_RETURN_OK &&
                       provider.versionName != nullptr;
    LOG_INFO(Render_Vulkan, "AMD FSR {}: {}x{} -> {}x{}, {}, depth {}",
             known ? provider.versionName : api.version_name.c_str(), in.render_size.width,
             in.render_size.height, in.upscale_size.width, in.upscale_size.height,
             in.hdr ? "HDR" : "LDR", in.depth_inverted ? "inverted" : "standard");
}

void FfxUpscaler::CreateOutput(VkExtent2D upscale_size) {
    if (output_image) {
        scheduler.Finish(); // the old output may still be in use
    }
    output_image = memory_allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = OUTPUT_FORMAT,
        .extent = {upscale_size.width, upscale_size.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = OUTPUT_USAGE,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    output_view = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *output_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = OUTPUT_FORMAT,
        .subresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    });
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([image = *output_image](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                               VkImageMemoryBarrier{
                                   .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                   .srcAccessMask = 0,
                                   .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                   .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = image,
                                   .subresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                               });
    });
}

bool FfxUpscaler::Upscale(const Inputs& in) {
    if (failed) {
        return false;
    }
    const bool extent_changed = !output_image || in.render_size.width != render_extent.width ||
                                in.render_size.height != render_extent.height ||
                                in.upscale_size.width != upscale_extent.width ||
                                in.upscale_size.height != upscale_extent.height;
    const bool options_changed =
        extent_changed || in.hdr != hdr_input || in.depth_inverted != depth_inverted;
    try {
        if (extent_changed) {
            CreateOutput(in.upscale_size);
            render_extent = in.render_size;
            upscale_extent = in.upscale_size;
        }
        if (options_changed || !context) {
            hdr_input = in.hdr;
            depth_inverted = in.depth_inverted;
            CreateContext(in);
        }
    } catch (const vk::Exception& exception) {
        LOG_ERROR(Render_Vulkan, "AMD FSR 4 unavailable: {}", exception.what());
        failed = true;
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    const f32 frame_time_ms =
        last_upscale ? std::chrono::duration<f32, std::milli>(now - *last_upscale).count()
                     : 1000.0f / 30.0f;
    last_upscale = now;
    const bool reset = in.reset || options_changed;
    const std::optional<f32> sharpness = Sharpness();
    static const bool srgb_input = [] {
        const char* const value = std::getenv("EDEN_NX2_FSR4_SRGB");
        return value && std::atoi(value) != 0;
    }();

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, in, reset, sharpness, frame_time_ms, ctx = context,
                      output = *output_image](vk::CommandBuffer cmdbuf) {
        if (failed) {
            return;
        }
        constexpr VkAccessFlags WRITES =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        // The inputs stay in GENERAL; FSR reads them as sampled images.
        const std::array to_read{
            Barrier(in.color, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.depth, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.motion_vectors, WRITES, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        };
        const std::array to_general{
            Barrier(in.color, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | WRITES,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.depth, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | WRITES,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.motion_vectors, VK_ACCESS_SHADER_READ_BIT,
                    VK_ACCESS_SHADER_READ_BIT | WRITES, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_GENERAL),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, to_read);

        ffxDispatchDescUpscale desc{};
        desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        desc.commandList = *cmdbuf;
        desc.color = Resource(in.color.image, in.color.format, in.render_size,
                              VK_IMAGE_USAGE_SAMPLED_BIT, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        desc.depth = Resource(in.depth.image, in.depth.format, in.render_size,
                              VK_IMAGE_USAGE_SAMPLED_BIT, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        desc.motionVectors =
            Resource(in.motion_vectors.image, in.motion_vectors.format, in.render_size,
                     VK_IMAGE_USAGE_SAMPLED_BIT, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        desc.output = Resource(output, OUTPUT_FORMAT, in.upscale_size, OUTPUT_USAGE,
                               FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        desc.jitterOffset = {in.jitter[0], in.jitter[1]};
        desc.motionVectorScale = {in.motion_vector_scale[0], in.motion_vector_scale[1]};
        desc.renderSize = {in.render_size.width, in.render_size.height};
        desc.upscaleSize = {in.upscale_size.width, in.upscale_size.height};
        desc.enableSharpening = sharpness.has_value();
        desc.sharpness = sharpness.value_or(0.0f);
        desc.frameTimeDelta = std::clamp(frame_time_ms, 1.0f, 250.0f);
        desc.preExposure = 1.0f;
        desc.reset = reset;
        desc.cameraNear = in.depth_inverted ? 1000.0f : 0.1f;
        desc.cameraFar = in.depth_inverted ? 0.1f : 1000.0f;
        desc.cameraFovAngleVertical = 1.0f;
        desc.viewSpaceToMetersFactor = 1.0f;
        desc.flags = !in.hdr && srgb_input ? FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB : 0u;
        void* dispatch_context = ctx;
        const ffxReturnCode_t result = GetApi().dispatch(&dispatch_context, &desc.header);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, {}, {}, to_general);
        if (result != FFX_API_RETURN_OK && !failed.exchange(true)) {
            LOG_ERROR(Render_Vulkan, "AMD FSR 4 dispatch failed ({}); the game's pass takes over",
                      result);
            // The caller samples the output this frame: clear it rather than show garbage.
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            cmdbuf.ClearColorImage(output, VK_IMAGE_LAYOUT_GENERAL, VkClearColorValue{}, range);
        }
    });
    return true;
}

} // namespace Vulkan
