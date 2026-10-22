#pragma once

// The scene's GPU tables shared by every epoch (SceneBuffers: the vertex stream, the features' scene resources, readback lists, the draw
// families' published pipelines), the root states' and cull bindings' readers (render thread). Self-contained (features include it);
// Internal.h includes it in its order.

// volk must precede every Vulkan header in this translation unit (Helpers.h: DrawPipelinesRhi.h's FamilyPipelines).
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CullInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Extensions/TableFamilies.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/ContentOwners.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Readback/Readbacks.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"

#include "RenderGraph/ComputeProgram.h"
#include <ORGModuleServices/GrowableBufferTypes.h>
#include <Resources/Buffers/Buffer.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace DCLF::Draws
{
	struct IndexPool;

	/**
	 * @brief The scene's tables, each a buffer of the scene's set (SceneBufferSet): every capacity a commit, build or pass reads is
	 * its named version's units, which the revision producer requires to hold the tables (RequireSceneTables).
	 */
	struct SceneBuffers
	{
		// The object records, extras and geometry table are the installed payload's (PayloadVersions).
		// The vertex stream's buffer (Api::VertexStreamContribution::select: Features/Faces' face positions), of its scene resources'
		// state, resolved once at setup: the second vertex stream the passes bind. Null without a stream.
		Versioned vertexStream;
		// The index pool every plain indexed draw binds; kept current by every commit that draws from it (UpdateIndexPool).
		std::shared_ptr<IndexPool> pool;
		// The features' scene resources' states (Api::SceneResources::setup, by registration: Extensions::RegisteredSceneResources;
		// Extensions::SceneResourcesOf), made with these tables, the vertex stream's first, and held until they go.
		std::vector<std::shared_ptr<void>> sceneResources;
		// The passes' states (Api::PassDef::setup, by pass: the executor's registration order, SetUpPassStates); null: no setup, or the
		// pass is not added.
		std::vector<std::shared_ptr<const void>> passStates;
		// Visibility readback (null without its program, or without the root states' rows: Api::CullBindingSlot::RootRows): events
		// (count zeroed by every depth commit), and each root's and object's last reported verdict. Every version is zero-filled; the
		// decoder restarts with the first batch of a version it has not followed (visibilityReportedFollowed: root, object).
		Versioned visibilityEvents, visibilityRootReported, visibilityObjectReported;
		std::array<org::services::FilledVersion, 2> visibilityReportedFollowed;
		std::shared_ptr<const ComputeProgram> visibilityEventsProgram;
		std::shared_ptr<VisibilityReadback> visibilityReadback;
		// The draw families' pipelines on their lane (Api::DrawFamily, by family), published by the frame's decision once built
		// (DrawFamilyFrame::Pipelines); read on the graph host's thread (FamilyPipelinesOf).
		std::array<std::atomic<std::shared_ptr<const FamilyPipelines>>, Extensions::kDrawFamilies> familyPipelines;
		// The owners of the descriptors the commits' rows in these tables name, by range (render thread): every execution holds them while
		// the bytes are current (dclf-architecture.md, "Descriptor ownership").
		ContentOwners content;
	};

	/**
	 * @brief The root states' family in a_tables (Api::RootStatesContribution::family; null without a producer, or before its first
	 * write): its objectRow is every object's root slot, read as data.
	 */
	inline const Extensions::TableFamilyColumns* RootFamilyOf(const SceneTables& a_tables)
	{
		return a_tables.families.Find(Extensions::RootStatesFamily());
	}

	/**
	 * @brief Cull binding a_slot's descriptor for frame a_frame as a_adopted names it (render thread, a commit's latch): a versioned
	 * buffer's SRV, a plain buffer's, a pooled buffer's index; 0 unbound.
	 */
	inline std::uint32_t CullBindingIndex(Api::CullBindingSlot a_slot, const SceneBuffers& a_scene, std::uint32_t a_frame, const NamedVersions& a_adopted)
	{
		const auto select = Extensions::SelectCullBinding(a_slot, a_scene, a_frame, &a_adopted);
		if (select.versioned && *select.versioned)
			return a_adopted.Of(**select.versioned).srvIndex;
		if (select.buffer && *select.buffer)
			return (*select.buffer)->GetSRVInfo(0).slot.index;
		return select.srv;
	}

	/**
	 * @brief Whether the root states' rows are bound and hold the tables' roots for the commit (Api::CullBindingSlot::RootRows, its rows
	 * answer): Core's cull binds them only then.
	 */
	inline bool RootRowsCurrent(const SceneBuffers& a_scene)
	{
		const auto select = Extensions::SelectCullBinding(Api::CullBindingSlot::RootRows, a_scene);
		return select.versioned && *select.versioned && select.rows;
	}

	/** @brief Sends sorted word indices as runs a_send(first, count), merging gaps up to kWordRunGap. */
	constexpr std::uint32_t kWordRunGap = 32;
	template <class Send>
	void SendWordRuns(const std::vector<std::uint32_t>& a_sorted, Send&& a_send)
	{
		for (std::size_t i = 0; i < a_sorted.size();) {
			const std::uint32_t first = a_sorted[i];
			std::uint32_t lastIndex = first;
			while (++i < a_sorted.size() && a_sorted[i] - lastIndex <= kWordRunGap)
				lastIndex = a_sorted[i];
			a_send(first, lastIndex - first + 1);
		}
	}
}
