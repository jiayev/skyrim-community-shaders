#pragma once

// The draws area's hook points (IndirectDraws' epochs, commits, builds and stores), for its observers (Diagnostics/Draws, subscribed in
// DrawsInstall.cpp). Each is announced with Diagnostics::Notify (Hooks.h) on the thread its comment names; an event that hands something
// back carries a reference to the output, which the main path initializes to its own answer with nothing observing.

#include "DrawsView.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/ShadowViewTally.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"

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
		// The scene payload the epoch committed with (the row tables BuildDraws read; null: none): its rows' bases for the build parity.
		const Draws::PayloadVersions* payloadVersions;
		IndirectDrawStats& stats;
	};

	/** @brief A main commit wrote its culling latch (the depth commit's terrain near test, the colour commit's sun test). */
	struct CullLatchWritten
	{
		const Draws::BuildDrawsLatch& latch;
		bool depthOnly;
	};

	/**
	 * @brief A frame texture register the drawn pipelines read that a main commit could not resolve (it reads zero), every commit
	 * per such register: the view captured there (null: nothing bound).
	 */
	struct FrameTextureUnresolved
	{
		std::uint32_t textureRegister;
		ID3D11ShaderResourceView* view;
	};

	/**
	 * @brief A main commit's frame buffer (a CPU-written structured buffer the pipelines read at t`textureRegister`) that the constant
	 * mirror cannot fill (it holds `mirrorBytes`): its copy reads zero. Every such commit.
	 */
	struct FrameBufferUnfilled
	{
		std::uint32_t textureRegister, elements, stride;
		std::size_t mirrorBytes;
	};

	/** @brief The depth commit's fade updates of `frame`, from the animation job, had differing inputs: the first's stand for all. */
	struct FadeInputsVaried
	{
		std::uint32_t frame;
	};

	/** @brief A main commit's payload drew past a geometry's buffers (MainBody::shortBuffer), the first time in the stats window. */
	struct ShortBuffersDrawn
	{
		const SceneTables& tables;
		const Draws::MainPayload& payload;
	};

	/**
	 * @brief A main commit's culling setup, every commit: the shape it commits into, its capture (the VS_PerFrame binding), whether it
	 * found the draws' ViewProj, and the replayed Z-prepass VS_PerFrame contents (colour with the prepass's vertex inputs; else empty).
	 */
	struct MainCullingSetup
	{
		const Draws::PassFrame& shape;
		const Draws::Capture& capture;
		bool depthOnly, hasViewProj, replay;
		std::uint32_t frame, liveObjects;
		std::span<const std::byte> replayPerFrame;
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

	/** @brief The first-person view's walk of the skeleton's geometry (every 16 frames), with the frame's claims (Features/FirstPerson's parity). */
	struct FirstPersonSkeletonWalked
	{
		const SceneTables& tables;
		const ankerl::unordered_dense::set<const RE::BSGeometry*>& skeleton;
		std::size_t walked;
		const SetSnapshot& claims;
	};

	/** @brief The engine drew a reflection face (Features/Reflection's FaceDrawn), its targets still bound: its face camera parity's point. */
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

	// ---- The frame's adoption, coverage, main epochs and retirement point (render thread, in frame order).

	/** @brief The frame adopted a snapshot whole (IndirectDraws::AdoptSnapshot): its set (null: none) and payload (null: none) are now the frame's. */
	struct SnapshotAdopted
	{
		std::uint32_t frame;
		std::uint64_t snapshot;  // the snapshot's serial
		const Draws::NamedVersions* set;
		const Draws::PayloadVersions* payload;
		bool growth;  // its set's versions are the first a growth made current (the frame noted as the growth's adoption)
	};

	/**
	 * @brief The frame's main coverage decided (Impl::DecideEpochCoverage): each epoch's (EpochCount() of them), and what it was decided
	 * from (the adopted snapshot, the installed payload, the active revision's set against the adopted one).
	 */
	struct EpochCoverageDecided
	{
		std::uint32_t frame;
		std::span<const bool> covered;
		std::uint64_t snapshot;  // the adopted snapshot's serial (0: none)
		bool drawsComplete, stale;
		const Draws::PayloadVersions* payload;  // the installed payload (null: none)
		bool versions;  // the active revision names a set
		bool current;   // ... the adopted one, with the snapshot current and its payload ready
	};

	/**
	 * @brief A main epoch's outcome (IndirectDraws::RunEpoch), every frame each: why it was not submitted, or (executed) whether its
	 * execution failed, committed nothing or was submitted, with the installed payload and the adopted snapshot it ran with.
	 */
	struct MainEpochOutcome
	{
		std::uint32_t frame;
		const char* outcome;
		const Draws::PayloadVersions* payload;  // the installed payload (null: none)
		std::uint64_t snapshot;                 // the adopted snapshot's serial (0: none)
		bool depthOnly;
		bool executed;   // the epoch reached its execution (outcome: "submitted", "FAILED" or "NOT COMMITTED")
		bool notDrawingYet;  // no claims were installed yet (FrameView::NotDrawingYet): the only claims-less frames
	};

	/**
	 * @brief The frame bound its adopted set and payload (IndirectDraws::BindAdopted, after its last submission) at `point`, with the
	 * retirement clock's completed point then: a point at or below it binds nothing.
	 */
	struct AdoptedBound
	{
		std::uint32_t frame;
		std::uint64_t point, completed;
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
		const ShadowCameras::Camera* cascade;
		std::uint32_t descriptor, target;
		ShadowView kind;
	};

	/** @brief A view whose block is DCLF's, before it replaces the engine's block the capture took (Policy::CaptureEngineShadowBlock). */
	struct OwnShadowCamera
	{
		const Draws::PendingView& view;
		const ShadowCameras::Camera& cascade;
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

	/**
	 * @brief The payload's extras backing `backing` (its lease) is about to be caught up with a publication's rows
	 * (PayloadBuilder::Publish, the snapshot builder's thread): an observer may give the mirror they are copied into as sent.
	 */
	struct ExtrasSending
	{
		const org::services::BackingLease& backing;
		std::vector<float>*& mirror;
	};

	/**
	 * @brief The payload's extras backing `backing` was caught up to `rows` (it now holds every row of them), from `tables` (whose
	 * extraOffset names the blocks a live object owns: a freed block's rows are never read, nor journalled once its owner is gone).
	 */
	struct ExtrasSent
	{
		const org::services::BackingLease& backing;
		const Draws::ExtrasOut& rows;
		const SceneTables* tables = nullptr;
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

	// ---- The sealed set (the snapshot is the only source of a versioned buffer's address and SRV index).

	/** @brief A scene revision sealed (snapshot builder), with the draws its snapshot pairs with it, built from `context`. */
	struct SnapshotSealed
	{
		const Draws::NamedVersions& sealed;
		const DrawsView::Impl::DrawPublication& draws;
		const DrawsView::Impl::AheadContext& context;
	};

	/** @brief A commit staged bytes into a versioned buffer (CommitUploads, render thread), through the adopted set (`sealed`). */
	struct CommitTargetStaged
	{
		const Draws::NamedVersions& sealed;
		const Draws::Versioned& target;
	};

	/**
	 * @brief The revision producer made shapes (MakeRevision, snapshot builder) from the sealed set `sealed`, the main layout and its
	 * inputs, for tables of `geometries` geometry slots.
	 */
	struct RevisionShapesMade
	{
		const Draws::NamedVersions& sealed;
		const Draws::MainLayout& layout;
		const Draws::MadeShapes& made;
		const Draws::RevisionInputs& inputs;
		std::size_t geometries;
	};
}
