#pragma once

// The views drawn by epochs of their own: the reflection's faces and the first-person view (one set of types, ViewTraits), and a view's
// frame state (ViewEpochState, which IndirectDraws::ViewState hands the view features). Captured and committed on the render thread;
// their shapes are the revision producer's. Self-contained (features include it); Internal.h includes it in its order.

// volk must precede every Vulkan header in this translation unit (Main/Resources.h first).
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Api/Passes.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesRhi.h"
#include "Features/DrawcallLimitFix/Core/Render/Main/FrameLayout.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Latched.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Epochs.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"

#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include <Render/LatchBlock.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <Resources/Buffers/Buffer.h>
#include <Resources/ExternalTextureResource.h>
#include <Resources/PixelBuffer.h>
#include <rhi.h>

#include <d3d11.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace DCLF
{
	struct SceneTables;
}

namespace DCLF::Draws
{
	/*
	 * The views (ViewEpoch.cpp and ViewEpochRecord.cpp, with their captures in Features/Reflection and Features/FirstPerson): epochs that
	 * cull the frame's scene list through BuildDraws' bucket path, frustum only, for faces with a camera of their own, and draw the
	 * buckets into the view's targets. Each face's VS_PerFrame (b12) is its camera's (PerFrameBlock); the frame constants otherwise
	 * the faces' own (OwnsFrameConstants: their commit's, from this frame's sources) or first person's, the colour segment's.
	 * - The water reflection's cube faces (dclf-architecture.md, "Water reflections"): one epoch per reflection update, after the engine's
	 *   face loop. Six faces, each drawing its LOD slots' forward pipelines into its slice of the engine's cube with DCLF's depth, and
	 *   tree LOD from a list of its own. The shape always has all six faces, so the recordings hold across updates: a face the update
	 *   does not render culls nothing (its latch's dispatch is 0, its tree row names no slots).
	 * - The first-person view (dclf-architecture.md, "First person as a view bit"): two epochs a frame the engine draws first person,
	 *   both recorded against one shape. The depth epoch at RenderDepth's first-person draw culls with the first-person camera and
	 *   draws the buckets' Z-prepass depth pipelines into the main depth; the colour epoch at Main::RenderFirstPersonView's
	 *   accumulator render draws the same sequences with each bucket's first-person forward pipeline into kMAIN and the motion
	 *   vectors, tested against kPOST_ZPREPASS_COPY, then its forward range (sorted back to front by the depth epoch). Both with the
	 *   viewport depth range [0, f1stPersonFarDepthRange].
	 */
	// The faces a view's epoch draws at most (Api::ViewShape::faces; the reflection's six).
	constexpr std::uint32_t kMaxViewFaces = 6;
	// A face's VS_PerFrame (b12, VS and PS) as the engine writes it for the view's camera: at most a D3D11 block's worth.
	constexpr std::uint32_t kViewBlockBytes = 1024;

	/** @brief What tells the views apart: their faces, options, epochs and names. */
	struct ViewTraits
	{
		ViewKind kind;
		std::uint32_t faces;
		bool trees;        // the draw families' faces drawn per face (tree LOD's)
		bool forwardPart;  // a forward range, sorted by the depth epoch and drawn after the buckets by the colour epoch
		bool depthPass;    // the depth epoch draws the buckets' depth pipelines into the main depth (the colour epoch then their colour ones)
		std::uint32_t viewBits;
		std::array<std::uint32_t, 2> epochs;  // the depth (or only) epoch, then the colour one (kNoEpoch: none): the schedule's (ViewEpochsOf)
		RenderGraphRuntime::Segment segments[2];
		const char* name;    // for the log
		const char* prefix;  // its graph resources' and passes' identifiers, and its extension's
		const char* blocks;  // its blocks' resource name
		const char* target;  // its target's resource name
		struct Labels
		{
			const char *map, *tables, *latches, *blocks, *trees;
		} labels;          // its latch writes, as FrameData counts them
		bool frustumCull;  // its cull's frustum test (Api::ViewShape::frustumCull); false: every member drawn untested
	};
	const ViewTraits& TraitsOf(ViewKind a_kind);

