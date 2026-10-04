#include "graphics/host_gpu/d3d12/dxilCompiler.h"

#include "common/assert.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/d3d12/d3d12Common.h"

#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <cstring>
#include <dxcapi.h>
#include <filesystem>
#include <fmt/format.h>
#include <fstream>
#include <iterator>
#include <spirv_to_dxil.h>
#include <string>
#include <xxhash.h>

namespace Libs::Graphics::D3D12 {

namespace {

// Wraps a byte range for IDxcValidator, which only reads and edits it in place.
class ByteBlob final : public IDxcBlob {
public:
	ByteBlob(void* data, size_t size): m_data(data), m_size(size) {}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
		if (riid == __uuidof(IUnknown) || riid == __uuidof(IDxcBlob)) {
			*out = static_cast<IDxcBlob*>(this);
			return S_OK;
		}
		*out = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
	ULONG STDMETHODCALLTYPE Release() override { return 1; }
	LPVOID STDMETHODCALLTYPE GetBufferPointer() override { return m_data; }
	SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return m_size; }

private:
	void*  m_data;
	size_t m_size;
};

// Hash of a loaded module's file, which identifies its build.
uint64_t ModuleFileHash(const wchar_t* module_name) {
	auto* module = GetModuleHandleW(module_name);
	EXIT_IF(module == nullptr);
	std::wstring path(MAX_PATH, L'\0');
	for (;;) {
		const auto length =
		    GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
		EXIT_IF(length == 0);
		if (length < path.size()) {
			path.resize(length);
			break;
		}
		path.resize(path.size() * 2);
	}
	std::ifstream        file(path, std::ios::binary);
	std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
	                           std::istreambuf_iterator<char>());
	EXIT_IF(bytes.empty());
	return XXH3_64bits(bytes.data(), bytes.size());
}

// Collects the parts of a translation's cache key.
class CacheKey {
public:
	void Add(uint64_t value) { m_parts.push_back(value); }
	void Add(std::span<const uint32_t> spirv) {
		m_parts.push_back(XXH3_64bits(spirv.data(), spirv.size_bytes()));
	}
	[[nodiscard]] uint64_t Hash() const {
		return XXH3_64bits(m_parts.data(), m_parts.size() * sizeof(uint64_t));
	}

private:
	std::vector<uint64_t> m_parts;
};

dxil_spirv_shader_stage SpirvStage(ShaderType stage) {
	switch (stage) {
		case ShaderType::Vertex: return DXIL_SPIRV_SHADER_VERTEX;
		case ShaderType::Pixel: return DXIL_SPIRV_SHADER_FRAGMENT;
		case ShaderType::Compute: return DXIL_SPIRV_SHADER_COMPUTE;
		default: return DXIL_SPIRV_SHADER_NONE;
	}
}

} // namespace

std::string SaveShaderDump(const std::string& name, std::span<const std::span<const uint8_t>> dxil,
                           std::span<const std::span<const uint32_t>> spirv) {
	const std::filesystem::path folder = "_ShaderDumps";
	(void)Common::File::CreateDirectories(folder);
	const auto write = [](const std::filesystem::path& path, const void* data, size_t size) {
		std::ofstream(path, std::ios::binary).write(static_cast<const char*>(data),
		                                            static_cast<std::streamsize>(size));
	};
	for (size_t i = 0; i < dxil.size(); i++) {
		write(folder / fmt::format("{}.{}.dxil", name, i), dxil[i].data(), dxil[i].size_bytes());
	}
	for (size_t i = 0; i < spirv.size(); i++) {
		write(folder / fmt::format("{}.{}.spv", name, i), spirv[i].data(), spirv[i].size_bytes());
	}
	return std::filesystem::absolute(folder).string();
}

