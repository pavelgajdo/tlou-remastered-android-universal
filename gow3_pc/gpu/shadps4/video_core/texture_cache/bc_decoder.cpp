// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

#include "common/alignment.h"
#include "common/assert.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/texture_cache/bc_decoder.h"
#include "video_core/texture_cache/image.h"

#include "video_core/host_shaders/bc_decode_comp.h"

namespace VideoCore {

namespace {

struct Params {
    u32 in_offset;
    u32 out_offset;
    u32 blocks_x;
    u32 blocks_y;
    u32 num_blocks;
    u32 in_row_blocks;
    u32 in_slice_blocks;
    u32 out_row_words;
    u32 out_slice_words;
};

struct Pass {
    Params params;
    u32 groups_x;
    u32 groups_y;
};

constexpr u32 BlockBytes(int format) {
    return format == 0 || format == 3 || format == 4 ? 8 : 16;
}

constexpr u32 TexelBytes(int format) {
    switch (format) {
    case 3:
    case 4:
        return 1;
    case 5:
    case 6:
        return 2;
    case 7:
    case 8:
        return 8;
    default:
        return 4;
    }
}

} // namespace

BcDecoder::BcDecoder(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                     Vulkan::Runtime& runtime_)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_} {
    const auto device = instance.GetDevice();
    const std::array<vk::DescriptorSetLayoutBinding, 2> bindings = {{
        {
            .binding = 0,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
        {
            .binding = 1,
            .descriptorType = vk::DescriptorType::eStorageBuffer,
            .descriptorCount = 1,
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
        },
    }};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    auto [desc_result, layout] = device.createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(desc_result == vk::Result::eSuccess, "Failed to create descriptor set layout: {}",
               vk::to_string(desc_result));
    desc_layout = std::move(layout);

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PushConstantRange push_range = {
        .stageFlags = vk::ShaderStageFlagBits::eCompute,
        .offset = 0,
        .size = sizeof(Params),
    };
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1U,
        .pPushConstantRanges = &push_range,
    };
    auto [layout_result, pipeline_layout] = device.createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess, "Failed to create pipeline layout: {}",
               vk::to_string(layout_result));
    pl_layout = std::move(pipeline_layout);
}

BcDecoder::~BcDecoder() = default;

vk::Pipeline BcDecoder::GetPipeline(u32 format) {
    if (const auto it = pipelines.find(format); it != pipelines.end()) {
        return *it->second;
    }
    const auto device = instance.GetDevice();
    const auto module = Vulkan::CompileSPV(std::span<const u32>{BC_DECODE_COMP}, device);
    const vk::SpecializationMapEntry entry{0, 0, sizeof(u32)};
    const vk::SpecializationInfo specialization{
        .mapEntryCount = 1,
        .pMapEntries = &entry,
        .dataSize = sizeof(u32),
        .pData = &format,
    };
    const vk::ComputePipelineCreateInfo pipeline_ci = {
        .stage{
            .stage = vk::ShaderStageFlagBits::eCompute,
            .module = module,
            .pName = "main",
            .pSpecializationInfo = &specialization,
        },
        .layout = *pl_layout,
    };
    auto [result, pipeline] = device.createComputePipelineUnique(VK_NULL_HANDLE, pipeline_ci);
    ASSERT_MSG(result == vk::Result::eSuccess, "BC decode pipeline creation failed: {}",
               vk::to_string(result));
    device.destroyShaderModule(module);
    const auto handle = *pipeline;
    pipelines.emplace(format, std::move(pipeline));
    return handle;
}

