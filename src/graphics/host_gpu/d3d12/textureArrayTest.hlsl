// D3D12 self test (textureArrayTest in selfTest.cpp): reads the centre texel of every (layer, mip) of a 2D texture array, once with Load and
// once with SampleLevel through a point sampler. Compiled offline into textureArrayTest.h:
//   dxc -T cs_6_0 -E main -Qstrip_reflect -Fh textureArrayTest.h -Vn TEXTURE_ARRAY_TEST_DXIL textureArrayTest.hlsl
// (with dxil.dll next to dxc, so that the output is signed).

#define ROOT_SIGNATURE                                                                                                   \
	"RootConstants(num32BitConstants = 4, b0), DescriptorTable(SRV(t0)), UAV(u0), "                                      \
	"StaticSampler(s0, filter = FILTER_MIN_MAG_MIP_POINT, addressU = TEXTURE_ADDRESS_CLAMP, "                            \
	"addressV = TEXTURE_ADDRESS_CLAMP, addressW = TEXTURE_ADDRESS_CLAMP)"

cbuffer Parameters : register(b0) {
	uint layers;
	uint mips;
	uint width;
	uint height;
};

Texture2DArray<float4>     tex : register(t0);
SamplerState               point_sampler : register(s0);
RWStructuredBuffer<float4> results : register(u0);

[RootSignature(ROOT_SIGNATURE)]
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
	if (id.x >= layers * mips) {
		return;
	}
	const uint layer = id.x / mips;
	const uint mip   = id.x % mips;
	const uint w     = max(width >> mip, 1u);
	const uint h     = max(height >> mip, 1u);
	results[id.x * 2 + 0] = tex.Load(int4(w / 2, h / 2, layer, mip));
	const float2 uv       = (float2(w / 2, h / 2) + 0.5) / float2(w, h);
	results[id.x * 2 + 1] = tex.SampleLevel(point_sampler, float3(uv, layer), mip);
}
