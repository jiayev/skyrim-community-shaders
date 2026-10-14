#pragma once

// IndirectDraws::Impl's view state (ImplViews, a base of Impl): the reflection's and first person's frame state.
// Part of IndirectDraws' implementation: included by Impl.h, before the struct.

namespace DCLF
{
	// The views' frame state (ViewEpoch.cpp): the water reflection's and first person's, and their revision pipelines.
	struct ImplViews
	{
		/**
		 * @brief A view's frame state (ViewEpoch.cpp, render thread). The scene lane reads only the published LaneTargets, never
		 * `targets`. `covered` is decided at the frame's start, before the engine draws the view.
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
			std::shared_ptr<ViewResources> resources;
			bool setupFailed = false;
			bool covered = false;
		};
		// The water reflection's faces (Reflection.cpp; dclf-lod.md, "Water reflections"): the LOD forward programs and pipelines,
		// this update's captured faces, and the report counters.
		struct ReflectionState : ViewEpochState
		{
			ReflectionState() { covered = true; }
			std::uint32_t lodSlots = 0, programsReady = 0, pipelinesReady = 0;
			bool treeReady = false;
			rhi::PipelineHandle treePipeline{};
			bool treeOwned = false;  // the faces' tree LOD is DCLF's this frame (PassCapture::SetReflectionTreeLodOwned)
			// This update's faces (ReflectionFaceCamera), until ExecuteReflection takes them.
			struct Face
			{
				bool captured = false;
				std::array<std::byte, kViewBlockBytes> perFrame{};
				std::uint32_t perFrameBytes = 0;
				std::array<float, 16> viewProj{};
				RE::NiPoint3 eye;
			};
			std::array<Face, kReflectionFaces> faces;
			// The face the engine last oriented the camera to (observers); Policy::KeepReflectionRoots' updates.
			std::uint32_t orientedFace = ~0u;
			std::uint32_t residueUpdates = 0;
			winrt::com_ptr<ID3D11Texture2D> cube;  // the cube target the faces render into
			std::uint32_t width = 0, height = 0;
			// Since the last report.
			std::uint32_t facesCaptured = 0, updates = 0, epochs = 0, facesDrawn = 0;
			std::array<std::uint32_t, 6> skipped{};  // not drawable, not covered (DecideCoverage), no faces, stale inputs, no resources, the epoch failed
			// The last ExecuteReflection's frame, and faces captured after it that frame (expected none).
			std::uint32_t executedFrame = ~0u, lateFaces = 0;
		} reflection;
		// The first-person view (FirstPerson.cpp): its colour targets are kMAIN's format, the motion vectors' (colour2) and
		// kPOST_ZPREPASS_COPY's.
		struct FirstPersonState : ViewEpochState
		{
			std::uint32_t slots = 0, pipelinesReady = 0;
			// The slots the skeleton's geometry draws with (BeginFirstPersonFrame's walk), and the frame they were found.
			std::vector<std::uint8_t> slotsWanted;
			std::uint32_t slotsFrame = 0;
			// Main::Draw's cVar23 recomputed, its frame; the engine's depth range (RenderDepth's second argument).
			bool latch = false;
			std::uint32_t latchFrame = ~0u;
			float depthRange = 0.1f;
			// The first-person camera as the engine set it up (FUN_1414cb020), in world space; last frame's for the motion vectors.
			std::array<std::byte, kViewBlockBytes> perFrame{};
			std::array<float, 16> viewProj{};
			RE::NiPoint3 eye;
			PreviousCamera previous;
			bool havePrevious = false;
			// The frame whose depth epoch built the sequences the colour epoch draws.
			std::uint32_t depthFrame = ~0u;
			// Since the last report.
			std::uint64_t frames = 0, latched = 0, depthEpochs = 0, colourEpochs = 0;
			std::array<std::uint64_t, 7> skipped{};  // not drawn (latch, claims), not covered, no camera, stale inputs, no resources, failed, no depth epoch
			// Whether the main set's forward pipelines can draw into the first-person pass (kPOST_ZPREPASS_COPY's depth format the
			// main pass's), published for FirstPersonSlotReady; the installed tables' forward members, summed since the last report.
			std::atomic<bool> forwardCompatible{ false };
			std::uint64_t forwardMembers = 0;
			std::uint32_t forwardMembersNow = 0;  // the last walk's (every 16 frames)
		} firstPerson;
		ViewEpochState& View(ViewKind a_kind) { return a_kind == ViewKind::Reflection ? static_cast<ViewEpochState&>(reflection) : firstPerson; }
		const ViewEpochState& View(ViewKind a_kind) const { return a_kind == ViewKind::Reflection ? static_cast<const ViewEpochState&>(reflection) : firstPerson; }
		// Revision code, per view: each pipeline slot's forward pipeline from its request's catalog, the reflection's tree LOD
		// pipeline, first person's forward slots.
		std::array<std::vector<rhi::PipelineHandle>, kViews> viewRevisionPipelines;
		rhi::PipelineHandle viewRevisionTree{};
		std::vector<std::uint8_t> viewForwardSlots;
	};
}
