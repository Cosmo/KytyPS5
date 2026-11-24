// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

// gfx7 (Sea Islands, the PS4 GPU) shader decoding for PS4 backward compatibility.
//
// RDNA kept most GCN opcode numbers, so gfx7 code mostly decodes with the gfx10 tables: SOP*,
// VOP1, VOPC, DS, MUBUF, MTBUF, MIMG, VINTRP and EXP match (checked against the Sea Islands
// numbering). What differs is handled here by rewriting the instruction into its gfx10 encoding
// and decoding that, then restoring the original words:
// - SMRD (one dword, 8-bit offset) instead of SMEM;
// - VOP3 with prefix 0x34, a 9-bit opcode at bit 17 and clamp at bit 11;
// - a few VOP2 opcodes: V_CNDMASK_B32 is 0, and the lane, carry, BFM, LDEXP and packing ops that
//   gfx10 only has as VOP3;
// - MIMG without a dimension field (2D, or 2D array when DA is set);
// - MUBUF/MTBUF ADDR64, which gfx10 dropped.

#include "graphics/shader/recompiler/frontend/decode/Gfx7Ops.h"

#include "graphics/shader/recompiler/frontend/decode/ImageOps.h"
#include "graphics/shader/recompiler/frontend/decode/MemoryOps.h"
#include "graphics/shader/recompiler/frontend/decode/VectorAluOps.h"

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
	const uint32_t word0  = code[word_index];
	const uint32_t opcode = (word0 >> 17u) & 0x1ffu;
	const uint32_t gfx10  = Vop3Opcode(opcode);
	if (gfx10 == Unsupported) {
		inst.pc = pc;
		SetRawWords(inst, code, word_index, 2);
		SetUnsupported(inst, Family::VOP3, opcode, "gfx7 VOP3 opcode is not implemented");
		return;
	}
	const uint32_t low = IsVop3b(opcode) ? (word0 & 0x7fffu)
	                                     : (word0 & 0x7ffu) | (((word0 >> 11u) & 1u) << 15u);
	const std::array<uint32_t, 3> words {low | (gfx10 << 16u) | Vop3Prefix, WordAt(code, word_index + 1u),
	                                     0u};
	DecodeVop3(pc, words, 0, inst);
	Restore(inst, code, word_index, 2, opcode);
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
			return false;
		case 0x3cu: DecodeMimgGfx7(pc, code, word_index, inst); return true;
		default: return false;
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Decoder
