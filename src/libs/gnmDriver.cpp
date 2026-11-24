// SPDX-FileCopyrightText: Copyright 2026 CapyPS5 Project
// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project (packet layouts and the default hardware state)
// SPDX-License-Identifier: GPL-2.0-only

// libSceGnmDriver, the PS4 graphics driver, for PS4 backward compatibility. PS4 games build their
// PM4 command buffers themselves (the Gnmx library is linked into the game); the driver only writes
// the shader-setup packets, sends buffers to the GPU and patches flip requests. Everything goes to
// the same GPU queues as AGC on PS5.

#include "common/abi.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/sync.h"
#include "kernel/eventQueue.h"
#include "libs/agc.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <array>
#include <atomic>
#include <bit>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <mutex>
#include <tuple>

namespace Libs::Graphics::Gen4Driver {

LIB_NAME("GnmDriver", "GnmDriver");

namespace {

constexpr uint32_t PacketNop          = 0x10;
constexpr uint32_t PacketSetContextReg = 0x69;
constexpr uint32_t PacketSetShReg     = 0x76;
constexpr uint32_t PacketWriteData    = 0x37;
constexpr uint32_t PacketClearState   = 0x12;

constexpr int GnmErrorSubmissionFailed = static_cast<int>(0x80d11000u);
constexpr int GnmErrorNotEnoughResources = static_cast<int>(0x80d11081u);

constexpr uint32_t HwInitPacketSize = 0x100;

// NOP payloads the Gnmx library leaves where the driver must put a flip.
constexpr uint32_t PrepareFlip               = 0x68750777u;
constexpr uint32_t PrepareFlipLabel          = 0x68750778u;
constexpr uint32_t PrepareFlipInterrupt      = 0x68750780u;
constexpr uint32_t PrepareFlipInterruptLabel = 0x68750781u;

constexpr uint32_t Type3(uint32_t opcode, uint32_t payload_dwords) {
	return 0xc0000000u | ((payload_dwords - 1u) << 16u) | (opcode << 8u);
}

uint32_t* WriteRegs(uint32_t* cmd, uint32_t opcode, uint32_t reg, std::initializer_list<uint32_t> values) {
	*cmd++ = Type3(opcode, static_cast<uint32_t>(values.size()) + 1u);
	*cmd++ = reg;
	for (const auto value: values) {
		*cmd++ = value;
	}
	return cmd;
}

uint32_t* SetShReg(uint32_t* cmd, uint32_t reg, std::initializer_list<uint32_t> values) {
	return WriteRegs(cmd, PacketSetShReg, reg, values);
}

uint32_t* SetContextReg(uint32_t* cmd, uint32_t reg, std::initializer_list<uint32_t> values) {
	return WriteRegs(cmd, PacketSetContextReg, reg, values);
}

// Fills [cmd, cmd + dwords) with one NOP (a type-2 filler when only one dword is left).
void WriteNop(uint32_t* cmd, uint32_t dwords) {
	if (dwords == 0) {
		return;
	}
	if (dwords == 1) {
		*cmd = 0x80000000u;
		return;
	}
	*cmd = Type3(PacketNop, dwords - 1u);
	std::memset(cmd + 1, 0, (dwords - 1u) * sizeof(uint32_t));
}

// The shader setters end with a NOP over the rest of the region the game reserved (`size` dwords
// from `start`): a dword left over (zero) reads as a type-0 packet that swallows the next header.
// A region far larger than the packets falls back to the usual NOP of `fixed` dwords.
void TrailingNop(uint32_t* cmd, const uint32_t* start, uint32_t size, uint32_t fixed) {
	const auto used = static_cast<uint32_t>(cmd - start);
	const auto rest = size > used ? size - used : 0u;
	WriteNop(cmd, rest >= fixed && rest <= fixed + 4u ? rest : fixed);
}

// PS4 default hardware state after CLEAR_STATE (SDK 1.x), as the firmware sets it.
// Default context state packets of Gnm (ported from shadPS4, GPL-2.0-or-later).
constexpr std::array<uint32_t, 0x0f> DefaultContextState {
    0xc0012800u, 0x80000000u, 0x80000000u, 0xc0001200u, 0u, 0xc0002f00u, 1u, 0xc0016900u,
    0x102u, 0u, 0xc0016900u, 0x202u, 0xcc0010u, 0xc0111000u, 0u};

constexpr std::array<uint32_t, 0x62> DefaultContextState400 {
    0xc0012800u, 0x80000000u, 0x80000000u, 0xc0001200u, 0u, 0xc0016900u, 0x2f9u, 0x2du,
    0xc0016900u, 0x282u, 8u, 0xc0016900u, 0x280u, 0x80008u, 0xc0016900u, 0x281u,
    0xffff0000u, 0xc0016900u, 0x204u, 0u, 0xc0016900u, 0x206u, 0x43fu, 0xc0016900u,
    0x83u, 0xffffu, 0xc0016900u, 0x317u, 0x10u, 0xc0016900u, 0x2fau, 0x3f800000u,
    0xc0016900u, 0x2fcu, 0x3f800000u, 0xc0016900u, 0x2fbu, 0x3f800000u, 0xc0016900u, 0x2fdu,
    0x3f800000u, 0xc0016900u, 0x202u, 0xcc0010u, 0xc0016900u, 0x30eu, 0xffffffffu, 0xc0016900u,
    0x30fu, 0xffffffffu, 0xc0002f00u, 1u, 0xc0016900u, 0x1b1u, 2u, 0xc0016900u,
    0x101u, 0u, 0xc0016900u, 0x100u, 0xffffffffu, 0xc0016900u, 0x103u, 0u,
    0xc0016900u, 0x284u, 0u, 0xc0016900u, 0x290u, 0u, 0xc0016900u, 0x2aeu,
    0u, 0xc0016900u, 0x102u, 0u, 0xc0016900u, 0x292u, 0u, 0xc0016900u,
    0x293u, 0x6020000u, 0xc0016900u, 0x2f8u, 0u, 0xc0016900u, 0x2deu, 0x1e9u,
    0xc0036900u, 0x295u, 0x100u, 0x100u, 4u, 0xc0016900u, 0x2aau, 0xffu,
    0xc09e1000u, 0u};

constexpr std::array<uint32_t, 115> DefaultHardwareState {
    0xc0017600u, 0x216u, 0xffffffffu, 0xc0017600u, 0x217u, 0xffffffffu, 0xc0017600u, 0x215u,
    0u, 0xc0016900u, 0x2f9u, 0x2du, 0xc0016900u, 0x282u, 8u, 0xc0016900u,
    0x280u, 0x80008u, 0xc0016900u, 0x281u, 0xffff0000u, 0xc0016900u, 0x204u, 0u,
    0xc0016900u, 0x206u, 0x43fu, 0xc0016900u, 0x83u, 0xffffu, 0xc0016900u, 0x317u,
    0x10u, 0xc0016900u, 0x2fau, 0x3f800000u, 0xc0016900u, 0x2fcu, 0x3f800000u, 0xc0016900u,
    0x2fbu, 0x3f800000u, 0xc0016900u, 0x2fdu, 0x3f800000u, 0xc0016900u, 0x202u, 0xcc0010u,
    0xc0016900u, 0x30eu, 0xffffffffu, 0xc0016900u, 0x30fu, 0xffffffffu, 0xc0002f00u, 1u,
    0xc0017600u, 7u, 0x1ffu, 0xc0017600u, 0x46u, 0x1ffu, 0xc0017600u, 0x87u,
    0x1ffu, 0xc0017600u, 0xc7u, 0x1ffu, 0xc0017600u, 0x107u, 0u, 0xc0017600u,
    0x147u, 0x1ffu, 0xc0016900u, 0x1b1u, 2u, 0xc0016900u, 0x101u, 0u,
    0xc0016900u, 0x100u, 0xffffffffu, 0xc0016900u, 0x103u, 0u, 0xc0016900u, 0x284u,
    0u, 0xc0016900u, 0x290u, 0u, 0xc0016900u, 0x2aeu, 0u, 0xc0016900u,
    0x292u, 0u, 0xc0016900u, 0x293u, 0x6000000u, 0xc0016900u, 0x2f8u, 0u,
    0xc0016900u, 0x2deu, 0x1e9u, 0xc0036900u, 0x295u, 0x100u, 0x100u, 4u,
    0xc0017900u, 0x200u, 0xe0000000u,
};

// The same state for games built with later SDKs.
constexpr std::array<uint32_t, 115> DefaultHardwareState175 {
    0xc0017600u, 0x216u, 0xffffffffu, 0xc0017600u, 0x217u, 0xffffffffu, 0xc0017600u, 0x215u,
    0u, 0xc0016900u, 0x2f9u, 0x2du, 0xc0016900u, 0x282u, 8u, 0xc0016900u,
    0x280u, 0x80008u, 0xc0016900u, 0x281u, 0xffff0000u, 0xc0016900u, 0x204u, 0u,
    0xc0016900u, 0x206u, 0x43fu, 0xc0016900u, 0x83u, 0xffffu, 0xc0016900u, 0x317u,
    0x10u, 0xc0016900u, 0x2fau, 0x3f800000u, 0xc0016900u, 0x2fcu, 0x3f800000u, 0xc0016900u,
    0x2fbu, 0x3f800000u, 0xc0016900u, 0x2fdu, 0x3f800000u, 0xc0016900u, 0x202u, 0xcc0010u,
    0xc0016900u, 0x30eu, 0xffffffffu, 0xc0016900u, 0x30fu, 0xffffffffu, 0xc0002f00u, 1u,
    0xc0017600u, 7u, 0x1ffu, 0xc0017600u, 0x46u, 0x1ffu, 0xc0017600u, 0x87u,
    0x1ffu, 0xc0017600u, 0xc7u, 0x1ffu, 0xc0017600u, 0x107u, 0u, 0xc0017600u,
    0x147u, 0x1ffu, 0xc0016900u, 0x1b1u, 2u, 0xc0016900u, 0x101u, 0u,
    0xc0016900u, 0x100u, 0xffffffffu, 0xc0016900u, 0x103u, 0u, 0xc0016900u, 0x284u,
    0u, 0xc0016900u, 0x290u, 0u, 0xc0016900u, 0x2aeu, 0u, 0xc0016900u,
    0x292u, 0u, 0xc0016900u, 0x293u, 0x6020000u, 0xc0016900u, 0x2f8u, 0u,
    0xc0016900u, 0x2deu, 0x1e9u, 0xc0036900u, 0x295u, 0x100u, 0x100u, 4u,
    0xc0017900u, 0x200u, 0xe0000000u,
};
constexpr std::array<uint32_t, 118> DefaultHardwareState200 {
    0xc0017600u, 0x216u, 0xffffffffu, 0xc0017600u, 0x217u, 0xffffffffu, 0xc0017600u, 0x215u,
    0u, 0xc0016900u, 0x2f9u, 0x2du, 0xc0016900u, 0x282u, 8u, 0xc0016900u,
    0x280u, 0x80008u, 0xc0016900u, 0x281u, 0xffff0000u, 0xc0016900u, 0x204u, 0u,
    0xc0016900u, 0x206u, 0x43fu, 0xc0016900u, 0x83u, 0xffffu, 0xc0016900u, 0x317u,
    0x10u, 0xc0016900u, 0x2fau, 0x3f800000u, 0xc0016900u, 0x2fcu, 0x3f800000u, 0xc0016900u,
    0x2fbu, 0x3f800000u, 0xc0016900u, 0x2fdu, 0x3f800000u, 0xc0016900u, 0x202u, 0xcc0010u,
    0xc0016900u, 0x30eu, 0xffffffffu, 0xc0016900u, 0x30fu, 0xffffffffu, 0xc0002f00u, 1u,
    0xc0017600u, 7u, 0x1701ffu, 0xc0017600u, 0x46u, 0x1701fdu, 0xc0017600u, 0x87u,
    0x1701ffu, 0xc0017600u, 0xc7u, 0x1701fdu, 0xc0017600u, 0x107u, 0x17u, 0xc0017600u,
    0x147u, 0x1701fdu, 0xc0017600u, 0x47u, 0x1cu, 0xc0016900u, 0x1b1u, 2u,
    0xc0016900u, 0x101u, 0u, 0xc0016900u, 0x100u, 0xffffffffu, 0xc0016900u, 0x103u,
    0u, 0xc0016900u, 0x284u, 0u, 0xc0016900u, 0x290u, 0u, 0xc0016900u,
    0x2aeu, 0u, 0xc0016900u, 0x292u, 0u, 0xc0016900u, 0x293u, 0x6020000u,
    0xc0016900u, 0x2f8u, 0u, 0xc0016900u, 0x2deu, 0x1e9u, 0xc0036900u, 0x295u,
    0x100u, 0x100u, 4u, 0xc0017900u, 0x200u, 0xe0000000u,
};
constexpr std::array<uint32_t, 124> DefaultHardwareState350 {
    0xc0017600u, 0x216u, 0xffffffffu, 0xc0017600u, 0x217u, 0xffffffffu, 0xc0017600u, 0x215u,
    0u, 0xc0016900u, 0x2f9u, 0x2du, 0xc0016900u, 0x282u, 8u, 0xc0016900u,
    0x280u, 0x80008u, 0xc0016900u, 0x281u, 0xffff0000u, 0xc0016900u, 0x204u, 0u,
    0xc0016900u, 0x206u, 0x43fu, 0xc0016900u, 0x83u, 0xffffu, 0xc0016900u, 0x317u,
    0x10u, 0xc0016900u, 0x2fau, 0x3f800000u, 0xc0016900u, 0x2fcu, 0x3f800000u, 0xc0016900u,
    0x2fbu, 0x3f800000u, 0xc0016900u, 0x2fdu, 0x3f800000u, 0xc0016900u, 0x202u, 0xcc0010u,
    0xc0016900u, 0x30eu, 0xffffffffu, 0xc0016900u, 0x30fu, 0xffffffffu, 0xc0002f00u, 1u,
    0xc0017600u, 7u, 0x1701ffu, 0xc0017600u, 0x46u, 0x1701fdu, 0xc0017600u, 0x87u,
    0x1701ffu, 0xc0017600u, 0xc7u, 0x1701fdu, 0xc0017600u, 0x107u, 0x17u, 0xc0017600u,
    0x147u, 0x1701fdu, 0xc0017600u, 0x47u, 0x1cu, 0xc0016900u, 0x1b1u, 2u,
    0xc0016900u, 0x101u, 0u, 0xc0016900u, 0x100u, 0xffffffffu, 0xc0016900u, 0x103u,
    0u, 0xc0016900u, 0x284u, 0u, 0xc0016900u, 0x290u, 0u, 0xc0016900u,
    0x2aeu, 0u, 0xc0016900u, 0x102u, 0u, 0xc0016900u, 0x292u, 0u,
    0xc0016900u, 0x293u, 0x6020000u, 0xc0016900u, 0x2f8u, 0u, 0xc0016900u, 0x2deu,
    0x1e9u, 0xc0036900u, 0x295u, 0x100u, 0x100u, 4u, 0xc0017900u, 0x200u,
    0xe0000000u, 0xc0016900u, 0x2aau, 0xffu,
};

struct ComputeQueue {
	uint32_t* ring      = nullptr;
	uint32_t  ring_dw   = 0;
	uint32_t* read_ptr  = nullptr;
	uint32_t  offset_dw = 0;
	uint32_t  queue     = 0;
};

std::mutex                     g_submit_mutex;
std::array<ComputeQueue, 64>   g_compute_queues {};
std::atomic<uint32_t>          g_flips_patched {0};

void Submit(const uint32_t* dcb, uint32_t dcb_dw, const uint32_t* ccb, uint32_t ccb_dw) {
	GraphicsDbgDumpDcb("d", dcb_dw, dcb);
	GetRenderContext().GetGpu().Submit(
	    std::span {dcb, dcb_dw},
	    ccb != nullptr ? std::span {ccb, ccb_dw} : std::span<const uint32_t> {});
}

// Gnmx leaves a 64-dword NOP with a PrepareFlip* payload at the end of the last DCB. Replace it
// with the flip packet the command processor runs on the GPU timeline, then the label write the
// game asked for, and a NOP over the rest.
int PatchFlipRequest(uint32_t* cmd, uint32_t size_dw, uint32_t handle, uint32_t index,
                     uint32_t flip_mode, int64_t flip_arg) {
	if (size_dw < 64) {
		return GnmErrorSubmissionFailed;
	}
	cmd += size_dw - 64;
	if (cmd[0] != Type3(PacketNop, 63)) {
		LOGF("GnmDriver: no PrepareFlip packet at the end of the DCB (0x%08" PRIx32 ")\n", cmd[0]);
		return GnmErrorSubmissionFailed;
	}
	std::array<uint32_t, 5> payload {};
	std::memcpy(payload.data(), cmd + 1, sizeof(payload));
	const auto kind = payload[0];
	if (kind != PrepareFlip && kind != PrepareFlipLabel && kind != PrepareFlipInterrupt &&
	    kind != PrepareFlipInterruptLabel) {
		LOGF("GnmDriver: unknown flip marker 0x%08" PRIx32 "\n", kind);
		return GnmErrorSubmissionFailed;
	}
	auto* out = cmd;
	*out++ = KYTY_PM4(6, PacketNop, Pm4::R_GNM_FLIP);
	*out++ = handle;
	*out++ = index;
	*out++ = flip_mode;
	*out++ = static_cast<uint32_t>(static_cast<uint64_t>(flip_arg));
	*out++ = static_cast<uint32_t>(static_cast<uint64_t>(flip_arg) >> 32u);
	if (kind == PrepareFlipLabel || kind == PrepareFlipInterruptLabel) {
		// WRITE_DATA to memory: label address in payload[1..2], value in payload[3].
		*out++ = Type3(PacketWriteData, 4);
		*out++ = 0x500u;
		*out++ = payload[1] & ~3u;
		*out++ = payload[2];
		*out++ = payload[3];
	}
	if (kind == PrepareFlipInterrupt || kind == PrepareFlipInterruptLabel) {
		static std::atomic<bool> warned {false};
		if (!warned.exchange(true)) {
			LOGF("GnmDriver: flip interrupts are not raised yet\n");
		}
	}
	WriteNop(out, static_cast<uint32_t>(cmd + 64 - out));
	g_flips_patched.fetch_add(1, std::memory_order_relaxed);
	return OK;
}

bool ValidSizes(uint32_t count, const uint32_t* dcb_bytes, const uint32_t* ccb_bytes) {
	for (uint32_t i = 0; i < count; i++) {
		if (dcb_bytes[i] == 0 || dcb_bytes[i] > 0x3ffffc ||
		    (ccb_bytes != nullptr && ccb_bytes[i] > 0x3ffffc)) {
			return false;
		}
	}
	return true;
}

int SubmitBuffers(uint32_t count, uint32_t* const* dcbs, const uint32_t* dcb_bytes,
                  uint32_t* const* ccbs, const uint32_t* ccb_bytes) {
	for (uint32_t i = 0; i < count; i++) {
		const auto* ccb = ccbs != nullptr && ccb_bytes != nullptr && ccb_bytes[i] != 0 ? ccbs[i] : nullptr;
		Submit(dcbs[i], dcb_bytes[i] / 4u, ccb, ccb != nullptr ? ccb_bytes[i] / 4u : 0u);
	}
	return OK;
}

} // namespace

// ---- Shader setup: each writes the registers of one stage into the caller's command buffer. ----

int KYTY_SYSV_ABI GnmSetVsShader(uint32_t* cmd, uint32_t size, const uint32_t* regs, uint32_t modifier) {
	PRINT_NAME();
	uint32_t* const start = cmd;
	if (cmd == nullptr || size <= 0x1c || regs == nullptr || (modifier & 0xfcfffc3fu) != 0 || regs[1] != 0) {
		return -1;
	}
	const uint32_t rsrc1 = modifier == 0 ? regs[2] : (regs[2] & 0xfcfffc3fu) | modifier;
	cmd = SetShReg(cmd, 0x48, {regs[0], 0});            // SPI_SHADER_PGM_LO/HI_VS
	cmd = SetShReg(cmd, 0x4a, {rsrc1, regs[3]});        // SPI_SHADER_PGM_RSRC1/2_VS
	cmd = SetContextReg(cmd, 0x207, {regs[6]});         // PA_CL_VS_OUT_CNTL
	cmd = SetContextReg(cmd, 0x1b1, {regs[4]});         // SPI_VS_OUT_CONFIG
	cmd = SetContextReg(cmd, 0x1c3, {regs[5]});         // SPI_SHADER_POS_FORMAT
	TrailingNop(cmd, start, size, 11);
	return OK;
}

int KYTY_SYSV_ABI GnmSetPsShader(uint32_t* cmd, uint32_t size, const uint32_t* regs) {
	PRINT_NAME();
	uint32_t* const start = cmd;
	if (cmd == nullptr || size <= 0x27) {
		return -1;
	}
	if (regs == nullptr) {
		cmd = SetShReg(cmd, 0x08, {0, 0});           // SPI_SHADER_PGM_LO/HI_PS
		cmd = SetContextReg(cmd, 0x203, {0});        // DB_SHADER_CONTROL
		TrailingNop(cmd, start, size, 0x20);
		return OK;
	}
	if (regs[1] != 0) {
		return -1;
	}
	cmd = SetShReg(cmd, 0x08, {regs[0], 0});              // SPI_SHADER_PGM_LO/HI_PS
	cmd = SetShReg(cmd, 0x0a, {regs[2], regs[3]});        // SPI_SHADER_PGM_RSRC1/2_PS
	cmd = SetContextReg(cmd, 0x1c4, {regs[4], regs[5]});  // SPI_SHADER_Z_FORMAT, COL_FORMAT
	cmd = SetContextReg(cmd, 0x1b3, {regs[6], regs[7]});  // SPI_PS_INPUT_ENA, INPUT_ADDR
	cmd = SetContextReg(cmd, 0x1b6, {regs[8]});           // SPI_PS_IN_CONTROL
	cmd = SetContextReg(cmd, 0x1b8, {regs[9]});           // SPI_BARYC_CNTL
	cmd = SetContextReg(cmd, 0x203, {regs[10]});          // DB_SHADER_CONTROL
	cmd = SetContextReg(cmd, 0x08f, {regs[11]});          // CB_SHADER_MASK
	TrailingNop(cmd, start, size, 11);
	return OK;
}

int KYTY_SYSV_ABI GnmSetCsShader(uint32_t* cmd, uint32_t size, const uint32_t* regs) {
	PRINT_NAME();
	uint32_t* const start = cmd;
	if (cmd == nullptr || size <= 0x18 || regs == nullptr || regs[1] != 0) {
		return -1;
	}
	cmd = SetShReg(cmd, 0x20c, {regs[0], 0});                 // COMPUTE_PGM_LO/HI
	cmd = SetShReg(cmd, 0x212, {regs[2], regs[3]});           // COMPUTE_PGM_RSRC1/2
	cmd = SetShReg(cmd, 0x207, {regs[4], regs[5], regs[6]});  // COMPUTE_NUM_THREAD_X/Y/Z
	TrailingNop(cmd, start, size, 11);
	return OK;
}

int KYTY_SYSV_ABI GnmSetEsShader(uint32_t* cmd, uint32_t size, const uint32_t* regs, uint32_t modifier) {
	PRINT_NAME();
	uint32_t* const start = cmd;
	if (cmd == nullptr || size < 0x14 || regs == nullptr || (modifier & 0xfcfffc3fu) != 0 || regs[1] != 0) {
		return -1;
	}
	const uint32_t rsrc1 = modifier == 0 ? regs[2] : (regs[2] & 0xfcfffc3fu) | modifier;
	cmd = SetShReg(cmd, 0xc8, {regs[0], 0});      // SPI_SHADER_PGM_LO/HI_ES
	cmd = SetShReg(cmd, 0xca, {rsrc1, regs[3]});  // SPI_SHADER_PGM_RSRC1/2_ES
	TrailingNop(cmd, start, size, 11);
	return OK;
}

int KYTY_SYSV_ABI GnmSetGsShader(uint32_t* cmd, uint32_t size, const uint32_t* regs) {
	PRINT_NAME();
	uint32_t* const start = cmd;
	if (cmd == nullptr || size < 0x1d || regs == nullptr || regs[1] != 0) {
		return -1;
	}
	cmd = SetShReg(cmd, 0x88, {regs[0], 0});         // SPI_SHADER_PGM_LO/HI_GS
	cmd = SetShReg(cmd, 0x8a, {regs[2], regs[3]});   // SPI_SHADER_PGM_RSRC1/2_GS
	cmd = SetContextReg(cmd, 0x2e5, {regs[4]});      // VGT_STRMOUT_CONFIG
	cmd = SetContextReg(cmd, 0x29b, {regs[5]});      // VGT_GS_OUT_PRIM_TYPE
	cmd = SetContextReg(cmd, 0x2e4, {regs[6]});      // VGT_GS_INSTANCE_CNT
	TrailingNop(cmd, start, size, 11);
	return OK;
}

int KYTY_SYSV_ABI GnmSetHsShader(uint32_t* cmd, uint32_t size, const uint32_t* regs, uint32_t ls_hs_config) {
	PRINT_NAME();
	uint32_t* const start = cmd;
	if (cmd == nullptr || size < 0x1e || regs == nullptr || regs[1] != 0) {
		return -1;
	}
	cmd = SetShReg(cmd, 0x108, {regs[0], 0});           // SPI_SHADER_PGM_LO/HI_HS
	cmd = SetShReg(cmd, 0x10a, {regs[2], regs[3]});     // SPI_SHADER_PGM_RSRC1/2_HS
	cmd = SetContextReg(cmd, 0x286, {regs[5], regs[6]}); // VGT_HOS_MAX/MIN_TESS_LEVEL
	cmd = SetContextReg(cmd, 0x2db, {regs[4]});         // VGT_TF_PARAM
	cmd = SetContextReg(cmd, 0x2d6, {ls_hs_config});    // VGT_LS_HS_CONFIG
	TrailingNop(cmd, start, size, 11);
	return OK;
}

int KYTY_SYSV_ABI GnmSetLsShader(uint32_t* cmd, uint32_t size, const uint32_t* regs, uint32_t modifier) {
	PRINT_NAME();
	uint32_t* const start = cmd;
	if (cmd == nullptr || size < 0x17 || regs == nullptr || regs[1] != 0) {
		return -1;
	}
	const uint32_t mask = (modifier & 0xfffffc3fu) == 0 ? 0xfffffc3fu : 0xfcfffc3fu;
	if ((modifier & mask) != 0) {
		return -1;
	}
	const uint32_t rsrc1 = modifier == 0 ? regs[2] : (regs[2] & mask) | modifier;
	cmd = SetShReg(cmd, 0x148, {regs[0], 0});      // SPI_SHADER_PGM_LO/HI_LS
	cmd = SetShReg(cmd, 0x14b, {regs[3]});         // SPI_SHADER_PGM_RSRC2_LS
	cmd = SetShReg(cmd, 0x14a, {rsrc1, regs[3]});  // SPI_SHADER_PGM_RSRC1/2_LS
	TrailingNop(cmd, start, size, 11);
	return OK;
}

// The caller reserved `size` dwords and resumes after them: pad the rest with one NOP packet.
template <size_t N>
static uint32_t WriteDefaultContextState(uint32_t* cmd, uint32_t size, const std::array<uint32_t, N>& state) {
	std::memcpy(cmd, state.data(), sizeof(state));
	cmd[N] = 0xC0001000u | ((size - static_cast<uint32_t>(N) - 2u) << 16u);
	return size;
}

uint32_t KYTY_SYSV_ABI GnmDrawInitToDefaultContextState(uint32_t* cmd, uint32_t size) {
	PRINT_NAME();
	if (size != 0x20u) {
		return 0;
	}
	return WriteDefaultContextState(cmd, size, DefaultContextState);
}

uint32_t KYTY_SYSV_ABI GnmDrawInitToDefaultContextState400(uint32_t* cmd, uint32_t size) {
	PRINT_NAME();
	if (size != 0x100u) {
		return 0;
	}
	return WriteDefaultContextState(cmd, size, DefaultContextState400);
}

int KYTY_SYSV_ABI GnmComputeWaitOnAddress(uint32_t* cmd, uint32_t size, uint64_t addr, uint32_t mask,
                                          uint32_t func, uint32_t ref) {
	PRINT_NAME();
	if (cmd == nullptr || size != 0xeu) {
		return -1;
	}
	WriteNop(cmd, 3u);
	const uint32_t is_mem = addr > 0xffffu ? 1u : 0u;
	cmd[3]  = Type3(0x3cu, 6u); // WAIT_REG_MEM
	cmd[4]  = (is_mem << 4u) | (func & 7u);
	cmd[5]  = static_cast<uint32_t>(addr) & (is_mem != 0u ? 0xfffffffcu : 0xffffu);
	cmd[6]  = static_cast<uint32_t>(addr >> 32u);
	cmd[7]  = ref;
	cmd[8]  = mask;
	cmd[9]  = 10u;
	WriteNop(cmd + 10, 4u);
	return OK;
}

uint32_t KYTY_SYSV_ABI GnmGetGpuCoreClockFrequency() {
	PRINT_NAME();
	return 800000000u;
}

// The SGPR offsets are per shader stage (Cs, Ps, Vs, Gs, Es, Hs, Ls), as in shadPS4.
int KYTY_SYSV_ABI GnmDrawIndirect(uint32_t* cmd, uint32_t size, uint32_t data_offset, uint32_t stage,
                                  uint32_t vertex_sgpr, uint32_t instance_sgpr, uint32_t /*flags*/) {
	PRINT_NAME();
	static constexpr std::array<uint32_t, 7> SgprBase {0u, 0u, 0x4cu, 0u, 0xccu, 0u, 0x14cu};
	if (cmd == nullptr || size != 9u || stage >= SgprBase.size() || vertex_sgpr >= 0x10u || instance_sgpr >= 0x10u) {
		return -1;
	}
	cmd[0] = Type3(0x24u, 4u); // DRAW_INDIRECT
	cmd[1] = data_offset;
	cmd[2] = vertex_sgpr == 0u ? 0u : vertex_sgpr + SgprBase[stage];
	cmd[3] = instance_sgpr == 0u ? 0u : instance_sgpr + SgprBase[stage];
	cmd[4] = 2u; // auto index
	WriteNop(cmd + 5, 4u);
	return OK;
}

uint32_t KYTY_SYSV_ABI GnmDrawInitDefaultHardwareState(uint32_t* cmd, uint32_t size) {
	PRINT_NAME();
	if (cmd == nullptr || size < HwInitPacketSize) {
		return 0;
	}
	auto* out = cmd;
	*out++    = Type3(PacketClearState, 1);
	*out++    = 0;
	std::memcpy(out, DefaultHardwareState.data(), DefaultHardwareState.size() * sizeof(uint32_t));
	out += DefaultHardwareState.size();
	WriteNop(out, static_cast<uint32_t>(cmd + HwInitPacketSize - out));
	return HwInitPacketSize;
}

template <size_t N>
uint32_t WriteDefaultHardwareState(uint32_t* cmd, uint32_t size, const std::array<uint32_t, N>& state) {
	if (cmd == nullptr || size < HwInitPacketSize) {
		return 0;
	}
	auto* out = cmd;
	*out++    = Type3(PacketClearState, 1);
	*out++    = 0;
	std::memcpy(out, state.data(), state.size() * sizeof(uint32_t));
	out += state.size();
	WriteNop(out, static_cast<uint32_t>(cmd + HwInitPacketSize - out));
	return HwInitPacketSize;
}

uint32_t KYTY_SYSV_ABI GnmDrawInitDefaultHardwareState175(uint32_t* cmd, uint32_t size) {
	PRINT_NAME();
	return WriteDefaultHardwareState(cmd, size, DefaultHardwareState175);
}

uint32_t KYTY_SYSV_ABI GnmDrawInitDefaultHardwareState200(uint32_t* cmd, uint32_t size) {
	PRINT_NAME();
	return WriteDefaultHardwareState(cmd, size, DefaultHardwareState200);
}

uint32_t KYTY_SYSV_ABI GnmDrawInitDefaultHardwareState350(uint32_t* cmd, uint32_t size) {
	PRINT_NAME();
	return WriteDefaultHardwareState(cmd, size, DefaultHardwareState350);
}

// ---- Draws, for games that call the driver instead of writing the packets themselves. ----

int KYTY_SYSV_ABI GnmDrawIndex(uint32_t* cmd, uint32_t size, uint32_t index_count, uint64_t index_addr,
                               uint32_t flags, uint32_t type) {
	PRINT_NAME();
	(void)type;
	if (cmd == nullptr || size != 10 || index_addr == 0 || (index_addr & 1u) != 0 ||
	    (flags & 0x1ffffffeu) != 0) {
		return -1;
	}
	*cmd++ = Type3(0x27, 5); // DRAW_INDEX_2
	*cmd++ = index_count;
	*cmd++ = static_cast<uint32_t>(index_addr);
	*cmd++ = static_cast<uint32_t>(index_addr >> 32u);
	*cmd++ = index_count;
	*cmd++ = 0;
	WriteNop(cmd, 4);
	return OK;
}

int KYTY_SYSV_ABI GnmDrawIndexAuto(uint32_t* cmd, uint32_t size, uint32_t index_count, uint32_t flags) {
	PRINT_NAME();
	if (cmd == nullptr || size != 7 || (flags & 0x1ffffffeu) != 0) {
		return -1;
	}
	*cmd++ = Type3(0x2d, 2); // DRAW_INDEX_AUTO
	*cmd++ = index_count;
	*cmd++ = 2; // auto-generated indices
	WriteNop(cmd, 4);
	return OK;
}

// COMPUTE_PGM_RSRC1 with the caller's modifier bits, otherwise GnmSetCsShader.
int KYTY_SYSV_ABI GnmSetCsShaderWithModifier(uint32_t* cmd, uint32_t size, const uint32_t* regs, uint32_t modifier) {
	PRINT_NAME();
	uint32_t* const start = cmd;
	if (cmd == nullptr || size <= 0x18 || regs == nullptr || (modifier & 0xfffffc3fu) != 0 || regs[1] != 0) {
		return -1;
	}
	const uint32_t rsrc1 = modifier == 0 ? regs[2] : (regs[2] & 0xfffffc3fu) | modifier;
	cmd = SetShReg(cmd, 0x20c, {regs[0], 0});                 // COMPUTE_PGM_LO/HI
	cmd = SetShReg(cmd, 0x212, {rsrc1, regs[3]});             // COMPUTE_PGM_RSRC1/2
	cmd = SetShReg(cmd, 0x207, {regs[4], regs[5], regs[6]});  // COMPUTE_NUM_THREAD_X/Y/Z
	TrailingNop(cmd, start, size, 11);
	return OK;
}

int KYTY_SYSV_ABI GnmDispatchDirect(uint32_t* cmd, uint32_t size, uint32_t x, uint32_t y, uint32_t z, uint32_t flags) {
	PRINT_NAME();
	if (cmd == nullptr || size != 9 || static_cast<int32_t>(x | y | z) < 0) {
		return -1;
	}
	*cmd++ = Type3(0x15, 4) | 2u | (flags & 1u); // DISPATCH_DIRECT, compute shader type, predicate
	*cmd++ = x;
	*cmd++ = y;
	*cmd++ = z;
	*cmd++ = (flags & 0x18u) + 1u; // COMPUTE_SHADER_EN and the ordered-append mode
	WriteNop(cmd, 4);
	return OK;
}

// Indirect dispatches and draws (packet layouts as in shadPS4's GnmDriver). The arguments come from
// the base set by SET_BASE plus data_offset, or, on a compute queue (MEC), from `args` itself.
int KYTY_SYSV_ABI GnmDispatchIndirect(uint32_t* cmd, uint32_t size, uint32_t data_offset, uint32_t flags) {
	PRINT_NAME();
	if (cmd == nullptr || size != 7) {
		return -1;
	}
	*cmd++ = Type3(0x16, 2) | 2u | (flags & 1u); // DISPATCH_INDIRECT, compute shader type, predicate
	*cmd++ = data_offset;
	*cmd++ = (flags & 0x18u) + 1u;
	WriteNop(cmd, 4);
	return OK;
}

int KYTY_SYSV_ABI GnmDispatchIndirectOnMec(uint32_t* cmd, uint32_t size, uint64_t args, uint32_t modifier) {
	PRINT_NAME();
	if (cmd == nullptr || size != 8 || args == 0 || (args & 3u) != 0) {
		return -1;
	}
	*cmd++ = Type3(0x16, 3) | 2u | (modifier & 1u);
	*cmd++ = static_cast<uint32_t>(args);
	*cmd++ = static_cast<uint32_t>(args >> 32u);
	*cmd++ = (modifier & 0x18u) | 1u;
	WriteNop(cmd, 4);
	return OK;
}

int KYTY_SYSV_ABI GnmDrawIndexIndirect(uint32_t* cmd, uint32_t size, uint32_t data_offset, uint32_t stage,
                                       uint32_t vertex_sgpr, uint32_t instance_sgpr, uint32_t flags) {
	PRINT_NAME();
	// SPI_SHADER_USER_DATA_x_0 of the stage that receives the base vertex and instance (VS, ES, LS).
	constexpr std::array<uint32_t, 7> user_data {0u, 0u, 0x4cu, 0u, 0xccu, 0u, 0x14cu};
	if (cmd == nullptr || size != 9 || stage >= user_data.size() || vertex_sgpr >= 0x10u ||
	    instance_sgpr >= 0x10u) {
		return -1;
	}
	*cmd++ = Type3(0x25, 4); // DRAW_INDEX_INDIRECT
	*cmd++ = data_offset;
	*cmd++ = vertex_sgpr == 0 ? 0u : vertex_sgpr + user_data[stage];
	*cmd++ = instance_sgpr == 0 ? 0u : instance_sgpr + user_data[stage];
	*cmd++ = 0; // DRAW_INITIATOR
	WriteNop(cmd, 4);
	return OK;
}

int KYTY_SYSV_ABI GnmDrawIndexOffset(uint32_t* cmd, uint32_t size, uint32_t index_offset, uint32_t index_count,
                                     uint32_t flags) {
	PRINT_NAME();
	(void)flags;
	if (cmd == nullptr || size != 9) {
		return -1;
	}
	*cmd++ = Type3(0x35, 4); // DRAW_INDEX_OFFSET_2
	*cmd++ = index_count;    // max size
	*cmd++ = index_offset;
	*cmd++ = index_count;
	*cmd++ = 0; // DRAW_INITIATOR
	WriteNop(cmd, 4);
	return OK;
}

// PS4 PS setter of SDK 3.50 and later: the null shader also resets CB_SHADER_MASK.
int KYTY_SYSV_ABI GnmSetPsShader350(uint32_t* cmd, uint32_t size, const uint32_t* regs) {
	PRINT_NAME();
	if (regs != nullptr) {
		return GnmSetPsShader(cmd, size, regs);
	}
	uint32_t* const start = cmd;
	if (cmd == nullptr || size <= 0x27) {
		return -1;
	}
	cmd = SetShReg(cmd, 0x08, {0, 0});      // SPI_SHADER_PGM_LO/HI_PS
	cmd = SetContextReg(cmd, 0x203, {0});   // DB_SHADER_CONTROL
	cmd = SetContextReg(cmd, 0x08f, {0xf}); // CB_SHADER_MASK
	TrailingNop(cmd, start, size, 0x1d);
	return OK;
}

// IA_MULTI_VGT_PARAM only tunes how the PS4 hardware splits primitive groups: nothing to do on a
// host GPU, so the region is filler.
int KYTY_SYSV_ABI GnmSetVgtControl(uint32_t* cmd, uint32_t size, uint32_t prim_group_minus_one,
                                   uint32_t partial_vs_wave, uint32_t wd_switch_only_on_eop) {
	PRINT_NAME();
	if (cmd == nullptr || size != 3 || prim_group_minus_one >= 0x100u ||
	    (partial_vs_wave | wd_switch_only_on_eop) >= 2u) {
		return -1;
	}
	WriteNop(cmd, 3);
	return OK;
}

// Debug markers are only filler for the GPU, of the size the game reserved.
int KYTY_SYSV_ABI GnmInsertMarker(uint32_t* cmd, uint32_t size, const char* marker) {
	PRINT_NAME();
	(void)marker;
	if (cmd == nullptr || size < 2) {
		return -1;
	}
	WriteNop(cmd, size);
	return OK;
}

int KYTY_SYSV_ABI GnmInsertPopMarker(uint32_t* cmd, uint32_t size) {
	PRINT_NAME();
	if (cmd == nullptr || size != 6) {
		return -1;
	}
	WriteNop(cmd, size);
	return OK;
}

// Workloads only group submissions for profiling tools.
int KYTY_SYSV_ABI GnmCreateWorkloadStream(uint64_t name, uint32_t* stream) {
	PRINT_NAME();
	if (name == 0 || stream == nullptr) {
		return 3;
	}
	*stream = 1;
	return OK;
}

int KYTY_SYSV_ABI GnmBeginWorkload(uint32_t stream, uint64_t* workload) {
	PRINT_NAME();
	if (workload == nullptr) {
		return 3;
	}
	*workload = stream < 0x10u ? 1u : 0u;
	return stream < 0x10u ? 0 : 1;
}

int KYTY_SYSV_ABI GnmEndWorkload(uint64_t workload) {
	PRINT_NAME();
	if (workload == 0) {
		return 2;
	}
	return ((workload >> 0x38u) & 0xffu) > 0xfu ? 2 : 0;
}

uint32_t KYTY_SYSV_ABI GnmDispatchInitDefaultHardwareState(uint32_t* cmd, uint32_t size) {
	PRINT_NAME();
	if (cmd == nullptr || size < HwInitPacketSize) {
		return 0;
	}
	auto* out = cmd;
	// COMPUTE_STATIC_THREAD_MGMT_SE0/SE1 and COMPUTE_RESOURCE_LIMITS (compute shader type bit).
	for (const auto [reg, value]: {std::pair {0x216u, 0xffffffffu}, std::pair {0x217u, 0xffffffffu},
	                               std::pair {0x215u, 0x170u}}) {
		*out++ = Type3(PacketSetShReg, 2) | 2u;
		*out++ = reg;
		*out++ = value;
	}
	WriteNop(out, static_cast<uint32_t>(cmd + HwInitPacketSize - out));
	return HwInitPacketSize;
}

int KYTY_SYSV_ABI GnmInsertWaitFlipDone(uint32_t* cmd, uint32_t size, int handle, uint32_t index) {
	PRINT_NAME();
	if (cmd == nullptr || size != 7) {
		return -1;
	}
	// The command processor waits for the buffer's flip without stalling the other queues.
	cmd[0] = KYTY_PM4(7, PacketNop, Pm4::R_GNM_WAIT_FLIP_DONE);
	cmd[1] = static_cast<uint32_t>(handle);
	cmd[2] = index;
	cmd[3] = cmd[4] = cmd[5] = cmd[6] = 0;
	return OK;
}

// ---- Submission ----

int KYTY_SYSV_ABI GnmSubmitCommandBuffers(uint32_t count, uint32_t* const* dcbs, const uint32_t* dcb_bytes,
                                          uint32_t* const* ccbs, const uint32_t* ccb_bytes) {
	PRINT_NAME();
	if (count != 0 && (dcbs == nullptr || dcb_bytes == nullptr || !ValidSizes(count, dcb_bytes, ccb_bytes))) {
		return GnmErrorSubmissionFailed;
	}
	std::scoped_lock lock(g_submit_mutex);
	return SubmitBuffers(count, dcbs, dcb_bytes, ccbs, ccb_bytes);
}

int KYTY_SYSV_ABI GnmSubmitAndFlipCommandBuffers(uint32_t count, uint32_t* const* dcbs,
                                                 const uint32_t* dcb_bytes, uint32_t* const* ccbs,
                                                 const uint32_t* ccb_bytes, uint32_t handle,
                                                 uint32_t index, uint32_t flip_mode, int64_t flip_arg) {
	PRINT_NAME();
	if (count == 0 || dcbs == nullptr || dcb_bytes == nullptr || !ValidSizes(count, dcb_bytes, ccb_bytes)) {
		return GnmErrorSubmissionFailed;
	}
	std::scoped_lock lock(g_submit_mutex);
	if (const int result = PatchFlipRequest(dcbs[count - 1], dcb_bytes[count - 1] / 4u, handle, index,
	                                        flip_mode, flip_arg);
	    result != OK) {
		return result;
	}
	return SubmitBuffers(count, dcbs, dcb_bytes, ccbs, ccb_bytes);
}

// The workload variants take the workload id first; it is only a profiling tag.
int KYTY_SYSV_ABI GnmSubmitCommandBuffersForWorkload(uint64_t workload, uint32_t count, uint32_t* const* dcbs,
                                                     const uint32_t* dcb_bytes, uint32_t* const* ccbs,
                                                     const uint32_t* ccb_bytes) {
	PRINT_NAME();
	(void)workload;
	return GnmSubmitCommandBuffers(count, dcbs, dcb_bytes, ccbs, ccb_bytes);
}

int KYTY_SYSV_ABI GnmSubmitAndFlipCommandBuffersForWorkload(uint64_t workload, uint32_t count,
                                                            uint32_t* const* dcbs, const uint32_t* dcb_bytes,
                                                            uint32_t* const* ccbs, const uint32_t* ccb_bytes,
                                                            uint32_t handle, uint32_t index, uint32_t flip_mode,
                                                            int64_t flip_arg) {
	PRINT_NAME();
	(void)workload;
	return GnmSubmitAndFlipCommandBuffers(count, dcbs, dcb_bytes, ccbs, ccb_bytes, handle, index, flip_mode,
	                                      flip_arg);
}

int KYTY_SYSV_ABI GnmSubmitDone() {
	PRINT_NAME();
	std::scoped_lock lock(g_submit_mutex);
	GetRenderContext().GetGpu().SuspendPoint();
	return OK;
}

int KYTY_SYSV_ABI GnmRequestFlipAndSubmitDone(uint32_t* cmd, uint32_t size_bytes, uint32_t handle,
                                              uint32_t index, uint32_t flip_mode, int64_t flip_arg) {
	PRINT_NAME();
	if (cmd == nullptr || size_bytes < 0x100) {
		return GnmErrorNotEnoughResources;
	}
	std::scoped_lock lock(g_submit_mutex);
	cmd[0] = Type3(PacketNop, 63);
	cmd[1] = PrepareFlip;
	if (const int result = PatchFlipRequest(cmd, 64, handle, index, flip_mode, flip_arg); result != OK) {
		return result;
	}
	Submit(cmd, 64, nullptr, 0);
	GetRenderContext().GetGpu().SuspendPoint();
	return OK;
}

int KYTY_SYSV_ABI GnmAreSubmitsAllowed() {
	PRINT_NAME();
	return 1;
}

// ---- Asynchronous compute queues: a ring the game fills, submitted on each DingDong. ----

int KYTY_SYSV_ABI GnmMapComputeQueue(uint32_t pipe, uint32_t queue, uint32_t* ring, uint32_t ring_dw,
                                     uint32_t* read_ptr) {
	PRINT_NAME();
	if (pipe >= 7 || queue >= 8 || ring == nullptr || (reinterpret_cast<uintptr_t>(ring) & 0xffu) != 0 ||
	    !std::has_single_bit(ring_dw) || read_ptr == nullptr) {
		return static_cast<int>(0x80d17000u);
	}
	std::scoped_lock lock(g_submit_mutex);
	for (uint32_t vqid = 0; vqid < g_compute_queues.size(); vqid++) {
		auto& q = g_compute_queues[vqid];
		if (q.ring == nullptr) {
			// AGC compute queues use ids 0x20..0x57; give each PS4 pipe/queue pair its own.
			q         = {ring, ring_dw, read_ptr, 0, 0x20u + pipe * 8u + queue};
			*read_ptr = 0;
			return static_cast<int>(vqid + 1);
		}
	}
	return static_cast<int>(0x80d17000u);
}

int KYTY_SYSV_ABI GnmUnmapComputeQueue(uint32_t vqid) {
	PRINT_NAME();
	std::scoped_lock lock(g_submit_mutex);
	if (vqid != 0 && vqid <= g_compute_queues.size()) {
		g_compute_queues[vqid - 1] = {};
	}
	return OK;
}

void KYTY_SYSV_ABI GnmDingDong(uint32_t vqid, uint32_t next_offset_dw) {
	PRINT_NAME();
	std::scoped_lock lock(g_submit_mutex);
	if (vqid == 0 || vqid > g_compute_queues.size() || g_compute_queues[vqid - 1].ring == nullptr) {
		return;
	}
	auto& q   = g_compute_queues[vqid - 1];
	auto& gpu = GetRenderContext().GetGpu();
	if (next_offset_dw == q.offset_dw) {
		return;
	}
	if (next_offset_dw < q.offset_dw) {
		// The submission wraps around the end of the ring.
		gpu.SubmitCompute(q.queue, std::span<const uint32_t> {q.ring + q.offset_dw, q.ring_dw - q.offset_dw});
		q.offset_dw = 0;
	}
	if (next_offset_dw != 0) {
		gpu.SubmitCompute(q.queue, std::span<const uint32_t> {q.ring + q.offset_dw, next_offset_dw - q.offset_dw});
	}
	q.offset_dw  = next_offset_dw;
	*q.read_ptr  = next_offset_dw;
}

// ---- Events ----

int KYTY_SYSV_ABI GnmAddEqEvent(LibKernel::EventQueue::KernelEqueue eq, uint64_t id, void* udata) {
	PRINT_NAME();
	if (eq == LibKernel::EventQueue::KERNEL_EQUEUE_INVALID) {
		return LibKernel::KERNEL_ERROR_EBADF;
	}
	return Sync::AddEqEvent(GetRenderContext(), eq, static_cast<int>(id), udata);
}

int KYTY_SYSV_ABI GnmDeleteEqEvent(LibKernel::EventQueue::KernelEqueue eq, uint64_t id) {
	PRINT_NAME();
	if (eq == LibKernel::EventQueue::KERNEL_EQUEUE_INVALID) {
		return LibKernel::KERNEL_ERROR_EBADF;
	}
	return Sync::DeleteEqEvent(GetRenderContext(), eq, static_cast<int>(id));
}

int KYTY_SYSV_ABI GnmGetEqEventType(const LibKernel::EventQueue::KernelEvent* ev) {
	PRINT_NAME();
	if (ev == nullptr) {
		return 0;
	}
	return ev->filter == LibKernel::EventQueue::KERNEL_EVFILT_GRAPHICS ? static_cast<int>(ev->ident)
	                                                                   : static_cast<int>(ev->data);
}

uint64_t KYTY_SYSV_ABI GnmGetEqTimeStamp() {
	PRINT_NAME();
	return 0;
}

// ---- Configuration, debugging and profiling: nothing to do on the host. ----

int KYTY_SYSV_ABI GnmSetGsRingSizes(uint32_t esgs_ring_size, uint32_t gsvs_ring_size) {
	PRINT_NAME();
	constexpr uint32_t Min = 0x400000, Max = 0x800000, Align = 0x100000;
	for (const auto ring: {esgs_ring_size, gsvs_ring_size}) {
		if (ring < Min || ring > Max || (ring & (Align - 1)) != 0) {
			return 0x08000000;
		}
	}
	return OK;
}

uint64_t KYTY_SYSV_ABI GnmGetTheTessellationFactorRingBufferBaseAddress() {
	PRINT_NAME();
	// The PS4 SDK places the ring at a fixed address in the system-reserved range.
	return 0xff0000000ull;
}

uint32_t KYTY_SYSV_ABI GnmGetOffChipTessellationBufferSize() {
	PRINT_NAME();
	// The PS4 driver's off-chip tessellation buffer (as in shadPS4): games allocate it themselves.
	return 0x800000u;
}

void KYTY_SYSV_ABI GnmFlushGarlic() {
	PRINT_NAME();
}

bool KYTY_SYSV_ABI GnmIsUserPaEnabled() {
	PRINT_NAME();
	return false;
}

int KYTY_SYSV_ABI GnmLogicalCuMaskToPhysicalCuMask(int64_t /*unused*/, int logical_cu_mask) {
	PRINT_NAME();
	return logical_cu_mask;
}

int KYTY_SYSV_ABI GnmStubOk() {
	PRINT_NAME();
	return OK;
}

bool KYTY_SYSV_ABI RazorIsLoaded() {
	PRINT_NAME();
	return false;
}

LIB_DEFINE(InitGnmDriver_1) {
	namespace Gnm = Graphics::Gen4Driver;

	LIB_FUNC("gAhCn6UiU4Y", Gnm::GnmSetVsShader);
	LIB_FUNC("bQVd5YzCal0", Gnm::GnmSetPsShader);
	LIB_FUNC("KXltnCwEJHQ", Gnm::GnmSetCsShader);
	LIB_FUNC("FUHG8sQ3R58", Gnm::GnmSetEsShader);
	LIB_FUNC("UJwNuMBcUAk", Gnm::GnmSetGsShader);
	LIB_FUNC("VJNjFtqiF5w", Gnm::GnmSetHsShader);
	LIB_FUNC("vckdzbQ46SI", Gnm::GnmSetLsShader);
	LIB_FUNC("Idffwf3yh8s", Gnm::GnmDrawInitDefaultHardwareState);
	LIB_FUNC("4v+otIIdjqg", Gnm::GnmDrawIndirect);
	LIB_FUNC("ffrNQOshows", Gnm::GnmComputeWaitOnAddress);
	LIB_FUNC("Fwvh++m9IQI", Gnm::GnmGetGpuCoreClockFrequency);
	LIB_FUNC("8lH54sfjfmU", Gnm::GnmDrawInitToDefaultContextState);
	LIB_FUNC("im2ZuItabu4", Gnm::GnmDrawInitToDefaultContextState400);
	LIB_FUNC("QhnyReteJ1M", Gnm::GnmDrawInitDefaultHardwareState175);
	LIB_FUNC("0H2vBYbTLHI", Gnm::GnmDrawInitDefaultHardwareState200);
	LIB_FUNC("yb2cRhagD1I", Gnm::GnmDrawInitDefaultHardwareState350);
	LIB_FUNC("HlTPoZ-oY7Y", Gnm::GnmDrawIndex);
	LIB_FUNC("GGsn7jMTxw4", Gnm::GnmDrawIndexAuto);
	LIB_FUNC("5uFKckiJYRM", Gnm::GnmSetPsShader350);
	// The Update setters rewrite a region reserved by the matching setter, with the same layout.
	LIB_FUNC("V31V01UiScY", Gnm::GnmSetVsShader);     // UpdateVsShader
	LIB_FUNC("4MgRw-bVNQU", Gnm::GnmSetPsShader);     // UpdatePsShader
	LIB_FUNC("mLVL7N7BVBg", Gnm::GnmSetPsShader350);  // UpdatePsShader350
	LIB_FUNC("nLM2i2+65hA", Gnm::GnmSetGsShader);     // UpdateGsShader
	LIB_FUNC("Kx-h-nWQJ8A", Gnm::GnmSetCsShaderWithModifier);
	LIB_FUNC("0BzLGljcwBo", Gnm::GnmDispatchDirect);
	LIB_FUNC("Z43vKp5k7r0", Gnm::GnmDispatchIndirect);
	LIB_FUNC("wED4ZXCFJT0", Gnm::GnmDispatchIndirectOnMec);
	LIB_FUNC("ED9-Fjr8Ta4", Gnm::GnmDrawIndexIndirect);
	LIB_FUNC("oYM+YzfCm2Y", Gnm::GnmDrawIndexOffset);
	LIB_FUNC("cFCp0NX8wf0", Gnm::GnmSetVgtControl);
	LIB_FUNC("jiItzS6+22g", Gnm::GnmInsertMarker); // SetMarker
	LIB_FUNC("W1Etj-jlW7Y", Gnm::GnmInsertMarker); // PushMarker
	LIB_FUNC("7qZVNgEu+SY", Gnm::GnmInsertPopMarker);
	LIB_FUNC("5udAm+6boVg", Gnm::GnmCreateWorkloadStream);
	LIB_FUNC("ihxrbsoSKWc", Gnm::GnmBeginWorkload);
	LIB_FUNC("Fa3x75OOLRA", Gnm::GnmEndWorkload);
	LIB_FUNC("nF6bFRUBRAU", Gnm::GnmDispatchInitDefaultHardwareState);
	LIB_FUNC("1qXLHIpROPE", Gnm::GnmInsertWaitFlipDone);
	LIB_FUNC("zwY0YV91TTI", Gnm::GnmSubmitCommandBuffers);
	LIB_FUNC("jRcI8VcgTz4", Gnm::GnmSubmitCommandBuffersForWorkload);
	LIB_FUNC("Ga6r7H6Y0RI", Gnm::GnmSubmitAndFlipCommandBuffersForWorkload);
	LIB_FUNC("xbxNatawohc", Gnm::GnmSubmitAndFlipCommandBuffers);
	LIB_FUNC("yvZ73uQUqrk", Gnm::GnmSubmitDone);
	LIB_FUNC("gObODli-OH8", Gnm::GnmRequestFlipAndSubmitDone);
	LIB_FUNC("b08AgtPlHPg", Gnm::GnmAreSubmitsAllowed);
	LIB_FUNC("29oKvKXzEZo", Gnm::GnmMapComputeQueue);
	LIB_FUNC("ArSg-TGinhk", Gnm::GnmUnmapComputeQueue);
	LIB_FUNC("bX5IbRvECXk", Gnm::GnmDingDong);
	LIB_FUNC("b0xyllnVY-I", Gnm::GnmAddEqEvent);
	LIB_FUNC("PVT+fuoS9gU", Gnm::GnmDeleteEqEvent);
	LIB_FUNC("UoYY0DWMC0U", Gnm::GnmGetEqEventType);
	LIB_FUNC("H7-fgvEutM0", Gnm::GnmGetEqTimeStamp);
	LIB_FUNC("jtkqXpAOY6w", Gnm::GnmSetGsRingSizes);
	LIB_FUNC("ln33zjBrfjk", Gnm::GnmGetTheTessellationFactorRingBufferBaseAddress);
	LIB_FUNC("FFVZcCu3zWU", Gnm::GnmGetOffChipTessellationBufferSize);
	LIB_FUNC("iBt3Oe00Kvc", Gnm::GnmFlushGarlic);
	LIB_FUNC("jg33rEKLfVs", Gnm::GnmIsUserPaEnabled);
	LIB_FUNC("RU74kek-N0c", Gnm::GnmLogicalCuMaskToPhysicalCuMask);
	// Debugging, profiling and statistics: accepted and ignored.
	LIB_FUNC("26PM5Mzl8zc", Gnm::GnmStubOk); // LogicalCuIndexToPhysicalCuIndex
	LIB_FUNC("iCO804ZgzdA", Gnm::GnmStubOk); // ValidateCommandBuffers
	LIB_FUNC("qpGITzPE+Zc", Gnm::GnmStubOk); // DebugHardwareStatus
	LIB_FUNC("0O3xxFaiObw", Gnm::GnmStubOk); // SetSpiEnableSqCounters
	LIB_FUNC("lN7Gk-p9u78", Gnm::GnmStubOk); // SetSpiEnableSqCountersForUnitInstance
	LIB_FUNC("+xuDhxlWRPg", Gnm::GnmStubOk); // SetupMipStatsReport
	LIB_FUNC("HHo1BAljZO8", Gnm::GnmStubOk); // DisableMipStatsReport
	LIB_FUNC("f85orjx7qts", Gnm::GnmStubOk); // RequestMipStatsReportAndReset
	LIB_FUNC("f33OrruQYbM", Gnm::RazorIsLoaded);
	LIB_FUNC("u9YKpRRHe-M", Gnm::GnmStubOk); // RazorCaptureImmediate
	LIB_FUNC("4UFagYlfuAM", Gnm::GnmStubOk); // RazorCaptureSinceLastFlip
	LIB_FUNC("9thMn+uB1is", Gnm::GnmStubOk); // RazorCaptureCommandBuffersOnlySinceLastFlip
	LIB_FUNC("xeTLfxVIQO4", Gnm::GnmStubOk); // RazorCaptureCommandBuffersOnlyImmediate
}

} // namespace Libs::Graphics::Gen4Driver

namespace Libs {

LIB_DEFINE(InitGnmDriver_1) {
	Graphics::Gen4Driver::InitGnmDriver_1(s);
}

} // namespace Libs
