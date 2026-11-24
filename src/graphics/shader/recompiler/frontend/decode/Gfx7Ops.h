// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-License-Identifier: GPL-2.0-only

#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_GFX7OPS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_GFX7OPS_H_

#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"

namespace Libs::Graphics::ShaderRecompiler::Decoder {

// The encodings where gfx7 (PS4) differs from gfx10, decoded into the same Instruction. Returns
// false for an instruction gfx7 encodes as gfx10 does: the common decoder handles it.
bool DecodeGfx7Instruction(std::span<const uint32_t> code, uint32_t word_index, Instruction& inst);

} // namespace Libs::Graphics::ShaderRecompiler::Decoder

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_GFX7OPS_H_ */
