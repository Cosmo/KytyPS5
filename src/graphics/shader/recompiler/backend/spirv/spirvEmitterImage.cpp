#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/shader/recompiler/HostShaderFeatures.h"
#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"
#include "graphics/shader/recompiler/frontend/decode/ImageOps.h"

#include <algorithm>
#include <bit>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

uint32_t DmaskComponentIndex(uint32_t dmask, uint32_t component) {
	uint32_t index = 0;
	for (uint32_t i = 0; i < component; i++) {
		index += (dmask >> i) & 1u;
	}
	return index;
}

uint32_t DmaskComponent(uint32_t dmask, uint32_t index) {
	for (uint32_t component = 0; component < 4u; component++) {
		if (((dmask >> component) & 1u) != 0u && index-- == 0u) return component;
	}
	return 0u;
}

uint32_t ImageGatherComponent(uint32_t dmask) {
	switch (dmask) {
		case 0x2u: return 1;
		case 0x4u: return 2;
		case 0x8u: return 3;
		default: return 0;
	}
}

bool HasFlag(const IR::MemoryInfo& mem, uint32_t flag) {
	return (mem.image_sample_flags & flag) != 0u;
}

uint32_t AddressU32(ValueEmitContext& ctx, const IR::MemoryInfo& mem, const IR::Inst& address,
                    uint32_t component) {
	const auto layout = Decoder::ImageAddressComponentLayout(mem.image_sample_flags, component);
	const auto packed = layout.bit_offset / 32u;
	if (packed >= address.NumArgs()) return ConstantU32(ctx.state, 0);
	auto value = ctx.Def(address.Arg(packed));
	if (layout.bit_width == 16u) {
		if ((layout.bit_offset & 31u) != 0u) {
			value = Binary(ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state), value,
			               ConstantU32(ctx.state, 16));
		}
		value = Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state), value,
		               ConstantU32(ctx.state, 0xffffu));
	}
	return value;
}

uint32_t AddressF32(ValueEmitContext& ctx, const IR::MemoryInfo& mem, const IR::Inst& address,
                    uint32_t component) {
	const auto value = AddressU32(ctx, mem, address, component);
	return Decoder::ImageAddressComponentLayout(mem.image_sample_flags, component).bit_width == 16u
	           ? EmitF16BitsToF32(ctx.state, value)
	           : Unary(ctx.state, spv::OpBitcast, TypeF32(ctx.state), value);
}

ImageSampleLayout Layout(const IR::MemoryInfo& mem) {
	ImageSampleLayout layout;
	uint32_t          cursor = 0;
	const auto&       info   = ImageDimensionInfoFor(mem.image_dimension);
	if (HasFlag(mem, Decoder::ImageSampleFlagOffset)) layout.offset = cursor++;
	if (HasFlag(mem, Decoder::ImageSampleFlagBias)) layout.bias = cursor++;
	if (HasFlag(mem, Decoder::ImageSampleFlagCompare)) layout.dref = cursor++;
	if (HasFlag(mem, Decoder::ImageSampleFlagDerivative)) {
		layout.grad_x = cursor;
		cursor += info.spatial_components;
		layout.grad_y = cursor;
		cursor += info.spatial_components;
	}
	layout.coord = cursor;
	cursor += info.coordinate_components;
	if (HasFlag(mem, Decoder::ImageSampleFlagLod)) layout.lod = cursor++;
	return layout;
}

uint32_t ZeroF32(EmitterState& state) {
	return ConstantF32(state, 0);
}

uint32_t GatherMip(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                   const IR::Inst& address, const ImageSampleLayout& layout, uint32_t mip_count) {
	auto& state = ctx.state;
	const auto Extract = [&](uint32_t word, uint32_t first, uint32_t count) {
		return EmitBitFieldUExtract(state, word, ConstantU32(state, first),
		                           ConstantU32(state, count));
	};
	const auto LodFixed = [&](uint32_t word, uint32_t first) {
		return Binary(state, spv::OpFMul, TypeF32(state),
		              Unary(state, spv::OpConvertUToF, TypeF32(state), Extract(word, first, 12)),
		              ConstantF32Value(state, 1.0f / 256.0f));
	};
	const auto sampler_control = ctx.Arg(inst, 6);
	const auto mip_filter = Extract(sampler_control, 26, 2);
	const auto preclamp = Binary(state, spv::OpINotEqual, TypeBool(state),
	                            Extract(sampler_control, 28, 1), ConstantU32(state, 0));
	const auto zero = ZeroF32(state);
	// Point selects the nearest mip; linear gathers return the lower, primary mip only.
	const auto rounding = Select(state, TypeF32(state),
	    Binary(state, spv::OpIEqual, TypeBool(state), mip_filter, ConstantU32(state, 1)),
	    ConstantF32Value(state, 0.5f), zero);
	const auto base = Unary(state, spv::OpConvertUToF, TypeF32(state),
	                        Extract(ctx.Arg(inst, 4), 12, 4));
	const auto minimum = Binary(state, spv::OpFSub, TypeF32(state),
	                            LodFixed(ctx.Arg(inst, 3), 8), base);
	const auto last = ConstantF32Value(state, static_cast<float>(mip_count - 1u));
	auto lod = Binary(state, spv::OpFAdd, TypeF32(state),
	                  AddressF32(ctx, mem, address, layout.lod),
	                  Select(state, TypeF32(state), preclamp, rounding, zero));
	lod = EmitGlsl<GLSLstd450FClamp, IR::Type::F32>(
	    state, lod, LodFixed(ctx.Arg(inst, 5), 0), LodFixed(ctx.Arg(inst, 5), 12));
	// T# minimum LOD is in physical mip space and applies after the S# view-relative clamp.
	lod = EmitGlsl<GLSLstd450FMax, IR::Type::F32>(state, lod, minimum);
	lod = EmitGlsl<GLSLstd450FClamp, IR::Type::F32>(state, lod, zero, last);
	lod = Binary(state, spv::OpFAdd, TypeF32(state), lod,
	             Select(state, TypeF32(state), preclamp, zero, rounding));
	// The clamped value is nonnegative, so conversion performs the integer mip rounding.
	const auto mip = EmitUMin32(state, Unary(state, spv::OpConvertFToU, TypeU32(state), lod),
	                           ConstantU32(state, mip_count - 1u));
	const auto mip_none = Binary(state, spv::OpIEqual, TypeBool(state),
	                            mip_filter, ConstantU32(state, 0));
	return Select(state, TypeU32(state), mip_none, ConstantU32(state, 0), mip);
}

uint32_t CubeAxis(EmitterState& state, uint32_t value) {
	return Binary(state, spv::OpFSub, TypeF32(state), value, ConstantF32(state, 0x3f800000u));
}

uint32_t CubeLayer(EmitterState& state, uint32_t value) {
	const auto guest = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpConvertFToU, TypeU32(state), guest, value);
	const auto padding = Binary(
	    state, spv::OpShiftLeftLogical, TypeU32(state),
	    Binary(state, spv::OpShiftRightLogical, TypeU32(state), guest, ConstantU32(state, 3)),
	    ConstantU32(state, 1));
	const auto host   = Binary(state, spv::OpISub, TypeU32(state), guest, padding);
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpConvertUToF, TypeF32(state), result, host);
	return result;
}

uint32_t CoordF32(ValueEmitContext& ctx, const IR::MemoryInfo& mem, const IR::Inst& address,
                  uint32_t first, uint32_t components, bool cube = false) {
	auto x = AddressF32(ctx, mem, address, first);
	if (components == 1u) return x;
	auto y = mem.image_address_components > first + 1u ? AddressF32(ctx, mem, address, first + 1u)
	                                                   : ZeroF32(ctx.state);
	if (cube) {
		x = CubeAxis(ctx.state, x);
		y = CubeAxis(ctx.state, y);
	}
	const auto result = ctx.state.builder.AllocateId();
	if (components == 3u) {
		auto z = mem.image_address_components > first + 2u
		             ? AddressF32(ctx, mem, address, first + 2u)
		             : ZeroF32(ctx.state);
		if (cube) z = CubeLayer(ctx.state, z);
		ctx.state.builder.AddFunction(spv::OpCompositeConstruct, TypeF32Vector(ctx.state, 3),
		                              result, x, y, z);
	} else {
		ctx.state.builder.AddFunction(spv::OpCompositeConstruct, TypeF32Vector(ctx.state, 2),
		                              result, x, y);
	}
	return result;
}

uint32_t CoordU32(ValueEmitContext& ctx, const IR::MemoryInfo& mem, const IR::Inst& address,
                  ImageDimension dimension) {
	const auto components = ImageDimensionInfoFor(dimension).coordinate_components;
	const auto x          = AddressU32(ctx, mem, address, 0);
	if (components == 1u) return x;
	const auto y      = mem.image_address_components > 1u ? AddressU32(ctx, mem, address, 1)
	                                                      : ConstantU32(ctx.state, 0);
	const auto result = ctx.state.builder.AllocateId();
	if (components == 3u) {
		const auto z = mem.image_address_components > 2u ? AddressU32(ctx, mem, address, 2)
		                                                 : ConstantU32(ctx.state, 0);
		ctx.state.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(ctx.state, 3),
		                              result, x, y, z);
	} else {
		ctx.state.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(ctx.state, 2),
		                              result, x, y);
	}
	return result;
}