const Buffer* BcDecoder::Decode(const Image& image, const Buffer* in,
                                std::span<vk::BufferImageCopy> copies) {
    const int format = BcDecodeFormatIndex(image.info.pixel_format);
    ASSERT(format >= 0 && !copies.empty());
    const u32 block_bytes = BlockBytes(format);
    const u32 texel_bytes = TexelBytes(format);

    // Input blocks keep the upload's layout; output rows and slices are whole blocks of texels.
    std::vector<Pass> passes;
    passes.reserve(copies.size());
    u64 in_begin = ~u64{0}, in_end = 0, out_size = 0;
    std::vector<u64> out_offsets;
    out_offsets.reserve(copies.size());
    const u32 max_groups = std::max(instance.GetMaxComputeWorkGroupCountX(), 1u);
    for (const auto& copy : copies) {
        const u32 width = copy.imageExtent.width, height = copy.imageExtent.height;
        const u32 slices = copy.imageExtent.depth * copy.imageSubresource.layerCount;
        const u32 row_texels = copy.bufferRowLength ? copy.bufferRowLength : width;
        const u32 image_rows = copy.bufferImageHeight ? copy.bufferImageHeight : height;
        Params params{};
        params.blocks_x = (width + 3) / 4;
        params.blocks_y = (height + 3) / 4;
        params.num_blocks = params.blocks_x * params.blocks_y * slices;
        params.in_row_blocks = (row_texels + 3) / 4;
        params.in_slice_blocks = params.in_row_blocks * ((image_rows + 3) / 4);
        params.out_row_words = params.blocks_x * 4 * texel_bytes / 4;
        params.out_slice_words = params.out_row_words * params.blocks_y * 4;
        ASSERT(copy.bufferOffset % 4 == 0);
        const u64 in_bytes = (u64(slices - 1) * params.in_slice_blocks +
                              u64(params.blocks_y - 1) * params.in_row_blocks + params.blocks_x) *
                             block_bytes;
        in_begin = std::min<u64>(in_begin, copy.bufferOffset);
        in_end = std::max<u64>(in_end, copy.bufferOffset + in_bytes);
        out_offsets.push_back(out_size);
        out_size += Common::AlignUp(u64(params.out_slice_words) * 4 * slices, 16);

        const u32 groups = (params.num_blocks + 63) / 64;
        const u32 groups_x = std::min(groups, max_groups);
        passes.push_back({params, groups_x, (groups + groups_x - 1) / groups_x});
    }
    in_end = std::min<u64>(in_end, in->SizeBytes());
    const u64 in_bind = in_begin - in_begin % instance.StorageMinAlignment();

    const auto staging = runtime.GetStagingPool().Request(out_size, MemoryType::DeviceLocal,
                                                          instance.StorageMinAlignment());
    for (size_t i = 0; i < copies.size(); ++i) {
        auto& copy = copies[i];
        auto& params = passes[i].params;
        params.in_offset = static_cast<u32>((copy.bufferOffset - in_bind) / 4);
        params.out_offset = static_cast<u32>(out_offsets[i] / 4);
        copy.bufferOffset = staging.offset + out_offsets[i];
        copy.bufferRowLength = params.blocks_x * 4;
        copy.bufferImageHeight = params.blocks_y * 4;
    }

    scheduler.EndRendering();
    runtime.FlushBarriers();
    const vk::DescriptorBufferInfo in_info{
        .buffer = in->Handle(),
        .offset = in_bind,
        .range = in_end - in_bind,
    };
    const vk::DescriptorBufferInfo out_info{
        .buffer = staging.buffer->Handle(),
        .offset = staging.offset,
        .range = out_size,
    };
    scheduler.Record([pipeline = GetPipeline(static_cast<u32>(format)), layout = *pl_layout,
                      in_info, out_info,
                      passes = scheduler.RecordData(std::span<const Pass>{passes})](
                         vk::CommandBuffer cmdbuf) {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        const std::array<vk::WriteDescriptorSet, 2> set_writes = {{
            {
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .pBufferInfo = &in_info,
            },
            {
                .dstBinding = 1,
                .descriptorCount = 1,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .pBufferInfo = &out_info,
            },
        }};
        cmdbuf.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, layout, 0, set_writes);
        for (const Pass& pass : passes) {
            cmdbuf.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(Params),
                                 &pass.params);
            cmdbuf.dispatch(pass.groups_x, pass.groups_y, 1);
        }
    });
    runtime.AccessBuffer(in, in_bind, in_end - in_bind, vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderRead);
    runtime.AccessBuffer(staging.buffer, staging.offset, out_size,
                         vk::PipelineStageFlagBits2::eComputeShader,
                         vk::AccessFlagBits2::eShaderWrite);

    static bool noted = false;
    if (!noted) {
        noted = true;
        std::printf("GPU: decoding %s textures on the GPU before upload (the driver cannot sample "
                    "them)\n",
                    vk::to_string(image.info.pixel_format).c_str());
    }
    return staging.buffer;
}

} // namespace VideoCore