	/** @brief Whether a view draws with frame constants of its own (the faces': a view without a depth pass), not the main resources'. */
	inline bool OwnsFrameConstants(const ViewTraits& a_traits) { return !a_traits.depthPass; }

	/**
	 * @brief A view's latch block's region per frame slot: each face's BuildDrawsLatch, the slots' map, each face's bucket table
	 * (first, capacity), the state classes' map (a view with a forward part: its forward range's entries are per class), then each face's
	 * values its latched copies take (its block, and in a trees view each draw family's row and list header with faces:
	 * Extensions::DrawFamilyFaceLayout, at DrawFamilyFaceBytes a face), then a view with frame constants of its own its frame values
	 * (ViewFrame::kFrameList's copies: frameBytes, grown by the revision producer to what they take).
	 */
	struct ViewLatchLayout
	{
		std::uint32_t faces = 1;
		std::uint32_t slots = 0;    // pipeline slots the map holds a word for
		std::uint32_t buckets = 0;  // entries a face's table holds
		std::uint32_t faceBytes = kViewBlockBytes;
		std::uint32_t frameBytes = 0;  // the frame values' (none: a view drawing with the main frame constants)
		std::uint32_t classMapWords = 0;  // set indices the class map holds a word for (IndirectState::classOf; none: no forward part)
		static ViewLatchLayout Initial(const ViewTraits& a_traits) { return { a_traits.faces, 64, 4, a_traits.trees ? Extensions::DrawFamilyFaceBytes() : kViewBlockBytes }; }
		std::uint32_t MapOffset() const { return faces * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)); }
		std::uint32_t TableOffset(std::uint32_t a_face) const { return MapOffset() + slots * 4 + a_face * buckets * 8; }
		// BuildDrawsLatch::classMapOffset's: [set indices, 0, 0, 0], then a word per set index (16-byte aligned: the table ends on 8).
		std::uint32_t ClassMapOffset() const { return (TableOffset(faces) + 15u) & ~15u; }
		std::uint32_t FaceOffset(std::uint32_t a_face) const
		{
			return ((ClassMapOffset() + (kClassHeaderWords + classMapWords) * 4 + 255u) & ~255u) + a_face * faceBytes;
		}
		std::uint32_t FrameOffset() const { return (FaceOffset(faces) + 255u) & ~255u; }
		std::uint32_t Bytes() const { return frameBytes ? FrameOffset() + frameBytes : FaceOffset(faces); }
		bool operator==(const ViewLatchLayout&) const = default;
	};

	/** @brief What a view's epochs' passes record against; the per-frame values are in its latch. */
	struct ViewFrame
	{
		std::uint64_t generation = 0;
		ViewKind kind = ViewKind::Reflection;
		rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
		IndirectState indirect{};  // zLayout and zDrawSignature, the state classes' colour signatures (the version holds them)
		std::shared_ptr<const org::LatchBlock> latch;
		std::uint32_t width = 0, height = 0;  // a face's render area
		float depthRange = 1.0f;              // the viewport's maximum depth
		std::uint32_t sequenceDraws = 0;  // a face's range of the sequences
		// A bucket: the pipeline slots of one pair of pipelines (the depth one: the Z-prepass group's, for a view with a depth pass),
		// its range of a face's sequences.
		struct Bucket
		{
			rhi::PipelineHandle depth{}, colour{};
			std::uint32_t first = 0, capacity = 0;
			bool operator==(const Bucket& o) const
			{
				return SameHandle(depth, o.depth) && SameHandle(colour, o.colour) && first == o.first && capacity == o.capacity;
			}
		};
		std::vector<Bucket> buckets;
		// The forward range, per state class (IndirectState::classes; empty: none): capacity staging slots at first, the sorted half after
		// them; its table entry and bucket count word forwardEntry + its class (forwardEntry: buckets.size()), drawn through its class's
		// colour signature (a class without forward slots: capacity 0, no call).
		std::vector<SetClassRanges::Range> forwardRanges;
		std::uint32_t forwardEntry = 0;
		// Per face, the frame push data: the view's frame constants (its own, or the colour segment's) with VS and PS b12 the face's block.
		std::vector<std::array<std::uint32_t, kFramePushWords>> push;
		// Per draw family, its faces (a trees view's; tree LOD's): the forward pipeline and its draw's signature (invalid: none), the
		// cull's groups.
		struct Faces
		{
			rhi::PipelineHandle pipeline{};
			rhi::CommandSignatureHandle signature{};
			std::uint32_t groups = 0;
			bool operator==(const Faces& o) const { return SameHandle(pipeline, o.pipeline) && SameHandle(signature, o.signature) && groups == o.groups; }
		};
		std::array<Faces, Extensions::kDrawFamilies> faces;
		// The latched copies (LatchedCopiesPass): the faces' values from the latch, the counters' zeros, and a view with frame constants
		// of its own its frame values from the latch (kFrameList: what its commit writes through LatchedUploads, so its unwritten copies are
		// zeroed apart from the faces' values).
		static constexpr std::size_t kFrameList = 2;
		std::array<LatchedList, 3> latched;
		// Not compared (implied by the latch block and the buckets): its layout, and each pipeline slot's bucket.
		ViewLatchLayout latchLayout;
		std::shared_ptr<const std::vector<std::uint32_t>> map;

		bool SameShape(const ViewFrame& o) const
		{
			return latch == o.latch && latched == o.latched && SameHandle(resourceHeap, o.resourceHeap) && SameHandle(samplerHeap, o.samplerHeap) &&
			       SameIndirect(indirect, o.indirect) && width == o.width && height == o.height && depthRange == o.depthRange &&
			       sequenceDraws == o.sequenceDraws && buckets == o.buckets && push == o.push && forwardRanges == o.forwardRanges &&
			       forwardEntry == o.forwardEntry && faces == o.faces;
		}
	};

	/**
	 * @brief A view's graph resources. Its buffers are the scene's set's: a face's range of the sequences (a unit: a draw in every
	 * face), a face's bucket count words, and the faces' draw family lists (each family's units: tree LOD's shape slots, grown with its
	 * tables), each required by the revision producer to hold the view's plan (Impl::RequireView).
	 */
	struct ViewResources
	{
		const ViewTraits* traits = nullptr;
		// The main pass's (its depth inputs, targets, rows, visibility, and the frame constants of a view without its own) and, through it,
		// the scene's.
		std::shared_ptr<Resources> main;
		Versioned sequences;  // faces * a face's range
		std::shared_ptr<org::Buffer> count;
		std::array<Versioned, kMaxViewFaces> bucketCounts;
		std::shared_ptr<org::Buffer> blocks;  // kViewBlockBytes per face
		std::uint64_t blocksAddress = 0;
		// A view with frame constants of its own (OwnsFrameConstants: the faces'): laid out as the main frame slots (FrameSlots.h), with
		// what the faces' programs read (the record, PS b5/b6, the shared light block, the lighting slot's rows, VS b13), written by its
		// commit from this frame's sources. Null for a view drawing with the main resources' (first person's).
		std::shared_ptr<org::Buffer> frameConstants;
		std::uint64_t frameConstantsAddress = 0;
		bool sharedLightZeroed = false;
		// With them, its copies of the main pass's frame buffers (Resources::frameBuffers' shape: Deferred's sun cascades at t98), which its
		// commit fills from each buffer's CPU mirror (this frame's contents) and names at their registers. Made by SetupView, made again by
		// Setup when the main frame buffers' shape changes, replaced whole (the revision producer reads them); null without frame constants.
		std::atomic<std::shared_ptr<const std::vector<FrameBuffer>>> frameBuffers;
		// The reflection's: DCLF's depth for the faces (cleared per face) and the engine's cube target. First person's: the colour
		// epoch's depth, the engine's kPOST_ZPREPASS_COPY (FUN_1414cb6d0 binds it as the first-person pass's depth).
		std::shared_ptr<org::PixelBuffer> depth;
		std::shared_ptr<org::ExternalTextureResource> target;
		ID3D11Texture2D* targetTexture = nullptr;
		std::uint32_t width = 0, height = 0;
		// Per draw family with faces (a trees view's), per face a row (its list the face's) and a list, for the family's units (tree LOD's:
		// a draw row and a visible list for the scene's shape slots); named its rowName and listName with the face's index.
		std::array<std::array<std::shared_ptr<org::Buffer>, kMaxViewFaces>, Extensions::kDrawFamilies> faceRows;
		std::array<std::array<Versioned, kMaxViewFaces>, Extensions::kDrawFamilies> faceLists;
		std::array<std::array<std::uint64_t, kMaxViewFaces>, Extensions::kDrawFamilies> faceRowsAddress{};
		// The passes are to be declared again (new main resources): at the view's own import, never mid-frame.
		bool rebuildPending = false;
		std::shared_ptr<const ComputeProgram> buildDraws;
		rhi::CommandSignaturePtr dispatchSignature;
		// The forward range's draw's explicit preprocess (its sort is the main resources' ForwardSortCS).
		std::shared_ptr<PreprocessStates> preprocessForward;
	};

	std::unique_ptr<org::RenderGraph::IRenderGraphExtension> MakeViewExtension(std::shared_ptr<ViewResources> a_resources);
	/** @brief The views' epochs' passes (view.*: ViewEpochRecord.cpp), which Passes/CorePasses.cpp registers. */
	std::span<const Api::PassDef> ViewPassDefs();
	/**
	 * @brief Render thread, at the graph's build: whether a_view's passes can be added, drawing from a_main (its main resources, with the
	 * scene's index pool; its target imported; with a depth pass, the main depth and targets): its own extension's epochs, or a main
	 * epoch hosting its epoch (Api::EpochDef::hostedBy).
	 */
	bool ViewPassesReady(const ViewResources& a_view, const Resources& a_main);
	/**
	 * @brief The targets a view's latched copies write (ViewFrame::latched): the draw count, the blocks, a view's own frame constants and
	 * frame buffers' copies, each face's bucket counts and draw families' rows and lists. Its own epoch's latched copies declare them, or
	 * those of the main epoch hosting its epoch.
	 */
	std::vector<LatchedTarget> ViewLatchedTargets(const ViewResources& a_view);

	/**
	 * @brief A view's buckets: per pair of pipelines (the depth one only with a depth pass), the slots that draw with it, each slot's
	 * range what its objects can produce (the Z-prepass's bucket capacity); each slot's bucket (kNoBucket: none); and the forward
	 * range after them, per state class (ViewFrame::forwardRanges).
	 */
	struct ViewPlan
	{
		std::vector<std::uint32_t> map;
		std::vector<ViewFrame::Bucket> buckets;
		std::uint32_t draws = 0;  // a face's, all buckets and the forward range
		std::vector<SetClassRanges::Range> forwardRanges;
	};
	/** @brief A pipeline slot's Z-prepass group (its lookup entry's set index's, IndirectState::zGroups); kNoBucket without one. */
	std::uint32_t ZGroupOf(const Lookups* a_lookups, const IndirectState& a_indirect, std::uint32_t a_slot);
	/** @brief A pipeline slot's state class word (its lookup entry's set index's, IndirectState::classOf); kNoSetClass without one. */
	std::uint32_t SetClassOf(const Lookups* a_lookups, const IndirectState& a_indirect, std::uint32_t a_slot);
	void PlanViewBuckets(const ViewTraits& a_traits, const MainLayout& a_main, const Lookups* a_lookups, const IndirectState& a_indirect,
		std::span<const rhi::PipelineHandle> a_slotPipelines, std::span<const std::uint8_t> a_forwardSlots, ViewPlan& a_out);

	/** @brief A view's latch, its layout and the zeros its latched copies take: the blocks the revision's set names (NamedVersions::Latch). */
	struct ViewLatches
	{
		std::shared_ptr<const org::LatchBlock> latch, zeros;
		ViewLatchLayout layout;
	};

	/** @brief What a view's shape is made from (MakeViewShape): the plan, its sequences' named range and the latches. */
	struct ViewShapeInputs
	{
		rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
		IndirectState indirect{};
		ViewPlan plan;
		std::uint32_t sequenceDraws = 0;  // a face's range: the sequences' named units (the revision's set), at least plan.draws
		const ViewLatches* latches = nullptr;
		std::uint64_t frameConstantsAddress = 0;  // the view's own (OwnsFrameConstants) or the colour segment's, which the push data names
		std::vector<LatchedCopy> frameCopies;     // a view's own frame values' latched copies (ViewFrame::kFrameList), at the latch layout's FrameOffset
		std::uint32_t width = 0, height = 0;
		float depthRange = 1.0f;
		// Per draw family in the faces: its forward pipeline and draw signature, and the units its lists hold (tree LOD's shape slots;
		// none: invalid pipeline).
		struct Faces
		{
			rhi::PipelineHandle pipeline{};
			rhi::CommandSignatureHandle signature{};
			std::uint32_t units = 0;
		};
		std::array<Faces, Extensions::kDrawFamilies> faces;
	};
	std::shared_ptr<ViewFrame> MakeViewShape(const ViewResources& a_resources, const ViewShapeInputs& a_in);

	/**
	 * @brief The view's forward key of a_tables' pipeline slot a_slot, false for a slot it does not draw: the main pipeline's vertex
	 * descriptor and its pixel descriptor without Deferred. The faces': a LOD slot's, front faces culled unless two-sided. First
	 * person's: any but a forward slot (its forward range draws those), back faces culled unless two-sided, the first-person targets.
	 */
	bool ViewForwardKey(ViewKind a_kind, const SceneTables& a_tables, std::size_t a_slot, ForwardPipelineKey& a_key);
	/** @brief Per pipeline slot, its forward pipeline as a_catalog has it for a_targets (none while a_on is false). */
	void ViewPipelinesOf(ViewKind a_kind, const PipelineCatalog* a_catalog, const SceneTables& a_tables, const ForwardTargets& a_targets, bool a_on,
		std::vector<rhi::PipelineHandle>& a_slots);
	/**
	 * @brief First person's forward range: per pipeline slot, whether it is a forward key the view draws there (the main set's colour
	 * pipeline through its state class's colour signature): RasterForward, not refraction. None while a_on is false.
	 */
	void ViewForwardSlotsOf(const SceneTables& a_tables, bool a_on, std::vector<std::uint8_t>& a_slots);

	/** @brief A previous camera for PerFrameBlock's motion vector registers. */
	struct PreviousCamera
	{
		std::array<float, 16> viewProjUnjittered{};
		RE::NiPoint3 eye;
	};
	/**
	 * @brief VS_PerFrame (b12) for a camera, ShadowCameras::kBlockBytes at a_out, as the engine's SetCameraData builds it: the frame's
	 * registers (a_frame, the renderer's last upload) with the camera's over them - CameraView, CameraProj, CameraViewProj and
	 * CameraViewProjUnjittered; c20 the projection's inverse and c24 the projection (the engine's order: FrameBuffer.hlsli names them
	 * the other way), c28 the view's inverse, c32 the view-projection's, c36 the projection's; CameraPosAdjust. a_jitter: the TAA /
	 * upscaler jitter added to the projection's third row (State::projectionPosScaleX/Y). a_previous: CameraPreviousViewProjUnjittered
	 * (c16) and CameraPreviousPosAdjust (c41); none, both zero (with fDRClampOffset in c41's w) and the dynamic resolution registers
	 * (c43, c44) from the renderer state, as for a reflection face.
	 */
	void PerFrameBlock(const ShadowCameras::Camera& a_camera, std::span<const std::byte> a_frame, bool a_jitter, const PreviousCamera* a_previous, std::byte* a_out);
	/** @brief The renderer's last VS_PerFrame upload: the mirror's, or Community Shaders' copy of it until the mirror has seen a write. */
	std::span<const std::byte> FrameBlockNow();
	/** @brief The inverse of a row-major 4x4 (cofactors, in double); zero when singular. */
	std::array<float, 16> Inverse(const std::array<float, 16>& a_m);

	/**
	 * @brief A trees view's commit's rows (Impl::ViewFaceRows): per draw family its faces.row's (rowBytes), empty for a family without
	 * faces.
	 */
	using ViewFamilyRows = std::array<std::span<const std::byte>, Extensions::kDrawFamilies>;

	/** @brief A face's values a view's commit writes (WriteViewLatch). */
	struct ViewFace
	{
		bool drawn = false;
		std::span<const std::byte> block;  // written when not empty
		std::array<float, 16> viewProj{};
		RE::NiPoint3 eye;
	};

	/** @brief What a view's revision shapes read of the frame (RevisionInputs): its resources, targets, render area and depth range. */
	struct ViewInputs
	{
		std::shared_ptr<ViewResources> resources;
		ForwardTargets targets;
		std::uint32_t width = 0, height = 0;
		float depthRange = 1.0f;
		bool operator==(const ViewInputs&) const = default;
	};
}