uint32_t LodU32(ValueEmitContext& ctx, const IR::MemoryInfo& mem, const IR::Inst& address,
                ImageDimension dimension) {
	const auto component = ImageDimensionInfoFor(dimension).coordinate_components;
	return mem.image_has_mip && mem.image_address_components > component
	           ? AddressU32(ctx, mem, address, component)
	           : ConstantU32(ctx.state, 0);
}

uint32_t FloatBits(ValueEmitContext& ctx, uint32_t value) {
	return Unary(ctx.state, spv::OpBitcast, TypeU32(ctx.state), value);
}

uint32_t SampledComponentBits(ValueEmitContext& ctx, uint32_t value,
                              Prospero::TextureNumericClass numeric_class) {
	if (numeric_class == Prospero::TextureNumericClass::Uint) {
		return value;
	}
	return Unary(ctx.state, spv::OpBitcast, TypeU32(ctx.state), value);
}

uint32_t SampledComponentZero(EmitterState& state, Prospero::TextureNumericClass numeric_class) {
	switch (numeric_class) {
		case Prospero::TextureNumericClass::Float: return ZeroF32(state);
		case Prospero::TextureNumericClass::Uint: return ConstantU32(state, 0);
		case Prospero::TextureNumericClass::Sint: return ConstantI32(state, 0);
		case Prospero::TextureNumericClass::Unsupported: break;
	}
	EXIT("invalid sampled image numeric class");
}

uint32_t ResultVector(ValueEmitContext& ctx, uint32_t value,
                      Prospero::TextureNumericClass numeric_class, bool dref,
                      const IR::MemoryInfo& mem, bool gather = false) {
	auto value_class = numeric_class;
	const auto conversion = ctx.state.program.info.images[mem.resource].conversion_format;
	if (conversion != Prospero::BufferFormat::kInvalid) {
		value_class = Prospero::SampledTextureNumericClass(conversion);
	}
	if (dref) {
		value_class = Prospero::TextureNumericClass::Float;
	}
	const bool integer = value_class == Prospero::TextureNumericClass::Uint ||
	                     value_class == Prospero::TextureNumericClass::Sint;
	if (mem.data_bits == 16u) {
		uint32_t   packed[4] = {ConstantU32(ctx.state, 0), ConstantU32(ctx.state, 0),
		                        ConstantU32(ctx.state, 0), ConstantU32(ctx.state, 0)};
		const auto scalar    = [&](uint32_t index) {
			if (dref) return value;
			const auto component = gather ? index : DmaskComponent(mem.dmask, index);
			const auto result    = ctx.state.builder.AllocateId();
			ctx.state.builder.AddFunction(spv::OpCompositeExtract,
			                              ImageScalarType(ctx.state, value_class), result, value,
			                              component);
			return result;
		};
		for (uint32_t word = 0; word < mem.data_dwords; word++) {
			const auto low_index  = word * 2u;
			const auto high_index = low_index + 1u;
			const auto low        = scalar(low_index);
			const auto high       = high_index < mem.component_count
			                            ? scalar(high_index)
			                            : SampledComponentZero(ctx.state, value_class);
			if (integer) {
				const auto low_bits  = SampledComponentBits(ctx, low, value_class);
				const auto high_bits = SampledComponentBits(ctx, high, value_class);
				const auto mask      = ConstantU32(ctx.state, 0xffffu);
				packed[word] =
				    Binary(ctx.state, spv::OpBitwiseOr, TypeU32(ctx.state),
				           Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state), low_bits, mask),
				           Binary(ctx.state, spv::OpShiftLeftLogical, TypeU32(ctx.state),
				                  Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state),
				                         high_bits, mask),
				                  ConstantU32(ctx.state, 16u)));
			} else {
				const auto pair = ctx.state.builder.AllocateId();
				ctx.state.builder.AddFunction(spv::OpCompositeConstruct,
				                              TypeF32Vector(ctx.state, 2), pair, low, high);
				packed[word] = ctx.state.builder.AllocateId();
				ctx.state.builder.AddFunction(spv::OpExtInst, TypeU32(ctx.state), packed[word],
				                              GlslStd450(ctx.state), GLSLstd450PackHalf2x16, pair);
			}
		}
		const auto result = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(ctx.state, 4),
		                              result, packed[0], packed[1], packed[2], packed[3]);
		return result;
	}
	if (!dref)
		return value_class == Prospero::TextureNumericClass::Uint
		           ? value : Unary(ctx.state, spv::OpBitcast, TypeU32Vector(ctx.state, 4), value);
	const auto result = ctx.state.builder.AllocateId();
	const auto zero = ConstantU32(ctx.state, 0);
	ctx.state.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(ctx.state, 4), result,
	                              FloatBits(ctx, value), zero, zero, zero);
	return result;
}

uint32_t QueryDimensions(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                         const IR::Inst& address, uint32_t resource, uint32_t array_index) {
	const auto  dimension = ctx.state.program.info.images.at(resource).dimension;
	const auto& info      = ImageDimensionInfoFor(dimension);
	const auto  image     = LoadImageDescriptor(ctx.state, resource, 0u, array_index);
	const auto  size      = ctx.state.builder.AllocateId();
	if (info.multisampled != 0u) {
		ctx.state.builder.AddFunction(spv::OpImageQuerySize,
		                              ImageViewSizeType(ctx.state, dimension), size, image);
	} else {
		ctx.state.builder.AddFunction(spv::OpImageQuerySizeLod,
		                              ImageViewSizeType(ctx.state, dimension), size, image,
		                              AddressU32(ctx, mem, address, 0));
	}
	const auto components = info.coordinate_components;
	uint32_t   result[4]  = {ConstantU32(ctx.state, 0), ConstantU32(ctx.state, 0),
	                         ConstantU32(ctx.state, 0), ConstantU32(ctx.state, 0)};
	if (components == 1u) {
		result[0] = size;
	} else {
		for (uint32_t index = 0; index < components; index++) {
			result[index] = ctx.state.builder.AllocateId();
			ctx.state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(ctx.state),
			                              result[index], size, index);
		}
	}
	if (info.multisampled == 0u) {
		result[3] = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(spv::OpImageQueryLevels, TypeU32(ctx.state), result[3],
		                              image);
	}
	const auto vector = ctx.state.builder.AllocateId();
	ctx.state.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(ctx.state, 4), vector,
	                              result[0], result[1], result[2], result[3]);
	return vector;
}

uint32_t PackedOffset(ValueEmitContext& ctx, IR::Value value, ImageDimension dimension,
                      uint32_t& mask) {
	const auto components = ImageDimensionInfoFor(dimension).spatial_components;
	uint32_t values[3] {};
	if (value.IsImmediate()) {
		// RDNA2 packs signed six-bit texel offsets into the low bits of each byte.
		for (uint32_t index = 0; index < components; index++) {
			const auto bits = (value.U32() >> (index * 8u)) & 0x3fu;
			values[index] = ConstantI32(ctx.state, static_cast<int32_t>(bits ^ 0x20u) - 0x20);
		}
		mask |= spv::ImageOperandsConstOffsetMask;
		return components == 1u
		           ? values[0]
		           : ctx.state.builder.Constant(spv::OpConstantComposite,
		                                        TypeI32Vector(ctx.state, components),
		                                        std::span(values).first(components));
	}
	ctx.state.builder.RequireCapability(spv::CapabilityImageGatherExtended);
	mask |= spv::ImageOperandsOffsetMask;
	const auto packed = Unary(ctx.state, spv::OpBitcast, TypeI32(ctx.state), ctx.Def(value));
	for (uint32_t index = 0; index < components; index++) {
		values[index] = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(spv::OpBitFieldSExtract, TypeI32(ctx.state), values[index],
		                              packed, ConstantU32(ctx.state, index * 8u),
		                              ConstantU32(ctx.state, 6));
	}
	if (components == 1u) return values[0];
	const auto result = ctx.state.builder.AllocateId();
	ctx.state.builder.AddFunction(spv::OpCompositeConstruct, TypeI32Vector(ctx.state, components),
	                              result, std::span(values).first(components));
	return result;
}

