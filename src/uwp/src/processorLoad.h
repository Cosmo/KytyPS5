#ifndef EMULATOR_SRC_UWP_PROCESSORLOAD_H_
#define EMULATOR_SRC_UWP_PROCESSORLOAD_H_

#include <cstdint>
#include <vector>

// The processor as the app sees it, for the overlay's CPU panel.
namespace Kyty::Uwp {

// A core: its logical processors, by number.
struct ProcessorCore {
	std::vector<uint32_t> processors;
};

// The cores, read once; empty where Windows doesn't tell.
[[nodiscard]] const std::vector<ProcessorCore>& ProcessorCores();

// For each core (as ProcessorCores), whether the system keeps it for itself: reserved, but not for this app. On the Xbox one core is; on a PC
// none. Read anew each time: the Xbox reserves the app's cores a moment after it starts (on the Series X, half a second in, cores 1 to 3 were
// reserved but not yet for the app and core 0 was free; a second later they were the app's and core 0 the system's). The log says each change.
[[nodiscard]] std::vector<bool> SystemCores();

// Each logical processor's load since the last sample, 0 to 1, by processor number. It reads each processor's idle time with
// QueryIdleProcessorCycleTime, a function for desktop programs that UWP apps can't count on: the Xbox has it (tried on the Series X); where
// it's missing, Available is false and Sample empty.
class ProcessorLoad {
public:
	[[nodiscard]] static bool Available();
	// Empty the first time, and when Windows doesn't answer.
	[[nodiscard]] std::vector<double> Sample();

private:
	std::vector<uint64_t> m_idle;       // each processor's idle cycles at the last sample
	uint64_t              m_cycles = 0; // the time stamp counter then
};

} // namespace Kyty::Uwp

#endif // EMULATOR_SRC_UWP_PROCESSORLOAD_H_
