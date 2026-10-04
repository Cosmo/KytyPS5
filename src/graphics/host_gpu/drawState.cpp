#include "graphics/host_gpu/drawState.h"

#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"

namespace Libs::Graphics {

bool DrawHasValidVertexShader(const HW::Shader& shaders) {
	return shaders.GetVs().es_regs.data_addr != 0;
}

bool PixelShaderHasDepthOrCoverageSideEffects(const HW::ShaderRegisters& sh_regs) {
	const auto& db = sh_regs.db_shader_control;
	return db.shader_kill_enable || db.shader_z_export_enable || db.shader_mask_export_enable ||
	       db.shader_dual_export_enable || db.shader_execute_on_noop;
}

uint32_t DrawColorOutputMask(const HW::Context& registers) {
	const auto& sh_regs     = registers.GetShaderRegisters();
	const auto  write_mask  = registers.GetRenderTargetMask() & sh_regs.m_cbShaderMask;
	uint32_t    output_mask = 0;
	for (uint32_t slot = 0; slot < GuestColorTargetCount; slot++) {
		if (sh_regs.target_output_mode[slot] != 0 && ((write_mask >> (slot * 4u)) & 0x0fu) != 0) {
			output_mask |= 1u << slot;
		}
	}
	return output_mask;
}

bool DrawHasActivePixelShader(const HW::Context& registers, const HW::Shader& shaders) {
	return shaders.GetPs().ps_regs.data_addr != 0 &&
	       (DrawColorOutputMask(registers) != 0 ||
	        PixelShaderHasDepthOrCoverageSideEffects(registers.GetShaderRegisters()));
}

std::array<Prospero::ColorComponentMapping, GuestColorTargetCount>
RenderTargetExportMapping(const HW::Context& registers) {
	std::array<Prospero::ColorComponentMapping, GuestColorTargetCount> mapping {};
	const auto output_mask = DrawColorOutputMask(registers);
	for (uint32_t slot = 0; slot < GuestColorTargetCount; slot++) {
		const auto& rt = registers.GetRenderTarget(slot);
		if ((output_mask & (1u << slot)) != 0 && rt.base.addr != 0) {
			mapping[slot] = TextureGetRenderTargetFormat(rt.info.format, rt.info.channel_type,
			                                             rt.info.channel_order)
			                    .export_mapping;
		}
	}
	return mapping;
}

} // namespace Libs::Graphics
