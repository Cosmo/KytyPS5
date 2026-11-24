// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_GFX7OPS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_GFX7OPS_H_

#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"

namespace Libs::Graphics::ShaderRecompiler::Decoder {

// The encodings where gfx7 (PS4) differs from gfx10, decoded into the same Instruction. Returns
// false for an instruction gfx7 encodes as gfx10 does: the common decoder handles it.
bool DecodeGfx7Instruction(std::span<const uint32_t> code, uint32_t word_index, Instruction& inst);

// A gfx7 DFMT/NFMT pair as the gfx10 unified buffer format number, 0 when it has none.
[[nodiscard]] uint32_t Gfx7UnifiedBufferFormat(uint32_t dfmt, uint32_t nfmt);
// Rewrites a gfx7 V# (4 dwords) or T# (8 dwords) in place into its gfx10 layout.
void Gfx7BufferSharpToGfx10(uint32_t* fields);
void Gfx7TextureSharpToGfx10(uint32_t* fields);

} // namespace Libs::Graphics::ShaderRecompiler::Decoder

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_GFX7OPS_H_ */