DxilCompiler::DxilCompiler(ID3D12Device* device) {
	D3D12_FEATURE_DATA_SHADER_MODEL model {D3D_SHADER_MODEL_6_8};
	while (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))) &&
	       model.HighestShaderModel > D3D_SHADER_MODEL_6_0) {
		model.HighestShaderModel = static_cast<D3D_SHADER_MODEL>(model.HighestShaderModel - 1);
	}
	const uint32_t device_minor = static_cast<uint32_t>(model.HighestShaderModel) & 0xfu;

#if defined(KYTY_PLATFORM_UWP)
	// A UWP app loads the DLLs of its package this way.
	auto* dxil = LoadPackagedLibrary(L"dxil.dll", 0);
#else
	auto* dxil = LoadLibraryW(L"dxil.dll");
#endif
	if (dxil == nullptr) {
		EXIT("D3D12: dxil.dll (DXIL validator) is missing next to the emulator\n");
	}
	m_dxil_module = dxil;
	auto* create  = reinterpret_cast<DxcCreateInstanceProc>(
        reinterpret_cast<void*>(GetProcAddress(dxil, "DxcCreateInstance")));
	EXIT_IF(create == nullptr);
	Check(create(CLSID_DxcValidator, IID_PPV_ARGS(&m_validator)), "create DXIL validator");

	UINT32                          major = 0;
	UINT32                          minor = 0;
	ComPtr<IDxcVersionInfo> version;
	Check(m_validator->QueryInterface(IID_PPV_ARGS(&version)), "query DXIL validator version");
	Check(version->GetVersion(&major, &minor), "read DXIL validator version");
	EXIT_IF(major != 1);
	minor = std::min<uint32_t>(minor, DXIL_VALIDATOR_1_8 - DXIL_VALIDATOR_1_0);

	// A validator 1.x signs shader model 6.x up to the same minor version.
	const auto model_minor = std::min({device_minor, static_cast<uint32_t>(minor),
	                                   static_cast<uint32_t>(SHADER_MODEL_6_8 - SHADER_MODEL_6_0)});
	m_shader_model      = SHADER_MODEL_6_0 + model_minor;
	m_validator_version = DXIL_VALIDATOR_1_0 + minor;
	m_translator_hash   = ModuleFileHash(L"spirv_to_dxil.dll");
	Log::WriteToConsoleAndLog(
	    fmt::format("D3D12 shaders: shader model 6.{}, DXIL validator 1.{}\n", model_minor, minor));
}

DxilCompiler::~DxilCompiler() {
	if (m_validator != nullptr) {
		m_validator->Release();
	}
	if (m_dxil_module != nullptr) {
		FreeLibrary(static_cast<HMODULE>(m_dxil_module));
	}
}

static_assert(sizeof(VertexRuntimeData) == sizeof(dxil_spirv_vertex_runtime_data));
static_assert(offsetof(VertexRuntimeData, yz_flip_mask) ==
              offsetof(dxil_spirv_vertex_runtime_data, yz_flip_mask));
static_assert(offsetof(VertexRuntimeData, depth_bias) ==
              offsetof(dxil_spirv_vertex_runtime_data, depth_bias));
static_assert(ZFlipShift == DXIL_SPIRV_Z_FLIP_SHIFT);

namespace {

dxil_spirv_runtime_conf RuntimeConf(uint32_t shader_model, bool flip,
                                    bool sample_rate_shading = false, bool clip_halfz = false) {
	dxil_spirv_runtime_conf conf {};
	conf.runtime_data_cbv.register_space        = RuntimeDataSpace;
	conf.runtime_data_cbv.base_shader_register  = 0;
	conf.push_constant_cbv.register_space       = PushConstantSpace;
	conf.push_constant_cbv.base_shader_register = 0;
	conf.first_vertex_and_base_instance_mode    = DXIL_SPIRV_SYSVAL_TYPE_RUNTIME_DATA;
	conf.workgroup_id_mode                      = DXIL_SPIRV_SYSVAL_TYPE_NATIVE;
	conf.yz_flip.mode = flip ? DXIL_SPIRV_YZ_FLIP_CONDITIONAL : DXIL_SPIRV_YZ_FLIP_NONE;
	conf.shader_model_max          = static_cast<dxil_shader_model>(shader_model);
	conf.force_sample_rate_shading = sample_rate_shading;
	conf.clip_halfz                  = clip_halfz;
	conf.shadow_sampler_space_offset = ComparisonSamplerSpaceOffset;
	return conf;
}

DxilShader TakeObject(dxil_spirv_object& object) {
	DxilShader result;
	result.requires_runtime_data = object.metadata.requires_runtime_data;
	result.needs_draw_sysvals    = object.metadata.needs_draw_sysvals;
	const auto* bytes            = static_cast<const uint8_t*>(object.binary.buffer);
	result.bytecode.assign(bytes, bytes + object.binary.size);
	spirv_to_dxil_free(&object);
	return result;
}

} // namespace