// Without maintenance8 a sample's texel offset must be a constant (HostShaderFeatures::sample_offset_by_coordinates). An offset the
// shader computes moves the coordinates instead, by offset / the size of the mip level the sample reads: exact for that level (the
// second level of a trilinear sample is off by less than half a texel). The level comes from the sample's LOD, or from its gradients.
// A sample whose level the GPU picks takes the coordinates' own gradients explicitly, so that the moved coordinates keep its level.
// HostShaderFeatures::unnormalized_samplers_in_shader: texel positions (the sampler's FORCE_UNNORMALIZED) become normalized
// coordinates, divided by the size of the image's first level; an array layer stays as it is.
void NormalizeTexelCoordinates(ValueEmitContext& ctx, const IR::Inst& inst, const IR::ImageResource& candidate,
                               uint32_t resource, uint32_t array_index, uint32_t& coord) {
	auto& state = ctx.state;
	if (candidate.cube || candidate.dimension == ImageDimension::Dim3D) {
		ctx.Fail(inst, "samples a cube or 3D image with texel positions");
		return;
	}
	const auto& info    = ImageDimensionInfoFor(candidate.dimension);
	const auto  spatial = info.spatial_components;
	const auto  type_f  = spatial == 1u ? TypeF32(state) : TypeF32Vector(state, spatial);
	const auto  type_u  = spatial == 1u ? TypeU32(state) : TypeU32Vector(state, spatial);
	const auto  leading = [&](uint32_t value, uint32_t type) {
		if (info.coordinate_components == spatial) {
			return value;
		}
		const auto result = state.builder.AllocateId();
		if (spatial == 1u) {
			state.builder.AddFunction(spv::OpCompositeExtract, type, result, value, 0u);
		} else {
			state.builder.AddFunction(spv::OpVectorShuffle, type, result, value, value, 0u, 1u);
		}
		return result;
	};
	state.builder.RequireCapability(spv::CapabilityImageQuery);
	const auto image = LoadImageDescriptor(state, resource, 0u, array_index);
	const auto size  = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpImageQuerySizeLod, ImageViewSizeType(state, candidate.dimension), size, image,
	                          ConstantU32(state, 0));
	const auto normalized = Binary(state, spv::OpFDiv, type_f, leading(coord, type_f),
	                               Unary(state, spv::OpConvertUToF, type_f, leading(size, type_u)));
	if (info.coordinate_components == spatial) {
		coord = normalized;
		return;
	}
	const auto result = state.builder.AllocateId();
	if (spatial == 1u) {
		const auto layer = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpCompositeExtract, TypeF32(state), layer, coord, 1u);
		state.builder.AddFunction(spv::OpCompositeConstruct, TypeF32Vector(state, 2), result, normalized, layer);
	} else {
		state.builder.AddFunction(spv::OpVectorShuffle, TypeF32Vector(state, 3), result, normalized, coord, 0u, 1u, 4u);
	}
	coord = result;
}

void MoveCoordinatesByOffset(ValueEmitContext& ctx, const IR::Inst& inst, const IR::ImageResource& candidate,
                             uint32_t resource, uint32_t array_index, IR::Value offset_value, uint32_t& coord,
                             spv::Op& opcode, std::array<uint32_t, 4>& operands, size_t& operand_count) {
	auto& state = ctx.state;
	if (candidate.cube) {
		ctx.Fail(inst, "has a dynamic sample texel offset on a cube image");
	}
	const auto& info    = ImageDimensionInfoFor(candidate.dimension);
	const auto  spatial = info.spatial_components;
	const auto  type_f  = spatial == 1u ? TypeF32(state) : TypeF32Vector(state, spatial);
	const auto  type_u  = spatial == 1u ? TypeU32(state) : TypeU32Vector(state, spatial);
	const auto  glsl    = [&](uint32_t type, GLSLstd450 op, auto... args) {
		const auto result = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpExtInst, type, result, GlslStd450(state), op, args...);
		return result;
	};
	// The first `spatial` components (without the array layer).
	const auto leading = [&](uint32_t value, uint32_t components, uint32_t type) {
		if (components == spatial) {
			return value;
		}
		const auto result = state.builder.AllocateId();
		if (spatial == 1u) {
			state.builder.AddFunction(spv::OpCompositeExtract, type, result, value, 0u);
		} else {
			state.builder.AddFunction(spv::OpVectorShuffle, type, result, value, value, 0u, 1u);
		}
		return result;
	};
	state.builder.RequireCapability(spv::CapabilityImageQuery);
	const auto image   = LoadImageDescriptor(state, resource, 0u, array_index);
	const auto size_at = [&](uint32_t level) {
		const auto size = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpImageQuerySizeLod, ImageViewSizeType(state, candidate.dimension), size,
		                          image, level);
		return Unary(state, spv::OpConvertUToF, type_f, leading(size, info.coordinate_components, type_u));
	};
	const auto position = leading(coord, info.coordinate_components, type_f);

	const auto mask    = operand_count != 0u ? operands[0] : 0u;
	uint32_t   level_f = 0;
	uint32_t   grad_x  = 0;
	uint32_t   grad_y  = 0;
	if (opcode == spv::OpImageSampleImplicitLod || opcode == spv::OpImageSampleDrefImplicitLod) {
		grad_x = Unary(state, spv::OpDPdx, type_f, position);
		grad_y = Unary(state, spv::OpDPdy, type_f, position);
		if (mask == spv::ImageOperandsBiasMask) {
			// A bias of b scales the gradients by 2^b.
			const auto scale  = glsl(TypeF32(state), GLSLstd450Exp2, operands[1]);
			const auto scaled = spatial == 1u ? spv::OpFMul : spv::OpVectorTimesScalar;
			grad_x            = Binary(state, scaled, type_f, grad_x, scale);
			grad_y            = Binary(state, scaled, type_f, grad_y, scale);
		}
		opcode        = opcode == spv::OpImageSampleImplicitLod ? spv::OpImageSampleExplicitLod
		                                                         : spv::OpImageSampleDrefExplicitLod;
		operands      = {static_cast<uint32_t>(spv::ImageOperandsGradMask), grad_x, grad_y, 0u};
		operand_count = 3;
	} else if (mask == spv::ImageOperandsGradMask) {
		grad_x = operands[1];
		grad_y = operands[2];
	} else if (mask == spv::ImageOperandsLodMask) {
		level_f = operands[1];
	} else {
		level_f = ConstantF32Value(state, 0.0f);
	}
	if (grad_x != 0u) {
		// The level the gradients select: log2 of the longer footprint, in texels of the first level.
		const auto size0   = size_at(ConstantU32(state, 0));
		const auto dx      = Binary(state, spv::OpFMul, type_f, grad_x, size0);
		const auto dy      = Binary(state, spv::OpFMul, type_f, grad_y, size0);
		const auto length2 = [&](uint32_t v) {
			return Binary(state, spatial == 1u ? spv::OpFMul : spv::OpDot, TypeF32(state), v, v);
		};
		const auto longest = glsl(TypeF32(state), GLSLstd450FMax, length2(dx), length2(dy));
		const auto nonzero = glsl(TypeF32(state), GLSLstd450FMax, longest, ConstantF32Value(state, 1e-30f));
		level_f            = Binary(state, spv::OpFMul, TypeF32(state), glsl(TypeF32(state), GLSLstd450Log2, nonzero),
		                            ConstantF32Value(state, 0.5f));
	}
	// The level read: a whole one within the image's levels.
	const auto levels = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpImageQueryLevels, TypeI32(state), levels, image);
	const auto last  = Binary(state, spv::OpISub, TypeI32(state), levels, ConstantI32(state, 1));
	const auto whole = glsl(TypeF32(state), GLSLstd450FClamp, glsl(TypeF32(state), GLSLstd450Floor, level_f),
	                        ConstantF32Value(state, 0.0f), ConstantF32Value(state, 15.0f));
	const auto level = glsl(TypeI32(state), GLSLstd450SMax,
	                        glsl(TypeI32(state), GLSLstd450SMin, Unary(state, spv::OpConvertFToS, TypeI32(state), whole),
	                             last),
	                        ConstantI32(state, 0));

	uint32_t   unused_mask = 0;
	const auto offset =
	    Unary(state, spv::OpConvertSToF, type_f, PackedOffset(ctx, offset_value, candidate.dimension, unused_mask));
	const auto moved =
	    Binary(state, spv::OpFAdd, type_f, position, Binary(state, spv::OpFDiv, type_f, offset, size_at(level)));
	if (spatial == info.coordinate_components) {
		coord = moved;
		return;
	}
	const auto layer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpCompositeExtract, TypeF32(state), layer, coord, spatial);
	coord = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpCompositeConstruct, TypeF32Vector(state, info.coordinate_components), coord,
	                          moved, layer);
}

