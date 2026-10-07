#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_BDALAYOUT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_BDALAYOUT_H_

#include <cstdint>
#include <vector>

namespace Libs::Graphics {

// The BDA page table's layout, shared by the shaders (spirvEmitterMemory.cpp, DefineGetBdaPointer)
// and the backends. The table holds one 64-bit entry per 16 KB page of the guest's address space
// (the 1 TB below 2^40 and the 512 GB of extended memory, packed one after the other: 1.5 * 2^40
// bytes, 96 Mi pages), but games use few of those pages, so the entries live in blocks of 8192
// (64 KB, 128 MB of guest addresses), and only blocks with entries get one of the table's slots:
//
//   slots 0 and 1: the directory, one 64-bit value per block: 0, or the number of the block's slot
//                  counted from the first block slot (1 for the first block to get one)
//   slot 2: zeros; blocks without a slot read their entries there (it is never written)
//   slot 3 and up: the blocks that got one
//
// A page's entry is element ((directory[page >> 13] + DIRECTORY_SLOTS) << 13) | (page & 8191) of
// the table.
struct BdaLayout {
	static constexpr uint32_t PAGE_BITS       = 14;
	// BufferCache checks that this is the number of pages of the guest's address space.
	static constexpr uint64_t PAGE_COUNT      = ((uint64_t {1} << 40) + (uint64_t {1} << 39)) >> PAGE_BITS;
	static constexpr uint32_t BLOCK_BITS      = 13;
	static constexpr uint64_t BLOCK_ENTRIES   = uint64_t {1} << BLOCK_BITS;
	static constexpr uint64_t BLOCK_COUNT     = PAGE_COUNT >> BLOCK_BITS;
	static constexpr uint64_t DIRECTORY_SLOTS = (BLOCK_COUNT + BLOCK_ENTRIES - 1) / BLOCK_ENTRIES;
	static constexpr uint64_t SLOT_COUNT      = 512; // 32 MB
	static constexpr uint64_t TABLE_ELEMENTS  = SLOT_COUNT * BLOCK_ENTRIES;
	static_assert((PAGE_COUNT & (BLOCK_ENTRIES - 1)) == 0, "the table holds whole blocks");

	// The pages from `page` to the end of its block.
	static constexpr uint64_t PagesLeftInBlock(uint64_t page) {
		return BLOCK_ENTRIES - (page & (BLOCK_ENTRIES - 1));
	}
};

// Which blocks of the table have a slot (CPU side of BdaLayout).
class BdaBlocks {
public:
	// The table element of `page`'s entry, or 0 if its block has no slot.
	[[nodiscard]] uint64_t EntryElement(uint64_t page) const {
		const auto value = m_directory[page >> BdaLayout::BLOCK_BITS];
		return value == 0 ? 0
		                  : ((value + BdaLayout::DIRECTORY_SLOTS) << BdaLayout::BLOCK_BITS) |
		                        (page & (BdaLayout::BLOCK_ENTRIES - 1));
	}

	// Gives `page`'s block a slot if it has none. Returns false when the table is full; sets
	// `*directory_value` to the value to store at element `page >> BLOCK_BITS` of the table when
	// the block got its slot now (else 0).
	[[nodiscard]] bool Allocate(uint64_t page, uint64_t* directory_value) {
		*directory_value = 0;
		auto& value      = m_directory[page >> BdaLayout::BLOCK_BITS];
		if (value != 0) {
			return true;
		}
		if (BdaLayout::DIRECTORY_SLOTS + m_used + 1 >= BdaLayout::SLOT_COUNT) {
			return false;
		}
		value            = static_cast<uint16_t>(++m_used);
		*directory_value = value;
		return true;
	}

	[[nodiscard]] uint64_t Used() const noexcept { return m_used; }

private:
	std::vector<uint16_t> m_directory = std::vector<uint16_t>(BdaLayout::BLOCK_COUNT);
	uint64_t              m_used      = 0;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_BDALAYOUT_H_
