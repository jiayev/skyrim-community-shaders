#pragma once

#include <array>
#include <cstdint>

#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"

namespace DCLF::SkinPartitions
{
	// NiSkinPartition::Unk_25 (AE 140d43a10) draws partition i when this table holds a non-zero byte at
	// (LODMode.index + LODMode.singleLevel * 4) * 3 + the partition's LOD byte (Partition+0x42). Read from
	// AE 1.6.1170 at 0x14202a030, where nothing writes it: level n draws the LOD bytes below n, and
	// single-level n draws LOD byte n alone.
	inline constexpr std::array<std::uint8_t, 24> kPartitionLodTable{ 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };

	/**
	 * @brief Which skin partitions the engine draws for this LOD row (bit i = partition i): BSDismemberSkinInstance::Unk_25 (AE
	 * 140d31f40) skips a partition whose flag (Data byte 0) is clear, then NiSkinPartition::Unk_25 applies the table. 0 when it draws
	 * none.
	 */
	inline std::uint32_t Mask(const RE::NiSkinInstance& a_skin, std::uint32_t a_lodRow)
	{
		const auto* partition = a_skin.skinPartition.get();
		if (!partition || a_lodRow >= 8)
			return 0;
		static const REL::Relocation<const RE::NiRTTI*> dismemberSkinInstance{ RE::BSDismemberSkinInstance::Ni_RTTI };
		const RE::BSDismemberSkinInstance::Data* shown = a_skin.GetRTTI() == dismemberSkinInstance.get() ?
		                                                     static_cast<const RE::BSDismemberSkinInstance&>(a_skin).GetRuntimeData().partitions :
		                                                     nullptr;
		std::uint32_t mask = 0;
		for (std::uint32_t i = 0; i < partition->numPartitions && i < kMaxSkinPartitions; ++i) {
			if (shown && !shown[i].editorVisible)
				continue;
			const std::uint32_t lodByte = partition->partitions[i].pad42 & 0xFF;
			if (lodByte <= 2 && kPartitionLodTable[a_lodRow * 3 + lodByte])
				mask |= 1u << i;
		}
		return mask;
	}

	/** @brief Mask from the geometry's record: its partitions' LOD bytes and a dismember skin's shown flags. */
	inline std::uint32_t Mask(const SceneCapture::GeometryRecord& a_geometry, std::uint32_t a_lodRow)
	{
		if (!a_geometry.skinPartition || a_lodRow >= 8)
			return 0;
		// A dismember skin's shown flags (empty otherwise).
		const auto& shown = a_geometry.shown;
		std::uint32_t mask = 0;
		for (std::uint32_t i = 0; i < a_geometry.partitions.size() && i < kMaxSkinPartitions; ++i) {
			if (i < shown.size() && !shown[i])
				continue;
			const std::uint32_t lodByte = a_geometry.partitions[i].lodByte;
			if (lodByte <= 2 && kPartitionLodTable[a_lodRow * 3 + lodByte])
				mask |= 1u << i;
		}
		return mask;
	}

	/** @brief Tables::skinPartitions: 0 for none or a single drawn partition, kNoPartitions when row 3 draws none, else the mask. */
	inline std::uint16_t Of(const SceneCapture::GeometryRecord& a_geometry)
	{
		if (!a_geometry.skin || !a_geometry.skinPartition)
			return 0;
		// Row 3 (cumulative over levels); a LOD skin's draw narrows it (LodOf).
		const std::uint32_t mask = Mask(a_geometry, 3);
		return static_cast<std::uint16_t>(!mask ? kNoPartitions : a_geometry.partitions.size() > 1 ? mask : 0u);
	}

	/** @brief Tables::skinLodPartitions for a geometry: a kMeshLOD skin's masks per level (byte L: row L), 0 for any other. */
	inline std::uint32_t LodOf(const SceneCapture::LeafView& a_leaf)
	{
		if (!a_leaf || !a_leaf.geometry->skin || !a_leaf.geometry->skinPartition || !(a_leaf.node->flags & static_cast<std::uint32_t>(RE::NiAVObject::Flag::kMeshLOD)) ||
			!a_leaf.property || !a_leaf.property->fadeNode)
			return 0;
		std::uint32_t word = 0;
		for (std::uint32_t level = 0; level < 4; ++level)
			word |= (Mask(*a_leaf.geometry, level) & 0xFFu) << (8 * level);
		return word;
	}
}
