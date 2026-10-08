#pragma once

// IndirectDraws' implementation, shared by the files of this folder only (IndirectDraws.h is the interface).

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#	include "Features/DrawcallLimitFix/Draws/IndirectDraws.h"
#	include "Features/DrawcallLimitFix/Common/AsyncWorker.h"
#	include "Features/DrawcallLimitFix/Common/RenderThreadBudget.h"
#	include "Features/DrawcallLimitFix/Engine/EngineReadWindow.h"
#	include "Features/DrawcallLimitFix/Engine/ConstantMirror.h"
#	include "Features/DrawcallLimitFix/Draws/DrawPipelines.h"
#	include "Features/DrawcallLimitFix/Draws/DrawPipelinesRhi.h"
#	include "Features/DrawcallLimitFix/Engine/EngineAccess.h"
#	include "Features/DrawcallLimitFix/Engine/EngineStates.h"
#	include "Features/DrawcallLimitFix/Engine/FaceSnapshots.h"
#	include "Features/DrawcallLimitFix/Scene/FadeState.h"
#	include "Features/DrawcallLimitFix/Draws/FrameData.h"
#	include "Features/DrawcallLimitFix/Draws/FrameValues.h"
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
#	include <ORGModuleServices/Async/RevisionAssembly.h>
#	include "Features/DrawcallLimitFix/Common/Switches.h"
#	include "Features/DrawcallLimitFix/Common/SceneScheduler.h"
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
#	include <Resources/Buffers/VersionedBuffer.h>
#	include <ORGModuleServices/VersionedBufferGrowth.h>
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
#	include "Versioned.h"
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
			return SameHandle(a.zLayout, b.zLayout) && SameHandle(a.zDrawSignature, b.zDrawSignature);
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
		// variable. Both are produced from tables.objects and the shading row by the same rules, so the
		// comparison is exact rather than tolerant - a tolerance here would only hide a layout mistake.
		void CheckBindlessRecord(const SceneStore::Tables& a_tables, std::uint32_t a_objectIndex, const BindlessObject& a_record,
			const BindlessPlacement& a_placement, const BindlessShading& a_shading, const RE::NiPoint3& a_eye,
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
		/**
		 * @brief A copy a main epoch's latched-copies pass (MainLatchedCopiesPass) records: bytes of the latch block's slot region, at
		 * latchOffset, to a target the pass declared, at dstOffset. The commit wrote the bytes (LatchedUploads).
		 */
		struct LatchedCopy
		{
			const void* target = nullptr;  // LatchedTarget::key: the buffer, or a versioned buffer's (VersionedBuffer::Key)
			std::uint32_t latchOffset = 0, bytes = 0;
			std::uint64_t dstOffset = 0;
			bool operator==(const LatchedCopy&) const = default;
		};
		/** @brief A commit's latched copies: the block they read, and the list (part of the frame's shape). */
		struct LatchedList
		{
			std::shared_ptr<const org::LatchBlock> latch;
			std::vector<LatchedCopy> copies;
			bool operator==(const LatchedList& o) const { return latch == o.latch && copies == o.copies; }
		};

		struct ZBucketPlan;
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
			// The Z-prepass's plain draws (MainOpaquePass): a call per bucket, a group of pipeline slots sharing a depth pipeline. Phase
			// 2's ranges are the same past sequenceDraws.
			struct ZCall
			{
				std::uint32_t bucket = 0, first = 0, capacity = 0;
				rhi::PipelineHandle pipeline{};
				bool operator==(const ZCall& o) const
				{
					return bucket == o.bucket && first == o.first && capacity == o.capacity && SameHandle(pipeline, o.pipeline);
				}
			};
			std::vector<ZCall> zCalls;
			// The commit's per-frame values the epoch's first pass copies from the latch (MainLatchedCopiesPass).
			LatchedList latched;
			// Not compared (the latch block's layout, and the bucket plan zCalls are the calls of): what a commit writing its values
			// into this shape writes them against - a scene revision's shape, which that commit did not make (R3c).
			MainLatchLayout latchLayout;
			std::shared_ptr<const ZBucketPlan> zPlan;

			bool SameShape(const PassFrame& o) const
			{
				return zCalls == o.zCalls && latched == o.latched && drawCapacity == o.drawCapacity && decalCapacity == o.decalCapacity && sequenceDraws == o.sequenceDraws &&
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
		 * @brief The versions of the kept tables (ObjectRecordStore, ExtrasStore, GeometryStore) a set of buffers holds, written
		 * by the commit that uploads them; 0 for new buffers, which no version is.
		 */
		struct TablesHeld
		{
			std::uint64_t objects = 0, extras = 0, geometries = 0;
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
			std::shared_ptr<org::Buffer> counts, offsets, blockSums;
			Versioned staging, ranks;
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
				sort->staging = MakeVersioned(CreateWords(std::uint64_t(a_draws) * sizeof(DrawSequence) / 4, true, "cs.dclf.sort-staging"));
				sort->ranks = MakeVersioned(CreateWords(a_draws, true, "cs.dclf.sort-ranks"));
				return sort;
			}

			void Register(org::RenderGraph& a_graph) const
			{
				// Not the staging or the ranks: versioned, their passes declare them (Register, Passes.cpp).
				for (const auto& buffer : { counts, offsets, blockSums })
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

		struct IndexPool;

		/**
		 * @brief The scene's tables on the GPU, one set for every epoch - the shadow views', Skylighting's, the Z-prepass's and
		 * the colour segment's: the object records (t127), the bone rows (t126), the geometry table (BuildDraws' draws) and the
		 * NPC face shapes' positions (the face draws' second stream). Kept by one store each (Impl::objectStore, extrasStore,
		 * geometryStore), whose builds run in frame order - the shadow build, the Z-prepass's, the colour build's, each joined
		 * before the next is kicked - so each commit sends what changed since the version the buffers hold, which the commit
		 * before it wrote. Created with the first epoch's resources and kept across their recreation.
		 */
		/**
		 * @brief The fade write-back's host side (drawcall-limit-fix.md, "The fade write-back"): the event list's readback per
		 * frame slot, read when the slot comes round again (FadeEventReadbackPass, recording), and its events handed to the main
		 * thread in batches through a lock-free stack (IndirectDraws::ApplyFadeWriteBack).
		 */
		struct FadeWriteBack
		{
			struct Batch
			{
				std::vector<FadeEvent> events;
				std::uint32_t frame = 0;  // the scene frame whose FadeStateCS appended them (the list's header)
				Batch* next = nullptr;
			};
			// The recording's: a host buffer per frame slot, the events it holds, and whether it holds an execution's.
			std::vector<std::shared_ptr<org::Buffer>> readback;
			std::vector<std::uint32_t> readbackEvents;
			std::vector<std::uint8_t> filled;
			// A larger host buffer per slot once the list has grown (the render thread's), which the slot's next recording takes
			// after reading the one it holds: no execution's events are lost to a growth.
			std::unique_ptr<std::atomic<std::shared_ptr<org::Buffer>>[]> next;
			// The events FadeStateCS may append: the fewest any slot's host buffer holds (never more than the list).
			std::atomic<std::uint32_t> capacity{ 0 };
			// The largest count a recording read past its buffer's events: the list grows to it (the roots it left out try again).
			std::atomic<std::uint32_t> wanted{ 0 };
			std::atomic<Batch*> batches{ nullptr };
			// The write-back task's (IndirectDraws::KickFadeWriteBack, step 6e S4: one at a time on DCLF's executor, never joined): per
			// root slot the scene frame of the event last taken - an older one (a list read late) is not written over it. Its alone.
			std::vector<std::uint32_t> appliedFrame;
			std::atomic<bool> running{ false };
			/**
			 * @brief What a task found to write (step 6e F1): the stores, in event order, and the tables snapshot they were judged
			 * against - held until the render thread makes them, so no node they name is let go first (its publication's
			 * retirement node). The task touches no engine memory; the render thread makes the stores at the next frame's start.
			 */
			struct Stores
			{
				struct Store
				{
					void* node = nullptr;
					std::uint32_t flags = 0;  // the fade bits (kFadeFlagMask) only
					float fade = 0.0f;
				};
				std::shared_ptr<const SceneStore::Tables> tables;
				std::vector<Store> stores;
			};
			std::atomic<Stores*> ready{ nullptr };
			// Since the last report: milestones taken and written, stale, frames whose start found the last task still running, and
			// the render thread's time making the stores.
			std::atomic<std::uint64_t> applied{ 0 }, stale{ 0 }, busy{ 0 }, storeNs{ 0 };

			void Push(Batch* a_batch)
			{
				a_batch->next = batches.load(std::memory_order_relaxed);
				while (!batches.compare_exchange_weak(a_batch->next, a_batch, std::memory_order_release, std::memory_order_relaxed)) {}
			}
			~FadeWriteBack()
			{
				delete ready.exchange(nullptr);
				for (Batch* b = batches.exchange(nullptr); b;) {
					Batch* following = b->next;
					delete b;
					b = following;
				}
			}
			static std::uint64_t BytesFor(std::uint32_t a_events) { return (kFadeEventHeaderWords + std::uint64_t(a_events) * 4) * sizeof(std::uint32_t); }
		};

		/**
		 * @brief The scene tables' capacities (Impl::ReserveSceneTables), changed with their buffers' growth as one (Growths::Change):
		 * what the commits send within and the builds bound their objects by (SceneFit). The palettes are FrameValues', in buffers of
		 * the frame's own.
		 */
		struct SceneSizing
		{
			std::uint32_t objectCapacity = 0, geometryRows = 0, extraRows = 0, faceVertices = 0;
			std::uint32_t treeCapacity = 0, fadeRootCapacity = 0, fadeEventCapacity = 0;
			std::uint32_t treeLodShapeCapacity = 0, treeLodMeshCapacity = 0;
			bool operator==(const SceneSizing&) const = default;
		};

		struct SceneBuffers : SceneSizing
		{
			Versioned objects, extras, geometries, facePositions;
			// The index pool every plain indexed draw binds (IndexPool): the shadow views' and the Z-prepass's, made by whichever
			// sets up first (EnsureIndexPool) and kept current by every commit that draws from it (UpdateIndexPool).
			std::shared_ptr<IndexPool> pool;
			// Their SRVs' descriptor heap indices, and the positions' address: a growth gives the buffer new ones (the old ones
			// are retired once the GPU is done with them), so every build takes them from here, after ReserveSceneTables.
			std::uint32_t objectsIndex = 0, extrasIndex = 0;
			std::uint64_t facePositionsAddress = 0;
			// What each holds (SceneSizing: GpuLayouts.h, kInitialObjects), grown by ReserveSceneTables; `generation` counts the
			// growths, so a batch staged before one is not submitted after it.
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
			Versioned trees, treeClocks;
			std::shared_ptr<org::Buffer> treeFrameBuffer;  // TreeWindFrameRow, one row, every commit's
			// The trees' wind (TreeWindCS), by entry (kTreeWindEntryRows float4 rows: the nodeless trees', then tree slot s at
			// s + 1), one buffer per frame parity: the Z-prepass epoch's compute writes the frame's, and every draw reads the
			// other's through the frame record (kTreeWindRegister, treeWindIndex). Zeroed once per backing (treeWindZeroed): a
			// zero generation is never a listing's, so a record draws its own values until its tree's entry is written.
			std::array<Versioned, 2> treeWindRows;
			std::array<std::uint32_t, 2> treeWindIndex{};
			bool treeWindZeroed = false;
			/** @brief The wind buffer a frame's draws read: the one the frame before wrote (TreeWindCS writes by the frame's parity). */
			std::uint32_t TreeWindReadIndex(std::uint32_t a_frame) const { return treeWindIndex[(a_frame + 1) & 1]; }
			std::uint64_t treesHeld = ~0ull;
			std::uint32_t treeCount = 0;
			std::uint32_t treeFrame = 0;  // the scene frame of treeInputs
			TreeWindFrame treeInputs{};
			// The frame before's: what wrote the buffer this frame's draws read (TreeWindReadIndex). ~0u until a frame has run.
			std::uint32_t previousTreeFrame = ~0u;
			TreeWindFrame previousTreeInputs{};
			std::uint64_t previousTreesHeld = ~0ull;  // the tree rows that frame's pass read
			std::uint64_t frameTreesHeld = ~0ull;     // this frame's
			std::shared_ptr<const ComputeProgram> treeWind;
			// Fade roots (FadeStateCS; Records.h, FadeRootStatic): the static rows by root slot (the commits' uploads, the runs
			// Tables::fadeRootsJournal names since fadeRootsHeld), the GPU's state rows, the frame's inputs (a one-row buffer the depth commit writes once
			// a frame), and CS_DCLF_FADE_PARITY's log (kFadeLogEntries roots from fadeLogBase, ~0u: none this frame).
			Versioned fadeRoots, fadeStates;
			std::shared_ptr<org::Buffer> fadeFrameBuffer, fadeLog;
			std::shared_ptr<org::Buffer> fadeVisibility;  // the list processes' cull tests (Records.h, kFadeVisibilityLists blocks)
			// What fadeVisibility holds, as the commits sent it (the parts of each block its counts use), and the buffer it is of.
			std::vector<std::byte> fadeVisibilitySent;
			const org::Buffer* fadeVisibilitySentTo = nullptr;
			Versioned fadeRootLists;   // per root slot: its block (PrimaryCull::FadeRootLists)
			Versioned fadeAnimated;    // per root slot: the scene frame whose animation batch updated it
			// What fadeAnimated holds, as the commits wrote it (the GPU only reads it): a frame's words go up in runs of the roots it
			// stamped, runs closer than kFadeRunGap words merged, rather than a copy per root (about a hundred a frame at the bridge)
			// or the span from the lowest to the highest (16 KB a frame in motion).
			std::vector<std::uint32_t> fadeAnimatedMirror;
			// What fadeRootLists holds, as the commits sent it: the lists go up in the runs that differ, not whole.
			std::vector<std::uint32_t> fadeRootListsMirror;
			std::uint64_t fadeRootListsHeld = ~0ull;      // the root and list versions it holds
			// The states FadeStateCS publishes, one buffer per scene frame parity (the state rows are its own): the builds read the
			// frame before's (FadeStatesReadIndex, through their latches). Zeroed once per backing (frameAheadZeroed): a zero
			// generation is never a listing's, and the builds take the static row's state for it.
			std::array<Versioned, 2> fadeStatesOut;
			std::array<std::uint32_t, 2> fadeStatesOutIndex{};
			bool fadeStatesOutZeroed = false;
			/** @brief The published states a frame's builds read: the frame before's (FadeStateCS writes by the frame's parity). */
			std::uint32_t FadeStatesReadIndex(std::uint32_t a_frame) const { return fadeStatesOutIndex[(a_frame + 1) & 1]; }
			std::uint64_t fadeRootsHeld = ~0ull;
			std::uint32_t fadeRootCount = 0;
			std::uint32_t fadeFrameNumber = 0;  // the scene frame of fadeFrame
			FadeFrame fadeFrame{};
			std::uint32_t fadeLogBase = ~0u;
			std::shared_ptr<const ComputeProgram> fadeState;
			// The fade write-back: the event list (a count word, then FadeEvents; the count zeroed by every depth commit), the
			// events it holds, each root's last reported generation and milestone (zeroed once per backing), and the host side.
			Versioned fadeEvents, fadeReported;
			bool fadeReportedZeroed = false;
			std::shared_ptr<FadeWriteBack> fadeWriteBack;
			// Tree LOD (dclf-lod.md, "Tree LOD: the draws"; Scene/TreeLod.h): the shape rows, the instance records (75 a shape
			// slot), the mesh rows, the draw row and the visible list (the draw's arguments, then the culled records), every one read
			// through its address by the draws' vertex stage. The depth commit uploads what the mirror changed (UploadTreeLod);
			// TreeLodCullPass fills the list in the depth epoch, and both main passes draw it once the pipelines exist. treeLodReady: the
			// last depth commit's draw row named its slots (the frame draws tree LOD).
			Versioned treeLodShapes, treeLodInstances, treeLodMeshes, treeLodVisible;
			std::shared_ptr<org::Buffer> treeLodDraw;
			std::uint64_t treeLodInstancesAddress = 0, treeLodShapesAddress = 0, treeLodMeshesAddress = 0, treeLodDrawAddress = 0, treeLodVisibleAddress = 0;
			std::shared_ptr<const ComputeProgram> treeLodCull;
			// The pipelines the passes record with, published by the depth commit once built (read on the graph host's thread).
			std::atomic<std::shared_ptr<const TreeLodPipelines>> treeLodPipelines;
			std::atomic<bool> treeLodReady{ false };
			std::uint64_t treeLodUploads = 0, treeLodSlotsSent = 0;  // since the last report
			/**
			 * @brief Tree LOD's cull counts (TreeLodReadbackPass): the list's header copied to a host buffer per frame slot after the
			 * depth segment's second phase, read when the slot comes round again (the graph host's thread), summed for the report.
			 */
			struct TreeLodCounts
			{
				std::vector<std::shared_ptr<org::Buffer>> readback;
				std::vector<std::uint8_t> filled;
				std::atomic<std::uint64_t> frames{ 0 }, phaseOne{ 0 }, retests{ 0 }, phaseTwo{ 0 };
			};
			std::shared_ptr<TreeLodCounts> treeLodCounts;
			// The draw row the last depth commit uploaded (render thread): the reflection's faces draw from the same tables with
			// their own lists (ExecuteReflection).
			TreeLod::DrawRow treeLodRow{};
		};

		/**
		 * @brief Tree LOD's texture and sampler into a_row (UploadTreeLod): the engine's tree LOD atlas (BSDistantTreeShader::
		 * SetupTechnique's static at 0x1433dcd18) and slot 0's sampler as its draws leave it (address mode 0, filter 2). The bindings'
		 * owners go to the execution. False when there is no texture.
		 */
		inline bool TreeLodTextureBinding(TreeLod::DrawRow& a_row, std::vector<std::shared_ptr<const void>>& a_owners)
		{
			const auto* texture = Engine::Global<RE::NiSourceTexture*>(0x33dcd18);
			auto* view = texture && texture->rendererTexture ? texture->rendererTexture->resourceView : nullptr;
			if (!view)
				return false;
			auto& textures = GpuTextures::Get();
			auto textureBinding = textures.ResolveBinding(view, 0);
			auto samplerBinding = textures.SamplerBinding(0, 2);
			if (textureBinding.index == GpuTextures::kInvalid || samplerBinding.index == GpuTextures::kInvalid)
				return false;
			a_owners.push_back(std::move(textureBinding.owner));
			a_owners.push_back(std::move(samplerBinding.owner));
			a_row.textureIndex = textureBinding.index;
			a_row.samplerIndex = samplerBinding.index;
			return true;
		}

		/**
		 * @brief The depth commit's tree LOD uploads (render thread): the shape slots and meshes the mirror changed since the last
		 * commit, the draw row (the tables' addresses, the frame's texture and sampler) and the list's arguments with no instances.
		 * The leases of meshes no shape draws any more, and the texture's and sampler's bindings, go to the execution (a_owners).
		 */
		template <class Uploads>
		void UploadTreeLod(TreeLod::Mirror& a_mirror, SceneBuffers& a_scene, Uploads& a_uploads, std::vector<std::shared_ptr<const void>>& a_owners, bool a_draw)
		{
			a_scene.treeLodReady.store(false, std::memory_order_release);
			if (!a_scene.treeLodCull || !a_scene.treeLodShapes)
				return;
			for (auto& owner : a_mirror.TakeRetired())
				a_owners.push_back(std::move(owner));
			// Reserved before the epoch (ReserveSceneTables); a mirror past them waits a commit, its changes kept, drawing nothing.
			std::vector<std::uint32_t> slots, meshes;
			const bool fits = a_mirror.ShapeSlots() <= a_scene.treeLodShapeCapacity && a_mirror.MeshSlots() <= a_scene.treeLodMeshCapacity;
			if (!fits)
				a_draw = false;
			else
				a_mirror.TakeChanges(slots, meshes);
			for (const std::uint32_t slot : slots) {
				const auto& row = a_mirror.SlotRow(slot);
				a_uploads(a_scene.treeLodShapes, &row, sizeof(row), std::uint64_t(slot) * sizeof(TreeLod::ShapeRow));
				if (const auto* instances = a_mirror.SlotInstances(slot); instances && !instances->empty())
					a_uploads(a_scene.treeLodInstances, instances->data(), instances->size() * sizeof(TreeLod::Instance),
						std::uint64_t(slot) * TreeLod::kMaxGroupInstances * sizeof(TreeLod::Instance));
			}
			for (const std::uint32_t mesh : meshes) {
				const auto& row = a_mirror.MeshSlotRow(mesh);
				a_uploads(a_scene.treeLodMeshes, &row, sizeof(row), std::uint64_t(mesh) * sizeof(TreeLod::MeshRow));
			}
			a_scene.treeLodSlotsSent += slots.size();
			++a_scene.treeLodUploads;
			// The draw row and the list's header with no instances, every depth commit. The cull appends only while the row names
			// shape slots: 0 when tree LOD does not draw this frame (toggle off, no texture), so both passes draw nothing together.
			TreeLod::DrawRow row{};
			row.instances = a_scene.treeLodInstancesAddress;
			row.shapes = a_scene.treeLodShapesAddress;
			row.meshes = a_scene.treeLodMeshesAddress;
			row.visible = a_scene.treeLodVisibleAddress + TreeLod::kVisibleHeaderWords * sizeof(std::uint32_t);
			row.alphaRef = 128.0f / 255.0f;
			row.maxIndices = std::max(a_mirror.MaxIndices(), 1u);
			// The engine's texture for its tree LOD draws (the worldspace's tree LOD atlas, TreeLodTextureBinding) and the alpha
			// reference they draw with (128/255): measured at the engine's draws (dclf-lod.md, "Tree LOD: what the engine does").
			if (a_draw && a_mirror.MaxIndices() && TreeLodTextureBinding(row, a_owners))
				row.shapeSlots = a_scene.treeLodShapeCapacity;
			a_uploads(a_scene.treeLodDraw, &row, sizeof(row), 0);
			a_scene.treeLodRow = row;
			TreeLod::VisibleHeader header{};
			header.phaseOne[0] = header.phaseTwo[0] = header.colour[0] = row.maxIndices;
			a_uploads(a_scene.treeLodVisible, &header, sizeof(header), 0);
			if (row.shapeSlots)
				a_scene.treeLodReady.store(true, std::memory_order_release);
		}

		/**
		 * @brief Sorted word indices as runs, a_send(first, count), indices closer than kFadeRunGap merged into one run (a few words
		 * sent again rather than a copy each).
		 */
		constexpr std::uint32_t kFadeRunGap = 32;
		template <class Send>
		void SendWordRuns(const std::vector<std::uint32_t>& a_sorted, Send&& a_send)
		{
			for (std::size_t i = 0; i < a_sorted.size();) {
				const std::uint32_t first = a_sorted[i];
				std::uint32_t lastIndex = first;
				while (++i < a_sorted.size() && a_sorted[i] - lastIndex <= kFadeRunGap)
					lastIndex = a_sorted[i];
				a_send(first, lastIndex - first + 1);
			}
		}

		/**
		 * @brief The depth commit's fade uploads (render thread): the static rows the buffer lacks (the journal's runs since the
		 * version it holds; all of them for a new buffer), and the frame's inputs once a frame: of the list processes' cull tests, the
		 * parts of each block its counts use, where they changed. a_logBase: the parity log's first root this frame (~0u: none).
		 */
		template <class Uploads>
		void UploadFadeRoots(const SceneStore::Tables& a_tables, std::uint32_t a_frame, const FadeFrame& a_inputs, std::uint32_t a_logBase, SceneBuffers& a_scene,
			Uploads& a_uploads, const std::vector<std::byte>& a_visibility, std::uint32_t a_visibilityBlocks)
		{
			if (!a_scene.fadeState || !a_scene.fadeRoots)
				return;
			const std::uint64_t version = a_tables.FadeRootsVersion();
			if (a_scene.fadeRootsHeld != version && a_tables.fadeRoots.size() <= a_scene.fadeRootCapacity) {
				const auto* rows = a_tables.fadeRoots.data();
				a_tables.fadeRootsJournal.Take().ForEachRun(a_scene.fadeRootsHeld, a_tables.fadeRoots.size(), [&](std::uint64_t a_first, std::uint64_t a_count) {
					a_uploads(a_scene.fadeRoots, rows + a_first, static_cast<std::size_t>(a_count * sizeof(FadeRootStatic)), a_first * sizeof(FadeRootStatic));
				});
				a_scene.fadeRootsHeld = version;
			}
			a_scene.fadeRootCount = a_scene.fadeRootsHeld == version ? static_cast<std::uint32_t>(a_tables.fadeRoots.size()) : 0u;
			if (a_scene.fadeFrameNumber != a_frame) {
				a_scene.fadeFrameNumber = a_frame;
				a_scene.fadeFrame = a_inputs;
				if (a_scene.fadeVisibility && a_visibilityBlocks && std::size_t(a_visibilityBlocks) * kFadeVisibilityBytes <= a_visibility.size()) {
					// A block's header, the operators and plane sets its counts use, and the view planes: FadeStateCS reads no others
					// (the counts bound the program it runs). Each part where it differs from what the buffer holds.
					auto& sent = a_scene.fadeVisibilitySent;
					if (a_scene.fadeVisibilitySentTo != a_scene.fadeVisibility.get() || sent.size() != a_visibility.size()) {
						a_scene.fadeVisibilitySentTo = a_scene.fadeVisibility.get();
						sent.assign(a_visibility.size(), std::byte{ 0xFF });
					}
					auto part = [&](std::size_t a_offset, std::size_t a_bytes) {
						if (!a_bytes || std::memcmp(sent.data() + a_offset, a_visibility.data() + a_offset, a_bytes) == 0)
							return;
						std::memcpy(sent.data() + a_offset, a_visibility.data() + a_offset, a_bytes);
						a_uploads(a_scene.fadeVisibility, a_visibility.data() + a_offset, a_bytes, a_offset);
					};
					for (std::uint32_t b = 0; b < a_visibilityBlocks; ++b) {
						const std::size_t block = std::size_t(b) * kFadeVisibilityBytes;
						std::uint32_t header[4];
						std::memcpy(header, a_visibility.data() + block, sizeof(header));
						const std::size_t ops = std::min<std::uint32_t>(header[1], kFadeVisibilityOps);
						const std::size_t sets = std::min<std::uint32_t>(header[2], kFadeVisibilitySets);
						part(block, kFadeVisibilityOpsOffset + ops * 16);
						part(block + kFadeVisibilitySetsOffset, sets * kFadeVisibilitySetBytes);
						part(block + kFadeVisibilityViewOffset, kFadeVisibilitySetBytes);
					}
				}
				a_scene.fadeLogBase = a_logBase;
			}
			// The frame row, with the pass's per-frame values, every commit.
			a_scene.fadeFrame.rootCount = a_scene.fadeRootCount;
			a_scene.fadeFrame.sceneFrame = a_scene.fadeFrameNumber;
			a_scene.fadeFrame.logBase = a_scene.fadeLogBase;
			a_uploads(a_scene.fadeFrameBuffer, &a_scene.fadeFrame, sizeof(FadeFrame), 0);
			// The write-back's count, for this execution's FadeStateCS to append from (its readback copies what it appended).
			if (a_scene.fadeEvents) {
				static constexpr std::uint32_t kZeroCount[kFadeEventHeaderWords]{};
				a_uploads(a_scene.fadeEvents, kZeroCount, sizeof(kZeroCount), 0);
			}
		}

		/**
		 * @brief A commit's tree uploads (render thread): the rows and the list where the buffers do not hold the tables'
		 * versions, whole (they change when a tree member joins or leaves), and the frame's inputs once a frame. a_frameRow: the
		 * frame row too, for the epoch whose TreeWindPass reads it (the Z-prepass's: no other pass reads it).
		 */
		/**
		 * @brief A commit's zeroing of the frame-ahead outputs (the trees' wind, the published fade states) in a new backing, before
		 * any epoch's draws or builds read them: whichever epoch commits first after a growth.
		 */
		template <class Uploads>
		void ZeroFrameAheadOutputs(SceneBuffers& a_scene, Uploads& a_uploads)
		{
			if (!a_scene.treeWindZeroed && a_scene.treeWindRows[0]) {
				const std::vector<std::byte> zeros(std::size_t(a_scene.treeCapacity + 1) * kTreeWindEntryRows * 16);
				for (const auto& rows : a_scene.treeWindRows)
					a_uploads(rows, zeros.data(), zeros.size(), 0);
				a_scene.treeWindZeroed = true;
			}
			if (!a_scene.fadeStatesOutZeroed && a_scene.fadeStatesOut[0]) {
				const std::vector<std::byte> zeros(std::size_t(a_scene.fadeRootCapacity) * sizeof(FadeNodeState));
				for (const auto& states : a_scene.fadeStatesOut)
					a_uploads(states, zeros.data(), zeros.size(), 0);
				a_scene.fadeStatesOutZeroed = true;
			}
			// No root reported yet: a zero generation is never a listing's.
			if (!a_scene.fadeReportedZeroed && a_scene.fadeReported) {
				const std::vector<std::byte> zeros(std::size_t(a_scene.fadeRootCapacity) * 2 * sizeof(std::uint32_t));
				a_uploads(a_scene.fadeReported, zeros.data(), zeros.size(), 0);
				a_scene.fadeReportedZeroed = true;
			}
		}

		template <class Uploads>
		void UploadTrees(const SceneStore::Tables& a_tables, std::uint32_t a_frame, SceneBuffers& a_scene, Uploads& a_uploads, bool a_frameRow)
		{
			if (!a_scene.treeWind || !a_scene.trees)
				return;
			if (a_scene.treesHeld != a_tables.treesVersion && a_tables.trees.size() <= a_scene.treeCapacity) {
				if (!a_tables.trees.empty())
					a_uploads(a_scene.trees, a_tables.trees.data(), a_tables.trees.size() * sizeof(TreeStatic), 0);
				a_scene.treesHeld = a_tables.treesVersion;
			}
			a_scene.treeCount = a_scene.treesHeld == a_tables.treesVersion ? static_cast<std::uint32_t>(a_tables.trees.size()) : 0u;
			a_scene.frameTreesHeld = a_scene.treesHeld;
			if (a_scene.treeFrame != a_frame) {
				a_scene.previousTreeFrame = a_scene.treeFrame;
				a_scene.previousTreeInputs = a_scene.treeInputs;
				a_scene.previousTreesHeld = a_scene.frameTreesHeld;
				a_scene.treeFrame = a_frame;
				a_scene.treeInputs = SampleTreeWindFrame();
				// CS_DCLF_FOLIAGE_PARITY compares each frame's pixels with the frame before's: the trees' clocks stand still.
				if (FoliageParityOn() && SwitchValue(Switch::FoliageParity) != "wind")
					a_scene.treeInputs.deltaTime = 0.0f;
			}
			// The frame row (TreeWindFrameRow), every Z-prepass commit: the pass's invocation is prepared ahead of it.
			if (a_frameRow && a_scene.treeFrameBuffer) {
				const TreeWindFrameRow row{ a_scene.treeCount, 0, a_scene.treeFrame, 0, a_scene.treeInputs, {} };
				a_uploads(a_scene.treeFrameBuffer, &row, sizeof(row), 0);
			}
		}

		/**
		 * @brief What the main epochs' writers and shapes index the sequences and the Z-prepass's bucket counts by (Impl::
		 * ReserveMainSequences): changed with those buffers' growth, as one (Growths::Change; a revision's shapes take the change it
		 * names, RevisionSizing).
		 */
		struct MainSizing
		{
			// The Z-prepass's plain draws (MainOpaquePass, DCLF_PULLED): per pipeline slot, its bucket's range of each phase's sequences
			// - every draw the slot's objects can produce, held while they fit and grown to a power of two past it, so the recorded
			// calls change only then (zBucketsLayout) - and each phase's count words, a word a slot, zeroed by the depth commit.
			std::vector<std::uint32_t> zBucketCapacity, zBucketFirst;
			std::uint64_t zBucketsLayout = 0;
			std::uint32_t zBucketCountWords = 0;
			// The sequence buffer's ranges (GpuLayouts.h, SequenceSlots): draws per draw range (phase 1 and colour, phase 2) and per
			// decal group, holding every draw the scene can produce.
			std::uint32_t sequenceDraws = 0, sequenceDecals = 0;
			// The objects the per-object buffers hold (inputs, inputsDepth, visibility, frustum): the scene's object capacity, grown
			// with it (ReserveObjectBuffers).
			std::uint32_t objectCapacity = 0;
			bool operator==(const MainSizing&) const = default;
		};

		struct Resources : MainSizing
		{
			std::vector<FrameBuffer> frameBuffers;
			// The main pass's rows (DrawPipelines.h, kMaterialRowBytes / kPipelineRowBytes), one table each for both segments,
			// indexed by the scene's material and pipeline slots; grown before an epoch to the tables' slot counts
			// (Impl::ReserveMainSequences). The versions of the kept rows (MainRows) they hold: 0 in a new backing.
			GrowableRows materialRows, pipelineRows;
			std::uint64_t materialRowsHeld = 0, pipelineRowsHeld = 0;
			// The scene's tables, every epoch's (SceneBuffers): the object records, bone rows, geometry table and face positions.
			std::shared_ptr<SceneBuffers> scene;
			Versioned inputs, sequences;  // BuildDraws: in, out (and the scene's geometries)
			std::shared_ptr<org::Buffer> count;
			// The Z-prepass's bucket counts (MainSizing) and the index pool (the scene's).
			std::array<Versioned, 2> zBucketCounts;
			std::shared_ptr<IndexPool> pool;
			// The Z-prepass segment's draw inputs: each main segment keeps its resident region at the head of its own buffer
			// (the colour segment's is `inputs`), and the version of the region the buffer holds, per segment (0 Z-prepass,
			// 1 colour), written by the commit that uploads it.
			Versioned inputsDepth;
			std::array<std::uint64_t, 2> residentUploaded{};
			std::shared_ptr<const ComputeProgram> buildDraws;
			winrt::com_ptr<ID3D11Buffer> sequencesD3D11, countD3D11;  // CS_DCLF_BUILD_PARITY readback
			// CS_DCLF_FOLIAGE_PARITY (GpuLayouts.h, FoliageParityConstants): by frame parity, each pixel's object and colours; the
			// compare pass's results, and a host copy of them per frame slot, read when the slot comes round again.
			struct FoliageParity
			{
				std::array<std::shared_ptr<org::Buffer>, 2> ids, colours;
				std::array<std::uint64_t, 2> idsAddress{}, coloursAddress{};
				std::shared_ptr<org::Buffer> results;
				std::uint64_t resultsAddress = 0;
				// The Z-prepass's alpha-tested fragments' owner per pixel (GpuLayouts.h, kFoliageOwned), its UAV's index.
				std::shared_ptr<org::Buffer> owners;
				std::uint32_t ownersIndex = 0;
				std::uint32_t epoch = 0;  // colour commits so far: the buffers' parity and tag (the store's frame can repeat or skip)
				std::shared_ptr<const ComputeProgram> program;
				std::vector<std::shared_ptr<org::Buffer>> readback;
				std::vector<std::uint32_t> readbackFrame;  // 1 where the slot's copy holds a frame's results
				std::uint32_t width = 0, height = 0;
				// The report: frames read, frames with a defect, the counters' sums, the worst frame.
				std::uint64_t frames = 0, flagged = 0, inFrameFlagged = 0, whiteFlagged = 0;
				double whiteAverage = -1.0;     // the near-white foliage pixels' running average (kFoliageWhiteAlbedo + kFoliageWhiteDiffuse)
				std::uint32_t whiteLogged = 0, whiteMax = 0;
				double changeAverage = 0.0;     // the reprojected changes' running average (recoloured, brightened, whitened)
				std::uint32_t changeMax = 0;
				std::array<std::uint64_t, kFoliageCounters> totals{};
				std::uint32_t logged = 0, inFrameLogged = 0;
				std::mutex mutex;
			};
			std::shared_ptr<FoliageParity> foliage;
			winrt::com_ptr<ID3D11Buffer> visibilityD3D11;              // CS_DCLF_SET_PARITY readback
			winrt::com_ptr<ID3D11Buffer> frustumD3D11;                 // CS_DCLF_SET_PARITY readback (the fade test's drops)
			// The per-frame constant blocks at fixed slots (FrameSlotOffset), so a build can name them before
			// their contents exist.
			std::shared_ptr<org::Buffer> frameConstants;
			// The commits' latched copies (MainLatchedCopiesPass): per segment (kDepthShape, kColourShape) a latch block of its own -
			// both commits write in the same frame slot, and the first one's copies may not have run when the second writes - and the
			// targets the passes declared, which a commit's LatchedUploads take; anything else is staged.
			std::array<std::shared_ptr<org::LatchBlock>, 2> latchedBlocks;
			std::atomic<std::shared_ptr<const std::vector<const void*>>> latchedTargets;  // LatchedTarget::key
			std::uint64_t frameConstantsAddress = 0;
			// The zeroed StrictLightData block every bindless draw's b3 reads (kFrameSlotSharedLight): constant, so sent once, by the
			// first commit, as a staged upload outside the latched layout.
			bool sharedLightZeroed = false;
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
			Versioned visibility;  // per-object verdict, published by the depth segment
			// Per object, the stamp of the last frame the main camera's depth phase 1 found its bound inside the frustum
			// (occlusion aside: the engine's OnVisible semantics). Never cleared: a stale stamp is simply not this frame's.
			Versioned frustum;
			std::shared_ptr<PassStats> passStats;  // CS_DCLF_PASS_STATS
			// The explicit DGC preprocesses' state list of the colour segment's passes. The depth segment's draws are plain
			// (PassFrame::zCalls).
			std::shared_ptr<PreprocessStates> preprocessMain;
			std::shared_ptr<org::PixelBuffer> hzb;
			std::shared_ptr<const ComputeProgram> hzbProgram;
			// The single-pass downsample's count of finished groups: zeroed by the first depth commit (hzbCounterZeroed,
			// render thread), and after that by the last group of each dispatch.
			std::shared_ptr<org::Buffer> hzbCounter;
			bool hzbCounterZeroed = false;
			std::uint32_t hzbWidth = 0, hzbHeight = 0, hzbMips = 0;
			std::uint32_t width = 0, height = 0;
			bool lightLimitFix = false;  // LLF's graph buffers are registered (they are read at t35-t37)
			// Per frame slot, one BuildDrawsLatch (all BuildDraws dispatches of an epoch share the values), then the colour pass's
			// cascades (latchLayout, grown by ReserveMainLatch).
			std::shared_ptr<org::LatchBlock> latch;
			MainLatchLayout latchLayout;
			rhi::CommandSignaturePtr dispatchSignature;
			// The sort by pipeline of phase 1's and the colour segment's sequences (one view); null when it is off.
			std::shared_ptr<DrawSort> sort;
			// Per drawing segment, its last commit (render thread): the scene frame, its inputs, and the backings it wrote into (the
			// scene tables' growths, the per-object buffers' capacity). The reflection's faces draw from the depth segment's inputs
			// and the colour segment's frame record as the frame before left them, and only while nothing has grown since.
			struct Committed
			{
				std::uint32_t frame = ~0u, inputs = 0;
				std::uint64_t sceneGeneration = 0;
				std::uint32_t objectCapacity = 0;
				// The main rows' backings its inputs name by address (MainRowsGeneration): a growth adopted since leaves them on a
				// version nothing holds.
				std::uint64_t rowsGeneration = 0;
			};
			/** @brief The main rows' backings (material and pipeline rows' generations): what draw inputs embed the addresses of. */
			std::uint64_t MainRowsGeneration() const { return (materialRows.generation << 32) ^ pipelineRows.generation; }
			std::array<Committed, 2> committed;
		};

		struct PassBindings
		{
			std::array<org::DeclaredViewToken, kColorTargets> targets{};
			org::DeclaredViewToken depth;
			org::ResourceBindingToken sequences, count, materialRows, pipelineRows, objects, extras;
			org::ResourceBindingToken lights, lightIndexList, lightGrid;
			std::vector<org::ResourceBindingToken> frameBuffers;
			org::ResourceBindingToken pool, bucketCounts;  // the depth segment's plain draws
			org::ResourceBindingToken treeLodVisible;      // tree LOD's draw's arguments (UploadTreeLod, TreeLodCullPass)
		};

		/*
		 * The main epochs' shape producers (Shapes.cpp; dclf-async-publication.md, R3c). A segment's shape (PassFrame) is a function
		 * of what a scene revision names - its resource versions, the scene's draw bound, the pipeline set and the lookups - and of
		 * what changes only with the engine's setup (the main pass's viewport, the frame blocks' sizes); never of a frame's values,
		 * which the commit writes into the latch and the latched copies' layout the shape names. A revision's shape is made from
		 * these at the scene work's join (MakeRevisionShapes); a commit makes its own from the same producer (ShapeParity).
		 */

		/**
		 * @brief The Z-prepass's plain draws (MainOpaquePass): a bucket per group of pipeline slots that share a depth pipeline
		 * (IndirectState::zGroups), in the order the slots first name them, its range the slots' ranges together
		 * (Resources::zBucketCapacity, which the sequences' phase ranges hold); each slot's bucket (kNoBucket: a slot without a
		 * published pipeline, whose draws BuildDraws drops); each phase's table of (first, capacity) per bucket; a draw call per
		 * bucket. The calls are the shape's; the map and the tables the commit's latch.
		 */
		struct ZBucketPlan
		{
			std::vector<std::uint32_t> map;    // per pipeline slot
			std::vector<std::uint32_t> table;  // phase 1's (first, capacity) per bucket, then phase 2's
			std::vector<PassFrame::ZCall> calls;
			std::uint32_t Buckets() const { return static_cast<std::uint32_t>(calls.size()); }
		};
		void PlanZBuckets(const MainSizing& a_resources, const Lookups& a_lookups, const SceneStore::Tables& a_tables, const IndirectState& a_indirect, ZBucketPlan& a_out);

		/** @brief The sizes of a main epoch's frame blocks (FrameBlocks), per stage and register: what its latched copies of them take. */
		struct FrameBlockSizes
		{
			std::array<std::uint32_t, kConstantBufferRegisters> vs{}, ps{};
			bool operator==(const FrameBlockSizes&) const = default;
		};

		/**
		 * @brief A main commit's latched copies (LatchedUploads), in the order it writes them and packed as LatchedUploads packs
		 * them: a function of its resources, its segment, its frame blocks' sizes and the Z-prepass's bucket count.
		 */
		std::vector<LatchedCopy> MainLatchedLayout(const Resources& a_resources, bool a_depthOnly, const FrameBlockSizes& a_blocks, std::uint32_t a_buckets);
		/** @brief The bytes a layout takes of its latch block's slot. */
		std::size_t LatchedBytes(const std::vector<LatchedCopy>& a_copies);
		/** @brief A latched copies' block holding at least a_bytes a slot: a new, larger one when it does not (LatchedUploads' rule). */
		void ReserveLatchedBlock(std::shared_ptr<org::LatchBlock>& a_block, std::size_t a_bytes, std::uint32_t a_slots);

		/** @brief The main pass's render area and depth range, as a segment's last capture had them. */
		struct MainViewport
		{
			std::uint32_t width = 0, height = 0;
			float minDepth = 0.0f, maxDepth = 1.0f;
			bool operator==(const MainViewport&) const = default;
		};

		/** @brief What a main segment's shape is made from (MakeMainShape). */
		struct MainShapeInputs
		{
			bool depthOnly = false;
			std::uint32_t sequenceDraws = 0, sequenceDecals = 0;  // the sequence buffer's ranges (ReserveMainSequences)
			std::uint64_t materialRows = 0, pipelineRows = 0;     // the rows' tables' addresses
			MainViewport viewport;
			rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
			IndirectState indirect{};
			std::uint32_t cullMode = 0;
			std::shared_ptr<const org::LatchBlock> latch;  // the main latch (ReserveMainLatch)
			MainLatchLayout latchLayout;                   // its layout
			std::vector<PassFrame::ZCall> zCalls;          // the depth segment's (PlanZBuckets)
			std::shared_ptr<const ZBucketPlan> zPlan;      // the plan they are the calls of
			LatchedList latched;                           // MainLatchedLayout, in its reserved block
		};
		/**
		 * @brief The segment's resources' part of its shape's inputs: the ranges, the rows, the latch, the cull mode. Not the
		 * descriptor heaps: the device's, read only inside an epoch (org::runtime::GetActiveSRVDescriptorHeap).
		 */
		MainShapeInputs MainShapeInputsOf(const Resources& a_resources, bool a_depthOnly, bool a_revision = false);
		std::shared_ptr<PassFrame> MakeMainShape(const MainShapeInputs& a_in);

		/** @brief The parts of a main shape the shape parity tells apart (MainShapeDifferences). */
		enum MainShapeField : std::uint32_t
		{
			kShapeCapacity,
			kShapeSequences,
			kShapeRows,
			kShapeViewport,
			kShapeHeaps,
			kShapePipelines,
			kShapeCull,
			kShapeLatch,
			kShapeZCalls,
			kShapeLatchedCopies,
			kShapeLatchedBlock,
			kMainShapeFields
		};
		inline constexpr std::array<const char*, kMainShapeFields> kMainShapeFieldNames = { "capacity", "sequences", "rows", "viewport", "heaps", "pipelines", "cull",
			"latch", "z calls", "latched copies", "latched block" };
		/** @brief The fields (MainShapeField bits) where two main shapes differ: 0 for shapes SameShape holds the same. */
		std::uint32_t MainShapeDifferences(const PassFrame& a_a, const PassFrame& a_b);

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
			// Tree LOD's draw (UploadTreeLod): its pipelines and the draw row's address, which its push data names.
			std::shared_ptr<const TreeLodPipelines> treeLod;
			std::uint64_t treeLodDraw = 0;
		};

		struct ShadowFrame;
		struct ReflectionFrame;
		/**
		 * @brief The shapes a scene revision's recording of an epoch is prepared for (R3c; SceneRevision.cpp): what its passes read
		 * from the preparation's host data (PassPrepareContext::preparationData). A
		 * preparation without one draws nothing: every DCLF epoch is submitted from a revision.
		 */
		struct RevisionShapes
		{
			std::array<std::shared_ptr<const PassFrame>, 2> main;  // kDepthShape, kColourShape
			std::shared_ptr<const ShadowFrame> shadow, occlusion;
			std::shared_ptr<const ReflectionFrame> reflection;
			// The scene's sizing the revision names (Growths::RevisionSizing): what its passes size their dispatches and lists by, for
			// the versions it binds - never the live one, which moves only when the revision's growths are adopted.
			std::shared_ptr<const SceneSizing> scene;
		};
		inline const RevisionShapes* RevisionShapesOf(const org::PassPrepareContext& a_preparation)
		{
			return a_preparation.preparationData ? a_preparation.preparationData->Get<RevisionShapes>() : nullptr;
		}
		/** @brief The scene sizing a_preparation's buffers have: its revision's, else the current one (a commit's own preparation). */
		inline const SceneSizing& SceneSizingOf(const org::PassPrepareContext& a_preparation, const SceneBuffers& a_scene)
		{
			if (const auto* shapes = RevisionShapesOf(a_preparation); shapes && shapes->scene)
				return *shapes->scene;
			return a_scene;
		}
		/** @brief The shape a_preparation prepares the segment's passes for: its revision's, null without one or for a segment that does not draw. */
		inline std::shared_ptr<const PassFrame> CurrentFrame(const org::PassPrepareContext& a_preparation, [[maybe_unused]] const Resources& a_resources, RenderGraphRuntime::Segment a_segment)
		{
			if (const auto* shapes = RevisionShapesOf(a_preparation)) {
				if (a_segment == RenderGraphRuntime::Segment::ZPrepass)
					return shapes->main[kDepthShape];
				if (a_segment == RenderGraphRuntime::Segment::MainOpaque)
					return shapes->main[kColourShape];
			}
			return nullptr;
		}

		static_assert(DrawPipelines::kMaxPipelines == 4096, "BuildDrawsCS.hlsl's kSortKeys");

		/** @brief Whether BuildDraws runs in the segment (BuildDrawsPass::Prepare's conditions), which is when a sort follows it. */
		inline bool BuildsDraws(const org::PassPrepareContext& a_preparation, const Resources& a_resources, RenderGraphRuntime::Segment a_segment)
		{
			return a_resources.buildDraws && a_resources.latch && a_resources.dispatchSignature && CurrentFrame(a_preparation, a_resources, a_segment);
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

		/** @brief HzbCS.hlsl's constants: one single-pass downsample of the chain. */
		struct HzbConstants
		{
			std::uint32_t sourceIndex;
			std::uint32_t fromDepth;
			std::uint32_t validSize[2];   // the source texels holding depth
			std::uint32_t domainSize[2];  // the source's power-of-two extent; past validSize it reads the far plane
			std::uint32_t targetSize[2];  // the first written level's
			std::uint32_t mips;
			std::uint32_t workGroups;
			std::uint32_t counterIndex;
			std::uint32_t padding;
			std::uint32_t targetIndices[kHzbDispatchMips];
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
		 * with BuildDrawsCS's two drawn bits (depth, colour), against the frame's DCLF set (kObjectMember) and what the two
		 * builds decided on the CPU. See CheckSetParity.
		 */
		bool SetParityEnabled();

		/**
		 * CS_DCLF_REVISION_PARITY=1: on ParityDue frames each epoch's commit also makes its own shape and checks the selected revision's
		 * against it (versions, pipelines, viewport, shape, capacity, latch, admission: <- REVISION), and the installed payload against
		 * the frame's inputs (<- STALE). It observes only: the frame draws the revision's shape and the installed payload whatever it
		 * finds (the normal path trusts both).
		 */
		bool RevisionParityEnabled();

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
			std::uint32_t slot = 0;  // its placement's (Impl::shadowPlacements)
			float minDepth = 0.0f, maxDepth = 1.0f;
			RE::NiPoint3 eye;
			bool hasViewProj = false;
			std::array<float, 16> viewProj{};
			// The engine's caster volume for the view (NiCullingProcess::customCullPlanes), when its culling
			// process has one; see BuildDrawsLatch::cullPlanes.
			float cullPlanes[6][4] = {};
			std::uint32_t cullPlaneMask = 0;
			// DrawPipelines::ShadowRasterStateId of the state the engine draws the view's casters with: its table entry at cull mode 1,
			// the one the Utility shader sets for every pass but a two-sided property's (engine notes, shadow maps), whose key draws
			// without culling (kRasterTwoSided) whatever the view's state.
			std::uint32_t rasterState = 0;
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
		/**
		 * @brief A shadow view's bucket: one plain indirect draw with one pipeline over a range of the view slot's sequences
		 * (ShadowViewPass). A view has a bucket per pipeline its rasterizer state's map row names, in the row's order, and the
		 * bucket's index is what the row holds for its key slots (BuildDrawsCS).
		 */
		struct ShadowBucket
		{
			std::uint32_t pipeline = 0;  // its shadow set index (ShadowIndirectState::pipelines)
			std::uint32_t first = 0;     // its range, in sequences
			std::uint32_t capacity = 0;  // its draw's max count: every draw its key slots' inputs can produce, grown by doubling

			bool operator==(const ShadowBucket&) const = default;
		};

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
			// Its draws, one per bucket with a capacity, the depth-only class's pipelines first (ShadowEpochs.cpp, ShadowBuckets).
			std::vector<ShadowBucket> buckets;
			std::uint32_t rasterState = 0;  // the state whose pipeline map row the buckets are (ShadowViewLayout)

			bool operator==(const ShadowFrameView&) const = default;
		};

		/**
		 * @brief The shape of the frame's shadow epoch: which views, where they draw and with what capacity. Every view
		 * the hooks captured is drawn by one graph execution at AfterShadowMaps, rather than one per view, because the
		 * graph's own execution costs about 0.9 ms of CPU per epoch whatever it draws, and the exterior has four views a
		 * frame. Each view's matrices and input count are in its BuildDrawsLatch; the shape changes only when the engine's
		 * view layout or the pipelines do, and its identity is the shadow passes' revision.
		 */
		struct RowBuckets;
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
			// The latched copies' sources (ShadowLatchedCopiesPass): the views' blocks at viewBlocksOffset of the latch's slot region
			// (ShadowLatchLayout::ViewBlockOffset(0)), and the counters' zeros.
			std::uint32_t viewBlocksOffset = 0;
			std::shared_ptr<const org::LatchBlock> zeros;
			// The commit's other per-frame values (CS's SharedData and FeatureData blocks in the constants), latched (LatchedUploads).
			LatchedList latched;
			// Not compared (implied by the latch block and the views' buckets): what a commit writing its values into this shape writes
			// them against (R3c) - the latch's layout, and each view's map row (its key slots' buckets).
			ShadowLatchLayout latchLayout;
			std::shared_ptr<const std::vector<RowBuckets>> rows;

			bool SameShape(const ShadowFrame& o) const
			{
				// The version holds the pipelines the views' buckets bind (ShadowIndirectState::pipelines).
				return latch == o.latch && viewBlocksOffset == o.viewBlocksOffset && zeros == o.zeros && latched == o.latched && SameHandle(resourceHeap, o.resourceHeap) && SameHandle(samplerHeap, o.samplerHeap) && indirect.valid == o.indirect.valid &&
				       SameHandle(indirect.layout, o.indirect.layout) && indirect.version == o.indirect.version && views == o.views;
			}
		};

		/** @brief The shadow views' graph resources: the main path's set, without targets or an HZB, per view slot. */
		/**
		 * @brief The shadow views' index pool: the index buffers of the geometry slots, copied into one DCLF buffer, so that the
		 * views' and the Z-prepass's plain indexed draws bind one index buffer (ShadowViewPass, MainOpaquePass) and keep the
		 * hardware's vertex reuse. A range is a slot's own, never shared: a buffer's address is no identity (the device reuses a
		 * freed buffer's address at once). The first commit of a frame to draw from the pool gives a range out, and copies into
		 * it, for every slot the geometry log names, after taking back every named slot's old range (UpdateIndexPool); the
		 * copies run in that commit's epoch before its draws (IndexPoolCS). A pool that cannot fit a range doubles, and its
		 * ranges are laid out and copied again. Render thread.
		 */
		/**
		 * @brief What the index pool's buffers hold (Growths::Change at the join, Impl::ReserveIndexPool; adopted with the revision
		 * that names it): its indices, the geometry slots its first-index table holds, and the copies a commit may make. relayouts
		 * moves for a new indices version at the same size (a fragmented pool laid out again).
		 */
		struct PoolSizing
		{
			std::uint32_t capacity = 0;        // in indices
			std::uint32_t firstsCapacity = 0;  // geometry slots
			std::uint32_t copiesCapacity = 0;
			std::uint32_t relayouts = 0;
			bool operator==(const PoolSizing&) const = default;
		};

		struct IndexPool : PoolSizing
		{
			static constexpr std::uint32_t kNoRange = ~0u;
			// A geometry slot's range: its first index and count (even: 4-byte aligned), and the buffer it was copied from.
			struct Range
			{
				std::uint32_t first = kNoRange, count = 0;
				std::uint64_t address = 0, bytes = 0;
			};
			Versioned indices;  // 16-bit
			Versioned firsts;   // per geometry slot: its range's first index, kNoRange without one
			Versioned copies;   // this commit's copies: source address (low, high), first index, words
			// A new indices version adopted: every range laid out again from its start and copied (UpdateIndexPool, next).
			bool relayout = false;
			// Revision mode: the slots whose range waits for the pool's growth (no room, past the first-index table, or past this
			// commit's copies) - the unclaimed geometry the join asked room for, tried again by every update.
			std::vector<std::uint32_t> waiting;
			bool fragmented = false;  // a range found no room though the pool has twice what it holds: a relayout is asked for
			// The join's bound (ReserveIndexPool): every geometry slot's indices as the tables have them, kept from the log.
			struct Bound
			{
				LogCursor cursor;
				std::vector<std::uint32_t> indices;
				std::uint64_t total = 0;
			} bound;
			std::shared_ptr<const ComputeProgram> program;
			rhi::CommandSignaturePtr dispatchSignature;
			std::uint64_t layout = 0;  // bumped whenever a buffer gets a new backing
			LogCursor cursor;
			std::vector<Range> slots;                                   // per geometry slot: its range
			std::vector<std::uint32_t> slotFirst;                       // per geometry slot: its range's first index (what firsts holds)
			std::map<std::uint32_t, std::uint32_t> free;                // first -> count
			std::uint32_t end = 0;                                      // past the last range ever given out
			std::uint64_t indicesHeld = 0;                              // live indices, for the report
			std::uint32_t checks = 0;                                   // the updates, for the parity check
		};

		/** @brief What the shadow epochs' writers index by, changed with those buffers' growth (Growths::Change). */
		struct ShadowSizing
		{
			std::uint32_t objectCapacity = 0;  // what visibility and the inputs hold, as Resources::objectCapacity
			// Every view slot's sequences, in draws (of each caster class): every draw the scene can produce, grown with its bound
			// (Impl::ReserveShadowSequences). One size for all slots: a slot added while a growth is outstanding is made at the size
			// asked for (LatestSizing), so it holds what the sizing says once that is adopted, and more before.
			std::uint32_t sequenceDraws = 0;
			// Every view slot's bucket counts, a word a bucket (ShadowBucket): the latch's key slots, which bound a view's buckets,
			// grown with them. A shape's buckets stay within them (TrimmedRow): a pipeline past them waits for the growth.
			std::uint32_t bucketCountWords = 0;
			// The view slots whose view blocks the view-blocks buffer holds: what the epochs draw into (a frame's views past them stay
			// the engine's: IndirectDraws::ShadowViewCapacity). A slot's own buffers are made with the latch's slots, ahead of it.
			std::uint32_t viewSlots = 0;
			bool operator==(const ShadowSizing&) const = default;
		};

		struct ShadowResources : ShadowSizing
		{
			std::shared_ptr<org::Buffer> constants;
			Versioned visibility;
			// The material rows every view's draws name (ShadowMaterialRow), grown with the kept state.
			GrowableRows materialRows;
			// The scene's tables, every epoch's (SceneBuffers).
			std::shared_ptr<SceneBuffers> scene;
			std::array<Versioned, kShadowModeCount> inputs;               // per render mode
			// Per view slot (the occlusion views', then the shadow views'), as many as the latch layout's viewSlots (Impl::ReserveShadowLatch):
			// its sequence and count buffers, its sequence buffer's draws - grown before the epoch that uses the slot to hold
			// every draw the scene can produce (Impl::ReserveShadowSequences) - and its counters' readback view. Render thread.
			std::vector<Versioned> sequences;
			std::vector<std::shared_ptr<org::Buffer>> count;
			// Per view slot, its buckets' counts (ShadowBucket), a word a bucket (ShadowSizing::bucketCountWords).
			std::vector<Versioned> bucketCounts;
			std::shared_ptr<IndexPool> pool;
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
			// Per frame slot, one BuildDrawsLatch per view slot and the pipeline map rows (latchLayout); a new block when either
			// grows. The passes read it from the revision's frame (ShadowFrame::latch).
			std::shared_ptr<org::LatchBlock> latch;
			// Zeros, never written: the source the epochs' latched copies zero the views' counters from (a word per count and
			// per bucket, as many as bucketCountWords; a new one when that grows).
			std::shared_ptr<org::LatchBlock> zeros;
			// The shadow commit's latched values (LatchedUploads): their block, and the targets the pass declared (the constants).
			std::shared_ptr<org::LatchBlock> latchedBlock;
			std::atomic<std::shared_ptr<const std::vector<const void*>>> latchedTargets;  // LatchedTarget::key
			rhi::CommandSignaturePtr dispatchSignature;
			// This frame's views as the engine named them (render thread), for the culling readback's report.
			struct ViewLabel
			{
				std::uint32_t viewId = 0, renderMode = 0, slot = 0;
			};
			std::vector<ViewLabel> labels;
		};

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
		// (SceneStore::FrameCapture::lighting). A slot is D3D11's constant buffer maximum, so no block overflows.
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
		// The lighting slot's rows (PS b13): the frame lighting (c0-c5), the LOD fades' inputs (c6-c13), the foliage parity's (c14-c15),
		// the extras' pixel inputs (c16, ExtrasFrame::projectedGlobals).
		constexpr std::uint64_t kExtrasPixelFrameOffset = std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes + sizeof(FrameLighting) + sizeof(LodFadeFrame) + 32;
		static_assert(sizeof(FrameLighting) + sizeof(LodFadeFrame) + 32 == 16 * 16);
		static_assert(sizeof(FrameFog) == 3 * 16);

		/**
		 * @brief Where a write at a_offset into the frame-constants buffer goes, named for FrameData: a stage's register slot, the
		 * shared light block, the lighting slot's parts (the frame lighting, the LOD fades' inputs, the foliage parity's) or the record.
		 */
		std::string FrameConstantsPart(std::uint64_t a_offset);
		/** @brief FrameData's name of a write to a_target at a_offset, as a_route ("latched", "staged"). */
		template <class Target>
		std::string FrameDataWhere(std::string_view a_route, const Target& a_target, std::uint64_t a_offset)
		{
			std::string where(a_route);
			where += ": ";
			if constexpr (requires { a_target->GetName(); }) {
				const auto& name = a_target ? a_target->GetName() : std::string();
				if (name == "cs.dclf.frame-constants")
					return where + FrameConstantsPart(a_offset);
				where += name.empty() ? std::string("unnamed") : name;
			} else {
				where += "other";
			}
			return where;
		}

		/** @brief The frame push word of a stage's constant buffer register (FramePushWords' order), or ~0u when it is not pushed. */
		inline std::uint32_t FramePushWord(bool a_pixel, std::uint32_t a_register)
		{
			std::uint32_t word = kFramePushRegisters;
			for (const bool pixel : { false, true }) {
				const std::uint32_t mask = pixel ? kFramePushPS : kFramePushVS;
				for (std::uint32_t r = 0; r < kConstantBufferRegisters; ++r) {
					if (!((mask >> r) & 1))
						continue;
					if (pixel == a_pixel && r == a_register)
						return word;
					word += 2;
				}
			}
			return ~0u;
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

		/**
		 * @brief What the scene's buffers hold now (SceneSizing, the per-object buffers' objects): an object past them - its record,
		 * a geometry row it draws, its palettes or extras, its face positions - waits for their growth (Growths), drawing nothing and
		 * culling nothing (ObjectFits).
		 */
		struct SceneFit
		{
			std::uint32_t objects = ~0u, geometryRows = ~0u, extraRows = ~0u, faceVertices = ~0u;
			std::uint32_t trees = ~0u, fadeRoots = ~0u;  // the tree wind entries and fade root rows a draw reads by its slot (none: unbounded)
			bool operator==(const SceneFit&) const = default;
		};

		// What a build addresses in an epoch's buffers; a change means the resources were recreated under it.
		struct ResourceAddresses
		{
			// records: the material rows' table (the shadow views' ShadowMaterialRow, the main pass's MaterialRow); pipelineRows: the
			// main pass's pipeline rows.
			std::uint64_t constants = 0, records = 0, pipelineRows = 0, frameConstants = 0;
			std::uint64_t facePositions = 0;  // the face positions buffer (SceneBuffers::facePositions)
			std::uint32_t objectsIndex = 0, extrasIndex = 0;
			std::uint32_t placementsIndex = 0;  // the frame's placement rows (FrameValues::PlacementsIndex)
			std::uint32_t palettesIndex = 0;    // the frame's palettes (FrameValues::PalettesIndex)
			std::uint32_t shadingIndex = 0;     // the frame's shading rows (FrameValues::ShadingIndex)
			std::uint32_t treeWindIndex = 0;  // the wind buffer the frame's draws read (SceneBuffers::TreeWindReadIndex)
			std::uint32_t recordCapacity = 0;  // the material rows' table's rows (a row past it waits for the table to grow)
			std::uint32_t pipelineCapacity = 0;  // the main pipeline rows' likewise
			std::uint32_t sequenceDecals = 0;    // the sequences' decal range per group (MainSizing): a decal past it waits for their growth
			SceneFit fit;                        // what the scene's buffers hold (an object past them waits)
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

		/**
		 * @brief A view rasterizer state's buckets (ShadowBucket): the distinct pipelines its map row names, the depth-only class's
		 * first (DrawPipelines::ShadowDiscards) - a view draws them before the alpha-tested casters, whose fragments then fail
		 * the depth test sooner - each class in index order; and each key slot's bucket. A pipeline the epoch's state does not
		 * hold yet (published after it was taken) has no bucket: its key slots draw nothing this frame, as before its index.
		 */
		struct RowBuckets
		{
			std::vector<std::uint32_t> pipelines;     // by bucket
			std::vector<std::uint32_t> bucketOfSlot;  // by key slot: its bucket, or Lookups::kNone
			bool operator==(const RowBuckets&) const = default;
		};

		/**
		 * @brief The draws the scene's objects can produce (SceneDrawBound), in all and per pipeline slot, kept from the change log:
		 * a slot the log names has its share taken out and put back, so a reserve reads what changed rather than every slot. One,
		 * render thread: every epoch's reserve (shadow, occlusion, main) brings it up to date before reading it.
		 */
		struct DrawBoundStore
		{
			static constexpr std::uint32_t kNoPipeline = ~0u;
			LogCursor cursor;
			bool partitioned = false;                 // the tables had a skin partition column (SceneDrawBound reads it only then)
			std::vector<std::uint32_t> produced;      // by object slot: its draws (a free slot's none)
			std::vector<std::uint32_t> pipeline;      // by object slot: the pipeline slot they count toward (kNoPipeline: a decal's)
			std::vector<std::uint64_t> perPipeline;   // by pipeline slot
			std::uint64_t draws = 0;
			/**
			 * @brief What an object casts with: its caster technique (the shadow modes add their bits) and the occlusion views', its
			 * raster flags and vertex layout - a ShadowPipelineKey per mode - and its draws.
			 */
			struct ShadowShare
			{
				std::uint32_t produced = 0;  // 0: none (a free slot)
				bool caster = false;         // not kObjectNoShadow
				std::uint32_t technique = 0;
				std::array<std::uint32_t, kOcclusionViews> occlusion{};
				std::uint32_t rasterFlags = 0;
				std::uint64_t vertexLayout = 0;
				bool operator==(const ShadowShare&) const = default;
				/** @brief Its key slot's key under a_mode (Lookups::shadowSlots: the base technique), false when it does not draw in the mode. */
				bool KeyOf(std::uint32_t a_mode, ShadowPipelineKey& a_key) const;
			};
			// The same per shadow and occlusion mode and caster key: every draw an object of the scene can cast in the mode, the set's
			// or not. The shadow views' capacities are sized from it (ShadowBounds), so they change with the scene, not the frame's casters.
			std::vector<ShadowShare> shadow;  // by object slot
			std::array<ankerl::unordered_dense::map<ShadowPipelineKey, std::uint64_t, ShadowPipelineKeyHash>, kShadowModeCount> modeKeyDraws;
			// Since the last report.
			std::uint64_t updates = 0, changes = 0, resyncs = 0;
			// Moves whenever an object's casting share does (modeKeyDraws changed): what the revisions' shadow shapes are sized from.
			std::uint64_t shadowVersion = 0;
			ParityCounter parity;

			std::uint32_t Draws() const { return static_cast<std::uint32_t>(std::min<std::uint64_t>(draws, UINT32_MAX)); }
			std::uint32_t PipelineDraws(std::size_t a_pipeline) const
			{
				return a_pipeline < perPipeline.size() ? static_cast<std::uint32_t>(std::min<std::uint64_t>(perPipeline[a_pipeline], UINT32_MAX)) : 0u;
			}
		};

		/** @brief A build's view of the object records: the store's, or a full set of its own without one (version 0). */
		using ObjectRecordsOut = KeptView<BindlessObject>;

		/**
		 * @brief The rows the extras buffer holds (Records.h, kExtraRows a block): read straight from the tables' array, and journalled
		 * as rows, so the buffer is sent the rows changed since the version it holds. The change log names them: an extras block's
		 * rows or place (kChangeExtras). One, for every epoch's builds (SceneBuffers); in frame order like ObjectRecordStore.
		 */
		struct ExtrasStore
		{
			LogCursor cursor;
			ChangeJournal rows;
			std::vector<float> uploaded;  // CS_DCLF_PERSISTENT_PARITY: the rows as uploaded
			std::uint64_t updates = 0, rowsSent = 0, resyncs = 0;
			ParityCounter parity;
		};

		/** @brief A build's view of the rows: the tables' array, with the store's changes (version 0: sent whole). */
		struct ExtrasOut
		{
			const float* extras = nullptr;
			std::uint32_t extraRows = 0;
			ChangeJournal::Snapshot changes;
			std::uint64_t Version() const { return changes.version; }
			std::uint32_t Rows() const { return extraRows; }
			const float* Row(std::uint64_t a_row) const { return extras + std::size_t(a_row) * 4; }
			void Reset() { *this = {}; }
		};

		void UpdateExtras(ExtrasStore* a_store, std::uint64_t a_uploaded, const SceneStore::Tables& a_tables, std::uint32_t a_generation, ExtrasOut& a_out);

		/**
		 * @brief The uploads of a build's rows a buffer at a_held lacks (the changed runs, else all of them), clipped to the buffer
		 * (a_bufferRows: what is past it waits for its growth).
		 */
		template <class Emit>
		std::size_t EmitExtras(const ExtrasOut& a_out, std::uint64_t a_held, ExtrasStore* a_parity, Emit&& a_emit, std::uint32_t a_bufferRows = ~0u)
		{
			const std::uint32_t rows = std::min(a_out.Rows(), a_bufferRows);
			if (!rows || !a_out.extras)
				return 0;
			std::size_t sent = 0;
			a_out.changes.ForEachRun(a_held, rows, [&](std::uint64_t a_first, std::uint64_t a_count) {
				a_emit(a_out.Row(a_first), std::size_t(a_count) * 16, std::size_t(a_first) * 16);
				if (a_parity) {
					if (a_parity->uploaded.size() < std::size_t(rows) * 4)
						a_parity->uploaded.resize(std::size_t(rows) * 4, 0.0f);
					std::memcpy(&a_parity->uploaded[std::size_t(a_first) * 4], a_out.Row(a_first), std::size_t(a_count) * 16);
				}
				sent += static_cast<std::size_t>(a_count);
			});
			return sent;
		}

		/** @brief CS_DCLF_PERSISTENT_PARITY: the rows the buffer holds, as uploaded, against the tables' now. */
		inline void CheckExtras(ExtrasStore& a_store, const ExtrasOut& a_out)
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
		 * @brief The scene streams of one frame's tables (step 6e E1): the object records, the extras rows and the geometry slots'
		 * draws, brought up to date once (Impl::KickSceneStreams), after the frame's start wrote its set - nothing writes the frame's
		 * tables after it - and read by every build and commit of the frame as immutable views. The tables are held: the extras'
		 * view reads their array.
		 */
		struct StreamViews
		{
			std::shared_ptr<const SceneStore::Tables> tables;
			std::uint32_t generation = 0;
			ObjectRecordsOut objects;
			ExtrasOut extras;
			KeptView<GeometryDraw> geometries;
		};
		/** @brief What a set of stream views was made from: the frame's tables as they stood (their logs' ends) and their generation. */
		struct StreamsKey
		{
			const void* tables = nullptr;
			std::uint64_t log = 0, geometryLog = 0;
			std::uint32_t generation = 0;
			bool operator==(const StreamsKey&) const = default;
		};
		/**
		 * @brief A build's geometry slots: the frame's stream views', or packed whole from the tables without them (a probe's
		 * build, or no scene buffers yet).
		 */
		void TakeGeometryDraws(const StreamViews* a_streams, const SceneStore::Tables& a_tables, std::uint32_t a_generation, std::uint32_t a_frame,
			GeometryDrawsOut& a_out);

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

		/** @brief The stores brought up to a_tables (from the versions a_from), as views (StreamViews): the coordinator's alone. */
		std::shared_ptr<const StreamViews> MakeStreamViews(ObjectRecordStore& a_objects, ExtrasStore& a_extras, GeometryStore& a_geometries, const TablesHeld& a_from,
			std::shared_ptr<const SceneStore::Tables> a_hold, const SceneStore::Tables& a_tables, std::uint32_t a_generation, std::uint32_t a_frame);

		inline bool PersistentParityEnabled()
		{
			return SwitchEnabled(Switch::PersistentParity);
		}

		// What a record is built from (BuildObjectRecord): the alpha test and the LOD fades (bindings), the lights, the tree
		// animation, the palette's place and the extras'.
		constexpr std::uint32_t kObjectRecordCauses = kChangeBindings | kChangeLights | kChangeTree | kChangeSkin | kChangeExtras;

		/**
		 * @brief The build's object records: brought up to date from the change log in the store (a_uploaded is the version
		 * the buffer holds, as the inputs saw it), or built whole without one.
		 */
		void UpdateObjectRecords(ObjectRecordStore* a_store, std::uint64_t a_uploaded, const SceneStore::Tables& a_tables, std::uint32_t a_generation,
			std::uint32_t a_frame, ObjectRecordsOut& a_out);

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
				std::array<std::uint32_t, 8> key{};
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
			// Material rows written again, by the key part that moved (WriteMaterialRow): record, frame values, lookup, shared lookups,
			// technique bindings, projected, constant tables (two words).
			std::array<std::uint64_t, 8> keyMoved{};
			std::atomic<std::uint32_t> busy{ 0 };
		};

		/**
		 * @brief Rows a table at a_held lacks, each with its header's addresses made absolute (a_base plus the row's offset):
		 * a_emit(data, bytes, offset). a_patch(row, rowAddress) fixes one row's header. Rows past a_capacity wait for the table's
		 * growth (Growths): its version is filled with them, and the table then holds what it was filled with.
		 */
		template <class Row, class Patch, class Emit>
		std::size_t EmitMainRows(const KeptView<Row>& a_rows, std::uint64_t a_held, std::uint64_t a_base, std::uint64_t a_capacity, Patch&& a_patch, Emit&& a_emit)
		{
			if (!a_rows.elements)
				return 0;
			const auto& rows = *a_rows.elements;
			std::vector<Row> run;
			std::size_t sent = 0;
			a_rows.changes.ForEachRun(a_held, rows.size(), [&](std::uint64_t a_first, std::uint64_t a_count) {
				if (a_first >= a_capacity)
					return;
				a_count = std::min(a_count, a_capacity - a_first);
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
		inline void CheckSceneCapacity(const SceneBuffers& a_scene, std::size_t a_objects, std::size_t a_geometries, std::size_t a_extraRows, std::size_t a_inputs,
			std::uint32_t a_inputCapacity, const char* a_what)
		{
			if (a_objects <= a_scene.objectCapacity && a_geometries <= a_scene.geometryRows && a_extraRows <= a_scene.extraRows && a_inputs <= a_inputCapacity)
				return;
			stl::report_and_fail(fmt::format("Drawcall Limit Fix: {}'s build is past its tables: {} objects of {}, {} geometry rows of {}, {} extras rows of {}, {} inputs of {}",
				a_what, a_objects, a_scene.objectCapacity, a_geometries, a_scene.geometryRows, a_extraRows, a_scene.extraRows, a_inputs, a_inputCapacity));
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
		/** @brief Every row of a_rows, laid out for a table at a_address (a growth's fill: Growths, GrowableRows::Reserve). */
		template <class Row>
		std::vector<std::byte> CapturedMainRows(const KeptView<Row>& a_rows, std::uint64_t a_address)
		{
			std::vector<std::byte> bytes;
			if (!a_rows.elements)
				return bytes;
			const auto& rows = *a_rows.elements;
			bytes.resize(rows.size() * sizeof(Row));
			for (std::size_t i = 0; i < rows.size(); ++i) {
				Row row = rows[i];
				PatchRowAddresses(row, a_address + i * sizeof(Row));
				std::memcpy(bytes.data() + i * sizeof(Row), &row, sizeof(Row));
			}
			return bytes;
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
			RE::NiPoint3 eye, previousEye;
			std::uint32_t vsFrameMask = 0, psFrameMask = 0;  // the frame slots the commit supplies
			std::array<std::uint32_t, kDecalGroups> decalCount{};
			// The PerMaterial floats that are the frame's rather than the material's (SceneStore::
			// GetMaterialPatchedFloats / GetMaterialPatchedVSFloats, MaterialSources): the build cache leaves them
			// out of a pair's signature and repacks the group, so a drifting frame component or a scrolling
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
			// The frame's stream views the build read (its geometry slots), held with the payload (step 6e E1).
			std::shared_ptr<const StreamViews> streams;
			// The frame's textures (t16 and up) the drawn pipelines read, which only the commit can resolve (into the frame
			// record): the commit counts the ones it could not.
			std::array<std::uint64_t, 2> frameRegisters{};
			// Per object in the tables: what this build did with it - kObjectStateDrawable, kObjectStateDecal,
			// a Skip reason, or kObjectStateAbsent when it never reached the inputs (CS_DCLF_SET_PARITY explains
			// its mismatches with this).
			std::vector<std::uint8_t> objectState;
			// The culling's ViewProj (the latch's, the eye folded in) when the commit culled: set parity tests the live bounds with it.
			std::array<float, 16> cullViewProj{};
			bool culled = false;
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
			// Built at the epoch with rows of its own (the fallback, or a probe): their versions are no journal's the tables follow.
			bool foreignRows = false;
			// The segment's resident region (ResidentRegion): its inputs lead the input buffer, uploaded when residentVersion
			// is not the one the buffer holds (Resources::residentUploaded); inputList follows them.
			KeptView<DrawInput> resident;
			std::uint32_t residentDraws = 0, residentPairs = 0, residentUndrawable = 0, residentResyncs = 0;
			std::uint32_t residentResyncReasons = 0;  // bit k: kResyncReasons' k
			std::uint32_t residentDecalRetakes = 0;   // builds whose decal groups' counts moved (no resync: UpdateRegionEntries)
			std::uint32_t residentParityChecks = 0, residentParityMismatches = 0, residentMissing = 0;
			std::uint32_t residentPairsChecked = 0, residentPairsStale = 0;  // pairs whose witness moved with no event (UpdateRegionPairs)

			void Reset()
			{
				resident.Reset();
				residentDraws = residentPairs = residentUndrawable = residentResyncs = residentResyncReasons = residentDecalRetakes = 0;
				residentParityChecks = residentParityMismatches = residentMissing = 0;
				residentPairsChecked = residentPairsStale = 0;
				foreignRows = false;
				bindingOwners.clear();
				sequences.clear();
				inputList.clear();
				geometryDraws.Reset();
				streams.reset();
				faceStreams.clear();
				frameRegisters = {};
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
			// whose entry (SceneStore::Tables::sunEntryNode) is outside every one is no candidate of the sun's cascade culls. However
			// many there are: the sun views' latches name them in a shared region of the latch block (ReserveShadowLatch).
			std::vector<SunEntryProcess> sunEntryProcesses;
			// The sun entries the scene store found DCLF could take out of the cascade culls (SunAccumulation): the build
			// turns them into the next frame's exclusion.
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
			std::shared_ptr<const StreamViews> streams;  // the frame's stream views the build read (step 6e E1)
			ConstantArena arena;  // the view slots' head and the frame record are reserved; the commit writes the views into it
			DrawBindings frameRecord{};  // every view's textures and samplers but the diffuse (kShadowFrameRecordOffset)
			// The rows the inputs name: the kept state's (journalled), or the build's own (version 0: sent whole).
			KeptView<ShadowMaterialRow> materialRows;
			// Rows the build needed, within the table's capacity or not: what the next frame's Reserve grows it to. The
			// materials past the capacity wait for it (their casters stay the engine's this frame).
			std::uint32_t rowsWanted = 0, waitingRows = 0;
			// Per mode, the draws its inputs can produce (a skin draws once per partition): its views' max count.
			std::array<std::uint32_t, kShadowModeCount> modeDraws{};
			// Per mode, the same by key slot (DrawInput::pipelineIndex): what sizes a view's buckets (ShadowBucket).
			std::array<std::vector<std::uint32_t>, kShadowModeCount> keySlotDraws;
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
			// Set members a mode could not draw (waiting on a pipeline or a texture): a defect, the engine having withheld them.
			std::uint32_t setWaiting = 0;
			std::string setWaitingFirst;  // the first, and why
			// The build ahead also builds the next frame's exclusions, which the epoch publishes; empty for a build made at the epoch,
			// which builds them at the publish.
			std::shared_ptr<SunExclusion> sunExclusion;  // from the cascades' mode (BuildSunExclusion)
			std::shared_ptr<SunExclusion> parabolicExclusion;  // and from the paraboloid mode (LocalLightCull)

			void Reset()
			{
				setWaiting = 0;
				setWaitingFirst.clear();
				sunExclusion.reset();
				parabolicExclusion.reset();
				arena.Reset(kShadowConstantBytes);
				frameRecord = {};
				materialRows.Reset();
				rowsWanted = waitingRows = 0;
				modeDraws = {};
				for (auto& slots : keySlotDraws)
					slots.clear();
				bindingOwners.clear();
				objectRecord.clear();
				for (auto& modeInputs : inputList)
					modeInputs.clear();
				kept = false;
				regionInputs = {};
				membership = {};
				geometries.Reset();
				streams.reset();
				faceStreams.clear();
				skippedTexture = skippedPipeline = deferredTextures = deferredPipelines = 0;
			}
		};

		/**
		 * @brief A shadow or occlusion view as its epoch's shape names it: its slot, mode, target, slice and rectangle, and the
		 * rasterizer state whose pipeline map row its buckets are (ShadowRowBuckets). What the shape producer (MakeShadowShape) makes
		 * the rest of the view from; a frame's views are one of the layouts its revision has seen (the recent shapes).
		 */
		struct ShadowViewLayout
		{
			std::uint32_t slot = 0, modeIndex = 0, target = 0, slice = 0;
			std::uint32_t x = 0, y = 0, width = 0, height = 0;
			std::uint32_t rasterState = 0;
			bool operator==(const ShadowViewLayout&) const = default;
		};
		std::vector<ShadowViewLayout> LayoutOf(const ShadowFrame& a_frame);

		/**
		 * @brief What a shadow or occlusion epoch's shape is made from (MakeShadowShape): the views' layout, each view's map row's
		 * buckets, and the revision's payload (its modes' and key slots' draws, the arena's blocks), with the slots' resources.
		 */
		/**
		 * @brief Per mode, the draws the scene's objects can cast in it, in all and per key slot (DrawBoundStore::modeKeyDraws through
		 * the lookups' key slots; ShadowBoundsOf): a bound on any frame's payload (ShadowPayload::modeDraws, keySlotDraws) while the
		 * scene's objects stand.
		 */
		struct ShadowBounds
		{
			std::array<std::uint32_t, kShadowModeCount> modeDraws{};
			std::array<std::vector<std::uint32_t>, kShadowModeCount> keySlotDraws;
		};
		ShadowBounds ShadowBoundsOf(const DrawBoundStore& a_bound, const Lookups& a_lookups);

		struct ShadowShapeInputs
		{
			rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
			ShadowIndirectState indirect{};
			std::vector<ShadowViewLayout> views;
			std::vector<RowBuckets> rows;  // per view
			const ShadowPayload* payload = nullptr;
			// What the capacities are sized for, with the payload's draws: the scene's (the revision's draws stay within them).
			const ShadowBounds* bounds = nullptr;
			// The sizing, the material rows and the view blocks the shape is for: a revision's (Growths::RevisionSizing,
			// GrowableRows::RevisionAddress), else the resources' own.
			const ShadowSizing* sizing = nullptr;
			std::uint64_t materialRows = 0, viewBlocks = 0;
			std::shared_ptr<const ShadowFrame> previous;  // the last published shape: its slots' capacities only grow
			LatchedList latched;
		};
		std::shared_ptr<ShadowFrame> MakeShadowShape(const ShadowResources& a_resources, const ShadowShapeInputs& a_in);
		/**
		 * @brief The shadow commit's latched copies (CS's SharedData and FeatureData at their places in the constants, ShadowArenaBlocksOf),
		 * as its LatchedUploads makes them.
		 */
		std::vector<LatchedCopy> ShadowLatchedLayout(const ShadowResources& a_resources);
		/**
		 * @brief The blocks every shadow view's push data names besides its own (kShadowPushZeros and after), as offsets into the shadow
		 * constants: the zero block, then CS's SharedData and FeatureData at the places their sizes (fixed for the session) give. A
		 * block CS does not have reads the zero block.
		 */
		struct ShadowArenaBlocks
		{
			std::uint64_t zeros = 0, sharedData = 0, featureData = 0;
			std::uint32_t sharedBytes = 0, featureBytes = 0;
		};
		ShadowArenaBlocks ShadowArenaBlocksOf();

		/**
		 * @brief The geometry slots an object's draw writes a sequence for, in BuildDrawsCS's order: its one
		 * geometry, or for a skin of several partitions each partition its mask names, following the slots'
		 * nextPartition links from the first.
		 */
		template <class F>
		void ForEachDrawnGeometry(const SceneStore::Tables& a_tables, std::uint32_t a_firstSlot, std::uint32_t a_partitions, F&& a_draw);

		/**
		 * @brief What a_scene's buffers hold now, with per-object buffers of a_objectBuffers objects (an epoch's inputs). Without the
		 * tree wind or fade programs no draw reads their rows (the records keep their own wind; no fade root is tested).
		 */
		inline SceneFit SceneFitOf(const SceneBuffers& a_scene, std::uint32_t a_objectBuffers)
		{
			return { std::min(a_scene.objectCapacity, a_objectBuffers), a_scene.geometryRows, a_scene.extraRows, a_scene.faceVertices,
				a_scene.treeWind && a_scene.trees ? a_scene.treeCapacity : ~0u, a_scene.fadeState && a_scene.fadeRoots ? a_scene.fadeRootCapacity : ~0u };
		}

		/** @brief Whether every object of a_tables fits a_fit (ObjectFits for all of them, without a scan). */
		inline bool TablesFit(const SceneStore::Tables& a_tables, const SceneFit& a_fit)
		{
			return a_tables.objects.size() <= a_fit.objects && a_tables.geometries.size() + a_tables.faceStreams.size() <= a_fit.geometryRows &&
			       a_tables.extraRows.size() / 4 <= a_fit.extraRows &&
			       a_tables.trees.size() <= a_fit.trees && a_tables.fadeRoots.size() <= a_fit.fadeRoots;
		}

		/**
		 * @brief Whether object a_object is within what the scene's buffers hold (SceneFit): its record, every geometry row its draws
		 * read (its partitions', a face's positions' row), its extras and its face positions. One past them waits for
		 * their growth: no input names it, so no draw or culling test reads past a buffer.
		 */
		inline bool ObjectFits(const SceneStore::Tables& a_tables, std::uint32_t a_object, const SceneFit& a_fit)
		{
			if (a_object >= a_fit.objects)
				return false;
			const auto& object = a_tables.objects[a_object];
			if (object.geometryIndex >= a_fit.geometryRows)
				return false;
			bool rows = true;
			if (a_object < a_tables.skinPartitions.size() && a_tables.skinPartitions[a_object])
				ForEachDrawnGeometry(a_tables, object.geometryIndex, a_tables.skinPartitions[a_object], [&](std::uint32_t a_slot) { rows = rows && a_slot < a_fit.geometryRows; });
			if (!rows)
				return false;
			if (a_object < a_tables.extraOffset.size() && a_tables.extraOffset[a_object] != kNoExtraRows &&
				std::uint64_t(a_tables.extraOffset[a_object]) + kExtraRows > a_fit.extraRows)
				return false;
			if (a_object < a_tables.objectTree.size() && a_tables.objectTree[a_object] < kNodelessTree && a_tables.objectTree[a_object] >= a_fit.trees)
				return false;
			if (a_object < a_tables.objectFadeRoot.size() && a_tables.objectFadeRoot[a_object] != kNoFadeRoot && a_tables.objectFadeRoot[a_object] >= a_fit.fadeRoots)
				return false;
			if (a_object < a_tables.faceStream.size() && a_tables.faceStream[a_object] != kNoFaceStream) {
				const std::uint32_t stream = a_tables.faceStream[a_object];
				if (a_tables.geometries.size() + stream >= a_fit.geometryRows)
					return false;
				if (stream < a_tables.faceStreams.size() && std::uint64_t(a_tables.faceStreams[stream].region) + a_tables.faceStreams[stream].vertexCount > a_fit.faceVertices)
					return false;
			}
			return true;
		}

		template <class F>
		void ForEachDrawnGeometry(const SceneStore::Tables& a_tables, std::uint32_t a_firstSlot, std::uint32_t a_partitions, F&& a_draw)
		{
			if (a_partitions == kNoPartitions)
				return;
			std::uint32_t slot = a_firstSlot;
			if (a_partitions & kPartitionChain) {
				for (std::uint32_t i = 0; i < (a_partitions & kPartitionChainCount) && slot < a_tables.geometries.size(); ++i) {
					a_draw(slot);
					slot = a_tables.geometries[slot].nextPartition;
				}
				return;
			}
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
		 * centre is its placement row's (BindlessPlacement::lodFadeNode, FrameValues); the slot and the distance only
		 * change with its membership and bindings.
		 */
		inline void SetFadeRow(DrawInput& a_input, const SceneStore::Tables& a_tables, std::size_t a_object)
		{
			if (a_object < a_tables.objectFadeRoot.size())
				a_input.fadeRoot = a_tables.objectFadeRoot[a_object];
			if (!(a_input.flags & (kObjectFadeTest | kObjectHeightTest)) || a_object >= a_tables.fadeDistance.size() || a_object >= a_tables.hasFadeNode.size())
				return;
			if (!a_tables.hasFadeNode[a_object])
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
		std::size_t EmitGeometryDraws(const GeometryDrawsOut& a_out, std::uint64_t a_held, Emit&& a_emit, std::size_t a_rows = ~std::size_t(0))
		{
			// Clipped to the buffer's rows (a_rows): what is past them waits for its growth (Growths).
			std::size_t bytes = a_out.slots.Emit(a_held, a_emit, a_rows);
			if (!a_out.faces.empty() && a_out.SlotCount() < a_rows) {
				const std::size_t faces = std::min(a_out.faces.size(), a_rows - a_out.SlotCount());
				a_emit(a_out.faces.data(), faces * sizeof(GeometryDraw), a_out.SlotCount() * sizeof(GeometryDraw));
				bytes += faces * sizeof(GeometryDraw);
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
		// one the epoch's buffer holds. The snapshot stays the walk's until its next walk, which is after the commit. A region
		// past the buffer (a_vertices) waits for its growth.
		template <class Uploads>
		std::uint32_t UploadFaceStreams(const std::vector<SceneStore::Tables::FaceStream>& a_streams, const Versioned& a_positions,
			ankerl::unordered_dense::map<std::uint32_t, std::uint64_t>& a_uploaded, Uploads& a_uploads, std::uint32_t a_vertices = ~0u)
		{
			std::uint32_t count = 0;
			for (const auto& stream : a_streams) {
				if (stream.object == SceneStore::Tables::kNoFaceObject || std::uint64_t(stream.region) + stream.vertexCount > a_vertices)
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
			std::uint8_t segments = 0;      // the segments its pairs' verdicts are of (MainBuild::Segments)
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
			std::size_t draws = 0;
			std::size_t decals = 0;               // drawable decal entries
			std::size_t undrawable = 0;           // entries with no draw this frame
			// What every face shape's stream index is relative to (the geometry slots' count; FaceStreamGeometry) and whether the
			// positions buffer exists: when either moves, every face entry is taken again. ~0: not yet read.
			std::size_t faceBase = ~std::size_t(0);
			SceneFit fit;  // what the scene's buffers held at its last build: their growth reads every slot again
			std::array<std::uint32_t, kDecalGroups> decalCount{};  // the decal ranges it was built for (MainBuild::decalCount)
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
		 * @brief What BuildMainPayload keeps per segment across frames: the resident region. The blocks and
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
		void BuildMainPayload(const MainInputs& a_in, const SceneStore::Tables& a_tables, const FrameTables& a_frame, const Lookups& a_lookups, MainPayload& a_out, MainRows& a_rows,
			BuildCache* a_cache = nullptr, std::shared_ptr<const StreamViews> a_streams = nullptr);
		/**
		 * @brief Both main segments' payloads in one build (U4a): one list, which both read through their view bits, and a member
		 * drawable in both or neither. a_in[j] / a_out[j] per segment (kAsyncColour, kAsyncZPrepass); a segment with no inputs is not built.
		 */
		void BuildMainPayloads(const std::array<const MainInputs*, 2>& a_in, const SceneStore::Tables& a_tables, const FrameTables& a_frame, const Lookups& a_lookups,
			const std::array<MainPayload*, 2>& a_out, MainRows& a_rows, BuildCache* a_cache, std::shared_ptr<const StreamViews> a_streams);

		// The objects a mode's inputs draw, as the geometry the native shadow loop withholds (PassCapture).

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

		/** @brief The view types a set phase mask takes part in (kView*). */
		inline std::uint32_t ViewBitsOf(std::uint8_t a_phases)
		{
			std::uint32_t bits = 0;
			bits |= (a_phases & kSetMain) ? kViewMain : 0u;
			bits |= (a_phases & kSetCaster) ? (kViewSunCaster | kViewSpotCaster) : 0u;
			bits |= (a_phases & kSetCasterPoint) ? kViewPointCaster : 0u;
			bits |= (a_phases & kSetOccluderSky) ? kViewSkyOccluder : 0u;
			bits |= (a_phases & kSetOccluderPrecipitation) ? kViewPrecipOccluder : 0u;
			bits |= (a_phases & kSetReflection) ? kViewReflection : 0u;
			return bits;
		}
		/**
		 * @brief An input's view mask (DrawInput::ViewWords::mask): the view types of the object's applied phases (Tables::setPhases),
		 * and kViewMainCull for a main candidate (every input of the main payloads).
		 */
		inline std::uint32_t ViewMaskOf(const SceneStore::Tables& a_tables, std::size_t a_object, bool a_mainCandidate)
		{
			const std::uint8_t phases = a_object < a_tables.setPhases.size() ? a_tables.setPhases[a_object] : std::uint8_t{ 0 };
			return ViewBitsOf(phases) | (a_mainCandidate ? kViewMainCull : 0u);
		}
		/**
		 * @brief A main input's view mask (U4a: the one main list both segments read). A decal's is kViewDecal alone: the colour
		 * segment's fixed slots, culled by no other view. Every other candidate's is its phases' bits with kViewMainCull, kViewMain
		 * exactly while its record is a member (kObjectMember, what its drawable bit follows), so a drawable input is the colour
		 * segment's whenever it is the depth segment's.
		 */
		inline std::uint32_t MainMaskOf(const SceneStore::Tables& a_tables, std::size_t a_object)
		{
			const std::uint32_t flags = a_object < a_tables.objects.size() ? a_tables.objects[a_object].flags : 0u;
			if (ObjectDecalGroup(flags))
				return kViewDecal;
			return (ViewMaskOf(a_tables, a_object, true) & ~kViewMain) | ((flags & kObjectMember) ? kViewMain : 0u);
		}
		/** @brief The view bits a shadow or occlusion view draws: its mode's, a clamped view's split by whether it is the sun's. */
		inline std::uint32_t ShadowViewBits(std::uint32_t a_mode, bool a_sun)
		{
			if (IsOcclusionMode(a_mode))
				return OcclusionOfMode(a_mode) == kOcclusionSky ? kViewSkyOccluder : kViewPrecipOccluder;
			if (a_mode == kParabolicShadowMode)
				return kViewPointCaster;
			return a_sun ? kViewSunCaster : kViewSpotCaster;
		}

		/**
		 * @brief The technique object a_object's key slot names under mode a_mode (Lookups::shadowSlots): an occlusion view's own, or
		 * its caster technique without any mode's bits - one slot for every caster mode. 0: none.
		 */
		inline std::uint32_t BaseTechnique(const SceneStore::Tables& a_tables, std::uint32_t a_mode, std::size_t a_object)
		{
			if (IsOcclusionMode(a_mode)) {
				const auto& column = a_tables.occlusionTechnique[OcclusionOfMode(a_mode)];
				return a_object < column.size() ? column[a_object] : 0u;
			}
			return (a_tables.objects[a_object].flags & kObjectNoShadow) ? 0u : a_tables.shadowTechnique[a_object];
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
			std::uint64_t builds = 0, reused = 0, translated = 0;  // since the last report (translated: reused for newer candidates)
			ParityCounter parity;
		};

		/**
		 * @brief The next frame's sun entry exclusion, from the candidates and the cascades' mode's inputs, which are the set's
		 * casters (SceneSet.h): a candidate stays in the cascade culls when one of its table objects casts (no
		 * kObjectNoShadow) and is not an input, since the engine must then still draw it. Null without candidates.
		 */
		std::shared_ptr<SunExclusion> BuildSunExclusion(const std::shared_ptr<const SunCandidates>& a_candidates, const ShadowPayload& a_payload, std::uint32_t a_mode,
			const SceneStore::Tables& a_tables, SunExclusionCache* a_cache = nullptr);

		// Whether an object's entry is outside every one of the sun's full-frustum processes, so the sun's cascade
		// culls never reach it (ShadowInputs::sunEntryProcesses): the CPU's verdict, from the entry node's bound now (render
		// thread, diagnostics), which the persistent parity compares with the test BuildDraws makes on the placement row.
		inline bool OutsideSunEntry(const ShadowInputs& a_in, const SceneStore::Tables& a_tables, std::size_t a_object)
		{
			const auto* node = a_object < a_tables.sunEntryNode.size() ? a_tables.sunEntryNode[a_object] : nullptr;
			if (!node || node->worldBound.radius < 0.0f)
				return false;
			const auto& bound = node->worldBound;
			const float entry[4]{ bound.center.x, bound.center.y, bound.center.z, bound.radius };
			return OutsideSunEntryProcesses(a_in.sunEntryProcesses, entry);
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
			SceneFit fit;  // what the scene's buffers held at its last build: their growth reads every object again
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
				std::vector<std::uint32_t> waiting;  // objects waiting for a pipeline or a texture
				std::vector<std::uint8_t> waitingMark;
				std::vector<std::uint32_t> faces;    // objects the frame's list writes (face shapes, second-stream positions)
				std::vector<std::uint8_t> faceMark;
				// The build version at which an object last joined or left the mode's inputs (the region or the faces): what
				// the sun exclusion's reuse reads (BuildSunExclusion).
				std::uint64_t membership = 0;
				/** @brief Empty, for every object to be read again; the inputs' journal counts on. */
				void Reset()
				{
					auto kept = std::move(inputs);
					*this = Mode{};
					inputs = std::move(kept);
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
			std::shared_ptr<const StreamViews> a_streams = nullptr, ShadowKept* a_kept = nullptr);

		/**
		 * @brief CS_DCLF_PERSISTENT_PARITY: a kept shadow build against the same build made the per-frame way. Per used mode, the
		 * same casters with the same inputs (the record's number apart), records binding the same textures and samplers with the
		 * same texcoord values.
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

		// The resident region's resync reasons (MainBuild::UpdateRegionEntries): log, segment, shrunk, scope, fit.
		constexpr std::size_t kResyncReasons = 5;
		constexpr std::size_t kAsyncColour = 0;
		constexpr std::size_t kAsyncZPrepass = 1;
		constexpr std::size_t kAsyncShadow = 2;

		// Whether BuildMainPayload reads MainInputs::eye/previousEye: only CS_DCLF_BINDLESS_PARITY's reference, the engine's
		// eye-relative PerGeometry groups, does.
		inline bool BuildReadsEye(const MainInputs& a_in)
		{
			return a_in.bindlessParity;
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

		/**
		 * @brief The commit's hand-over of a batch a worker staged: its recorded list, which the submission takes as it is with nothing
		 * recorded on this thread, when the upload service can take it in its place (nothing queued before it, no backing released
		 * since its recording); otherwise the batch as staged, recorded at the submission as before.
		 */
		/** @brief Render thread, at a job's kick: the device its staged batch is recorded with (null without the graph host). */
		inline rhi::Device RecordingDevice()
		{
			auto* host = RenderGraphRuntime::Get().Host();
			return host ? host->Device() : rhi::Device{};
		}

		inline void SubmitWorkerBatch(std::shared_ptr<org::runtime::StagedUploadBatch> a_batch)
		{
			auto* service = org::runtime::GetActiveUploadService();
			if (!service->SubmitRecordedUploads(a_batch))
				service->SubmitStagedUploads(std::move(a_batch));
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
				if (!a_data || !a_bytes)
					return;
				batch->Stage(org::runtime::UploadTarget::FromShared(a_target), static_cast<std::size_t>(a_offset), a_data, a_bytes);
				FrameData::Note(FrameDataWhere("staged", a_target, a_offset), a_bytes);
			}
			// A versioned buffer: its current version.
			void operator()(const Versioned& a_target, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset)
			{
				(*this)(a_target->Get(), a_data, a_bytes, a_offset);
			}

		private:
			std::shared_ptr<org::runtime::StagedUploadBatch> batch;
		};

		/**
		 * @brief A main commit's uploads of per-frame values (a block written whole, a counter zeroed): a target the epoch's latched
		 * copies declared (Resources::latchedTargets) gets its bytes in the commit's latch block and a copy in the frame's list, which
		 * the epoch's first pass records with the ticket, on ORG's host thread; any other goes to a_fallback, staged. A copy is in the
		 * list only in a frame that writes it, so a target not written keeps what it holds, as with staging.
		 */
		class LatchedUploads
		{
		public:
			/** @brief a_block: the segment's latch block (Resources::latchedBlocks), written at a_slot, replaced by a larger one when full. */
			LatchedUploads(const std::atomic<std::shared_ptr<const std::vector<const void*>>>& a_targets, CommitUploads& a_fallback,
				std::shared_ptr<org::LatchBlock>& a_block, std::uint32_t a_slot, std::uint32_t a_slots) :
				targets(a_targets.load(std::memory_order_acquire)), fallback(a_fallback), block(&a_block), slot(a_slot), slots(a_slots)
			{}
			/**
			 * @brief Into a shape's latched copies (a scene revision's, R3c): each value at its copy's place in the list's block, the rest
			 * of the copy zeroed; a value the list has no copy for is staged (a_fallback), as a target no pass declared is. Finish zeroes
			 * the copies no value was written to, and returns the list.
			 */
			LatchedUploads(const LatchedList& a_layout, CommitUploads& a_fallback, std::uint32_t a_slot) :
				fallback(a_fallback), slot(a_slot), layout(&a_layout), written(a_layout.copies.size(), 0)
			{}
			/** @brief Values the layout had no copy for (staged instead). */
			std::uint32_t Misses() const { return misses; }

			// A latched value is counted here (FrameData), a staged one by the fallback.
			void operator()(const std::shared_ptr<org::Buffer>& a_target, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset)
			{
				if (Latch(a_target.get(), a_data, a_bytes, a_offset, [&] { fallback(a_target, a_data, a_bytes, a_offset); }))
					FrameData::Note(FrameDataWhere("latched", a_target, a_offset), a_bytes);
			}
			// A versioned buffer: latched by its key, copied into whichever version the epoch's preparation resolved.
			void operator()(const Versioned& a_target, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset)
			{
				if (Latch(a_target->Key(), a_data, a_bytes, a_offset, [&] { fallback(a_target, a_data, a_bytes, a_offset); }))
					FrameData::Note(FrameDataWhere("latched", a_target->Get(), a_offset), a_bytes);
			}

		private:
			// True when the value went into the latch block (false: nothing, or staged by a_fallback).
			template <class Fallback>
			bool Latch(const void* a_key, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset, Fallback&& a_fallback)
			{
				if (!a_data || !a_bytes)
					return false;
				if (layout) {
					const auto& listed = layout->copies;
					for (std::size_t i = 0; i < listed.size(); ++i) {
						const auto& copy = listed[i];
						if (copy.target != a_key || copy.dstOffset != a_offset || a_bytes > copy.bytes || !layout->latch)
							continue;
						auto region = layout->latch->Slot(slot).subspan(copy.latchOffset, copy.bytes);
						std::memcpy(region.data(), a_data, a_bytes);
						std::memset(region.data() + a_bytes, 0, copy.bytes - a_bytes);
						written[i] = 1;
						return true;
					}
					++misses;
					a_fallback();
					return false;
				}
				if (!targets || std::find(targets->begin(), targets->end(), a_key) == targets->end()) {
					a_fallback();
					return false;
				}
				const std::size_t at = (used + 15) & ~std::size_t(15);
				auto& current = *block;
				if (!current || at + a_bytes > current->Stride()) {
					// A larger block, with what this commit has written so far: frames in flight keep the old one (LatchedList::latch).
					auto grown = std::make_shared<org::LatchBlock>("cs.dclf.latched-copies",
						static_cast<std::uint32_t>(std::bit_ceil(std::max<std::size_t>(2 * (at + a_bytes), 4096))), slots);
					if (current && used)
						std::memcpy(grown->Slot(slot).data(), current->Slot(slot).data(), used);
					current = std::move(grown);
				}
				std::memcpy(current->Slot(slot).data() + at, a_data, a_bytes);
				used = at + a_bytes;
				copies.push_back({ a_key, static_cast<std::uint32_t>(at), static_cast<std::uint32_t>(a_bytes), a_offset });
				return true;
			}

		public:
			/** @brief The frame's list: the copies of what this commit latched, from the block it wrote. */
			LatchedList Finish()
			{
				if (layout) {
					// A copy no value was written to this commit copies zeros, never what an older frame left in the slot.
					for (std::size_t i = 0; i < written.size(); ++i)
						if (!written[i] && layout->latch) {
							const auto& copy = layout->copies[i];
							auto region = layout->latch->Slot(slot).subspan(copy.latchOffset, copy.bytes);
							std::memset(region.data(), 0, region.size());
						}
					return *layout;
				}
				LatchedList out;
				if (copies.empty())
					return out;
				out.latch = *block;
				out.copies = std::move(copies);
				return out;
			}

		private:
			std::shared_ptr<const std::vector<const void*>> targets;
			CommitUploads& fallback;
			std::shared_ptr<org::LatchBlock>* block = nullptr;
			std::uint32_t slot = 0, slots = 0;
			std::size_t used = 0;
			std::vector<LatchedCopy> copies;
			const LatchedList* layout = nullptr;
			std::vector<std::uint8_t> written;
			std::uint32_t misses = 0;
		};

		/**
		 * @brief The index pool every plain indexed draw binds (IndexPool): the scene's (SceneBuffers::pool), made with its
		 * copy program by whichever extension sets up first. Null, logged, when the program cannot be created.
		 */
		std::shared_ptr<IndexPool> EnsureIndexPool(SceneBuffers& a_scene, rhi::Device a_device, bool a_small);

		/**
		 * @brief Brings the index pool up to the tables (ShadowEpochs.cpp): whichever commit draws from it first in a frame gives the
		 * ranges out and writes their copies' dispatch at a_poolOffset of its latch slot; a later commit finds nothing to copy.
		 */
		void UpdateIndexPool(IndexPool& a_pool, const SceneStore::Tables& a_tables, std::uint32_t a_generation, const org::LatchBlock& a_latch,
			std::uint32_t a_latchSlot, std::uint32_t a_poolOffset, CommitUploads& a_uploads);

		// The graph extensions that add DCLF's passes (Passes.cpp).
		std::unique_ptr<org::RenderGraph::IRenderGraphExtension> MakeMainOpaqueExtension(std::shared_ptr<Resources> a_resources);
		std::unique_ptr<org::RenderGraph::IRenderGraphExtension> MakeShadowExtension(std::shared_ptr<ShadowResources> a_resources);

		/*
		 * The water reflection's cube map faces drawn by DCLF (dclf-lod.md, "Water reflections"; Reflection.cpp). One epoch per
		 * reflection update (TESWaterReflections::Update), after the engine's face loop. The faces render before this frame's
		 * builds, so they draw from what the frame before left: the depth segment's inputs, the main rows, the object records and
		 * geometry, the index pool, the colour segment's frame constants, tree LOD's tables. Each face culls the depth inputs
		 * through BuildDraws' bucket path (frustum only), its map sending each LOD pipeline slot to the bucket of its forward
		 * pipeline and every other slot to none; tree LOD is culled into a list of the face's. The shape always has all six faces,
		 * so the recordings hold across updates: a face the update does not render culls nothing (its latch's dispatch is 0, its
		 * tree row names no slots) and draws nothing.
		 */
		constexpr std::uint32_t kReflectionFaces = 6;
		// A face's PerFrame (b12, VS and PS), as the engine wrote it for the face: at most a D3D11 block's worth DCLF captures.
		constexpr std::uint32_t kReflectionFaceBlockBytes = 1024;

		/** @brief The reflection latch block's region per frame slot: the faces' BuildDrawsLatch, the slots' map, each face's bucket table. */
		struct ReflectionLatchLayout
		{
			std::uint32_t slots = 0;    // pipeline slots the map holds a word for
			std::uint32_t buckets = 0;  // buckets a face's table holds (first, capacity)
			static constexpr std::uint32_t MapOffset() { return kReflectionFaces * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)); }
			std::uint32_t TableOffset(std::uint32_t a_face) const { return MapOffset() + slots * 4 + a_face * buckets * 8; }
			// A face's values the latched copies take to its buffers (ReflectionLatchedCopiesPass): its per-frame block, its tree LOD
			// row and its visible list's header.
			static constexpr std::uint32_t kTreeRowInFace = kReflectionFaceBlockBytes;
			static constexpr std::uint32_t kTreeHeaderInFace = kTreeRowInFace + static_cast<std::uint32_t>(sizeof(TreeLod::DrawRow));
			static constexpr std::uint32_t kFaceBytes = (kTreeHeaderInFace + static_cast<std::uint32_t>(sizeof(TreeLod::VisibleHeader)) + 255u) & ~255u;
			std::uint32_t FaceOffset(std::uint32_t a_face) const { return ((TableOffset(kReflectionFaces) + 255u) & ~255u) + a_face * kFaceBytes; }
			std::uint32_t Bytes() const { return FaceOffset(kReflectionFaces); }
			bool operator==(const ReflectionLatchLayout&) const = default;
		};

		/** @brief What the reflection epoch's passes record against; per-update values are in its latch and buffers. */
		struct ReflectionFrame
		{
			std::uint64_t generation = 0;
			rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
			IndirectState indirect{};  // zLayout and zDrawSignature (the version holds them)
			std::shared_ptr<const org::LatchBlock> latch;
			std::uint32_t width = 0, height = 0;   // a face
			std::uint64_t materialRows = 0, pipelineRows = 0;  // the main rows' tables (BuildDraws' RowsOf)
			std::uint32_t sequenceDraws = 0;       // a face's range of the sequences
			// A face's buckets: a forward pipeline, its range in the face's sequences.
			struct Bucket
			{
				rhi::PipelineHandle pipeline{};
				std::uint32_t first = 0, capacity = 0;
				bool operator==(const Bucket& o) const { return SameHandle(pipeline, o.pipeline) && first == o.first && capacity == o.capacity; }
			};
			std::vector<Bucket> buckets;
			// Per face, the frame push data: the colour segment's frame constants with VS and PS b12 the face's block.
			std::array<std::array<std::uint32_t, kFramePushWords>, kReflectionFaces> push{};
			// Tree LOD: the forward pipeline and its draw's signature (invalid: no tree LOD in the faces), the cull's groups.
			rhi::PipelineHandle tree{};
			rhi::CommandSignatureHandle treeSignature{};
			std::uint32_t treeGroups = 0;
			// The latched copies' sources (ReflectionLatchedCopiesPass): the faces' values at facesOffset of the latch's slot region
			// (ReflectionLatchLayout::FaceOffset(0)), and the zeros (ReflectionResources::zeros).
			std::uint32_t facesOffset = 0;
			std::shared_ptr<const org::LatchBlock> zeros;
			// Not compared (implied by the latch block and the buckets): what a commit writing its values into this shape writes them
			// against (R3c) - the latch's layout, and each pipeline slot's bucket (PlanReflectionBuckets).
			ReflectionLatchLayout latchLayout;
			std::shared_ptr<const std::vector<std::uint32_t>> map;

			bool SameShape(const ReflectionFrame& o) const
			{
				return latch == o.latch && facesOffset == o.facesOffset && zeros == o.zeros && SameHandle(resourceHeap, o.resourceHeap) && SameHandle(samplerHeap, o.samplerHeap) && SameIndirect(indirect, o.indirect) &&
				       width == o.width && height == o.height && materialRows == o.materialRows && pipelineRows == o.pipelineRows && sequenceDraws == o.sequenceDraws &&
				       buckets == o.buckets && push == o.push && SameHandle(tree, o.tree) && SameHandle(treeSignature, o.treeSignature) && treeGroups == o.treeGroups;
			}
		};

		/**
		 * @brief What the reflection's buffers hold (Growths::Change, adopted with the revision that names it; RevisionSizing for a
		 * revision's shape): each face's range of the sequences, the faces' bucket counts in words, and the scene's tree LOD shape
		 * slots its tree lists hold (the scene's, adopted with or after it).
		 */
		struct ReflectionSizing
		{
			std::uint32_t sequenceDraws = 0;  // per face
			std::uint32_t bucketCountWords = 0;
			std::uint32_t treeShapeCapacity = 0;
			bool operator==(const ReflectionSizing&) const = default;
		};

		struct ReflectionResources : ReflectionSizing
		{
			// The main pass's (its depth inputs, rows, visibility, frame constants) and, through it, the scene's.
			std::shared_ptr<Resources> main;
			std::shared_ptr<org::LatchBlock> latch;
			ReflectionLatchLayout latchLayout;
			Versioned sequences;
			std::shared_ptr<org::Buffer> count;
			std::array<Versioned, kReflectionFaces> bucketCounts;
			// Zeros, never written: what the latched copies zero the draw count and the faces' bucket counts from.
			std::shared_ptr<org::LatchBlock> zeros;
			std::shared_ptr<org::Buffer> faceBlocks;  // kReflectionFaceBlockBytes per face
			std::uint64_t faceBlocksAddress = 0;
			// DCLF's depth for the faces, cleared per face, and the engine's cube target, with a render target view per face.
			std::shared_ptr<org::PixelBuffer> depth;
			std::shared_ptr<org::ExternalTextureResource> cube;
			ID3D11Texture2D* cubeTexture = nullptr;
			std::uint32_t width = 0, height = 0;
			// Tree LOD: per face a draw row (TreeLod::DrawRow, its visible list the face's) and a visible list, for the scene's shape slots.
			std::array<std::shared_ptr<org::Buffer>, kReflectionFaces> treeRows;
			std::array<Versioned, kReflectionFaces> treeVisible;
			std::array<std::uint64_t, kReflectionFaces> treeRowsAddress{}, treeVisibleAddress{};
			// The passes are to be declared again (new main resources, new tree lists, their growth adopted): by the reflection epoch's
			// own reserve (ReserveReflection's a_epoch), never mid-frame at the join, where another epoch may hold a recording of the
			// graph as it is.
			bool rebuildPending = false;
			std::shared_ptr<const ComputeProgram> buildDraws;
			rhi::CommandSignaturePtr dispatchSignature;
		};

		std::unique_ptr<org::RenderGraph::IRenderGraphExtension> MakeReflectionExtension(std::shared_ptr<ReflectionResources> a_resources);

		/**
		 * @brief The faces' buckets (ExecuteReflection): per distinct forward pipeline, the LOD slots that draw with it, each slot's
		 * range what its objects can produce (the Z-prepass's bucket capacity); and each pipeline slot's bucket (kNoBucket: none).
		 */
		struct ReflectionPlan
		{
			std::vector<std::uint32_t> map;
			std::vector<ReflectionFrame::Bucket> buckets;
			std::uint32_t draws = 0;  // a face's, all buckets
		};
		void PlanReflectionBuckets(const MainSizing& a_main, std::span<const rhi::PipelineHandle> a_slotPipelines, ReflectionPlan& a_out);

		/** @brief What the reflection epoch's shape is made from (MakeReflectionShape): its resources, reserved for a_plan, and the main's. */
		struct ReflectionShapeInputs
		{
			rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
			IndirectState indirect{};
			std::vector<ReflectionFrame::Bucket> buckets;
			std::shared_ptr<const std::vector<std::uint32_t>> map;  // each pipeline slot's bucket
			// Tree LOD in the faces: its forward pipeline and draw signature, and the scene's shape slots (none: invalid pipeline).
			rhi::PipelineHandle tree{};
			rhi::CommandSignatureHandle treeSignature{};
			std::uint32_t treeShapes = 0;
			// The main rows' tables (the commit's: their current versions; a revision's: the versions it names, RevisionAddress).
			std::uint64_t materialRows = 0, pipelineRows = 0;
			// The reflection's sizing the shape is for: a revision's (Growths::RevisionSizing), else the resources' own.
			const ReflectionSizing* sizing = nullptr;
		};
		std::shared_ptr<ReflectionFrame> MakeReflectionShape(const ReflectionResources& a_resources, const ReflectionShapeInputs& a_in);

		/**
		 * @brief The versions of every versioned buffer (VersionRegistry) as a scene revision names them (R3c): a preparation for the
		 * revision resolves each buffer to its version here (org::IResourceVersions), whatever is current when it runs.
		 */
		struct VersionSet final : org::IResourceVersions
		{
			std::vector<std::pair<const void*, std::shared_ptr<const org::BufferVersion>>> versions;  // by VersionedBuffer::Key
			std::uint64_t changes = 0;  // VersionRegistry::changes when it was taken: the versions are current while that holds
			std::shared_ptr<const org::BufferVersion> Find(const org::VersionedBuffer& a_buffer) const noexcept override;
			/**
			 * @brief Render thread: the registry's buffers' current versions, but a ready growth's where it has one (Growths::Ready):
			 * such a set is not current until its selection adopts them, so it takes a value of its own (VersionRegistry::next).
			 */
			static std::shared_ptr<const VersionSet> Snapshot();
			/** @brief Whether these are the registry's buffers' current versions, every one. */
			bool Current() const;
		};

		/** @brief A scene revision's recording of one epoch prepares for this (host data): the revision's versions and its epoch's shape. */
		struct EpochRevisionData final : org::IHostExecutionData
		{
			std::shared_ptr<const VersionSet> versions;
			RevisionShapes shapes;
			const void* TryGet(std::type_index a_type) const noexcept override
			{
				if (a_type == typeid(RevisionShapes))
					return &shapes;
				if (a_type == typeid(org::IResourceVersions))
					return static_cast<const org::IResourceVersions*>(versions.get());
				return nullptr;
			}
		};

		/** @brief A shadow or occlusion epoch's shapes in a revision: one per view layout it has seen (ShadowViewLayout). */
		struct ShadowVariants
		{
			std::vector<std::shared_ptr<const ShadowFrame>> shapes;
		};
		/** @brief An epoch's recordings for a revision: one per shape (a shadow epoch's variants), by the shapes' order. */
		struct RevisionRecordings
		{
			std::vector<std::shared_ptr<const org::PersistentGraphHost::EpochRecording>> recordings;
		};
		/**
		 * @brief An epoch's part of the selected revision (Impl::RevisionOf): its shape fragment and its recordings. A commit writes the
		 * frame's values into the shape and submits the recording, trusting both (the frame's coverage was decided before the engine
		 * drew: DecideCoverage, DecideShadowCoverage, OcclusionReady).
		 */
		struct EpochRevision
		{
			std::shared_ptr<const org::async::RevisionFragment> shape;
			std::shared_ptr<const RevisionRecordings> recordings;
			explicit operator bool() const { return shape && recordings && !recordings->recordings.empty(); }
		};
	}

	// What was one translation unit's anonymous namespace: its names resolve here as they did there.
	using namespace Draws;

	// a_device: records the batch's copies on the worker as well (StagedUploadBatch::Record), so the commit hands the list over
	// as it is (SubmitWorkerBatch); null leaves them to be recorded at the submission.

	struct IndirectDraws::Impl
	{
		std::shared_ptr<Resources> resources;
		// The main rows the last main commit sent (its payload's kept views): what a deferred growth of their tables is filled with.
		KeptView<MaterialRow> committedMaterialRows;
		KeptView<PipelineRow> committedPipelineRows;
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
		/**
		 * @brief A shadow view of a captured phase this frame's epoch cannot draw: the casters the engine withheld from it are a hole
		 * this frame (counted, flagged). The capability stays: the set does not change with a frame.
		 */
		void ShadowsNotDrawn(IndirectDraws::ShadowStats& a_stats);
		/**
		 * The shadow capability (IndirectDraws::ShadowCapability): the modes DCLF draws and the rasterizer state catalog their
		 * pipelines are built for (PhaseReady's), set up once (UpdateShadowCapability) and changed only by a toggle, a failure, an
		 * occlusion map's first capture, or a defect: a view of a mode or under a state the catalog does not hold (NoteCapability,
		 * flagged "<- PHASES"). Render thread; the set reads the published phases.
		 */
		std::array<bool, kShadowModeCount> readyModes{};
		std::array<ModeRasterStates, kShadowModeCount> readyStates{};
		std::uint64_t shadowReadinessSerial = 0;
		std::atomic<std::uint8_t> shadowCapability{ 0 };
		bool shadowCatalogBuilt = false;
		DXGI_FORMAT shadowTargetFormat = DXGI_FORMAT_UNKNOWN;
		/** @brief The catalog: every solid-fill state of the engine's table its shadow views can draw with, and the cascades' clones. */
		void BuildShadowCatalog();
		/**
		 * @brief The capability's modes or catalog changed: the readiness serial moves and every caster is decided again. a_defect: a
		 * view the capability did not foresee (counted and named, "<- PHASES").
		 */
		void NoteCapability(bool a_defect, std::string a_cause);
		struct CapabilityStats
		{
			std::uint32_t changes = 0;  // every change of the modes or the catalog, setup's included
			std::uint32_t defects = 0;  // of them, a view the capability did not foresee
			std::string lastCause;
		} capabilityStats;
		void CheckCascadeCulling(const PendingView& a_view, const BuildDrawsLatch& a_latch, std::uint32_t a_frame, const ShadowPayload& a_payload);
		/**
		 * @brief Step 6e S1: the shadow payload the frame's epoch committed (the installed publication's, built ahead with it), which the
		 * occlusion epoch and the revision shapes read until the next commit. It holds no publication (the builds ahead drop their
		 * streams: the publication holds them).
		 */
		std::shared_ptr<ShadowPayload> committedShadow;
		std::uint64_t shadowUnbuilt = 0;  // covered frames whose installed publication had no shadow payload (<- UNBUILT), since the last report
		/**
		 * @brief CS_DCLF_REVISION_PARITY, on ParityDue frames: a shadow (a_epoch 2) or occlusion (3) commit's own shape for a_layouts (the
		 * shape parity) and whether the selected revision's variant holds it (ShadowRevisionFor: <- REVISION). It observes only.
		 */
		void CheckShadowRevision(std::uint32_t a_epoch, const std::vector<ShadowViewLayout>& a_layouts, const ShadowPayload& a_payload,
			const ShadowIndirectState& a_indirect, std::size_t a_sunProcesses, const std::shared_ptr<const ShadowFrame>& a_revision, ShadowResources& a_resources,
			SceneStore& a_store);
		std::vector<std::shared_ptr<ShadowPayload>> shadowPayloadPool;  // the builds task's
		std::shared_ptr<ShadowPayload> AcquireShadowPayload();
		std::shared_ptr<const void> shadowExecutionOwner;  // reused by the sky epoch's copy of the shadow records
		/**
		 * @brief The capability's modes (occlusion maps included) with the catalog's states, and the shadow targets' format, which the
		 * builds ahead and the frame's start's shadow lookups are for (UpdateShadowCapability).
		 */
		struct LastShadow
		{
			std::array<bool, kShadowModeCount> modes{};
			std::array<ModeRasterStates, kShadowModeCount> rasterStates{};
			DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
			bool known = false;
			std::uint32_t loggedStale = 0;
		} lastShadow;
		/** @brief Whether the installed shadow payload can be committed by an epoch with the frame's inputs a_frame. */
		bool ShadowAheadUsable(const ShadowPayload& a_payload, const ShadowInputs& a_frame) const;
		void LogStaleShadow(const ShadowInputs& a_built, const ShadowInputs& a_frame);
		/**
		 * @brief The shadow views' placements (BuildShadowPlacements, at setup): every place the engine's shadow maps can take a view -
		 * the sun's cascade slices and their volumetric copies, and each slice of the lights' map whole (a spot light) or as two halves
		 * (a point light's paraboloid pair) - each with its mode and rasterizer state, at a slot of its own (kFirstShadowViewSlot + its
		 * index). The shadow epoch's shape is all of them, whichever views a frame has: a view draws at its placement's slot, and a slot
		 * without one does no work. A view at no placement, under another state, or twice in a frame is a defect
		 * (ShadowNotReady::Placement): the casters withheld from it are a hole, flagged.
		 */
		std::vector<ShadowViewLayout> shadowPlacements;
		bool BuildShadowPlacements();
		/** @brief The placement a captured view drew at (its target, slice and viewport), or SIZE_MAX. */
		std::size_t PlacementOf(const PendingView& a_view) const;
		std::uint32_t placementDefectsLogged = 0;
		// Since the last report: frames whose views were left to the engine as the selected revision had no shape for the placements, or
		// the installed publication no shadow payload (startup, a toggle).
		std::uint64_t shadowUnrecorded = 0;
		/**
		 * @brief The occlusion maps' layout as their last captures drew (CaptureOcclusion, taken whether DCLF draws a map or not): what
		 * a revision makes the occlusion epoch's shapes for (recentOcclusionLayouts, newest first), and what OcclusionReady asks the
		 * selected revision to have a shape for.
		 */
		std::vector<ShadowViewLayout> PredictedOcclusion() const;
		std::vector<std::vector<ShadowViewLayout>> recentOcclusionLayouts;
		static constexpr std::size_t kRecentOcclusionLayouts = 4;
		std::uint64_t occlusionUnrecorded = 0;  // maps left to the engine for want of the revision's shape, since the last report
		/**
		 * @brief The sequence buffers' reserves (render thread, before an epoch): each grown to hold every draw the scene's tracked
		 * objects can produce (SceneDrawBound, kept in drawBound). A bound over the device's max sequence count, or over the sort's rank field,
		 * is a hard failure.
		 */
		/**
		 * @brief Per view rasterizer state, its map row's buckets (BucketsOfRow), kept while what they are made from holds: the
		 * lookups (a map row changes only with their shadow generation) and the published shadow pipeline set (its count and
		 * classes). Render thread: the shadow and occlusion epochs' views.
		 */
		struct
		{
			std::uint64_t lookups = 0;
			std::uint32_t generation = 0;
			std::shared_ptr<const void> version;
			std::array<std::vector<std::optional<RowBuckets>>, kShadowModeCount> rows;  // by mode, then state
		} shadowRowBuckets;
		const RowBuckets& ShadowRowBuckets(std::uint32_t a_mode, std::uint32_t a_state, const Lookups& a_lookups, const ShadowIndirectState& a_indirect);
		DrawBoundStore drawBound;
		/** @brief drawBound brought up to date with a_tables' change log (render thread, the reserves). */
		void UpdateDrawBound(const SceneStore::Tables& a_tables);
		void ReserveMainSequences(const SceneStore::Tables& a_tables);
		/** @brief At the join: the index pool's room for every geometry the tables have (a growth, Growths::Change). */
		void ReserveIndexPool(const SceneStore::Tables& a_tables, std::uint32_t a_generation);
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
		static void ReserveMainLatch(Resources& a_resources, std::uint32_t a_cascades, std::uint32_t a_shadowVolumes, std::uint32_t a_buckets = 0);
		std::uint32_t shadowRowsWanted = 0;  // the last shadow build's (ShadowPayload::rowsWanted)
		ShadowInputs PrepareShadowInputs(const SceneStore& a_store, const ShadowResources& a_resources, const std::array<bool, kShadowModeCount>& a_modeUsed,
			const std::array<ModeRasterStates, kShadowModeCount>& a_modeRasterStates) const;
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
		/** @brief ImportShadowDepth's import of a_texture, through the depth-stencil view a_view, as depth target a_index (a_label: for the log). */
		bool ImportDepthTexture(std::uint32_t a_index, ID3D11Texture2D* a_texture, ID3D11DepthStencilView* a_view, std::uint32_t a_label);

		// The volatile t16+ inputs retain a live import only while their register
		// keeps naming that SRV. This bounds reuse without an age-based import cache.
		struct FrameTextureBinding
		{
			ID3D11ShaderResourceView* view = nullptr;
			GpuTextures::Binding binding;
		};
		std::array<FrameTextureBinding, kTextureRegisters> frameTextureBindings{};
		std::uint32_t frameTextureGeneration = ~0u;
		// The commits' own uploads on the render thread (CommitUploads).
		std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>> commitStagedPool;
		BuildCache mainCache;  // the main list's region, both segments' (BuildMainPayloads)
		/**
		 * @brief Step 6e E3b, the builds ahead: the coordinator builds the main payloads with the publication they draw (BuildAhead),
		 * from what the frame's start posted (AheadContext: the resources, the inputs' frame part, prepared by PrepareMainInputs). Its
		 * journals keep what the payload ring's entries lack (ringHolders, step 6e E5): the frame's producer sends each entry that. The
		 * frames that install the publication commit them (installedDraws); only CS_DCLF_BINDLESS_PARITY builds at the epoch.
		 */
		struct DrawPublication
		{
			std::shared_ptr<const SceneStore::Tables> tables;
			std::shared_ptr<const StreamViews> streams;
			std::array<std::shared_ptr<MainPayload>, 2> payloads;  // kAsyncColour, kAsyncZPrepass
			std::shared_ptr<ShadowPayload> shadow;                 // with its exclusions (step 6e S1)
		};
		struct AheadContext
		{
			bool valid = false;
			std::array<MainInputs, 2> inputs;
			std::array<bool, 2> build{};
			std::shared_ptr<Resources> target;
			// The shadow payload's (step 6e S1): its inputs' frame part (PrepareShadowInputs for the last epoch's views) and resources.
			bool shadow = false;
			ShadowInputs shadowInputs;
			std::shared_ptr<ShadowResources> shadowTarget;
		} aheadContext;  // render thread at the frame's start, read by the coordinator
		// The fits the builds ahead are made against (the main builds', the shadow build's), posted with aheadContext; changes counted.
		std::array<SceneFit, 2> postedFit{};
		std::array<std::uint32_t, 2> postedRows{};  // the main material and pipeline rows' capacities (MainBuild::ResolvePair's rowsFit)
		std::uint64_t postedFitSerial = 0;
		void PostFit();
		std::vector<std::shared_ptr<MainPayload>> payloadPool;  // the builds' task's
		/**
		 * @brief A publication's draws as its builds' task makes them (BuildAhead): one task at a time, in publication order, on
		 * the preparation pool, never joined - the frame's start installs a publication only once they are done (DrawsReady).
		 */
		struct AheadSlot
		{
			std::atomic<bool> done{ false };
			std::shared_ptr<const DrawPublication> result;
		};
		std::uint64_t aheadKicked = 0;               // the coordinator's
		std::atomic<std::uint64_t> aheadDone{ 0 };   // the last task done
		/** @brief The builds' task: a publication's stream views and main payloads (on the pool, in order). */
		/**
		 * @brief CS_DCLF_PERSISTENT_PARITY (U3): every input of a publication's payloads against its object's view mask (ViewMaskOf), and
		 * every input of a list against the list's view types (a list holding an object the mask leaves out of it). Run by the builds
		 * ahead (one at a time); the report takes the counts.
		 */
		void CheckViewMasks(const DrawPublication& a_draws, const SceneStore::Tables& a_tables);
		struct ViewMaskParity
		{
			std::atomic<std::uint64_t> checks{ 0 }, inputs{ 0 }, differ{ 0 };
			// The first that differed: (list << 56) | (object << 24) | expected mask; and its mask.
			std::atomic<std::uint64_t> first{ ~0ull }, firstMask{ 0 };
		} viewMaskParity;
		std::shared_ptr<const DrawPublication> RunAhead(std::shared_ptr<const SceneStore::Tables> a_tables, const AheadContext& a_context, const Lookups& a_lookups,
			std::uint32_t a_generation, std::uint32_t a_frame, std::shared_ptr<const SunCandidates> a_sunCandidates, std::shared_ptr<const SunCandidates> a_lightCandidates);
		/** @brief Every builds' task kicked done (teardown, the toggle, a load screen). */
		void WaitAhead();
		/** @brief Teardown, the toggle, a load screen: the fade write-back task done (never per frame). */
		void WaitFadeWriteBack();
		std::shared_ptr<const DrawPublication> installedDraws;  // the frame's
		/**
		 * @brief The pipeline slots the installed publication's members can draw with: its tables'. A revision that covers it was made at or
		 * after its commit, so its latch holds them (ReserveMainLatch, ReserveReflection); the newest tables may name more, which no member
		 * of the frame uses.
		 */
		std::uint32_t InstalledPipelineSlots() const
		{
			return installedDraws && installedDraws->tables ? static_cast<std::uint32_t>(installedDraws->tables->pipelines.size()) : 0u;
		}
		/**
		 * @brief CS_DCLF_REVISION_PARITY, at a latch clamp: whether a member of the installed publication draws with a pipeline slot at or past
		 * a_slots (its draws are dropped). A slot only a non-member names (an object added since the revision's join, a render-thread
		 * republish's tables) loses nothing.
		 */
		bool MemberPastSlots(std::uint32_t a_slots) const
		{
			if (!installedDraws || !installedDraws->tables)
				return false;
			for (const auto& object : installedDraws->tables->objects)
				if ((object.flags & kObjectMember) && object.pipelineIndex != Lookups::kNone && object.pipelineIndex >= a_slots)
					return true;
			return false;
		}
		/**
		 * @brief Step 6e E4: the payload ring. Each frame the epochs read one entry - the installed publication's payload buffers
		 * (object records, extras rows, geometry table, the rows' tables, each segment's inputs) - which the frame's producer
		 * (FrameValues' job, its other uploads) brings up to that publication on the dedicated uploader before it signals the frame's
		 * wait: each buffer is sent what changed since the version it holds. An entry is reused once the frame that last read it is
		 * done on the GPU. The epochs name it by values (the latches, the frame record): no recording binds it.
		 */
		static constexpr std::uint32_t kPayloadRing = 4;
		struct RingPart
		{
			std::shared_ptr<org::Buffer> buffer;
			std::uint64_t capacity = 0;  // elements
			std::uint32_t srvIndex = 0;
			std::uint64_t address = 0;
			std::uint64_t held = 0;  // the version it holds (the producer's)
		};
		struct RingEntry
		{
			RingPart objects, extras, geometries, materialRows, pipelineRows;
			RingPart inputs;  // the main list, both segments' (U4a)
			// The shadow payload's (step 6e S2): its material rows and each mode's inputs (the occlusion maps' included).
			RingPart shadowRows;
			std::array<RingPart, kShadowModeCount> shadowInputs;
			org::PersistentGraphHost::GpuPoint reuse;  // the last frame that read it
		};
		std::array<RingEntry, kPayloadRing> payloadRing;
		std::uint64_t payloadRingSeq = 0;
		/** @brief What a frame's epochs read of the ring: its entry's values, for the payloads of its publication. */
		struct RingFrame
		{
			bool valid = false;
			std::uint32_t entry = 0;
			std::shared_ptr<const DrawPublication> draws;
			std::uint32_t objectsIndex = 0, extrasIndex = 0, geometriesIndex = 0;
			std::uint32_t inputsIndex = 0;  // the main list's
			std::uint64_t materialRows = 0, pipelineRows = 0;
			bool shadow = false;  // the entry holds the publication's shadow payload
			std::uint64_t shadowRows = 0;
			std::array<std::uint32_t, kShadowModeCount> shadowInputsIndex{};
		};
		RingFrame ringFrame;
		// The last Z-prepass commit's (the reflection draws from the frame before's depth inputs): its entry, when it read one.
		RingFrame ringDepth;
		std::uint32_t ringDepthInputs = 0;
		struct RingStats
		{
			std::uint64_t frames = 0, grown = 0, bytes = 0, runs = 0, committed = 0, shadowCommitted = 0;
		} ringStats;
		std::atomic<std::uint64_t> ringBytes{ 0 }, ringRuns{ 0 };
		// By buffer (RingPartIndex), for the report.
		enum RingPartIndex : std::size_t
		{
			kRingObjects,
			kRingExtras,
			kRingGeometries,
			kRingMaterialRows,
			kRingPipelineRows,
			kRingResident,
			kRingFrameInputs,
			kRingShadowRows,
			kRingShadowInputs,
			kRingParts
		};
		std::array<std::atomic<std::uint64_t>, kRingParts> ringPartBytes{};
		/**
		 * @brief Step 6e E5: the versions the ring's entries hold of each journal (their producer sets them as it queues an entry's
		 * uploads), the oldest of which the builds keep changes back to (ChangeJournal::BeginBuild): each entry is sent what changed
		 * since it was last filled, a few frames back, never everything. The scene buffers (the shadow commits', a fallback's) are
		 * holders too, uncounted: they are a frame behind at most in steady play, and one that falls below is sent everything.
		 */
		struct RingHolders
		{
			KeptHolders<kPayloadRing> objects, extras, geometries, materialRows, pipelineRows;
			KeptHolders<kPayloadRing> resident;  // the main list's region
			KeptHolders<kPayloadRing> shadowRows;
			std::array<KeptHolders<kPayloadRing>, kShadowModeCount> shadowInputs;
		} ringHolders;
		// The entry the frame's shadow commit read (none: its own buffers), which the occlusion epoch's latches name too.
		RingFrame ringShadow;
		/** @brief The ring's values into a shadow or occlusion view's latch, for its mode (none: the views' own buffers). */
		static void ShadowRingLatch(const RingFrame& a_ring, std::uint32_t a_mode, BuildDrawsLatch& a_latch)
		{
			if (!a_ring.valid || !a_ring.shadow || a_mode >= kShadowModeCount)
				return;
			a_latch.payloadValid = 1;
			a_latch.inputsIndex = a_ring.shadowInputsIndex[a_mode];
			a_latch.geometriesIndex = a_ring.geometriesIndex;
			a_latch.materialRowsLo = static_cast<std::uint32_t>(a_ring.shadowRows);
			a_latch.materialRowsHi = static_cast<std::uint32_t>(a_ring.shadowRows >> 32);
			a_latch.pipelineRowsLo = a_latch.pipelineRowsHi = 0;
		}
		/** @brief Whether a commit of a_payload reads the frame's ring entry (an installed payload, the entry filled for it). */
		bool RingFor(const MainPayload& a_payload, std::size_t a_job) const
		{
			return ringFrame.valid && !a_payload.foreignRows && ringFrame.draws && ringFrame.draws->payloads[a_job].get() == &a_payload;
		}
		/** @brief The ring's values into a culling latch (none: the pass's own buffers). */
		static void RingLatch(const RingFrame& a_ring, BuildDrawsLatch& a_latch)
		{
			if (!a_ring.valid)
				return;
			a_latch.payloadValid = 1;
			a_latch.inputsIndex = a_ring.inputsIndex;
			a_latch.geometriesIndex = a_ring.geometriesIndex;
			a_latch.materialRowsLo = static_cast<std::uint32_t>(a_ring.materialRows);
			a_latch.materialRowsHi = static_cast<std::uint32_t>(a_ring.materialRows >> 32);
			a_latch.pipelineRowsLo = static_cast<std::uint32_t>(a_ring.pipelineRows);
			a_latch.pipelineRowsHi = static_cast<std::uint32_t>(a_ring.pipelineRows >> 32);
		}
		MainRows fallbackRows;
		/** @brief Whether the installed payload can be committed by an epoch with the frame's inputs a_frame. */
		bool AheadUsable(const MainPayload& a_payload, const MainInputs& a_frame, const Resources& a_resources) const;
		// The scene tables (SceneBuffers) and their stores, every epoch's: the builds ahead run one at a time, in publication order.
		std::shared_ptr<SceneBuffers> scene;
		// The frame's tree LOD decision (DecideTreeLod), which the depth commit draws on; and the commits that could not.
		bool treeLodOwned = false;
		std::uint32_t treeLodMissed = 0;
		// Strict epochs: the frame's reflection faces are DCLF's (DecideCoverage): its claims stand and the faces' cube is imported into
		// the graph as built. Decided at BeginSceneFrame, before the engine renders the faces.
		bool reflectionCovered = true;
		// The water reflection's faces (Reflection.cpp; dclf-lod.md, "Water reflections"): their targets as the last face had them,
		// and the forward programs and pipelines of the LOD they draw (PrepareReflection); faces captured since the last report.
		struct ReflectionState
		{
			ForwardTargets targets;
			std::uint32_t lodSlots = 0, programsReady = 0, pipelinesReady = 0;
			bool treeReady = false;
			// Per pipeline slot, its forward pipeline when it is a LOD slot whose pipeline is built (PrepareReflection): what the faces'
			// map draws, and the reflection phase's readiness (PhaseReady). readinessKey changes with it.
			std::vector<rhi::PipelineHandle> slotPipelines;
			std::uint64_t readinessKey = 0;
			rhi::PipelineHandle treePipeline{};
			bool treeOwned = false;  // the faces' tree LOD is DCLF's this frame (PassCapture::SetReflectionTreeLodOwned)
			// This update's faces (CaptureReflectionFace), until ExecuteReflection takes them.
			struct Face
			{
				bool captured = false;
				std::array<std::byte, kReflectionFaceBlockBytes> perFrame{};
				std::uint32_t perFrameBytes = 0;
				std::array<float, 16> viewProj{};
				RE::NiPoint3 eye;
			};
			std::array<Face, kReflectionFaces> faces;
			winrt::com_ptr<ID3D11Texture2D> cube;  // the cube target the faces render into
			std::uint32_t width = 0, height = 0;
			std::shared_ptr<ReflectionResources> resources;
			bool setupFailed = false;
			// Since the last report: faces captured, updates, epochs and faces drawn, and the updates not drawn by cause.
			std::uint32_t facesCaptured = 0, updates = 0, epochs = 0, facesDrawn = 0;
			std::array<std::uint32_t, 6> skipped{};  // not drawable, not covered (DecideCoverage), no faces, stale inputs, no resources, the epoch failed
			// The scene frame of the last ExecuteReflection, and the faces captured after it in the same frame (none: every update
			// of a frame runs before BeforeShadowMaps).
			std::uint32_t executedFrame = ~0u, lateFaces = 0;
		} reflection;
		/** @brief The reflection's graph resources, created once its targets are known (render thread). */
		bool SetupReflection();
		/** @brief The reflection phase's readiness of an object (PhaseReady): its pipeline slot's forward pipeline is built. */
		bool ReflectionPhaseReady(const SceneStore::Tables& a_tables, std::uint32_t a_slot) const;
		/**
		 * @brief The reflection's resources grown before its epoch: the latch for a_slots pipeline slots and a_buckets buckets, the
		 * sequences for a_draws a face, the faces' tree LOD lists for the scene's shape slots.
		 */
		void ReserveReflection(std::uint32_t a_slots, std::uint32_t a_buckets, std::uint32_t a_draws, bool a_epoch = false);
		/** @brief The engine's cube target imported (again when it changes). */
		bool ImportReflectionCube(ID3D11Texture2D* a_texture);
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
		ExtrasStore extrasStore;
		GeometryStore geometryStore;
		ObjectRecordStore* SceneObjects() { return &objectStore; }
		ExtrasStore* SceneExtras() { return &extrasStore; }
		/**
		 * @brief The per-frame streams (drawcall-limit-fix.md, "The streams leave the builds"): the object records and the extras
		 * rows the tables hold now, updated from the change log and uploaded as the changes since what the scene buffers hold.
		 * Render thread, at every epoch's commit, before its passes: a placement, a palette or a shading value reaches the
		 * epoch that draws it, whichever build it ran with, and no build waits for them. Returns the rows it sent.
		 */
		struct SceneStreams
		{
			std::size_t objects = 0, extraRows = 0, objectBytes = 0, extraRowsSent = 0;
		};
		/**
		 * @brief Render thread: the frame's stream views - the installed publication's (the builds task made them with it), or, with
		 * none installed for the frame's tables, made here while the coordinator and the builds task are idle (they own the stores;
		 * none is made while they run, counted). What the epochs' own builds and a commit that reads no ring entry take (step 6e S3).
		 */
		std::shared_ptr<const StreamViews> StreamsNow();
		std::shared_ptr<const StreamViews> streamViews;  // made here, for the tables of streamViewsKey (released at the frame's end)
		StreamsKey streamViewsKey;
		std::uint64_t streamsRefused = 0;  // views wanted while the coordinator or the builds task ran (since the last report)
		std::shared_ptr<MainPayload> AcquirePayload()
		{
			// An idle payload holds nothing: what it held (its stream views and their tables, its owners) would keep the retirement
			// chain from that publication on (step 6e E3).
			std::shared_ptr<MainPayload> free;
			for (auto& payload : payloadPool)
				if (payload.use_count() == 1) {
					payload->Reset();
					if (!free)
						free = payload;
				}
			return free ? free : payloadPool.emplace_back(std::make_shared<MainPayload>());
		}
		// The payloads the frame's main epochs committed (for the parities), by job.
		std::array<const MainPayload*, 2> committedPayload{};
		std::array<std::uint32_t, 2> committedFrame{};
		// CS_DCLF_BINDLESS_PARITY's payloads, built at the epoch (the parity's builds read the eye the Z-prepass captures).
		std::array<MainPayload, 2> fallbackPayloads;
		// Per main epoch since the last report: covered frames whose installed publication had no payload for the main resources
		// (not submitted, <- UNBUILT).
		std::array<std::uint64_t, 2> unbuilt{};
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
		// Per main epoch (Z-prepass, colour): each frame slot's last captured block, for a frame that lacks it (PackFrameBlocks).
		struct CarriedBlocks
		{
			std::array<std::vector<std::byte>, kConstantBufferRegisters> vs, ps;
		};
		std::array<CarriedBlocks, 2> carriedBlocks;
		std::uint64_t frameSlotsCarried = 0;  // since the last report
		std::uint32_t carriedLogged = 0;      // the slots logged once (bit: slot, +8 colour, +16 pixel)
		/** @brief The build's inputs, snapshotted on the render thread. Without a capture (a job kicked ahead of the epoch) the eye is the replayed one. */
		MainInputs PrepareMainInputs(const Capture* a_capture, bool a_depthOnly, const Resources& a_resources, std::uint32_t a_vsMask, std::uint32_t a_psMask, const SceneStore& a_store);

		// Per main epoch (kAsyncColour, kAsyncZPrepass): the frame slots it last supplied, which the builds ahead are made for (a
		// change shows a frame late: the epoch builds its own meanwhile, AheadUsable), and the stale payloads it logged.
		struct EpochMasks
		{
			std::uint32_t vsMask = 0, psMask = 0;
			bool known = false;
			std::uint32_t loggedStale = 0;
		};
		std::array<EpochMasks, 2> epochMasks;
		// CS_DCLF_CAPTURE_POINT_PARITY (CheckCapturePoint), since the last report: the frames that differ, and how often
		// each binding does.
		struct
		{
			std::uint32_t checks = 0, frames = 0;
			ankerl::unordered_dense::map<std::string, std::uint32_t> differ;
		} captureParity;
		void LogStalePayload(std::size_t a_job, const MainInputs& a_actual, const MainInputs& a_built);
		/** @brief Inside the epoch's preparation: the frame textures and blocks, the uploads, the PassFrame. */
		bool CommitMainPayload(const Capture& a_capture, const FrameBlocks& a_blocks, const MainInputs& a_frame, MainPayload& a_payload,
			const std::shared_ptr<Resources>& a_resources, SceneStore& a_store, IndirectDraws::Stats& a_stats,
			std::vector<std::shared_ptr<const void>>& a_bindingOwners, const EpochRevision& a_revision);
		/**
		 * @brief CS_DCLF_REVISION_PARITY, on ParityDue frames: the main commit's own shape (the shape parity) and whether the selected
		 * revision's shape a_shape holds the frame (versions, pipelines, viewport, capacities, latch): a miss is counted and logged
		 * (<- REVISION). It observes only: the commit writes into a_shape whatever it finds.
		 */
		void CheckMainRevision(std::uint32_t a_epoch, const Capture& a_capture, const FrameBlocks& a_blocks, const MainPayload& a_payload,
			const std::shared_ptr<Resources>& a_resources, SceneStore& a_store, const PassFrame& a_shape);

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
		// The depth commit's scratch: its buckets (PlanZBuckets) and the zeros their count words take.
		ZBucketPlan zBucketPlan;
		std::vector<std::uint32_t> zBucketZeros;

		/**
		 * @brief R3c (a), the shape parity (live mode, counts only). At the scene work's join (MakeRevisionShapes) each main segment's
		 * shape is made as a scene revision would make it, from the revision's inputs and what the segment's last commit captured
		 * (its viewport, its frame blocks' sizes); each commit's own shape is then compared with the revision made in its frame
		 * (an input the commit takes later than the join) and with the one made the frame before (the revision the frame would
		 * select at BeginSceneFrame). Since the last report: per segment, comparisons, the same, no revision to compare with, and
		 * per field (MainShapeField) the differences; and commits whose latched copies were not MainLatchedLayout's of their own
		 * inputs (a commit writing what the layout does not name).
		 */
		struct ShapeParity
		{
			// Per segment, the revisions made at this frame's join and at the frame before's, and those frames.
			std::array<std::array<std::shared_ptr<const PassFrame>, 2>, 2> revisions;
			std::array<std::array<std::uint32_t, 2>, 2> revisionFrames{ { { ~0u, ~0u }, { ~0u, ~0u } } };
			// What the segment's last commit captured, which the next revision's shape is made with.
			std::array<bool, 2> known{};
			std::array<MainViewport, 2> viewport{};
			std::array<FrameBlockSizes, 2> blockSizes{};
			rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
			struct Counts
			{
				std::uint64_t compared = 0, same = 0, missing = 0;
				std::array<std::uint64_t, kMainShapeFields> differ{};
			};
			// Per segment: against the frame's own revision, and against the frame before's.
			std::array<std::array<Counts, 2>, 2> counts{};
			std::array<std::uint64_t, 2> layoutMisses{};
			std::uint32_t logged = 0;
		} shapeParity;
		/** @brief A commit's shape against the revisions (ShapeParity); a_layout: MainLatchedLayout of its own inputs. */
		void NoteShapeParity(std::size_t a_shape, const PassFrame& a_frame, const std::vector<LatchedCopy>& a_layout, std::uint32_t a_frameNumber);
		/**
		 * @brief The reflection epoch's (ShapeParity's): its shape as a revision at the join makes it (MakeRevisionShapes, once an
		 * epoch has run: the heaps), against its commit's, made in the same frame or the frame before.
		 */
		struct ReflectionParity
		{
			std::array<std::shared_ptr<const ReflectionFrame>, 2> revisions;
			std::array<std::uint32_t, 2> revisionFrames{ ~0u, ~0u };
			bool known = false;
			rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
			std::array<ShapeParity::Counts, 2> counts{};  // differ: kShapePipelines (anything)
			std::uint32_t logged = 0;
		} reflectionParity;
		void NoteReflectionParity(const ReflectionFrame& a_frame, std::uint32_t a_frameNumber);
		/**
		 * @brief CS_DCLF_REVISION_PARITY, on ParityDue frames: the reflection commit's own shape (the shape parity) and whether the selected
		 * revision's a_shape holds the frame's faces (<- REVISION). It observes only.
		 */
		void CheckReflectionRevision(const ReflectionFrame& a_shape, const IndirectState& a_indirect, std::uint32_t a_slots, const TreeLodPipelines* a_treeLod,
			std::uint32_t a_frame);
		/**
		 * @brief The shadow and occlusion epochs' (ShapeParity's). Their views come and go with the engine's (a local light's pair,
		 * the cascades' alternating states), so a revision holds a shape per view layout it has seen: at the join each recent shape's
		 * layout is made again from the revision's inputs (MakeShadowShape), and a commit is compared with the one of its layout
		 * ("without one": a layout the revision has not seen, whose views a revision's frame would leave to the engine).
		 */
		struct ShadowParity
		{
			enum Field : std::uint32_t
			{
				kCapacity,
				kBuckets,
				kPush,
				kRows,
				kLatch,
				kLatched,
				kHeaps,
				kPipelines,
				kFields
			};
			static constexpr std::array<const char*, kFields> kFieldNames = { "capacity", "buckets", "push", "rows", "latch", "latched copies", "heaps", "pipelines" };
			bool known = false;
			rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
			// Per epoch (shadow, occlusion): the revisions made at this frame's join and the frame before's, a shape per layout.
			std::array<std::array<std::vector<std::shared_ptr<const ShadowFrame>>, 2>, 2> revisions;
			std::array<std::array<std::uint32_t, 2>, 2> revisionFrames{ { { ~0u, ~0u }, { ~0u, ~0u } } };
			struct Counts
			{
				std::uint64_t compared = 0, same = 0, missing = 0;
				std::array<std::uint64_t, kFields> differ{};
			};
			std::array<std::array<Counts, 2>, 2> counts{};
			std::uint64_t layoutMisses = 0;
			std::uint32_t logged = 0;
		} shadowParity;
		void NoteShadowParity(bool a_occlusion, const ShadowFrame& a_frame, const std::vector<LatchedCopy>& a_layout, std::uint32_t a_frameNumber);

		/**
		 * @brief R3c (b), the scene revisions (SceneRevision.cpp), assembled and selected but not drawn with yet. At the scene work's
		 * join (AssembleRevision) a draft names the versions (VersionSet), each epoch's shape (MakeRevisionShapes': a fragment kept
		 * while the shape is the same) and each epoch's recording, which requires exactly its shape and the versions: an epoch whose
		 * shape and versions are unchanged inherits the last one's, and any other is recorded for the revision on ORG's host thread
		 * (PersistentGraphHost::RequestEpochRecording; a live epoch's recording is made but never admitted). BeginSceneFrame takes the
		 * newest complete revision (SelectRevision). Render thread, but for the recordings' completions (the host's thread).
		 */
		struct SceneRevisions
		{
			static constexpr std::uint32_t kEpochs = 5;  // kDepthShape, kColourShape, shadow, occlusion, reflection
			static constexpr std::uint32_t kVersionsSlot = 0, kShapeSlot = 1, kRecordingSlot = kShapeSlot + kEpochs, kSlots = kRecordingSlot + kEpochs;
			static constexpr std::array<RenderGraphRuntime::Segment, kEpochs> kSegments = { RenderGraphRuntime::Segment::ZPrepass, RenderGraphRuntime::Segment::MainOpaque,
				RenderGraphRuntime::Segment::ShadowView, RenderGraphRuntime::Segment::SkyOcclusion, RenderGraphRuntime::Segment::Reflection };
			static constexpr std::array<const char*, kEpochs> kNames = { "Z-prepass", "colour", "shadow", "occlusion", "reflection" };
			org::async::RevisionAssembler assembler{ kSlots };
			std::uint64_t versionChanges = ~0ull;  // VersionRegistry::changes the versions fragment was made at
			std::uint64_t graphBuilds = ~0ull;     // and the host's graph build (PersistentGraphHost::BuildGeneration)
			std::uint64_t growthStamp = ~0ull;     // and Growths::stamp (the ready growths it names)
			// G2: a growth still pending at this join (Growths::Settle): no revision is sealed, so the commit's claims wait (SetApplicable).
			bool growthPending = false;
			std::uint64_t growthWaits = 0;  // joins that sealed nothing for it, since the last report
			// Per sequence (a ring), the scene frame whose join made it: the selected revision's age.
			std::array<std::pair<std::uint64_t, std::uint32_t>, 64> madeAt{};
			std::uint64_t selected = 0;
			// Since the last report: drafts sealed, published, selections, the frames between a selected revision's join and its
			// selection; per epoch the shapes that changed, the recordings requested, refused (no async epochs), recorded (a request
			// whose every shape is recorded) and failed, and the shapes recorded.
			std::uint64_t sealed = 0, published = 0, selections = 0, selectedAge = 0, versionSets = 0, sealFailures = 0;
			struct Epoch
			{
				std::uint64_t changed = 0, requested = 0, refused = 0;
				std::atomic<std::uint64_t> recorded = 0, failed = 0, shapes = 0;
			};
			std::array<Epoch, kEpochs> epochs;
			std::atomic<std::uint32_t> failuresLogged = 0;
			// The selected revision, for the frame's epochs (SelectRevision), and the scene frame whose join made it.
			org::async::RevisionAssembler::Lease active;
			std::uint32_t activeFrame = ~0u;
			// The frame of the last revision sealed, and of the commit whose set the frame's claims are (SetApplicable, NoteSetApplied).
			std::uint32_t sealedFrame = ~0u, claimsFrame = ~0u;
			std::uint64_t setsHeld = 0;  // publications passed over at a frame's start: their commit's revision not selected yet (since the last report)
			// Per epoch since the last report: commits that submitted the selected revision's recording, and what the revision parity
			// (CS_DCLF_REVISION_PARITY) found the revision's shape lacked for the frame, by kind (NoteRevisionMiss).
			enum Miss : std::uint32_t
			{
				kNoRevision,
				kNoRecording,
				kVersions,
				kShape,
				kPipelines,
				kViewport,
				kCapacity,
				kLatch,
				kMisses
			};
			static constexpr std::array<const char*, kMisses> kMissNames = { "no revision", "no recording", "versions moved", "shape differs", "pipelines moved",
				"viewport moved", "draws past its capacity", "values past its latch" };
			std::uint64_t latchedMisses = 0;  // values a commit writing into a revision's shape staged: its latched copies lacked them
			// Per epoch since the last report: commits that found no shape or recording in the selected revision (not submitted: a
			// coverage decision missed it), values past the revision's latch (clamped: dropped), and the revision parity's checks.
			std::array<std::uint64_t, kEpochs> unrevised{}, latchClamped{}, parityChecks{};
			struct Coverage
			{
				std::uint64_t covered = 0;
				std::array<std::uint64_t, kMisses> missed{};
			};
			std::array<Coverage, kEpochs> coverage{};
			// Strict epochs: the frame's coverage (DecideCoverage, BeginSceneFrame): per epoch, whether the selected revision has its
			// recordings, of the graph as built now; the main epochs' decide the frame's claims (uncovered: withdrawn). And the graph
			// builds made at the frame's build point (BuildPoint), and the frames withdrawn, since the last report.
			std::array<bool, kEpochs> covered{};
			bool explicitBuilds = false;
			std::uint64_t builds = 0, withdrawn = 0, ticketsReleased = 0;
			std::array<std::uint64_t, kEpochs> uncovered{};
			// Per epoch, the inputs (versions, shape) whose recording failed: not asked for again until they change.
			std::array<std::pair<std::shared_ptr<const org::async::RevisionFragment>, std::shared_ptr<const org::async::RevisionFragment>>, kEpochs> failedFor;
		} revisions;
		/** @brief The scene work's join: the revision of MakeRevisionShapes' shapes, sealed (SceneRevisions). */
		void AssembleRevision(std::uint32_t a_frame);
		/**
		 * @brief CS_DCLF_REVISION_PARITY: the selected revision's shape fragment and recordings for epoch a_epoch, when it has them and its
		 * versions are current; else false, the miss counted (NoteRevisionMiss).
		 */
		bool ActiveRevision(std::uint32_t a_epoch, std::shared_ptr<const org::async::RevisionFragment>& a_shape, std::shared_ptr<const RevisionRecordings>& a_recordings);
		/**
		 * @brief The selected revision's shape and recordings for epoch a_epoch, trusted (invariant 5): nothing of the frame is checked
		 * against them. Empty (counted, <- UNREVISED) only when a coverage decision let a frame through without them: the epoch is
		 * then not submitted.
		 */
		EpochRevision RevisionOf(std::uint32_t a_epoch);
		void NoteRevisionMiss(std::uint32_t a_epoch, std::uint32_t a_miss) { ++revisions.coverage[a_epoch].missed[a_miss]; }
		/**
		 * @brief Strict epochs: whether the frame's selected revision covers epoch a_epoch (DecideCoverage) and the graph it was recorded
		 * on still runs (no rebuild requested since: explicit builds keep the graph as built until the next frame's build point). An
		 * epoch that is not covered is the engine's, decided before the engine draws its work, and is not submitted. True without
		 * revisions.
		 */
		bool EpochCovered(std::uint32_t a_epoch) const;
		void SubmitRevisionRecording(std::uint32_t a_epoch, const RevisionRecordings& a_recordings, std::size_t a_index);
		/**
		 * @brief R3c: the selected revision's variant of a shadow or occlusion epoch's shape (a_epoch 2 or 3) that covers the frame, or
		 * null (the miss counted): the variant of the commit's own shape's view layout, with the same heaps, pipeline layout and map
		 * rows, push addresses and slots' buffers, a latch that holds the frame's states, key slots and a_sunProcesses, and capacities
		 * that hold the payload's draws. a_variant: its index among the revision's variants (and recordings).
		 */
		std::shared_ptr<const ShadowFrame> ShadowRevisionFor(std::uint32_t a_epoch, const ShadowFrame& a_own, const ShadowPayload& a_payload, const ShadowIndirectState& a_indirect,
			std::size_t a_sunProcesses, std::shared_ptr<const RevisionRecordings>& a_recordings, std::size_t& a_variant);
		/**
		 * @brief Whether the selected revision's pipeline set holds every pipeline the frame's claims draw with, though a newer set
		 * is published: the sets only append (DrawPipelines' versions), and the claims were committed before the revision was made.
		 */
		bool RevisionHoldsClaims() const;
		std::string RevisionReport();
		/**
		 * @brief What the revisions' shapes are made from (MakeRevisionShapes), hashed: the growths, the pipeline sets, the heaps, the
		 * toggles, the lookups, the latches and their layouts, the main commits' viewport and frame blocks, the shadow placements and the
		 * occlusion maps' layouts, the scene's casting bound, the reflection's resources and pipelines. The shapes are made again only
		 * when it moves.
		 */
		enum ShapesKeyGroup : std::size_t
		{
			kKeyGrowths,
			kKeyPipelines,
			kKeyLookups,
			kKeyMain,
			kKeyShadow,
			kKeyBound,
			kKeyReflection,
			kKeyGroups
		};
		static constexpr std::array<const char*, kKeyGroups> kKeyGroupNames = { "growths", "pipelines", "lookups", "main latch and captures", "shadow latch and layouts",
			"casting bound outgrown", "reflection" };
		using ShapesKey = std::array<std::uint64_t, kKeyGroups>;
		ShapesKey RevisionShapesKey(const IndirectState& a_indirect, const ShadowIndirectState& a_shadowIndirect, const rhi::DescriptorHeapHandle& a_resourceHeap,
			const rhi::DescriptorHeapHandle& a_samplerHeap) const;
		ShapesKey revisionShapesKey{};
		std::array<std::uint64_t, kKeyGroups> shapesMadeBy{};  // since the last report: makes by the group whose inputs moved
		bool shapesKeyUnchanged = false;  // this join's key is the last made's (a parity make: any shape it changes is the key's miss)
		// Since the last report: joins that made the shapes, that kept the last ones, and per epoch the shapes a parity make changed
		// under an unchanged key (<- SHAPE KEY).
		std::uint64_t shapesMade = 0, shapesKept = 0;
		std::array<std::uint64_t, SceneRevisions::kEpochs> shapeKeyMisses{};
		/**
		 * @brief Whether the last made shadow and occlusion shapes hold a_bounds: every view's capacity and buckets at least what the
		 * bound needs of them, and no row at the bucket words (TrimmedRow orders such a row by the draws). Their capacities only grow
		 * (SizeShadowBuckets, GrowCapacity from the last made), so a shape made again from a bound they hold is the same shape.
		 */
		bool ShadowShapesHold(const ShadowBounds& a_bounds) const;
		// The casting bound's version last checked against the shapes (drawBound.shadowVersion), and the times it outgrew them: what the
		// shapes key names of the bound (kKeyBound), so a move of the bound alone keeps the shapes.
		std::uint64_t shadowBoundChecked = ~0ull, shadowBoundOutgrown = 0;

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
			std::array<float, 4> worldCamera{};
			std::vector<std::array<float, 3>> nodeCentres;  // the nodes' world bound centres, read with them
			std::vector<std::string> nodeNames;
			std::vector<std::uint32_t> nodeFlags;
			// What FadeStateCS was given that frame: the list blocks, each root's list and the radius it read (the sun entry row).
			std::vector<std::byte> visibility;
			std::vector<std::uint32_t> lists;
			std::vector<float> radii, nodeRadii;  // the engine's world root camera when the nodes were read: position, lodAdjust
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
			// Per object: bit 0 in the frame's set (the main phase), bit 1 bound, bit 2 alpha tested, bit 3 its record's kObjectMember
			// disagrees with the set.
			std::vector<std::uint8_t> flags;
			std::vector<const RE::BSGeometry*> geometry;
			// The published tables the snapshot read (render thread): held, they keep every geometry they name alive (the retirement
			// chain), so the readback frames later reads them without asking the scene lane's live state.
			std::shared_ptr<const SceneStore::Tables> tables;
			// The frustum stamps (the fade test's drops), and per set member whether its live bound is inside the depth build's
			// frustum (the engine's verdict for it, occlusion and fading aside), with that bound: a rejection of one inside is not
			// the culling's to make.
			winrt::com_ptr<ID3D11Buffer> frustumStaging;
			std::vector<std::uint8_t> inView;
			std::vector<std::array<float, 4>> bound;
			// Per object, its geometry's phases in the claims the registration hooks withhold by (PassCapture::CurrentSet): the main
			// claim must be the frame's set exactly, or an object is drawn by nobody (claimed, not in the set) or twice.
			std::vector<std::uint8_t> claims;
		};
		std::deque<SetParityFrame> setParityFrames;
		std::vector<winrt::com_ptr<ID3D11Buffer>> setParityStaging;  // released stagings, reused
		struct SetParityCounts
		{
			std::uint32_t frames = 0, framesWithDamage = 0, skipped = 0;
			std::uint32_t depthOnly = 0, colourOnly = 0, colourUnpublished = 0, withheldUndrawn = 0, withheldCulled = 0;
			std::uint32_t colourDrawnTotal = 0, depthDrawnTotal = 0;
			std::uint32_t alphaDepthOnly = 0, alphaColourOnly = 0, alphaWithheldUndrawn = 0;
			// Drawn by DCLF outside the set (the engine draws it too: a double draw), and records whose kObjectMember is not the set.
			std::uint32_t outsideDrawn = 0, recordDisagrees = 0;
			std::uint32_t samples = 0;
			// Gaps: a resident object drawn (by DCLF, or outside the set), then withheld and GPU-culled for one to eight frames, then
			// drawn again. Counted, not flagged: an occlusion rejection is against this frame's depth, which only gets nearer, so it is
			// hidden in the final image too (two-phase oscillation behind an occluder that moves between the phases is most of them).
			// By the first gap frame's verdict (occluded retest, rejected), and how many were trees.
			std::uint32_t gaps = 0, gapsRetest = 0, gapsRejected = 0, gapsTree = 0, gapSamples = 0;
			// By length (one, two, longer), by the first gap frame's rejection (kGapClasses), with the live bound in view then,
			// within 60 frames of the geometry's first resident frame, and outside the set before and after.
			std::array<std::uint32_t, 3> gapsByLength{};
			std::array<std::uint32_t, 7> gapsByClass{};
			std::uint32_t gapsInView = 0, gapsNew = 0, gapsAfterNative = 0, gapsBeforeNative = 0;
			// Set members rejected while their live bound was in view: by the frustum (the bound the culling read was not the live
			// one), by the fade test (a fade root DCLF owns, faded out), and how many of those were skinned.
			std::uint32_t rejectedInView = 0, fadeHiddenInView = 0, skinnedInView = 0, inViewSamples = 0;
			// The registration hooks' main claims against the frame's set: claimed and not in it (withheld from the engine and not
			// drawn by DCLF), in it and not claimed (drawn by both).
			std::uint32_t claimedOutside = 0, unclaimedMembers = 0, claimSamples = 0;
		} setParity;
		// Per geometry, for the gap detector: its last frame's state (0 not resident, 1 resident and drawn by DCLF or outside the set,
		// 2 resident, withheld and GPU-culled), and its first resident frame.
		struct GapHistory
		{
			std::uint32_t frame = 0, first = 0;
			std::uint8_t last = 0;
			// The run of withheld, GPU-culled frames in progress: its length, its first frame's class (kGapClasses) and verdict, whether
			// its live bound was in view then, and who had it before the run (1 outside the set, 2 DCLF); drawnBy, the last frame's.
			std::uint8_t run = 0, runClass = 0, runVerdict = 0, runInView = 0, drawnBy = 0, runDrawnBy = 0;
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
