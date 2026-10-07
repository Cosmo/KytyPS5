#include "graphics/host_gpu/renderer/cache/bdaPageTable.h"

#include "graphics/host_gpu/renderer/commandScheduler.h"

#include <algorithm>
#include <vector>

namespace Libs::Graphics {

BdaPageTable::BdaPageTable(GraphicContext& graphics, CommandScheduler& scheduler,
                           StreamBuffer& staging, BufferCache& buffer_cache, uint64_t page_count)
    : m_scheduler(scheduler), m_staging(staging),
      m_page_table(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags,
                   page_count * sizeof(vk::DeviceAddress)),
      m_fault_manager(graphics, scheduler, buffer_cache) {
	m_page_table.SetName("BDA Page Table Buffer");
}

void BdaPageTable::Clear() {
	m_page_table.Fill(0, m_page_table.Size(), 0);
	auto* faults = m_fault_manager.GetFaultBuffer();
	faults->Fill(0, faults->Size(), 0);
}

void BdaPageTable::Map(uint64_t first_page, uint64_t page_count, uint32_t page_bits,
                       const Buffer& buffer) {
	std::vector<vk::DeviceAddress> addresses;
	addresses.reserve(page_count);
	for (uint64_t i = 0; i < page_count; ++i) {
		addresses.push_back(buffer.BufferDeviceAddress() + (i << page_bits));
	}
	const auto* bytes   = reinterpret_cast<const uint8_t*>(addresses.data());
	uint64_t    address = first_page * sizeof(vk::DeviceAddress);
	uint64_t    size    = addresses.size() * sizeof(vk::DeviceAddress);
	while (size != 0) {
		const auto chunk  = std::min(size, m_staging.Size());
		// Copy waits for ring reuse and flushes the fresh host data before submission.
		const auto offset = m_staging.Copy(bytes, chunk, 4);
		EXIT_IF(address > m_page_table.Size() || chunk > m_page_table.Size() - address);
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
		before.offset              = address;
		before.size                = chunk;
		vk::DependencyInfo dependency {};
		dependency.dependencyFlags          = vk::DependencyFlagBits::eByRegion;
		dependency.bufferMemoryBarrierCount = 1;
		dependency.pBufferMemoryBarriers    = &before;
		command.pipelineBarrier2(dependency);
		const vk::BufferCopy copy {offset, address, chunk};
		command.copyBuffer(m_staging.Handle(), m_page_table.Handle(), 1, &copy);
		auto after          = before;
		after.srcStageMask  = vk::PipelineStageFlagBits2::eTransfer;
		after.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
		after.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
		after.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
		dependency.pBufferMemoryBarriers = &after;
		command.pipelineBarrier2(dependency);
		bytes += chunk;
		address += chunk;
		size -= chunk;
	}
}

void BdaPageTable::Unmap(uint64_t first_page, uint64_t page_count) {
	m_page_table.Fill(first_page * sizeof(vk::DeviceAddress),
	                  page_count * sizeof(vk::DeviceAddress), 0);
}

} // namespace Libs::Graphics