// HostShaderFeatures::image_atomic64_lock: a 64-bit image atomic as a critical section under a lock word in the guest buffer
// table (IR::ImageAtomic64LockSlot). The lane that gets the lock reads the texel, writes the result and releases the lock in the
// same iteration, so lanes of one wave that want the same lock take turns instead of waiting for each other forever.
uint32_t EmitLockedImageAtomic64(ValueEmitContext& ctx, const IR::Inst& inst, const IR::MemoryInfo& mem,
                                 const IR::Inst& address, const IR::ImageResource& image) {
	auto& state = ctx.state;
	EXIT_IF(state.guest_buffer_table == 0);
	state.builder.RequireCapability(spv::CapabilityInt64);
	const auto& info  = ImageDimensionInfoFor(image.dimension);
	const auto  coord = CoordU32(ctx, mem, address, image.dimension);
	// The texel's lock: a hash of its coordinates and the image.
	constexpr uint32_t Multipliers[] {0x9e3779b1u, 0x85ebca77u, 0xc2b2ae3du};
	auto               hash = ConstantU32(state, (mem.resource + 1u) * 0x27d4eb2fu);
	for (uint32_t i = 0; i < info.coordinate_components; i++) {
		auto component = coord;
		if (info.coordinate_components > 1u) {
			component = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), component, coord, i);
		}
		hash = Binary(state, spv::OpBitwiseXor, TypeU32(state), hash,
		              Binary(state, spv::OpIMul, TypeU32(state), component, ConstantU32(state, Multipliers[i])));
	}
	const auto index = Binary(state, spv::OpBitwiseAnd, TypeU32(state), hash,
	                          ConstantU32(state, IR::ImageAtomic64LockCount - 1u));
	const auto lock  = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypeStorageBufferElementPointer(state, 32), lock,
	                          state.guest_buffer_table, ConstantU32(state, IR::ImageAtomic64LockSlot),
	                          ConstantU32(state, 0), index);
	const auto value = ctx.Arg(inst, inst.NumArgs() - 2);
	const auto apply = [&](uint32_t old) {
		const auto u64 = TypeU64(state);
		switch (inst.GetOpcode()) {
			case IR::ValueOpcode::ImageAtomicSwap64: return value;
			case IR::ValueOpcode::ImageAtomicIAdd64: return Binary(state, spv::OpIAdd, u64, old, value);
			case IR::ValueOpcode::ImageAtomicAnd64: return Binary(state, spv::OpBitwiseAnd, u64, old, value);
			case IR::ValueOpcode::ImageAtomicOr64: return Binary(state, spv::OpBitwiseOr, u64, old, value);
			case IR::ValueOpcode::ImageAtomicXor64: return Binary(state, spv::OpBitwiseXor, u64, old, value);
			case IR::ValueOpcode::ImageAtomicUMin64:
				return Select(state, u64, Binary(state, spv::OpULessThan, TypeBool(state), value, old), value, old);
			case IR::ValueOpcode::ImageAtomicUMax64:
				return Select(state, u64, Binary(state, spv::OpUGreaterThan, TypeBool(state), value, old), value, old);
			default: ctx.Fail(inst, "is a 64-bit image atomic without an emulation");
		}
	};

	const auto scope   = ConstantU32(state, spv::ScopeDevice);
	const auto none    = ConstantU32(state, spv::MemorySemanticsMaskNone);
	const auto acquire = ConstantU32(state, spv::MemorySemanticsAcquireMask | spv::MemorySemanticsImageMemoryMask |
	                                            spv::MemorySemanticsUniformMemoryMask);
	const auto release = ConstantU32(state, spv::MemorySemanticsReleaseMask | spv::MemorySemanticsImageMemoryMask |
	                                            spv::MemorySemanticsUniformMemoryMask);
	const auto entry   = state.current_label;
	const auto header  = state.builder.AllocateId();
	const auto body    = state.builder.AllocateId();
	const auto locked  = state.builder.AllocateId();
	const auto after   = state.builder.AllocateId();
	const auto cont    = state.builder.AllocateId();
	const auto merge   = state.builder.AllocateId();
	const auto done    = state.builder.AllocateId();
	const auto result  = state.builder.AllocateId();
	const auto done_next   = state.builder.AllocateId();
	const auto result_next = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpBranch, header);

	EmitLabel(state, header);
	state.builder.AddFunction(spv::OpPhi, TypeBool(state), done, ConstantBool(state, false),
	                          entry, done_next, cont);
	state.builder.AddFunction(spv::OpPhi, TypeU64(state), result, ConstantU64(state, 0), entry, result_next, cont);
	state.builder.AddFunction(spv::OpLoopMerge, merge, cont, spv::LoopControlMaskNone);
	state.builder.AddFunction(spv::OpBranchConditional, done, merge, body);

	EmitLabel(state, body);
	const auto previous = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAtomicCompareExchange, TypeU32(state), previous, lock, scope, acquire, none,
	                          ConstantU32(state, 1), ConstantU32(state, 0));
	const auto got = Binary(state, spv::OpIEqual, TypeBool(state), previous, ConstantU32(state, 0));
	state.builder.AddFunction(spv::OpSelectionMerge, after, spv::SelectionControlMaskNone);
	state.builder.AddFunction(spv::OpBranchConditional, got, locked, after);

	EmitLabel(state, locked);
	const auto texels = LoadImageDescriptor(state, mem.resource);
	const auto texel  = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpImageRead, TypeU32Vector(state, 4), texel, texels, coord);
	const auto low  = state.builder.AllocateId();
	const auto high = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), low, texel, 0u);
	state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), high, texel, 1u);
	const auto old     = PackU64(state, low, high);
	const auto updated = apply(old);
	const auto words   = Unary(state, spv::OpBitcast, TypeU32Vector(state, 2), updated);
	const auto word0   = state.builder.AllocateId();
	const auto word1   = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), word0, words, 0u);
	state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(state), word1, words, 1u);
	const auto written = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(state, 4), written, word0, word1,
	                          ConstantU32(state, 0), ConstantU32(state, 0));
	state.builder.AddFunction(spv::OpImageWrite, texels, coord, written);
	const auto released = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAtomicExchange, TypeU32(state), released, lock, scope, release, ConstantU32(state, 0));
	const auto locked_exit = state.current_label;
	state.builder.AddFunction(spv::OpBranch, after);

	EmitLabel(state, after);
	state.builder.AddFunction(spv::OpPhi, TypeBool(state), done_next, ConstantBool(state, true),
	                          locked_exit, ConstantBool(state, false), body);
	state.builder.AddFunction(spv::OpPhi, TypeU64(state), result_next, old, locked_exit, result, body);
	state.builder.AddFunction(spv::OpBranch, cont);

	EmitLabel(state, cont);
	state.builder.AddFunction(spv::OpBranch, header);

	EmitLabel(state, merge);
	return result;
}

uint32_t HorizontalOffsets(EmitterState& state, ImageDimension dimension) {
	const auto components   = ImageDimensionInfoFor(dimension).spatial_components;
	const auto count        = ConstantU32(state, 4);
	const auto element_type = components == 1u ? TypeI32(state) : TypeI32Vector(state, 2);
	const auto array_type   = state.builder.Type(spv::OpTypeArray, element_type, count);
	uint32_t   offsets[4] {};
	for (uint32_t index = 0; index < 4u; index++) {
		const auto x = ConstantI32(state, static_cast<int32_t>(index) - 1);
		if (components == 1u) {
			offsets[index] = x;
		} else {
			offsets[index] = state.builder.Constant(
			    spv::OpConstantComposite, TypeI32Vector(state, 2), x, ConstantI32(state, 0));
		}
	}
	return state.builder.Constant(spv::OpConstantComposite, array_type, offsets[0], offsets[1],
	                              offsets[2], offsets[3]);
}

uint32_t InverseSwizzle(uint32_t swizzle, uint32_t component) {
	for (uint32_t source = 0; source < 4u; source++) {
		if (((swizzle >> (source * 3u)) & 7u) == 4u + component) return source;
	}
	return UINT32_MAX;
}

Format::BufferFormatInfo ImageConversionFormat(const EmitterState&   state,
                                               const IR::MemoryInfo& mem) {
	const auto& image = state.program.info.images[mem.resource];
	const auto format = image.conversion_format;
	if (format == Prospero::BufferFormat::kInvalid) return {};
	const auto info = Format::GetFormatInfo(format);
	const bool raw_signed = IR::RawSignedStorageFormat(image, format) != Prospero::BufferFormat::kInvalid;
	EXIT_IF((Prospero::RemapTextureFormat(format) == format && !raw_signed) || info.component_count == 0u ||
	        info.component_count > 4u);
	EXIT_IF(!raw_signed && info.type != Format::ComponentType::Uscaled &&
	        ((info.type != Format::ComponentType::Uint &&
	          info.type != Format::ComponentType::Unorm) || !info.packed_bitfield ||
	         info.byte_size != sizeof(uint32_t)));
	return info;
}

uint32_t ExtractPackedImageComponent(EmitterState& state, const Format::BufferFormatInfo& info,
                                     uint32_t packed, uint32_t type, uint32_t component) {
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpBitFieldUExtract, type, result, packed,
	                          ConstantU32(state, info.component_bit_offset[component]),
	                          ConstantU32(state, info.component_bits[component]));
	return result;
}

uint32_t ImageGatherSource(const EmitterState& state, const IR::MemoryInfo& mem) {
	const auto component = ImageGatherComponent(mem.dmask);
	const auto info      = ImageConversionFormat(state, mem);
	if (info.format == Prospero::BufferFormat::kInvalid) return component;
	if (info.packed_bitfield) return 0u;
	const auto selector =
	    (state.program.info.images[mem.resource].shader_swizzle >> (component * 3u)) & 7u;
	return selector >= 4u ? (selector - 4u) % info.component_count : 0u;
}

