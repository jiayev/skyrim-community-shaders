// CS_DCLF_PERSISTENT_PARITY: the shadow dependency index kept by changes against a reconstruction from the same tables (the walk makes the
// sets whole again on the passes Policy::RebuildShadowSets names).

#include "SceneObservers.h"
#include "SceneState.h"

#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Internal.h"

namespace DCLF::Diagnostics
{
	namespace
	{
		using V = SceneView;

		struct KeptShadowSets
		{
			std::vector<ID3D11ShaderResourceView*> textures;
			std::vector<ShadowPipelineKey> shadowKeys;
			std::array<std::vector<ShadowPipelineKey>, kOcclusionViews> occlusionKeys;
			std::uint32_t casters = 0;
			std::array<std::uint32_t, 16> rejects{};
			std::uint32_t staleInputs = 0;
		};
		KeptShadowSets kept;  // the coordinator's, between the two hooks

		void OnKept(const On<ScenePoint::ShadowSetsKept>&)
		{
			auto& store = Scene::Coordinator::Get();
			const auto& tables = V::tables(store);
			const auto& stats = V::stats(store);
			kept.textures = tables.shadowTextureSet;
			kept.shadowKeys = tables.shadowKeysUsed;
			kept.occlusionKeys = tables.occlusionKeysUsed;
			kept.casters = stats.shadowCasters;
			kept.rejects = stats.shadowRejects;
			kept.staleInputs = 0;
			const auto& indexed = V::shadowIndexedInputs(store);
			const auto& live = V::shadowIndexedLive(store);
			for (std::uint32_t slot = 0; slot < tables.objects.size(); ++slot)
				if (slot >= indexed.size() || indexed[slot] != V::ShadowInputsOf(store, slot) ||
					(slot < live.size() && (live[slot] != 0) != !(tables.objects[slot].flags & kObjectFree)))
					++kept.staleInputs;
		}

		void OnRebuilt(const On<ScenePoint::ShadowSetsRebuilt>&)
		{
			auto& store = Scene::Coordinator::Get();
			const auto& tables = V::tables(store);
			const auto& stats = V::stats(store);
			const auto& textures = kept.textures;
			const auto& shadowKeys = kept.shadowKeys;
			const auto& occlusionKeys = kept.occlusionKeys;
			const auto casters = kept.casters;
			const auto& rejects = kept.rejects;
			if (textures != tables.shadowTextureSet || shadowKeys != tables.shadowKeysUsed || occlusionKeys != tables.occlusionKeysUsed ||
				casters != stats.shadowCasters || rejects != stats.shadowRejects)
				logger::error("[DCLF] incremental shadow dependencies disagreed with same-table full reconstruction (frame {}, stale inputs {}, textures {}->{}, shadow keys {}->{}, sky keys {}->{}, precipitation keys {}->{}, casters {}->{}, rejects {}->{})",
					V::sceneFrame(store), kept.staleInputs, textures.size(), tables.shadowTextureSet.size(), shadowKeys.size(), tables.shadowKeysUsed.size(), occlusionKeys[kOcclusionSky].size(),
					tables.occlusionKeysUsed[kOcclusionSky].size(), occlusionKeys[kOcclusionPrecipitation].size(), tables.occlusionKeysUsed[kOcclusionPrecipitation].size(),
					casters, stats.shadowCasters, rejects[0], stats.shadowRejects[0]);
		}
	}

	void InstallShadowSetParity()
	{
		Subscribe<ScenePoint::ShadowSetsKept>(OnKept);
		Subscribe<ScenePoint::ShadowSetsRebuilt>(OnRebuilt);
	}
}
