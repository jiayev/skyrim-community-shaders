#pragma once

// The views drawn by epochs of their own: the reflection's faces and the first-person view (one set of types, ViewTraits).
// Captured and committed on the render thread; their shapes are the revision producer's. Included by Internal.h, in its order.

namespace DCLF::Draws
{
	/*
	 * The views (ViewEpoch.cpp, with their captures in Reflection.cpp and FirstPerson.cpp): epochs that cull the frame's scene list
	 * through BuildDraws' bucket path, frustum only, for faces with a camera of their own, and draw the buckets into the view's
	 * targets. Each face's VS_PerFrame (b12) is its camera's (PerFrameBlock); the colour segment's frame constants otherwise.
	 * - The water reflection's cube faces (dclf-lod.md, "Water reflections"): one epoch per reflection update, after the engine's
	 *   face loop. Six faces, each drawing its LOD slots' forward pipelines into its slice of the engine's cube with DCLF's depth, and
	 *   tree LOD from a list of its own. The shape always has all six faces, so the recordings hold across updates: a face the update
	 *   does not render culls nothing (its latch's dispatch is 0, its tree row names no slots).
	 * - The first-person view (drawcall-limit-fix.md, "First person as a view bit"): two epochs a frame the engine draws first person,
	 *   both recorded against one shape. The depth epoch at RenderDepth's first-person draw culls with the first-person camera and
	 *   draws the buckets' Z-prepass depth pipelines into the main depth; the colour epoch at Main::RenderFirstPersonView's
	 *   accumulator render draws the same sequences with each bucket's first-person forward pipeline into kMAIN and the motion
	 *   vectors, tested against kPOST_ZPREPASS_COPY, then its forward range (sorted back to front by the depth epoch). Both with the
	 *   viewport depth range [0, f1stPersonFarDepthRange].
	 */
	constexpr std::uint32_t kReflectionFaces = 6;
	constexpr std::uint32_t kMaxViewFaces = kReflectionFaces;
	// A face's VS_PerFrame (b12, VS and PS) as the engine writes it for the view's camera: at most a D3D11 block's worth.
	constexpr std::uint32_t kViewBlockBytes = 1024;

	/** @brief What tells the views apart: their faces, options, epochs and names. */
	struct ViewTraits
	{
		ViewKind kind;
		std::uint32_t faces;
		bool trees;        // tree LOD drawn per face
		bool forwardPart;  // a forward range, sorted by the depth epoch and drawn after the buckets by the colour epoch
		bool depthPass;    // the depth epoch draws the buckets' depth pipelines into the main depth (the colour epoch then their colour ones)
		std::uint32_t viewBits;
		std::array<std::uint32_t, 2> epochs;  // the depth (or only) epoch, then the colour one (Epoch::Count: none)
		RenderGraphRuntime::Segment segments[2];
		const char* name;    // for the log
		const char* prefix;  // its graph resources' and passes' identifiers, and its extension's
		const char* blocks;  // its blocks' resource name
		const char* target;  // its target's resource name
		struct Labels
		{
			const char *map, *tables, *latches, *blocks, *trees;
		} labels;  // its latch writes, as FrameData counts them
	};
	const ViewTraits& TraitsOf(ViewKind a_kind);