uint32_t UnpackImageTexel(ValueEmitContext& ctx, const IR::MemoryInfo& mem, uint32_t texel) {
	const auto info = ImageConversionFormat(ctx.state, mem);
	if (info.format == Prospero::BufferFormat::kInvalid) return texel;

	const auto numeric_class = ctx.state.program.info.images[mem.resource].numeric_class;
	const auto scalar_type   = ImageScalarType(ctx.state, numeric_class);
	uint32_t   packed        = 0;
	if (info.packed_bitfield) {
		packed = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(spv::OpCompositeExtract, scalar_type, packed, texel, 0u);
	}
	uint32_t components[4] {};
	for (uint32_t component = 0; component < info.component_count; component++) {
		if (info.packed_bitfield) {
			components[component] =
			    ExtractPackedImageComponent(ctx.state, info, packed, scalar_type, component);
			components[component] =
			    NormalizeFormatComponent(ctx.state, info, component, components[component]);
		} else {
			components[component] = ctx.state.builder.AllocateId();
			ctx.state.builder.AddFunction(spv::OpCompositeExtract, scalar_type,
			                              components[component], texel, component);
			// UNorm backing preserves the guest's filtering; scaling precedes swizzle constants.
			components[component] = Binary(ctx.state, spv::OpFMul, scalar_type, components[component],
			                               ConstantF32Value(ctx.state, 255.0f));
		}
	}
	for (uint32_t component = info.component_count; component < 4u; component++) {
		components[component] = components[component % info.component_count];
	}

	const auto swizzle = ctx.state.program.info.images[mem.resource].shader_swizzle;
	uint32_t   selected[4] {};
	for (uint32_t component = 0; component < 4u; component++) {
		const auto selector = (swizzle >> (component * 3u)) & 7u;
		if (selector == 1u) {
			selected[component] = info.packed_bitfield
			    ? ConstantU32(ctx.state, Format::FormattedConstantBits(info, Format::FormattedSourceKind::One))
			    : ConstantF32Value(ctx.state, 1.0f);
		} else if (selector >= 4u) {
			selected[component] = components[selector - 4u];
		} else {
			selected[component] = info.packed_bitfield ? ConstantU32(ctx.state, 0u)
			                                          : ZeroF32(ctx.state);
		}
	}
	const auto result = ctx.state.builder.AllocateId();
	ctx.state.builder.AddFunction(spv::OpCompositeConstruct,
	                              ImageVectorType(ctx.state, numeric_class, 4), result, selected[0],
	                              selected[1], selected[2], selected[3]);
	return info.type == Format::ComponentType::Unorm
	           ? Unary(ctx.state, spv::OpBitcast, TypeF32Vector(ctx.state, 4), result) : result;
}

uint32_t UnpackImageGather(ValueEmitContext& ctx, const IR::MemoryInfo& mem, uint32_t gathered) {
	const auto info = ImageConversionFormat(ctx.state, mem);
	if (info.format == Prospero::BufferFormat::kInvalid) return gathered;

	const auto component = ImageGatherComponent(mem.dmask);
	const auto selector =
	    (ctx.state.program.info.images[mem.resource].shader_swizzle >> (component * 3u)) & 7u;
	const auto numeric_class = ctx.state.program.info.images[mem.resource].numeric_class;
	const auto vector_type   = ImageVectorType(ctx.state, numeric_class, 4);
	if (selector < 4u) {
		uint32_t value;
		switch (info.type) {
			case Format::ComponentType::Uscaled:
				value = ConstantF32Value(ctx.state, selector == 1u ? 1.0f : 0.0f);
				break;
			default: value = ConstantU32(ctx.state, selector == 1u ? 1u : 0u); break;
		}
		return ctx.state.builder.Constant(spv::OpConstantComposite, vector_type, value, value,
		                                  value, value);
	}
	if (!info.packed_bitfield) {
		const auto scale  = ConstantF32Value(ctx.state, 255.0f);
		const auto scales = ctx.state.builder.Constant(spv::OpConstantComposite, vector_type,
		                                                scale, scale, scale, scale);
		return Binary(ctx.state, spv::OpFMul, vector_type, gathered, scales);
	}

	return ExtractPackedImageComponent(ctx.state, info, gathered, TypeU32Vector(ctx.state, 4),
	                                   (selector - 4u) % info.component_count);
}

uint32_t SamplePackedUnormLz(ValueEmitContext& ctx, const IR::MemoryInfo& mem, uint32_t coord) {
	auto& state = ctx.state;
	const auto& image = state.program.info.images[mem.resource];
	const auto info = ImageConversionFormat(state, mem);
	const auto sampled = MakeSampledImage(state, mem.resource,
	                                     LoadSamplerDescriptor(state, mem.sampler));
	const auto gathered = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpImageGather, TypeU32Vector(state, 4), gathered, sampled,
	                          coord, ConstantU32(state, 0u));
	state.builder.RequireCapability(spv::CapabilityImageQuery);
	const auto size = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpImageQuerySizeLod, TypeU32Vector(state, 2), size,
	                          LoadImageDescriptor(state, mem.resource), ConstantU32(state, 0u));
	const auto size_f32 = Unary(state, spv::OpConvertUToF, TypeF32Vector(state, 2), size);
	const auto half = ConstantF32Value(state, 0.5f);
	const auto halves = state.builder.Constant(spv::OpConstantComposite,
	                                           TypeF32Vector(state, 2), half, half);
	const auto fractions = state.builder.AllocateId();
	state.builder.AddFunction(
	    spv::OpExtInst, TypeF32Vector(state, 2), fractions, GlslStd450(state), GLSLstd450Fract,
	    Binary(state, spv::OpFSub, TypeF32Vector(state, 2),
	           Binary(state, spv::OpFMul, TypeF32Vector(state, 2), coord, size_f32), halves));
	uint32_t fraction[2];
	uint32_t complement[2];
	for (uint32_t axis = 0; axis < 2u; axis++) {
		fraction[axis] = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpCompositeExtract, TypeF32(state), fraction[axis],
		                          fractions, axis);
		complement[axis] = Binary(state, spv::OpFSub, TypeF32(state),
		                          ConstantF32Value(state, 1.0f), fraction[axis]);
	}
	// Gather order is (x0, y1), (x1, y1), (x1, y0), (x0, y0).
	const auto weights = state.builder.AllocateId();
	state.builder.AddFunction(
	    spv::OpCompositeConstruct, TypeF32Vector(state, 4), weights,
	    Binary(state, spv::OpFMul, TypeF32(state), complement[0], fraction[1]),
	    Binary(state, spv::OpFMul, TypeF32(state), fraction[0], fraction[1]),
	    Binary(state, spv::OpFMul, TypeF32(state), fraction[0], complement[1]),
	    Binary(state, spv::OpFMul, TypeF32(state), complement[0], complement[1]));
	uint32_t decoded[4] {};
	uint32_t selected[4] {ZeroF32(state), ZeroF32(state), ZeroF32(state), ZeroF32(state)};
	for (uint32_t component = 0; component < 4u; component++) {
		if ((mem.dmask & (1u << component)) == 0u) continue;
		const auto selector = (image.shader_swizzle >> (component * 3u)) & 7u;
		if (selector == 1u) {
			selected[component] = ConstantF32Value(state, 1.0f);
		} else if (selector >= 4u) {
			const auto physical = (selector - 4u) % info.component_count;
			if (decoded[physical] == 0u) {
				const auto texels = Unary(
				    state, spv::OpConvertUToF, TypeF32Vector(state, 4),
				    ExtractPackedImageComponent(state, info, gathered,
				                                TypeU32Vector(state, 4), physical));
				decoded[physical] = Binary(
				    state, spv::OpFMul, TypeF32(state),
				    Binary(state, spv::OpDot, TypeF32(state), texels, weights),
				    ConstantF32Value(state, 1.0f / ((1u << info.component_bits[physical]) - 1u)));
			}
			selected[component] = decoded[physical];
		}
	}
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpCompositeConstruct, TypeF32Vector(state, 4), result,
	                          selected[0], selected[1], selected[2], selected[3]);
	return result;
}

uint32_t EmitOneDimensionalGatherLz(ValueEmitContext& ctx, const IR::MemoryInfo& mem,
                                    uint32_t coord, Prospero::TextureNumericClass numeric_class) {
	auto& state = ctx.state;
	state.builder.RequireCapability(spv::CapabilityImageQuery);
	const auto image = LoadImageDescriptor(state, mem.resource);
	const auto width = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpImageQuerySizeLod, TypeU32(state), width, image,
	                          ConstantU32(state, 0));
	const auto width_f32 = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpConvertUToF, TypeF32(state), width_f32, width);
	const auto left = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpExtInst, TypeF32(state), left, GlslStd450(state),
	                          GLSLstd450Floor,
	                          Binary(state, spv::OpFSub, TypeF32(state),
	                                 Binary(state, spv::OpFMul, TypeF32(state), coord, width_f32),
	                                 ConstantF32(state, 0x3f000000u)));

	const auto sampled = MakeSampledImage(state, mem.resource,
	                                     LoadSamplerDescriptor(state, mem.sampler));
	const auto vector_type = ImageVectorType(state, numeric_class, 4);
	const auto scalar_type = ImageScalarType(state, numeric_class);
	const auto component = ImageGatherSource(state, mem);
	uint32_t values[2] {};
	for (uint32_t index = 0; index < 2u; index++) {
		const auto sample_coord =
		    Binary(state, spv::OpFDiv, TypeF32(state),
		           Binary(state, spv::OpFAdd, TypeF32(state), left,
		                  ConstantF32(state, index == 0u ? 0x3f000000u : 0x3fc00000u)),
		           width_f32);
		const auto texel = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpImageSampleExplicitLod, vector_type, texel, sampled,
		                          sample_coord, spv::ImageOperandsLodMask, ZeroF32(state));
		values[index] = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpCompositeExtract, scalar_type, values[index], texel,
		                          component);
	}
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpCompositeConstruct, vector_type, result, values[0], values[1],
	                          values[1], values[0]);
	return result;
}

