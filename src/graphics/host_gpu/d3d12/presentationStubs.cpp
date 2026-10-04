#include "graphics/presentation/renderDoc.h"

// The RenderDoc integration is Vulkan only; the D3D12 backend has none.

namespace Libs::Graphics {

void RenderDocInit() {}

void RenderDocRequestCapture() {}

void RenderDocOnGuestFlip(RenderContext& /*renderer*/) {}

} // namespace Libs::Graphics
