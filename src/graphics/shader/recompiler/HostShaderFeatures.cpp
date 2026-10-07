#include "graphics/shader/recompiler/HostShaderFeatures.h"

// The features of the host GPU's shaders, which the backend sets before a shader is compiled. Kept apart from the recompiler so that the tests that
// run passes alone can link it.
namespace Libs::Graphics::ShaderRecompiler {

namespace {

HostShaderFeatures g_host_shader_features;

} // namespace

void SetHostShaderFeatures(const HostShaderFeatures& features) {
	g_host_shader_features = features;
}

const HostShaderFeatures& GetHostShaderFeatures() {
	return g_host_shader_features;
}

} // namespace Libs::Graphics::ShaderRecompiler