uint32_t StoreTexel(ValueEmitContext& ctx, const IR::MemoryInfo& mem, uint32_t data, bool integer) {
	const auto swizzle = ctx.state.program.info.images[mem.resource].shader_swizzle;
	const auto info = ImageConversionFormat(ctx.state, mem);
	const bool converted = info.format != Prospero::BufferFormat::kInvalid;
	const bool packed = info.packed_bitfield;
	EXIT_IF(converted && info.type != Format::ComponentType::Sint &&
	        (!packed || (info.type != Format::ComponentType::Uint &&
	                     info.type != Format::ComponentType::Unorm)));
	const auto zero = ConstantU32(ctx.state, 0);
	uint32_t values[4] {zero, zero, zero, zero};
	auto packed_value = zero;
	const auto dmask = mem.dmask != 0u ? mem.dmask : 1u;
	for (uint32_t component = 0; component < (converted ? info.component_count : 4u); component++) {
		const auto source = InverseSwizzle(swizzle, component);
		const bool selected = source < 4u && ((dmask >> source) & 1u) != 0u;
		uint32_t raw = zero;
		if (selected) {
			const auto packed_index = DmaskComponentIndex(dmask, source);
			raw                     = ctx.state.builder.AllocateId();
			ctx.state.builder.AddFunction(spv::OpCompositeExtract, TypeU32(ctx.state), raw, data,
			                              mem.data_bits == 16u ? packed_index / 2u : packed_index);
			if (mem.data_bits == 16u) {
				if ((packed_index & 1u) != 0u) {
					raw = Binary(ctx.state, spv::OpShiftRightLogical, TypeU32(ctx.state), raw,
					             ConstantU32(ctx.state, 16u));
				}
				if (!packed) {
					raw = Binary(ctx.state, spv::OpBitwiseAnd, TypeU32(ctx.state), raw,
					             ConstantU32(ctx.state, 0xffffu));
				}
			}
		}
		if (converted) {
			if (!selected) continue;
			if (!packed) {
				values[component] =
				    EncodeFormattedStoreComponent(ctx.state, info, component, raw, mem.data_bits);
				continue;
			}
			const auto value =
			    EncodePackedColorComponent(ctx.state, info, component, raw, mem.data_bits);
			packed_value = packed_value == zero ? value
			    : Binary(ctx.state, spv::OpBitwiseOr, TypeU32(ctx.state), packed_value, value);
			continue;
		}
		values[component] = integer ? raw
		                    : mem.data_bits == 16u
		                        ? EmitF16BitsToF32(ctx.state, raw)
		                        : Unary(ctx.state, spv::OpBitcast, TypeF32(ctx.state), raw);
	}
	if (packed) values[0] = packed_value;
	const auto texel = ctx.state.builder.AllocateId();
	ctx.state.builder.AddFunction(spv::OpCompositeConstruct,
	                              integer ? TypeU32Vector(ctx.state, 4)
	                                      : TypeF32Vector(ctx.state, 4),
	                              texel, values[0], values[1], values[2], values[3]);
	return texel;
}

template <typename Emit>
auto EmitImageAccess(ValueEmitContext& ctx, const IR::Inst& inst, uint32_t result_type,
                     const Emit& emit) {
	auto& state = ctx.state;
	const auto& mem = ctx.Memory(inst);
	const auto& image = state.program.info.images.at(mem.resource);
	if (image.indirect_root != mem.resource) return emit(mem.resource, 0u);
	const auto* handle = inst.Arg(0).ResolveInstruction();
	const auto* source = image.source < state.program.descriptor_sources.size()
	                         ? &state.program.descriptor_sources[image.source]
	                         : nullptr;
	if (handle == nullptr || source == nullptr || !source->indirect_descriptor.has_value() ||
	    handle->NumArgs() == 0u) {
		ctx.Fail(inst, "has invalid indirect image key provenance");
	}
	const auto key = ctx.Def(handle->Arg(0));
	if (state.flattened_srt_variable == 0 ||
	    image.indirect_resources.size() < 2u) {
		ctx.Fail(inst, "has no indirect image runtime mapping");
	}
	const auto selected = EmitIndirectResourceIndex(
	    state, key, image.indirect_mapping_offset, image.indirect_search_iterations, 0u);
	const auto SameType = [&](uint32_t lhs, uint32_t rhs) {
		const auto& first = state.program.info.images[lhs];
		const auto& second = state.program.info.images[rhs];
		return first.dimension == second.dimension && first.cube == second.cube &&
		       IR::DescriptorBindingForImage(first) == IR::DescriptorBindingForImage(second) &&
		       (inst.GetOpcode() != IR::ValueOpcode::ImageWrite ||
		        first.shader_swizzle == second.shader_swizzle) &&
		       first.mip_count == 1u && second.mip_count == 1u;
	};
	// Descriptor order is unrelated to image type. Share each typed access and
	// select its descriptor slot independently, including gaps between native roots.
	struct ImageGroup { uint32_t resource; uint32_t count; };
	std::vector<ImageGroup> groups;
	std::vector<uint32_t> ordinal_groups;
	for (const auto resource: image.indirect_resources) {
		const auto found = std::ranges::find_if(groups, [&](const ImageGroup& group) {
			return SameType(resource, group.resource);
		});
		ordinal_groups.push_back(static_cast<uint32_t>(found - groups.begin()));
		if (found == groups.end()) groups.push_back({resource, 1u});
		else ++found->count;
	}
	const bool dynamic_slot = std::ranges::any_of(groups, [](const ImageGroup& group) {
		return group.count > 1u;
	});
	auto group_index = ConstantU32(state, 0u);
	auto slot_bias = ConstantU32(state, image.descriptor_index);
	auto previous_bias = image.descriptor_index;
	for (uint32_t ordinal = 1; ordinal < image.indirect_resources.size(); ++ordinal) {
		const auto& candidate = state.program.info.images[image.indirect_resources[ordinal]];
		const auto bias = candidate.descriptor_index - ordinal;
		const bool select_group = ordinal_groups[ordinal] != ordinal_groups[ordinal - 1u];
		const bool select_slot = dynamic_slot && bias != previous_bias;
		previous_bias = bias;
		if (!select_group && !select_slot) continue;
		const auto at_or_after = Binary(state, spv::OpUGreaterThanEqual, TypeBool(state),
		                               selected, ConstantU32(state, ordinal));
		if (select_group) group_index = Select(state, TypeU32(state), at_or_after,
		                                      ConstantU32(state, ordinal_groups[ordinal]), group_index);
		if (select_slot) slot_bias = Select(state, TypeU32(state), at_or_after,
		                                   ConstantU32(state, bias), slot_bias);
	}
	const auto slot = dynamic_slot
	    ? Binary(state, spv::OpIAdd, TypeU32(state), selected, slot_bias) : 0u;
	const auto EmitGroup = [&](uint32_t index) {
		const auto& group = groups[index];
		return emit(group.resource, group.count == 1u ? 0u : slot);
	};
	return groups.size() == 1u
	           ? EmitGroup(0u)
	           : EmitIndexSwitch(state, group_index, static_cast<uint32_t>(groups.size()),
	                             result_type, EmitGroup);
}

template <typename Emit>
uint32_t EmitImageResult(ValueEmitContext& ctx, const IR::Inst& inst, const Emit& emit) {
	auto& state = ctx.state;
	const auto& mem = ctx.Memory(inst);
	const auto& image = state.program.info.images[mem.resource];
	const bool gather = inst.GetOpcode() == IR::ValueOpcode::ImageGatherRaw;
	const bool dref = HasFlag(mem, Decoder::ImageSampleFlagCompare);
	const bool scalar = dref && !gather;
	const bool mixed = std::ranges::any_of(image.indirect_resources, [&](uint32_t resource) {
		return state.program.info.images[resource].numeric_class != image.numeric_class;
	});
	const auto native_type = [&](uint32_t resource) {
		return scalar ? TypeF32(state) : ImageVectorType(
		    state, dref ? Prospero::TextureNumericClass::Float
		                : state.program.info.images[resource].numeric_class, 4);
	};
	const auto result_bits = [&](uint32_t resource, uint32_t value) {
		auto candidate_mem = mem;
		candidate_mem.resource = resource;
		if (!scalar) {
			value = gather ? UnpackImageGather(ctx, candidate_mem, value)
			               : UnpackImageTexel(ctx, candidate_mem, value);
		}
		return ResultVector(ctx, value,
		                    dref ? Prospero::TextureNumericClass::Float
		                         : state.program.info.images[resource].numeric_class,
		                    scalar, candidate_mem, gather);
	};
	// RDNA returns register bits. Convert per typed run only when the join mixes
	// numeric classes; homogeneous accesses keep their single conversion after it.
	const auto value = EmitImageAccess(
	    ctx, inst, mixed ? TypeU32Vector(state, 4) : native_type(mem.resource),
	    [&](uint32_t resource, uint32_t array_index) {
		    const auto native = emit(resource, array_index, native_type(resource));
		    return mixed ? result_bits(resource, native) : native;
	    });
	return mixed ? value : result_bits(mem.resource, value);
}

