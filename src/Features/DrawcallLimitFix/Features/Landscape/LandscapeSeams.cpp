// Landscape seam resources are per quad, captured into the object shading row without splitting terrain batches.
#include "EngineFixes/LandscapeSeamsFix.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadingFields.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Features/Landscape/Landscape.h"

#include <array>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
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
		};
		std::unordered_set<std::uint32_t> candidates;
		std::unordered_map<std::uint32_t, Captured> captured;
		std::uint32_t generation = ~0u;

		void CaptureSeams(Api::ShadingFieldCapture& a_capture)
		{
			auto& textures = GpuTextures::Get();
			const bool changedDevice = generation != textures.Generation();
			generation = textures.Generation();
			const std::unordered_set<std::uint32_t> reset(a_capture.changedObjects.begin(), a_capture.changedObjects.end());
			for (const auto slot : a_capture.changedObjects)
				candidates.insert(slot);
			for (auto it = candidates.begin(); it != candidates.end();) {
				const auto slot = *it;
				auto* geometry = slot < a_capture.objectGeometry.size() ? a_capture.objectGeometry[slot] : nullptr;
				auto* property = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
				const bool land = property && property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape);
				auto source = land ? LandscapeSeamsFix::GetInstance().CaptureResources(geometry) : nullptr;
				if (!source) {
					if (captured.erase(slot))
						a_capture.Put(slot, {});
					if (!land)
						it = candidates.erase(it);
					else
						++it;
					continue;
				}
				++it;
				auto [entry, inserted] = captured.try_emplace(slot);
				auto& previous = entry->second;
				std::array<ID3D11ShaderResourceView*, LandscapeSeamsFix::NumPSTextures> views{};
				views[0] = source->weights.get();
				views[1] = source->data.get();
				for (std::size_t extra = 0; extra < source->extras.size(); ++extra)
					for (std::size_t t = 0; t < source->extras[extra].size(); ++t)
						if (const auto& texture = source->extras[extra][t]; texture && texture->rendererTexture)
							views[2 + extra * LandscapeSeamsFix::TexturesPerExtra + t] = texture->rendererTexture->resourceView;
				bool changed = inserted || changedDevice || previous.source != source || previous.views != views || reset.contains(slot);
				for (std::size_t t = 0; t < previous.bindings.size(); ++t) {
					const auto& binding = previous.bindings[t];
					// Null descriptors have device owners, not imports that Superseded can inspect.
					changed |= binding.index == GpuTextures::kInvalid || (previous.views[t] && GpuTextures::Superseded(binding.owner.get()));
				}
				if (!changed)
					continue;
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
				a_capture.Put(slot, value, textures.Seal(owners));
			}
		}
	}

	void InstallSeams(Api::InstallContext& a_context)
	{
		a_context.RegisterShadingField({ .name = "landscape seams", .field = Api::ShadingRowField::LandscapeSeams, .capture = &CaptureSeams });
	}
}
