// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// gfx7 (Sea Islands, the PS4 GPU) shader decoding for PS4 backward compatibility.
//
// RDNA kept most GCN opcode numbers, so gfx7 code mostly decodes with the gfx10 tables: SOP*,
// VOP1, VOPC, DS, MUBUF, MTBUF, MIMG, VINTRP and EXP match (checked against the Sea Islands
// numbering). What differs is handled here by rewriting the instruction into its gfx10 encoding
// and decoding that, then restoring the original words:
// - SMRD (one dword, 8-bit offset) instead of SMEM;
// - VOP3 with prefix 0x34, a 9-bit opcode at bit 17 and clamp at bit 11 (PS4 Pro code adds a
//   tenth opcode bit at bit 16 for a few gfx10-numbered operations);
// - a few VOP2 opcodes: V_CNDMASK_B32 is 0, and the lane, carry, BFM, LDEXP and packing ops that
//   gfx10 only has as VOP3;
// - MIMG without a dimension field (2D, or 2D array when DA is set);
// - MUBUF/MTBUF ADDR64, which gfx10 dropped.

#include "graphics/shader/recompiler/frontend/decode/Gfx7Ops.h"

#include "graphics/shader/recompiler/frontend/decode/ImageOps.h"
#include "graphics/shader/recompiler/frontend/decode/MemoryOps.h"
#include "graphics/shader/recompiler/frontend/decode/VectorAluOps.h"

#include <algorithm>
#include <array>

