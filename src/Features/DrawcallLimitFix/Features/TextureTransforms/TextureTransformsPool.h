#pragma once

// The frame-value pool's parts its diagnostics share: the engine read of an entry's value (its sampling's and its parity's),
// and the parity and stats line it calls (Diagnostics/TextureTransformsParity.cpp). Included by TextureTransformsPool.cpp and the
// parity only.

#include "Features/DrawcallLimitFix/Features/TextureTransforms/TextureTransforms.h"
#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameValuePools.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace DCLF::Features::TextureTransforms
{
	// A read only while this producer's Kick is the newest: its list's materials are held by the publication its frame installed, which
	// is let go no earlier than the next frame's start (released at that frame's Present, after the window closed).
	inline bool Readable(const Api::FrameValuePoolProducer& a_producer, EngineReadWindow::Lease& a_lease) { return a_lease && a_producer.Newest(); }

	// TexcoordOffset from the frame's buffer; a declared frame variable through its components' read (the port's evaluate, bit for bit:
	// one product, or the bits copied), as data.
	inline void Read(const TextureTransformList::Entry& a_entry, std::size_t a_buffer, float (&a_values)[4])
	{
		const auto* material = a_entry.material;
		if (const auto* components = a_entry.components) {
			const auto* bytes = reinterpret_cast<const std::byte*>(material);
			for (std::size_t c = 0; c < 4; ++c) {
				const auto& component = components[c];
				std::memcpy(&a_values[c], bytes + component.source, sizeof(float));
				if (component.scale != Api::MaterialFrameComponent::kNoScale) {
					float scale;
					std::memcpy(&scale, bytes + component.scale, sizeof(scale));
					a_values[c] = scale * a_values[c];
				}
			}
			return;
		}
		a_values[0] = material->texCoordOffset[a_buffer].x;
		a_values[1] = material->texCoordOffset[a_buffer].y;
		a_values[2] = material->texCoordScale[a_buffer].x;
		a_values[3] = material->texCoordScale[a_buffer].y;
	}

	// Diagnostics/TextureTransformsParity.cpp, the frame's producer (one at a time in frame order) at its sample's end: the parity on its
	// parity frames, against the rows it sends and the materials each entry was last read from; the line's counts every frame.
	void CheckPoolParity(const Api::FrameValuePoolProducer& a_producer, const TextureTransformList* a_list, std::size_t a_count, std::size_t a_buffer,
		const std::vector<TextureTransformRow>& a_out, const std::vector<const RE::BSShaderMaterial*>& a_materials);
	void CountPoolLine(std::size_t a_count, std::uint64_t a_changed, std::uint64_t a_refused, std::size_t a_variables, std::uint64_t a_variablesChanged);
}
