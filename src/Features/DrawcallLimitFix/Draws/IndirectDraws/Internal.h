#pragma once

// IndirectDraws' implementation, shared by the files of this folder only (IndirectDraws.h is the interface).

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#	include "Features/DrawcallLimitFix/Draws/IndirectDraws.h"
#	include "Features/DrawcallLimitFix/Common/AsyncWorker.h"
#	include "Features/DrawcallLimitFix/Engine/ConstantMirror.h"
#	include "Features/DrawcallLimitFix/Draws/DrawPipelines.h"
#	include "Features/DrawcallLimitFix/Draws/DrawPipelinesRhi.h"
#	include "Features/DrawcallLimitFix/Engine/EngineAccess.h"
#	include "Features/DrawcallLimitFix/Engine/EngineStates.h"
#	include "Features/DrawcallLimitFix/Engine/FaceSnapshots.h"
#	include "Features/DrawcallLimitFix/Scene/FadeState.h"
#	include "Features/DrawcallLimitFix/Draws/GpuResources.h"
#	include "Features/DrawcallLimitFix/Draws/GpuTextures.h"
#	include "Features/DrawcallLimitFix/Scene/LightingConstants.h"
#	include "Features/DrawcallLimitFix/Scene/MaterialSources.h"
#	include "Features/DrawcallLimitFix/Engine/PassCapture.h"
#	include "Features/DrawcallLimitFix/Engine/PrimaryCull.h"
#	include "Features/DrawcallLimitFix/Scene/SceneStore.h"
#	include "Features/DrawcallLimitFix/Draws/ShaderPrograms.h"
#	include "Features/DrawcallLimitFix/Engine/ShadowViews.h"
#	include "Features/DrawcallLimitFix/Engine/SunAccumulation.h"
#	include "RE/B/BSShadowLight.h"
#	include "RE/B/BSLightingShaderMaterialBase.h"
#	include "RE/B/BSShadowDirectionalLight.h"
#	include "RE/B/BSCullingProcess.h"
#	include "RE/N/NiCamera.h"
#	include "Features/DrawcallLimitFix/Common/Toggles.h"
#	include "Features/DrawcallLimitFix/Common/Switches.h"
#	include "Features/DrawcallLimitFix/Scene/VertexInput.h"
#	include "Deferred.h"
#	include "Features/LinearLighting.h"
#	include "Features/Skin.h"
#	include "Features/LightLimitFix/ORGLightCulling.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/DxvkOrgInterop.h"
#	include "RenderGraph/NvPerfBridge.h"
#	include "RenderGraph/PrefixSum.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "ShaderCache.h"
#	include "State.h"
#	include <OpenRenderGraph/PersistentGraphHost.h>
#	include <Render/BindingTable.h>
#	include <Render/LatchBlock.h>
#	include <Render/Runtime/ExternalSignalReservation.h>
#	include <Render/Runtime/StagedUploadBatch.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <Render/Runtime/DescriptorServiceAccess.h>
#	include <Render/Runtime/IDescriptorService.h>
#	include <Render/Runtime/UploadServiceAccess.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>
#	include <Resources/ExternalTextureResource.h>
#	include <Resources/PixelBuffer.h>
#	include <rhi_helpers.h>
#	include <bit>
#	include <chrono>
#	include <cstdlib>
#	include <cstddef>
#	include <filesystem>
#	include <fstream>
#	include <iterator>
#	include <map>
#	include <optional>
#	include <cstring>

#	include "GpuLayouts.h"
#	include "GrowableRows.h"

namespace DCLF
{
	namespace Draws
	{
		inline std::uint32_t CaptureViewIndex(const org::PassPrepareContext& a_preparation, const org::DeclaredViewToken& a_token)
		{
			(void)a_preparation.Capture(a_token);
			return a_preparation.Resolve(a_token).index;
		}

		// The format that reads a depth resource's depth aspect in a shader, as DXGI names it.
		inline rhi::Format DepthReadFormat(rhi::Format a_format)
		{
			switch (a_format) {
			case rhi::Format::D24_UNorm_S8_UInt:
			case rhi::Format::R24G8_Typeless:
				return rhi::Format::R24_UNorm_X8_Typeless;
			case rhi::Format::D32_Float_S8X24_UInt:
			case rhi::Format::R32G8X24_Typeless:
				return rhi::Format::R32_Float_X8X24_Typeless;
			case rhi::Format::D32_Float:
				return rhi::Format::R32_Float;
			case rhi::Format::D16_UNorm:
				return rhi::Format::R16_UNorm;
			default:
				return a_format;
			}
		}

		// A draw's ExecuteIndirect max count: a power of two that only grows, so the recording settles while
		// the preprocess memory stays near what the frame draws (the GPU count buffer says how many run).
		inline std::uint32_t GrowCapacity(std::uint32_t a_current, std::uint32_t a_needed, std::uint32_t a_limit)
		{
			if (a_needed <= a_current)
				return a_current;
			return std::min(a_limit, std::max(256u, std::bit_ceil(a_needed)));
		}

		template <class H>
		bool SameHandle(const H& a, const H& b)
		{
			return a.index == b.index && a.generation == b.generation;
		}

		inline bool SameIndirect(const IndirectState& a, const IndirectState& b)
		{
			if (a.valid != b.valid || !SameHandle(a.layout, b.layout))
				return false;
			for (std::size_t i = 0; i < a.sets.size(); ++i)
				if (!SameHandle(a.sets[i], b.sets[i]) || !SameHandle(a.signatures[i], b.signatures[i]))
					return false;
			return SameHandle(a.depthPassSignature, b.depthPassSignature);
		}

		// The dispatch signature every BuildDraws pass records with: one Dispatch argument, read from a latch.
		inline rhi::CommandSignaturePtr CreateDispatchSignature(rhi::Device a_device, rhi::PipelineLayoutHandle a_layout)
		{
			rhi::IndirectArg args[] = { { .kind = rhi::IndirectArgKind::Dispatch } };
			rhi::CommandSignaturePtr signature;
			if (a_device.CreateCommandSignature(rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(args, 1), sizeof(std::uint32_t) * 3 }, a_layout, signature) != rhi::Result::Ok)
				return {};
			return signature;
		}

		// CS_DCLF_BINDLESS_PARITY: the per-object record against the packed constant group, variable by
		// variable. Both are produced from tables.objects and tables.shading by the same rules, so the
		// comparison is exact rather than tolerant - a tolerance here would only hide a layout mistake.
		void CheckBindlessRecord(const SceneStore::Tables& a_tables, std::uint32_t a_objectIndex, const BindlessObject& a_record, const RE::NiPoint3& a_eye,
			const RE::NiPoint3& a_previousEye, const GeometryPatchOffsets& a_offsets, std::span<const std::byte> a_vs, std::span<const std::byte> a_ps,
			IndirectDraws::Stats& a_stats);

		inline std::shared_ptr<org::Buffer> CreateWords(std::uint64_t a_words, bool a_unorderedAccess, const char* a_name)
		{
			auto buffer = org::Buffer::CreateUnmaterializedStructuredBuffer(static_cast<std::uint32_t>(a_words), sizeof(std::uint32_t), a_unorderedAccess);
			buffer->SetName(a_name);
			buffer->Materialize();
			return buffer;
		}