namespace DCLF
{
	/**
	 * @brief A view's frame state (ViewEpoch.cpp and the view engine, ViewEngine.cpp; render thread). The scene lane reads only the
	 * published LaneTargets and forwardCompatible, never `targets`. `covered` is decided at the frame's start, before the engine draws
	 * the view (ImplViews::ViewCovered reads it).
	 */
	struct ViewEpochState
	{
		ForwardTargets targets;
		std::atomic<std::uint64_t> laneColours{ 0 }, laneDepth{ 0 };  // colour in the low 32 bits, colour2 in the high
		void PublishTargets()
		{
			const std::uint64_t colours = std::uint64_t(static_cast<std::uint32_t>(targets.colour)) | (std::uint64_t(static_cast<std::uint32_t>(targets.colour2)) << 32);
			if (laneColours.load(std::memory_order_relaxed) != colours)
				laneColours.store(colours, std::memory_order_release);
			if (laneDepth.load(std::memory_order_relaxed) != static_cast<std::uint64_t>(targets.depth))
				laneDepth.store(static_cast<std::uint64_t>(targets.depth), std::memory_order_release);
		}
		ForwardTargets LaneTargets() const
		{
			const std::uint64_t colours = laneColours.load(std::memory_order_acquire);
			ForwardTargets out;
			out.colour = static_cast<DXGI_FORMAT>(colours & 0xFFFFFFFFu);
			out.colour2 = static_cast<DXGI_FORMAT>(colours >> 32);
			out.depth = static_cast<DXGI_FORMAT>(laneDepth.load(std::memory_order_acquire));
			return out;
		}
		std::vector<rhi::PipelineHandle> slotPipelines;
		std::shared_ptr<Draws::ViewResources> resources;
		bool setupFailed = false;
		// Until the first coverage decided it (coverageDecided), the view's coveredInitially stands for it (ImplViews::ViewCovered).
		bool covered = false;
		bool coverageDecided = false;
		// The engine's viewport depth range for the view (a revision input: the shapes' viewport); 0 until the view's work sets one
		// (IndirectDraws::SetViewDepthRange), the shape's (Api::ViewShape::depthRange) until then.
		float depthRange = 0.0f;
		// Whether the main set's forward pipelines can draw into the view's forward part (its depth format the main pass's), published
		// for the slot readiness (IndirectDraws::ViewSlotReady).
		std::atomic<bool> forwardCompatible{ false };
		// A trees view's draw families per face (Api::ViewShape::trees, the view engine's: IndirectDraws::ResolveViewFaces,
		// DecideViewFaces), by family: its forward pipeline for the view's targets; the commit's row (faces.row's: Impl::ViewFaceRows,
		// and the decision's query) and a face's row and list header (faces.face's: CommitView), sized once at the family's faces'.
		struct FamilyFaces
		{
			rhi::PipelineHandle pipeline{};
			bool ready = false;
			std::vector<std::byte> row, faceRow, faceHeader;
		};
		std::array<FamilyFaces, Extensions::kDrawFamilies> faces;
		// By family, whether the faces' draws are DCLF's this frame (PassCapture::SetViewFamilyOwned; the roots they own: ViewRoots.cpp).
		std::array<bool, Extensions::kDrawFamilies> facesOwned{};
		// A view with frame constants of its own (Draws::OwnsFrameConstants): its frame textures (t16 and up) as its last commit resolved
		// them from this frame's sources (IndirectDraws::SubmitViewEpoch) and its commit named its frame buffers' copies at theirs (CommitView),
		// each register's binding kept while it names the same view (of the textures' generation), and the registers its frame record left
		// null.
		std::array<std::uint32_t, kTextureRegisters> frameTextures{};
		std::array<ID3D11ShaderResourceView*, kTextureRegisters> frameTextureViews{};
		std::array<GpuTextures::Binding, kTextureRegisters> frameTextureBindings{};
		std::uint32_t frameTextureGeneration = ~0u;
		std::array<std::uint64_t, 2> nullTextures{};
	};
}
