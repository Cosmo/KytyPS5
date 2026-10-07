#ifndef EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_MESHEMULATION_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_MESHEMULATION_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

// A host without mesh shaders (the Series X) runs a guest merged geometry (NGG) program as a
// compute shader: one workgroup per mesh group writes the group's vertices (records) and triangles
// (indices) into fixed slots of two buffers, and a generated vertex stage reads the records back
// for one indexed draw of all the slots. Triangles a group does not make, or culls, are degenerate.
struct MeshEmulation {
	// Register spaces of the two buffers (root descriptors), outside the guest resource spaces.
	static constexpr uint32_t RecordSpace = 1002;
	static constexpr uint32_t IndexSpace  = 1003;

	// The draw parameters of a mesh program (PushData::MeshDrawDwordCount of them describe the
	// draw), then the first group and the first instance of the chunk the dispatch runs, and the
	// width of the dispatch in groups.
	static constexpr uint32_t GroupBaseParameter    = PushData::MeshDrawDwordCount;
	static constexpr uint32_t InstanceBaseParameter = GroupBaseParameter + 1;
	static constexpr uint32_t ChunkWidthParameter   = GroupBaseParameter + 2;
	static constexpr uint32_t DrawDwordCount        = GroupBaseParameter + 3;

	// Where an output lives in a record: a vec4 slot, and the component of it (distances only).
	struct Slot {
		uint32_t vec4      = 0;
		uint32_t component = 0;
	};

	// A record is `stride` vec4 slots of four dwords: one slot per position, parameter and layer
	// output, then the clip distances and the cull distances, four to a slot.
	struct RecordLayout {
		uint32_t          stride = 0;
		std::vector<Slot> outputs; // per output of the program, in order

		[[nodiscard]] uint32_t StrideDwords() const noexcept { return stride * 4u; }
	};

	[[nodiscard]] static RecordLayout LayoutFor(std::span<const StageOutput> outputs) {
		RecordLayout layout;
		layout.outputs.resize(outputs.size());
		uint32_t clip_count = 0;
		uint32_t cull_count = 0;
		for (size_t i = 0; i < outputs.size(); i++) {
			switch (outputs[i].kind) {
				case StageOutputKind::ClipDistance:
					clip_count = std::max(clip_count, outputs[i].index + 1u);
					break;
				case StageOutputKind::CullDistance:
					cull_count = std::max(cull_count, outputs[i].index + 1u);
					break;
				default: layout.outputs[i] = {layout.stride++, 0}; break;
			}
		}
		const uint32_t clip_base = layout.stride;
		const uint32_t cull_base = clip_base + (clip_count + 3u) / 4u;
		layout.stride            = cull_base + (cull_count + 3u) / 4u;
		for (size_t i = 0; i < outputs.size(); i++) {
			if (outputs[i].kind == StageOutputKind::ClipDistance) {
				layout.outputs[i] = {clip_base + outputs[i].index / 4u, outputs[i].index % 4u};
			} else if (outputs[i].kind == StageOutputKind::CullDistance) {
				layout.outputs[i] = {cull_base + outputs[i].index / 4u, outputs[i].index % 4u};
			}
		}
		return layout;
	}
};

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif // EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_MESHEMULATION_H_
