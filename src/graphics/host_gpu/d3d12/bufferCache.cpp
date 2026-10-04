#include "graphics/host_gpu/d3d12/bufferCache.h"

#include "common/assert.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>

namespace Libs::Graphics {

void BufferCache::FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds) {
	if ((vaddr & 3u) != 0 || size == 0 || (size & 3u) != 0 || size > UINT64_MAX - vaddr) {
		EXIT("BufferCache: fill range must be dword aligned\n");
	}
	if (is_gds) {
		if (vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - vaddr) {
			EXIT("BufferCache: GDS fill range is out of bounds\n");
		}
		auto* destination = reinterpret_cast<uint32_t*>(m_gds_buffer.Mapped().data() + vaddr);
		std::fill(destination, destination + size / sizeof(uint32_t), value);
		return;
	}
	if (vaddr == 0) {
		EXIT("BufferCache: invalid fill memory address\n");
	}
	// The write goes through the guest mapping, so write faults reach the memory tracker.
	auto* destination = reinterpret_cast<uint32_t*>(vaddr);
	std::fill(destination, destination + size / sizeof(uint32_t), value);
}

void BufferCache::CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds, bool src_gds) {
	const auto gds_size = m_gds_buffer.Size();
	if ((!dst_gds && dst_vaddr == 0) || (!src_gds && src_vaddr == 0) || size == 0 || ((dst_gds || src_gds) && ((dst_vaddr | src_vaddr | size) & 3u) != 0) ||
	    size > UINT64_MAX - dst_vaddr || size > UINT64_MAX - src_vaddr || (dst_gds && src_gds) || (dst_gds && (dst_vaddr > gds_size || size > gds_size - dst_vaddr)) ||
	    (src_gds && (src_vaddr > gds_size || size > gds_size - src_vaddr))) {
		EXIT("BufferCache: invalid copy range, src=0x%016" PRIx64 " dst=0x%016" PRIx64 " size=0x%016" PRIx64 " src_gds=%d dst_gds=%d\n", src_vaddr, dst_vaddr, size,
		     static_cast<int>(src_gds), static_cast<int>(dst_gds));
	}
	auto* const gds = m_gds_buffer.Mapped().data();
	auto*       dst = dst_gds ? gds + dst_vaddr : reinterpret_cast<uint8_t*>(dst_vaddr);
	const auto* src = src_gds ? gds + src_vaddr : reinterpret_cast<const uint8_t*>(src_vaddr);
	std::memmove(dst, src, size);
}

} // namespace Libs::Graphics
