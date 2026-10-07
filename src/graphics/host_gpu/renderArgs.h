#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERARGS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERARGS_H_

#include <cstdint>

// Backend-neutral arguments passed from the guest command processor to the renderer.

namespace Libs::Graphics {

enum class CommandBufferDebugOp : uint32_t {
	DispatchDirect,
	DrawIndex,
	DrawIndexAuto,
	EopWrite,
	EopInterrupt,
	EopWriteBack,
	EopFlip,
	EopWriteBackFlip,
	EopOnlyFlip,
	DispatchIndirect,
	Unknown,
};

enum class DrawOffsetSource : uint8_t {
	DrawRegisters, // Not "DrawState": windows.h defines a DrawState macro.
	IndirectArgs,
};

// The layout of the indirect dispatch arguments in guest memory.
struct DispatchIndirectArgs {
	uint32_t x = 0;
	uint32_t y = 0;
	uint32_t z = 0;
};

struct DrawIndexArgs {
	uint32_t         index_count                = 0;
	const void*      index_addr                 = nullptr;
	uint32_t         instance_count             = 0;
	uint32_t         index_type_and_size        = 0;
	int32_t          base_vertex                = 0;
	uint32_t         first_instance             = 0;
	DrawOffsetSource offset_source              = DrawOffsetSource::DrawRegisters;
	uint32_t         render_target_slice_offset = 0;
};

struct DrawAutoArgs {
	uint32_t         vertex_count               = 0;
	uint32_t         instance_count             = 0;
	uint32_t         first_vertex               = 0;
	uint32_t         first_instance             = 0;
	DrawOffsetSource offset_source              = DrawOffsetSource::DrawRegisters;
	uint32_t         render_target_slice_offset = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERARGS_H_
