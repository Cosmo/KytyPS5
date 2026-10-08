#include "graphics/host_gpu/d3d12/selfTest.h"

#include "common/logging/log.h"
#include "graphics/host_gpu/d3d12/d3d12Common.h"
#include "graphics/host_gpu/d3d12/dxilCompiler.h"
#include "graphics/host_gpu/d3d12/graphicContext.h"
#include "graphics/host_gpu/d3d12/textureArrayTest.h"

#include <D3D12MemAlloc.h>

#include "gpu_tiler_shaders/gpu_tiler_depth_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_prt_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_render_target_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_standard256_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_standard4_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_standard64_spv.h"
#include "gpu_tiler_shaders/gpu_tiler_color_transform_spv.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>
#include <fmt/format.h>
#include <span>

namespace Libs::Graphics::D3D12 {

namespace {

void Say(const std::string& text) {
	Log::WriteToConsoleAndLog("D3D12 self test: " + text + "\n");
}

// The tiling shaders' resources as spirv_to_dxil lays them out: a read-only storage buffer (t0), a storage buffer (u1), a uniform buffer (b2), their push
// constants (b0 in PushConstantSpace) and the translator's runtime data (b0 in RuntimeDataSpace).
ComPtr<ID3D12RootSignature> CreateRootSignature(ID3D12Device* device) {
	D3D12_ROOT_PARAMETER1 parameters[5] {};
	parameters[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
	parameters[0].Constants.Num32BitValues = 16;
	parameters[0].Constants.RegisterSpace  = PushConstantSpace;
	parameters[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_CBV;
	parameters[1].Descriptor.RegisterSpace = RuntimeDataSpace;
	parameters[2].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_SRV;
	parameters[2].Descriptor.ShaderRegister = 0;
	parameters[3].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_UAV;
	parameters[3].Descriptor.ShaderRegister = 1;
	parameters[4].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_CBV;
	parameters[4].Descriptor.ShaderRegister = 2;

	D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc {};
	desc.Version                = D3D_ROOT_SIGNATURE_VERSION_1_1;
	desc.Desc_1_1.NumParameters = 5;
	desc.Desc_1_1.pParameters   = parameters;

	ComPtr<ID3DBlob> blob;
	ComPtr<ID3DBlob> error;
	Check(D3D12SerializeVersionedRootSignature(&desc, &blob, &error), "serialize root signature");
	ComPtr<ID3D12RootSignature> root_signature;
	Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&root_signature)), "CreateRootSignature");
	return root_signature;
}

// Texture arrays: on the Series X, block-compressed arrays read back wrong where a layer's mip level is smaller than 64 KB (the copies that upload
// them and the texture unit disagree on how such layers share 64 KB tiles; texture-to-texture copies and copies of a CPU-written texture are just as
// wrong). Every (layer, mip) of a test array is filled with its own solid colour (for BC, blocks whose two endpoints are equal), uploaded from a
// buffer per subresource like the texture cache, or written by the CPU into a CPU-visible texture (what d3d12/image.cpp does where copies fail),
// and a compute shader reads the centre texel of each with Load and with SampleLevel. Uncompressed arrays are the control.

enum class ArrayKind { Bc6h, Bc7, Rgba8, Rgba16f };
enum class Upload { CopyFromBuffer, CpuWrite };

struct ArrayCase {
	ArrayKind kind;
	uint32_t  size;
	uint32_t  mips;
	uint32_t  layers;
	Upload    upload;
	bool      like_renderer; // CopyFromBuffer only: through the memory allocator and typeless, as d3d12/image.cpp makes images
};

const char* KindName(ArrayKind kind) {
	switch (kind) {
		case ArrayKind::Bc6h: return "BC6H";
		case ArrayKind::Bc7: return "BC7";
		case ArrayKind::Rgba8: return "RGBA8";
		default: return "RGBA16F";
	}
}

const char* UploadName(const ArrayCase& test) {
	switch (test.upload) {
		case Upload::CopyFromBuffer: return test.like_renderer ? "copy from a buffer, typeless via the allocator like the renderer" : "copy from a buffer, committed";
		default: return "CPU write into a CPU-visible texture";
	}
}

DXGI_FORMAT ViewFormat(ArrayKind kind) {
	switch (kind) {
		case ArrayKind::Bc6h: return DXGI_FORMAT_BC6H_UF16;
		case ArrayKind::Bc7: return DXGI_FORMAT_BC7_UNORM;
		case ArrayKind::Rgba8: return DXGI_FORMAT_R8G8B8A8_UNORM;
		default: return DXGI_FORMAT_R16G16B16A16_FLOAT;
	}
}

DXGI_FORMAT FamilyFormat(ArrayKind kind) {
	switch (kind) {
		case ArrayKind::Bc6h: return DXGI_FORMAT_BC6H_TYPELESS;
		case ArrayKind::Bc7: return DXGI_FORMAT_BC7_TYPELESS;
		case ArrayKind::Rgba8: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
		default: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
	}
}

// Writes value (bits wide) at bit position of a 16-byte block, least significant bit first.
void PutBits(std::array<uint8_t, 16>& block, uint32_t& position, uint32_t value, uint32_t bits) {
	for (uint32_t i = 0; i < bits; i++, position++) {
		if (((value >> i) & 1u) != 0) {
			block[position / 8] |= static_cast<uint8_t>(1u << (position % 8));
		}
	}
}

// The half a BC6H_UF16 decoder gives for a 10-bit endpoint (unquantize, then finish_unquantize).
uint16_t Bc6hHalf(uint32_t endpoint) {
	const uint32_t unquantized = endpoint == 0 ? 0 : endpoint == 1023 ? 0xffff : ((endpoint << 16) + 0x8000) >> 10;
	return static_cast<uint16_t>((unquantized * 31) >> 6);
}

float HalfToFloat(uint16_t half) {
	const uint32_t exponent = (half >> 10) & 0x1f;
	const uint32_t mantissa = half & 0x3ff;
	const float    value    = exponent == 0 ? std::ldexp(static_cast<float>(mantissa), -24)
	                                        : std::ldexp(1.0f + static_cast<float>(mantissa) / 1024.0f, static_cast<int>(exponent) - 15);
	return (half & 0x8000) != 0 ? -value : value;
}

// The channels of a (layer, mip): 10-bit endpoints for BC6H and RGBA16F (stored as the halves BC6H decodes them to), 8-bit values otherwise.
std::array<uint32_t, 4> SolidColour(ArrayKind kind, uint32_t layer, uint32_t mip) {
	if (kind == ArrayKind::Bc6h || kind == ArrayKind::Rgba16f) {
		return {32 + layer * 16, 32 + mip * 64, 500, 0};
	}
	return {(layer * 4) & 0xfe, (mip * 20) & 0xfe, 128, 254};
}

// One element (a 16-byte block or a texel) of that colour.
std::vector<uint8_t> SolidElement(ArrayKind kind, const std::array<uint32_t, 4>& colour) {
	std::array<uint8_t, 16> block {};
	uint32_t                position = 0;
	switch (kind) {
		case ArrayKind::Bc6h:
			// Mode 11: 10-bit endpoints, no deltas; equal endpoints.
			PutBits(block, position, 0x03, 5);
			for (const uint32_t value: {colour[0], colour[1], colour[2], colour[0], colour[1], colour[2]}) {
				PutBits(block, position, value, 10);
			}
			return {block.begin(), block.end()};
		case ArrayKind::Bc7:
			// Mode 6: 7-bit endpoints and p-bits 0; equal endpoints.
			PutBits(block, position, 1u << 6, 7);
			for (const uint32_t value: {colour[0], colour[0], colour[1], colour[1], colour[2], colour[2], colour[3], colour[3]}) {
				PutBits(block, position, value >> 1, 7);
			}
			return {block.begin(), block.end()};
		case ArrayKind::Rgba8:
			return {static_cast<uint8_t>(colour[0]), static_cast<uint8_t>(colour[1]), static_cast<uint8_t>(colour[2]), static_cast<uint8_t>(colour[3])};
		default: {
			std::vector<uint8_t> texel(8);
			const uint16_t       halves[4] = {Bc6hHalf(colour[0]), Bc6hHalf(colour[1]), Bc6hHalf(colour[2]), 0x3c00};
			std::memcpy(texel.data(), halves, 8);
			return texel;
		}
	}
}

std::array<float, 4> ExpectedColour(ArrayKind kind, const std::array<uint32_t, 4>& colour) {
	if (kind == ArrayKind::Bc6h || kind == ArrayKind::Rgba16f) {
		return {HalfToFloat(Bc6hHalf(colour[0])), HalfToFloat(Bc6hHalf(colour[1])), HalfToFloat(Bc6hHalf(colour[2])), 1.0f};
	}
	return {static_cast<float>(colour[0]) / 255.0f, static_cast<float>(colour[1]) / 255.0f, static_cast<float>(colour[2]) / 255.0f,
	        static_cast<float>(colour[3]) / 255.0f};
}

// Fills rows of a subresource (row_bytes each, row_pitch apart) with copies of one element.
void FillRows(uint8_t* destination, uint64_t row_pitch, uint32_t rows, uint64_t row_bytes, const std::vector<uint8_t>& element) {
	for (uint32_t row = 0; row < rows; row++) {
		uint8_t* line = destination + row * row_pitch;
		for (uint64_t offset = 0; offset + element.size() <= row_bytes; offset += element.size()) {
			std::memcpy(line + offset, element.data(), element.size());
		}
	}
}

D3D12_RESOURCE_DESC BufferDesc(UINT64 size, D3D12_RESOURCE_FLAGS flags) {
	D3D12_RESOURCE_DESC buffer {};
	buffer.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
	buffer.Width            = size;
	buffer.Height           = 1;
	buffer.DepthOrArraySize = 1;
	buffer.MipLevels        = 1;
	buffer.SampleDesc.Count = 1;
	buffer.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	buffer.Flags            = flags;
	return buffer;
}

D3D12_RESOURCE_DESC ArrayDesc(uint32_t width, uint32_t height, uint32_t mips, uint32_t layers, DXGI_FORMAT format) {
	D3D12_RESOURCE_DESC desc {};
	desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width            = width;
	desc.Height           = height;
	desc.DepthOrArraySize = static_cast<UINT16>(layers);
	desc.MipLevels        = static_cast<UINT16>(mips);
	desc.Format           = format;
	desc.SampleDesc.Count = 1;
	return desc;
}

// Uploads a buffer holding every subresource of desc (as GetCopyableFootprints lays them out) and records the copies into destination.
ComPtr<ID3D12Resource> RecordBufferUpload(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* destination, const D3D12_RESOURCE_DESC& desc,
                                          uint32_t first_subresource_of_destination, uint32_t subresources,
                                          const std::function<std::vector<uint8_t>(uint32_t)>& element_of) {
	std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresources);
	std::vector<UINT>                               rows(subresources);
	std::vector<UINT64>                             row_bytes(subresources);
	UINT64                                          size = 0;
	device->GetCopyableFootprints(&desc, 0, subresources, 0, footprints.data(), rows.data(), row_bytes.data(), &size);
	D3D12_HEAP_PROPERTIES  upload_heap {D3D12_HEAP_TYPE_UPLOAD};
	const auto             buffer_desc = BufferDesc(size, D3D12_RESOURCE_FLAG_NONE);
	ComPtr<ID3D12Resource> upload;
	Check(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)),
	      "texture array test: upload buffer");
	uint8_t* mapped = nullptr;
	Check(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "texture array test: map upload");
	for (uint32_t s = 0; s < subresources; s++) {
		FillRows(mapped + footprints[s].Offset, footprints[s].Footprint.RowPitch, rows[s], row_bytes[s], element_of(s));
	}
	upload->Unmap(0, nullptr);
	for (uint32_t s = 0; s < subresources; s++) {
		D3D12_TEXTURE_COPY_LOCATION target {destination, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
		target.SubresourceIndex = first_subresource_of_destination + s;
		D3D12_TEXTURE_COPY_LOCATION source {upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
		source.PlacedFootprint = footprints[s];
		list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
	}
	return upload;
}

bool RunTextureArrayCase(GraphicContext& graphics, const ArrayCase& test, bool quiet = false) {
	auto*          device       = graphics.device;
	const uint32_t subresources = test.mips * test.layers;
	const auto     view_format  = ViewFormat(test.kind);
	const auto     title        = fmt::format("texture array {} {}x{}, {} mips, {} layers, {}", KindName(test.kind), test.size, test.size, test.mips,
	                                          test.layers, UploadName(test));
	// Subresource s = mip + layer * mips, as D3D12 numbers them.
	const auto element_of = [&](uint32_t s) { return SolidElement(test.kind, SolidColour(test.kind, s / test.mips, s % test.mips)); };

	ComPtr<ID3D12RootSignature> root_signature;
	Check(device->CreateRootSignature(0, TEXTURE_ARRAY_TEST_DXIL, sizeof(TEXTURE_ARRAY_TEST_DXIL), IID_PPV_ARGS(&root_signature)),
	      "texture array test: CreateRootSignature");
	D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc {};
	pipeline_desc.pRootSignature     = root_signature.Get();
	pipeline_desc.CS.pShaderBytecode = TEXTURE_ARRAY_TEST_DXIL;
	pipeline_desc.CS.BytecodeLength  = sizeof(TEXTURE_ARRAY_TEST_DXIL);
	ComPtr<ID3D12PipelineState> pipeline;
	Check(device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline)), "texture array test: CreateComputePipelineState");

	ComPtr<ID3D12CommandAllocator>    command_allocator;
	ComPtr<ID3D12GraphicsCommandList> list;
	Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&command_allocator)), "texture array test: command allocator");
	Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, command_allocator.Get(), nullptr, IID_PPV_ARGS(&list)),
	      "texture array test: command list");
	const auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
		if (before == after) {
			return;
		}
		D3D12_RESOURCE_BARRIER barrier {};
		barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource   = resource;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = before;
		barrier.Transition.StateAfter  = after;
		list->ResourceBarrier(1, &barrier);
	};

	D3D12_HEAP_PROPERTIES               default_heap {D3D12_HEAP_TYPE_DEFAULT};
	ComPtr<ID3D12Resource>              texture;       // the one the shader reads
	D3D12MA::Allocation*                allocation = nullptr;
	std::vector<ComPtr<ID3D12Resource>> keep;          // upload buffers, alive until the GPU is done
	D3D12_RESOURCE_STATES               state = D3D12_RESOURCE_STATE_COPY_DEST;
	const auto                          typed = ArrayDesc(test.size, test.size, test.mips, test.layers, view_format);

	switch (test.upload) {
		case Upload::CopyFromBuffer: {
			if (test.like_renderer) {
				const auto               desc = ArrayDesc(test.size, test.size, test.mips, test.layers, FamilyFormat(test.kind));
				D3D12MA::ALLOCATION_DESC allocation_desc {};
				allocation_desc.HeapType = D3D12_HEAP_TYPE_DEFAULT;
				Check(graphics.allocator->CreateResource(&allocation_desc, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, &allocation, IID_NULL, nullptr),
				      "texture array test: allocator CreateResource");
				texture = allocation->GetResource();
				transition(texture.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
			} else {
				Check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &typed, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
				                                      IID_PPV_ARGS(&texture)),
				      "texture array test: CreateCommittedResource");
			}
			keep.push_back(RecordBufferUpload(device, list.Get(), texture.Get(), typed, 0, subresources, element_of));
			break;
		}
		case Upload::CpuWrite: {
			D3D12_HEAP_PROPERTIES  cpu_heap {D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK, D3D12_MEMORY_POOL_L0};
			ComPtr<ID3D12Resource> cpu_texture;
			const HRESULT          made = device->CreateCommittedResource(&cpu_heap, D3D12_HEAP_FLAG_NONE, &typed, D3D12_RESOURCE_STATE_COMMON, nullptr,
			                                                              IID_PPV_ARGS(&cpu_texture));
			if (FAILED(made)) {
				Say(fmt::format("{}: not supported (a CPU-visible texture: 0x{:08x})", title, static_cast<uint32_t>(made)));
				return true;
			}
			std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(subresources);
			std::vector<UINT>                               rows(subresources);
			std::vector<UINT64>                             row_bytes(subresources);
			device->GetCopyableFootprints(&typed, 0, subresources, 0, footprints.data(), rows.data(), row_bytes.data(), nullptr);
			std::vector<uint8_t> data;
			for (uint32_t s = 0; s < subresources; s++) {
				data.assign(static_cast<size_t>(row_bytes[s]) * rows[s], 0);
				FillRows(data.data(), row_bytes[s], rows[s], row_bytes[s], element_of(s));
				Check(cpu_texture->Map(s, nullptr, nullptr), "texture array test: map CPU-visible texture");
				Check(cpu_texture->WriteToSubresource(s, nullptr, data.data(), static_cast<UINT>(row_bytes[s]), static_cast<UINT>(data.size())),
				      "texture array test: WriteToSubresource");
				cpu_texture->Unmap(s, nullptr);
			}
			texture = cpu_texture;
			state   = D3D12_RESOURCE_STATE_COMMON;
			break;
		}
	}
	transition(texture.Get(), state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	const UINT64           results_size = static_cast<UINT64>(subresources) * 2 * 16;
	ComPtr<ID3D12Resource> results;
	ComPtr<ID3D12Resource> readback;
	D3D12_HEAP_PROPERTIES  readback_heap {D3D12_HEAP_TYPE_READBACK};
	const auto             results_desc  = BufferDesc(results_size, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	const auto             readback_desc = BufferDesc(results_size, D3D12_RESOURCE_FLAG_NONE);
	Check(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &results_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
	                                      IID_PPV_ARGS(&results)),
	      "texture array test: results buffer");
	Check(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
	                                      IID_PPV_ARGS(&readback)),
	      "texture array test: readback buffer");

	ComPtr<ID3D12DescriptorHeap> heap;
	D3D12_DESCRIPTOR_HEAP_DESC   heap_desc {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
	Check(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)), "texture array test: descriptor heap");
	D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
	srv.Format                         = view_format;
	srv.ViewDimension                  = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
	srv.Shader4ComponentMapping        = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srv.Texture2DArray.MostDetailedMip = 0;
	srv.Texture2DArray.MipLevels       = test.mips;
	srv.Texture2DArray.FirstArraySlice = 0;
	srv.Texture2DArray.ArraySize       = test.layers;
	device->CreateShaderResourceView(texture.Get(), &srv, heap->GetCPUDescriptorHandleForHeapStart());

	list->SetComputeRootSignature(root_signature.Get());
	list->SetPipelineState(pipeline.Get());
	ID3D12DescriptorHeap* heaps[] = {heap.Get()};
	list->SetDescriptorHeaps(1, heaps);
	const uint32_t parameters[4] = {test.layers, test.mips, test.size, test.size};
	list->SetComputeRoot32BitConstants(0, 4, parameters, 0);
	list->SetComputeRootDescriptorTable(1, heap->GetGPUDescriptorHandleForHeapStart());
	list->SetComputeRootUnorderedAccessView(2, results->GetGPUVirtualAddress());
	list->Dispatch((subresources + 63) / 64, 1, 1);
	transition(results.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
	list->CopyBufferRegion(readback.Get(), 0, results.Get(), 0, results_size);
	Check(list->Close(), "texture array test: close");

	ComPtr<ID3D12Fence> fence;
	Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "texture array test: fence");
	{
		Common::LockGuard  lock(graphics.queue_mutex);
		ID3D12CommandList* lists[] = {list.Get()};
		graphics.queue->ExecuteCommandLists(1, lists);
		Check(graphics.queue->Signal(fence.Get(), 1), "texture array test: signal");
	}
	HANDLE event = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
	Check(fence->SetEventOnCompletion(1, event), "texture array test: wait");
	WaitForSingleObject(event, 10000);
	CloseHandle(event);
	if (FAILED(device->GetDeviceRemovedReason())) {
		Say(fmt::format("{}: the device was removed (0x{:08x}); the remaining cases are skipped", title,
		                static_cast<uint32_t>(device->GetDeviceRemovedReason())));
		return false;
	}

	float* values = nullptr;
	Check(readback->Map(0, nullptr, reinterpret_cast<void**>(&values)), "texture array test: map readback");
	uint32_t                 wrong_load   = 0;
	uint32_t                 wrong_sample = 0;
	std::string              first_wrong;
	std::vector<std::string> marks_by_mip(test.mips);
	for (uint32_t layer = 0; layer < test.layers; layer++) {
		for (uint32_t mip = 0; mip < test.mips; mip++) {
			const uint32_t index    = layer * test.mips + mip;
			const auto     expected = ExpectedColour(test.kind, SolidColour(test.kind, layer, mip));
			char           mark     = '.';
			for (uint32_t kind = 0; kind < 2; kind++) {
				const float* got   = values + (static_cast<size_t>(index) * 2 + kind) * 4;
				bool         right = true;
				bool         zero  = true;
				for (int c = 0; c < 4; c++) {
					right = right && std::fabs(got[c] - expected[c]) <= std::max(0.002f, std::fabs(expected[c]) * 0.002f);
					zero  = zero && got[c] == 0.0f;
				}
				if (!right) {
					(kind == 0 ? wrong_load : wrong_sample)++;
					mark = zero ? 'z' : 'x';
					if (first_wrong.size() < 600) {
						first_wrong += fmt::format("\n    layer {} mip {} {}: got ({:.4g}, {:.4g}, {:.4g}, {:.4g}) expected ({:.4g}, {:.4g}, {:.4g}, {:.4g})", layer, mip,
						                           kind == 0 ? "Load" : "SampleLevel", got[0], got[1], got[2], got[3], expected[0], expected[1],
						                           expected[2], expected[3]);
					}
				}
			}
			marks_by_mip[mip] += mark;
		}
	}
	readback->Unmap(0, nullptr);
	std::string map;
	if (wrong_load != 0 || wrong_sample != 0) {
		map = " layers per mip (. right, z zero, x other):";
		for (uint32_t mip = 0; mip < test.mips; mip++) {
			map += fmt::format("\n    mip {:2} ({:4}x{:<4}) {}", mip, std::max(test.size >> mip, 1u), std::max(test.size >> mip, 1u), marks_by_mip[mip]);
		}
	}
	if (allocation != nullptr) {
		texture.Reset();
		allocation->Release();
	}
	if (!quiet) {
		Say(fmt::format("{}: {} of {} wrong with Load, {} with SampleLevel{}{}", title, wrong_load, subresources, wrong_sample, map, first_wrong));
	}
	return wrong_load == 0 && wrong_sample == 0;
}

