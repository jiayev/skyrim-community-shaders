#pragma once

// The draws area's hook points (IndirectDraws' epochs, commits, builds and stores), for its observers (Diagnostics/Draws, subscribed in
// DrawsInstall.cpp). Each is announced with Diagnostics::Notify (Hooks.h) on the thread its comment names; an event that hands something
// back carries a reference to the output, which the main path initializes to its own answer with nothing observing.

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "DrawsView.h"
#	include "Features/DrawcallLimitFix/Diagnostics/Engine/ShadowViewTally.h"
#	include "Features/DrawcallLimitFix/Engine/Views/SunViews.h"

namespace DCLF::Diagnostics
{
	// ---- The main epochs (render thread).

	/** @brief A main epoch ran (after its execution): its payload, and the frame's Z-prepass payload when it is this frame's (colour only). */
	struct MainEpochDone
	{
		const std::shared_ptr<Draws::Resources>& resources;
		const Draws::MainPayload& payload;
		const Draws::MainPayload* depth;  // the frame's committed Z-prepass payload (colour epoch, the same frame), or null
		bool depthOnly;
		IndirectDrawStats& stats;
	};

	/** @brief A main commit wrote its culling latch (the depth commit's terrain near test, the colour commit's sun test). */
	struct CullLatchWritten
	{
		const Draws::BuildDrawsLatch& latch;
		bool depthOnly;
	};

	/**
	 * @brief The depth commit's fade roots, before they are uploaded: an observer may name the first root of the frame's FadeStateCS
	 * log window (base, ~0u: none logged).
	 */
	struct FadeLogWindow
	{
		std::uint32_t frame;
		const SceneTables& tables;
		const FadeFrame& inputs;
		std::uint32_t& base;
	};

	/** @brief A main commit's culling ViewProj (latch.viewProj, the eye folded in), with the shape's size. */
	struct MainCullViewProj
	{
		const Draws::BuildDrawsLatch& latch;
		std::uint32_t width, height, frame;
		bool depthOnly;
		const SceneTables& tables;
	};

	/** @brief The first-person view's walk of the skeleton's geometry (every 16 frames), with the frame's claims. */
	struct FirstPersonSkeletonWalked
	{
		const SceneTables& tables;
		const ankerl::unordered_dense::set<const RE::BSGeometry*>& skeleton;
		std::size_t walked;
		const SetSnapshot& claims;
	};

	/** @brief The engine drew a reflection face (IndirectDraws::CaptureReflectionFace), its targets still bound. */
	struct ReflectionFaceDrawn
	{
	};

	/** @brief The main resources' foliage record made (Policy::RecordFoliagePixels): it stands when an observer finds it ready. */
	struct FoliageRecordMade
	{
		Draws::Resources::FoliageRecord& record;
		rhi::Device device;
		std::uint32_t slots;  // the host's frame slots
		bool& ready;
	};

	/** @brief The main epochs' graph passes being listed: an observer may append its own at `point` (render thread, a graph build). */
	struct MainGraphPasses
	{
		enum Point : std::uint8_t
		{
			kBeforeColour,  // before the colour draws (the colour epoch)
			kAfterColour,   // after them, before the depth segment's two-phase tail (the colour epoch)
			kAfterDepth,    // after the depth segment's passes (the Z-prepass epoch)
		};
		Point point;
		const std::shared_ptr<Draws::Resources>& resources;
		std::uint32_t epoch;  // the point's epoch (RenderGraphRuntime::EpochOf)
		std::vector<org::RenderGraph::ExternalPassDesc>& out;
	};

	// ---- The shadow epochs (render thread).

	/** @brief The scene's index pool brought up to the tables' first `count` geometry slots (UpdateIndexPool). */
	struct IndexPoolUpdated
	{
		const Draws::IndexPool& pool;
		const SceneTables& tables;
		std::uint32_t count;
	};

	/** @brief A shadow view's capture: where DCLF predicts the engine draws it (the renderer's state, just set by its draw, holds where it did). */
	struct ShadowTargetPredicted
	{
		std::uint32_t viewId;
		const ShadowViews::View* shadowView;
		std::uint32_t occurrence, target, slice;
	};

	/** @brief A view whose viewport and eye are DCLF's (cascade: its view this frame, or null), before they replace what the capture read from the renderer (view). */
	struct OwnShadowViewport
	{
		const Draws::PendingView& view;
		const SunViews::Cascade* cascade;
		std::uint32_t descriptor, target;
		ShadowView kind;
	};

	/** @brief A view whose block is DCLF's, before it replaces the engine's block the capture took (Policy::CaptureEngineShadowBlock). */
	struct OwnShadowCamera
	{
		const Draws::PendingView& view;
		const SunViews::Cascade& cascade;
		std::uint32_t descriptor;
		ShadowView kind;
	};

	/** @brief A shadow build's inputs took DCLF's full-frustum processes for frame `frame` (taken), or found none current. */
	struct SunEntryPlanesTaken
	{
		std::uint32_t frame;
		bool taken;
	};

	/** @brief The engine's FinishAccumulating of occlusion map `view` (IndirectDraws::CaptureOcclusion). */
	struct OcclusionCaptured
	{
		std::uint32_t view;
	};

	/** @brief A shadow view's culling latch written (its whole latch: the epoch's last write of it), with the payload it draws. */
	struct ShadowViewLatched
	{
		const Draws::PendingView& view;
		const Draws::BuildDrawsLatch& latch;
		std::uint32_t frame;
		const Draws::ShadowPayload& payload;
		ShadowDrawStats& stats;
	};

	// ---- The stores the builds keep (the builder's thread, or the render thread while the builder is idle) and the buffers they fill.

	/** @brief The extras rows' buffer `entry` (kSceneBuffers, else a payload ring entry's, on the uploader's thread) is about to be sent rows: an observer may give the mirror they are copied into as sent. */
	struct ExtrasSending
	{
		static constexpr std::uint32_t kSceneBuffers = ~0u;
		std::uint32_t entry;
		std::vector<float>*& mirror;
	};

	/** @brief The extras rows' buffer `entry` was sent `rows` for frame `frame` (bufferRows: the rows it holds; ~0u, a ring entry: every row). */
	struct ExtrasSent
	{
		std::uint32_t entry;
		std::uint32_t frame;
		const Draws::ExtrasOut& rows;
		std::uint32_t bufferRows;
	};

	/**
	 * @brief A kept store brought up to `tables`, before its view is taken: the object records (UpdateObjectRecords), the geometry table
	 * (UpdateGeometryDraws), the revision producer's draw bound (UpdateDrawBound).
	 */
	template <class Store>
	struct StoreUpdated
	{
		const Store& store;
		const SceneTables& tables;
		std::uint32_t frame;
	};

	/** @brief A kept shadow build made (BuildShadowPayload with its kept state), from `in`. */
	struct ShadowBuilt
	{
		const Draws::ShadowInputs& in;
		const SceneTables& tables;
		const Lookups& lookups;
		const Draws::ShadowPayload& payload;
		const Draws::ShadowKept& kept;
	};

	/** @brief An exclusion its cache would have reused was built in full (Policy::ReuseShadowExclusion): whether the two agree. */
	struct ExclusionRebuilt
	{
		bool parabolic;
		bool same;
	};

	/** @brief A publication's draws built ahead (RunAhead), for frame `frame`. */
	struct DrawsBuiltAhead
	{
		const DrawsView::Impl::DrawPublication& draws;
		const SceneTables& tables;
		std::uint32_t frame;
	};
}
#endif
