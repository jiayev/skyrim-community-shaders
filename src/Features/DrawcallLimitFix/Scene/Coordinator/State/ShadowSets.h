#pragma once

// The shadow sets (CoordinatorShadowSets): each slot's shadow inputs, the texture and key indices and their order, the emitted
// lists and their counters. AccumulatePhase.cpp (RefreshShadowSets), with the walk.
//
// A private base of Scene::Coordinator, included by Coordinator.h: names, types and initialisers are the coordinator's own,
// and the functions over them are declared there.

namespace DCLF::Scene
{
	struct CoordinatorShadowSets
	{
		/** @brief A slot's inputs to the frame's shadow sets. */
		struct ShadowInputs
		{
			ID3D11ShaderResourceView* diffuse = nullptr;
			std::uint64_t vertexDesc = 0;
			std::uint32_t technique = 0;
			std::uint32_t flags = 0;
			std::uint32_t reject = 0;
			std::array<std::uint32_t, kOcclusionViews> occlusionTechnique{};
			bool operator==(const ShadowInputs&) const = default;
		};
		// Only slots whose shadow inputs changed this walk update their contributions.
		bool shadowSetsDirty = true;
		bool shadowIndexNeedsRebuild = true;
		std::vector<std::uint32_t> shadowDirtySlots;
		std::vector<ShadowInputs> shadowIndexedInputs;
		std::vector<std::uint8_t> shadowIndexedLive;
		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, std::set<std::uint32_t>> shadowTextureMembers;
		ankerl::unordered_dense::map<ShadowPipelineKey, std::set<std::uint32_t>, ShadowPipelineKeyHash> shadowKeyMembers;
		std::array<ankerl::unordered_dense::map<ShadowPipelineKey, std::set<std::uint32_t>, ShadowPipelineKeyHash>, kOcclusionViews> occlusionKeyMembers;
		// Each index's keys by their lowest member slot (unique within an index: a slot names one texture, one key per view), the sets'
		// order, kept as members come and go (RefreshShadowSets); made again with a rebuild.
		std::map<std::uint32_t, ID3D11ShaderResourceView*> shadowTextureOrder;
		std::map<std::uint32_t, ShadowPipelineKey> shadowKeyOrder;
		std::array<std::map<std::uint32_t, ShadowPipelineKey>, kOcclusionViews> occlusionKeyOrder;
		// RefreshShadowSets' scratch: each texture's membership change over the call (+1 appeared, -1 gone).
		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, std::int32_t> shadowTextureNet;
		// RefreshShadowSets' public lists as last emitted: a list is made again from its order only when that order moved, the seen set
		// patched by the net changes (made again with a rebuild), and the tables take copies (the walk drops theirs each dirty pass).
		std::vector<ID3D11ShaderResourceView*> shadowEmittedTextures;
		ankerl::unordered_dense::set<ID3D11ShaderResourceView*> shadowEmittedSeen;
		std::vector<ShadowPipelineKey> shadowEmittedKeys;
		std::array<std::vector<ShadowPipelineKey>, kOcclusionViews> shadowEmittedOcclusionKeys;
		/** @brief RefreshShadowSets' counters since the last report (CS_DCLF_STATS). */
		struct ShadowSetStats
		{
			std::uint64_t refreshes = 0, rebuilds = 0;  // calls; of them index rebuilds (an explicit invalidation, or parity's)
			std::uint64_t journaled = 0, applied = 0;   // slots the journal named (deduplicated); of them with inputs or liveness moved
			std::uint64_t unjournaled = 0;              // dirty with nothing journaled: the lists emitted again, the index untouched
			std::uint64_t listsEmitted = 0;             // public lists made again from their order (texture, key, per occlusion view)
		};
		ShadowSetStats shadowSetStats;
		std::uint32_t keptShadowCasters = 0;
		std::array<std::uint32_t, 16> keptShadowRejects{};
	};
}