		// Constant buffers the native draw rebinds per object (b0-b2 are the Lighting groups): everything
		// else bound in the main pass is per frame and comes from its CPU mirror.
		constexpr std::uint32_t kPerDrawVS = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 4) | (1u << 9) | (1u << 10);
		// b7 is Advanced Skin's SkinPerGeometry: bound per draw, for the actor the engine drew last (kSkinRegister).
		constexpr std::uint32_t kPerDrawPS = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 7) | (1u << 8) | (1u << 11);
		// Bytes per texel, for printing exactly the pixel a readback holds and nothing beyond it.
		inline std::uint32_t FormatBytes(DXGI_FORMAT a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_R32G32B32A32_FLOAT:
				return 16;
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
			case DXGI_FORMAT_R16G16B16A16_UNORM:
			case DXGI_FORMAT_R32G32_FLOAT:
				return 8;
			case DXGI_FORMAT_R16_FLOAT:
			case DXGI_FORMAT_R16_UNORM:
			case DXGI_FORMAT_D16_UNORM:
				return 2;
			case DXGI_FORMAT_R8_UNORM:
				return 1;
			default:
				return 4;  // the 32-bit packed formats the G-buffer uses (RGBA8, R10G10B10A2, R11G11B10)
			}
		}

		// The probe's sample points, in the order they happen within a frame.
		constexpr const char* kProbeFirstLabel = "before z-prepass";

		constexpr std::uint32_t kLinearLightingRegister = 8;  // LLPerGeometry: Linear Lighting's per-object emissive multiplier
		constexpr std::uint32_t kSkinRegister = 7;             // SkinPerGeometry: Advanced Skin's per-object wetness
		constexpr std::uint32_t kStrictLightDataBytes = 1216;  // LightLimitFix.hlsli StrictLightData (15 lights)
		constexpr std::uint32_t kLightsRegister = 35;          // t35-t37: Light Limit Fix's lights, list and grid
		constexpr std::uint32_t kCharacterLightMaterialRegister = 11;  // the engine's register of the character light's noise (kCharacterLightRegister)
		constexpr std::uint32_t kInvalidIndex = GpuTextures::kInvalid;

		/**
		 * @brief What one segment's passes record against: everything their recorded commands depend on,
		 * and nothing that changes every frame. The frame's own values (the counts, the view-projection, the
		 * culling stamp) are in the segment's BuildDrawsLatch, and the draw counts on the GPU. Published by
		 * the commit before the epoch prepares and replaced only when it changes, so its identity is what
		 * the passes' invocation revisions carry.
		 */
		struct PassFrame
		{
			std::uint64_t generation = 0;
			// ExecuteIndirect max counts (GrowCapacity, within the buffer's ranges): the phase-1/colour draws, and each decal group.
			std::uint32_t drawCapacity = 0;
			std::array<std::uint32_t, kDecalGroups> decalCapacity{};
			// The sequence buffer's ranges as the epoch sized it (Resources::sequenceDraws, sequenceDecals): its layout.
			std::uint32_t sequenceDraws = 0, sequenceDecals = 0;
			// The rows' tables' addresses (Resources::materialRows, pipelineRows), which a growth moves.
			std::uint64_t materialRows = 0, pipelineRows = 0;
			std::uint32_t cullMode = 0;
			bool probePixel = false;
			std::uint32_t probeX = 0, probeY = 0;
			std::uint32_t width = 0, height = 0;  // render area: the main pass viewport
			float minDepth = 0.0f, maxDepth = 1.0f;  // its depth range (the engine uses [0, 0.999998])
			rhi::DescriptorHeapHandle resourceHeap{};
			rhi::DescriptorHeapHandle samplerHeap{};
			IndirectState indirect{};
			// The latch block the epoch wrote (Resources::latch): a colour epoch with more cascades than it holds makes a new one
			// (ReserveMainLatch), and a frame in flight keeps reading its own.
			std::shared_ptr<const org::LatchBlock> latch;

			bool SameShape(const PassFrame& o) const
			{
				return drawCapacity == o.drawCapacity && decalCapacity == o.decalCapacity && sequenceDraws == o.sequenceDraws &&
				       sequenceDecals == o.sequenceDecals && materialRows == o.materialRows && pipelineRows == o.pipelineRows && cullMode == o.cullMode &&
				       probePixel == o.probePixel && probeX == o.probeX && probeY == o.probeY && width == o.width && height == o.height &&
				       minDepth == o.minDepth && maxDepth == o.maxDepth && SameHandle(resourceHeap, o.resourceHeap) &&
				       SameHandle(samplerHeap, o.samplerHeap) && SameIndirect(indirect, o.indirect) && latch == o.latch;
			}
		};

		// Index of a drawing segment's shape (Resources::frames).
		constexpr std::size_t kDepthShape = 0, kColourShape = 1;

		// A CPU-written buffer the main pass reads as a structured buffer SRV (t16 and up): the graph reads a
		// copy, refilled every epoch from the buffer's CPU mirror.
		struct FrameBuffer
		{
			std::uint32_t textureRegister = 0;
			std::uint32_t stride = 0;
			std::uint32_t firstElement = 0;
			std::uint32_t elements = 0;
			std::shared_ptr<org::Buffer> copy;

			bool SameShape(const FrameBuffer& a_other) const
			{
				return textureRegister == a_other.textureRegister && stride == a_other.stride && firstElement == a_other.firstElement && elements == a_other.elements;
			}
		};

		/**
		 * @brief The versions of the kept tables (ObjectRecordStore, BonesStore, GeometryStore) a set of buffers holds, written
		 * by the commit that uploads them; 0 for new buffers, which no version is.
		 */
		struct TablesHeld
		{
			std::uint64_t objects = 0, bones = 0, geometries = 0;
			bool operator==(const TablesHeld&) const = default;
		};

		struct SortSequencesConstants
		{
			std::uint32_t stagingIndex;
			std::uint32_t ranksIndex;
			std::uint32_t offsetsIndex;
			std::uint32_t countIndex;
			std::uint32_t sequencesIndex;
			std::uint32_t sequenceStride;  // bytes (sizeof(DrawSequence))
			std::uint32_t drawLimit;       // the draws' range (BuildDrawsConstants::phaseTwoBase)
			std::uint32_t padding;
		};
		constexpr std::uint32_t kSortSequencesConstantWords = sizeof(SortSequencesConstants) / 4;
		constexpr std::uint32_t kSortSequencesGroup = 64;  // SortSequencesCS.hlsl's
		// A sort's rank word holds the pipeline in its top 12 bits and the rank below (BuildDrawsCS.hlsl, kSortRankBits): a
		// draw range past this is a hard failure until the ranks get a word of their own.
		constexpr std::uint32_t kSortRankLimit = 1u << 20;
		static_assert(DrawPipelines::kMaxPipelines <= (1u << 12));

		/**
		 * @brief The sort by pipeline (SortDraws) of phase 1's and the colour segment's sequences. BuildDraws stages the sequences
		 * and counts them per pipeline, PrefixSum turns the counts into each pipeline's first slot, and SortSequencesPass
		 * (SortSequencesCS.hlsl) writes the sequences grouped by pipeline. The counts are zeroed once, by the first commit
		 * (countsZeroed), and after that by the scan that reads them.
		 *
		 * Not the shadow views': sorting them (a part of these buffers per view slot, one scan over all of them) left their draw
		 * time within noise at Riverwood (1.28-1.32 against 1.23-1.29 ms) while the scan of 16 views' counts cost 0.08-0.10 ms.
		 */
		struct DrawSort
		{
			std::shared_ptr<org::Buffer> counts, offsets, blockSums, staging, ranks;
			std::shared_ptr<const PrefixSum::Programs> prefixSum;
			std::shared_ptr<const ComputeProgram> scatter;
			bool countsZeroed = false;  // render thread

			static constexpr std::uint32_t kKeys = DrawPipelines::kMaxPipelines;

			/** @brief Null, logged, when either program could not be created. Staging and ranks for a_draws (they grow with the sequences). */
			static std::shared_ptr<DrawSort> Create(rhi::Device a_device, std::uint32_t a_draws)
			{
				auto sort = std::make_shared<DrawSort>();
				sort->prefixSum = PrefixSum::Load(a_device);
				sort->scatter = ComputeProgram::Load(a_device, { .source = kSortSequencesShader, .constantWords = kSortSequencesConstantWords });
				if (!sort->prefixSum || !sort->scatter) {
					logger::warn("[DCLF] The draws are not sorted by pipeline: its programs could not be created");
					return {};
				}
				sort->counts = CreateWords(kKeys, true, "cs.dclf.sort-counts");
				sort->offsets = CreateWords(kKeys, true, "cs.dclf.sort-offsets");
				sort->blockSums = CreateWords(PrefixSum::Blocks(kKeys), true, "cs.dclf.sort-block-sums");
				sort->staging = CreateWords(std::uint64_t(a_draws) * sizeof(DrawSequence) / 4, true, "cs.dclf.sort-staging");
				sort->ranks = CreateWords(a_draws, true, "cs.dclf.sort-ranks");
				return sort;
			}

			void Register(org::RenderGraph& a_graph) const
			{
				for (const auto& buffer : { counts, offsets, blockSums, staging, ranks })
					a_graph.RegisterResource(org::ResourceIdentifier(buffer->GetName()), buffer);
			}

			/** @brief The scan, which runs in every execution of its epoch, so the counts it clears stay zero whether or not a build ran. */
			std::shared_ptr<org::RenderPass> ScanPass() const
			{
				PrefixSum::Desc scan{};
				scan.programs = prefixSum;
				scan.counts = counts;
				scan.offsets = offsets;
				scan.blockSums = blockSums;
				scan.elements = kKeys;
				scan.clearCounts = true;
				return PrefixSum::CreatePass(std::move(scan));
			}

			template <class Uploads>
			void ZeroCountsOnce(Uploads& a_uploads)
			{
				if (countsZeroed)
					return;
				static const std::array<std::uint32_t, kKeys> zeros{};
				a_uploads(counts, zeros.data(), sizeof(zeros), 0);
				countsZeroed = true;
			}
		};

		struct PassStats;

		/**
		 * @brief The state lists of one recording site's explicit DGC preprocesses (DrawPipelines.h), one per frame slot:
		 * graphics lists that are never submitted. vkCmdPreprocessGeneratedCommandsEXT is recorded outside any pass but
		 * generates for the state of another list, render pass included, which the execution must then match exactly; so
		 * a site begins each of its passes on its slot's list and sets it up as the execution will (heaps, layout, topology,
		 * push data) before preprocessing that pass's calls against it. A site of its own for every pass that can record at
		 * the same time as another.
		 */
		class PreprocessStates
		{
		public:
			static std::shared_ptr<PreprocessStates> Create(rhi::Device a_device, std::uint32_t a_frameSlots, const char* a_site)
			{
				auto states = std::make_shared<PreprocessStates>();
				states->slots.resize(std::max<std::uint32_t>(a_frameSlots, 1u));
				for (auto& slot : states->slots) {
					if (rhi::Failed(a_device.CreateCommandAllocator(rhi::QueueKind::Graphics, slot.allocator)) || !slot.allocator ||
						rhi::Failed(a_device.CreateCommandList(rhi::QueueKind::Graphics, slot.allocator.Get(), slot.list)) || !slot.list) {
						logger::error("[DCLF] DGC preprocess: no state list for {}; its draws cannot execute", a_site);
						return nullptr;
					}
				}
				return states;
			}

			// The frame slot's list, reset and recording.
			rhi::CommandList& Begin(std::uint32_t a_frameSlot)
			{
				ZoneScopedN("CS.DCLF.Record.RecyclePreprocessState");
				auto& slot = slots[a_frameSlot % slots.size()];
				slot.list->Recycle(slot.allocator.Get());
				return slot.list.Get();
			}

		private:
			struct Slot
			{
				rhi::CommandAllocatorPtr allocator;
				rhi::CommandListPtr list;
			};
			std::vector<Slot> slots;
		};

		/**
		 * @brief The scene's tables on the GPU, one set for every epoch - the shadow views', Skylighting's, the Z-prepass's and
		 * the colour segment's: the object records (t127), the bone rows (t126), the geometry table (BuildDraws' draws) and the
		 * NPC face shapes' positions (the face draws' second stream). Kept by one store each (Impl::objectStore, boneStore,
		 * geometryStore), whose builds run in frame order - the shadow build, the Z-prepass's, the colour build's, each joined
		 * before the next is kicked - so each commit sends what changed since the version the buffers hold, which the commit
		 * before it wrote. Created with the first epoch's resources and kept across their recreation.
		 */
		struct SceneBuffers
		{
			std::shared_ptr<org::Buffer> objects, bones, geometries, facePositions;
			// Their SRVs' descriptor heap indices, and the positions' address: a growth gives the buffer new ones (the old ones
			// are retired once the GPU is done with them), so every build takes them from here, after ReserveSceneTables.
			std::uint32_t objectsIndex = 0, bonesIndex = 0;
			std::uint64_t facePositionsAddress = 0;
			// What each holds (GpuLayouts.h, kInitialObjects), grown by ReserveSceneTables; `generation` counts the growths, so a
			// batch staged before one is not submitted after it.
			std::uint32_t objectCapacity = 0, geometryRows = 0, boneRows = 0, faceVertices = 0;
			std::uint64_t generation = 0;
			// The same count, for the invocation revisions of the passes that bind these buffers (read on the graph host's thread,
			// which prepares invocations ahead of the commits).
			std::atomic<std::uint64_t> layout{ 0 };
			std::uint32_t growths = 0;  // since the last report
			ankerl::unordered_dense::map<std::uint32_t, std::uint64_t> faceUploaded;  // region -> generation (render thread)
			// The versions of the stores' records, rows and slots the buffers hold, written by the commit that uploads them.
			TablesHeld held;
			// Tree wind (TreeWindCS; Records.h, TreeStatic): the tree rows and their clocks by tree slot, and the members drawing
			// under a tree. The rows and the list are the commits' uploads (UploadTrees), against the tables' versions; the
			// clocks are the GPU's alone. The counts and the frame's inputs are what the passes dispatch with (the last commit's).
			std::shared_ptr<org::Buffer> trees, treeClocks, treeObjects;
			std::shared_ptr<org::Buffer> treeFrameBuffer;  // TreeWindFrameRow, one row, every commit's
			std::uint32_t treeCapacity = 0, treeObjectCapacity = 0;
			std::uint64_t treesHeld = ~0ull, treeObjectsHeld = ~0ull;
			std::uint32_t treeCount = 0, treeObjectCount = 0;
			std::uint32_t treeFrame = 0;  // the scene frame of treeInputs
			TreeWindFrame treeInputs{};
			std::shared_ptr<const ComputeProgram> treeWind;
			// Fade roots (FadeStateCS; Records.h, FadeRootStatic): the static rows by root slot (the commits' uploads, against
			// Tables::fadeRootsVersion), the GPU's state rows, the frame's inputs (a one-row buffer the depth commit writes once
			// a frame), and CS_DCLF_FADE_PARITY's log (kFadeLogEntries roots from fadeLogBase, ~0u: none this frame).
			std::shared_ptr<org::Buffer> fadeRoots, fadeStates, fadeFrameBuffer, fadeLog;
			std::shared_ptr<org::Buffer> fadeVisibility;  // the main camera's cull test (Records.h, kFadeVisibilityBytes)
			std::uint32_t fadeRootCapacity = 0;
			std::uint64_t fadeRootsHeld = ~0ull;
			std::uint32_t fadeRootCount = 0;
			std::uint32_t fadeFrameNumber = 0;  // the scene frame of fadeFrame
			FadeFrame fadeFrame{};
			std::uint32_t fadeLogBase = ~0u;
			std::shared_ptr<const ComputeProgram> fadeState;
		};

		/**
		 * @brief The depth commit's fade uploads (render thread): the static rows where the buffer does not hold the tables'
		 * version, whole, and the frame's inputs once a frame. a_logBase: the parity log's first root this frame (~0u: none).
		 */
		template <class Uploads>
		void UploadFadeRoots(const SceneStore::Tables& a_tables, std::uint32_t a_frame, const FadeFrame& a_inputs, std::uint32_t a_logBase, SceneBuffers& a_scene,
			Uploads& a_uploads, const std::vector<std::byte>& a_visibility)
		{
			if (!a_scene.fadeState || !a_scene.fadeRoots)
				return;
			if (a_scene.fadeRootsHeld != a_tables.fadeRootsVersion && a_tables.fadeRoots.size() <= a_scene.fadeRootCapacity) {
				if (!a_tables.fadeRoots.empty())
					a_uploads(a_scene.fadeRoots, a_tables.fadeRoots.data(), a_tables.fadeRoots.size() * sizeof(FadeRootStatic), 0);
				a_scene.fadeRootsHeld = a_tables.fadeRootsVersion;
			}
			a_scene.fadeRootCount = a_scene.fadeRootsHeld == a_tables.fadeRootsVersion ? static_cast<std::uint32_t>(a_tables.fadeRoots.size()) : 0u;
			if (a_scene.fadeFrameNumber != a_frame) {
				a_scene.fadeFrameNumber = a_frame;
				a_scene.fadeFrame = a_inputs;
				if (a_scene.fadeVisibility && a_visibility.size() == kFadeVisibilityBytes)
					a_uploads(a_scene.fadeVisibility, a_visibility.data(), a_visibility.size(), 0);
				a_scene.fadeLogBase = a_logBase;
			}
			// The frame row, with the pass's per-frame values, every commit.
			a_scene.fadeFrame.rootCount = a_scene.fadeRootCount;
			a_scene.fadeFrame.sceneFrame = a_scene.fadeFrameNumber;
			a_scene.fadeFrame.logBase = a_scene.fadeLogBase;
			a_uploads(a_scene.fadeFrameBuffer, &a_scene.fadeFrame, sizeof(FadeFrame), 0);
		}

		/**
		 * @brief A commit's tree uploads (render thread): the rows and the list where the buffers do not hold the tables'
		 * versions, whole (they change when a tree member joins or leaves), and the frame's inputs once a frame.
		 */
		template <class Uploads>
		void UploadTrees(const SceneStore::Tables& a_tables, std::uint32_t a_frame, SceneBuffers& a_scene, Uploads& a_uploads)
		{
			if (!a_scene.treeWind || !a_scene.trees)
				return;
			if (a_scene.treesHeld != a_tables.treesVersion && a_tables.trees.size() <= a_scene.treeCapacity) {
				if (!a_tables.trees.empty())
					a_uploads(a_scene.trees, a_tables.trees.data(), a_tables.trees.size() * sizeof(TreeStatic), 0);
				a_scene.treesHeld = a_tables.treesVersion;
			}
			if (a_scene.treeObjectsHeld != a_tables.treeObjectsVersion && a_tables.treeObjects.size() <= a_scene.treeObjectCapacity) {
				if (!a_tables.treeObjects.empty())
					a_uploads(a_scene.treeObjects, a_tables.treeObjects.data(), a_tables.treeObjects.size() * sizeof(TreeObject), 0);
				a_scene.treeObjectsHeld = a_tables.treeObjectsVersion;
			}
			a_scene.treeCount = a_scene.treesHeld == a_tables.treesVersion ? static_cast<std::uint32_t>(a_tables.trees.size()) : 0u;
			a_scene.treeObjectCount = a_scene.treeObjectsHeld == a_tables.treeObjectsVersion ? static_cast<std::uint32_t>(a_tables.treeObjects.size()) : 0u;
			if (a_scene.treeFrame != a_frame) {
				a_scene.treeFrame = a_frame;
				a_scene.treeInputs = SampleTreeWindFrame();
			}
			// The frame row (TreeWindFrameRow), every commit: the pass's invocation is prepared ahead of it.
			if (a_scene.treeFrameBuffer) {
				const TreeWindFrameRow row{ a_scene.treeCount, a_scene.treeObjectCount, a_scene.treeFrame, 0, a_scene.treeInputs, {} };
				a_uploads(a_scene.treeFrameBuffer, &row, sizeof(row), 0);
			}
		}

		struct Resources
		{
			std::vector<FrameBuffer> frameBuffers;
			// The main pass's rows (DrawPipelines.h, kMaterialRowBytes / kPipelineRowBytes), one table each for both segments,
			// indexed by the scene's material and pipeline slots; grown before an epoch to the tables' slot counts
			// (Impl::ReserveMainSequences). The versions of the kept rows (MainRows) they hold: 0 in a new backing.
			GrowableRows materialRows, pipelineRows;
			std::uint64_t materialRowsHeld = 0, pipelineRowsHeld = 0;
			// The scene's tables, every epoch's (SceneBuffers): the object records, bone rows, geometry table and face positions.
			std::shared_ptr<SceneBuffers> scene;
			std::shared_ptr<org::Buffer> inputs, sequences, count;  // BuildDraws: in, out, out (and the scene's geometries)
			// The objects the per-object buffers hold (inputs, inputsDepth, visibility, frustum): the scene's
			// object capacity, grown with it (ReserveObjectBuffers).
			std::uint32_t objectCapacity = 0;
			// The sequence buffer's ranges (GpuLayouts.h, SequenceSlots): draws per draw range (phase 1 and colour, phase 2) and per
			// decal group. Grown before an epoch to hold every draw the scene can produce (Impl::ReserveMainSequences).
			std::uint32_t sequenceDraws = 0, sequenceDecals = 0;
			// The Z-prepass segment's draw inputs: each main segment keeps its resident region at the head of its own buffer
			// (the colour segment's is `inputs`), and the version of the region the buffer holds, per segment (0 Z-prepass,
			// 1 colour), written by the commit that uploads it.
			std::shared_ptr<org::Buffer> inputsDepth;
			std::array<std::uint64_t, 2> residentUploaded{};
			// The frame lighting version (SceneStore::Tables::frameLightingVersion) its frame slot holds (kFrameSlotLighting).
			std::uint32_t frameLightingUploaded = 0;
			std::shared_ptr<const ComputeProgram> buildDraws;
			winrt::com_ptr<ID3D11Buffer> sequencesD3D11, countD3D11;  // CS_DCLF_BUILD_PARITY readback
			winrt::com_ptr<ID3D11Buffer> visibilityD3D11;              // CS_DCLF_SET_PARITY readback
			// The per-frame constant blocks at fixed slots (FrameSlotOffset), so a build can name them before
			// their contents exist.
			std::shared_ptr<org::Buffer> frameConstants;
			std::uint64_t frameConstantsAddress = 0;
			// The main pass's own targets and depth, imported: DCLF draws into them, and the native loop skips the
			// objects it drew (DrawcallLimitFix's RenderPassImmediately hooks).
			std::uint32_t targetCount = 0;
			std::array<std::shared_ptr<org::ExternalTextureResource>, kColorTargets> native;
			std::shared_ptr<org::ExternalTextureResource> nativeDepth;
			// CS_DCLF_GBUFFER_PROBE: one texel of every target, copied inside the epoch both before and
			// after the colour draws. The copies are graph passes so the graph orders them against the
			// draws; a D3D11 readback issued around the epoch is not ordered against ORG's submissions at
			// all, and silently reports that nothing changed.
			std::shared_ptr<org::Buffer> probe;
			winrt::com_ptr<ID3D11Buffer> probeD3D11;
			// Phase 4's hierarchical depth buffer. Its source is nativeDepth, which is imported with a
			// shader-resource view as well as a depth-stencil one so that the graph sees the depth the draws
			// write and the depth the build reads as one resource and orders them.
			std::shared_ptr<org::Buffer> visibility;  // per-object verdict, published by the depth segment
			// Per object, the stamp of the last frame the main camera's depth phase 1 found its bound inside the frustum
			// (occlusion aside: the engine's OnVisible semantics). Never cleared: a stale stamp is simply not this frame's.
			std::shared_ptr<org::Buffer> frustum;
			std::shared_ptr<PassStats> passStats;  // CS_DCLF_PASS_STATS
			// The explicit DGC preprocesses' state list of the colour segment's passes. The depth pass
			// has none: IndirectState::depthPassSignature.
			std::shared_ptr<PreprocessStates> preprocessMain;
			std::shared_ptr<org::PixelBuffer> hzb;
			std::shared_ptr<const ComputeProgram> hzbProgram;
			std::uint32_t hzbWidth = 0, hzbHeight = 0, hzbMips = 0;
			std::uint32_t width = 0, height = 0;
			bool lightLimitFix = false;  // LLF's graph buffers are registered (they are read at t35-t37)
			// Per drawing segment (kDepthShape, kColourShape): the shape its passes record against, null while
			// the segment has nothing to draw this epoch. published keeps the last one across the null.
			std::array<std::atomic<std::shared_ptr<const PassFrame>>, 2> frames;
			std::array<std::shared_ptr<const PassFrame>, 2> published;
			std::uint64_t shapeGenerations = 0;
			// Per frame slot, one BuildDrawsLatch (all BuildDraws dispatches of an epoch share the values), then the colour pass's
			// cascades (latchLayout, grown by ReserveMainLatch).
			std::shared_ptr<org::LatchBlock> latch;
			MainLatchLayout latchLayout;
			rhi::CommandSignaturePtr dispatchSignature;
			// The sort by pipeline of phase 1's and the colour segment's sequences (one view); null when it is off.
			std::shared_ptr<DrawSort> sort;
		};

		struct PassBindings
		{
			std::array<org::DeclaredViewToken, kColorTargets> targets{};
			org::DeclaredViewToken depth;
			org::ResourceBindingToken sequences, count, materialRows, pipelineRows, objects, bones;
			org::ResourceBindingToken lights, lightIndexList, lightGrid;
			std::vector<org::ResourceBindingToken> frameBuffers;
		};

		/**
		 * @brief CS_DCLF_PASS_STATS=1: pipeline statistics (input vertices and primitives, vertex and pixel shader
		 * invocations) of the main epochs' draw passes, per frame slot, read back when the slot comes round again: the counter
		 * check that DCLF's passes draw and depth-test what they should (the colour pass's pixel shader invocations, above all).
		 */
		struct PassStats
		{
			enum Kind : std::uint32_t
			{
				kDepth,
				kDepthPhaseTwo,
				kColour,
				kKinds
			};
			static constexpr std::uint32_t kFields = 4;  // IA vertices, IA primitives, VS invocations, PS invocations
			rhi::QueryPoolPtr pool;
			std::vector<std::shared_ptr<org::Buffer>> readback;  // per frame slot, kKinds results
			std::vector<std::array<bool, kKinds>> written;
			std::mutex mutex;
			std::array<std::array<std::uint64_t, kFields>, kKinds> totals{};
			std::array<std::uint32_t, kKinds> samples{};

			static bool Enabled()
			{
				return SwitchEnabled(Switch::PassStats);
			}

			/** @brief The slot's results from its last use, then its query reset; before the pass begins. */
			void Start(rhi::CommandList& a_commands, std::uint32_t a_slot, Kind a_kind)
			{
				if (a_slot >= readback.size())
					return;
				const std::lock_guard lock(mutex);
				if (written[a_slot][a_kind]) {
					auto resource = readback[a_slot]->GetAPIResource();
					void* mapped = nullptr;
					resource.Map(&mapped);
					if (mapped) {
						const auto* values = static_cast<const std::uint64_t*>(mapped) + std::size_t(a_kind) * kFields;
						for (std::uint32_t f = 0; f < kFields; ++f)
							totals[a_kind][f] += values[f];
						++samples[a_kind];
						resource.Unmap(0, 0);
					}
				}
				if (a_kind == kColour && samples[kColour] >= 300) {
					static const char* kNames[kKinds] = { "Z-prepass", "Z-prepass phase 2", "colour" };
					std::string text;
					for (std::uint32_t k = 0; k < kKinds; ++k) {
						if (!samples[k])
							continue;
						const double n = samples[k];
						text += fmt::format("; {}: {:.0f} vertices, {:.0f} primitives, {:.0f} VS invocations, {:.0f} PS invocations", kNames[k], totals[k][0] / n,
							totals[k][1] / n, totals[k][2] / n, totals[k][3] / n);
					}
					logger::info("[DCLF] pass statistics, per frame{}", text);
					totals = {};
					samples = {};
				}
				a_commands.ResetQueries(pool->GetHandle(), a_slot * kKinds + a_kind, 1);
			}
			void Begin(rhi::CommandList& a_commands, std::uint32_t a_slot, Kind a_kind) const
			{
				if (a_slot < readback.size())
					a_commands.BeginQuery(pool->GetHandle(), a_slot * kKinds + a_kind);
			}
			void End(rhi::CommandList& a_commands, std::uint32_t a_slot, Kind a_kind) const
			{
				if (a_slot < readback.size())
					a_commands.EndQuery(pool->GetHandle(), a_slot * kKinds + a_kind);
			}
			/** @brief After the pass ends. */
			void Resolve(rhi::CommandList& a_commands, std::uint32_t a_slot, Kind a_kind)
			{
				if (a_slot >= readback.size())
					return;
				a_commands.ResolveQueryData(pool->GetHandle(), a_slot * kKinds + a_kind, 1, readback[a_slot]->GetAPIResource().GetHandle(),
					std::uint64_t(a_kind) * kFields * sizeof(std::uint64_t));
				const std::lock_guard lock(mutex);
				written[a_slot][a_kind] = true;
			}
		};

		// Frame push: the pass-wide constant buffers' addresses, as the layout's push address ranges read them.
		inline std::array<std::uint32_t, kFramePushWords> FramePushWords(std::uint64_t a_frameConstants);

		struct PreparedDraws
		{
			PassStats* stats = nullptr;  // CS_DCLF_PASS_STATS
			PreprocessStates* preprocess = nullptr;  // the frame slot's explicit DGC preprocess state list
			std::array<std::uint32_t, kFramePushWords> framePushWords{};
			std::shared_ptr<const PassFrame> frame;
			std::array<org::PreparedDescriptorReference, kColorTargets> targetViews{};
			org::PreparedDescriptorReference depthView{};
			std::uint32_t targetCount = 0;
			bool phaseTwo = false;
			bool zPrepass = false;
		};

		// The shape of the segment a pass serves, or null when that segment does not draw (or, without epochs,
		// when the current segment is not a drawing one).
		inline std::shared_ptr<const PassFrame> CurrentFrame(const Resources& a_resources, RenderGraphRuntime::Segment a_segment)
		{
			if (a_segment == RenderGraphRuntime::Segment::ZPrepass)
				return a_resources.frames[kDepthShape].load(std::memory_order_acquire);
			if (a_segment == RenderGraphRuntime::Segment::MainOpaque)
				return a_resources.frames[kColourShape].load(std::memory_order_acquire);
			return nullptr;
		}

		static_assert(DrawPipelines::kMaxPipelines == 4096, "BuildDrawsCS.hlsl's kSortKeys");

		/** @brief Whether BuildDraws runs in the segment (BuildDrawsPass::Prepare's conditions), which is when a sort follows it. */
		inline bool BuildsDraws(const Resources& a_resources, RenderGraphRuntime::Segment a_segment)
		{
			return a_resources.buildDraws && a_resources.latch && a_resources.dispatchSignature && CurrentFrame(a_resources, a_segment);
		}

		/**
		 * @brief Folds the camera translation into the view-projection, so that a bound's absolute world
		 * position can be projected without carrying the camera separately.
		 *
		 * The draws are camera-relative (the engine's posAdjust), and a bound is absolute: the camera is subtracted
		 * here, one matrix column, instead of from every bound, which is exact and keeps the camera out of a
		 * push-constant block at Vulkan's guaranteed 128 bytes.
		 *
		 * Row-major, as the shader's `mul(M, v)` reads it: M'(p,1) = M(p-e,1) needs only column 3 changed,
		 * by subtracting M's upper-left 3x3 applied to the camera.
		 */
		void FoldEyeIntoViewProj(const std::array<float, 16>& a_viewProj, const RE::NiPoint3& a_eye, float (&a_out)[16]);

		// Records one BuildDrawsCS dispatch whose values and group count are the slot's latch at a_latchOffset.
		void RecordLatchedDispatch(BuildDrawsConstants a_constants, const org::LatchBlock& a_latch,
			rhi::CommandSignatureHandle a_signature, std::uint32_t a_latchOffset, org::PassRecordContext& a_recording);

		struct HzbConstants
		{
			std::uint32_t sourceIndex;
			std::uint32_t targetIndex;
			std::uint32_t targetSize[2];
			std::uint32_t sourceSize[2];
			std::uint32_t fromDepth;
			std::uint32_t padding;
		};
		static_assert(sizeof(HzbConstants) / 4 == kHzbConstantWords);

		// Bytes reserved per sampled texel; a copy footprint's row pitch wants generous alignment.
		constexpr std::uint32_t kProbeSlotBytes = 256;
		// Slots: the colour targets before the draws, the same after, then the depth after the Z-prepass and
		// again just before the colour draws - the two numbers the colour pass's depth test compares.
		constexpr std::uint32_t kProbeDepthAfterPrepass = 2 * kColorTargets;
		constexpr std::uint32_t kProbeDepthBeforeColour = kProbeDepthAfterPrepass + 1;
		constexpr std::uint32_t kProbeDepthAfterColour = kProbeDepthBeforeColour + 1;
		constexpr std::uint32_t kProbeDepthAfterSky = kProbeDepthAfterColour + 1;
		constexpr std::uint32_t kProbeDepthAfterLightCulling = kProbeDepthAfterSky + 1;
		constexpr std::uint32_t kProbeSlots = kProbeDepthAfterLightCulling + 1;

		// MainPayload::objectState values besides the Skip reasons.
		constexpr std::uint8_t kObjectStateDrawable = 0xF0;
		constexpr std::uint8_t kObjectStateDecal = 0xF1;
		constexpr std::uint8_t kObjectStateAbsent = 0xFF;

		/**
		 * CS_DCLF_SET_PARITY=1: every frame, the per-object visibility words read back after the colour epoch,
		 * with BuildDrawsCS's two drawn bits (depth, colour), against what the two builds and the native
		 * withholding decided on the CPU. See CheckSetParity.
		 */
		bool SetParityEnabled();

		// CS_DCLF_BUILD_PARITY=1: compare BuildDraws' output with the CPU templates every 300 epochs.
		bool BuildParityEnabled();

		// A DXVK image the graph uses in place: imported without ownership, with simultaneous access (DXVK
		// keeps the images it hands out in GENERAL and uses them between the graph's commands).
		std::shared_ptr<org::ExternalTextureResource> ImportImage(rhi::Device a_device, const DxvkOrgInteropImageInfo& a_image, org::TextureDescription a_desc,
			const char* a_name);

		/** @brief A view the hook captured for the frame's epoch (CaptureShadowView, ExecuteShadowFrame). */
		struct PendingView
		{
			std::uint32_t viewId = 0, renderMode = 0, modeIndex = 0, targetIndex = 0, slice = 0;
			std::uint32_t x = 0, y = 0, width = 0, height = 0;
			float minDepth = 0.0f, maxDepth = 1.0f;
			RE::NiPoint3 eye;
			bool hasViewProj = false;
			std::array<float, 16> viewProj{};
			// The engine's caster volume for the view (NiCullingProcess::customCullPlanes), when its culling
			// process has one; see BuildDrawsLatch::cullPlanes.
			float cullPlanes[6][4] = {};
			std::uint32_t cullPlaneMask = 0;
			std::uint32_t rasterState = 0;  // DrawPipelines::ShadowRasterStateId of the state the engine draws it with
			// 0: ordinary casters (kObjectVolumetricOnly inputs skipped); 1: the volumetric lighting copy, which
			// draws the volumetric-only casters alone.
			std::uint32_t casterClass = 0;
			bool sunView = false;  // the directional light's: its casters are its full-frustum entries' (kCullSunEntry)
			float viewBlock[12] = {};  // PerTechnique: HighDetailRange, ParabolaParam, EyeDelta
			std::array<std::byte, 1024> perFrame{};
			std::uint32_t perFrameBytes = 0;
			DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
		};

		/**
		 * @brief One captured view of the frame's shadow epoch: where to draw its render mode's inputs and
		 * with which per-view blocks (its record region names them).
		 */
		struct ShadowFrameView
		{
			std::uint32_t slot = 0;       // the view's sequence and count buffers, record region and latch
			std::uint32_t modeIndex = 0;  // the inputs buffer (render mode 0xD, 0xE, 0xF)
			std::uint32_t capacity = 0;   // its draw's ExecuteIndirect max count (GrowCapacity, per slot)
			std::uint32_t x = 0, y = 0, width = 0, height = 0;  // the view's viewport, in its slice
			float minDepth = 0.0f, maxDepth = 1.0f;
			std::uint32_t target = 0;  // index into ShadowResources::depth
			std::uint32_t slice = 0;
			std::uint64_t materialRows = 0;  // the shadow material rows' address (one table for every view)
			std::uint32_t sequenceDraws = 0;  // its slot's sequence buffer, in draws (ShadowResources::sequenceDraws)
			// The view's push data (DrawPipelines.h, kShadowPushWords): its blocks' addresses, pushed once for its draws.
			std::array<std::uint32_t, kShadowPushWords> push{};

			bool operator==(const ShadowFrameView&) const = default;
		};

		/**
		 * @brief The shape of the frame's shadow epoch: which views, where they draw and with what capacity. Every view
		 * the hooks captured is drawn by one graph execution at AfterShadowMaps, rather than one per view, because the
		 * graph's own execution costs about 0.9 ms of CPU per epoch whatever it draws, and the exterior has four views a
		 * frame. Each view's matrices and input count are in its BuildDrawsLatch; the shape changes only when the engine's
		 * view layout or the pipelines do, and its identity is the shadow passes' revision.
		 */
		struct ShadowFrame
		{
			std::uint64_t generation = 0;
			rhi::DescriptorHeapHandle resourceHeap{};
			rhi::DescriptorHeapHandle samplerHeap{};
			ShadowIndirectState indirect{};
			std::vector<ShadowFrameView> views;
			// The latch block the views' values were written to (ShadowResources::latch, replaced when the slots grow): read by
			// the passes' preparation from here, on whichever thread prepares them.
			std::shared_ptr<const org::LatchBlock> latch;

			bool SameShape(const ShadowFrame& o) const
			{
				return latch == o.latch && SameHandle(resourceHeap, o.resourceHeap) && SameHandle(samplerHeap, o.samplerHeap) && indirect.valid == o.indirect.valid &&
				       SameHandle(indirect.layout, o.indirect.layout) && SameHandle(indirect.set, o.indirect.set) && SameHandle(indirect.signature, o.indirect.signature) &&
				       views == o.views;
			}
		};

		/**
		 * @brief Publishes a frame's shape: the one already published while nothing the recordings depend on changed, so the
		 * passes reuse their invocations; a_frame under a new generation otherwise, kept in a_published for the next frame.
		 */
		template <class Frame>
		void PublishShape(std::shared_ptr<Frame> a_frame, std::shared_ptr<const Frame>& a_published, std::atomic<std::shared_ptr<const Frame>>& a_current,
			std::uint64_t& a_generations)
		{
			if (a_published && a_published->SameShape(*a_frame)) {
				a_current.store(a_published, std::memory_order_release);
				return;
			}
			a_frame->generation = ++a_generations;
			a_published = a_frame;
			a_current.store(std::move(a_frame), std::memory_order_release);
		}

		/** @brief The shadow views' graph resources: the main path's set, without targets or an HZB, per view slot. */
		struct ShadowResources
		{
			// The explicit DGC preprocesses' state lists: the shadow views' pass, Skylighting's.
			std::shared_ptr<PreprocessStates> preprocessShadow, preprocessSky;
			std::shared_ptr<org::Buffer> constants, visibility;
			std::uint32_t objectCapacity = 0;  // what visibility and the inputs hold, as Resources::objectCapacity
			// The material rows every view's draws name (ShadowMaterialRow), grown with the kept state.
			GrowableRows materialRows;
			// The scene's tables, every epoch's (SceneBuffers).
			std::shared_ptr<SceneBuffers> scene;
			std::array<std::shared_ptr<org::Buffer>, kShadowModeCount> inputs;               // per render mode
			// Per view slot (the occlusion views', then the shadow views'), as many as the latch layout's viewSlots (Impl::ReserveShadowLatch):
			// its sequence and count buffers, its sequence buffer's draws - grown before the epoch that uses the slot to hold
			// every draw the scene can produce (Impl::ReserveShadowSequences) - and its counters' readback view. Render thread.
			std::vector<std::shared_ptr<org::Buffer>> sequences, count;
			std::vector<std::uint32_t> sequenceDraws;
			std::vector<winrt::com_ptr<ID3D11Buffer>> countD3D11;
			// The view slots' blocks (kShadowViewSlotBytes a row: b0, b12), written by each epoch's commit.
			GrowableRows viewBlocks;
			ShadowLatchLayout latchLayout;
			// The kept shadow state's versions (ShadowKept) each mode's input buffer and the material rows hold: 0 when a build
			// without the kept state wrote the buffer, or when the rows' backing is new.
			std::array<std::uint64_t, kShadowModeCount> inputsUploaded{};
			std::uint64_t materialRowsHeld = 0;
			std::uint64_t constantsAddress = 0;
			std::shared_ptr<const ComputeProgram> buildDraws;
			// The engine's shadow map arrays, imported once each (re-imported when the engine recreates
			// them), with a depth-stencil view per slice.
			std::array<std::shared_ptr<org::ExternalTextureResource>, kShadowDepthTargets> depth;
			std::array<ID3D11Texture2D*, kShadowDepthTargets> depthTexture{};
			std::array<std::uint32_t, kShadowDepthTargets> depthLayers{};
			std::atomic<std::shared_ptr<const ShadowFrame>> frame;
			std::shared_ptr<const ShadowFrame> published;  // survives a frame without views
			// The occlusion maps: their own epoch's views (ExecuteOcclusion).
			std::atomic<std::shared_ptr<const ShadowFrame>> occlusionFrame;
			std::shared_ptr<const ShadowFrame> occlusionPublished;
			std::uint64_t shapeGenerations = 0;
			// Per frame slot, one BuildDrawsLatch per view slot and the pipeline map rows (latchLayout); a new block when either
			// grows. The passes read it from the published frame (ShadowFrame::latch).
			std::shared_ptr<org::LatchBlock> latch;
			rhi::CommandSignaturePtr dispatchSignature;
			// This frame's views as the engine named them (render thread), for the culling readback's report.
			struct ViewLabel
			{
				std::uint32_t viewId = 0, renderMode = 0, slot = 0;
			};
			std::vector<ViewLabel> labels;
		};

		// These passes run only in their own epoch (the shadow views' or Skylighting's).
		std::shared_ptr<const ShadowFrame> CurrentShadowFrame(const ShadowResources& a_resources, bool a_sky);

		// The main pass's bindings where its opaque batches start (DrawcallLimitFix::BeforeOpaquePass), or the depth pass's.
		struct Capture
		{
			std::array<ID3D11Buffer*, kConstantBufferRegisters> vsBuffers{};
			std::array<ID3D11Buffer*, kConstantBufferRegisters> psBuffers{};
			std::array<ID3D11ShaderResourceView*, kTextureRegisters> psViews{};
			winrt::com_ptr<ID3D11Texture2D> depth;
			std::array<winrt::com_ptr<ID3D11Texture2D>, kColorTargets> targets;
			std::uint32_t targetCount = 0;
			std::uint32_t viewportWidth = 0, viewportHeight = 0;
			float minDepth = 0.0f, maxDepth = 1.0f;
			// The camera the main pass draws with: the world transforms are stored relative to it, and the
			// engine has moved it on by the time the epoch packs the constants.
			RE::NiPoint3 eye, previousEye;

			void Release()
			{
				for (auto* buffer : vsBuffers)
					if (buffer)
						buffer->Release();
				for (auto* buffer : psBuffers)
					if (buffer)
						buffer->Release();
				for (auto* view : psViews)
					if (view)
						view->Release();
			}
		};

		Capture CaptureBindings();

		// The CPU-written structured buffers the main pass binds at t16 and up (Light Limit Fix's come from the graph).
		std::vector<FrameBuffer> FrameBuffersOf(const Capture& a_capture, bool a_lightLimitFix);

		inline ID3D11Buffer* BufferOf(ID3D11ShaderResourceView* a_view)
		{
			winrt::com_ptr<ID3D11Resource> resource;
			a_view->GetResource(resource.put());
			return static_cast<ID3D11Buffer*>(resource.get());  // the view keeps it alive
		}

		// A growing byte arena of 256-byte aligned constant blocks.
		class ConstantArena
		{
		public:
			void Reset(std::uint64_t a_capacity)
			{
				bytes.clear();
				capacity = a_capacity;
			}

			// Offset of a zeroed block, or ~0 when the arena is full.
			std::uint64_t Allocate(std::size_t a_size)
			{
				const std::uint64_t offset = (bytes.size() + kConstantAlignment - 1) & ~(kConstantAlignment - 1);
				if (offset + a_size > capacity)
					return ~0ull;
				bytes.resize(offset + std::max<std::size_t>(a_size, 16));
				return offset;
			}

			std::span<std::byte> At(std::uint64_t a_offset, std::size_t a_size) { return { bytes.data() + a_offset, a_size }; }
			const std::vector<std::byte>& Bytes() const { return bytes; }

		private:
			std::vector<std::byte> bytes;
			std::uint64_t capacity = 0;
		};

		// ---------------------------------------------------------------------------------------------
		// The epoch's inputs, their build, and their commit.
		//
		// An epoch's CPU work is in three parts, so that the build - the part proportional to the scene -
		// can run off the render thread (docs: asynchronous tables). Prepare, on the render thread, snapshots
		// everything the build needs that is not immutable for the frame. Build is a pure function of that
		// snapshot, the tables and the lookups (Lookups.h), and writes only its payload: nothing in it may
		// call the engine, D3D11, DXVK or ORG. Commit, on the render thread inside the epoch's preparation,
		// validates the payload against what it was built for, resolves what only the services can resolve,
		// and issues the uploads.
		// ---------------------------------------------------------------------------------------------

		// The per-frame constant blocks - whatever the main pass binds outside the per-draw registers - live
		// in Resources::frameConstants at fixed slots, so that a record can name a block before its contents
		// exist: the vertex stage's b0-b13, then the pixel stage's, then the zeroed StrictLightData block
		// every bindless draw's b3 reads, and the frame lighting every DCLF_BINDLESS draw's b13 reads
		// (SceneStore::Tables::frameLighting). A slot is D3D11's constant buffer maximum, so no block overflows.
		constexpr std::uint64_t kFrameSlotBytes = 65536;
		constexpr std::uint32_t kFrameSlotSharedLight = 2 * kConstantBufferRegisters;
		constexpr std::uint32_t kFrameSlotLighting = kFrameSlotSharedLight + 1;
		// The frame record (DrawBindings): every register a draw's rows do not give - the frame slots' addresses, the frame's
		// textures (t16 and up), the object and bone tables - written by each commit (FrameRecordOf).
		constexpr std::uint32_t kFrameSlotRecord = kFrameSlotLighting + 1;
		constexpr std::uint32_t kFrameSlotCount = kFrameSlotRecord + 1;
		constexpr std::uint64_t kFrameConstantBytes = std::uint64_t(kFrameSlotCount) * kFrameSlotBytes;

		constexpr std::uint64_t FrameSlotOffset(bool a_pixelStage, std::uint32_t a_register)
		{
			return (std::uint64_t(a_pixelStage ? kConstantBufferRegisters : 0u) + a_register) * kFrameSlotBytes;
		}

		inline std::array<std::uint32_t, kFramePushWords> FramePushWords(std::uint64_t a_frameConstants)
		{
			std::array<std::uint32_t, kFramePushWords> words{};
			std::uint32_t word = kFramePushRegisters;
			for (const bool pixel : { false, true }) {
				const std::uint32_t mask = pixel ? kFramePushPS : kFramePushVS;
				for (std::uint32_t r = 0; r < kConstantBufferRegisters; ++r) {
					if (!((mask >> r) & 1))
						continue;
					// The frame slot, the shared light block at PS b3 and the frame lighting at PS b13 (kFrameLightingRegister).
					std::uint64_t address = a_frameConstants + FrameSlotOffset(pixel, r);
					if (pixel && r == 3)
						address = a_frameConstants + std::uint64_t(kFrameSlotSharedLight) * kFrameSlotBytes;
					else if (pixel && r == kFrameLightingRegister)
						address = a_frameConstants + std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes;
					words[word++] = static_cast<std::uint32_t>(address);
					words[word++] = static_cast<std::uint32_t>(address >> 32);
				}
			}
			const std::uint64_t record = a_frameConstants + std::uint64_t(kFrameSlotRecord) * kFrameSlotBytes;
			words[kFramePushRecord] = static_cast<std::uint32_t>(record);
			words[kFramePushRecord + 1] = static_cast<std::uint32_t>(record >> 32);
			return words;
		}

		// The blocks the render thread packs into the frame slots for one epoch.
		struct FrameBlocks
		{
			std::array<std::vector<std::byte>, kConstantBufferRegisters> vs, ps;
			std::uint32_t vsMask = 0, psMask = 0;  // the slots supplied
		};

		// What a build addresses in an epoch's buffers; a change means the resources were recreated under it.
		struct ResourceAddresses
		{
			// records: the material rows' table (the shadow views' ShadowMaterialRow, the main pass's MaterialRow); pipelineRows: the
			// main pass's pipeline rows.
			std::uint64_t constants = 0, records = 0, pipelineRows = 0, frameConstants = 0;
			std::uint64_t facePositions = 0;  // the face positions buffer (SceneBuffers::facePositions)
			std::uint32_t objectsIndex = 0, bonesIndex = 0;
			std::uint32_t recordCapacity = 0;  // the material rows' table's rows (a row past it waits for the table to grow)
			const void* identity = nullptr;

			bool operator==(const ResourceAddresses&) const = default;
		};

		/**
		 * @brief The per-object records (BindlessObject) one objects buffer holds, kept across frames (drawcall-limit-fix.md,
		 * "Persistent draw state", Step 3): a record is written again only when the change log names a column it is built
		 * from, and the buffer is sent what changed since the version it holds (KeptArray). One, for every epoch's builds
		 * (SceneBuffers). Its builds run in frame order - the shadow build, then the Z-prepass, then the colour build, each
		 * joined (or cancelled, which waits) before the next is kicked, on the one worker or inline - so nothing else writes
		 * it meanwhile; `collisions` counts it if anything ever does.
		 */
		struct ObjectRecordStore
		{
			LogCursor cursor;
			MarkedList changedObjects;
			KeptArray<BindlessObject> records;
			std::atomic<std::uint32_t> busy{ 0 };
			// Since the last report.
			std::uint64_t updates = 0, rewritten = 0, resyncs = 0, collisions = 0;
			// Phase D's measure: updates whose changes were only placements and palettes (the per-frame streams), and the
			// updates with any other cause, by cause.
			std::uint64_t streamOnly = 0, structural = 0;
			std::array<std::uint64_t, kChangeCauseCount> byCause{};
			ParityCounter parity;
		};

		/** @brief A build's view of the object records: the store's, or a full set of its own without one (version 0). */
		using ObjectRecordsOut = KeptView<BindlessObject>;

		/**
		 * @brief The rows one bones buffer holds (Step 5): every palette, current then previous (one capacity further), then
		 * the extras - read straight from the tables' arrays, and journalled as rows, so the buffer is sent the rows changed
		 * since the version it holds. The change log names them: a palette's rows (kChangePalette), its place (kChangeSkin),
		 * an extras block's rows or place (kChangeExtras). A new capacity moves everything past the palettes, so it is a
		 * resync. One, for every epoch's builds (SceneBuffers); in frame order like ObjectRecordStore.
		 */
		struct BonesStore
		{
			LogCursor cursor;
			std::uint32_t capacity = 0;
			ChangeJournal rows;
			std::vector<float> uploaded;  // CS_DCLF_PERSISTENT_PARITY: the rows as uploaded
			std::uint64_t updates = 0, rowsSent = 0, resyncs = 0;
			ParityCounter parity;
		};

		/** @brief A build's view of the rows: the tables' arrays, with the store's changes (version 0: sent whole). */
		struct BonesOut
		{
			const float* bones = nullptr;
			const float* previous = nullptr;
			const float* extras = nullptr;
			std::uint32_t capacity = 0, extraRows = 0;
			ChangeJournal::Snapshot changes;
			std::uint64_t Version() const { return changes.version; }
			std::uint32_t Rows() const { return 2 * capacity + extraRows; }
			const float* Row(std::uint64_t a_row) const
			{
				if (a_row < capacity)
					return bones + std::size_t(a_row) * 4;
				if (a_row < 2ull * capacity)
					return previous + std::size_t(a_row - capacity) * 4;
				return extras + std::size_t(a_row - 2ull * capacity) * 4;
			}
			void Reset() { *this = {}; }
		};

		void UpdateBones(BonesStore* a_store, std::uint64_t a_uploaded, const SceneStore::Tables& a_tables, std::uint32_t a_generation, BonesOut& a_out);

		/**
		 * @brief The uploads of a build's rows a buffer at a_held lacks (the changed runs, else all of them), clipped to the buffer.
		 * A run is sent a section at a time: the current palettes, the previous ones and the extras are separate arrays.
		 */
		template <class Emit>
		std::size_t EmitBones(const BonesOut& a_out, std::uint64_t a_held, BonesStore* a_parity, Emit&& a_emit)
		{
			const std::uint32_t rows = a_out.Rows();
			if (!rows || !a_out.bones)
				return 0;
			const std::uint64_t capacity = a_out.capacity;
			std::size_t sent = 0;
			a_out.changes.ForEachRun(a_held, rows, [&](std::uint64_t a_first, std::uint64_t a_count) {
				for (std::uint64_t at = a_first, end = a_first + a_count; at < end;) {
					const std::uint64_t sectionEnd = at < capacity ? capacity : at < 2 * capacity ? 2 * capacity : end;
					const std::uint64_t count = std::min(end, sectionEnd) - at;
					a_emit(a_out.Row(at), std::size_t(count) * 16, std::size_t(at) * 16);
					if (a_parity) {
						if (a_parity->uploaded.size() < std::size_t(rows) * 4)
							a_parity->uploaded.resize(std::size_t(rows) * 4, 0.0f);
						std::memcpy(&a_parity->uploaded[std::size_t(at) * 4], a_out.Row(at), std::size_t(count) * 16);
					}
					sent += static_cast<std::size_t>(count);
					at += count;
				}
			});
			return sent;
		}

		/** @brief CS_DCLF_PERSISTENT_PARITY: the rows the buffer holds, as uploaded, against the tables' now. */
		inline void CheckBones(BonesStore& a_store, const BonesOut& a_out)
		{
			const std::uint32_t rows = a_out.Rows();
			if (a_store.uploaded.size() < std::size_t(rows) * 4)
				return;
			bool same = true;
			for (std::uint32_t row = 0; row < rows && same; ++row)
				same = std::memcmp(&a_store.uploaded[std::size_t(row) * 4], a_out.Row(row), 16) == 0;
			a_store.parity.Check(same);
		}

		/**
		 * @brief A build's view of the geometry table: the geometry slots' draws (the store's, or a set of its own without one)
		 * and the face streams' after them, which are the frame's and sent whole every build.
		 */
		struct GeometryDrawsOut
		{
			KeptView<GeometryDraw> slots;
			std::vector<GeometryDraw> faces;
			std::size_t SlotCount() const { return slots.Count(); }
			std::size_t Count() const { return SlotCount() + faces.size(); }
			std::uint64_t Version() const { return slots.Version(); }
			std::vector<GeometryDraw> Flat() const
			{
				std::vector<GeometryDraw> flat;
				if (slots.elements)
					flat = *slots.elements;
				flat.insert(flat.end(), faces.begin(), faces.end());
				return flat;
			}
			void Reset() { *this = {}; }
		};

		/**
		 * @brief The geometry slots' draws one geometry buffer holds, kept across builds (Step 7): repacked from the tables'
		 * geometry log (Tables::geometryLog), and sent as the slots changed since the version the buffer holds. One, for every
		 * epoch's builds (SceneBuffers); in frame order like ObjectRecordStore.
		 */
		struct GeometryStore
		{
			LogCursor cursor;
			KeptArray<GeometryDraw> packed;
			// Since the last report.
			std::uint64_t updates = 0, rewritten = 0, resyncs = 0;
			ParityCounter parity;
		};

		inline bool PersistentParityEnabled()
		{
			return SwitchEnabled(Switch::PersistentParity);
		}

		// What a record is built from (BuildObjectRecord): the placement, the alpha test (bindings), the shading, the lights,
		// the tree animation, the palette and the extras.
		constexpr std::uint32_t kObjectRecordCauses = kChangePlacement | kChangeBindings | kChangeShading | kChangeLights | kChangeTree | kChangeSkin | kChangeExtras;

		/**
		 * @brief The build's object records: brought up to date from the change log in the store (a_uploaded is the version
		 * the buffer holds, as the inputs saw it), or built whole without one.
		 */
		void UpdateObjectRecords(ObjectRecordStore* a_store, std::uint64_t a_uploaded, const SceneStore::Tables& a_tables, std::uint32_t a_generation,
			std::uint32_t a_frame, ObjectRecordsOut& a_out);

		/** @brief Per object slot, what the colour epoch drew (IndirectDraws' drawn state, render thread). */
		struct SlotDrawn
		{
			const RE::BSGeometry* geometry = nullptr;  // what it was last drawn as
			std::uint32_t last = 0;                     // the last frame it was drawn, when it is not now
			bool drawn = false;
			bool DrewLast(const RE::BSGeometry* a_geometry, std::uint32_t a_frame) const { return geometry == a_geometry && (drawn || a_frame - last <= 1); }
		};

		/**
		 * @brief A main-pass row's bytes (DrawPipelines.h: kMaterialRowBytes, kPipelineRowBytes). Its header's addresses hold the
		 * blocks' offsets in the row until the upload adds the row's own address (EmitMainRows): a table that grows is sent again
		 * whole, with the new addresses.
		 */
		template <std::size_t Bytes>
		struct RowBytes
		{
			std::array<std::byte, Bytes> bytes{};
		};
		using MaterialRow = RowBytes<kMaterialRowBytes>;
		using PipelineRow = RowBytes<kPipelineRowBytes>;
		inline MaterialRowHeader& HeaderOf(MaterialRow& a_row) { return *reinterpret_cast<MaterialRowHeader*>(a_row.bytes.data() + kMaterialRowHeader); }
		inline PipelineRowHeader& HeaderOf(PipelineRow& a_row) { return *reinterpret_cast<PipelineRowHeader*>(a_row.bytes.data() + kPipelineRowHeader); }

		/**
		 * @brief The main pass's rows, kept across frames and shared by the Z-prepass and colour builds (which run in frame order,
		 * one at a time, like the object records'): a material row per material slot and a pipeline row per pipeline slot, each
		 * written again only when what it is written from changed (its key), and sent as what changed since the version the
		 * table holds. What a pair of them can draw is checked per build (MainBuild::AssembleRecord), from what each row holds.
		 */
		struct MainRows
		{
			KeptArray<MaterialRow> material;
			KeptArray<PipelineRow> pipeline;
			struct MaterialState
			{
				std::array<std::uint32_t, 8> key{};
				bool written = false;
				bool texturesOk = false;  // every texture the material sets resolved (else deferred: waiting for them)
				bool deferred = false;
				std::uint32_t missingTexture = 0;
				std::uint32_t textures = 0;  // t0-t15 the row gives a descriptor (bit per register)
				std::uint32_t samplers = 0;  // s0-s15 likewise
				std::uint32_t features = 0;  // kFeatureMaterialRegisters likewise
				std::uint64_t tables = 0;    // the constant tables its blocks were packed with (a hash)
				bool blocksOk = false;       // both blocks fit their places
			};
			struct PipelineState
			{
				std::array<std::uint32_t, 6> key{};
				bool written = false;
				bool blocksOk = false;
				bool shadowMask = false, shadowMaskSampler = false;  // t14, s14 given
			};
			std::vector<MaterialState> materials;
			std::vector<PipelineState> pipelines;
			const void* identity = nullptr;
			std::uint32_t generation = 0;  // the tables generation its rows were written for
			bool active = false;
			// Since the last report.
			std::uint64_t builds = 0, materialsWritten = 0, pipelinesWritten = 0, resyncs = 0;
			std::atomic<std::uint32_t> busy{ 0 };
		};

		/**
		 * @brief Rows a table at a_held lacks, each with its header's addresses made absolute (a_base plus the row's offset):
		 * a_emit(data, bytes, offset). a_patch(row, rowAddress) fixes one row's header.
		 */
		template <class Row, class Patch, class Emit>
		std::size_t EmitMainRows(const KeptView<Row>& a_rows, std::uint64_t a_held, std::uint64_t a_base, Patch&& a_patch, Emit&& a_emit)
		{
			if (!a_rows.elements)
				return 0;
			const auto& rows = *a_rows.elements;
			std::vector<Row> run;
			std::size_t sent = 0;
			a_rows.changes.ForEachRun(a_held, rows.size(), [&](std::uint64_t a_first, std::uint64_t a_count) {
				run.assign(rows.begin() + static_cast<std::ptrdiff_t>(a_first), rows.begin() + static_cast<std::ptrdiff_t>(a_first + a_count));
				for (std::size_t i = 0; i < run.size(); ++i)
					a_patch(run[i], a_base + (a_first + i) * sizeof(Row));
				a_emit(run.data(), run.size() * sizeof(Row), static_cast<std::size_t>(a_first) * sizeof(Row));
				sent += run.size();
			});
			return sent;
		}
		/**
		 * @brief A build's scene rows and inputs against the capacities reserved before its inputs were taken (ReserveSceneTables):
		 * past them is a defect of that reserve, never data to drop.
		 */
		inline void CheckSceneCapacity(const SceneBuffers& a_scene, std::size_t a_objects, std::size_t a_geometries, std::size_t a_boneRows, std::size_t a_inputs,
			std::uint32_t a_inputCapacity, const char* a_what)
		{
			if (a_objects <= a_scene.objectCapacity && a_geometries <= a_scene.geometryRows && a_boneRows <= a_scene.boneRows && a_inputs <= a_inputCapacity)
				return;
			stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s build is past its tables: {} objects of {}, {} geometry rows of {}, {} bone rows of {}, {} inputs of {}",
				a_what, a_objects, a_scene.objectCapacity, a_geometries, a_scene.geometryRows, a_boneRows, a_scene.boneRows, a_inputs, a_inputCapacity));
		}

		inline void PatchRowAddresses(MaterialRow& a_row, std::uint64_t a_address)
		{
			auto& header = HeaderOf(a_row);
			header.vsMaterial += a_address;
			header.psMaterial += a_address;
		}
		inline void PatchRowAddresses(PipelineRow& a_row, std::uint64_t a_address)
		{
			auto& header = HeaderOf(a_row);
			for (auto* address : { &header.vsTechnique, &header.psTechnique, &header.vsGeometry, &header.psGeometry, &header.vsPermutation, &header.psPermutation })
				*address += a_address;
		}
		/** @brief An input's rows (BuildDrawsCS.hlsl, RowsOf): the pipeline slot in the high 12 bits, the material slot in the low 20. */
		constexpr std::uint32_t kRowPipelineShift = 20;
		constexpr std::uint32_t kRowMaterialMask = (1u << kRowPipelineShift) - 1;
		constexpr std::uint32_t RowsOf(std::uint32_t a_pipeline, std::uint32_t a_material) { return (a_pipeline << kRowPipelineShift) | a_material; }

		struct MainInputs
		{
			std::uint32_t frameNumber = 0;
			bool depthOnly = false;
			bool bindlessParity = false;
			bool withholding = false;
			RE::NiPoint3 eye, previousEye;
			std::uint32_t vsFrameMask = 0, psFrameMask = 0;  // the frame slots the commit supplies
			std::array<std::uint32_t, kDecalGroups> decalCount{};
			// The colour epoch's drawn state (render thread's, read while no colour commit can run): the Z-prepass's gate
			// without withholding. The colour build's DrawnMarks are sent relative to drawnCommitted, all of them on drawnResync.
			const std::vector<SlotDrawn>* drawnSlots = nullptr;
			std::uint64_t drawnCommitted = 0;
			bool drawnResync = false;
			// The PerMaterial floats that are the frame's rather than the material's (SceneStore::
			// GetMaterialPatchedFloats / GetMaterialPatchedVSFloats, MaterialSources): the build cache leaves them
			// out of a pair's signature and repacks the group, so a drifting IBLParams or a scrolling
			// TexcoordOffset does not rebuild every pair every frame.
			std::vector<std::uint32_t> materialPatchedFloats;
			std::vector<std::uint32_t> materialPatchedVSFloats;
			ResourceAddresses addresses{};
			std::uint32_t lookupGeneration = 0, tablesGeneration = 0;
			// The resident region version the segment's input buffer holds (Resources::residentUploaded): the region
			// uploads only the entries changed since that one.
			std::uint64_t residentUploaded = 0;
			TablesHeld tablesHeld;  // likewise the kept tables (Resources::tablesHeld)
			// The versions of the kept rows the tables hold (Resources::materialRowsHeld, pipelineRowsHeld): what the rows'
			// journals keep changes for.
			std::uint64_t materialRowsHeld = 0, pipelineRowsHeld = 0;
		};

		struct MainPayload
		{
			MainInputs inputs;
			std::vector<std::shared_ptr<const void>> bindingOwners;  // exact roots used by this payload, including clean kept rows
			std::vector<DrawSequence> sequences;  // CPU templates of BuildDraws' output
			std::array<std::vector<DrawSequence>, kDecalGroups> decalTemplates;  // by group and slot
			std::vector<DrawInput> inputList;
			GeometryDrawsOut geometryDraws;
			std::vector<SceneStore::Tables::FaceStream> faceStreams;  // the tables', for the commit's uploads
			// The rows (MainRows), kept across frames and shared by both segments: uploaded as the rows changed since the version
			// the tables hold.
			KeptView<MaterialRow> materialRows;
			KeptView<PipelineRow> pipelineRows;
			std::uint64_t materialRowsWritten = 0, pipelineRowsWritten = 0;
			// The frame's textures (t16 and up) the drawn pipelines read, which only the commit can resolve (into the frame
			// record): the commit counts the ones it could not.
			std::array<std::uint64_t, 2> frameRegisters{};
			// The colour build's drawn changes (DrawnMarks): each slot whose drawn state changed since version drawnBase, with its
			// state now; all of them when drawnFull. The commit applies them when drawnBase is what it applied last.
			struct DrawnChange
			{
				std::uint32_t slot = 0;
				const RE::BSGeometry* geometry = nullptr;
				bool drawn = false;
			};
			std::vector<DrawnChange> drawnChanges;
			std::uint64_t drawnVersion = 0, drawnBase = 0;
			bool drawnFull = false, drawnValid = false;
			// Per object in the tables: what this build did with it - kObjectStateDrawable, kObjectStateDecal,
			// a Skip reason, or kObjectStateAbsent when it never reached the inputs (CS_DCLF_SET_PARITY explains
			// its mismatches with this).
			std::vector<std::uint8_t> objectState;
			std::array<std::uint32_t, kDecalGroups> decalCount{};
			// The build's stats, merged into IndirectDraws::Stats by the commit.
			std::array<std::uint32_t, static_cast<std::size_t>(IndirectDraws::Skip::Count)> skipped{};
			std::array<std::uint32_t, 4> missingTextures{};
			std::uint32_t missingNext = 0;
			std::uint32_t missingVertexConstants = 0, missingPixelConstants = 0;
			std::uint32_t decalsDrawn = 0, shortBuffers = 0, deferredTextures = 0;
			std::uint32_t bindlessParityChecks = 0, bindlessParityMismatches = 0;
			std::uint32_t rowTableConflicts = 0;  // pairs whose pipeline's constant tables are not the ones its material row was packed with
			std::array<double, 7> partMs{};
			// The first draw past its buffers, for the commit to log with the geometry's name.
			struct ShortBuffer
			{
				std::uint32_t object = ~0u;
				std::uint64_t vertexNeeded = 0, indexNeeded = 0;
			} shortBuffer;
			// The worker's build stages its uploads itself (StageMainPayload), so the commit on the render thread only submits the
			// batch. For the resources it was staged against, and the rows' backings then; a build made on the render thread has none.
			std::shared_ptr<org::runtime::StagedUploadBatch> staged;
			const void* stagedFor = nullptr;
			std::uint64_t stagedRowsGeneration = 0;
			std::uint64_t stagedSceneGeneration = 0;  // the scene tables' (SceneBuffers::generation) the batch was staged against
			// The segment's resident region (ResidentRegion): its inputs lead the input buffer, uploaded when residentVersion
			// is not the one the buffer holds (Resources::residentUploaded); inputList follows them.
			KeptView<DrawInput> resident;
			std::uint32_t residentDraws = 0, residentPairs = 0, residentUndrawable = 0, residentResyncs = 0;
			std::uint32_t residentParityChecks = 0, residentParityMismatches = 0, residentMissing = 0;
			std::uint32_t residentPairsChecked = 0, residentPairsStale = 0;  // pairs whose witness moved with no event (UpdateRegionPairs)

			void Reset()
			{
				resident.Reset();
				residentDraws = residentPairs = residentUndrawable = residentResyncs = 0;
				residentParityChecks = residentParityMismatches = residentMissing = 0;
				residentPairsChecked = residentPairsStale = 0;
				staged.reset();
				stagedFor = nullptr;
				stagedRowsGeneration = 0;
				stagedSceneGeneration = 0;
				bindingOwners.clear();
				sequences.clear();
				inputList.clear();
				geometryDraws.Reset();
				faceStreams.clear();
				frameRegisters = {};
				drawnChanges.clear();
				drawnVersion = drawnBase = 0;
				drawnFull = drawnValid = false;
				objectState.clear();
				materialRows.Reset();
				pipelineRows.Reset();
				materialRowsWritten = pipelineRowsWritten = 0;
				for (auto& templates : decalTemplates)
					templates.clear();
				decalCount = {};
				skipped = {};
				missingTextures = {};
				missingNext = 0;
				missingVertexConstants = missingPixelConstants = 0;
				decalsDrawn = shortBuffers = deferredTextures = 0;
				bindlessParityChecks = bindlessParityMismatches = rowTableConflicts = 0;
				partMs = {};
				shortBuffer = {};
			}
		};

		/**
		 * @brief A row of the shadow views' material table: the Utility vertex shader's PerMaterial block (b1, its texture
		 * offset), which the draw's pushed address names directly, and the diffuse's descriptor index for t0 after it
		 * (kShadowRowDiffuseOffset). One constant-buffer block (256 bytes): the table is an array of them. Row 0 is every caster
		 * without alpha testing (no offset, the null texture).
		 */
		struct ShadowMaterialRow
		{
			std::array<float, 4> texcoord{};  // offset xy, scale zw
			std::uint32_t diffuse = 0;
			std::array<std::uint32_t, 59> unused{};

			bool operator==(const ShadowMaterialRow& o) const { return texcoord == o.texcoord && diffuse == o.diffuse; }
		};
		static_assert(sizeof(ShadowMaterialRow) == kConstantAlignment && offsetof(ShadowMaterialRow, diffuse) == kShadowRowDiffuseOffset);

		/**
		 * @brief A shadow render mode's view rasterizer states this frame (DrawPipelines::ShadowRasterStateId), ascending: those
		 * of the views of ordinary casters, and those of the views of the volumetric lighting copy, which draw the
		 * volumetric-only casters alone (kObjectVolumetricOnly). A caster is an input only when its pipeline is ready under
		 * every state of the views that draw its class.
		 */
		struct ModeRasterStates
		{
			std::vector<std::uint32_t> casters, volumetric;

			void Add(std::uint32_t a_state, bool a_volumetric)
			{
				auto& ids = a_volumetric ? volumetric : casters;
				if (const auto it = std::lower_bound(ids.begin(), ids.end(), a_state); it == ids.end() || *it != a_state)
					ids.insert(it, a_state);
			}
			const std::vector<std::uint32_t>& Of(bool a_volumetric) const { return a_volumetric ? volumetric : casters; }
			bool Empty() const { return casters.empty() && volumetric.empty(); }
			/** @brief Every state of either class, once each. */
			std::vector<std::uint32_t> All() const
			{
				std::vector<std::uint32_t> all;
				std::set_union(casters.begin(), casters.end(), volumetric.begin(), volumetric.end(), std::back_inserter(all));
				return all;
			}
			bool operator==(const ModeRasterStates&) const = default;
		};

		struct ShadowInputs
		{
			std::uint32_t frameNumber = 0;
			std::array<bool, kShadowModeCount> modeUsed{};
			// Per mode, the rasterizer states of its views.
			std::array<ModeRasterStates, kShadowModeCount> modeRasterStates{};
			// The sun's full-frustum culling processes, as the full-frustum cull (FUN_141511f30) has just used them: an object
			// whose entry (SceneStore::Tables::sunEntry) is outside every one is no candidate of the sun's cascade culls. However
			// many there are: the sun views' latches name them in a shared region of the latch block (ReserveShadowLatch).
			std::vector<SunEntryProcess> sunEntryProcesses;
			// The sun entries the scene store found DCLF could take out of the cascade culls (SunAccumulation): the build
			// turns them into the next frame's exclusion, with the claims.
			std::shared_ptr<const SunCandidates> sunCandidates;
			// And those it could take out of the point lights' culls (LocalLightCull): the paraboloid exclusion's.
			std::shared_ptr<const SunCandidates> lightCandidates;
			ResourceAddresses addresses{};
			// Community Shaders' SharedData (b5) and FeatureData (b6), copied from the structs CS keeps.
			std::vector<std::byte> sharedData, featureData;
			std::uint32_t lookupGeneration = 0, tablesGeneration = 0;
			TablesHeld tablesHeld;  // ShadowResources::tablesHeld
			// The versions of the kept shadow state the buffers hold (ShadowResources::inputsUploaded, materialRowsHeld): what the
			// journals keep changes for.
			std::array<std::uint64_t, kShadowModeCount> inputsHeld{};
			std::uint64_t materialRowsHeld = 0;
		};

		struct ShadowPayload
		{
			ShadowInputs inputs;
			std::vector<std::shared_ptr<const void>> bindingOwners;
			ConstantArena arena;  // the view slots' head and the frame record are reserved; the commit writes the views into it
			DrawBindings frameRecord{};  // every view's textures and samplers but the diffuse (kShadowFrameRecordOffset)
			// The rows the inputs name: the kept state's (journalled), or the build's own (version 0: sent whole).
			KeptView<ShadowMaterialRow> materialRows;
			// Rows the build needed, within the table's capacity or not: what the next frame's Reserve grows it to. The
			// materials past the capacity wait for it (their casters stay the engine's this frame).
			std::uint32_t rowsWanted = 0, waitingRows = 0;
			// Per mode, the draws its inputs can produce (a skin draws once per partition): its views' max count.
			std::array<std::uint32_t, kShadowModeCount> modeDraws{};
			// The blocks every view's push data names besides its own (kShadowPushZeros and after), in the arena.
			std::uint64_t zerosAddress = 0, sharedDataAddress = 0, featureDataAddress = 0;
			std::vector<std::uint32_t> objectRecord;  // per object: its material row, or ~0u when it cannot draw
			std::array<std::vector<DrawInput>, kShadowModeCount> inputList;  // per render mode: the frame's own (after the kept region)
			// The kept state (ShadowKept): per mode the region's inputs at the head of the mode's buffer, sent as what changed
			// since the version the buffer holds.
			bool kept = false;
			std::array<KeptView<DrawInput>, kShadowModeCount> regionInputs;
			// Per mode, the build version at which an object last joined or left its inputs (ShadowKept::Mode::membership);
			// 0 without the kept state.
			std::array<std::uint64_t, kShadowModeCount> membership{};
			std::size_t RegionCount(std::uint32_t a_mode) const { return regionInputs[a_mode].Count(); }
			std::size_t ModeInputs(std::uint32_t a_mode) const { return RegionCount(a_mode) + inputList[a_mode].size(); }
			template <class F>
			void ForEachInput(std::uint32_t a_mode, F&& a_visit) const
			{
				if (regionInputs[a_mode].elements)
					for (const auto& input : *regionInputs[a_mode].elements)
						a_visit(input);
				for (const auto& input : inputList[a_mode])
					a_visit(input);
			}
			/** @brief The mode's inputs as one list (the diagnostics' copy). */
			std::vector<DrawInput> Flat(std::uint32_t a_mode) const
			{
				std::vector<DrawInput> out;
				out.reserve(ModeInputs(a_mode));
				ForEachInput(a_mode, [&](const DrawInput& a_input) { out.push_back(a_input); });
				return out;
			}
			GeometryDrawsOut geometries;
			std::vector<SceneStore::Tables::FaceStream> faceStreams;  // the tables', for the commit's uploads
			std::uint32_t skippedTexture = 0, skippedPipeline = 0, deferredTextures = 0, deferredPipelines = 0;
			// Per occlusion view, its occluders left out (no record, no pipeline yet): the map is then the engine's this frame.
			std::array<std::uint32_t, kOcclusionViews> occlusionSkipped{};
			// The worker's build stages its uploads itself (StageShadowPayload): everything but the arena's view
			// head, and the records of the first stagedSlots view slots. For the resources it was staged against.
			std::shared_ptr<org::runtime::StagedUploadBatch> staged;
			const void* stagedFor = nullptr;
			std::uint32_t stagedSlots = 0;
			// The worker's build also builds each used mode's claim set (ShadowClaimSet), which the epoch
			// publishes; empty for a build made on the render thread, which builds them at the publish.
			std::array<std::shared_ptr<const PassCapture::ClaimSet>, kShadowModeCount> claims;
			std::shared_ptr<SunExclusion> sunExclusion;  // likewise, from the cascades' mode (BuildSunExclusion)
			std::shared_ptr<SunExclusion> parabolicExclusion;  // and from the paraboloid mode (LocalLightCull)

			void Reset()
			{
				staged.reset();
				stagedFor = nullptr;
				stagedSlots = 0;
				claims = {};
				sunExclusion.reset();
				parabolicExclusion.reset();
				arena.Reset(kShadowConstantBytes);
				frameRecord = {};
				materialRows.Reset();
				rowsWanted = waitingRows = 0;
				modeDraws = {};
				zerosAddress = sharedDataAddress = featureDataAddress = 0;
				bindingOwners.clear();
				objectRecord.clear();
				for (auto& modeInputs : inputList)
					modeInputs.clear();
				kept = false;
				regionInputs = {};
				membership = {};
				geometries.Reset();
				faceStreams.clear();
				skippedTexture = skippedPipeline = deferredTextures = deferredPipelines = 0;
				occlusionSkipped = {};
			}
		};

		/**
		 * @brief The geometry slots an object's draw writes a sequence for, in BuildDrawsCS's order: its one
		 * geometry, or for a skin of several partitions each partition its mask names, following the slots'
		 * nextPartition links from the first.
		 */
		template <class F>
		void ForEachDrawnGeometry(const SceneStore::Tables& a_tables, std::uint32_t a_firstSlot, std::uint32_t a_partitions, F&& a_draw)
		{
			if (a_partitions == kNoPartitions)
				return;
			std::uint32_t slot = a_firstSlot;
			for (std::uint32_t i = 0; i < kMaxSkinPartitions && slot < a_tables.geometries.size(); ++i) {
				if (a_partitions == 0 || ((a_partitions >> i) & 1))
					a_draw(slot);
				if ((a_partitions >> (i + 1)) == 0)
					break;
				slot = a_tables.geometries[slot].nextPartition;
			}
		}

		inline std::uint32_t PartitionsOf(const SceneStore::Tables& a_tables, std::uint32_t a_object)
		{
			return a_object < a_tables.skinPartitions.size() ? a_tables.skinPartitions[a_object] : 0u;
		}

		/**
		 * @brief An object's fade root slot (Tables::objectFadeRoot), ~0u for none: a shadow input's, whose BuildDraws drops the
		 * caster while a stood-in root fades (kFadeRootStoodIn). A member's root changes only with a noted change (ListFadeRoot).
		 */
		inline std::uint32_t FadeRootOf(const SceneStore::Tables& a_tables, std::uint32_t a_object)
		{
			return a_object < a_tables.objectFadeRoot.size() ? a_tables.objectFadeRoot[a_object] : ~0u;
		}

		/**
		 * @brief A depth-segment input's fade row: its fade root's slot (FadeStateCS's state, which an owned root's members
		 * follow), and for the distance test of a root DCLF does not own (kObjectFadeTest), its fade-out distance. The node's
		 * centre is its object record's (BindlessObject::lodFadeNode), which a move rewrites; the slot and the distance only
		 * change with its membership and bindings.
		 */
		inline void SetFadeRow(DrawInput& a_input, const SceneStore::Tables& a_tables, std::size_t a_object)
		{
			if (a_object < a_tables.objectFadeRoot.size())
				a_input.fadeRoot = a_tables.objectFadeRoot[a_object];
			if (!(a_input.flags & (kObjectFadeTest | kObjectHeightTest)) || a_object >= a_tables.fadeDistance.size() || a_object >= a_tables.lodFade.size())
				return;
			if (a_tables.lodFade[a_object][3] < 0.0f)
				return;  // no fade node: nothing to measure
			a_input.fadeDistance = a_tables.fadeDistance[a_object];
		}

		// A draw template's geometry half, from a slot (SceneStore builds the object's first the same way). The second
		// stream repeats the first unless the draw has one of its own (a face shape's positions), which a skin's
		// partitions share.
		inline void SetSequenceGeometry(DrawSequence& a_sequence, const GeometryRecord& a_geometry, bool a_ownStream = false)
		{
			if (!a_ownStream) {
				a_sequence.streamBufferAddress = a_geometry.vertexAddress;
				a_sequence.streamBufferSize = static_cast<std::uint32_t>(std::min<std::uint64_t>(a_geometry.vertexBytes, UINT32_MAX));
				a_sequence.streamStride = a_geometry.vertexStride;
			}
			a_sequence.vertexBufferAddress = a_geometry.vertexAddress;
			a_sequence.vertexBufferSize = static_cast<std::uint32_t>(std::min<std::uint64_t>(a_geometry.vertexBytes, UINT32_MAX));
			a_sequence.vertexStride = a_geometry.vertexStride;
			a_sequence.indexBufferAddress = a_geometry.indexAddress;
			a_sequence.indexBufferSize = static_cast<std::uint32_t>(std::min<std::uint64_t>(a_geometry.indexBytes, UINT32_MAX));
			a_sequence.indexCount = a_geometry.indexCount;
			a_sequence.firstIndex = a_geometry.firstIndex;
		}

		inline GeometryDraw PackGeometryDraw(const SceneStore::Tables& a_tables, std::uint32_t a_slot, std::size_t a_count)
		{
			const auto& geometry = a_tables.geometries[a_slot];
			return { geometry.vertexAddress, static_cast<std::uint32_t>(std::min<std::uint64_t>(geometry.vertexBytes, UINT32_MAX)), geometry.vertexStride,
				geometry.indexAddress, static_cast<std::uint32_t>(std::min<std::uint64_t>(geometry.indexBytes, UINT32_MAX)), geometry.indexCount, geometry.firstIndex,
				geometry.nextPartition < a_count ? geometry.nextPartition : kNoPartition };
		}

		/**
		 * @brief The build's geometry slots' draws: brought up to date from the geometry log in the store (a_uploaded is the
		 * version the buffer holds, as the inputs saw it), or packed whole without one.
		 */
		void UpdateGeometryDraws(GeometryStore* a_store, std::uint64_t a_uploaded, const SceneStore::Tables& a_tables, std::uint32_t a_generation,
			std::uint32_t a_frame, GeometryDrawsOut& a_out);

		/**
		 * @brief The geometry buffer's uploads: the slots a buffer at a_held lacks, then the face streams' draws after them.
		 * The bytes sent.
		 */
		template <class Emit>
		std::size_t EmitGeometryDraws(const GeometryDrawsOut& a_out, std::uint64_t a_held, Emit&& a_emit)
		{
			std::size_t bytes = a_out.slots.Emit(a_held, a_emit);
			if (!a_out.faces.empty()) {
				a_emit(a_out.faces.data(), a_out.faces.size() * sizeof(GeometryDraw), a_out.SlotCount() * sizeof(GeometryDraw));
				bytes += a_out.faces.size() * sizeof(GeometryDraw);
			}
			return bytes;
		}

		// ---- NPC face shapes' positions (FaceSnapshots, SceneStore::Tables::faceStreams). Each face stream has a
		// GeometryDraw after the geometry slots, whose vertex buffer view is the shape's region of the epoch's
		// positions buffer; an input names it as its second stream (DrawInput::streamIndex).

		// BSGraphics::VertexDesc: the position attribute's flag in the second stream (bit 54 + attribute 0).
		constexpr std::uint64_t kPositionInSecondStream = 1ull << 54;

		// The GeometryDraw of object a_object's positions, or ~0u: not a face shape, or no positions buffer. AppendFaceStreams
		// writes them at these indices, after the slots (the geometry table holds both: ReserveSceneTables).
		inline std::uint32_t FaceStreamGeometry(const SceneStore::Tables& a_tables, std::size_t a_object, std::uint64_t a_positions)
		{
			if (!a_positions || a_object >= a_tables.faceStream.size() || a_tables.faceStream[a_object] == kNoFaceStream)
				return ~0u;
			return static_cast<std::uint32_t>(a_tables.geometries.size() + a_tables.faceStream[a_object]);
		}

		inline bool IsFaceObject(const SceneStore::Tables& a_tables, std::size_t a_object)
		{
			return a_object < a_tables.faceStream.size() && a_tables.faceStream[a_object] != kNoFaceStream;
		}

		// After PackGeometryDraws: the face streams' GeometryDraws, in faceStreams order.
		inline void AppendFaceStreams(const SceneStore::Tables& a_tables, std::uint64_t a_positions, GeometryDrawsOut& a_geometries)
		{
			if (!a_positions)
				return;
			auto& a_out = a_geometries.faces;
			for (const auto& stream : a_tables.faceStreams) {
				GeometryDraw draw{};
				// A free entry (its object's record has none) is a draw nothing names.
				if (stream.object == SceneStore::Tables::kNoFaceObject) {
					draw.nextPartition = kNoPartition;
					a_out.push_back(draw);
					continue;
				}
				draw.vertexBufferAddress = a_positions + std::uint64_t(stream.region) * 16;
				draw.vertexBufferSize = stream.vertexCount * 16;
				draw.vertexStride = 16;
				draw.nextPartition = kNoPartition;
				a_out.push_back(draw);
			}
		}

		// The second stream of a face object's draw template.
		inline void SetSequenceStream(DrawSequence& a_sequence, const SceneStore::Tables& a_tables, std::size_t a_object, std::uint64_t a_positions)
		{
			const auto& stream = a_tables.faceStreams[a_tables.faceStream[a_object]];
			a_sequence.streamBufferAddress = a_positions + std::uint64_t(stream.region) * 16;
			a_sequence.streamBufferSize = stream.vertexCount * 16;
			a_sequence.streamStride = 16;
		}

		// A commit's face uploads (render thread): a region at a time, and only when the head's snapshot is not the
		// one the epoch's buffer holds. The snapshot stays the walk's until its next walk, which is after the commit.
		template <class Uploads>
		std::uint32_t UploadFaceStreams(const std::vector<SceneStore::Tables::FaceStream>& a_streams, const std::shared_ptr<org::Buffer>& a_positions,
			ankerl::unordered_dense::map<std::uint32_t, std::uint64_t>& a_uploaded, Uploads& a_uploads)
		{
			std::uint32_t count = 0;
			for (const auto& stream : a_streams) {
				if (stream.object == SceneStore::Tables::kNoFaceObject)
					continue;
				auto& uploaded = a_uploaded[stream.region];
				if (uploaded == stream.generation)
					continue;
				a_uploads(a_positions, stream.positions, std::size_t(stream.vertexCount) * 16, std::uint64_t(stream.region) * 16);
				uploaded = stream.generation;
				++count;
			}
			return count;
		}

		/**
		 * @brief A (material, pipeline) pair's verdict for one build (MainBuild::AssembleRecord): its rows, or why it cannot draw.
		 * The skip counters are per draw, so a pair that cannot keeps the reason and every draw of it raises it.
		 */
		struct ResolvedBindings
		{
			std::uint32_t recordIndex = kNoRecord;  // RowsOf(pipeline, material)
			std::uint32_t skipReason = kNoSkip;
			bool deferred = false;
			std::uint32_t missingTexture = 0;  // the register that failed, for the skip sample
		};

		// The PerGeometry group, packed once per pipeline. Every object on a pipeline shares all of it but five
		// variables (the two world matrices and three shading values), so an object copies this and rewrites
		// only those instead of walking the whole variable table twice.
		struct GeometryTemplate
		{
			std::vector<std::byte> vs;
			std::vector<std::byte> ps;
			GeometryPatchOffsets offsets;
		};

		/**
		 * @brief A pipeline's PerGeometry template from its constants. Under DCLF_BINDLESS what those draws never read from
		 * the block (kVSBindlessGeometryUnread, kPSBindlessGeometryUnread: the frame lighting has a block of its own) packs
		 * as zero, so the block's bytes change only with the pipeline's own values; the group keeps its full size.
		 */
		inline void PackGeometryTemplate(const GeometryConstants& a_constants, std::span<const std::uint8_t> a_vsTable, std::span<const std::uint8_t> a_psTable, bool a_bindless,
			GeometryTemplate& a_out)
		{
			const auto vsSize = ConstantGroupSize(LightingVSLayout(), a_vsTable, kVSGroups[kPerGeometry], kVSFirstVariable[kPerGeometry]);
			const auto psSize = ConstantGroupSize(LightingPSLayout(), a_psTable, kPSGroups[kPerGeometry], kPSFirstVariable[kPerGeometry]);
			a_out.vs.assign(std::max<std::size_t>(vsSize, 16), std::byte{});
			a_out.ps.assign(std::max<std::size_t>(psSize, 16), std::byte{});
			const std::uint64_t vsUnread = a_bindless ? kVSBindlessGeometryUnread : 0, psUnread = a_bindless ? kPSBindlessGeometryUnread : 0;
			PackConstantGroup(a_constants.vs, LightingVSLayout(), a_vsTable, kVSGroups[kPerGeometry] & ~vsUnread, kVSFirstVariable[kPerGeometry], a_out.vs);
			PackConstantGroup(a_constants.ps, LightingPSLayout(), a_psTable, kPSGroups[kPerGeometry] & ~psUnread, kPSFirstVariable[kPerGeometry], a_out.ps);
			a_out.offsets = GeometryPatchOffsetsOf(a_vsTable, a_psTable);
			a_out.vs.resize(vsSize);
			a_out.ps.resize(psSize);
		}

		/** @brief ResidentRegion::indexOf: the object is not in the region. */
		constexpr std::uint32_t kNoRegion = ~0u;
		// ResidentRegion::drawsOf: a drawable decal entry (its sequence is its group's slot), and the draws an entry counts.
		constexpr std::uint8_t kRegionDecal = 0x80;
		constexpr std::uint8_t RegionDraws(std::uint8_t a_draws) { return a_draws & 0x7F; }
		constexpr std::uint64_t kNoPair = ~0ull;  // a region entry without bindings (a cull-only candidate)

		/** @brief CS_DCLF_RESIDENT_DRAW_PARITY=1: every 60 frames, each region entry written again from the tables and compared. */
		inline bool ResidentDrawParityEnabled()
		{
			return SwitchEnabled(Switch::ResidentDrawParity);
		}

		/**
		 * @brief Draw inputs kept densely by object slot, at the head of an input buffer: the main segments' resident regions
		 * (ResidentRegion) and each shadow mode's (ShadowKept::Mode). The inputs are journalled like any KeptArray; removing an
		 * entry moves the last one into its place.
		 */
		struct KeptRegion
		{
			KeptArray<DrawInput> inputs;
			std::vector<std::uint32_t> indexOf;  // per object slot: its entry, or kNoRegion

			std::uint32_t EntryOf(std::size_t a_object) const { return a_object < indexOf.size() ? indexOf[a_object] : kNoRegion; }
			bool Holds(std::size_t a_object) const { return EntryOf(a_object) != kNoRegion; }
			void Cover(std::size_t a_objects)
			{
				if (indexOf.size() < a_objects)
					indexOf.resize(a_objects, kNoRegion);
			}
			/** @brief A new entry for a_object at the end, marked; returns its index. */
			std::uint32_t Add(std::uint32_t a_object, const DrawInput& a_input = {})
			{
				Cover(std::size_t(a_object) + 1);
				auto& list = inputs.Mutable();
				const auto i = static_cast<std::uint32_t>(list.size());
				list.push_back(a_input);
				indexOf[a_object] = i;
				inputs.Mark(i);
				return i;
			}
			/** @brief The entry removed, and the last one moved into its place (the same index when none moved); kNoRegion when a_object had none. */
			struct Removal
			{
				std::uint32_t at = kNoRegion, from = kNoRegion;
			};
			Removal Remove(std::uint32_t a_object)
			{
				const std::uint32_t i = EntryOf(a_object);
				if (i == kNoRegion)
					return {};
				auto& list = inputs.Mutable();
				const auto tail = static_cast<std::uint32_t>(list.size() - 1);
				if (i != tail) {
					list[i] = list[tail];
					indexOf[list[i].objectIndex] = i;
					inputs.Mark(i);
				}
				list.pop_back();
				indexOf[a_object] = kNoRegion;
				return { i, tail };
			}
		};

		/**
		 * @brief One main segment's resident draws (drawcall-limit-fix.md, "Persistent resident draws"): the draw inputs of
		 * the resident records (SceneStore's), dense, at the head of the segment's input buffer. Written by the segment's
		 * builds only (the worker's, or an inline one after it), from the tables' change feed; the payloads share the inputs
		 * copy-on-write, and the commit uploads them when their version is not the buffer's.
		 */
		struct ResidentRegion : KeptRegion
		{
			LogCursor cursor;               // the change log, and the tables generation it was read from
			bool depth = false;             // the Z-prepass's: a join waits for the colour epoch's first draw of it
			std::vector<std::uint64_t> pairOf;   // per entry: its (material, pipeline)
			// Per entry: its sequences, 0 when it cannot be drawn this frame; a drawable decal is kRegionDecal (its sequence is in
			// its group's range, not the draws').
			std::vector<std::uint8_t> drawsOf;
			struct Pair
			{
				std::uint32_t slot = 0;   // its rows (RowsOf), once it can draw
				std::uint32_t count = 0;  // the entries using it
				bool ok = true;           // this build found its rows can draw it
				// What its resolution read (MainBuild::PairWitness): it is resolved again only when that changes. 0: never resolved.
				std::uint64_t witness = 0;
				std::uint32_t skip = 0;  // its skip reason when it cannot draw (the report counts it every build)
				std::array<std::uint64_t, 2> frameRegisters{};  // the frame textures its resolution asked for
				// The owners the region's bundle took for it (its material's binding block, its pipeline's shadow mask): a resolution
				// that finds others makes the bundle again.
				const void* bindingSeen = nullptr;
				const void* maskSeen = nullptr;
			};
			ankerl::unordered_dense::map<std::uint64_t, Pair> pairs;
			// The build's own inputs the pairs' resolutions read (the frame slots bound, the segment), every pair's frame textures,
			// and every pair's binding owners in one bundle, which each build's payload holds: made again when a pair is resolved.
			std::uint64_t frameWitness = 0;
			std::array<std::uint64_t, 2> frameRegisters{};
			std::shared_ptr<const void> owners;
			bool pairsChanged = true;
			// Which pairs a build checks (UpdateRegionPairs): those of the materials the tables' and the lookups' material logs name
			// since its last build, the pairs new since, and every pair of a pipeline whose half of the witness moved. A log it
			// cannot continue, or a frame witness that moved: every pair.
			LogCursor materialCursor, lookupCursor;
			ankerl::unordered_dense::map<std::uint32_t, std::vector<std::uint64_t>> materialPairs;  // material -> its pairs
			std::vector<std::uint64_t> freshPairs;
			ankerl::unordered_dense::map<std::uint32_t, std::uint64_t> pipelineWitness;  // the last build's, per pipeline
			// The pairs that cannot draw, by skip reason: the report counts them every build without visiting them.
			std::array<std::uint32_t, static_cast<std::size_t>(IndirectDraws::Skip::Count)> skipCounts{};
			ankerl::unordered_dense::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> pipelines;  // pipeline -> (set index, entries)
			MarkedList pending;  // depth: joined slots the colour epoch has not drawn yet
			std::size_t draws = 0;
			std::size_t decals = 0;               // drawable decal entries
			std::size_t undrawable = 0;           // entries with no draw this frame
			// What every face shape's stream index is relative to (the geometry slots' count; FaceStreamGeometry) and whether the
			// positions buffer exists: when either moves, every face entry is taken again. ~0: not yet read.
			std::size_t faceBase = ~std::size_t(0);
			std::vector<std::uint32_t> touched;  // this build: the slots whose entry it wrote or removed, or whose log entry it read
			// The whole scene (Step 5): every object the loop would draw as one input with its pair's record, not only the residents,
			// and the depth segment's cull-only candidates, decals (the colour segment's, with their ordinals) and face shapes (with
			// their position streams). Every other live object is the per-frame loop's (loopList: a decal with no ordinal or of
			// several partitions, a shape whose stream is missing), or nobody's (a candidate the segment does not submit).
			bool wholeScene = false;
			std::vector<std::uint32_t> loopList;   // the slots the loop visits
			std::vector<std::uint32_t> loopIndex;  // per slot: its place in loopList, or kNoRegion
			std::vector<std::uint8_t> candidate;   // per slot: a culling candidate only (no bindings, or shadow-only)
			std::size_t candidates = 0;

			/** @brief Empty, for every slot to be read again; the inputs' journal counts on (KeptArray::Clear). */
			void Reset()
			{
				auto kept = std::move(inputs);
				*this = ResidentRegion{};
				inputs = std::move(kept);
				inputs.Clear();
			}
		};

		/**
		 * @brief The colour segment's drawn state as its builds leave it (Step 5): per slot, whether the epoch draws it and as
		 * which geometry. A build changes the marks of the slots whose inputs it changed, and of the per-frame loop's objects;
		 * the payload carries the slots changed since the version the render thread applied (a ChangeJournal, whose holder is
		 * the render thread), so the render thread's state follows without ever seeing the whole set.
		 */
		struct DrawnMarks
		{
			bool active = false;
			std::uint32_t generation = 0;
			std::vector<std::uint8_t> drawn;
			std::vector<const RE::BSGeometry*> geometry;
			ChangeJournal changes;
			std::vector<std::uint32_t> loopDrawn;  // the slots the per-frame loop drew in the last build
			std::vector<std::uint32_t> loopStamp;  // per slot: the build that last drew it in the loop
			std::uint32_t serial = 0;
			void Set(std::uint32_t a_slot, const RE::BSGeometry* a_geometry, bool a_drawn)
			{
				if (drawn.size() <= a_slot) {
					drawn.resize(std::size_t(a_slot) + 1, 0);
					geometry.resize(std::size_t(a_slot) + 1, nullptr);
				}
				if ((drawn[a_slot] != 0) == a_drawn && (!a_drawn || geometry[a_slot] == a_geometry))
					return;
				drawn[a_slot] = a_drawn;
				if (a_drawn)
					geometry[a_slot] = a_geometry;
				changes.Mark(a_slot);
			}
		};

		/**
		 * @brief What BuildMainPayload keeps per segment across frames: the resident region and the drawn marks. The blocks and
		 * descriptors a draw reads are the rows' (MainRows), which both segments share.
		 *
		 * One per epoch kind (colour, Z-prepass), used by one build at a time: the worker's, or the render
		 * thread's when it builds inline (the worker's job for that kind is then done or waited for).
		 */
		struct BuildCache
		{
			struct PackedGroup
			{
				std::size_t size = 0;           // what ConstantGroupSize said
				std::vector<std::byte> bytes;   // the block's contents as packed (at least 16 bytes)
				bool valid = false;
			};
			std::vector<std::byte> scratch;
			ResidentRegion region;
			DrawnMarks drawnMarks;
		};

		/** @brief Packs a constant group into a cached group (at least 16 bytes, the rest zero). */
		inline void PackGroupInto(const ConstantBlock& a_block, const StageLayout& a_layout, std::span<const std::uint8_t> a_table, std::uint64_t a_variables, std::uint32_t a_first,
			BuildCache::PackedGroup& a_out)
		{
			const auto size = ConstantGroupSize(a_layout, a_table, a_variables, a_first);
			a_out.bytes.assign(std::max<std::size_t>(size, 16), std::byte{});
			PackConstantGroup(a_block, a_layout, a_table, a_variables, a_first, a_out.bytes);
			a_out.size = size;
			a_out.valid = true;
		}

		// The cache's source signatures: plain bytes of trivially copyable values, appended member by member.
		template <class T>
		void AppendSource(std::vector<std::byte>& a_out, const T& a_value)
		{
			static_assert(std::is_trivially_copyable_v<T>);
			const auto* bytes = reinterpret_cast<const std::byte*>(&a_value);
			a_out.insert(a_out.end(), bytes, bytes + sizeof(T));
		}

		inline void AppendSource(std::vector<std::byte>& a_out, std::span<const std::uint8_t> a_table)
		{
			AppendSource(a_out, a_table.size());
			const auto* bytes = reinterpret_cast<const std::byte*>(a_table.data());
			a_out.insert(a_out.end(), bytes, bytes + a_table.size());
		}

		// Whether the entry's sources equal the ones just written to the scratch; when not, they replace them.
		inline bool SameSources(std::vector<std::byte>& a_entry, const std::vector<std::byte>& a_current)
		{
			if (a_entry.size() == a_current.size() && std::memcmp(a_entry.data(), a_current.data(), a_current.size()) == 0)
				return true;
			a_entry.assign(a_current.begin(), a_current.end());
			return false;
		}


		/**
		 * @brief The main-pass epoch's draws, from the tables and the lookups: pure.
		 *
		 * Everything a draw needs from a service - the pipeline set indices, the shaders' constant tables, the
		 * descriptor indices of textures and samplers - comes from the lookups;
		 * an entry the render thread has not resolved yet defers the draw (deferredTextures), and the frame's
		 * own textures are left as patches for the commit. The per-frame constant blocks are addressed by
		 * their fixed slots (FrameSlotOffset) for the slots the inputs say the commit supplies.
		 */
		void BuildMainPayload(const MainInputs& a_in, const SceneStore::Tables& a_tables, const Lookups& a_lookups, MainPayload& a_out, MainRows& a_rows,
			BuildCache* a_cache = nullptr, ObjectRecordStore* a_objects = nullptr, BonesStore* a_bones = nullptr, GeometryStore* a_geometries = nullptr);

		// The objects a mode's inputs draw, as the geometry the native shadow loop withholds (PassCapture).
		inline std::shared_ptr<const PassCapture::ClaimSet> ShadowClaimSet(const std::vector<DrawInput>& a_inputs, const SceneStore::Tables& a_tables)
		{
			auto claims = std::make_shared<PassCapture::ClaimSet>();
			claims->reserve(a_inputs.size());
			for (const auto& input : a_inputs) {
				if (input.objectIndex < a_tables.objectGeometry.size())
					if (const auto* geometry = a_tables.objectGeometry[input.objectIndex])
						claims->insert(geometry);
			}
			return claims;
		}

		// The cascades' render mode (0xE, ShadowMapClamped) as an index of the shadow modes.
		constexpr std::uint32_t kSunShadowMode = 0xE - PassCapture::kFirstShadowMode;
		// The point lights' render mode (0xF, ShadowMapParabolic) as an index of the shadow modes.
		constexpr std::uint32_t kParabolicShadowMode = 0xF - PassCapture::kFirstShadowMode;

		/**
		 * @brief Whether an object is of the volumetric-only class in mode a_mode: drawn only by the views of the sun's
		 * volumetric lighting copy. A point light registers a volumetric-only caster like any other (drawcall-limit-fix.md,
		 * "Point lights' shadow culls without DCLF's entries"), so in the paraboloid mode it is an ordinary caster.
		 */
		inline bool VolumetricClass(std::uint32_t a_mode, std::uint32_t a_flags)
		{
			return !IsOcclusionMode(a_mode) && a_mode != kParabolicShadowMode && (a_flags & kObjectVolumetricOnly) != 0;
		}
		/** @brief A shadow input's flags: the object's, as the mode's views test them (BuildDrawsCS's caster classes). */
		inline std::uint32_t InputFlagsOf(std::uint32_t a_mode, std::uint32_t a_flags)
		{
			const std::uint32_t flags = (a_flags & ~kObjectDecal) | kInputDrawable;
			return VolumetricClass(a_mode, a_flags) || IsOcclusionMode(a_mode) ? flags : flags & ~kObjectVolumetricOnly;
		}

		/** @brief The technique bits a mode index adds to an object's base technique: none for an occlusion view's, which are complete. */
		inline std::uint32_t ModeBitsOf(std::uint32_t a_mode)
		{
			return IsOcclusionMode(a_mode) ? 0u : ShadowModeBits(PassCapture::kFirstShadowMode + a_mode);
		}

		/** @brief The technique object a_object draws with under mode a_mode: an occlusion view's own, or its caster technique with the mode's bits. 0: none. */
		inline std::uint32_t ModeTechnique(const SceneStore::Tables& a_tables, std::uint32_t a_mode, std::size_t a_object)
		{
			if (IsOcclusionMode(a_mode)) {
				const auto& column = a_tables.occlusionTechnique[OcclusionOfMode(a_mode)];
				return a_object < column.size() ? column[a_object] : 0u;
			}
			return (a_tables.objects[a_object].flags & kObjectNoShadow) ? 0u : (a_tables.shadowTechnique[a_object] | ModeBitsOf(a_mode));
		}

		/** @brief The techniques of the occlusion views among a_modeUsed object a_object draws into, ORed (its record, and whether alpha-tested). */
		inline std::uint32_t OcclusionTechniques(const SceneStore::Tables& a_tables, const std::array<bool, kShadowModeCount>& a_modeUsed, std::size_t a_object)
		{
			std::uint32_t techniques = 0;
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
				if (a_modeUsed[OcclusionModeOf(v)] && a_object < a_tables.occlusionTechnique[v].size())
					techniques |= a_tables.occlusionTechnique[v][a_object];
			return techniques;
		}

		/**
		 * @brief The last sun exclusion's inputs and verdicts: reused while the candidates, the tables, the mode's membership
		 * (ShadowKept::Mode::membership) and every object's flags and geometry (the change log's bindings and geometry causes)
		 * are what it was built from.
		 */
		struct SunExclusionCache
		{
			std::shared_ptr<const SunCandidates> candidates;
			LogCursor cursor;
			std::uint64_t membership = 0;
			std::vector<std::uint8_t> excluded;
			std::uint32_t excludedCount = 0;
			std::uint64_t version = 0;  // SunExclusion::version of excluded
			bool valid = false;
			std::uint64_t builds = 0, reused = 0;  // since the last report
			ParityCounter parity;
		};

		/**
		 * @brief The next frame's sun entry exclusion, from the candidates and the cascades' mode's inputs, which are
		 * what that mode's claims hold: a candidate stays in the cascade culls when one of its table objects casts (no
		 * kObjectNoShadow) and is not an input, since the engine must then still draw it. Null without candidates.
		 */
		std::shared_ptr<SunExclusion> BuildSunExclusion(const std::shared_ptr<const SunCandidates>& a_candidates, const ShadowPayload& a_payload, std::uint32_t a_mode,
			const SceneStore::Tables& a_tables, SunExclusionCache* a_cache = nullptr);

		// Whether an object's entry is outside every one of the sun's full-frustum processes, so the sun's cascade
		// culls never reach it (ShadowInputs::sunEntryProcesses): the CPU's verdict, from the tables, which the persistent
		// parity compares with the test BuildDraws makes on the input's fade row.
		inline bool OutsideSunEntry(const ShadowInputs& a_in, const SceneStore::Tables& a_tables, std::size_t a_object)
		{
			if (a_object >= a_tables.sunEntry.size() || a_tables.sunEntry[a_object][3] < 0.0f)
				return false;
			return OutsideSunEntryProcesses(a_in.sunEntryProcesses, a_tables.sunEntry[a_object].data());
		}

		/**
		 * @brief The shadow epoch's inputs and binding records, kept across frames (drawcall-limit-fix.md, "Persistent draw
		 * state", Step 6). Per render mode, a region of the casters' inputs, changed only by the change log's entries for them,
		 * by a mode's views changing their rasterizer states, and by what was waiting (a pipeline or a diffuse texture not yet
		 * resolved) becoming ready; the per-frame list is only the face shapes. The material rows (ShadowMaterialRow): the plain
		 * one and one per alpha-tested material, at slots the materials keep, lowest free first, in one table every view reads
		 * (ShadowResources::materialRows, which grows when the slots outnumber it: a slot past its capacity waits a frame). The
		 * rows and each mode's inputs are KeptArrays: a buffer is sent what changed since the version it holds.
		 */
		struct ShadowKept
		{
			static constexpr std::uint32_t kWaiting = ~0u - 1;  // an object's record while its material's texture is not resolved
			static constexpr std::uint32_t kNoRecord = ~0u;
			LogCursor cursor;
			const void* identity = nullptr;
			std::uint64_t build = 0;  // counts the builds: what ShadowKept::Mode::membership stamps
			KeptArray<ShadowMaterialRow> rows;
			ankerl::unordered_dense::map<const RE::BSShaderMaterial*, std::uint32_t> slotOf;
			std::vector<const RE::BSShaderMaterial*> slotMaterial;
			std::vector<ID3D11ShaderResourceView*> slotDiffuse;
			std::vector<std::uint32_t> slotRefs;
			std::vector<std::uint8_t> slotReady;
			std::vector<std::shared_ptr<const void>> slotOwner;  // the diffuse texture's import, while the slot's row names it
			std::vector<std::uint32_t> freeSlots;  // a min-heap: the lowest free slot is taken first, so the table stays dense
			// The rows written on events, not every build: a slot is written when acquired, while it waits (its texture not
			// resolved, or past the table's capacity), while its material's texture transform moves (the controllers' events,
			// watched while its two buffers differ or for 2 builds after the last event), and all of them when the lookups or
			// the capacity change.
			std::vector<std::uint32_t> rowDirty;
			std::vector<std::uint8_t> rowDirtyMark;
			std::vector<std::uint32_t> transformWatch;
			std::vector<std::uint64_t> transformWatchBuild;  // per slot: the build of its last event, 0 unwatched
			std::uint64_t lookupsGeneration = ~0ull;
			std::uint32_t rowCapacity = 0;
			// Every slot's owner, as one root for the payloads (bindingOwners), rebuilt when an owner changes.
			std::shared_ptr<const std::vector<std::shared_ptr<const void>>> owners;
			bool ownersChanged = true;
			std::vector<std::uint32_t> objectRecord;  // per object: its material row, 0 the plain one, kNoRecord, kWaiting
			// CS_DCLF_PERSISTENT_PARITY: the per-frame build's inputs the kept build lacks, by why, and the first of each.
			std::map<std::string, std::uint64_t> missingBy;
			std::map<std::string, std::string> missingFirst;
			std::vector<const RE::BSShaderMaterial*> objectMaterial;  // per object: the material its slot is held for
			struct Mode : KeptRegion
			{
				bool active = false;
				ModeRasterStates rasterStates;
				std::vector<const RE::BSGeometry*> claimedOf;  // per object: the geometry its entry claims
				std::vector<std::uint32_t> waiting;  // objects waiting for a pipeline or a texture
				std::vector<std::uint8_t> waitingMark;
				std::vector<std::uint32_t> faces;    // objects the frame's list writes (face shapes, second-stream positions)
				std::vector<std::uint8_t> faceMark;
				// The build version at which an object last joined or left the mode's inputs (the region or the faces): what
				// the sun exclusion's reuse reads (BuildSunExclusion).
				std::uint64_t membership = 0;
				// The region's geometries, each with the object whose entry claims it: a geometry that moves between slots is
				// claimed by its new slot before its old one's entry goes, and only its owner's removal drops it.
				ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint32_t> claims;
				bool claimsChanged = true;
				std::vector<const RE::BSGeometry*> lastFaces;
				std::shared_ptr<const PassCapture::ClaimSet> published;
				/** @brief Empty, for every object to be read again; the published claims stay, and the inputs' journal counts on. */
				void Reset()
				{
					auto kept = std::move(inputs);
					auto keptClaims = std::move(published);
					*this = Mode{};
					inputs = std::move(kept);
					published = std::move(keptClaims);
					inputs.Clear();
				}
			};
			std::array<Mode, kShadowModeCount> modes;
			/** @brief Empty, for every object to be read again; the journals count on and the report's counters stay. */
			void Reset()
			{
				auto keptRows = std::move(rows);
				std::array<Mode, kShadowModeCount> keptModes;
				for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
					keptModes[m].inputs = std::move(modes[m].inputs);
				const auto counters = std::tuple{ build, builds, entriesWritten, rowsWritten, resyncs };
				auto keptParity = std::move(parity);
				*this = ShadowKept{};
				rows = std::move(keptRows);
				rows.Clear();
				for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
					modes[m].inputs = std::move(keptModes[m].inputs);
					modes[m].inputs.Clear();
				}
				std::tie(build, builds, entriesWritten, rowsWritten, resyncs) = counters;
				parity = std::move(keptParity);
			}
			// Since the last report.
			std::uint64_t builds = 0, entriesWritten = 0, rowsWritten = 0, resyncs = 0;
			ParityCounter parity;
		};

		/**
		 * @brief A mode's inputs into its buffer: the kept region's entries newer than the version the buffer holds (all of them
		 * without the kept state), then the frame's own list after the region.
		 */
		template <class Emit>
		void EmitShadowInputs(const ShadowPayload& a_payload, std::uint32_t a_mode, std::uint64_t a_held, Emit&& a_emit)
		{
			const std::size_t region = a_payload.RegionCount(a_mode);
			a_payload.regionInputs[a_mode].Emit(a_held, a_emit);
			const auto& list = a_payload.inputList[a_mode];
			if (!list.empty())
				a_emit(list.data(), list.size() * sizeof(DrawInput), region * sizeof(DrawInput));
		}

		/**
		 * @brief The shadow epoch's frame-shared part and its per-mode inputs, from the tables and the lookups:
		 * pure. The per-view blocks are the commit's, because the views are captured while the engine draws
		 * them, after the build may have started.
		 */
		void BuildShadowPayload(const ShadowInputs& a_in, const SceneStore::Tables& a_tables, const Lookups& a_lookups, ShadowPayload& a_out,
			ObjectRecordStore* a_objects = nullptr, BonesStore* a_bones = nullptr, ShadowKept* a_kept = nullptr, GeometryStore* a_geometries = nullptr);

		/**
		 * @brief CS_DCLF_PERSISTENT_PARITY: a kept shadow build against the same build made the per-frame way. Per used mode, the
		 * same casters with the same inputs (the record's number apart), records binding the same textures and samplers with the
		 * same texcoord values, and the same claims.
		 */
		void CheckKeptShadow(const ShadowInputs& a_in, const SceneStore::Tables& a_tables, const Lookups& a_lookups, const ShadowPayload& a_kept, ShadowKept& k);

		/**
		 * @brief Resolves the descriptor entries an epoch's build reads (render thread, descriptor service
		 * active): the null texture, the sampler table, the projected textures, and every material slot used
		 * this frame. Material slots retain their exact imported bindings until a
		 * replacement or slot-retirement event releases them.
		 */
		void RefreshMaterialLookups(SceneStore& a_store, const SceneStore::Tables& a_tables, bool a_members, const SceneStore::ProjectedTextures& a_projected, Lookups& a_lookups);

		/** @brief The shadow epoch's entries: the alpha-tested casters' diffuse textures, and the pipelines of the modes in use. */
		void RefreshShadowLookups(SceneStore& a_store, const SceneStore::Tables& a_tables, const std::array<bool, kShadowModeCount>& a_modeUsed,
			const std::array<ModeRasterStates, kShadowModeCount>& a_modeRasterStates, DXGI_FORMAT a_dsvFormat, const std::array<DXGI_FORMAT, kOcclusionViews>& a_occlusionFormats,
			Lookups& a_lookups);

		constexpr std::size_t kAsyncColour = 0;
		constexpr std::size_t kAsyncZPrepass = 1;
		constexpr std::size_t kAsyncShadow = 2;

		// Whether a build made for `a_job` serves an epoch whose inputs are `a_epoch`: every scalar the build
		// read has to agree, the lookup generation is checked separately after the commit's refresh.
		// Whether BuildMainPayload reads MainInputs::eye/previousEye: only CS_DCLF_BINDLESS_PARITY's reference, the engine's
		// eye-relative PerGeometry groups, does.
		inline bool BuildReadsEye(const MainInputs& a_in)
		{
			return a_in.bindlessParity;
		}

		inline bool SameInputs(const MainInputs& a_job, const MainInputs& a_epoch)
		{
			auto sameEye = [](const RE::NiPoint3& a, const RE::NiPoint3& b) { return std::memcmp(&a, &b, sizeof(RE::NiPoint3)) == 0; };
			return a_job.frameNumber == a_epoch.frameNumber && a_job.depthOnly == a_epoch.depthOnly &&
			       a_job.bindlessParity == a_epoch.bindlessParity && a_job.withholding == a_epoch.withholding &&
			       sameEye(a_job.eye, a_epoch.eye) && sameEye(a_job.previousEye, a_epoch.previousEye) &&
			       a_job.vsFrameMask == a_epoch.vsFrameMask && a_job.psFrameMask == a_epoch.psFrameMask && a_job.decalCount == a_epoch.decalCount &&
			       a_job.drawnCommitted == a_epoch.drawnCommitted && a_job.drawnResync == a_epoch.drawnResync &&
			       a_job.materialPatchedFloats == a_epoch.materialPatchedFloats &&
			       a_job.materialPatchedVSFloats == a_epoch.materialPatchedVSFloats &&
			       a_job.addresses == a_epoch.addresses &&
			       a_job.lookupGeneration == a_epoch.lookupGeneration && a_job.tablesGeneration == a_epoch.tablesGeneration;
		}

		inline bool SameShadowInputs(const ShadowInputs& a_job, const ShadowInputs& a_epoch)
		{
			// Not the sun's full-frustum planes: the build does not read them, and the epoch writes its own into the latch (the
			// full-frustum cull runs after the scene phase, where the build may be kicked).
			// Nor CS's SharedData and FeatureData beyond their sizes: the build only places their blocks, and the epoch writes the
			// frame's over them (the water reflections' prepasses refresh them after the scene phase).
			return a_job.frameNumber == a_epoch.frameNumber && a_job.modeUsed == a_epoch.modeUsed &&
			       a_job.modeRasterStates == a_epoch.modeRasterStates && a_job.sunCandidates == a_epoch.sunCandidates &&
			       a_job.lightCandidates == a_epoch.lightCandidates &&
			       a_job.addresses == a_epoch.addresses && a_job.sharedData.size() == a_epoch.sharedData.size() &&
			       a_job.featureData.size() == a_epoch.featureData.size() &&
			       a_job.lookupGeneration == a_epoch.lookupGeneration && a_job.tablesGeneration == a_epoch.tablesGeneration;
		}

		// CS_DCLF_ASYNC=probe: two builds of the same inputs, compared byte for byte. The first difference is
		// named by buffer and offset.
		bool SamePayload(const MainPayload& a, const MainPayload& b, std::string& a_difference);
		bool SamePayload(const ShadowPayload& a, const ShadowPayload& b, std::string& a_difference);

		/** @brief Waits for an epoch's kicked job up to the async budget; a late one is cancelled, as the epoch builds its payload itself. */
		inline AsyncWorker::WaitResult JoinJob(const AsyncWorker::JobHandle& a_handle)
		{
			if (!a_handle)
				return AsyncWorker::WaitResult::None;
			auto& worker = AsyncWorker::Get();
			const auto joined = worker.Wait(a_handle, AsyncWaitBudget());
			if (joined == AsyncWorker::WaitResult::Late)
				worker.Cancel(a_handle);
			return joined;
		}

		/**
		 * @brief Whether an epoch commits its joined job's payload: done, and built for exactly the epoch's inputs (a_same(),
		 * which a lookup refresh that changed an entry the build read makes false). The verdict is counted; a_stale runs for a
		 * done job whose inputs differ.
		 */
		template <class Same, class Stale>
		bool TakeJob(AsyncWorker::WaitResult a_joined, IndirectDraws::Stats::Async& a_async, Same&& a_same, Stale&& a_stale)
		{
			switch (a_joined) {
			case AsyncWorker::WaitResult::Done:
				if (a_same())
					return true;
				++a_async.stale;
				a_stale();
				return false;
			case AsyncWorker::WaitResult::Late:
				++a_async.late;
				return false;
			case AsyncWorker::WaitResult::Failed:
				++a_async.failed;
				return false;
			default:
				++a_async.cancelled;
				return false;
			}
		}

		/**
		 * @brief CS_DCLF_ASYNC=probe: the worker's payload against a_build(a_probe), the same build on this thread from the
		 * job's inputs. The first difference is logged once.
		 */
		template <class Payload, class Build>
		void ProbeWorkerBuild(const Payload& a_worker, Payload& a_probe, IndirectDraws::Stats::Async& a_async, const char* a_name, Build&& a_build)
		{
			if (AsyncModeSetting() != AsyncMode::Probe)
				return;
			a_build(a_probe);
			++a_async.probeCompared;
			std::string difference;
			if (!SamePayload(a_worker, a_probe, difference) && a_async.probeDiffer++ == 0)
				logger::warn("[DCLF] async {} probe: the worker's build differs from the inline one: {}", a_name, difference);
		}

		// A commit's own uploads on the render thread, staged directly (StagedUploadBatch) and submitted when the
		// commit ends: each is one memcpy into mapped staging here and one copy in the submission, instead of a
		// trip through the upload pass's per-upload bookkeeping. The batch comes from a pool of released ones.
		/**
		 * @brief A released batch from a pool (neither a payload nor the upload service still holds it), reset, or a new one
		 * of a_pageSize pages added to the pool.
		 */
		inline std::shared_ptr<org::runtime::StagedUploadBatch> AcquireStagedBatch(std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& a_pool,
			std::size_t a_pageSize = std::size_t{ 4 } << 20)
		{
			ZoneScopedN("CS.DCLF.Stage.AcquireBatch");
			std::shared_ptr<org::runtime::StagedUploadBatch> batch;
			for (const auto& candidate : a_pool) {
				if (candidate.use_count() == 1) {
					batch = candidate;
					break;
				}
			}
			if (!batch) {
				batch = org::runtime::StagedUploadBatch::Create(a_pageSize);
				a_pool.push_back(batch);
			}
			batch->Reset();
			return batch;
		}

		class CommitUploads
		{
		public:
			explicit CommitUploads(std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& a_pool) :
				batch(AcquireStagedBatch(a_pool, std::size_t{ 1 } << 20))
			{}
			~CommitUploads()
			{
				if (!batch->Entries().empty())
					org::runtime::GetActiveUploadService()->SubmitStagedUploads(std::move(batch));
			}
			CommitUploads(const CommitUploads&) = delete;
			CommitUploads& operator=(const CommitUploads&) = delete;

			template <class Target>
			void operator()(const Target& a_target, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset)
			{
				if (a_data && a_bytes)
					batch->Stage(org::runtime::UploadTarget::FromShared(a_target), static_cast<std::size_t>(a_offset), a_data, a_bytes);
			}

		private:
			std::shared_ptr<org::runtime::StagedUploadBatch> batch;
		};

		// The graph extensions that add DCLF's passes (Passes.cpp).
		std::unique_ptr<org::RenderGraph::IRenderGraphExtension> MakeMainOpaqueExtension(std::shared_ptr<Resources> a_resources);
		std::unique_ptr<org::RenderGraph::IRenderGraphExtension> MakeShadowExtension(std::shared_ptr<ShadowResources> a_resources);
	}

	// What was one translation unit's anonymous namespace: its names resolve here as they did there.
	using namespace Draws;

	void StageMainPayload(MainPayload& a_payload, const Resources& a_resources, std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& a_pool);
	void StageShadowPayload(ShadowPayload& a_payload, const ShadowResources& a_resources, std::span<const std::shared_ptr<org::Buffer>> a_counts,
		std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& a_pool);

	struct IndirectDraws::Impl
	{
		std::shared_ptr<Resources> resources;
		// What the resources were created for; a change rebuilds them (and the graph).
		TargetFormats formats{};
		std::uint32_t width = 0, height = 0;
		std::optional<Capture> pending;  // this frame's main-pass bindings, until the epoch runs

		// The shadow views (CS_DCLF_SHADOWS). Their own resources and CPU staging, so the main path's are
		// untouched; the frame's shared uploads happen in the first view's epoch.
		std::shared_ptr<ShadowResources> shadow;
		bool shadowSetupFailed = false;
		// One view's culling counters, sampled every so many epochs and mapped a few frames later.
		struct ShadowCullReadback
		{
			winrt::com_ptr<ID3D11Buffer> count;
			std::uint32_t copiedFrame = 0;
			std::uint32_t view = 0, mode = 0;
		};
		std::optional<ShadowCullReadback> shadowCullReadback;
		std::uint32_t shadowCullEpochs = 0;
		std::uint32_t shadowLoggedTargets = 0;
		void ReadShadowCullCounters(std::uint32_t a_frame, IndirectDraws::ShadowStats& a_stats);
		using PendingView = Draws::PendingView;
		std::vector<PendingView> pendingViews;
		// Per occlusion view (ExecuteOcclusion): the view as the engine's RenderMask set it up (CaptureOcclusion), the state and
		// format its pipelines are built for, and the frame whose shadow commit uploaded its occluders.
		struct OcclusionState
		{
			PendingView view;
			std::uint32_t capturedFrame = ~0u;
			std::uint32_t rasterState = 0;
			DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
			std::uint32_t committedFrame = ~0u;
			std::uint32_t inputs = 0, skipped = 0;
		};
		std::array<OcclusionState, kOcclusionViews> occlusion;
		/** @brief The occlusion views' formats, for RefreshShadowLookups. */
		std::array<DXGI_FORMAT, kOcclusionViews> OcclusionFormats() const
		{
			std::array<DXGI_FORMAT, kOcclusionViews> viewFormats{};
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
				viewFormats[v] = occlusion[v].dsvFormat;
			return viewFormats;
		}
		// CS_DCLF_SHADOW_OWNERSHIP=static: the claim set built from the inputs of a mode, published once per
		// input rebuild (the views of one frame that share a mode share the inputs and the claims).
		/** @brief A shadow view not drawn: counts a hole when its mode withholds casters this frame, and hands the mode back. */
		void GiveBackShadowMode(std::uint32_t a_modeIndex, IndirectDraws::ShadowStats& a_stats);
		void PublishShadowClaims(std::uint32_t a_renderMode, const std::vector<DrawInput>& a_inputs, std::shared_ptr<const PassCapture::ClaimSet> a_built,
			IndirectDraws::ShadowStats& a_stats);
		ShadowPayload shadowPayload;
		std::shared_ptr<const void> shadowExecutionOwner;  // reused by the sky epoch's copy of the shadow records
		// The shadow job (CS_DCLF_ASYNC): kicked at BeforeShadowMaps, joined by ExecuteShadowFrame. The render
		// modes are known only once the views are captured, so the job builds last frame's set; the epoch's
		// own set is checked like every other input.
		struct ShadowJob
		{
			AsyncWorker::JobHandle handle;
			ShadowInputs inputs;
			std::array<bool, kShadowModeCount> modes{};
			std::array<ModeRasterStates, kShadowModeCount> rasterStates{};
			bool modesKnown = false;
			std::uint32_t views = 0;  // last frame's view count: the record slots the job stages
			std::uint32_t loggedStale = 0;
			std::uint32_t loggedStates = 0;
			std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>> stagedPool;
		} shadowJob;
		// Per mode, the rasterizer states its views have drawn with, per caster class (ExecuteShadowFrame): what the shadow
		// build's inputs are for.
		std::array<ModeRasterStates, kShadowModeCount> shadowStatesSeen{};
		ShadowPayload shadowProbePayload;
		/**
		 * @brief The sequence buffers' reserves (render thread, before an epoch): each grown to hold every draw the scene's tracked
		 * objects can produce (SceneDrawBound). A bound over the device's max sequence count, or over the sort's rank field,
		 * is a hard failure.
		 */
		void ReserveMainSequences(const SceneStore::Tables& a_tables);
		void ReserveShadowSequences(const SceneStore::Tables& a_tables, std::uint32_t a_slots, std::uint32_t a_first = 0);
		/** @brief Grows the shadow material rows' table to what the last build wanted (render thread, before the inputs are taken). */
		void ReserveShadowRows();
		/**
		 * @brief View slots for Skylighting's map and a_views views, and pipeline map rows of a_keys key slots, before the epoch:
		 * more slots are more buffers, which the shadow passes declare, so the graph is built again; either growth is a new
		 * latch block.
		 */
		void ReserveShadowLatch(std::uint32_t a_views, std::uint32_t a_keys, std::uint32_t a_rasterStates, std::uint32_t a_sunProcesses);
		// The main latch block's cascade region: a new block when the frame has more cascades than it holds.
		static void ReserveMainLatch(Resources& a_resources, std::uint32_t a_cascades, std::uint32_t a_shadowVolumes);
		std::uint32_t shadowRowsWanted = 0;  // the last shadow build's (ShadowPayload::rowsWanted)
		ShadowInputs PrepareShadowInputs(const SceneStore& a_store, const ShadowResources& a_resources, const std::array<bool, kShadowModeCount>& a_modeUsed,
			const std::array<ModeRasterStates, kShadowModeCount>& a_modeRasterStates) const;
		void DropShadowJob(IndirectDraws::Stats& a_stats);
		std::uint32_t shadowLoggedReasons = 0;
		bool ShadowNotReady(std::uint32_t a_reason, const char* a_what)
		{
			if (!((shadowLoggedReasons >> a_reason) & 1)) {
				shadowLoggedReasons |= 1u << a_reason;
				logger::info("[DCLF] shadow views not ready: {}", a_what);
			}
			return false;
		}
		bool SetupShadow();
		bool ImportShadowDepth(std::uint32_t a_index, std::uint32_t a_target);

		// The two main epochs' payloads (colour, then the Z-prepass): what their builds produce and their
		// commits upload. Kept past the commit, because the upload pass reads them when the epoch executes.
		std::array<MainPayload, 2> mainPayload;
		// The volatile t16+ inputs retain a live import only while their register
		// keeps naming that SRV. This bounds reuse without an age-based import cache.
		struct FrameTextureBinding
		{
			ID3D11ShaderResourceView* view = nullptr;
			GpuTextures::Binding binding;
		};
		std::array<FrameTextureBinding, kTextureRegisters> frameTextureBindings{};
		std::uint32_t frameTextureGeneration = ~0u;
		// Per main job: the staged upload batches its builds write (StageMainPayload), reused once released.
		std::array<std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>, 2> stagedPools;
		// The commits' own uploads on the render thread (CommitUploads).
		std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>> commitStagedPool;
		std::array<BuildCache, 2> buildCaches;  // indexed like mainPayload
		BuildCache* CacheFor(std::size_t a_job) { return &buildCaches[a_job]; }
		// The scene tables (SceneBuffers) and their stores, every epoch's: the builds run in frame order, each joined before
		// the next is kicked (KickShadowBuild and KickMainJob drop a job still outstanding).
		std::shared_ptr<SceneBuffers> scene;
		/** @brief The scene tables, created once (render thread). False when they have no device address. */
		bool EnsureSceneBuffers(rhi::Device a_device);
		/**
		 * @brief The scene tables grown to hold what a_tables does, and the per-object buffers with them (ReserveObjectBuffers).
		 * Render thread, before a build's inputs are taken: the tables are final for the frame from the scene phase on, so the
		 * first call of a frame does any growing and the builds after it see the capacities they are built against.
		 */
		void ReserveSceneTables(const SceneStore::Tables& a_tables);
		/** @brief The main and shadow resources' per-object buffers grown to the scene's object capacity. */
		void ReserveObjectBuffers();
		ObjectRecordStore objectStore;
		BonesStore boneStore;
		GeometryStore geometryStore;
		ObjectRecordStore* SceneObjects() { return &objectStore; }
		BonesStore* SceneBones() { return &boneStore; }
		/**
		 * @brief The per-frame streams (drawcall-limit-fix.md, "The streams leave the builds"): the object records and the bone
		 * rows the tables hold now, updated from the change log and uploaded as the changes since what the scene buffers hold.
		 * Render thread, at every epoch's commit, before its passes: a placement, a palette or a shading value reaches the
		 * epoch that draws it, whichever build it ran with, and no build waits for them. Returns the rows it sent.
		 */
		struct SceneStreams
		{
			std::size_t objects = 0, boneRows = 0, objectBytes = 0, boneRowsSent = 0;
		};
		SceneStreams CommitSceneStreams(SceneBuffers& a_scene, const SceneStore::Tables& a_tables, std::uint32_t a_frame, std::uint32_t a_generation,
			CommitUploads& a_uploads);
		GeometryStore* SceneGeometries() { return &geometryStore; }
		MainRows mainRows;  // both main segments' (MainRows)
		ShadowKept shadowKept;  // the shadow epoch's inputs and records (Step 6)
		ShadowKept* ShadowKeptState() { return &shadowKept; }
		SunExclusionCache sunExclusionCache;  // the shadow builds', in frame order
		SunExclusionCache parabolicExclusionCache;  // likewise, the paraboloid mode's (LocalLightCull)
		std::array<std::uint32_t, 5> decalWords{};  // the count buffer's decal words (19-22, then 27), uploaded per colour epoch

		/** @brief The per-frame constant blocks of an epoch from the capture's mirrors (render thread; records the Z-prepass's bytes for the replay). */
		void PackFrameBlocks(const Capture& a_capture, bool a_depthOnly, FrameBlocks& a_out);
		/** @brief The build's inputs, snapshotted on the render thread. Without a capture (a job kicked ahead of the epoch) the eye is the replayed one. */
		MainInputs PrepareMainInputs(const Capture* a_capture, bool a_depthOnly, const Resources& a_resources, std::uint32_t a_vsMask, std::uint32_t a_psMask, const SceneStore& a_store);

		// The main epochs' jobs (CS_DCLF_ASYNC), indexed like mainPayload (kAsyncColour, kAsyncZPrepass): the
		// colour job is kicked at Prepass, the Z-prepass job at the end of EarlyPrepass, each joined by its
		// epoch. The inputs are kept to check the epoch's against; the frame-slot masks the epoch will supply
		// are predicted from the same epoch's last ones and validated the same way.
		struct MainJob
		{
			AsyncWorker::JobHandle handle;
			MainInputs inputs;
			std::uint32_t vsMask = 0, psMask = 0;
			bool masksKnown = false;
			std::uint32_t loggedStale = 0;
		};
		std::array<MainJob, 2> mainJobs;
		MainPayload probePayload;  // CS_DCLF_ASYNC=probe: the inline build to compare the worker's against
		// CS_DCLF_CAPTURE_POINT_PARITY (CheckCapturePoint), since the last report: the frames that differ, and how often
		// each binding does.
		struct
		{
			std::uint32_t checks = 0, frames = 0;
			ankerl::unordered_dense::map<std::string, std::uint32_t> differ;
		} captureParity;
		void KickMainJob(bool a_depthOnly, const RE::NiPoint3* a_eye, const RE::NiPoint3* a_previousEye, IndirectDraws::Stats& a_stats);
		void DropMainJob(std::size_t a_job, IndirectDraws::Stats& a_stats);
		void LogStaleMainJob(std::size_t a_job, const MainInputs& a_actual);
		/** @brief Inside the epoch's preparation: the frame textures and blocks, the uploads, the PassFrame. */
		bool CommitMainPayload(const Capture& a_capture, const FrameBlocks& a_blocks, MainPayload& a_payload,
			const std::shared_ptr<Resources>& a_resources, SceneStore& a_store, IndirectDraws::Stats& a_stats,
			std::vector<std::shared_ptr<const void>>& a_bindingOwners);

		// What the colour epoch draws (drawcall-limit-fix.md, "Persistent draw state", Step 5): the native loop skips a pass
		// whose geometry the epoch drew, and the claims are what it draws. Only the colour epoch records it, so the native
		// loop never skips an object that the Z-prepass drew but the colour pass then left out (a missing texture, say),
		// which would leave a hole that writes depth and shows the background. Kept from the colour builds' changes alone
		// (DrawnMarks): per geometry and per object slot, whether it is drawn now and, when not, the last frame it was.
		struct DrawnGeometry
		{
			std::uint32_t last = 0;
			std::uint32_t slot = ~0u;  // the slot that draws it: a geometry moves between slots, and only its own slot undraws it
			bool drawn = false;
		};
		ankerl::unordered_dense::map<const RE::BSGeometry*, DrawnGeometry> drawnGeometry;
		std::vector<SlotDrawn> slotDrawn;
		std::uint64_t drawnCommitted = 0;       // the DrawnMarks version applied
		bool drawnResync = true;                // the next colour build sends every slot
		std::uint32_t drawnCommitFrame = ~0u;   // the frame of the last colour commit
		// The claims (PassCapture), kept: a geometry is claimed from its draw until a frame after its last one.
		PassCapture::ClaimSet claimSet;
		std::shared_ptr<const PassCapture::ClaimSet> publishedClaims;
		bool claimsChanged = true;
		std::vector<std::pair<const RE::BSGeometry*, std::uint32_t>> pendingUnclaims;  // (geometry, the frame it is unclaimed from)
		std::uint32_t claimsAdded = 0, claimsDropped = 0, claimsDroppedAfterCull = 0;
		std::uint64_t drawnChangesApplied = 0, drawnResyncs = 0;
		std::vector<const RE::BSGeometry*> newlyDrawn;  // geometries whose drawn mark turned on since PublishClaims (PrimaryCull::Admit)
		bool DrawnThisFrame(const RE::BSGeometry* a_geometry, std::uint32_t a_frame) const
		{
			if (drawnCommitFrame != a_frame)
				return false;
			const auto it = drawnGeometry.find(a_geometry);
			return it != drawnGeometry.end() && it->second.drawn;
		}
		/** @brief A colour build's change for one slot: drawn as a_geometry now, or not drawn. */
		void ApplyDrawn(std::uint32_t a_slot, const RE::BSGeometry* a_geometry, bool a_drawn, std::uint32_t a_frame)
		{
			if (slotDrawn.size() <= a_slot)
				slotDrawn.resize(std::size_t(a_slot) + 1);
			auto& slot = slotDrawn[a_slot];
			if (slot.drawn && slot.geometry && (slot.geometry != a_geometry || !a_drawn)) {
				// Its geometry is undrawn only while this slot is the one drawing it (the order of a build's changes is the
				// order of its slots, so a geometry that moved may already be drawn by its new slot).
				if (auto held = drawnGeometry.find(slot.geometry); held != drawnGeometry.end() && held->second.slot == a_slot) {
					held->second.drawn = false;
					held->second.last = a_frame - 1;
					held->second.slot = ~0u;
					pendingUnclaims.emplace_back(slot.geometry, a_frame + 1);
				}
				slot.last = a_frame - 1;
			}
			if (a_drawn && a_geometry && (!slot.drawn || slot.geometry != a_geometry)) {
				newlyDrawn.push_back(a_geometry);
				auto& held = drawnGeometry[a_geometry];
				held.drawn = true;
				held.slot = a_slot;
				if (claimSet.insert(a_geometry).second) {
					claimsChanged = true;
					++claimsAdded;
				}
			}
			if (a_drawn)
				slot.geometry = a_geometry;
			slot.drawn = a_drawn;
			++drawnChangesApplied;
		}
		// The vertex-stage inputs the Z-prepass wrote its depth with. The colour pass tests EQUAL against
		// that depth, so it has to transform the geometry to exactly the same place - and it cannot simply
		// read the buffers again, because the engine rewrites the per-frame constants between the depth
		// pass and the composite, and the camera moves on. Reusing the bytes makes the two epochs agree.
		std::array<std::vector<std::byte>, kConstantBufferRegisters> prepassVS;
		RE::NiPoint3 prepassEye, prepassPreviousEye;
		// The main pass's viewport depth range, kept from its capture. The Z-prepass writes its depth with
		// this rather than with the depth pass's own range, so that the depth in the buffer is what the
		// main pass - DCLF's colour draws and the native draws alike - tests EQUAL against. It is stable
		// frame to frame, so the previous frame's value is right for this frame's prepass.
		float mainMinDepth = 0.0f, mainMaxDepth = 0.0f;
		bool prepassInputs = false;
		// The colour build kicked at EarlyPrepass (KickZPrepassBuild), and the witness of the tables it read
		// (SceneStore::BuildInputsWitness), until Prepass keeps or replaces it.
		bool colourEarly = false;
		std::uint64_t colourWitness = 0;
		// The shadow build kicked at the end of the scene phase (KickShadowBuildEarly), and the change logs' witness it read
		// (SceneStore::ShadowInputsWitness), until BeforeShadowMaps keeps or replaces it.
		bool shadowEarly = false;
		std::uint64_t shadowWitness = 0;
		std::uint32_t loggedDepthViewport = 0;
		std::uint32_t loggedColourViewport = 0;

		winrt::com_ptr<ID3D11Texture2D> mainPassDepth;  // what the main pass binds, to check the depth pass against
		bool loggedDepthMismatch = false;
		bool loggedNoViewProj = false;
		// The colour epoch's jitter measurement (Uploads.cpp, under CS_DCLF_STATS).
		struct JitterProbe
		{
			std::uint32_t epochs = 0;
			float vsUnjittered = 0, vsUnjitteredMax = 0, vsMain = 0, vsMainMax = 0, vsCached = 0, vsCachedMax = 0, vsPrevious = 0, vsPreviousMax = 0;
			std::array<float, 16> previous{};
			bool havePrevious = false;
		} jitterProbe;
		bool loggedFrameConstants = false;

		// CS_DCLF_GBUFFER_PROBE=<x>x<y>: the texel the in-epoch probe passes copy out, read back a few
		// frames later from the buffer they wrote.
		winrt::com_ptr<ID3D11Buffer> gbufferStaging;
		std::uint32_t gbufferFramesLeft = 0;
		std::uint32_t gbufferEpochs = 0;
		std::uint32_t gbufferCount = 0;
		std::uint32_t gbufferX = 0, gbufferY = 0;
		std::array<std::uint32_t, kColorTargets> gbufferBytes{};
		// The main pass's targets, kept past the capture so their formats can be named in the report.
		std::array<winrt::com_ptr<ID3D11Texture2D>, kColorTargets> probeTargets;
		winrt::com_ptr<ID3D11Texture2D> probeDepth;
		std::uint32_t probeTargetCount = 0;

		void ProbeGBuffer(const char* a_label);

		// CS_DCLF_BUILD_PARITY: GPU output copied to staging after an epoch, compared a few frames later.
		struct ParityReadback
		{
			winrt::com_ptr<ID3D11Buffer> sequences, count;
			std::vector<DrawSequence> expected;
			// Per decal group, by slot: what the CPU expects there (a culled slot reads back with an index
			// count of zero and is otherwise identical).
			std::array<std::vector<DrawSequence>, kDecalGroups> expectedDecals;
			std::uint32_t sequenceDraws = 0, sequenceDecals = 0;  // the buffer's ranges when it was copied
			std::uint32_t framesLeft = 0;
			// By object: the local shadow lights the CPU selects with the volumes the epoch uploaded (LocalShadowLights), which
			// BuildDraws put in the draw's object word (kObjectLocalShadowMask); none on the Z-prepass.
			ankerl::unordered_dense::map<std::uint32_t, std::uint32_t> expectedLocalShadows;
		};
		std::optional<ParityReadback> parity;
		std::uint32_t parityEpochs = 0;

		// The culling's own counters, read back from the draw-count buffer a few frames after the epoch
		// wrote them. Always on: the per-phase counts are what says whether the culling is doing anything,
		// and the buffer is two words.
		struct CullReadback
		{
			winrt::com_ptr<ID3D11Buffer> count;
			std::uint32_t framesLeft = 0;
			std::uint32_t sunCpuTested = 0, sunCpuMissed = 0;
		};
		std::optional<CullReadback> cullReadback;
		std::uint32_t cullEpochs = 0;
		BuildDrawsLatch sunUpload{};  // the colour epoch's last latch
		std::vector<SunAccumulation::GpuCascade> sunCascades;  // and the cascades its BuildDraws tested against
		// The local shadow lights its BuildDraws selected against (LocalShadowLights), and their volumes as uploaded.
		LocalShadowLights localShadows;
		std::vector<GpuShadowVolume> shadowVolumes;

		void ReadCullCounters(const std::shared_ptr<Resources>& a_resources, IndirectDraws::Stats& a_stats, const MainPayload& a_payload);
		/**
		 * @brief CS_DCLF_PERSISTENT_PARITY: tree wind parity. Every 120th colour epoch the records of up to 64 tree members are
		 * read back (3 frames later) and checked: TreeParams.y and .w against the frame's wind magnitude and the node's leaf
		 * frequency, the amplitude's fade against its own distance, and the gust against the engine's formula at the record's
		 * timer; and the timers' drift from the engine's own clock on the node is reported.
		 */
		void ReadTreeWind(const std::shared_ptr<Resources>& a_resources);
		struct TreeReadback
		{
			winrt::com_ptr<ID3D11Buffer> records;
			std::uint32_t framesLeft = 0;
			struct Sample
			{
				std::uint32_t object = 0, tree = 0;
				const void* node = nullptr;
			};
			std::vector<Sample> samples;
			TreeWindFrame inputs{};
		};
		std::optional<TreeReadback> treeReadback;
		ankerl::unordered_dense::map<const void*, std::pair<float, float>> treeTimers;  // node -> engine's and GPU's timers at the last readback
		std::uint32_t treeEpochs = 0;

		/**
		 * @brief CS_DCLF_FADE_PARITY: FadeStateCS against the C++ port (Scene/FadeState.h). Every 30th frame the depth commit
		 * has the pass log kFadeLogEntries roots from a rotating cursor (NextFadeLog keeps their static rows and the frame's
		 * inputs); the log is copied after the depth epoch and checked three frames later (ReadFadeLog): each logged update
		 * made again by the port from the state it started from, with the GPU's frustum verdict.
		 */
		std::uint32_t NextFadeLog(std::uint32_t a_frame, const SceneStore::Tables& a_tables, const FadeFrame& a_inputs);
		void ReadFadeLog(const std::shared_ptr<Resources>& a_resources);
		struct FadeReadback
		{
			winrt::com_ptr<ID3D11Buffer> log;
			std::uint32_t frame = 0, base = 0, framesLeft = 0;
			std::vector<FadeRootStatic> roots;  // the static rows from base, as uploaded that frame
			FadeFrame inputs{};
			// An owned root with engine-drawn parts (not stood in): its node after the engine's own OnVisible that frame (the
			// list jobs have run), which FadeStateCS's update for its members must equal.
			std::vector<FadeNodeState> nodes;
			std::vector<std::uint8_t> engine;
		};
		std::optional<FadeReadback> fadeReadback;
		std::uint32_t fadeLogCursor = 0;
		struct FadeParity
		{
			std::uint64_t logs = 0, updates = 0, inView = 0, serviced = 0, exact = 0, rounding = 0, differ = 0;
			std::uint64_t engineChecked = 0, engineExact = 0, engineRounding = 0, engineDiffer = 0;  // the GPU's state against the engine's node
			std::string first, engineFirst;
		};
		FadeParity fadeParity;

		// CS_DCLF_SET_PARITY: one snapshot per frame in flight, read back a few frames later.
		struct SetParityFrame
		{
			winrt::com_ptr<ID3D11Buffer> staging;
			std::uint32_t frame = 0;
			std::uint32_t framesLeft = 0;
			std::vector<std::uint8_t> depthState, colourState;  // MainPayload::objectState of the two builds
			// Per object: bit 0 withheld from the native loop this frame (claimed, and registered by the
			// engine), bit 1 native-visible, bit 2 alpha tested, bit 3 claimed.
			std::vector<std::uint8_t> flags;
			std::vector<const RE::BSGeometry*> geometry;
		};
		std::deque<SetParityFrame> setParityFrames;
		std::vector<winrt::com_ptr<ID3D11Buffer>> setParityStaging;  // released stagings, reused
		struct SetParityCounts
		{
			std::uint32_t frames = 0, framesWithDamage = 0, skipped = 0;
			std::uint32_t depthOnly = 0, colourOnly = 0, colourUnpublished = 0, withheldUndrawn = 0, withheldCulled = 0;
			std::uint32_t colourDrawnTotal = 0, depthDrawnTotal = 0;
			std::uint32_t alphaDepthOnly = 0, alphaColourOnly = 0, alphaWithheldUndrawn = 0;
			std::uint32_t samples = 0;
			// One-frame gaps: an object the engine kept on three consecutive frames, drawn on the first and the
			// third and withheld and GPU-culled on the second - a flicker, unless it really was hidden for that
			// one frame. By the gap frame's verdict (occluded retest, rejected), and how many were trees.
			std::uint32_t gaps = 0, gapsRetest = 0, gapsRejected = 0, gapsTree = 0, gapSamples = 0;
		} setParity;
		// Per geometry, the last two frames' state for the gap detector: 0 not kept, 1 kept and drawn by someone,
		// 2 kept, withheld and GPU-culled (verdict in the high bits).
		struct GapHistory
		{
			std::uint32_t frame = 0;
			std::uint8_t last = 0, before = 0;
		};
		ankerl::unordered_dense::map<const RE::BSGeometry*, GapHistory> gapHistory;
		void CheckSetParity(const std::shared_ptr<Resources>& a_resources, const MainPayload& a_depth, const MainPayload& a_colour);

		void CheckBuildParity(const std::shared_ptr<Resources>& a_resources, const MainPayload& a_payload, IndirectDraws::Stats& a_stats);

		std::uint32_t loggedReasons = 0;

		bool NotReady(std::uint32_t a_reason, const char* a_what)
		{
			if (!((loggedReasons >> a_reason) & 1)) {
				loggedReasons |= 1u << a_reason;
				logger::info("[DCLF] Main-pass draws not ready: {}", a_what);
			}
			return false;
		}

		bool Setup(const Capture& a_capture, bool a_depthOnly = false);
	};
}

#endif