DxilShader DxilCompiler::Compile(std::span<const uint32_t> spirv, ShaderType stage,
                                 uint64_t shader_hash, bool last_vertex_stage,
                                 std::span<const SpecializationConstant> constants,
                                 bool* from_cache) const {
	CacheKey key;
	key.Add(0); // a single stage
	key.Add(spirv);
	key.Add(static_cast<uint64_t>(stage));
	key.Add(last_vertex_stage ? 1 : 0);
	for (const auto& constant: constants) {
		key.Add((static_cast<uint64_t>(constant.id) << 32u) | constant.value);
	}
	const auto        hash = key.Hash();
	Common::LockGuard lock(m_cache_mutex);
	if (from_cache != nullptr) {
		*from_cache = m_loaded.contains(hash);
	}
	if (const auto found = m_cache.find(hash); found != m_cache.end()) {
		return found->second.front();
	}
	auto shader = Translate(spirv, stage, shader_hash, last_vertex_stage, constants);
	m_cache.emplace(hash, std::vector {shader});
	return shader;
}

bool DxilCompiler::SupportsMeshShaders() const noexcept {
	return m_shader_model >= SHADER_MODEL_6_5;
}

std::vector<DxilShader> DxilCompiler::CompileLinked(std::span<const LinkedStage> stages,
                                                    uint64_t pipeline_hash,
                                                    bool*    from_cache) const {
	CacheKey key;
	key.Add(stages.size());
	for (const auto& stage: stages) {
		key.Add(stage.spirv);
		key.Add(static_cast<uint64_t>(stage.kind) | (stage.flip ? 0x100u : 0u) |
		        (stage.sample_rate_shading ? 0x200u : 0u) | (stage.clip_halfz ? 0x400u : 0u));
	}
	const auto        hash = key.Hash();
	Common::LockGuard lock(m_cache_mutex);
	if (from_cache != nullptr) {
		*from_cache = m_loaded.contains(hash);
	}
	if (const auto found = m_cache.find(hash); found != m_cache.end()) {
		return found->second;
	}
	auto shaders = TranslateLinked(stages, pipeline_hash);
	m_cache.emplace(hash, shaders);
	return shaders;
}

std::string DxilCompiler::CacheSignature() const {
	return fmt::format("spirv_to_dxil={:016x} sm=6.{} validator=1.{}", m_translator_hash,
	                   m_shader_model - SHADER_MODEL_6_0, m_validator_version - DXIL_VALIDATOR_1_0);
}

// Saved translations: u32 entry count, then per entry u64 key, u32 shader count and per shader
// u8 flags (bit 0: requires runtime data, bit 1: needs draw sysvals), u32 size, DXIL.
std::vector<uint8_t> DxilCompiler::SaveCache() const {
	std::vector<uint8_t> data;
	const auto           append = [&](const void* bytes, size_t size) {
        const auto* begin = static_cast<const uint8_t*>(bytes);
        data.insert(data.end(), begin, begin + size);
	};
	Common::LockGuard lock(m_cache_mutex);
	const auto        entries = static_cast<uint32_t>(m_cache.size());
	append(&entries, sizeof(entries));
	for (const auto& [key, shaders]: m_cache) {
		const auto count = static_cast<uint32_t>(shaders.size());
		append(&key, sizeof(key));
		append(&count, sizeof(count));
		for (const auto& shader: shaders) {
			const uint8_t  flags = (shader.requires_runtime_data ? 1u : 0u) |
			                       (shader.needs_draw_sysvals ? 2u : 0u);
			const auto     size  = static_cast<uint32_t>(shader.bytecode.size());
			append(&flags, sizeof(flags));
			append(&size, sizeof(size));
			append(shader.bytecode.data(), size);
		}
	}
	return data;
}