struct ImageSampler {
	uint32_t id = 0;
	bool dynamic = false;
};

std::array<ImageSampler, 2> LoadImageSamplers(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto& state = ctx.state;
	const auto& mem = ctx.Memory(inst);
	const auto& samplers = state.program.info.samplers;
	const auto& root = samplers.at(mem.sampler);
	std::array<ImageSampler, 2> result {};
	const auto load = [&](const IR::ImageResource& image) {
		const bool integer = image.numeric_class != Prospero::TextureNumericClass::Float;
		auto& sampler = result[integer];
		if (sampler.id != 0u) return;
		uint32_t index = mem.sampler;
		if (root.integer_border != integer) {
			const auto found = std::ranges::find_if(samplers, [&](const auto& candidate) {
				return candidate.snapshot_index == root.snapshot_index &&
				       candidate.integer_border == integer;
			});
			if (found == samplers.end()) ctx.Fail(inst, "has no typed sampler variant");
			index = static_cast<uint32_t>(found - samplers.begin());
		}
		uint32_t array_index = 0;
		const auto& candidates = samplers[index].indirect_resources;
		if (!candidates.empty()) {
			const auto* handle = inst.Arg(1).ResolveInstruction();
			if (handle == nullptr || handle->GetOpcode() != IR::ValueOpcode::GetSamplerResource)
				ctx.Fail(inst, "has invalid finite sampler key provenance");
			auto selected = ctx.Def(handle->Arg(0));
			if (samplers[index].indirect_root != IR::SamplerResource::NoIndirectSampler)
				selected = EmitIndirectResourceIndex(state, selected,
				    samplers[index].indirect_mapping_offset,
				    samplers[index].indirect_search_iterations, 0u);
			array_index = EmitIndexSwitch(
			    state, selected, static_cast<uint32_t>(candidates.size()),
			    TypeU32(state), [&](uint32_t ordinal) { return ConstantU32(state, candidates[ordinal]); });
		}
		sampler = {LoadSamplerDescriptor(state, index, array_index), array_index != 0u};
	};
	const auto& image = state.program.info.images[mem.resource];
	load(image);
	for (const auto candidate: image.indirect_resources) load(state.program.info.images[candidate]);
	return result;
}

} // namespace

