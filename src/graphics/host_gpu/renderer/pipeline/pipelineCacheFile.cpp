#include "graphics/host_gpu/renderer/pipeline/pipelineCacheFile.h"

#include "common/common.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "kytyGitVersion.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <xxhash.h>

namespace Libs::Graphics {

namespace {

std::string TitleId() {
	std::string title_id;
	if ((!Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) || title_id.empty()) &&
	    (!Loader::SystemContentParamSfoGetString("CONTENT_ID", &title_id) || title_id.empty())) {
		return {};
	}
	if (!std::ranges::all_of(title_id, [](unsigned char c) {
		    return std::isalnum(c) != 0 || c == '-' || c == '_';
	    })) {
		return {};
	}
	return title_id;
}

} // namespace

PipelineCacheFile::PipelineCacheFile(std::string label, std::string_view suffix)
    : m_label(std::move(label)) {
	const auto title_id = TitleId();
	if (title_id.empty()) {
		return;
	}
	if (KYTY_BUILD != KYTY_BUILD_RELEASE) {
		Log("disabled (non-Release build)");
		return;
	}
	const std::string_view git_hash     = KYTY_GIT_HASH;
	const std::string_view git_revision = KYTY_GIT_REVISION;
	if (git_hash == "unknown" || git_revision == "unknown") {
		Log("disabled (unknown git revision)");
		return;
	}
	if (git_hash.ends_with("-dirty")) {
		Log("disabled (dirty build)");
		return;
	}
	m_path = std::filesystem::path("_PipelineCache") / (title_id + std::string(suffix));
}

void PipelineCacheFile::LogMessage(const std::string& message) const {
	Log::WriteToConsoleAndLog(m_label + ": " + message + "\n");
}

std::vector<uint8_t> PipelineCacheFile::Load(std::string_view signature) const {
	if (!Enabled()) {
		return {};
	}
	const auto path = Common::PathToString(m_path);
	if (!Common::File::IsFileExisting(m_path)) {
		Log("initializing {}", path);
		return {};
	}
	Log("loading {}", path);
	Common::File file(m_path, Common::File::Mode::Read);
	const auto   file_size = file.IsInvalid() ? 0 : file.Size();
	if (file_size < signature.size() + sizeof(uint64_t) ||
	    file_size > std::numeric_limits<uint32_t>::max()) {
		file.Close();
		Log("invalidating {} (invalid file size)", path);
		return {};
	}
	std::string          cached_signature(signature.size(), '\0');
	uint64_t             payload_hash = 0;
	std::vector<uint8_t> payload(file_size - signature.size() - sizeof(payload_hash));
	uint32_t             signature_read = 0;
	uint32_t             hash_read      = 0;
	uint32_t             payload_read   = 0;
	file.Read(cached_signature.data(), static_cast<uint32_t>(cached_signature.size()),
	          &signature_read);
	file.Read(&payload_hash, sizeof(payload_hash), &hash_read);
	file.Read(payload.data(), static_cast<uint32_t>(payload.size()), &payload_read);
	file.Close();
	if (signature_read != cached_signature.size() || hash_read != sizeof(payload_hash) ||
	    payload_read != payload.size() || cached_signature != signature ||
	    XXH3_64bits(payload.data(), payload.size()) != payload_hash) {
		Log("invalidating {} (driver, emulator, or data mismatch)", path);
		return {};
	}
	return payload;
}

bool PipelineCacheFile::Save(std::string_view signature,
                             std::span<const uint8_t> payload) const {
	if (!Enabled()) {
		return false;
	}
	if (payload.size() > std::numeric_limits<uint32_t>::max()) {
		Log("save failed ({} bytes)", payload.size());
		return false;
	}
	std::string prefix(signature);
	const auto  payload_hash = XXH3_64bits(payload.data(), payload.size());
	prefix.append(reinterpret_cast<const char*>(&payload_hash), sizeof(payload_hash));
	if (!Common::File::CreateDirectories(m_path.parent_path())) {
		Log("failed to create cache directory");
		return false;
	}
	auto temp_path = m_path;
	temp_path += ".tmp";
	Common::File file;
	uint32_t     prefix_written  = 0;
	uint32_t     payload_written = 0;
	if (file.Create(temp_path)) {
		file.Write(prefix.data(), static_cast<uint32_t>(prefix.size()), &prefix_written);
		file.Write(payload.data(), static_cast<uint32_t>(payload.size()), &payload_written);
	}
	const bool flushed = !file.IsInvalid() && file.Flush();
	file.Close();
	if (prefix_written != prefix.size() || payload_written != payload.size() || !flushed ||
	    !Common::File::RenameFile(temp_path, m_path)) {
		Log("failed to write {}", Common::PathToString(m_path));
		return false;
	}
	Log("saved {} bytes to {}", payload.size(), Common::PathToString(m_path));
	return true;
}

} // namespace Libs::Graphics
