// The emulator's window and presentation for the UWP app, in place of the SDL window (graphics/presentation/window/window.cpp). For now only the entry
// points the rest of the emulator calls; there is no host GPU backend in this build yet.
#include "graphics/presentation/window.h"

#include "common/assert.h"
#include "graphics/presentation/window/windowInternal.h"

namespace Libs::Graphics {

Presenter& WindowInit(uint32_t /*width*/, uint32_t /*height*/) {
	EXIT("the UWP build has no host GPU backend yet\n");
}

void WindowRun() {}

void WindowShutdown() {}

void WindowContext::UpdateTitle() {}

} // namespace Libs::Graphics
