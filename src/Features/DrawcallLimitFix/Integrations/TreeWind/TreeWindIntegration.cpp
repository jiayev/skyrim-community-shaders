#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadingFields.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Integrations/TreeWind/TreeWind.h"
#include "Features/TreeWind.h"

#include <bit>
#include <unordered_map>

namespace DCLF::Integrations::TreeWind
{
	namespace
	{
		struct Captured
		{
			RE::BSGeometry* geometry = nullptr;
			TreeShapes::Kind kind = TreeShapes::Kind::None;
			winrt::com_ptr<ID3D11ShaderResourceView> view;
			GpuTextures::Binding binding;
			bool dirty = true;
		};
		std::unordered_map<std::uint32_t, Captured> captured;
		std::uint32_t generation = ~0u;
		bool wasLoaded = false;
		bool treeModSupport = false;

		void Capture(Api::ShadingFieldCapture& a_capture)
		{
			auto& wind = globals::features::treeWind;
			if (!wind.loaded) {
				for (const auto& [slot, value] : captured)
					a_capture.Put(slot, {});
				captured.clear();
				wasLoaded = false;
				return;
			}

			const auto classify = [&](std::uint32_t slot) {
				auto* geometry = slot < a_capture.objectGeometry.size() ? a_capture.objectGeometry[slot] : nullptr;
				const auto kind = wind.Classify(geometry);
				if (kind == TreeShapes::Kind::None) {
					if (captured.erase(slot))
						a_capture.Put(slot, {});
					return;
				}
				auto& entry = captured[slot];
				entry.geometry = geometry;
				entry.kind = kind;
				entry.dirty = true;
			};
			if (!wasLoaded || treeModSupport != wind.settings.TreeModSupport) {
				for (std::uint32_t slot = 0; slot < a_capture.objectGeometry.size(); ++slot)
					classify(slot);
			} else {
				for (const auto slot : a_capture.changedObjects)
					classify(slot);
			}
			wasLoaded = true;
			treeModSupport = wind.settings.TreeModSupport;

			auto& textures = GpuTextures::Get();
			const bool changedDevice = generation != textures.Generation();
			generation = textures.Generation();
			// Trunk cards establish each LOD block's trees before crown vertices use them.
			for (const auto kind : { TreeShapes::Kind::Trunk, TreeShapes::Kind::Crown }) {
				for (auto it = captured.begin(); it != captured.end();) {
					const auto slot = it->first;
					auto& entry = it->second;
					if (slot >= a_capture.objectGeometry.size() || a_capture.objectGeometry[slot] != entry.geometry) {
						a_capture.Put(slot, {});
						it = captured.erase(it);
						continue;
					}
					++it;
					if (entry.kind != kind)
						continue;
					auto* view = wind.CaptureBendWeights(entry.geometry, kind);
					if (entry.dirty || changedDevice || entry.view.get() != view || entry.binding.index == GpuTextures::kInvalid || GpuTextures::Superseded(entry.binding.owner.get())) {
						entry.view.copy_from(view);
						entry.binding = view ? textures.ResolveBinding(view, 184) : GpuTextures::Binding{};
						const std::uint32_t descriptor = entry.binding.index == GpuTextures::kInvalid ? 0 : entry.binding.index + 1;
						const std::array<float, 4> value{ std::bit_cast<float>(descriptor), 0, 0, 0 };
						a_capture.Put(slot, value, textures.Seal({ entry.binding.owner }));
						entry.dirty = false;
					}
				}
			}
		}

		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterShadingField({ .name = "tree LOD wind", .field = Api::ShadingRowField::LodTreeWind, .capture = &Capture });
			a_context.DeclarePerDrawBuffer({ .name = "TreeWindCB", .stage = Api::ShaderStage::Vertex, .constantRegister = 3 });
		}
	}

	Api::Feature Integration()
	{
		return { .name = "tree LOD wind", .install = &Install, .status = [] { return FeatureStatus(globals::features::treeWind); } };
	}
}