	/**
	 * @brief A view's latch block's region per frame slot: each face's BuildDrawsLatch, the slots' map, each face's bucket table
	 * (first, capacity), then each face's values its latched copies take (its block, and with tree LOD its row and list header).
	 */
	struct ViewLatchLayout
	{
		std::uint32_t faces = 1;
		std::uint32_t slots = 0;    // pipeline slots the map holds a word for
		std::uint32_t buckets = 0;  // entries a face's table holds
		std::uint32_t faceBytes = kViewBlockBytes;
		static constexpr std::uint32_t kTreeRowInFace = kViewBlockBytes;
		static constexpr std::uint32_t kTreeHeaderInFace = kTreeRowInFace + static_cast<std::uint32_t>(sizeof(TreeLod::DrawRow));
		static constexpr std::uint32_t kTreeFaceBytes = (kTreeHeaderInFace + static_cast<std::uint32_t>(sizeof(TreeLod::VisibleHeader)) + 255u) & ~255u;
		static ViewLatchLayout Initial(const ViewTraits& a_traits) { return { a_traits.faces, 64, 4, a_traits.trees ? kTreeFaceBytes : kViewBlockBytes }; }
		std::uint32_t MapOffset() const { return faces * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)); }
		std::uint32_t TableOffset(std::uint32_t a_face) const { return MapOffset() + slots * 4 + a_face * buckets * 8; }
		std::uint32_t FaceOffset(std::uint32_t a_face) const { return ((TableOffset(faces) + 255u) & ~255u) + a_face * faceBytes; }
		std::uint32_t Bytes() const { return FaceOffset(faces); }
		bool operator==(const ViewLatchLayout&) const = default;
	};

	/** @brief What a view's epochs' passes record against; the per-frame values are in its latch. */
	struct ViewFrame
	{
		std::uint64_t generation = 0;
		ViewKind kind = ViewKind::Reflection;
		rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
		IndirectState indirect{};  // zLayout and zDrawSignature, the colour signature (the version holds them)
		std::shared_ptr<const org::LatchBlock> latch;
		std::uint32_t width = 0, height = 0;  // a face's render area
		float depthRange = 1.0f;              // the viewport's maximum depth
		std::uint64_t materialRows = 0, pipelineRows = 0;  // the main rows' tables (BuildDraws' RowsOf)
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
		// The forward range: forwardCapacity staging slots at forwardFirst, the sorted half after them; its table entry and bucket count
		// word forwardEntry (buckets.size()), drawn through the main colour signature. 0 capacity: none.
		std::uint32_t forwardFirst = 0, forwardCapacity = 0, forwardEntry = 0;
		// Per face, the frame push data: the colour segment's frame constants with VS and PS b12 the face's block.
		std::vector<std::array<std::uint32_t, kFramePushWords>> push;
		// Tree LOD: the forward pipeline and its draw's signature (invalid: none), the cull's groups.
		rhi::PipelineHandle tree{};
		rhi::CommandSignatureHandle treeSignature{};
		std::uint32_t treeGroups = 0;
		// The latched copies (LatchedCopiesPass): the faces' values from the latch, and the counters' zeros.
		std::array<LatchedList, 2> latched;
		// Not compared (implied by the latch block and the buckets): its layout, and each pipeline slot's bucket.
		ViewLatchLayout latchLayout;
		std::shared_ptr<const std::vector<std::uint32_t>> map;

		bool SameShape(const ViewFrame& o) const
		{
			return latch == o.latch && latched == o.latched && SameHandle(resourceHeap, o.resourceHeap) && SameHandle(samplerHeap, o.samplerHeap) &&
			       SameIndirect(indirect, o.indirect) && width == o.width && height == o.height && depthRange == o.depthRange && materialRows == o.materialRows &&
			       pipelineRows == o.pipelineRows && sequenceDraws == o.sequenceDraws && buckets == o.buckets && push == o.push && forwardFirst == o.forwardFirst &&
			       forwardCapacity == o.forwardCapacity && forwardEntry == o.forwardEntry && SameHandle(tree, o.tree) && SameHandle(treeSignature, o.treeSignature) &&
			       treeGroups == o.treeGroups;
		}
	};

	/**
	 * @brief What a view's buffers hold (Growths::Change, adopted with the revision that names it): a face's range of the sequences,
	 * a face's bucket counts in words, and the scene's tree LOD shape slots the faces' tree lists hold.
	 */
	struct ViewSizing
	{
		std::uint32_t sequenceDraws = 0;
		std::uint32_t bucketCountWords = 0;
		std::uint32_t treeShapeCapacity = 0;
		bool operator==(const ViewSizing&) const = default;
	};

	struct ViewResources : ViewSizing
	{
		const ViewTraits* traits = nullptr;
		// The main pass's (its depth inputs, targets, rows, visibility, frame constants) and, through it, the scene's.
		std::shared_ptr<Resources> main;
		Versioned sequences;  // faces * sequenceDraws
		std::shared_ptr<org::Buffer> count;
		std::array<Versioned, kMaxViewFaces> bucketCounts;
		std::shared_ptr<org::Buffer> blocks;  // kViewBlockBytes per face
		std::uint64_t blocksAddress = 0;
		// The reflection's: DCLF's depth for the faces (cleared per face) and the engine's cube target. First person's: the colour
		// epoch's depth, the engine's kPOST_ZPREPASS_COPY (FUN_1414cb6d0 binds it as the first-person pass's depth).
		std::shared_ptr<org::PixelBuffer> depth;
		std::shared_ptr<org::ExternalTextureResource> target;
		ID3D11Texture2D* targetTexture = nullptr;
		std::uint32_t width = 0, height = 0;
		// Tree LOD: per face a draw row (its visible list the face's) and a visible list, for the scene's shape slots.
		std::array<std::shared_ptr<org::Buffer>, kMaxViewFaces> treeRows;
		std::array<Versioned, kMaxViewFaces> treeVisible;
		std::array<std::uint64_t, kMaxViewFaces> treeRowsAddress{}, treeVisibleAddress{};
		// The passes are to be declared again (new main resources, new tree lists): at the view's own reserve, never mid-frame.
		bool rebuildPending = false;
		std::shared_ptr<const ComputeProgram> buildDraws;
		rhi::CommandSignaturePtr dispatchSignature;
		// The forward range's draw's explicit preprocess (its sort is the main resources' ForwardSortCS).
		std::shared_ptr<PreprocessStates> preprocessForward;
	};

	std::unique_ptr<org::RenderGraph::IRenderGraphExtension> MakeViewExtension(std::shared_ptr<ViewResources> a_resources);

	/**
	 * @brief A view's buckets: per pair of pipelines (the depth one only with a depth pass), the slots that draw with it, each slot's
	 * range what its objects can produce (the Z-prepass's bucket capacity); each slot's bucket (kNoBucket: none); and the forward
	 * range after them.
	 */
	struct ViewPlan
	{
		std::vector<std::uint32_t> map;
		std::vector<ViewFrame::Bucket> buckets;
		std::uint32_t draws = 0;  // a face's, all buckets and the forward range
		std::uint32_t forwardFirst = 0, forwardCapacity = 0;
	};
	/** @brief A pipeline slot's Z-prepass group (its lookup entry's set index's, IndirectState::zGroups); kNoBucket without one. */
	std::uint32_t ZGroupOf(const Lookups* a_lookups, const IndirectState& a_indirect, std::uint32_t a_slot);
	void PlanViewBuckets(const ViewTraits& a_traits, const MainSizing& a_main, const Lookups* a_lookups, const IndirectState& a_indirect,
		std::span<const rhi::PipelineHandle> a_slotPipelines, std::span<const std::uint8_t> a_forwardSlots, ViewPlan& a_out);

	/** @brief A view's latch, its layout and the zeros its latched copies take (the revision producer's: RevisionLatches). */
	struct ViewLatches
	{
		std::shared_ptr<org::LatchBlock> latch, zeros;
		ViewLatchLayout layout;
	};

	/** @brief What a view's shape is made from (MakeViewShape): its resources' sizing, the plan, the main rows and the latches. */
	struct ViewShapeInputs
	{
		rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
		IndirectState indirect{};
		ViewPlan plan;
		std::uint64_t materialRows = 0, pipelineRows = 0;
		const ViewSizing* sizing = nullptr;  // a revision's (Growths::RevisionSizing)
		const ViewLatches* latches = nullptr;
		std::uint64_t frameConstantsAddress = 0;  // the colour segment's, which the push data names
		std::uint32_t width = 0, height = 0;
		float depthRange = 1.0f;
		// Tree LOD in the faces: its forward pipeline and draw signature, and the scene's shape slots (none: invalid pipeline).
		rhi::PipelineHandle tree{};
		rhi::CommandSignatureHandle treeSignature{};
		std::uint32_t treeShapes = 0;
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
	 * pipeline through the colour signature): RasterForward, not refraction. None while a_on is false.
	 */
	void ViewForwardSlotsOf(const SceneTables& a_tables, bool a_on, std::vector<std::uint8_t>& a_slots);

	/** @brief A previous camera for PerFrameBlock's motion vector registers. */
	struct PreviousCamera
	{
		std::array<float, 16> viewProjUnjittered{};
		RE::NiPoint3 eye;
	};
	/**
	 * @brief VS_PerFrame (b12) for a camera, SunViews::kBlockBytes at a_out, as the engine's SetCameraData builds it: the frame's
	 * registers (a_frame, the renderer's last upload) with the camera's over them - CameraView, CameraProj, CameraViewProj and
	 * CameraViewProjUnjittered; c20 the projection's inverse and c24 the projection (the engine's order: FrameBuffer.hlsli names them
	 * the other way), c28 the view's inverse, c32 the view-projection's, c36 the projection's; CameraPosAdjust. a_jitter: the TAA /
	 * upscaler jitter added to the projection's third row (State::projectionPosScaleX/Y). a_previous: CameraPreviousViewProjUnjittered
	 * (c16) and CameraPreviousPosAdjust (c41); none, both zero (with fDRClampOffset in c41's w) and the dynamic resolution registers
	 * (c43, c44) from the renderer state, as for a reflection face.
	 */
	void PerFrameBlock(const SunViews::Cascade& a_camera, std::span<const std::byte> a_frame, bool a_jitter, const PreviousCamera* a_previous, std::byte* a_out);
	/** @brief The renderer's last VS_PerFrame upload: the mirror's, or Community Shaders' copy of it until the mirror has seen a write. */
	std::span<const std::byte> FrameBlockNow();
	/** @brief The inverse of a row-major 4x4 (cofactors, in double); zero when singular. */
	std::array<float, 16> Inverse(const std::array<float, 16>& a_m);

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