struct Kernel {
	const char*               name;
	std::span<const uint32_t> spirv;
};

} // namespace

bool BlockArrayCopiesWork(GraphicContext& graphics) {
	return RunTextureArrayCase(graphics, {ArrayKind::Bc7, 16, 3, 32, Upload::CopyFromBuffer, false}, true);
}

bool RunShaderSelfTest(bool debug_layer) {
	using Clock = std::chrono::steady_clock;
	const auto milliseconds = [](Clock::time_point from) { return std::chrono::duration<double, std::milli>(Clock::now() - from).count(); };

	GraphicContext graphics;
	graphics.Create(debug_layer);
	Say(fmt::format("device {}", graphics.DeviceName()));

	bool ok = true;
	{
		const auto start = Clock::now();
		DxilCompiler compiler(graphics.device);
		Say(fmt::format("DXIL validator loaded in {:.1f} ms", milliseconds(start)));

		const auto root_signature = CreateRootSignature(graphics.device);
		const Kernel kernels[] = {
		    {"color_transform", GPU_TILER_COLOR_TRANSFORM_SPV}, {"standard4", GPU_TILER_STANDARD4_SPV}, {"standard64", GPU_TILER_STANDARD64_SPV},
		    {"standard256", GPU_TILER_STANDARD256_SPV}, {"depth", GPU_TILER_DEPTH_SPV},         {"render_target", GPU_TILER_RENDER_TARGET_SPV},
		    {"prt", GPU_TILER_PRT_SPV},
		};
		for (const auto& kernel: kernels) {
			const auto begin  = Clock::now();
			const auto dxil   = compiler.Compile(kernel.spirv, ShaderType::Compute, 0, false);
			const auto signed_ms = milliseconds(begin);

			D3D12_COMPUTE_PIPELINE_STATE_DESC desc {};
			desc.pRootSignature     = root_signature.Get();
			desc.CS.pShaderBytecode = dxil.bytecode.data();
			desc.CS.BytecodeLength  = dxil.bytecode.size();
			ComPtr<ID3D12PipelineState> pipeline;
			const auto                  made   = Clock::now();
			const HRESULT               result = graphics.device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline));
			if (FAILED(result)) {
				ok = false;
				Say(fmt::format("{}: translated and signed ({} bytes of SPIR-V, {} bytes of DXIL, {:.1f} ms) but CreateComputePipelineState failed: 0x{:08x}",
				                kernel.name, kernel.spirv.size_bytes(), dxil.bytecode.size(), signed_ms, static_cast<uint32_t>(result)));
				continue;
			}
			Say(fmt::format("{}: {} bytes of SPIR-V, {} bytes of DXIL, translated and signed in {:.1f} ms, pipeline in {:.1f} ms", kernel.name,
			                kernel.spirv.size_bytes(), dxil.bytecode.size(), signed_ms, milliseconds(made)));
		}
	}
	const ArrayCase array_cases[] = {
	    {ArrayKind::Bc6h, 512, 10, 48, Upload::CopyFromBuffer, true},
	    {ArrayKind::Bc6h, 512, 10, 48, Upload::CpuWrite, false},
	    {ArrayKind::Bc7, 16, 3, 32, Upload::CopyFromBuffer, true},
	    {ArrayKind::Bc7, 16, 3, 32, Upload::CpuWrite, false},
	    {ArrayKind::Rgba8, 512, 10, 48, Upload::CopyFromBuffer, true},
	    {ArrayKind::Rgba16f, 256, 9, 48, Upload::CopyFromBuffer, true},
	};
	for (const auto& test: array_cases) {
		ok = RunTextureArrayCase(graphics, test) && ok;
		if (FAILED(graphics.device->GetDeviceRemovedReason())) {
			break;
		}
	}
	graphics.Destroy();
	Say(ok ? "passed" : "FAILED");
	return ok;
}

} // namespace Libs::Graphics::D3D12
