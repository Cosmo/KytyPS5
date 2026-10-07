#include "graphics/host_gpu/renderer/cache/bdaPageTable.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"

#include <algorithm>
#include <bit>
#include <cinttypes>
#include <fmt/format.h>
#include <vector>

namespace Libs::Graphics {

BdaPageTable::BdaPageTable(GraphicContext& graphics, CommandScheduler& scheduler,
                           StreamBuffer& staging, BufferCache& buffer_cache, uint64_t /*page_count*/)
    : m_scheduler(scheduler), m_staging(staging),
      m_page_table(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags,
                   BdaLayout::TABLE_ELEMENTS * sizeof(vk::DeviceAddress)),
      m_fault_manager(graphics, scheduler, buffer_cache) {
	m_page_table.SetName("BDA Page Table Buffer");
}

void BdaPageTable::EnsureCleared() {
	if (m_cleared) {
		return;
	}
	m_page_table.Fill(0, m_page_table.Size(), 0);
	m_cleared = true;
}

void BdaPageTable::Write(uint64_t offset, const void* data, uint64_t size) {
	const auto* bytes = static_cast<const uint8_t*>(data);
	while (size != 0) {
		const auto chunk  = std::min(size, m_staging.Size());
		// Copy waits for ring reuse and flushes the fresh host data before submission.
		const auto source = m_staging.Copy(bytes, chunk, 4);
		EXIT_IF(offset > m_page_table.Size() || chunk > m_page_table.Size() - offset);
		m_scheduler.EndRendering();
		const auto command = m_scheduler.Current().Handle();
		vk::BufferMemoryBarrier2 before {};
		before.srcStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
		before.srcAccessMask       = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
		before.dstStageMask        = vk::PipelineStageFlagBits2::eTransfer;
		before.dstAccessMask       = vk::AccessFlagBits2::eTransferWrite;
		before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.buffer              = m_page_table.Handle();
		before.offset              = offset;
		before.size                = chunk;
		vk::DependencyInfo dependency {};
		dependency.dependencyFlags          = vk::DependencyFlagBits::eByRegion;
		dependency.bufferMemoryBarrierCount = 1;
		dependency.pBufferMemoryBarriers    = &before;
		command.pipelineBarrier2(dependency);
		const vk::BufferCopy copy {source, offset, chunk};
		command.copyBuffer(m_staging.Handle(), m_page_table.Handle(), 1, &copy);
		auto after          = before;
		after.srcStageMask  = vk::PipelineStageFlagBits2::eTransfer;
		after.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
		after.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
		after.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
		dependency.pBufferMemoryBarriers = &after;
		command.pipelineBarrier2(dependency);
		bytes += chunk;
		offset += chunk;
		size -= chunk;
	}
}

void BdaPageTable::Map(uint64_t first_page, uint64_t page_count, uint32_t page_bits,
                       const Buffer& buffer) {
	EnsureCleared();
	std::vector<vk::DeviceAddress> addresses;
	for (uint64_t i = 0; i < page_count;) {
		const uint64_t page            = first_page + i;
		const uint64_t count           = std::min(page_count - i, BdaLayout::PagesLeftInBlock(page));
		uint64_t       directory_value = 0;
		if (!m_blocks.Allocate(page, &directory_value)) {
			EXIT("BDA page table: all %" PRIu64 " slots in use\n", BdaLayout::SLOT_COUNT);
		}
		if (directory_value != 0) {
			Write((page >> BdaLayout::BLOCK_BITS) * sizeof(uint64_t), &directory_value,
			      sizeof(directory_value));
			if (std::has_single_bit(m_blocks.Used())) {
				Log::WriteToConsoleAndLog(fmt::format(
				    "BDA page table: {} blocks of guest addresses in use ({} MB table)\n",
				    m_blocks.Used(), m_page_table.Size() >> 20u));
			}
		}
		addresses.clear();
		for (uint64_t j = i; j < i + count; ++j) {
			addresses.push_back(buffer.BufferDeviceAddress() + (j << page_bits));
		}
		Write(m_blocks.EntryElement(page) * sizeof(vk::DeviceAddress), addresses.data(),
		      addresses.size() * sizeof(vk::DeviceAddress));
		i += count;
	}
}

void BdaPageTable::Unmap(uint64_t first_page, uint64_t page_count) {
	EnsureCleared();
	for (uint64_t i = 0; i < page_count;) {
		const uint64_t page  = first_page + i;
		const uint64_t count = std::min(page_count - i, BdaLayout::PagesLeftInBlock(page));
		// Blocks without a slot read the zero slot, which is never written.
		if (const auto element = m_blocks.EntryElement(page); element != 0) {
			m_page_table.Fill(element * sizeof(vk::DeviceAddress),
			                  count * sizeof(vk::DeviceAddress), 0);
		}
		i += count;
	}
}

} // namespace Libs::Graphics
