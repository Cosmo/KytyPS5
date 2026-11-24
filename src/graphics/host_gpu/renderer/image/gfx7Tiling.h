// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// PS4 (gfx7, backward compatibility) surface tiling: the tile modes of the PS4's GB_TILE_MODE table,
// their array, micro and macro tile modes, and the guest layout of a surface (pitch, height, size and
// offset of each mip level). Ported from shadPS4 (src/video_core/amdgpu/tiling.h, tiling.cpp and
// src/video_core/texture_cache/tile.h at main, 2026-11-25); adapted to this code base's types and
// assertions, nothing else changed.

#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_GFX7TILING_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_GFX7TILING_H_

#include "common/assert.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <utility>

namespace Libs::Graphics::Gfx7 {



static constexpr size_t NUM_TILE_MODES = 32;

enum class PipeConfig : uint32_t {
    P2 = 0,
    P4_8x16 = 4,
    P4_16x16 = 5,
    P4_16x32 = 6,
    P4_32x32 = 7,
    P8_16x16_8x16 = 8,
    P8_16x32_8x16 = 9,
    P8_32x32_8x16 = 10,
    P8_16x32_16x16 = 11,
    P8_32x32_16x16 = 12,
    P8_32x32_16x32 = 13,
    P8_32x64_32x32 = 14,
    P16_32x32_8x16 = 16,
    P16_32x32_16x16 = 17,
    P16 = 18,
};

enum class MicroTileMode : uint32_t {
    Display = 0,
    Thin = 1,
    Depth = 2,
    Rotated = 3,
    Thick = 4,
};

enum class MacroTileMode : uint32_t {
    Mode_1x4_16 = 0,
    Mode_1x2_16 = 1,
    Mode_1x1_16 = 2,
    Mode_1x1_16_Dup = 3,
    Mode_1x1_8 = 4,
    Mode_1x1_4 = 5,
    Mode_1x1_2 = 6,
    Mode_1x1_2_Dup = 7,
    Mode_1x8_16 = 8,
    Mode_1x4_16_Dup = 9,
    Mode_1x2_16_Dup = 10,
    Mode_1x1_16_Dup2 = 11,
    Mode_1x1_8_Dup = 12,
    Mode_1x1_4_Dup = 13,
    Mode_1x1_2_Dup2 = 14,
    Mode_1x1_2_Dup3 = 15,
};

enum class ArrayMode : uint32_t {
    ArrayLinearGeneral = 0,
    ArrayLinearAligned = 1,
    Array1DTiledThin1 = 2,
    Array1DTiledThick = 3,
    Array2DTiledThin1 = 4,
    ArrayPrtTiledThin1 = 5,
    ArrayPrt2DTiledThin1 = 6,
    Array2DTiledThick = 7,
    Array2DTiledXThick = 8,
    ArrayPrtTiledThick = 9,
    ArrayPrt2DTiledThick = 10,
    ArrayPrt3DTiledThin1 = 11,
    Array3DTiledThin1 = 12,
    Array3DTiledThick = 13,
    Array3DTiledXThick = 14,
    ArrayPrt3DTiledThick = 15,
};

enum class TileMode : uint32_t {
    Depth2DThin64 = 0,
    Depth2DThin128 = 1,
    Depth2DThin256 = 2,
    Depth2DThin512 = 3,
    Depth2DThin1K = 4,
    Depth1DThin = 5,
    Depth2DThinPrt256 = 6,
    Depth2DThinPrt1K = 7,
    DisplayLinearAligned = 8,
    Display1DThin = 9,
    Display2DThin = 10,
    DisplayThinPrt = 11,
    Display2DThinPrt = 12,
    Thin1DThin = 13,
    Thin2DThin = 14,
    Thin3DThin = 15,
    ThinThinPrt = 16,
    Thin2DThinPrt = 17,
    Thin3DThinPrt = 18,
    Thick1DThick = 19,
    Thick2DThick = 20,
    Thick3DThick = 21,
    ThickThickPrt = 22,
    Thick2DThickPrt = 23,
    Thick3DThickPrt = 24,
    Thick2DXThick = 25,
    Thick3DXThick = 26,
    DisplayLinearGeneral = 31,
};

constexpr bool IsMacroTiled(ArrayMode array_mode) {
    switch (array_mode) {
    case ArrayMode::ArrayLinearGeneral:
    case ArrayMode::ArrayLinearAligned:
    case ArrayMode::Array1DTiledThin1:
    case ArrayMode::Array1DTiledThick:
        return false;
    case ArrayMode::Array2DTiledThin1:
    case ArrayMode::ArrayPrtTiledThin1:
    case ArrayMode::ArrayPrt2DTiledThin1:
    case ArrayMode::Array2DTiledThick:
    case ArrayMode::Array2DTiledXThick:
    case ArrayMode::ArrayPrtTiledThick:
    case ArrayMode::ArrayPrt2DTiledThick:
    case ArrayMode::ArrayPrt3DTiledThin1:
    case ArrayMode::Array3DTiledThin1:
    case ArrayMode::Array3DTiledThick:
    case ArrayMode::Array3DTiledXThick:
    case ArrayMode::ArrayPrt3DTiledThick:
        return true;
    default:
        EXIT("gfx7 tiling: unknown array mode %u\n", static_cast<uint32_t>(array_mode));
    }
}

constexpr uint32_t GetMicroTileThickness(ArrayMode array_mode) {
    switch (array_mode) {
    case ArrayMode::ArrayLinearGeneral:
    case ArrayMode::ArrayLinearAligned:
    case ArrayMode::Array1DTiledThin1:
    case ArrayMode::Array2DTiledThin1:
    case ArrayMode::ArrayPrtTiledThin1:
    case ArrayMode::ArrayPrt2DTiledThin1:
    case ArrayMode::ArrayPrt3DTiledThin1:
    case ArrayMode::Array3DTiledThin1:
        return 1;
    case ArrayMode::Array1DTiledThick:
    case ArrayMode::Array2DTiledThick:
    case ArrayMode::Array3DTiledThick:
    case ArrayMode::ArrayPrtTiledThick:
    case ArrayMode::ArrayPrt2DTiledThick:
    case ArrayMode::ArrayPrt3DTiledThick:
        return 4;
    case ArrayMode::Array2DTiledXThick:
    case ArrayMode::Array3DTiledXThick:
        return 8;
    default:
        EXIT("gfx7 tiling: unknown array mode %u\n", static_cast<uint32_t>(array_mode));
    }
}

constexpr uint32_t GetAltNumBanks(MacroTileMode mode) {
    switch (mode) {
    case MacroTileMode::Mode_1x1_2_Dup:
    case MacroTileMode::Mode_1x1_2_Dup2:
    case MacroTileMode::Mode_1x1_2_Dup3:
        return 2;
    case MacroTileMode::Mode_1x1_2:
    case MacroTileMode::Mode_1x1_8_Dup:
    case MacroTileMode::Mode_1x1_4_Dup:
        return 4;
    case MacroTileMode::Mode_1x4_16:
    case MacroTileMode::Mode_1x2_16:
    case MacroTileMode::Mode_1x1_16:
    case MacroTileMode::Mode_1x1_16_Dup:
    case MacroTileMode::Mode_1x1_8:
    case MacroTileMode::Mode_1x1_4:
    case MacroTileMode::Mode_1x4_16_Dup:
    case MacroTileMode::Mode_1x2_16_Dup:
    case MacroTileMode::Mode_1x1_16_Dup2:
        return 8;
    case MacroTileMode::Mode_1x8_16:
        return 16;
    default:
        EXIT("gfx7 tiling: unknown macro tile mode %u\n", static_cast<uint32_t>(mode));
    }
}

constexpr ArrayMode GetArrayMode(TileMode tile_mode) {
    switch (tile_mode) {
    case TileMode::Depth1DThin:
    case TileMode::Display1DThin:
    case TileMode::Thin1DThin:
        return ArrayMode::Array1DTiledThin1;
    case TileMode::Depth2DThin64:
    case TileMode::Depth2DThin128:
    case TileMode::Depth2DThin256:
    case TileMode::Depth2DThin512:
    case TileMode::Depth2DThin1K:
    case TileMode::Display2DThin:
    case TileMode::Thin2DThin:
        return ArrayMode::Array2DTiledThin1;
    case TileMode::DisplayThinPrt:
    case TileMode::ThinThinPrt:
        return ArrayMode::ArrayPrtTiledThin1;
    case TileMode::Depth2DThinPrt256:
    case TileMode::Depth2DThinPrt1K:
    case TileMode::Display2DThinPrt:
    case TileMode::Thin2DThinPrt:
        return ArrayMode::ArrayPrt2DTiledThin1;
    case TileMode::Thin3DThin:
        return ArrayMode::Array3DTiledThin1;
    case TileMode::Thin3DThinPrt:
        return ArrayMode::ArrayPrt3DTiledThin1;
    case TileMode::Thick1DThick:
        return ArrayMode::Array1DTiledThick;
    case TileMode::Thick2DThick:
        return ArrayMode::Array2DTiledThick;
    case TileMode::Thick3DThick:
        return ArrayMode::Array3DTiledThick;
    case TileMode::ThickThickPrt:
        return ArrayMode::ArrayPrtTiledThick;
    case TileMode::Thick2DThickPrt:
        return ArrayMode::ArrayPrt2DTiledThick;
    case TileMode::Thick3DThickPrt:
        return ArrayMode::ArrayPrt3DTiledThick;
    case TileMode::Thick2DXThick:
        return ArrayMode::Array2DTiledXThick;
    case TileMode::Thick3DXThick:
        return ArrayMode::Array3DTiledXThick;
    case TileMode::DisplayLinearAligned:
        return ArrayMode::ArrayLinearAligned;
    case TileMode::DisplayLinearGeneral:
        return ArrayMode::ArrayLinearGeneral;
    default:
        EXIT("gfx7 tiling: unknown tile mode %u\n", static_cast<uint32_t>(tile_mode));
    }
}

constexpr MicroTileMode GetMicroTileMode(TileMode tile_mode) {
    switch (tile_mode) {
    case TileMode::Depth2DThin64:
    case TileMode::Depth2DThin128:
    case TileMode::Depth2DThin256:
    case TileMode::Depth2DThin512:
    case TileMode::Depth2DThin1K:
    case TileMode::Depth1DThin:
    case TileMode::Depth2DThinPrt256:
    case TileMode::Depth2DThinPrt1K:
        return MicroTileMode::Depth;
    case TileMode::DisplayLinearAligned:
    case TileMode::Display1DThin:
    case TileMode::Display2DThin:
    case TileMode::DisplayThinPrt:
    case TileMode::Display2DThinPrt:
    case TileMode::DisplayLinearGeneral:
        return MicroTileMode::Display;
    case TileMode::Thin1DThin:
    case TileMode::Thin2DThin:
    case TileMode::Thin3DThin:
    case TileMode::ThinThinPrt:
    case TileMode::Thin2DThinPrt:
    case TileMode::Thin3DThinPrt:
        return MicroTileMode::Thin;
    case TileMode::Thick1DThick:
    case TileMode::Thick2DThick:
    case TileMode::Thick3DThick:
    case TileMode::ThickThickPrt:
    case TileMode::Thick2DThickPrt:
    case TileMode::Thick3DThickPrt:
    case TileMode::Thick2DXThick:
    case TileMode::Thick3DXThick:
        return MicroTileMode::Thick;
    default:
        EXIT("gfx7 tiling: unknown tile mode %u\n", static_cast<uint32_t>(tile_mode));
    }
}

















inline constexpr uint32_t MICROTILE_SIZE = 8;
inline constexpr uint32_t DRAM_ROW_SIZE = 1024;


inline PipeConfig GetPipeConfig(TileMode tile_mode) {
    switch (tile_mode) {
    case TileMode::Depth2DThin64:
    case TileMode::Depth2DThin128:
    case TileMode::Depth2DThin256:
    case TileMode::Depth2DThin512:
    case TileMode::Depth2DThin1K:
    case TileMode::Depth1DThin:
    case TileMode::Depth2DThinPrt256:
    case TileMode::Depth2DThinPrt1K:
    case TileMode::DisplayLinearAligned:
    case TileMode::Display1DThin:
    case TileMode::Display2DThin:
    case TileMode::Display2DThinPrt:
    case TileMode::Thin1DThin:
    case TileMode::Thin2DThin:
    case TileMode::Thin2DThinPrt:
    case TileMode::Thin3DThinPrt:
    case TileMode::Thick1DThick:
    case TileMode::Thick2DThick:
    case TileMode::Thick2DThickPrt:
    case TileMode::Thick2DXThick:
        return PipeConfig::P8_32x32_16x16;
    case TileMode::DisplayThinPrt:
    case TileMode::Thin3DThin:
    case TileMode::ThinThinPrt:
    case TileMode::Thick3DThick:
    case TileMode::ThickThickPrt:
    case TileMode::Thick3DThickPrt:
    case TileMode::Thick3DXThick:
        return PipeConfig::P8_32x32_8x16;
    case TileMode::DisplayLinearGeneral:
        return PipeConfig::P2;
    default:
        EXIT("gfx7 tiling: unknown tile mode %u\n", static_cast<uint32_t>(tile_mode));
    }
}

inline PipeConfig GetAltPipeConfig(TileMode tile_mode) {
    switch (tile_mode) {
    case TileMode::Depth2DThin64:
    case TileMode::Depth2DThin128:
    case TileMode::Depth2DThin256:
    case TileMode::Depth2DThin512:
    case TileMode::Depth2DThin1K:
    case TileMode::Depth1DThin:
    case TileMode::Depth2DThinPrt256:
    case TileMode::Depth2DThinPrt1K:
    case TileMode::DisplayLinearAligned:
    case TileMode::Display1DThin:
    case TileMode::Display2DThin:
    case TileMode::DisplayThinPrt:
    case TileMode::Display2DThinPrt:
    case TileMode::Thin1DThin:
    case TileMode::Thin2DThin:
    case TileMode::Thin3DThin:
    case TileMode::ThinThinPrt:
    case TileMode::Thin2DThinPrt:
    case TileMode::Thin3DThinPrt:
    case TileMode::Thick1DThick:
    case TileMode::Thick2DThick:
    case TileMode::Thick3DThick:
    case TileMode::ThickThickPrt:
    case TileMode::Thick2DThickPrt:
    case TileMode::Thick3DThickPrt:
    case TileMode::Thick2DXThick:
    case TileMode::Thick3DXThick:
        return PipeConfig::P16_32x32_8x16;
    case TileMode::DisplayLinearGeneral:
        return PipeConfig::P2;
    default:
        EXIT("gfx7 tiling: unknown tile mode %u\n", static_cast<uint32_t>(tile_mode));
    }
}

inline uint32_t GetSampleSplit(TileMode tile_mode) {
    switch (tile_mode) {
    case TileMode::Depth2DThin64:
    case TileMode::Depth2DThin128:
    case TileMode::Depth2DThin256:
    case TileMode::Depth2DThin512:
    case TileMode::Depth2DThin1K:
    case TileMode::Depth1DThin:
    case TileMode::Depth2DThinPrt256:
    case TileMode::Depth2DThinPrt1K:
    case TileMode::DisplayLinearAligned:
    case TileMode::Display1DThin:
    case TileMode::Thin1DThin:
    case TileMode::Thick1DThick:
    case TileMode::Thick2DThick:
    case TileMode::Thick3DThick:
    case TileMode::ThickThickPrt:
    case TileMode::Thick2DThickPrt:
    case TileMode::Thick3DThickPrt:
    case TileMode::Thick2DXThick:
    case TileMode::Thick3DXThick:
    case TileMode::DisplayLinearGeneral:
        return 1;
    case TileMode::Display2DThin:
    case TileMode::DisplayThinPrt:
    case TileMode::Display2DThinPrt:
    case TileMode::Thin2DThin:
    case TileMode::Thin3DThin:
    case TileMode::ThinThinPrt:
    case TileMode::Thin2DThinPrt:
    case TileMode::Thin3DThinPrt:
        return 2;
    default:
        EXIT("gfx7 tiling: unknown tile mode %u\n", static_cast<uint32_t>(tile_mode));
    }
}

inline uint32_t GetTileSplitHw(TileMode tile_mode) {
    switch (tile_mode) {
    case TileMode::Depth2DThin64:
    case TileMode::Depth1DThin:
    case TileMode::DisplayLinearAligned:
    case TileMode::Display1DThin:
    case TileMode::Display2DThin:
    case TileMode::DisplayThinPrt:
    case TileMode::Display2DThinPrt:
    case TileMode::Thin1DThin:
    case TileMode::Thin2DThin:
    case TileMode::Thin3DThin:
    case TileMode::ThinThinPrt:
    case TileMode::Thin2DThinPrt:
    case TileMode::Thin3DThinPrt:
    case TileMode::Thick1DThick:
    case TileMode::Thick2DThick:
    case TileMode::Thick3DThick:
    case TileMode::ThickThickPrt:
    case TileMode::Thick2DThickPrt:
    case TileMode::Thick3DThickPrt:
    case TileMode::Thick2DXThick:
    case TileMode::Thick3DXThick:
    case TileMode::DisplayLinearGeneral:
        return 64;
    case TileMode::Depth2DThin128:
        return 128;
    case TileMode::Depth2DThin256:
    case TileMode::Depth2DThinPrt256:
        return 256;
    case TileMode::Depth2DThin512:
        return 512;
    case TileMode::Depth2DThin1K:
    case TileMode::Depth2DThinPrt1K:
        return 1024;
    default:
        EXIT("gfx7 tiling: unknown tile mode %u\n", static_cast<uint32_t>(tile_mode));
    }
}

inline uint32_t GetBankWidth(MacroTileMode mode) {
    switch (mode) {
    case MacroTileMode::Mode_1x4_16:
    case MacroTileMode::Mode_1x2_16:
    case MacroTileMode::Mode_1x1_16:
    case MacroTileMode::Mode_1x1_16_Dup:
    case MacroTileMode::Mode_1x1_8:
    case MacroTileMode::Mode_1x1_4:
    case MacroTileMode::Mode_1x1_2:
    case MacroTileMode::Mode_1x1_2_Dup:
    case MacroTileMode::Mode_1x8_16:
    case MacroTileMode::Mode_1x4_16_Dup:
    case MacroTileMode::Mode_1x2_16_Dup:
    case MacroTileMode::Mode_1x1_16_Dup2:
    case MacroTileMode::Mode_1x1_8_Dup:
    case MacroTileMode::Mode_1x1_4_Dup:
    case MacroTileMode::Mode_1x1_2_Dup2:
    case MacroTileMode::Mode_1x1_2_Dup3:
        return 1;
    default:
        EXIT("gfx7 tiling: unknown macro tile mode %u\n", static_cast<uint32_t>(mode));
    }
}

inline uint32_t GetBankHeight(MacroTileMode mode) {
    switch (mode) {
    case MacroTileMode::Mode_1x1_16:
    case MacroTileMode::Mode_1x1_16_Dup:
    case MacroTileMode::Mode_1x1_8:
    case MacroTileMode::Mode_1x1_4:
    case MacroTileMode::Mode_1x1_2:
    case MacroTileMode::Mode_1x1_2_Dup:
    case MacroTileMode::Mode_1x1_16_Dup2:
    case MacroTileMode::Mode_1x1_8_Dup:
    case MacroTileMode::Mode_1x1_4_Dup:
    case MacroTileMode::Mode_1x1_2_Dup2:
    case MacroTileMode::Mode_1x1_2_Dup3:
        return 1;
    case MacroTileMode::Mode_1x2_16:
    case MacroTileMode::Mode_1x2_16_Dup:
        return 2;
    case MacroTileMode::Mode_1x4_16:
    case MacroTileMode::Mode_1x4_16_Dup:
        return 4;
    case MacroTileMode::Mode_1x8_16:
        return 8;
    default:
        EXIT("gfx7 tiling: unknown macro tile mode %u\n", static_cast<uint32_t>(mode));
    }
}

inline uint32_t GetNumBanks(MacroTileMode mode) {
    switch (mode) {
    case MacroTileMode::Mode_1x1_2:
    case MacroTileMode::Mode_1x1_2_Dup:
    case MacroTileMode::Mode_1x1_2_Dup2:
    case MacroTileMode::Mode_1x1_2_Dup3:
        return 2;
    case MacroTileMode::Mode_1x1_4:
    case MacroTileMode::Mode_1x1_4_Dup:
        return 4;
    case MacroTileMode::Mode_1x1_8:
    case MacroTileMode::Mode_1x1_8_Dup:
        return 8;
    case MacroTileMode::Mode_1x4_16:
    case MacroTileMode::Mode_1x2_16:
    case MacroTileMode::Mode_1x1_16:
    case MacroTileMode::Mode_1x1_16_Dup:
    case MacroTileMode::Mode_1x8_16:
    case MacroTileMode::Mode_1x4_16_Dup:
    case MacroTileMode::Mode_1x2_16_Dup:
    case MacroTileMode::Mode_1x1_16_Dup2:
        return 16;
    default:
        EXIT("gfx7 tiling: unknown macro tile mode %u\n", static_cast<uint32_t>(mode));
    }
}

inline uint32_t GetMacrotileAspect(MacroTileMode mode) {
    switch (mode) {
    case MacroTileMode::Mode_1x1_8:
    case MacroTileMode::Mode_1x1_4:
    case MacroTileMode::Mode_1x1_2:
    case MacroTileMode::Mode_1x1_2_Dup:
    case MacroTileMode::Mode_1x1_8_Dup:
    case MacroTileMode::Mode_1x1_4_Dup:
    case MacroTileMode::Mode_1x1_2_Dup2:
    case MacroTileMode::Mode_1x1_2_Dup3:
        return 1;
    case MacroTileMode::Mode_1x2_16:
    case MacroTileMode::Mode_1x1_16:
    case MacroTileMode::Mode_1x1_16_Dup:
    case MacroTileMode::Mode_1x2_16_Dup:
    case MacroTileMode::Mode_1x1_16_Dup2:
        return 2;
    case MacroTileMode::Mode_1x4_16:
    case MacroTileMode::Mode_1x8_16:
    case MacroTileMode::Mode_1x4_16_Dup:
        return 4;
    default:
        EXIT("gfx7 tiling: unknown macro tile mode %u\n", static_cast<uint32_t>(mode));
    }
}

inline uint32_t GetAltBankHeight(MacroTileMode mode) {
    switch (mode) {
    case MacroTileMode::Mode_1x1_8:
    case MacroTileMode::Mode_1x1_4:
    case MacroTileMode::Mode_1x1_2:
    case MacroTileMode::Mode_1x1_2_Dup:
    case MacroTileMode::Mode_1x1_16_Dup2:
    case MacroTileMode::Mode_1x1_8_Dup:
    case MacroTileMode::Mode_1x1_4_Dup:
    case MacroTileMode::Mode_1x1_2_Dup2:
    case MacroTileMode::Mode_1x1_2_Dup3:
        return 1;
    case MacroTileMode::Mode_1x1_16:
    case MacroTileMode::Mode_1x1_16_Dup:
    case MacroTileMode::Mode_1x2_16_Dup:
        return 2;
    case MacroTileMode::Mode_1x4_16:
    case MacroTileMode::Mode_1x2_16:
    case MacroTileMode::Mode_1x8_16:
    case MacroTileMode::Mode_1x4_16_Dup:
        return 4;
    default:
        EXIT("gfx7 tiling: unknown macro tile mode %u\n", static_cast<uint32_t>(mode));
    }
}

inline uint32_t GetAltMacrotileAspect(MacroTileMode mode) {
    switch (mode) {
    case MacroTileMode::Mode_1x1_16:
    case MacroTileMode::Mode_1x1_16_Dup:
    case MacroTileMode::Mode_1x1_8:
    case MacroTileMode::Mode_1x1_4:
    case MacroTileMode::Mode_1x1_2:
    case MacroTileMode::Mode_1x1_2_Dup:
    case MacroTileMode::Mode_1x2_16_Dup:
    case MacroTileMode::Mode_1x1_16_Dup2:
    case MacroTileMode::Mode_1x1_8_Dup:
    case MacroTileMode::Mode_1x1_4_Dup:
    case MacroTileMode::Mode_1x1_2_Dup2:
    case MacroTileMode::Mode_1x1_2_Dup3:
        return 1;
    case MacroTileMode::Mode_1x4_16:
    case MacroTileMode::Mode_1x2_16:
    case MacroTileMode::Mode_1x8_16:
    case MacroTileMode::Mode_1x4_16_Dup:
        return 2;
    default:
        EXIT("gfx7 tiling: unknown macro tile mode %u\n", static_cast<uint32_t>(mode));
    }
}

inline bool IsPrt(ArrayMode array_mode) {
    switch (array_mode) {
    case ArrayMode::ArrayPrtTiledThin1:
    case ArrayMode::ArrayPrtTiledThick:
    case ArrayMode::ArrayPrt2DTiledThin1:
    case ArrayMode::ArrayPrt2DTiledThick:
    case ArrayMode::ArrayPrt3DTiledThin1:
    case ArrayMode::ArrayPrt3DTiledThick:
        return true;
    case ArrayMode::ArrayLinearGeneral:
    case ArrayMode::ArrayLinearAligned:
    case ArrayMode::Array1DTiledThin1:
    case ArrayMode::Array1DTiledThick:
    case ArrayMode::Array2DTiledThin1:
    case ArrayMode::Array2DTiledThick:
    case ArrayMode::Array2DTiledXThick:
    case ArrayMode::Array3DTiledThin1:
    case ArrayMode::Array3DTiledThick:
    case ArrayMode::Array3DTiledXThick:
        return false;
    default:
        EXIT("gfx7 tiling: unknown array mode %u\n", static_cast<uint32_t>(array_mode));
    }
}

inline uint32_t GetPipeCount(PipeConfig pipe_cfg) {
    switch (pipe_cfg) {
    case PipeConfig::P2:
        return 2;
    case PipeConfig::P8_32x32_8x16:
    case PipeConfig::P8_32x32_16x16:
        return 8;
    case PipeConfig::P16_32x32_8x16:
        return 16;
    default:
        EXIT("gfx7 tiling: unknown pipe config %u\n", static_cast<uint32_t>(pipe_cfg));
    }
}

inline uint32_t CalculateTileSplit(TileMode tile_mode, ArrayMode array_mode, MicroTileMode micro_tile_mode,
                       uint32_t bpp) {
    const uint32_t sample_split = GetSampleSplit(tile_mode);
    const uint32_t tile_split_hw = GetTileSplitHw(tile_mode);
    const uint32_t tile_thickness = GetMicroTileThickness(array_mode);
    const uint32_t tile_bytes_1x = (bpp * MICROTILE_SIZE * MICROTILE_SIZE * tile_thickness + 7) / 8;
    const uint32_t color_tile_split = std::max(256U, sample_split * tile_bytes_1x);
    const uint32_t tile_split =
        micro_tile_mode == MicroTileMode::Depth ? tile_split_hw : color_tile_split;
    return std::min(DRAM_ROW_SIZE, tile_split);
}

inline MacroTileMode CalculateMacrotileMode(TileMode tile_mode, uint32_t bpp, uint32_t num_samples) {
    EXIT_IF(!(std::has_single_bit(num_samples) && num_samples <= 16));
    EXIT_IF(!(bpp >= 1 && bpp <= 128));

    const ArrayMode array_mode = GetArrayMode(tile_mode);
    EXIT_IF(!(IsMacroTiled(array_mode)));

    const MicroTileMode micro_tile_mode = GetMicroTileMode(tile_mode);
    const uint32_t tile_thickness = GetMicroTileThickness(array_mode);
    const uint32_t tile_bytes_1x = bpp * MICROTILE_SIZE * MICROTILE_SIZE * tile_thickness / 8;
    const uint32_t tilesplic = CalculateTileSplit(tile_mode, array_mode, micro_tile_mode, bpp);
    const uint32_t tile_bytes = std::min(tilesplic, num_samples * tile_bytes_1x);
    const uint32_t mtm_idx = std::bit_width(tile_bytes / 64) - 1;
    return IsPrt(array_mode) ? MacroTileMode(mtm_idx + 8) : MacroTileMode(mtm_idx);
}



// clang-format off
// The table of macro tiles parameters for given tiling index (row) and bpp (column)
/* Calculation:
 * - Inputs:
 *     TileMode, BytesPerPixel, NumFragments
 * - Constants:
 *     MicroTileWidth = 8, MicroTileHeight = 8,
 *     Tile Mode LUTs: IsDepth(), IsPrt(), TileThickness(), TileSplit(), SampleSplit(), NumPipes()
 *     Macro Tile Mode LUTs: BankWidth(), BankHeight(), NumBanks(), MacroTileAspect()
 * - Determine the macro tile mode:
 *     TileBytes = MicroTileWidth * MicroTileHeight * TileThickness(TileMode) * BytesPerPixel
 *     TileSplit = min(IsDepth(TileMode) ? TileSplit(TileMode) : max(TileBytes * SampleSplit(TileMode), 256), NumFragments * TileBytes, 1024)
 *     MacroTileModeIndex = log2(TileSplit / 64)
 *     MacroTileMode = IsPrt(TileMode) ? MacroTileModeIndex + 8 : MacroTileModeIndex
 * - Calculate macro tile width and height:
 *     Width = NumPipes(TileMode) * BankWidth(MacroTileMode) * MicroTileWidth * MacroTileAspect(MacroTileMode, AltTileMode)
 *     Height = NumBanks(MacroTileMode, AltTileMode) * BankHeight(MacroTileMode, AltTileMode) * MicroTileHeight / MacroTileAspect(MacroTileMode, AltTileMode)
 */

constexpr std::array macro_tile_extents_x1{
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, // 00
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 01
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 02
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 03
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   // 04
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 05
    std::pair{256u, 256u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 06
    std::pair{256u, 256u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   // 07
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 08
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 09
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   // 0A
    std::pair{256u, 256u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   // 0B
    std::pair{256u, 256u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   // 0C
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 0D
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   // 0E
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   // 0F
    std::pair{256u, 256u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   // 10
    std::pair{256u, 256u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   // 11
    std::pair{256u, 256u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   // 12
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 13
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 14
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 15
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 16
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 17
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 18
    std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 19
    std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 1A
};

constexpr std::array macro_tile_extents_x2{
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, // 00
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 01
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 02
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 03
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 04
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 05
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 06
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 07
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 08
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 09
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0A
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0B
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0C
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 0D
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0E
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0F
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 10
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 11
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 12
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 13
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 14
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 15
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 16
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 17
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 18
    std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 19
    std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 1A
};

constexpr std::array macro_tile_extents_x4{
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, // 00
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 01
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 02
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 03
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 04
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 05
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 06
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 07
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 08
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 09
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0A
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0B
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0C
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 0D
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0E
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0F
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 10
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 11
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 12
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 13
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 14
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 15
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 16
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 17
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 18
    std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 19
    std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 1A
};

constexpr std::array macro_tile_extents_x8{
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, // 00
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 01
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 02
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 03
    std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 04
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 05
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 06
    std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 07
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 08
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 09
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0A
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0B
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0C
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 0D
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0E
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 0F
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 10
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 11
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   // 12
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 13
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 14
    std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 15
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 16
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 17
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 18
    std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 19
    std::pair{128u, 64u},  std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   std::pair{64u, 64u},   // 1A
};

constexpr std::array macro_tile_extents_alt_x1{
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, // 00
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, // 01
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 02
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 03
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  // 04
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 05
    std::pair{256u, 256u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 06
    std::pair{256u, 256u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  // 07
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 08
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 09
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  // 0A
    std::pair{256u, 256u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  // 0B
    std::pair{256u, 256u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  // 0C
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 0D
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  // 0E
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  // 0F
    std::pair{256u, 256u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  // 10
    std::pair{256u, 256u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  // 11
    std::pair{256u, 256u}, std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  // 12
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 13
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 14
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 15
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 16
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 17
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 18
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 19
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 1A
};

constexpr std::array macro_tile_extents_alt_x2{
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, // 00
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, // 01
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 02
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 03
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  // 04
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 05
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 06
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 07
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 08
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 09
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  // 0A
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 0B
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 0C
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 0D
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  // 0E
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  // 0F
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 10
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 11
    std::pair{256u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 12
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 13
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 14
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 15
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 16
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 17
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 18
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 19
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 1A
};

constexpr std::array macro_tile_extents_alt_x4{
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, // 00
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, // 01
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 02
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 03
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 04
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 05
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 06
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 07
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 08
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 09
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  // 0A
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 0B
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 0C
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 0D
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  // 0E
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  // 0F
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 10
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 11
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 12
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 13
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 14
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 15
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 16
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 17
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 18
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 19
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 1A
};

constexpr std::array macro_tile_extents_alt_x8{
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, // 00
    std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, std::pair{256u, 128u}, // 01
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 02
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 03
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 04
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 05
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, // 06
    std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 07
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 08
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 09
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  // 0A
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 0B
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 0C
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 0D
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  // 0E
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  // 0F
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 10
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 11
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 12
    std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     std::pair{0u, 0u},     // 13
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 14
    std::pair{128u, 128u}, std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 15
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 16
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 17
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 32u},  std::pair{128u, 32u},  std::pair{128u, 32u},  // 18
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 19
    std::pair{128u, 128u}, std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  std::pair{128u, 64u},  // 1A
};

constexpr std::array macro_tile_extents{
    macro_tile_extents_x1,
    macro_tile_extents_x2,
    macro_tile_extents_x4,
    macro_tile_extents_x8,
};

constexpr std::array macro_tile_extents_alt{
    macro_tile_extents_alt_x1,
    macro_tile_extents_alt_x2,
    macro_tile_extents_alt_x4,
    macro_tile_extents_alt_x8,
};
// clang-format on

constexpr std::pair micro_tile_extent{8u, 8u};
constexpr auto hw_pipe_interleave = 256u;

constexpr std::pair<uint32_t, uint32_t> GetMacroTileExtents(TileMode tile_mode, uint32_t bpp,
                                                  uint32_t num_samples, bool alt) {
    EXIT_IF(!(num_samples > 0 && num_samples <= 8));
    const auto samples_log = std::bit_width(num_samples) - 1;
    const auto row = uint32_t(tile_mode) * 5;
    const auto column = std::bit_width(bpp) - 4; // bpps are 8, 16, 32, 64, 128
    return (alt ? macro_tile_extents_alt : macro_tile_extents)[samples_log][row + column];
}

constexpr std::tuple<uint16_t, uint16_t, size_t> ImageSizeLinearAligned(uint32_t pitch, uint32_t height, uint32_t bpp,
                                                              uint32_t num_samples) {
    const auto pitch_align = std::max(8u, 64u / ((bpp + 7) / 8));
    auto pitch_aligned = (pitch + pitch_align - 1) & ~(pitch_align - 1);
    const auto height_aligned = height;
    size_t log_sz = pitch_aligned * height_aligned * num_samples;
    const auto slice_align = std::max(64u, 256u / ((bpp + 7) / 8));
    while (log_sz % slice_align) {
        pitch_aligned += pitch_align;
        log_sz = pitch_aligned * height_aligned * num_samples;
    }
    return {pitch_aligned, height_aligned, (log_sz * bpp + 7) / 8};
}

constexpr std::tuple<uint16_t, uint16_t, size_t> ImageSizeMicroTiled(uint32_t pitch, uint32_t height, uint32_t thickness,
                                                           uint32_t bpp, uint32_t num_samples) {
    constexpr auto pitch_align = micro_tile_extent.first;
    constexpr auto height_align = micro_tile_extent.second;
    auto pitch_aligned = (pitch + pitch_align - 1) & ~(pitch_align - 1);
    const auto height_aligned = (height + height_align - 1) & ~(height_align - 1);
    size_t log_sz = (pitch_aligned * height_aligned * bpp * num_samples + 7) / 8;
    while ((log_sz * thickness) % 256) {
        pitch_aligned += pitch_align;
        log_sz = (pitch_aligned * height_aligned * bpp * num_samples + 7) / 8;
    }
    return {pitch_aligned, height_aligned, log_sz};
}

constexpr bool IsMacroTiledMip(uint32_t pitch, uint32_t height, uint32_t bpp, uint32_t num_samples,
                               TileMode tile_mode, uint32_t mip_n, bool alt) {
    if (mip_n == 0) {
        return true;
    }
    const auto [pitch_align, height_align] = GetMacroTileExtents(tile_mode, bpp, num_samples, alt);
    return pitch >= pitch_align && height >= height_align;
}

constexpr std::tuple<uint16_t, uint16_t, size_t> ImageSizeMacroTiled(uint32_t pitch, uint32_t height, uint32_t bpp,
                                                           uint32_t num_samples,
                                                           TileMode tile_mode, bool alt) {
    const auto [pitch_align, height_align] = GetMacroTileExtents(tile_mode, bpp, num_samples, alt);
    EXIT_IF(!(pitch_align != 0 && height_align != 0));
    const auto pitch_aligned = (pitch + pitch_align - 1) & ~(pitch_align - 1);
    const auto height_aligned = (height + height_align - 1) & ~(height_align - 1);
    const auto log_sz = pitch_aligned * height_aligned * num_samples;
    return {pitch_aligned, height_aligned, (log_sz * bpp + 7) / 8};
}

struct MipInfo {
    uint32_t size;
    uint16_t pitch;
    uint16_t height;
    uint32_t offset;
};

static constexpr std::size_t MAX_MIPS = 16;

constexpr void ComputeImageSize(uint32_t pitch, uint32_t height, uint32_t depth, uint32_t levels, uint32_t layers,
                                uint32_t num_bits, uint32_t num_samples, bool is_block, bool is_pow2,
                                bool alt_tile, TileMode tile_mode,
                                ArrayMode array_mode, uint32_t* out_size,
                                uint32_t* out_micro_tiled_mips, MipInfo* out_mips_layout) {
    const uint32_t thickness = GetMicroTileThickness(array_mode);
    const bool macro = IsMacroTiled(array_mode);
    *out_size = 0;
    *out_micro_tiled_mips = 0;
    for (int32_t mip = 0; mip < levels; ++mip) {
        uint32_t mip_w = pitch >> mip;
        uint32_t mip_h = height >> mip;
        if (is_block) {
            mip_w = (mip_w + 3) / 4;
            mip_h = (mip_h + 3) / 4;
        }
        mip_w = std::max(mip_w, 1u);
        mip_h = std::max(mip_h, 1u);
        uint32_t mip_d = std::max(depth >> mip, 1u);

        if (is_pow2) {
            mip_w = std::bit_ceil(mip_w);
            mip_h = std::bit_ceil(mip_h);
            mip_d = std::bit_ceil(mip_d);
        }

        auto& mip_info = out_mips_layout[mip];
        uint32_t mip_thickness = 1;
        if (array_mode == ArrayMode::ArrayLinearAligned) {
            std::tie(mip_info.pitch, mip_info.height, mip_info.size) =
                ImageSizeLinearAligned(mip_w, mip_h, num_bits, num_samples);
        } else if (macro &&
                   IsMacroTiledMip(mip_w, mip_h, num_bits, num_samples, tile_mode, mip, alt_tile)) {
            mip_thickness = thickness;
            std::tie(mip_info.pitch, mip_info.height, mip_info.size) =
                ImageSizeMacroTiled(mip_w, mip_h, num_bits, num_samples, tile_mode, alt_tile);
        } else {
            mip_thickness = std::min(thickness, 4u);
            std::tie(mip_info.pitch, mip_info.height, mip_info.size) =
                ImageSizeMicroTiled(mip_w, mip_h, mip_thickness, num_bits, num_samples);
            if (macro) {
                *out_micro_tiled_mips |= 1u << mip;
            }
        }
        if (is_block) {
            mip_info.pitch = std::max<uint16_t>(mip_info.pitch * 4, 32u);
            mip_info.height = std::max<uint16_t>(mip_info.height * 4, 32u);
        }
        uint32_t num_slices = mip_d * layers;
        num_slices += (-num_slices) & (mip_thickness - 1);
        mip_info.size *= num_slices;
        mip_info.offset = *out_size;
        *out_size += mip_info.size;
    }
}

struct TableEntry {
    uint32_t guest_size;
    uint32_t micro_tiled_mips;
    std::array<MipInfo, MAX_MIPS> mips_layout;
};

static constexpr uint32_t MAX_DIM_LOG2 = 12;

constexpr auto Pow2Bcn64ImageTable = [] {
    std::array<TableEntry, MAX_DIM_LOG2> entry{};
    for (uint32_t i = 0; i < MAX_DIM_LOG2; ++i) {
        const uint32_t pitch = 1 << i;
        ComputeImageSize(pitch, pitch, 1, std::bit_width(pitch), 1, 64, 1, true, true, false,
                         TileMode::Thin1DThin, ArrayMode::Array1DTiledThin1,
                         &entry[i].guest_size, &entry[i].micro_tiled_mips,
                         entry[i].mips_layout.data());
    }
    return entry;
}();

constexpr auto Pow2Bcn128ImageTable = [] {
    std::array<TableEntry, MAX_DIM_LOG2> entry{};
    for (uint32_t i = 0; i < MAX_DIM_LOG2; ++i) {
        const uint32_t pitch = 1 << i;
        ComputeImageSize(pitch, pitch, 1, std::bit_width(pitch), 1, 128, 1, true, true, false,
                         TileMode::Thin1DThin, ArrayMode::Array1DTiledThin1,
                         &entry[i].guest_size, &entry[i].micro_tiled_mips,
                         entry[i].mips_layout.data());
    }
    return entry;
}();


} // namespace Libs::Graphics::Gfx7

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGE_GFX7TILING_H_ */
