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
		const auto offset = m_staging.Copy(bytes, chunk, 4);
		m_page_table.CopyFrom(m_scheduler.Current(), m_staging, offset, address, chunk,
		                      vk::AccessFlagBits::eHostWrite);
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