bool DxilCompiler::LoadCache(std::span<const uint8_t> data) {
	size_t     offset = 0;
	const auto read   = [&](void* value, size_t size) {
        if (size > data.size() - offset) {
            return false;
        }
        std::memcpy(value, data.data() + offset, size);
        offset += size;
        return true;
	};
	std::unordered_map<uint64_t, std::vector<DxilShader>> loaded;
	uint32_t                                               entries = 0;
	if (!read(&entries, sizeof(entries))) {
		return false;
	}
	for (uint32_t entry = 0; entry < entries; entry++) {
		uint64_t key   = 0;
		uint32_t count = 0;
		if (!read(&key, sizeof(key)) || !read(&count, sizeof(count)) || count == 0 || count > 3) {
			return false;
		}
		auto& shaders = loaded[key];
		shaders.resize(count);
		for (auto& shader: shaders) {
			uint8_t  flags = 0;
			uint32_t size  = 0;
			if (!read(&flags, sizeof(flags)) || !read(&size, sizeof(size)) ||
			    size > data.size() - offset) {
				return false;
			}
			shader.requires_runtime_data = (flags & 1u) != 0;
			shader.needs_draw_sysvals    = (flags & 2u) != 0;
			shader.bytecode.assign(data.begin() + static_cast<ptrdiff_t>(offset),
			                       data.begin() + static_cast<ptrdiff_t>(offset + size));
			offset += size;
		}
	}
	if (offset != data.size()) {
		return false;
	}
	Common::LockGuard lock(m_cache_mutex);
	for (const auto& entry: loaded) {
		m_loaded.insert(entry.first);
	}
	m_cache.merge(loaded);
	return true;
}

DxilShader DxilCompiler::Translate(std::span<const uint32_t> spirv, ShaderType stage,
                                   uint64_t shader_hash, bool last_vertex_stage,
                                   std::span<const SpecializationConstant> constants) const {
	std::vector<dxil_spirv_specialization> specializations(constants.size());
	for (size_t i = 0; i < constants.size(); i++) {
		specializations[i].id        = constants[i].id;
		specializations[i].value.u32 = constants[i].value;
	}
	if (SpirvStage(stage) == DXIL_SPIRV_SHADER_NONE) {
		Log::WriteToConsoleAndLog(fmt::format("D3D12: shader stage {} is not supported yet, shader 0x{:016x} is skipped\n", static_cast<int>(stage), shader_hash));
		return {};
	}
	const auto conf = RuntimeConf(m_shader_model, last_vertex_stage);

	std::string                    messages;
	const dxil_spirv_logger        logger {&messages, [](void* priv, const char* message) {
		                                   *static_cast<std::string*>(priv) += message;
	                                   }};
	const dxil_spirv_debug_options debug {};
	dxil_spirv_object              object {};
	if (!spirv_to_dxil(spirv.data(), spirv.size(), specializations.data(),
	                   static_cast<unsigned>(specializations.size()), SpirvStage(stage), "main",
	                   static_cast<dxil_validator_version>(m_validator_version), &debug, &conf,
	                   &logger, &object)) {
		Log::WriteToConsoleAndLog(fmt::format("D3D12: SPIR-V to DXIL translation failed, shader 0x{:016x} is skipped:\n{}\n", shader_hash, messages));
		return {};
	}
	auto                            result    = TakeObject(object);
	const std::span<const uint32_t> sources[] = {spirv};
	Sign(result, shader_hash, sources);
	return result;
}