namespace Libs::Graphics::ShaderRecompiler::Decoder {

namespace {

constexpr uint32_t Vop3Prefix   = 0x35u << 26u;
constexpr uint32_t SmemPrefix   = 0x3du << 26u;
constexpr uint32_t SgprNull     = 0x7du;
constexpr uint32_t SgprVccLo    = 106u;
constexpr uint32_t LiteralCode  = 255u;
constexpr uint32_t Unsupported  = 0xffffffffu;

// gfx7 VOP2 opcodes that gfx10 only encodes as VOP3: their gfx10 VOP3 opcode.
uint32_t Vop2AsVop3(uint32_t opcode) {
	switch (opcode) {
		case 0x01u: return 0x360u; // V_READLANE_B32
		case 0x02u: return 0x361u; // V_WRITELANE_B32
		case 0x1eu: return 0x363u; // V_BFM_B32
		case 0x25u: return 0x30fu; // V_ADD_I32 (carry out)
		case 0x26u: return 0x310u; // V_SUB_I32
		case 0x27u: return 0x319u; // V_SUBREV_I32
		case 0x2bu: return 0x362u; // V_LDEXP_F32
		case 0x2du: return 0x368u; // V_CVT_PKNORM_I16_F32
		case 0x2eu: return 0x369u; // V_CVT_PKNORM_U16_F32
		case 0x30u: return 0x36au; // V_CVT_PK_U16_U32
		case 0x31u: return 0x36bu; // V_CVT_PK_I16_I32
		default: return 0u;
	}
}

// gfx7 VOP2 opcodes with no counterpart here yet (legacy min/max, PKACCUM).
bool Vop2Unsupported(uint32_t opcode) {
	return opcode == 0x0du || opcode == 0x0eu || opcode == 0x2cu;
}

// VOP3B in gfx7: SDST at bits 14:8 and no clamp.
bool IsVop3b(uint32_t opcode) {
	return (opcode >= 0x125u && opcode <= 0x12au) || opcode == 0x16du || opcode == 0x16eu ||
	       opcode == 0x176u || opcode == 0x177u;
}

// A gfx7 VOP3 opcode as the gfx10 VOP3 opcode of the same operation.
uint32_t Vop3Opcode(uint32_t opcode) {
	if (opcode >= 0x200u) {
		// With bit 16 set (PS4 Pro), the opcode is the gfx10 number of these operations
		// (V_ADD_NC_U16, V_MAD_U32_U16, V_ADD3_U32, V_LSHL_OR_B32, V_AND_OR_B32, V_OR3_B32...).
		// List from AnyPS5 (AnyPS5 contributors, GPL-2.0-only), GcnOpcodeTables.hpp.
		constexpr std::array<uint16_t, 16> Extended {0x303, 0x307, 0x308, 0x30d, 0x30e, 0x314,
		                                             0x346, 0x347, 0x351, 0x354, 0x357, 0x35e,
		                                             0x36d, 0x36f, 0x371, 0x372};
		return std::ranges::find(Extended, opcode) != Extended.end() ? opcode : Unsupported;
	}
	if (opcode >= 0x100u && opcode <= 0x13fu) {
		const auto vop2 = opcode - 0x100u;
		if (vop2 == 0x00u) {
			return 0x101u; // V_CNDMASK_B32
		}
		if (const auto vop3 = Vop2AsVop3(vop2); vop3 != 0u) {
			return vop3;
		}
		if (Vop2Unsupported(vop2)) {
			return Unsupported;
		}
	}
	return opcode;
}

void Restore(Instruction& inst, std::span<const uint32_t> code, uint32_t word_index,
             uint32_t word_count, uint32_t opcode) {
	SetRawWords(inst, code, word_index, word_count);
	inst.opcode_id = opcode;
}

uint32_t WordAt(std::span<const uint32_t> code, uint32_t index) {
	return index < code.size() ? code[index] : 0u;
}

void DecodeVop3Gfx7(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                    Instruction& inst) {
	const uint32_t word0 = code[word_index];
	// Bit 16 is the opcode's tenth bit: without it V_ADD3_U32 (0x36d) decoded as V_DIV_SCALE_F32
	// (0x16d) and wrote an SGPR pair.
	const uint32_t opcode = ((word0 >> 17u) & 0x1ffu) | (((word0 >> 16u) & 1u) << 9u);
	// Those operations' OP_SEL bits (15:12) select 16-bit halves, which this path drops.
	const uint32_t gfx10 =
	    opcode >= 0x200u && ((word0 >> 12u) & 0xfu) != 0u ? Unsupported : Vop3Opcode(opcode);
	if (gfx10 == Unsupported) {
		inst.pc = pc;
		SetRawWords(inst, code, word_index, 2);
		SetUnsupported(inst, Family::VOP3, opcode, "gfx7 VOP3 opcode is not implemented");
		return;
	}
	uint32_t low = IsVop3b(opcode) ? (word0 & 0x7fffu)
	                               : (word0 & 0x7ffu) | (((word0 >> 11u) & 1u) << 15u);
	uint32_t word1  = WordAt(code, word_index + 1u);
	uint32_t opcode10 = gfx10;
	if (opcode >= 0x161u && opcode <= 0x163u) {
		// V_LSHL_B64, V_LSHR_B64, V_ASHR_I64 exist in gfx10 only as the *REV forms (0x2ff-0x301):
		// swap src0/src1 with their abs and neg bits.
		opcode10 = 0x2ffu + (opcode - 0x161u);
		const uint32_t src0 = word1 & 0x1ffu, src1 = (word1 >> 9u) & 0x1ffu;
		const uint32_t neg  = word1 >> 29u;
		word1 = (word1 & ~0x3ffffu & 0x1fffffffu) | src1 | (src0 << 9u) |
		        (((neg & 4u) | ((neg & 1u) << 1u) | ((neg >> 1u) & 1u)) << 29u);
		const uint32_t abs = (low >> 8u) & 7u;
		low = (low & ~(7u << 8u)) | (((abs & 4u) | ((abs & 1u) << 1u) | ((abs >> 1u) & 1u)) << 8u);
	}
	const std::array<uint32_t, 3> words {low | (opcode10 << 16u) | Vop3Prefix, word1, 0u};
	DecodeVop3(pc, words, 0, inst);
	Restore(inst, code, word_index, 2, opcode);
	// V_CMPX in VOP3 form: gfx7 writes SDST too (gfx10 drops it); the translator writes dst2.
	if (inst.dst.kind == OperandKind::ExecLo && opcode < 0x100u) {
		DecodeScalarDestination(word0 & 0xffu, pc, inst.dst2);
	}
}

bool DecodeVop2Gfx7(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                    Instruction& inst) {
	const uint32_t word    = code[word_index];
	const uint32_t opcode  = (word >> 25u) & 0x3fu;
	const uint32_t src0    = word & 0x1ffu;
	const uint32_t vsrc1   = (word >> 9u) & 0xffu;
	const uint32_t vdst    = (word >> 17u) & 0xffu;
	const uint32_t literal = WordAt(code, word_index + 1u);
	const uint32_t words_used = src0 == LiteralCode ? 2u : 1u;
	if (opcode == 0x00u) {
		// V_CNDMASK_B32 is VOP2 opcode 1 in gfx10.
		const std::array<uint32_t, 2> words {(word & ~(0x3fu << 25u)) | (0x01u << 25u), literal};
		DecodeVop2(pc, words, 0, inst);
		Restore(inst, code, word_index, words_used, opcode);
		return true;
	}
	if (Vop2Unsupported(opcode)) {
		inst.pc = pc;
		SetRawWords(inst, code, word_index, words_used);
		SetUnsupported(inst, Family::VOP2, opcode, "gfx7 VOP2 opcode is not implemented");
		return true;
	}
	const uint32_t vop3 = Vop2AsVop3(opcode);
	if (vop3 == 0u) {
		return false;
	}
	// The lane ops take an SGPR in VSRC1 (the lane select); the others a VGPR.
	const bool     lane_op = opcode == 0x01u || opcode == 0x02u;
	const uint32_t src1    = lane_op ? vsrc1 : vsrc1 + 256u;
	const bool     carry   = opcode >= 0x25u && opcode <= 0x27u;
	const uint32_t low     = vdst | (carry ? SgprVccLo << 8u : 0u);
	const std::array<uint32_t, 3> words {low | (vop3 << 16u) | Vop3Prefix, src0 | (src1 << 9u), literal};
	DecodeVop3(pc, words, 0, inst);
	Restore(inst, code, word_index, words_used, opcode);
	return true;
}

void DecodeSmrd(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                Instruction& inst) {
	const uint32_t word   = code[word_index];
	const uint32_t offset = word & 0xffu;
	const bool     imm    = ((word >> 8u) & 1u) != 0u;
	const uint32_t sbase  = (word >> 9u) & 0x3fu;
	const uint32_t sdst   = (word >> 15u) & 0x7fu;
	const uint32_t opcode = (word >> 22u) & 0x1fu;
	uint32_t       count  = 1;
	uint32_t       word1  = 0;
	if (imm) {
		word1 = (offset * 4u) | (SgprNull << 25u); // dwords
	} else if (offset == LiteralCode) {
		const uint32_t dwords = WordAt(code, word_index + 1u);
		count                 = 2;
		if (dwords >= (1u << 19u)) {
			inst.pc = pc;
			SetRawWords(inst, code, word_index, count);
			SetUnsupported(inst, Family::SMEM, opcode, "gfx7 SMRD literal offset is too large");
			return;
		}
		word1 = (dwords * 4u) | (SgprNull << 25u);
	} else {
		word1 = offset << 25u; // an SGPR holding a byte offset
	}
	// The opcodes are the gfx10 SMEM ones (S_DCACHE_INV aside, which shaders do not issue).
	const std::array<uint32_t, 2> words {sbase | (sdst << 6u) | (opcode << 18u) | SmemPrefix, word1};
	DecodeSmem(pc, words, 0, inst);
	Restore(inst, code, word_index, count, opcode);
}

void DecodeMimgGfx7(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                    Instruction& inst) {
	const uint32_t word0 = code[word_index];
	const bool     array = ((word0 >> 14u) & 1u) != 0u;
	// gfx10 DIM: 1 = 2D, 5 = 2D array. gfx7 takes the dimension from the T#.
	const uint32_t dim = array ? 5u : 1u;
	const std::array<uint32_t, 2> words {(word0 & ~((1u << 14u) | 0xffu)) | (dim << 3u),
	                                     WordAt(code, word_index + 1u) & 0x3fffffffu};
	DecodeMimg(pc, words, 0, inst);
	Restore(inst, code, word_index, 2, (word0 >> 18u) & 0x7fu);
}

} // namespace

// A gfx7 DFMT/NFMT pair as the gfx10 unified buffer format number, 0 when it has none. Formats run
// UNORM, SNORM, USCALED, SSCALED, UINT, SINT and, where it exists, FLOAT.
uint32_t Gfx7UnifiedBufferFormat(uint32_t dfmt, uint32_t nfmt) {
	constexpr uint32_t Float = 7u;
	if (nfmt > 5u && nfmt != Float) {
		return 0u;
	}
	struct Group {
		uint32_t base;
		bool     has_float;
	};
	constexpr std::array<Group, 15> Small {{
	    {0u, false},  {1u, false},  {7u, true},   {14u, false}, {0u, false},
	    {23u, true},  {30u, true},  {37u, true},  {44u, false}, {50u, false},
	    {56u, false}, {0u, false},  {65u, true},  {0u, false},  {0u, false},
	}};
	// 32-bit channels only have UINT, SINT and FLOAT.
	uint32_t wide = 0u;
	switch (dfmt) {
		case 4u: wide = 20u; break;
		case 11u: wide = 62u; break;
		case 13u: wide = 72u; break;
		case 14u: wide = 75u; break;
		default: break;
	}
	if (wide != 0u) {
		return nfmt == 4u ? wide : nfmt == 5u ? wide + 1u : nfmt == Float ? wide + 2u : 0u;
	}
	if (dfmt >= Small.size() || Small[dfmt].base == 0u || (nfmt == Float && !Small[dfmt].has_float)) {
		return 0u;
	}
	return Small[dfmt].base + (nfmt == Float ? 6u : nfmt);
}

// The gfx10 image format number of a gfx7 T#'s DFMT/NFMT.
static uint32_t Gfx7ImageFormat(uint32_t dfmt, uint32_t nfmt) {
	constexpr uint32_t Snorm = 1u, Float = 7u, Srgb = 9u;
	switch (dfmt) {
		case 1u:
			if (nfmt == Srgb) return 128u;
			break;
		case 3u:
			if (nfmt == Srgb) return 129u;
			break;
		case 10u:
			if (nfmt == Srgb) return 130u;
			break;
		case 16u: return 133u; // 5_6_5
		case 17u: return 134u; // 1_5_5_5
		case 18u: return 135u; // 5_5_5_1
		case 19u: return 136u; // 4_4_4_4
		case 34u: return 132u; // 5_9_9_9
		case 35u: return nfmt == Srgb ? 170u : 169u; // BC1
		case 36u: return nfmt == Srgb ? 172u : 171u; // BC2
		case 37u: return nfmt == Srgb ? 174u : 173u; // BC3
		case 38u: return nfmt == Snorm ? 176u : 175u; // BC4
		case 39u: return nfmt == Snorm ? 178u : 177u; // BC5
		case 40u: return nfmt == Snorm ? 180u : 179u; // BC6
		case 41u: return nfmt == Srgb ? 182u : 181u; // BC7
		default: break;
	}
	(void)Float;
	return Gfx7UnifiedBufferFormat(dfmt, nfmt);
}

void Gfx7BufferSharpToGfx10(uint32_t* fields) {
	// The base address has 44 bits; the top four of gfx10's 48 hold the gfx7 memory type.
	fields[1] &= ~0xf000u;
	const uint32_t nfmt = (fields[3] >> 12u) & 0x7u;
	const uint32_t dfmt = (fields[3] >> 15u) & 0xfu;
	fields[3] = (fields[3] & ~(0x7fu << 12u)) | (Gfx7UnifiedBufferFormat(dfmt, nfmt) << 12u);
	// gfx10's OOB_SELECT has no gfx7 counterpart, and its value 0 makes a stride-0 buffer read as
	// zeros. On gfx7 that is an ordinary byte buffer bounded by its size: mode 3, the raw one.
	fields[3] = (fields[3] & ~(3u << 28u)) | (3u << 28u);
}

void Gfx7TextureSharpToGfx10(uint32_t* fields) {
	if (fields[0] == 0u && (fields[1] & 0x3fu) == 0u) {
		return; // a null T#
	}
	const uint32_t base_low  = fields[0];
	const uint32_t base_high = fields[1] & 0x3fu;
	const uint32_t dfmt      = (fields[1] >> 20u) & 0x3fu;
	const uint32_t nfmt      = (fields[1] >> 26u) & 0xfu;
	const uint32_t width     = fields[2] & 0x3fffu;
	const uint32_t height    = (fields[2] >> 14u) & 0x3fffu;
	const uint32_t word3     = fields[3];
	const uint32_t tile      = (word3 >> 20u) & 0x1fu;
	const uint32_t type      = word3 >> 28u;
	const uint32_t last_mip  = (word3 >> 16u) & 0xfu;
	const bool     array     = type == 12u || type == 13u || type == 15u;
	// gfx7 counts whole cubes in a cube T#; gfx10 counts faces.
	const uint32_t depth     = array       ? (fields[5] >> 13u) & 0x1fffu
	                           : type == 11u ? ((fields[4] & 0x1fffu) + 1u) * 6u - 1u
	                                         : fields[4] & 0x1fffu;
	const uint32_t base_array = fields[5] & 0x1fffu;
	const uint32_t pitch      = (fields[4] >> 13u) & 0x3fffu;
	// gfx7 tiling is not decoded yet. Indices 0..7 are depth surfaces and 9..12 the displayable
	// thin modes render targets use, so sampling a target finds its image; the rest read as linear.
	const uint32_t mode = tile <= 7u ? 0x18u : ((tile >= 9u && tile <= 12u) || tile == 14u) ? 0x1bu : 0x00u;
	fields[0] = base_low;
	fields[1] = base_high | (Gfx7ImageFormat(dfmt, nfmt) << 20u) | ((width & 3u) << 30u);
	fields[2] = ((width >> 2u) & 0xfffu) | (height << 14u);
	fields[3] = (word3 & 0xfffffu) | (mode << 20u) | (type << 28u);
	fields[4] = depth | (base_array << 16u);
	fields[5] = last_mip << 4u;
	fields[6] = 0u;
	// For the texture binding: the gfx7 tile index, padded pitch and POW2_PAD.
	fields[7] = 0x80000000u | (((word3 >> 25u) & 1u) << 30u) | (pitch << 5u) | tile;
}

bool DecodeGfx7Instruction(std::span<const uint32_t> code, uint32_t word_index, Instruction& inst) {
	const uint32_t word = code[word_index];
	const uint32_t pc   = word_index * sizeof(uint32_t);
	if ((word & 0x80000000u) == 0u) {
		if (((word >> 25u) & 0x3fu) >= 0x3eu) {
			return false; // VOPC, VOP1
		}
		return DecodeVop2Gfx7(pc, code, word_index, inst);
	}
	if ((word >> 27u) == 0x18u) {
		DecodeSmrd(pc, code, word_index, inst);
		return true;
	}
	switch (word >> 26u) {
		case 0x34u: DecodeVop3Gfx7(pc, code, word_index, inst); return true;
		case 0x35u:
			inst.pc = pc;
			SetRawWords(inst, code, word_index, 1);
			SetUnsupported(inst, Family::Unknown, word >> 26u, "gfx10 VOP3 prefix in gfx7 code");
			return true;
		case 0x38u:
		case 0x3au:
			if (((word >> 15u) & 1u) != 0u) {
				inst.pc = pc;
				SetRawWords(inst, code, word_index, 2);
				SetUnsupported(inst, (word >> 26u) == 0x38u ? Family::MUBUF : Family::MTBUF,
				               (word >> 16u) & 0xffu, "gfx7 ADDR64 buffer access is not implemented");
				return true;
			}
			if ((word >> 26u) == 0x3au) {
				// The decoded fields carry the gfx10 unified format split in two.
				DecodeMtbuf(pc, code, word_index, inst);
				const auto format  = Gfx7UnifiedBufferFormat(inst.data_format, inst.number_format);
				inst.data_format   = format & 0xfu;
				inst.number_format = (format >> 4u) & 0x7u;
				return true;
			}
			return false;
		case 0x3cu: DecodeMimgGfx7(pc, code, word_index, inst); return true;
		default: return false;
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Decoder
