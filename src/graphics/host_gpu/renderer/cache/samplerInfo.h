#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_SAMPLERINFO_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_SAMPLERINFO_H_

#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/shaderBindings.h"

namespace Libs::Graphics {

// The host sampler state of a guest sampler descriptor, shared by all host GPU backends.
// `integer_border` picks integer border colors (for integer textures), `max_lod_bias` is the host limit the guest bias is clamped to.
[[nodiscard]] vk::SamplerCreateInfo GuestSamplerInfo(const ShaderSamplerResource& r, bool integer_border, float max_lod_bias);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_SAMPLERINFO_H_
