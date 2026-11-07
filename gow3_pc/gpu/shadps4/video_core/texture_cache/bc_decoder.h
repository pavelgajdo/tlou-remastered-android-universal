// SPDX-License-Identifier: GPL-2.0-or-later
// BC texture decoding for drivers that cannot sample BC formats (bc_decode.comp).

#pragma once

#include <span>
#include <unordered_map>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
class Scheduler;
class Runtime;
} // namespace Vulkan

namespace VideoCore {

struct Buffer;
struct Image;

/// The decoder's format number (bc_decode.comp FORMAT), or -1 for a format it does not decode.
constexpr int BcDecodeFormatIndex(vk::Format format) {
    switch (format) {
    case vk::Format::eBc1RgbaUnormBlock:
    case vk::Format::eBc1RgbaSrgbBlock:
        return 0;
    case vk::Format::eBc2UnormBlock:
    case vk::Format::eBc2SrgbBlock:
        return 1;
    case vk::Format::eBc3UnormBlock:
    case vk::Format::eBc3SrgbBlock:
        return 2;
    case vk::Format::eBc4UnormBlock:
        return 3;
    case vk::Format::eBc4SnormBlock:
        return 4;
    case vk::Format::eBc5UnormBlock:
        return 5;
    case vk::Format::eBc5SnormBlock:
        return 6;
    case vk::Format::eBc6HUfloatBlock:
        return 7;
    case vk::Format::eBc6HSfloatBlock:
        return 8;
    case vk::Format::eBc7UnormBlock:
    case vk::Format::eBc7SrgbBlock:
        return 9;
    default:
        return -1;
    }
}

/// The uncompressed format a decoded BC image is created with.
constexpr vk::Format BcDecodedFormat(vk::Format format) {
    switch (format) {
    case vk::Format::eBc1RgbaSrgbBlock:
    case vk::Format::eBc2SrgbBlock:
    case vk::Format::eBc3SrgbBlock:
    case vk::Format::eBc7SrgbBlock:
        return vk::Format::eR8G8B8A8Srgb;
    case vk::Format::eBc4UnormBlock:
        return vk::Format::eR8Unorm;
    case vk::Format::eBc4SnormBlock:
        return vk::Format::eR8Snorm;
    case vk::Format::eBc5UnormBlock:
        return vk::Format::eR8G8Unorm;
    case vk::Format::eBc5SnormBlock:
        return vk::Format::eR8G8Snorm;
    case vk::Format::eBc6HUfloatBlock:
    case vk::Format::eBc6HSfloatBlock:
        return vk::Format::eR16G16B16A16Sfloat;
    default:
        return vk::Format::eR8G8B8A8Unorm;
    }
}

class BcDecoder {
public:
    explicit BcDecoder(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                       Vulkan::Runtime& runtime);
    ~BcDecoder();

    /// Records the decode of the blocks `copies` read from `in` (linear, as uploaded to a BC
    /// image) and points `copies` at the decoded texels in the returned buffer.
    const Buffer* Decode(const Image& image, const Buffer* in, std::span<vk::BufferImageCopy> copies);

private:
    vk::Pipeline GetPipeline(u32 format);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    Vulkan::Runtime& runtime;
    vk::UniqueDescriptorSetLayout desc_layout;
    vk::UniquePipelineLayout pl_layout;
    std::unordered_map<u32, vk::UniquePipeline> pipelines;
};

} // namespace VideoCore