void EmitImage(ValueEmitContext& ctx, const IR::Inst& inst) {
	const auto op         = inst.GetOpcode();
	const auto image_info = IR::ImageOpcodeInfoOf(op);
	auto&       state     = ctx.state;
	const auto& mem       = ctx.Memory(inst);
	const auto  image_arg = inst.Arg(0);
	ctx.ResourceIndex(image_arg, IR::ValueOpcode::GetImageResource);
	const auto& image   = state.program.info.images.at(mem.resource);
	const auto* address = ctx.ImageAddress(inst.Arg(image_info.needs_sampler ? 2 : 1));
	if (op == IR::ValueOpcode::ImageQueryDimensions) {
		state.builder.RequireCapability(spv::CapabilityImageQuery);
		ctx.Define(inst, EmitImageAccess(ctx, inst, TypeU32Vector(state, 4),
		    [&](uint32_t resource, uint32_t array_index) {
			    return QueryDimensions(ctx, mem, *address, resource, array_index);
		    }));
		return;
	}
	if (image_info.needs_sampler &&
	    image.conversion_format == Prospero::BufferFormat::k11_11_10UNorm) {
		if (op != IR::ValueOpcode::ImageSampleRaw ||
		    image.dimension != ImageDimension::Dim2D ||
		    mem.image_dimension != ImageDimension::Dim2D || mem.data_bits != 32u ||
		    mem.image_sample_flags != Decoder::ImageSampleFlagLevelZero ||
		    image.indirect_root != IR::ImageResource::NoIndirectImage ||
		    !state.program.info.samplers.at(mem.sampler).indirect_resources.empty()) {
			ctx.Fail(inst, "packed UNorm sampling requires a direct 2D sample_lz");
			return;
		}
		const auto coord = CoordF32(ctx, mem, *address, Layout(mem).coord, 2u);
		ctx.Define(inst, ResultVector(ctx, SamplePackedUnormLz(ctx, mem, coord),
		                              Prospero::TextureNumericClass::Float, false, mem));
		return;
	}
	if (op == IR::ValueOpcode::ImageQueryLod) {
		state.builder.RequireCapability(spv::CapabilityImageQuery);
		const auto dimension = image.dimension;
		const auto sampled = MakeSampledImage(state, mem.resource,
		                                     LoadSamplerDescriptor(state, mem.sampler));
		const auto lod       = state.builder.AllocateId();
		state.builder.AddFunction(
		    spv::OpImageQueryLod, TypeF32Vector(state, 2), lod, sampled,
		    CoordF32(ctx, mem, *address, 0, ImageDimensionInfoFor(dimension).spatial_components,
		             image.cube));
		uint32_t values[4] = {ConstantU32(state, 0), ConstantU32(state, 0), ConstantU32(state, 0),
		                      ConstantU32(state, 0)};
		for (uint32_t index = 0; index < 2u; index++) {
			const auto component = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpCompositeExtract, TypeF32(state), component, lod,
			                          index);
			values[index] = FloatBits(ctx, component);
		}
		const auto result = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(state, 4), result,
		                          values[0], values[1], values[2], values[3]);
		ctx.Define(inst, result);
		return;
	}
	if (op == IR::ValueOpcode::ImageRead) {
		const auto condition = ctx.Arg(inst, 2);
		ctx.Define(
		    inst,
		    EmitValueOrDefaultIfCondition(
		        state, condition, TypeU32Vector(state, 4), ConstantU32CompositeZero(state, 4),
		        [&]() {
			        return EmitImageResult(
			            ctx, inst, [&](uint32_t resource, uint32_t array_index, uint32_t result_type) {
				        const auto& candidate = state.program.info.images[resource];
				        const auto dimension = candidate.dimension;
				        const auto& dimension_info = ImageDimensionInfoFor(dimension);
				        const auto descriptor = LoadImageDescriptor(state, resource, 0u, array_index);
				        const auto fetched = state.builder.AllocateId();
				        const auto coord = CoordU32(ctx, mem, *address, dimension);
				        if (dimension_info.multisampled != 0u) {
					        state.builder.AddFunction(
					            spv::OpImageFetch, result_type, fetched,
					            descriptor, coord, spv::ImageOperandsSampleMask,
					            AddressU32(ctx, mem, *address, dimension_info.coordinate_components));
				        } else {
					        state.builder.AddFunction(
					            spv::OpImageFetch, result_type, fetched,
					            descriptor, coord, spv::ImageOperandsLodMask,
					            LodU32(ctx, mem, *address, mem.image_dimension));
				        }
				        return fetched;
			            });
		        }));
		return;
	}
	if (op == IR::ValueOpcode::ImageWrite) {
		EmitIfCondition(state, ctx.Arg(inst, 3), [&]() {
			EmitImageAccess(ctx, inst, 0u, [&](uint32_t resource, uint32_t array_index) {
				const auto& candidate = state.program.info.images[resource];
				auto selected = mem;
				selected.resource = resource;
				const auto mip_lod = candidate.mip_mode == IR::ImageMipMode::Dynamic
				    ? LodU32(ctx, mem, *address, mem.image_dimension) : 0u;
				const auto coord = CoordU32(ctx, mem, *address, candidate.dimension);
				const auto texel = StoreTexel(ctx, selected, ctx.Arg(inst, 2),
				    candidate.numeric_class == Prospero::TextureNumericClass::Uint);
				EmitStorageImageWrite(state, resource, mip_lod, coord, texel, array_index);
			});
		});
		return;
	}
	if (op == IR::ValueOpcode::ImageSampleRaw || op == IR::ValueOpcode::ImageGatherRaw) {
		const auto  dimension      = image.dimension;
		const auto& dimension_info = ImageDimensionInfoFor(dimension);
		const auto  layout         = Layout(mem);
		const auto  numeric_class  = image.numeric_class;
		const bool  dref           = HasFlag(mem, Decoder::ImageSampleFlagCompare);
		IR::Value offset_value;
		if (layout.offset != NoImageComponent) {
			const auto value = address->Arg(layout.offset).Resolve();
			offset_value = IR::ResolveActiveU32(value, inst.Arg(inst.NumArgs() - 1u).Resolve());
			if (offset_value.IsEmpty()) offset_value = value;
		}
		if (dref && state.program.info.images[mem.resource].conversion_format !=
		                Prospero::BufferFormat::kInvalid) {
			ctx.Fail(inst, "uses depth comparison with a converted image");
			return;
		}
		if (op == IR::ValueOpcode::ImageGatherRaw) {
			if (dimension == ImageDimension::Dim1D) {
				if (image.indirect_root == mem.resource || dref ||
				    !HasFlag(mem, Decoder::ImageSampleFlagLevelZero) ||
				    HasFlag(mem, Decoder::ImageSampleFlagOffset) ||
				    HasFlag(mem, Decoder::ImageSampleFlagGatherHorizontal)) {
					ctx.Fail(inst, "has an unsupported 1D gather variant");
					return;
				}
				const auto coord = CoordF32(ctx, mem, *address, layout.coord,
				                            dimension_info.coordinate_components, image.cube);
				const auto sample = EmitOneDimensionalGatherLz(ctx, mem, coord, numeric_class);
				ctx.Define(inst, ResultVector(ctx, UnpackImageGather(ctx, mem, sample),
				                              numeric_class, false, mem, true));
				return;
			}
			if (dimension == ImageDimension::Dim1DArray) {
				ctx.Fail(inst, "has an unsupported 1D-array gather");
				return;
			}
			uint32_t component_or_dref;
			if (dref) {
				component_or_dref = layout.dref != NoImageComponent
				                        ? AddressF32(ctx, mem, *address, layout.dref)
				                        : ZeroF32(state);
			} else {
				component_or_dref = ConstantU32(state, ImageGatherSource(state, mem));
			}
			const auto samplers = LoadImageSamplers(ctx, inst);
			const auto EmitGather = [&](uint32_t resource, uint32_t array_index, uint32_t result_type) {
				const auto& candidate = state.program.info.images[resource];
				const auto sampler = samplers[candidate.numeric_class != Prospero::TextureNumericClass::Float];
				const auto dimension = candidate.dimension;
				auto       coord = CoordF32(
				    ctx, mem, *address, layout.coord,
				    ImageDimensionInfoFor(dimension).coordinate_components, candidate.cube);
				if (state.program.info.samplers.at(mem.sampler).unnormalized) {
					NormalizeTexelCoordinates(ctx, inst, candidate, resource, array_index, coord);
				}
				uint32_t operand_mask = 0;
				uint32_t offset = 0;
				if (HasFlag(mem, Decoder::ImageSampleFlagGatherHorizontal)) {
					operand_mask = spv::ImageOperandsConstOffsetsMask;
					offset = HorizontalOffsets(state, dimension);
				} else if (layout.offset != NoImageComponent) {
					offset = PackedOffset(ctx, offset_value, dimension, operand_mask);
				}
				const auto EmitMip = [&](uint32_t mip) {
					const auto sampled = MakeSampledImage(state, resource, sampler.id, mip, array_index,
					                                      sampler.dynamic);
					const auto sample = state.builder.AllocateId();
					const std::array operands {operand_mask, offset};
					state.builder.AddFunction(
					    dref ? spv::OpImageDrefGather : spv::OpImageGather, result_type,
					    sample, sampled, coord, component_or_dref,
					    std::span(operands).first(operand_mask != 0u ? 2u : 0u));
					return sample;
				};
				return candidate.mip_mode == IR::ImageMipMode::Dynamic && candidate.mip_count > 1u
				           ? EmitIndexSwitch(
				                 state, GatherMip(ctx, inst, mem, *address, layout, candidate.mip_count),
				                 candidate.mip_count, result_type, EmitMip)
				           : EmitMip(0u);
			};
			ctx.Define(inst, EmitImageResult(ctx, inst, EmitGather));
			return;
		}
		const bool explicit_lod = HasFlag(mem, Decoder::ImageSampleFlagDerivative) ||
		                          HasFlag(mem, Decoder::ImageSampleFlagLod) ||
		                          HasFlag(mem, Decoder::ImageSampleFlagLevelZero) ||
		                          state.program.stage != ShaderType::Pixel;
		auto       opcode       = spv::OpImageSampleImplicitLod;
		if (explicit_lod) {
			opcode = dref ? spv::OpImageSampleDrefExplicitLod : spv::OpImageSampleExplicitLod;
		} else if (dref) {
			opcode = spv::OpImageSampleDrefImplicitLod;
		}
		uint32_t dref_value  = 0;
		if (dref) {
			dref_value  = ZeroF32(state);
			if (layout.dref != NoImageComponent) {
				dref_value = AddressF32(ctx, mem, *address, layout.dref);
			}
		}
		std::array<uint32_t, 4> operands {};
		size_t operand_count = 0;
		if (HasFlag(mem, Decoder::ImageSampleFlagDerivative)) {
			operands[0] = spv::ImageOperandsGradMask;
			operands[1] = CoordF32(ctx, mem, *address, layout.grad_x, dimension_info.spatial_components);
			operands[2] = CoordF32(ctx, mem, *address, layout.grad_y, dimension_info.spatial_components);
			operand_count = 3;
		} else if (explicit_lod) {
			auto lod = ZeroF32(state);
			if (HasFlag(mem, Decoder::ImageSampleFlagLod) && layout.lod != NoImageComponent) {
				lod = AddressF32(ctx, mem, *address, layout.lod);
			}
			operands[0] = spv::ImageOperandsLodMask;
			operands[1] = lod;
			operand_count = 2;
		} else if (layout.bias != NoImageComponent) {
			operands[0] = spv::ImageOperandsBiasMask;
			operands[1] = AddressF32(ctx, mem, *address, layout.bias);
			operand_count = 2;
		}
		const bool move_offset = layout.offset != NoImageComponent && !offset_value.IsImmediate() &&
		                         !state.maintenance8_enabled && GetHostShaderFeatures().sample_offset_by_coordinates;
		if (layout.offset != NoImageComponent &&
		    !offset_value.IsImmediate() && !state.maintenance8_enabled && !move_offset) {
			ctx.Fail(inst, "requires maintenance8 for a dynamic sample texel offset");
		}
		const auto samplers = LoadImageSamplers(ctx, inst);
		const auto EmitSample = [&](uint32_t resource, uint32_t array_index, uint32_t result_type) {
			const auto& candidate = state.program.info.images[resource];
			const auto sampler = samplers[candidate.numeric_class != Prospero::TextureNumericClass::Float];
			auto sample_operands = operands;
			auto sample_operand_count = operand_count;
			auto sample_opcode = opcode;
			if (layout.offset != NoImageComponent && !move_offset) {
				sample_operand_count = std::max(size_t{1}, sample_operand_count);
				sample_operands[sample_operand_count++] = PackedOffset(
				    ctx, offset_value, candidate.dimension, sample_operands[0]);
			}
			auto coord =
			    CoordF32(ctx, mem, *address, layout.coord,
			             ImageDimensionInfoFor(candidate.dimension).coordinate_components,
			             candidate.cube);
			if (state.program.info.samplers.at(mem.sampler).unnormalized) {
				NormalizeTexelCoordinates(ctx, inst, candidate, resource, array_index, coord);
			}
			if (move_offset) {
				MoveCoordinatesByOffset(ctx, inst, candidate, resource, array_index, offset_value, coord,
				                        sample_opcode, sample_operands, sample_operand_count);
			}
			const auto sampled = MakeSampledImage(state, resource, sampler.id, 0u, array_index,
			                                      sampler.dynamic);
			const auto sample = state.builder.AllocateId();
			state.builder.AddFunction(sample_opcode, result_type, sample, sampled, coord,
			                          std::span(&dref_value, dref ? 1u : 0u),
			                          std::span<const uint32_t>(sample_operands).first(sample_operand_count));
			return sample;
		};
		ctx.Define(inst, EmitImageResult(ctx, inst, EmitSample));
		return;
	}
	if (image_info.access == IR::ImageAccess::Atomic && image.atomic64 &&
	    GetHostShaderFeatures().image_atomic64_lock) {
		ctx.Define(inst, EmitValueOrDefaultIfCondition(
		                     state, ctx.Arg(inst, inst.NumArgs() - 1), TypeU64(state), ConstantU64(state, 0),
		                     [&]() { return EmitLockedImageAtomic64(ctx, inst, mem, *address, image); }));
		return;
	}
	if (image_info.access == IR::ImageAccess::Atomic) {
		const auto dimension = image.dimension;
		const auto result_type = TypeId(state, inst.GetType());
		const auto zero = image.atomic64 ? ConstantU64(state, 0) : ConstantU32(state, 0);
		ctx.Define(inst, EmitValueOrDefaultIfCondition(
		                     state, ctx.Arg(inst, inst.NumArgs() - 1), result_type, zero, [&]() {
			           const auto pointer      = state.builder.AllocateId();
			           const auto pointer_type =
			               state.builder.Type(spv::OpTypePointer, spv::StorageClassImage,
			                                  image.atomic64 ? TypeU64(state) : TypeU32(state));
			           state.builder.AddFunction(spv::OpImageTexelPointer, pointer_type, pointer,
			                                     ImageDescriptorPointer(state, mem.resource),
			                                     CoordU32(ctx, mem, *address, dimension),
			                                     ConstantU32(state, 0));
			           if (op == IR::ValueOpcode::ImageAtomicFMin32 ||
			               op == IR::ValueOpcode::ImageAtomicFMax32) {
				           return AtomicUpdate(state, pointer, IR::ResourceKind::Image,
				                               [&](uint32_t old) {
					                               return EmitFloatAtomicReplacement(
					                                   state, old, ctx.Arg(inst, 2),
					                                   op == IR::ValueOpcode::ImageAtomicFMax32);
				                               });
			           }
			           const auto old = EmitAtomicOperation(ctx, inst, pointer, spv::ScopeDevice);
			           EmitAtomicMemoryBarrier(state, IR::ResourceKind::Image);
			           return old;
		           }));
		return;
	}
	ctx.Fail(inst, "has no image SPIR-V emitter");
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
