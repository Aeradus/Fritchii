// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// AMD FSR 4 (through AMD's FidelityFX API) in place of a game's own upscale pass. AMD's signed
// amd_fidelityfx_loader_vk.dll and amd_fidelityfx_upscaler_vk.dll (FSR SDK 2.4 or later) are
// loaded at run time from next to the executable; only the MIT-licensed API headers are
// compiled in (externals/ffx-api).

#pragma once

#include <atomic>
#include <chrono>
#include <optional>
#include <string>

#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"

namespace Vulkan {

class Device;
class MemoryAllocator;
class Scheduler;

class FfxUpscaler final : public TemporalUpscaler {
public:
    /// AMD's loader loads and offers an upscaler of the wanted version (FSR 4 unless
    /// EDEN_NX2_FSR_VERSION names another, e.g. 3.1) for this device. Checked once.
    [[nodiscard]] static bool IsAvailable(const Device& device);

    /// Throws vk::Exception when IsAvailable is false.
    explicit FfxUpscaler(const Device& device, Scheduler& scheduler,
                         MemoryAllocator& memory_allocator);
    ~FfxUpscaler() override;

    bool Upscale(const Inputs& inputs) override;

    [[nodiscard]] bool Failed() const noexcept override {
        return failed.load();
    }
    [[nodiscard]] VkImage OutputImage() const noexcept override {
        return *output_image;
    }
    [[nodiscard]] VkImageView OutputView() const noexcept override {
        return *output_view;
    }
    [[nodiscard]] VkSampler PointSampler() const noexcept override {
        return *point_sampler;
    }

private:
    void CreateContext(const Inputs& inputs);
    void DestroyContext();
    void CreateOutput(VkExtent2D upscale_size);

    const Device& device;
    Scheduler& scheduler;
    MemoryAllocator& memory_allocator;
    void* context{}; ///< ffxContext
    VkExtent2D render_extent{};
    VkExtent2D upscale_extent{};
    bool hdr_input = true;
    bool depth_inverted = true;
    std::optional<std::chrono::steady_clock::time_point> last_upscale;
    vk::Image output_image;
    vk::ImageView output_view;
    vk::Sampler point_sampler;
    std::atomic<bool> failed{};
};

} // namespace Vulkan
