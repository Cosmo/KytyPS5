#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_DESCRIPTORCOMMIT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_DESCRIPTORCOMMIT_H_

#include "graphics/host_gpu/renderer/pipeline/descriptors.h"
#include "graphics/host_gpu/vulkanCommon.h"

namespace Libs::Graphics {

[[nodiscard]] vk::DescriptorType
NativeDescriptorType(ShaderRecompiler::IR::DescriptorBindingKind kind);
[[nodiscard]] vk::DescriptorImageInfo MakeImageInfo(const TextureBinding& texture,
                                                    uint32_t              element = 0);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_DESCRIPTORCOMMIT_H_
