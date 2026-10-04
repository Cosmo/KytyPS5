#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINECACHEFILE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINECACHEFILE_H_

#include <cstdint>
#include <filesystem>
#include <fmt/format.h>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Libs::Graphics {

// The file a host GPU backend keeps its pipeline cache of the running title in
// (_PipelineCache/<title id><suffix>). Only clean Release builds use it, so its contents always
// come from the emulator build that reads them. The backend's signature ties them to the host
// (driver, shader compiler); a file with another signature or corrupt data is ignored.
class PipelineCacheFile {
public:
	PipelineCacheFile() = default;
	// `label` prefixes the log messages, e.g. "Vulkan pipeline cache".
	PipelineCacheFile(std::string label, std::string_view suffix);

	[[nodiscard]] bool Enabled() const noexcept { return !m_path.empty(); }

	// The payload saved with `signature`; empty when there is none or it cannot be used.
	[[nodiscard]] std::vector<uint8_t> Load(std::string_view signature) const;
	// Replaces the file; false (logged) when it could not be written.
	bool Save(std::string_view signature, std::span<const uint8_t> payload) const;

	template <typename... Args>
	void Log(fmt::format_string<Args...> format, Args&&... args) const {
		LogMessage(fmt::format(format, std::forward<Args>(args)...));
	}

private:
	void LogMessage(const std::string& message) const;

	std::string           m_label;
	std::filesystem::path m_path;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINECACHEFILE_H_
