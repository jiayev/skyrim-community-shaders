// Landscape seam resources are per quad, captured into the object shading row without splitting terrain batches.
#include "EngineFixes/LandscapeSeamsFix.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadingFields.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Features/Landscape/Landscape.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace DCLF::Features::Landscape
{
	namespace
	{
		struct Captured
		{
			std::shared_ptr<const LandscapeSeamsFix::Resources> source;
			std::array<GpuTextures::Binding, LandscapeSeamsFix::NumPSTextures> bindings;
			std::array<ID3D11ShaderResourceView*, LandscapeSeamsFix::NumPSTextures> views{};
			const RE::BSGeometry* geometry = nullptr;
		};
		using Views = std::array<ID3D11ShaderResourceView*, LandscapeSeamsFix::NumPSTextures>;
		std::unordered_set<std::uint32_t> candidates;
		std::unordered_map<std::uint32_t, Captured> captured;
		std::uint32_t generation = ~0u;
		// The seams fix's change count and the relocation log's cursor at the last capture. While neither moves, a captured quad
		// changes only through its slot, its geometry or its textures' views, so the frame checks those alone.
		std::uint64_t seamsChanges = ~0ull;
		std::uint64_t relocationCursor = 0;
		std::vector<ID3D11ShaderResourceView*> moved;
		std::vector<std::uint32_t> dirty;  // the frame's slots to capture again

		Views ViewsOf(const LandscapeSeamsFix::Resources& a_source)
		{
			Views views{};
			views[0] = a_source.weights.get();
			views[1] = a_source.data.get();
			for (std::size_t extra = 0; extra < a_source.extras.size(); ++extra)
				for (std::size_t t = 0; t < a_source.extras[extra].size(); ++t)
					if (const auto& texture = a_source.extras[extra][t]; texture && texture->rendererTexture)
						views[2 + extra * LandscapeSeamsFix::TexturesPerExtra + t] = texture->rendererTexture->resourceView;
			return views;
		}

		/** @brief Whether a captured quad's row still holds, when the seams fix changed nothing and the relocation log is complete. */
		bool Unchanged(const Captured& a_previous, const RE::BSGeometry* a_geometry)
		{
			if (a_previous.geometry != a_geometry || !a_previous.source || ViewsOf(*a_previous.source) != a_previous.views)
				return false;
			for (std::size_t t = 0; t < a_previous.bindings.size(); ++t) {
				// An unresolved binding is tried again every frame.
				if (a_previous.bindings[t].index == GpuTextures::kInvalid)
					return false;
				// A view in the relocation log since the last capture: its import was superseded.
				if (a_previous.views[t] && !moved.empty() && std::binary_search(moved.begin(), moved.end(), a_previous.views[t]))
					return false;
			}
			return true;
		}

		const RE::BSGeometry* GeometryOf(const Api::ShadingFieldCapture& a_capture, std::uint32_t a_slot)
		{
			return a_slot < a_capture.objectGeometry.size() ? a_capture.objectGeometry[a_slot] : nullptr;
		}

		/**
		 * @brief One candidate's row, put when it changed (a_forced: its slot changed). Returns false when the slot is no land quad, which
		 * leaves the candidates.
		 */
		bool CaptureSlot(Api::ShadingFieldCapture& a_capture, std::uint32_t a_slot, bool a_changedDevice, bool a_forced)
		{
			auto& textures = GpuTextures::Get();
			auto* geometry = a_slot < a_capture.objectGeometry.size() ? a_capture.objectGeometry[a_slot] : nullptr;
			auto* property = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
			const bool land = property && property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape);
			auto source = land ? LandscapeSeamsFix::GetInstance().CaptureResources(geometry) : nullptr;
			if (!source) {
				if (captured.erase(a_slot))
					a_capture.Put(a_slot, {});
				return land;
			}
			auto [entry, inserted] = captured.try_emplace(a_slot);
			auto& previous = entry->second;
			const Views views = ViewsOf(*source);
			previous.geometry = geometry;
			bool changed = inserted || a_changedDevice || previous.source != source || previous.views != views || a_forced;
			for (std::size_t t = 0; t < previous.bindings.size(); ++t) {
				const auto& binding = previous.bindings[t];
				// Null descriptors have device owners, not imports that Superseded can inspect.
				changed |= binding.index == GpuTextures::kInvalid || (previous.views[t] && GpuTextures::Superseded(binding.owner.get()));
			}
			if (!changed)
				return true;
			std::array<std::uint32_t, LandscapeSeamsFix::NumPSTextures> indices{};
			std::vector<std::shared_ptr<const void>> owners;
			bool valid = views[0] && views[1];
			for (std::size_t t = 0; t < views.size(); ++t) {
				auto& binding = previous.bindings[t];
				binding = textures.ResolveBinding(views[t], 64 + LandscapeSeamsFix::FirstPSTexture + static_cast<std::uint32_t>(t));
				valid &= binding.index != GpuTextures::kInvalid;
				indices[t] = binding.index + 1;  // Zero is an unpopulated row; descriptor zero is still usable.
				owners.push_back(binding.owner);
			}
			previous.source = std::move(source);
			previous.views = views;
			std::array<float, Api::kShadingFieldValueFloats> value{};
			static_assert(sizeof(indices) <= sizeof(value));
			if (valid)
				std::memcpy(value.data(), indices.data(), sizeof(indices));
			a_capture.Put(a_slot, value, textures.Seal(owners));
			return true;
		}

		void CaptureSeams(Api::ShadingFieldCapture& a_capture)
		{
			ZoneScopedN("CS.DCLF.Landscape.Seams");
			ZoneValue(candidates.size());
			auto& textures = GpuTextures::Get();
			// Both read before any quad: a change after them is seen at the next capture.
			const std::uint64_t changes = LandscapeSeamsFix::GetInstance().ResourceChanges();
			moved.clear();
			const bool relocationsComplete = textures.RelocatedViews(relocationCursor, moved);
			std::sort(moved.begin(), moved.end());
			const bool changedDevice = generation != textures.Generation();
			generation = textures.Generation();
			const bool full = changedDevice || !relocationsComplete || std::exchange(seamsChanges, changes) != changes;
			const std::unordered_set<std::uint32_t> reset(a_capture.changedObjects.begin(), a_capture.changedObjects.end());
			for (const auto slot : a_capture.changedObjects)
				candidates.insert(slot);
			if (full) {
				for (auto it = candidates.begin(); it != candidates.end();) {
					if (CaptureSlot(a_capture, *it, changedDevice, reset.contains(*it)))
						++it;
					else
						it = candidates.erase(it);
				}
				return;
			}
			// Nothing changed the seams fix's quads: a land quad still without resources stays so. The changed slots are captured, and the
			// captured quads whose row no longer holds.
			dirty.assign(a_capture.changedObjects.begin(), a_capture.changedObjects.end());
			for (const auto& [slot, previous] : captured)
				if (!Unchanged(previous, GeometryOf(a_capture, slot)))
					dirty.push_back(slot);
			std::sort(dirty.begin(), dirty.end());
			dirty.erase(std::unique(dirty.begin(), dirty.end()), dirty.end());
			for (const auto slot : dirty)
				if (!CaptureSlot(a_capture, slot, false, reset.contains(slot)))
					candidates.erase(slot);
		}
	}

	void InstallSeams(Api::InstallContext& a_context)
	{
		a_context.RegisterShadingField({ .name = "landscape seams", .field = Api::ShadingRowField::LandscapeSeams, .capture = &CaptureSeams });
	}
}
