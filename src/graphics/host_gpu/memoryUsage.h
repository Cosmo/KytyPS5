#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORYUSAGE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORYUSAGE_H_

#include <cstdint>

namespace Libs::Graphics {

// Where a host buffer lives and how the CPU accesses it.
enum class MemoryUsage : uint8_t {
	DeviceLocal, // GPU memory, no CPU access
	Upload,      // CPU writes, GPU reads once (staging)
	Download,    // GPU writes, CPU reads (readback)
	Stream,      // CPU writes, GPU reads directly
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_MEMORYUSAGE_H_
