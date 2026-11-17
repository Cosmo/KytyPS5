#include "processorLoad.h"

#include "log.h"

#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace Kyty::Uwp {

namespace {

// Looked up, not linked: the headers for UWP apps don't declare it.
using QueryIdleFunction = BOOL(WINAPI*)(PULONG, PULONG64);

QueryIdleFunction QueryIdle() {
	static const auto function = [] {
		HMODULE module = GetModuleHandleW(L"kernelbase.dll");
		return module != nullptr ? reinterpret_cast<QueryIdleFunction>(GetProcAddress(module, "QueryIdleProcessorCycleTime")) : nullptr;
	}();
	return function;
}

// One processor group: the Xbox and UWP apps see at most 64 logical processors.
constexpr uint32_t MAX_PROCESSORS = 64;

std::vector<ProcessorCore> ReadCores() {
	std::vector<ProcessorCore> cores;
	DWORD                      length = 0;
	GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
	std::vector<uint8_t> buffer(length);
	if (length == 0 ||
	    !GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &length)) {
		return cores;
	}
	for (DWORD offset = 0; offset < length;) {
		const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
		ProcessorCore core;
		for (uint32_t processor = 0; processor < MAX_PROCESSORS; processor++) {
			if (((entry->Processor.GroupMask[0].Mask >> processor) & 1u) != 0) {
				core.processors.push_back(processor);
			}
		}
		cores.push_back(std::move(core));
		offset += entry->Size;
	}
	return cores;
}

} // namespace

const std::vector<ProcessorCore>& ProcessorCores() {
	static const auto cores = ReadCores();
	return cores;
}

std::vector<bool> SystemCores() {
	const auto&       cores = ProcessorCores();
	std::vector<bool> system(cores.size(), false);
	ULONG             size = 0;
	GetSystemCpuSetInformation(nullptr, 0, &size, GetCurrentProcess(), 0);
	std::vector<uint8_t> sets(size);
	if (size == 0 || !GetSystemCpuSetInformation(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(sets.data()), size, &size, GetCurrentProcess(), 0)) {
		return system;
	}
	// The logical processors reserved, but not for this app; and each one's state for the log.
	std::vector<bool> others(MAX_PROCESSORS, false);
	std::string       states;
	for (ULONG offset = 0; offset < size;) {
		const auto* entry = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(sets.data() + offset);
		const auto& set   = entry->CpuSet;
		if (set.Group == 0 && set.LogicalProcessorIndex < MAX_PROCESSORS && set.Allocated != 0 && set.AllocatedToTargetProcess == 0) {
			others[set.LogicalProcessorIndex] = true;
		}
		char text[48];
		snprintf(text, sizeof(text), " %u:%s", static_cast<unsigned>(set.LogicalProcessorIndex),
		         set.Allocated == 0 ? "free" : (set.AllocatedToTargetProcess != 0 ? "app" : "other"));
		states += text;
		offset += entry->Size;
	}
	for (size_t index = 0; index < cores.size(); index++) {
		const auto& processors = cores[index].processors;
		system[index]          = !processors.empty() &&
		                std::all_of(processors.begin(), processors.end(), [&others](uint32_t processor) { return others[processor]; });
	}
	// Each change, for reports: which logical processors are free, the app's, or reserved for another process.
	static std::string s_logged;
	if (states != s_logged) {
		s_logged = states;
		Log("processor: %zu cores; logical processors%s\n", cores.size(), states.c_str());
	}
	return system;
}

bool ProcessorLoad::Available() {
	return QueryIdle() != nullptr;
}

std::vector<double> ProcessorLoad::Sample() {
	auto* query = QueryIdle();
	if (query == nullptr) {
		return {};
	}
	ULONG bytes = 0;
	query(&bytes, nullptr);
	std::vector<ULONG64> idle(bytes / sizeof(ULONG64));
	bytes                 = static_cast<ULONG>(idle.size() * sizeof(ULONG64));
	const uint64_t cycles = __rdtsc();
	if (idle.empty() || !query(&bytes, idle.data())) {
		m_idle.clear();
		return {};
	}
	// The idle cycles count at the time stamp counter's rate: what is not idle of the cycles since the last sample is the load.
	std::vector<double> load;
	if (m_idle.size() == idle.size() && cycles > m_cycles) {
		const auto elapsed = static_cast<double>(cycles - m_cycles);
		for (size_t i = 0; i < idle.size(); i++) {
			const auto idle_cycles = static_cast<double>(idle[i] >= m_idle[i] ? idle[i] - m_idle[i] : 0);
			load.push_back(std::clamp(1.0 - idle_cycles / elapsed, 0.0, 1.0));
		}
	}
	m_idle.assign(idle.begin(), idle.end());
	m_cycles = cycles;
	return load;
}

} // namespace Kyty::Uwp