std::vector<DxilShader> DxilCompiler::TranslateLinked(std::span<const LinkedStage> stages,
                                                      uint64_t pipeline_hash) const {
	EXIT_IF(stages.empty() || stages.size() > 3);
	std::vector<dxil_spirv_runtime_conf> confs;
	std::vector<dxil_spirv_stage>        inputs;
	confs.reserve(stages.size());
	for (const auto& stage: stages) {
		confs.push_back(
		    RuntimeConf(m_shader_model, stage.flip, stage.sample_rate_shading, stage.clip_halfz));
		dxil_spirv_stage input {};
		input.words            = stage.spirv.data();
		input.word_count       = stage.spirv.size();
		input.entry_point_name = "main";
		input.conf             = &confs.back();
		switch (stage.kind) {
			case LinkedStage::Kind::Vertex: input.stage = DXIL_SPIRV_SHADER_VERTEX; break;
			case LinkedStage::Kind::Geometry: input.stage = DXIL_SPIRV_SHADER_GEOMETRY; break;
			case LinkedStage::Kind::Pixel: input.stage = DXIL_SPIRV_SHADER_FRAGMENT; break;
			case LinkedStage::Kind::Mesh: input.stage = DXIL_SPIRV_SHADER_MESH; break;
		}
		inputs.push_back(input);
	}

	std::string                    messages;
	const dxil_spirv_logger        logger {&messages, [](void* priv, const char* message) {
		                                   *static_cast<std::string*>(priv) += message;
	                                   }};
	const dxil_spirv_debug_options debug {};
	std::vector<dxil_spirv_object> objects(stages.size());
	if (!spirv_to_dxil_linked(inputs.data(), static_cast<unsigned>(inputs.size()),
	                          static_cast<dxil_validator_version>(m_validator_version), &debug,
	                          &logger, objects.data())) {
		Log::WriteToConsoleAndLog(fmt::format("D3D12: SPIR-V to DXIL translation failed, the shaders of pipeline 0x{:016x} are skipped:\n{}\n",
		                                      pipeline_hash, messages));
		return std::vector<DxilShader>(stages.size());
	}
	std::vector<std::span<const uint32_t>> sources;
	for (const auto& stage: stages) {
		sources.push_back(stage.spirv);
	}
	std::vector<DxilShader> result;
	for (auto& object: objects) {
		result.push_back(TakeObject(object));
		Sign(result.back(), pipeline_hash, sources);
	}
	return result;
}

// Validates the DXIL and signs it in place, as D3D12 requires.
void DxilCompiler::Sign(DxilShader& shader, uint64_t shader_hash,
                        std::span<const std::span<const uint32_t>> sources) const {
	ByteBlob                    blob(shader.bytecode.data(), shader.bytecode.size());
	ComPtr<IDxcOperationResult> validation;
	Check(m_validator->Validate(&blob, DxcValidatorFlags_InPlaceEdit, &validation),
	      "validate DXIL");
	HRESULT status = E_FAIL;
	Check(validation->GetStatus(&status), "read DXIL validation status");
	if (FAILED(status)) {
		ComPtr<IDxcBlobEncoding> errors;
		std::string              text;
		if (SUCCEEDED(validation->GetErrorBuffer(&errors)) && errors != nullptr) {
			text.assign(static_cast<const char*>(errors->GetBufferPointer()),
			            errors->GetBufferSize());
		}
		const std::span<const uint8_t> dxil[] = {shader.bytecode};
		const auto dump = SaveShaderDump(fmt::format("{:016x}", shader_hash), dxil, sources);
		Log::WriteToConsoleAndLog(fmt::format("D3D12: DXIL validation failed, shader 0x{:016x} is skipped (saved to {}):\n{}\n", shader_hash, dump, text));
		shader.bytecode.clear();
	}
}

} // namespace Libs::Graphics::D3D12
