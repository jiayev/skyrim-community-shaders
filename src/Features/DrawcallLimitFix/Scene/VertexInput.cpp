#include "VertexInput.h"

namespace DCLF
{
	namespace
	{
		// BSGraphics::Vertex::Attribute
		enum Attribute : std::uint32_t
		{
			kPosition,
			kTexcoord0,
			kTexcoord1,
			kNormal,
			kBinormal,
			kColor,
			kSkinning,
			kLandData,
			kEyeData,
			kInstanceData,
		};

		constexpr std::uint32_t kNoSlot = ~0u;

		// Stream 0 when the attribute's flag is set there (bit 44 + a), else stream 1 (bit 54 + a).
		std::uint32_t SlotOf(std::uint64_t a_key, std::uint32_t a_attribute)
		{
			if (a_key & (1ull << (44 + a_attribute)))
				return 0;
			if (a_key & (1ull << (54 + a_attribute)))
				return 1;
			return kNoSlot;
		}

		std::uint32_t OffsetOf(std::uint64_t a_key, std::uint32_t a_attribute)
		{
			return static_cast<std::uint32_t>((a_key >> (4 * a_attribute + 4)) & 0xF) * 4;
		}
	}

	void AddVertexAttribute(std::uint64_t& a_mask, std::uint32_t a_attribute)
	{
		a_mask |= (1ull << (44 + a_attribute)) | (1ull << (54 + a_attribute)) | (0b1111ull << (4 * a_attribute + 4));
	}

	VertexElements BuildVertexElements(std::uint64_t a_key)
	{
		VertexElements out;
		auto add = [&](const char* a_semantic, std::uint32_t a_index, DXGI_FORMAT a_format, std::uint32_t a_slot, std::uint32_t a_offset, bool a_perInstance = false) {
			out.elements[out.count++] = { a_semantic, a_index, a_format, a_slot, a_offset, a_perInstance };
		};
		auto attribute = [&](std::uint32_t a_attribute) { return SlotOf(a_key, a_attribute); };

		// The position element is always present (slot ~0 when the key has no position).
		add("POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, attribute(kPosition), 0);
		if (const auto slot = attribute(kTexcoord0); slot != kNoSlot)
			add("TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT, slot, OffsetOf(a_key, kTexcoord0));
		if (const auto slot = attribute(kTexcoord1); slot != kNoSlot)
			add("TEXCOORD", 1, DXGI_FORMAT_R16G16B16A16_FLOAT, slot, OffsetOf(a_key, kTexcoord1));
		if (const auto slot = attribute(kNormal); slot != kNoSlot)
			add("NORMAL", 0, DXGI_FORMAT_R8G8B8A8_UNORM, slot, OffsetOf(a_key, kNormal));
		if (const auto slot = attribute(kBinormal); slot != kNoSlot)
			add("BINORMAL", 0, DXGI_FORMAT_R8G8B8A8_UNORM, slot, OffsetOf(a_key, kBinormal));
		if (const auto slot = attribute(kColor); slot != kNoSlot)
			add("COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, slot, OffsetOf(a_key, kColor));
		if (const auto slot = attribute(kSkinning); slot != kNoSlot) {
			const auto offset = OffsetOf(a_key, kSkinning);
			add("BLENDWEIGHT", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, slot, offset);
			add("BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UNORM, slot, offset + 8);
		}
		if (const auto slot = attribute(kLandData); slot != kNoSlot) {
			const auto offset = OffsetOf(a_key, kLandData);
			add("TEXCOORD", 2, DXGI_FORMAT_R8G8B8A8_UNORM, slot, offset);
			add("TEXCOORD", 3, DXGI_FORMAT_R8G8B8A8_UNORM, slot, offset + 4);
		}
		if (const auto slot = attribute(kEyeData); slot != kNoSlot)
			add("TEXCOORD", 2, DXGI_FORMAT_R32_FLOAT, slot, OffsetOf(a_key, kEyeData));
		if (const auto slot = attribute(kInstanceData); slot != kNoSlot) {
			const auto offset = OffsetOf(a_key, kInstanceData);
			for (std::uint32_t i = 0; i < 4; ++i)
				add("TEXCOORD", 4 + i, DXGI_FORMAT_R16G16B16A16_FLOAT, slot, offset + 8 * i, true);
		}
		return out;
	}
}
