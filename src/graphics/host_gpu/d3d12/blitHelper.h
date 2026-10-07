#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_BLITHELPER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_BLITHELPER_H_

#include "common/assert.h"
#include "common/common.h"

namespace Libs::Graphics {

class CommandScheduler;
class Image;
struct GraphicContext;

// Graphics-pipeline image conversions. The one the texture cache needs (color reinterpreted as
// multisampled depth) is not implemented by this backend yet.
class BlitHelper {
public:
	BlitHelper(GraphicContext& /*graphics*/, CommandScheduler& /*scheduler*/) {}
	KYTY_CLASS_NO_COPY(BlitHelper);

	void ReinterpretColorAsMsDepth(Image& /*source*/, Image& /*destination*/) {
		EXIT("D3D12: reinterpreting color as multisampled depth is not supported yet\n");
	}
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_D3D12_BLITHELPER_H_
