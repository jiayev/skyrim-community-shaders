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
#	include "Features/DrawcallLimitFix/Common/FrameRecordPatches.h"
#	include "Features/DrawcallLimitFix/Draws/GpuResources.h"
#	include "Features/DrawcallLimitFix/Draws/GpuTextures.h"
#	include "Features/DrawcallLimitFix/Scene/LightingConstants.h"
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
		constexpr std::uint32_t kAlternatingMaterialTextureRegister = 11;  // character-light ping-pong render target
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
			// ExecuteIndirect max counts (GrowCapacity): the phase-1/colour draws, and each decal group.
			std::uint32_t drawCapacity = 0;
			std::array<std::uint32_t, kDecalGroups> decalCapacity{};
			std::uint32_t cullMode = 0;
			bool probePixel = false;
			std::uint32_t probeX = 0, probeY = 0;
			std::uint32_t width = 0, height = 0;  // render area: the main pass viewport
			float minDepth = 0.0f, maxDepth = 1.0f;  // its depth range (the engine uses [0, 0.999998])
			rhi::DescriptorHeapHandle resourceHeap{};
			rhi::DescriptorHeapHandle samplerHeap{};
			IndirectState indirect{};

			bool SameShape(const PassFrame& o) const
			{
				return drawCapacity == o.drawCapacity && decalCapacity == o.decalCapacity && cullMode == o.cullMode &&
				       probePixel == o.probePixel && probeX == o.probeX && probeY == o.probeY && width == o.width && height == o.height &&
				       minDepth == o.minDepth && maxDepth == o.maxDepth && SameHandle(resourceHeap, o.resourceHeap) &&
				       SameHandle(samplerHeap, o.samplerHeap) && SameIndirect(indirect, o.indirect);
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
			std::uint32_t padding[2];
		};
		constexpr std::uint32_t kSortSequencesConstantWords = sizeof(SortSequencesConstants) / 4;
		constexpr std::uint32_t kSortSequencesGroup = 64;  // SortSequencesCS.hlsl's

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

			/** @brief Null, logged, when either program could not be created. */
			static std::shared_ptr<DrawSort> Create(rhi::Device a_device)
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
				sort->staging = CreateWords(std::uint64_t(kMaxDraws) * sizeof(DrawSequence) / 4, true, "cs.dclf.sort-staging");
				sort->ranks = CreateWords(kMaxDraws, true, "cs.dclf.sort-ranks");
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

		struct Resources
		{
			std::vector<FrameBuffer> frameBuffers;
			std::shared_ptr<org::Buffer> constants, records;
			// The per-object records the DCLF_BINDLESS builds read, at t127 of every draw of the epoch.
			std::shared_ptr<org::Buffer> objects;
			std::uint32_t objectsIndex = 0;  // its SRV's descriptor heap index
			// The bone palette rows the skinned draws read, at t126 (kBonesBufferRegister).
			std::shared_ptr<org::Buffer> bones;
			std::uint32_t bonesIndex = 0;
			// NPC face shapes' positions, the draws' second stream: the shadow epoch's buffer, this epoch's own copy.
			std::shared_ptr<org::Buffer> facePositions;
			std::uint64_t facePositionsAddress = 0;
			ankerl::unordered_dense::map<std::uint32_t, std::uint64_t> faceUploaded;  // region -> generation (render thread)
			std::shared_ptr<org::Buffer> inputs, geometries, sequences, count;  // BuildDraws: in, in, out, out
			// The Z-prepass segment's draw inputs: each main segment keeps its resident region at the head of its own buffer
			// (the colour segment's is `inputs`), and the version of the region the buffer holds, per segment (0 Z-prepass,
			// 1 colour), written by the commit that uploads it.
			std::shared_ptr<org::Buffer> inputsDepth;
			std::array<std::uint64_t, 2> residentUploaded{};
			// The version of the persistent object records (ObjectRecordStore) `objects` holds, written by the commit that
			// uploads it; 0 for a new buffer, which no version is. Likewise the bone rows (BonesStore) and the geometry slots'
			// draws (GeometryStore).
			TablesHeld tablesHeld;
			// The frame lighting version (SceneStore::Tables::frameLightingVersion) its frame slot holds (kFrameSlotLighting).
			std::uint32_t frameLightingUploaded = 0;
			// The Z-prepass segment's constant blocks and binding records (Step 4 of "Persistent draw state"): each segment
			// keeps its blocks and records across frames in buffers of its own. Per segment (0 Z-prepass, 1 colour): the
			// versions the buffers hold, and the frame textures (t16 and up) as the last commit resolved them.
			// Frame textures are patched into upload copies, never into kept record templates.
			std::shared_ptr<org::Buffer> constantsDepth, recordsDepth;
			std::uint64_t constantsDepthAddress = 0, recordsDepthAddress = 0;
			std::array<std::uint64_t, 2> constantsUploaded{}, recordsUploaded{};
			std::array<std::array<std::uint32_t, kTextureRegisters>, 2> committedFrameTextures{};
			std::shared_ptr<const ComputeProgram> buildDraws;
			winrt::com_ptr<ID3D11Buffer> sequencesD3D11, countD3D11;  // CS_DCLF_BUILD_PARITY readback
			winrt::com_ptr<ID3D11Buffer> visibilityD3D11;              // CS_DCLF_SET_PARITY readback
			std::uint64_t constantsAddress = 0, recordsAddress = 0;
			std::uint32_t recordCapacity = 0;  // entries in `records`, which deduplication makes far fewer
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
			/**
			 * @brief The main camera's visibility feedback (dclf-cull-job-elimination.md, "Phase 2"): a ring of readback
			 * slots the frustum stamps are copied into after the colour segment, a timeline of its own signalled after each
			 * copy completes, and a consumer that decodes completed slots in fence order. Nothing waits for it, and nothing
			 * assumes one frame in flight: a frame with no free slot records no copy.
			 */
			struct Feedback
			{
				enum State : std::uint32_t
				{
					Free,
					Recording,  // armed at the colour commit, not yet prepared
					Submitted,  // its copy is recorded and its fence value reserved
					Decoding,
				};
				struct Slot
				{
					std::shared_ptr<org::Buffer> staging;
					std::atomic<std::uint32_t> state{ Free };
					std::uint64_t fenceValue = 0;
					std::uint32_t frame = 0, stamp = 0, objects = 0;
					std::shared_ptr<void> tag;
				};
				std::vector<std::unique_ptr<Slot>> slots;
				std::shared_ptr<rhi::TimelinePtr> timeline;
				std::atomic<std::uint64_t> fenceCounter{ 0 };
				std::atomic<int> armed{ -1 };
				std::uint32_t cursor = 0;
				std::atomic<std::uint64_t> statArmed{ 0 }, statDropped{ 0 }, statAbandoned{ 0 }, statDecoded{ 0 };
			};
			std::shared_ptr<Feedback> feedback;
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
			// Per frame slot, one BuildDrawsLatch: all BuildDraws dispatches of an epoch share the values.
			std::shared_ptr<org::LatchBlock> latch;
			rhi::CommandSignaturePtr dispatchSignature;
			// The sort by pipeline of phase 1's and the colour segment's sequences (one view); null when it is off.
			std::shared_ptr<DrawSort> sort;
		};

		struct PassBindings
		{
			std::array<org::DeclaredViewToken, kColorTargets> targets{};
			org::DeclaredViewToken depth;
			org::ResourceBindingToken sequences, count, records, constants, objects, bones;
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

			bool SameShape(const ShadowFrame& o) const
			{
				return SameHandle(resourceHeap, o.resourceHeap) && SameHandle(samplerHeap, o.samplerHeap) && indirect.valid == o.indirect.valid &&
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
			std::shared_ptr<org::Buffer> constants, objects, bones, geometries, visibility;
			// The material rows every view's draws name (ShadowMaterialRow), grown with the kept state.
			GrowableRows materialRows;
			// NPC face shapes' positions (SceneStore::Tables::faceStreams), a region per shape, read by the draws as
			// the second vertex stream. A region is uploaded when its snapshot's generation is not the one it holds.
			std::shared_ptr<org::Buffer> facePositions;
			std::uint64_t facePositionsAddress = 0;
			ankerl::unordered_dense::map<std::uint32_t, std::uint64_t> faceUploaded;  // region -> generation (render thread)
			std::array<std::shared_ptr<org::Buffer>, kShadowModeCount> inputs;               // per render mode
			std::array<std::shared_ptr<org::Buffer>, kMaxShadowViews> sequences, count;      // per view slot
			std::array<winrt::com_ptr<ID3D11Buffer>, kMaxShadowViews> countD3D11;             // the counters, read back
			std::uint32_t objectsIndex = 0, bonesIndex = 0;
			TablesHeld tablesHeld;  // as Resources::tablesHeld
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
			// Skylighting's occlusion map: its own epoch's one view (ExecuteSkyOcclusion).
			std::atomic<std::shared_ptr<const ShadowFrame>> skyFrame;
			std::shared_ptr<const ShadowFrame> skyPublished;
			std::uint64_t shapeGenerations = 0;
			// Per frame slot, one BuildDrawsLatch per view slot.
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
			void Reset(std::uint64_t a_capacity = kConstantBytes)
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
			std::uint64_t capacity = kConstantBytes;
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
		constexpr std::uint32_t kFrameSlotCount = kFrameSlotLighting + 1;
		constexpr std::uint64_t kFrameConstantBytes = std::uint64_t(kFrameSlotCount) * kFrameSlotBytes;

		constexpr std::uint64_t FrameSlotOffset(bool a_pixelStage, std::uint32_t a_register)
		{
			return (std::uint64_t(a_pixelStage ? kConstantBufferRegisters : 0u) + a_register) * kFrameSlotBytes;
		}

		inline std::array<std::uint32_t, kFramePushWords> FramePushWords(std::uint64_t a_frameConstants)
		{
			std::array<std::uint32_t, kFramePushWords> words{};
			std::uint32_t word = 0;
			for (const bool pixel : { false, true }) {
				const std::uint32_t mask = pixel ? kFramePushPS : kFramePushVS;
				for (std::uint32_t r = 0; r < kConstantBufferRegisters; ++r) {
					if (!((mask >> r) & 1))
						continue;
					// What BuildMainPayload names under bindless draws: the frame slot, the shared light block at PS b3 and the
					// frame lighting at PS b13 (kFrameLightingRegister).
					std::uint64_t address = a_frameConstants + FrameSlotOffset(pixel, r);
					if (pixel && r == 3)
						address = a_frameConstants + std::uint64_t(kFrameSlotSharedLight) * kFrameSlotBytes;
					else if (pixel && r == kFrameLightingRegister)
						address = a_frameConstants + std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes;
					words[word++] = static_cast<std::uint32_t>(address);
					words[word++] = static_cast<std::uint32_t>(address >> 32);
				}
			}
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
			std::uint64_t constants = 0, records = 0, frameConstants = 0;
			std::uint64_t facePositions = 0;  // the shadow epoch's face positions buffer (kFacePositionVertices float4s)
			std::uint32_t objectsIndex = 0, bonesIndex = 0, recordCapacity = 0;
			const void* identity = nullptr;

			bool operator==(const ResourceAddresses&) const = default;
		};

		/**
		 * @brief The per-object records (BindlessObject) one objects buffer holds, kept across frames (drawcall-limit-fix.md,
		 * "Persistent draw state", Step 3): a record is written again only when the change log names a column it is built
		 * from, and the buffer is sent what changed since the version it holds (KeptArray). One for the main epochs' buffer,
		 * one for the shadow epoch's. Its builds run in frame order - the shadow build, then the Z-prepass, then the colour
		 * build, each joined (or cancelled, which waits) before the next is kicked, on the one worker or inline - so nothing
		 * else writes it meanwhile; `collisions` counts it if anything ever does.
		 */
		struct ObjectRecordStore
		{
			LogCursor cursor;
			MarkedList changedObjects;
			KeptArray<BindlessObject> records;
			std::atomic<std::uint32_t> busy{ 0 };
			// Since the last report.
			std::uint64_t updates = 0, rewritten = 0, resyncs = 0, collisions = 0;
			ParityCounter parity;
		};

		/** @brief A build's view of the object records: the store's, or a full set of its own without one (version 0). */
		using ObjectRecordsOut = KeptView<BindlessObject>;

		/**
		 * @brief The rows one bones buffer holds (Step 5): every palette, current then previous (one capacity further), then
		 * the extras - read straight from the tables' arrays, and journalled as rows, so the buffer is sent the rows changed
		 * since the version it holds. The change log names them: a palette's rows (kChangePalette), its place (kChangeSkin),
		 * an extras block's rows or place (kChangeExtras). A new capacity moves everything past the palettes, so it is a
		 * resync. One for the main epochs' buffer, one for the shadow epoch's; in frame order like ObjectRecordStore.
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
			const std::uint32_t rows = std::min<std::uint32_t>(a_out.Rows(), kMaxBoneRows);
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
			const std::uint32_t rows = std::min<std::uint32_t>(a_out.Rows(), kMaxBoneRows);
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
		 * geometry log (Tables::geometryLog), and sent as the slots changed since the version the buffer holds. One for the
		 * main epochs' buffer, one for the shadow epoch's; in frame order like ObjectRecordStore.
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

		struct MainInputs
		{
			std::uint32_t frameNumber = 0;
			bool depthOnly = false;
			bool dedupParity = false, bindlessParity = false;
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
			// Step 4: the versions of the segment's constants and records its buffers hold, and the frame textures the last
			// commit resolved (Resources::constantsUploaded, recordsUploaded, committedFrameTextures).
			std::uint64_t constantsUploaded = 0, recordsUploaded = 0;
			std::array<std::uint32_t, kTextureRegisters> frameTextures{};
		};

		struct MainPayload
		{
			MainInputs inputs;
			ConstantArena arena;
			std::vector<DrawBindings> records;
			std::vector<std::shared_ptr<const void>> bindingOwners;  // exact roots used by this payload, including clean kept records
			std::vector<DrawSequence> sequences;  // CPU templates of BuildDraws' output
			std::array<std::vector<DrawSequence>, kDecalGroups> decalTemplates;  // by group and slot
			std::vector<DrawInput> inputList;
			GeometryDrawsOut geometryDraws;
			std::vector<SceneStore::Tables::FaceStream> faceStreams;  // the tables', for the commit's uploads
			ObjectRecordsOut objectRecords;  // the DCLF_BINDLESS per-object table
			// Step 4 (PersistentBindings): the segment's constant blocks and binding records, kept across frames. The arena and
			// `records` stay empty; these are uploaded as the ranges changed since the version the buffers hold.
			bool persistent = false;
			KeptView<std::byte> keptConstants;
			KeptView<DrawBindings> keptRecords;  // immutable templates; the commit patches separate upload copies
			std::vector<std::array<std::uint64_t, 2>> patchMasks;  // per record: the frame registers (t64 * i + bit) it reads
			std::uint32_t recordsHeld = 0;
			std::uint64_t blocksWritten = 0, recordsWritten = 0;
			BonesOut bones;                             // the rows: current then previous, then the extras
			// The frame's textures (t16 and up) are the epoch's own descriptor indices, which only the commit
			// can resolve: the build leaves these (record, register) pairs for it.
			std::vector<std::pair<std::uint32_t, std::uint32_t>> framePatches;
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
			std::uint32_t recordParityChecks = 0, recordParityMismatches = 0;
			std::array<double, 7> partMs{};
			// The first draw past its buffers, for the commit to log with the geometry's name.
			struct ShortBuffer
			{
				std::uint32_t object = ~0u;
				std::uint64_t vertexNeeded = 0, indexNeeded = 0;
			} shortBuffer;
			// The worker's build stages its uploads itself (StageMainPayload), so the commit on the render thread
			// only submits the batch and patches the frame textures into the staged records. For the resources
			// it was staged against; a build made on the render thread has none.
			std::shared_ptr<org::runtime::StagedUploadBatch> staged;
			DrawBindings* stagedRecords = nullptr;
			const void* stagedFor = nullptr;
			// The segment's resident region (ResidentRegion): its inputs lead the input buffer, uploaded when residentVersion
			// is not the one the buffer holds (Resources::residentUploaded); inputList follows them.
			KeptView<DrawInput> resident;
			std::uint32_t residentDraws = 0, residentPairs = 0, residentUndrawable = 0, residentResyncs = 0;
			std::uint32_t residentParityChecks = 0, residentParityMismatches = 0, residentMissing = 0;

			void Reset()
			{
				resident.Reset();
				residentDraws = residentPairs = residentUndrawable = residentResyncs = 0;
				residentParityChecks = residentParityMismatches = residentMissing = 0;
				staged.reset();
				stagedRecords = nullptr;
				stagedFor = nullptr;
				arena.Reset();
				records.clear();
				bindingOwners.clear();
				sequences.clear();
				inputList.clear();
				geometryDraws.Reset();
				faceStreams.clear();
				framePatches.clear();
				drawnChanges.clear();
				drawnVersion = drawnBase = 0;
				drawnFull = drawnValid = false;
				objectState.clear();
				objectRecords.Reset();
				bones.Reset();
				persistent = false;
				keptConstants.Reset();
				keptRecords.Reset();
				patchMasks.clear();
				recordsHeld = 0;
				blocksWritten = recordsWritten = 0;
				for (auto& templates : decalTemplates)
					templates.clear();
				decalCount = {};
				skipped = {};
				missingTextures = {};
				missingNext = 0;
				missingVertexConstants = missingPixelConstants = 0;
				decalsDrawn = shortBuffers = deferredTextures = 0;
				bindlessParityChecks = bindlessParityMismatches = recordParityChecks = recordParityMismatches = 0;
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

		struct ShadowInputs
		{
			std::uint32_t frameNumber = 0;
			std::array<bool, kShadowModeCount> modeUsed{};
			// Per mode, the rasterizer states of its views (bit DrawPipelines::ShadowRasterStateId): a caster is
			// an input only when its pipeline is ready under every state of the views that draw its class. Bits
			// 0-15 are the states of the views of ordinary casters, bits 16-31 those of the views of the
			// volumetric lighting copy, which draw the volumetric-only casters alone (kObjectVolumetricOnly).
			std::array<std::uint32_t, kShadowModeCount> modeRasterStates{};
			// The sun's full-frustum culling processes' planes ((normal, constant), inside where
			// dot(normal, p) - constant >= 0) and their active masks, as the full-frustum cull (FUN_141511f30)
			// has just used them: an object whose entry (SceneStore::Tables::sunEntry) is outside every process
			// is no candidate of the sun's cascade culls (kInputOutsideSunEntry).
			std::vector<std::array<float, 4>> sunEntryPlanes;  // 6 per process
			std::vector<std::uint32_t> sunEntryPlaneMasks;     // 1 per process
			// The sun entries the scene store found DCLF could take out of the cascade culls (SunAccumulation): the build
			// turns them into the next frame's exclusion, with the claims.
			std::shared_ptr<const SunCandidates> sunCandidates;
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
			ObjectRecordsOut objects;
			BonesOut bones;
			GeometryDrawsOut geometries;
			std::vector<SceneStore::Tables::FaceStream> faceStreams;  // the tables', for the commit's uploads
			std::uint32_t skippedTexture = 0, skippedPipeline = 0, deferredTextures = 0, deferredPipelines = 0;
			// Occluders of Skylighting's map left out (no record, no pipeline yet): the map is then the engine's this frame.
			std::uint32_t skySkipped = 0;
			// The worker's build stages its uploads itself (StageShadowPayload): everything but the arena's view
			// head, and the records of the first stagedSlots view slots. For the resources it was staged against.
			std::shared_ptr<org::runtime::StagedUploadBatch> staged;
			const void* stagedFor = nullptr;
			std::uint32_t stagedSlots = 0;
			// The worker's build also builds each used mode's claim set (ShadowClaimSet), which the epoch
			// publishes; empty for a build made on the render thread, which builds them at the publish.
			std::array<std::shared_ptr<const PassCapture::ClaimSet>, kShadowModeCount> claims;
			std::shared_ptr<SunExclusion> sunExclusion;  // likewise, from the cascades' mode (BuildSunExclusion)

			void Reset()
			{
				staged.reset();
				stagedFor = nullptr;
				stagedSlots = 0;
				claims = {};
				sunExclusion.reset();
				arena.Reset();
				frameRecord = {};
				materialRows.Reset();
				rowsWanted = waitingRows = 0;
				zerosAddress = sharedDataAddress = featureDataAddress = 0;
				bindingOwners.clear();
				objectRecord.clear();
				for (auto& modeInputs : inputList)
					modeInputs.clear();
				objects.Reset();
				bones.Reset();
				kept = false;
				regionInputs = {};
				membership = {};
				geometries.Reset();
				faceStreams.clear();
				skippedTexture = skippedPipeline = deferredTextures = deferredPipelines = 0;
				skySkipped = 0;
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
			std::uint32_t slot = a_firstSlot;
			for (std::uint32_t i = 0; i < kMaxSkinPartitions && slot < a_tables.geometries.size() && slot < kMaxGeometries; ++i) {
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

		/** @brief The sun's full-frustum processes a shadow view's latch holds (BuildDrawsLatch::sunEntryPlanes). */
		constexpr std::size_t kMaxSunEntryProcesses = 8;

		/**
		 * @brief A shadow input's sun entry (kCullSunEntry): its entry's sphere in the fade row, which BuildDraws tests against the
		 * view's processes (outside every one: no caster of the sun). An entry that is never tested is inside every process.
		 */
		inline void SetSunEntryRow(DrawInput& a_input, const SceneStore::Tables& a_tables, std::size_t a_object)
		{
			const bool entry = a_object < a_tables.sunEntry.size() && a_tables.sunEntry[a_object][3] >= 0.0f;
			if (entry) {
				const auto& sphere = a_tables.sunEntry[a_object];
				a_input.fade[0] = sphere[0];
				a_input.fade[1] = sphere[1];
				a_input.fade[2] = sphere[2];
				a_input.fade[3] = sphere[3];
			} else {
				a_input.fade[0] = a_input.fade[1] = a_input.fade[2] = 0.0f;
				a_input.fade[3] = std::numeric_limits<float>::max();
			}
		}

		/** @brief A depth-segment input's fade test (kObjectFadeTest): its entry root's centre and its fade-out distance. */
		inline void SetFadeRow(DrawInput& a_input, const SceneStore::Tables& a_tables, std::size_t a_object)
		{
			if (!(a_input.flags & (kObjectFadeTest | kObjectHeightTest)) || a_object >= a_tables.fadeDistance.size() || a_object >= a_tables.sunEntry.size())
				return;
			const auto& entry = a_tables.sunEntry[a_object];
			if (entry[3] < 0.0f)
				return;  // no entry root: nothing to measure
			a_input.fade[0] = entry[0];
			a_input.fade[1] = entry[1];
			a_input.fade[2] = entry[2];
			a_input.fade[3] = a_tables.fadeDistance[a_object];
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

		// The GeometryDraw of object a_object's positions, or ~0u: not a face shape, no positions buffer, or past
		// kMaxGeometries. AppendFaceStreams writes them at these indices.
		inline std::uint32_t FaceStreamGeometry(const SceneStore::Tables& a_tables, std::size_t a_object, std::uint64_t a_positions)
		{
			if (!a_positions || a_object >= a_tables.faceStream.size() || a_tables.faceStream[a_object] == kNoFaceStream)
				return ~0u;
			const std::size_t index = std::min<std::size_t>(a_tables.geometries.size(), kMaxGeometries) + a_tables.faceStream[a_object];
			return index < kMaxGeometries ? static_cast<std::uint32_t>(index) : ~0u;
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
				if (a_geometries.SlotCount() + a_out.size() >= kMaxGeometries)
					break;
				GeometryDraw draw{};
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
				auto& uploaded = a_uploaded[stream.region];
				if (uploaded == stream.generation)
					continue;
				a_uploads(a_positions, stream.positions, std::size_t(stream.vertexCount) * 16, std::uint64_t(stream.region) * 16);
				uploaded = stream.generation;
				++count;
			}
			return count;
		}

		// Resolved descriptor heap indices per (material, pipeline) pair. The texture and sampler loops depend on
		// nothing per-object: the material's textures, the pipeline's technique (the shadow mask) and its
		// register usage, plus this frame's shared textures. Running them per draw meant 128 + 16 iterations and
		// a heap lookup each for every one of ~950 draws, when there are only about 150 distinct pairs.
		struct ResolvedBindings
		{
			std::array<std::uint32_t, kTextureRegisters> textures{};
			std::array<std::uint32_t, kSamplerRegisters> samplers{};
			std::vector<std::uint32_t> patchRegisters;  // the frame textures the pair's record needs
			bool texturesOk = false;
			bool samplersOk = false;
			bool deferred = false;
			std::uint32_t missingTexture = 0;  // the register that failed, for the skip sample
			// Deduplication: once nothing in the binding record is per-object, every draw of a (material,
			// pipeline) pair wants the same record, so the pair keeps its index here and the assembly runs once.
			// A pair that could not be assembled keeps the reason instead, because the skip counters are per
			// DRAW and have to be raised for each of them, not once per pair.
			std::uint32_t recordIndex = kNoRecord;
			std::uint32_t skipReason = kNoSkip;
		};

		// The PerGeometry group, packed once per pipeline. Every object on a pipeline shares all of it but five
		// variables (the two world matrices and three shading values), so an object copies this and rewrites
		// only those instead of walking the whole variable table twice.
		struct GeometryTemplate
		{
			std::vector<std::byte> vs;
			std::vector<std::byte> ps;
			GeometryPatchOffsets offsets;
			// With DCLF_BINDLESS nothing in the group is per-object, so the pipeline's objects share one pair of
			// arena blocks instead of allocating, copying and patching a pair each.
			std::uint64_t vsAddress = 0, psAddress = 0;
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
		constexpr std::uint64_t kNoPair = ~0ull;  // a region entry without bindings (a cull-only candidate)
		// What the region leaves the per-frame loop of the input and draw capacity (decals have their own ranges).
		constexpr std::uint32_t kLoopReserve = 2048;

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
			std::vector<std::uint8_t> drawsOf;   // per entry: its sequences, 0 when it cannot be drawn this frame
			struct Pair
			{
				std::uint32_t slot = 0;   // its record's index, stable while any entry uses it
				std::uint32_t count = 0;  // the entries using it
				bool ok = true;           // this build assembled its record
			};
			ankerl::unordered_dense::map<std::uint64_t, Pair> pairs;
			std::vector<std::uint32_t> freeSlots;
			std::uint32_t slotCount = 0;
			ankerl::unordered_dense::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> pipelines;  // pipeline -> (set index, entries)
			MarkedList pending;  // depth: joined slots the colour epoch has not drawn yet
			std::size_t draws = 0;
			std::size_t undrawable = 0;           // entries with no draw this frame
			std::vector<std::uint32_t> touched;  // this build: the slots whose entry it wrote or removed, or whose log entry it read
			// The whole scene (Step 5): every object the loop would draw as one input with its pair's record, not only the residents,
			// and the depth segment's cull-only candidates. Every other live object is the per-frame loop's (loopList: decals,
			// faces, second-stream shapes, what does not fit), or nobody's (a candidate the segment does not submit).
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

		/** @brief A constant block kept across frames in a segment's constants buffer (PersistentBindings). */
		struct PersistentBlock
		{
			std::uint64_t offset = ~0ull;
			std::uint32_t size = 0;
		};

		/**
		 * @brief A segment's constant blocks and binding records, kept across frames (drawcall-limit-fix.md, "Persistent draw
		 * state", Step 4). Each pipeline's and each (material, pipeline) pair's blocks stay at one address in the segment's
		 * constants buffer, and each pair's record at one slot of its records buffer; a block or a record is written again
		 * only when its bytes change, and the commit uploads the ranges written since the version the buffers hold. The
		 * records carry the frame textures (t16 and up) as the last commit resolved them; the commit patches the records
		 * again when one of those changes.
		 */
		struct PersistentBindings
		{
			bool active = false;
			const void* identity = nullptr;
			std::uint64_t constantsBase = 0, recordsBase = 0, constantsCapacity = 0;
			std::uint32_t recordCapacity = 0;
			KeptArray<std::byte> constants;  // journalled by block ranges
			std::array<std::vector<std::uint64_t>, 257> constantFree{};  // freed blocks by 256-byte units
			KeptArray<DrawBindings> records;
			std::vector<std::array<std::uint64_t, 2>> patchMasks;  // per record slot
			std::vector<std::uint32_t> recordFree;
			std::uint64_t blocksWritten = 0, recordsRewritten = 0;  // this build

			static std::uint32_t Units(std::size_t a_bytes) { return static_cast<std::uint32_t>(std::min<std::size_t>((std::max<std::size_t>(a_bytes, 16) + 255) / 256, 256)); }
			void FreeBlock(PersistentBlock& a_block)
			{
				if (a_block.offset != ~0ull)
					constantFree[Units(a_block.size)].push_back(a_block.offset);
				a_block = {};
			}
			/** @brief The block's address with a_bytes of a_data in it (written only when they differ), or 0 when the buffer is full. */
			std::uint64_t Place(PersistentBlock& a_block, const std::byte* a_data, std::size_t a_bytes, std::size_t a_size)
			{
				const std::size_t size = std::max<std::size_t>({ a_size, a_bytes, 16 });
				const std::uint32_t units = Units(size);
				if (a_bytes > std::size_t(units) * 256)
					return 0;
				if (a_block.offset != ~0ull && Units(a_block.size) != units)
					FreeBlock(a_block);
				bool fresh = false;
				if (a_block.offset == ~0ull) {
					auto& free = constantFree[units];
					if (!free.empty()) {
						a_block.offset = free.back();
						free.pop_back();
					} else {
						const std::uint64_t offset = constants.Size();
						if (offset + std::uint64_t(units) * 256 > constantsCapacity)
							return 0;
						constants.Mutable().resize(offset + std::size_t(units) * 256);
						a_block.offset = offset;
					}
					fresh = true;
				}
				a_block.size = static_cast<std::uint32_t>(size);
				const std::size_t blockBytes = std::size_t(units) * 256;
				const auto* held = constants.Get().data() + a_block.offset;
				if (fresh || std::memcmp(held, a_data, a_bytes) != 0) {
					auto* out = constants.Mutable().data() + a_block.offset;
					std::memcpy(out, a_data, a_bytes);
					std::memset(out + a_bytes, 0, blockBytes - a_bytes);
					constants.MarkRange(a_block.offset, blockBytes);
					++blocksWritten;
				}
				return constantsBase + a_block.offset;
			}
			std::uint32_t AcquireRecord()
			{
				if (!recordFree.empty()) {
					const std::uint32_t slot = recordFree.back();
					recordFree.pop_back();
					return slot;
				}
				if (records.Size() >= recordCapacity)
					return kNoRecord;
				records.Mutable().emplace_back();
				patchMasks.emplace_back();
				records.Mark(records.Size() - 1);
				return static_cast<std::uint32_t>(records.Size() - 1);
			}
			void ReleaseRecord(std::uint32_t& a_slot)
			{
				if (a_slot != kNoRecord) {
					recordFree.push_back(a_slot);
					if (a_slot < patchMasks.size())
						patchMasks[a_slot] = {};
				}
				a_slot = kNoRecord;
			}
			void WriteRecord(std::uint32_t a_slot, const DrawBindings& a_record, const std::array<std::uint64_t, 2>& a_patchMask)
			{
				patchMasks[a_slot] = a_patchMask;
				recordsRewritten += records.Set(a_slot, a_record) ? 1u : 0u;
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
		 * @brief What BuildMainPayload derives per (material, pipeline) pair and per pipeline, kept across frames.
		 *
		 * The pair's texture and sampler indices and its packed PerMaterial groups, and the pipeline's packed
		 * PerTechnique groups and PerGeometry template, change only when a slot, a lookup or an evaluated constant
		 * does, and deriving them is most of a build's cost. An entry keeps a copy of every input it was derived
		 * from, member by member (MaterialRecord has padding that nothing writes), and is reused only when this
		 * build's inputs are byte-identical; anything else rebuilds it. So the cache cannot serve a stale value,
		 * and a build with the cache produces the same bytes as one without: CS_DCLF_ASYNC=probe compares the
		 * worker's (cached) build with an uncached inline one every epoch.
		 *
		 * One per epoch kind (colour, Z-prepass), used by one build at a time: the worker's, or the render
		 * thread's when it builds inline (the worker's job for that kind is then done or waited for).
		 */
		struct BuildCache
		{
			struct PackedGroup
			{
				std::size_t size = 0;           // what ConstantGroupSize said, the size the block is allocated with
				std::vector<std::byte> bytes;   // the block's contents as packed (at least 16 bytes)
				bool valid = false;
			};
			struct Pair
			{
				std::vector<std::byte> sources;
				ResolvedBindings resolved;  // the resolution fields only; recordIndex and skipReason are per build
				bool hasResolved = false;
				PackedGroup vs, ps;         // PerMaterial
				// Where the frame's floats (MainInputs::materialPatchedFloats / materialPatchedVSFloats, in that
				// order) sit in the packed groups, as dword offsets or ~0: a reused group gets this frame's values
				// written there.
				std::vector<std::uint32_t> psPatchPositions;
				std::vector<std::uint32_t> vsPatchPositions;
				std::uint32_t lastUsed = 0;
				// PersistentBindings: the pair's PerMaterial blocks and its record's slot, and the versions of everything they were
				// written from (PairKeyOf): while they are the same, the build takes the slot and does nothing else.
				PersistentBlock materialVS, materialPS;
				std::uint32_t recordSlot = kNoRecord;
				std::array<std::uint32_t, 12> cleanKey{};
				bool clean = false;
			};
			struct Pipeline
			{
				std::vector<std::byte> sources;
				PackedGroup techniqueVS, techniquePS;
				GeometryTemplate geometry;  // without its addresses, which are per build
				bool hasGeometry = false;
				std::uint32_t lastUsed = 0;
				// PersistentBindings: the pipeline's technique, PerGeometry template and permutation blocks; the versions they were
				// written from (the constants' and the lookup entry's), and a count of their moves (their pairs' records hold
				// their addresses).
				PersistentBlock techniqueVSBlock, techniquePSBlock, geometryVSBlock, geometryPSBlock, permutationBlock;
				std::uint32_t constantsVersion = 0, techniqueVersion = 0, lookupVersion = 0, addressVersion = 0;
				bool clean = false;
			};
			ankerl::unordered_dense::map<std::uint64_t, Pair> pairs;
			ankerl::unordered_dense::map<std::uint32_t, Pipeline> pipelines;
			std::vector<std::byte> scratch;
			ResidentRegion region;
			PersistentBindings persistent;
			DrawnMarks drawnMarks;
			std::uint64_t pairHits = 0, pairMisses = 0, pipelineHits = 0, pipelineMisses = 0;
			std::uint64_t persistentBuilds = 0, persistentBlocks = 0, persistentRecords = 0, persistentResets = 0;  // since the last report
			std::uint64_t persistentParityChecks = 0, persistentParityMismatches = 0;
			std::uint64_t persistentCleanPipelines = 0, persistentCleanPairs = 0, persistentDirtyPipelines = 0, persistentDirtyPairs = 0;
			std::string persistentParityFirst;

			void Sweep(std::uint32_t a_frame)
			{
				static constexpr std::uint32_t kIdleFrames = 64;
				for (auto it = pairs.begin(); it != pairs.end();) {
					if (a_frame - it->second.lastUsed <= kIdleFrames) {
						++it;
						continue;
					}
					persistent.FreeBlock(it->second.materialVS);
					persistent.FreeBlock(it->second.materialPS);
					persistent.ReleaseRecord(it->second.recordSlot);
					it = pairs.erase(it);
				}
				for (auto it = pipelines.begin(); it != pipelines.end();) {
					if (a_frame - it->second.lastUsed <= kIdleFrames) {
						++it;
						continue;
					}
					for (auto* block : { &it->second.techniqueVSBlock, &it->second.techniquePSBlock, &it->second.geometryVSBlock, &it->second.geometryPSBlock,
							 &it->second.permutationBlock })
						persistent.FreeBlock(*block);
					it = pipelines.erase(it);
				}
			}

			/** @brief A build's start: the persistent state follows the segment's buffers, and its dirty ranges the uploads. */
			void BeginPersistent(const ResourceAddresses& a_addresses, std::uint64_t a_constantsCapacity, std::uint64_t a_constantsUploaded, std::uint64_t a_recordsUploaded)
			{
				auto& state = persistent;
				// What the buffers hold is the version they were sent: what is written from here on is sent alone.
				state.constants.BeginBuild(a_constantsUploaded);
				state.records.BeginBuild(a_recordsUploaded);
				if (!state.active || state.identity != a_addresses.identity || state.constantsBase != a_addresses.constants || state.recordsBase != a_addresses.records ||
					state.recordCapacity != a_addresses.recordCapacity) {
					// New buffers (the resources were recreated), or the first build: every block and record again. The versions
					// go on counting, so no version of the old buffers is taken for one of the new.
					auto constants = std::move(state.constants);
					auto records = std::move(state.records);
					state = {};
					state.constants = std::move(constants);
					state.records = std::move(records);
					state.constants.Clear();
					state.records.Clear();
					state.active = true;
					state.identity = a_addresses.identity;
					state.constantsBase = a_addresses.constants;
					state.recordsBase = a_addresses.records;
					state.constantsCapacity = a_constantsCapacity;
					state.recordCapacity = a_addresses.recordCapacity;
					for (auto& [key, pair] : pairs) {
						pair.materialVS = pair.materialPS = {};
						pair.recordSlot = kNoRecord;
						pair.clean = false;
					}
					for (auto& [key, pipeline] : pipelines) {
						pipeline.techniqueVSBlock = pipeline.techniquePSBlock = pipeline.geometryVSBlock = pipeline.geometryPSBlock = pipeline.permutationBlock = {};
						pipeline.clean = false;
					}
					++persistentResets;
				}
				state.blocksWritten = state.recordsRewritten = 0;
			}
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
		 * @brief CS_DCLF_PERSISTENT_PARITY: a kept build against the same build made the per-frame way (a_reference): every
		 * object either draws must draw in both, with a record that binds the same textures and samplers and constant buffers
		 * holding the same bytes (the kept blocks' sizes are their groups'; an address outside the segment's constants, a frame
		 * slot, must be the same address).
		 */
		void CheckPersistentBindings(const MainPayload& a_kept, const MainPayload& a_reference, const BuildCache& a_cache, std::uint64_t a_constantsBase,
			const std::array<std::uint32_t, kTextureRegisters>& a_frameTextures, std::uint64_t& a_checks, std::uint64_t& a_mismatches, std::string& a_first);

		/**
		 * @brief The main-pass epoch's draws, from the tables and the lookups: pure.
		 *
		 * Everything a draw needs from a service - the pipeline set indices, the shaders' constant tables, the
		 * descriptor indices of textures and samplers - comes from the lookups;
		 * an entry the render thread has not resolved yet defers the draw (deferredTextures), and the frame's
		 * own textures are left as patches for the commit. The per-frame constant blocks are addressed by
		 * their fixed slots (FrameSlotOffset) for the slots the inputs say the commit supplies.
		 */
		void BuildMainPayload(const MainInputs& a_in, const SceneStore::Tables& a_tables, const Lookups& a_lookups, MainPayload& a_out,
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

		/** @brief The technique bits a mode index adds to an object's base technique: none for Skylighting's map, whose are complete. */
		inline std::uint32_t ModeBitsOf(std::uint32_t a_mode)
		{
			return a_mode == kSkyMode ? 0u : ShadowModeBits(PassCapture::kFirstShadowMode + a_mode);
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
		// culls never reach it (ShadowInputs::sunEntryPlanes).
		inline bool OutsideSunEntry(const ShadowInputs& a_in, const SceneStore::Tables& a_tables, std::size_t a_object)
		{
			if (a_in.sunEntryPlaneMasks.empty() || a_object >= a_tables.sunEntry.size())
				return false;
			const auto& entry = a_tables.sunEntry[a_object];
			if (entry[3] < 0.0f)
				return false;
			for (std::size_t process = 0; process < a_in.sunEntryPlaneMasks.size(); ++process) {
				bool outside = false;
				for (std::uint32_t p = 0; p < 6 && !outside; ++p) {
					if (!(a_in.sunEntryPlaneMasks[process] & (1u << p)))
						continue;
					const auto& plane = a_in.sunEntryPlanes[process * 6 + p];
					outside = plane[0] * entry[0] + plane[1] * entry[1] + plane[2] * entry[2] - plane[3] < -entry[3];
				}
				if (!outside)
					return false;
			}
			return true;
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
			std::vector<std::uint32_t> freeSlots;  // a min-heap: the lowest free slot is taken first, so the table stays dense
			std::vector<std::uint32_t> objectRecord;  // per object: its material row, 0 the plain one, kNoRecord, kWaiting
			// CS_DCLF_PERSISTENT_PARITY: the per-frame build's inputs the kept build lacks, by why, and the first of each.
			std::map<std::string, std::uint64_t> missingBy;
			std::map<std::string, std::string> missingFirst;
			std::vector<const RE::BSShaderMaterial*> objectMaterial;  // per object: the material its slot is held for
			struct Mode : KeptRegion
			{
				bool active = false;
				std::uint32_t rasterStates = 0;
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
		void RefreshMaterialLookups(SceneStore& a_store, const SceneStore::Tables& a_tables, std::uint32_t a_frame, const SceneStore::ProjectedTextures& a_projected, Lookups& a_lookups);

		/**
		 * @brief Outside an epoch (render thread): brings resolved material entries up to date where every view
		 * that changed is one GpuTextures already knows. A volatile material's textures can change every frame
		 * - the character light's t11 alternates between two render targets - and a job kicked before the
		 * epoch's own refresh would otherwise be built against the old indices and go stale. Anything that
		 * needs an import is left to the epoch (RefreshMaterialLookups), where the descriptor service is active.
		 */
		void RefreshKnownMaterialTextures(SceneStore& a_store, Lookups& a_lookups);

		/** @brief The shadow epoch's entries: the alpha-tested casters' diffuse textures, and the pipelines of the modes in use. */
		void RefreshShadowLookups(SceneStore& a_store, const SceneStore::Tables& a_tables, const std::array<bool, kShadowModeCount>& a_modeUsed,
			const std::array<std::uint32_t, kShadowModeCount>& a_modeRasterStates, DXGI_FORMAT a_dsvFormat, DXGI_FORMAT a_skyFormat, Lookups& a_lookups);

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
			       a_job.dedupParity == a_epoch.dedupParity && a_job.bindlessParity == a_epoch.bindlessParity && a_job.withholding == a_epoch.withholding &&
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
			return a_job.frameNumber == a_epoch.frameNumber && a_job.modeUsed == a_epoch.modeUsed &&
			       a_job.modeRasterStates == a_epoch.modeRasterStates && a_job.sunEntryPlanes == a_epoch.sunEntryPlanes &&
			       a_job.sunEntryPlaneMasks == a_epoch.sunEntryPlaneMasks && a_job.sunCandidates == a_epoch.sunCandidates &&
			       a_job.addresses == a_epoch.addresses && a_job.sharedData == a_epoch.sharedData && a_job.featureData == a_epoch.featureData &&
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
	void StageShadowPayload(ShadowPayload& a_payload, const ShadowResources& a_resources, std::uint32_t a_slots,
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
		// Skylighting's occlusion map (ExecuteSkyOcclusion): the view as the engine's RenderMask set it up (CaptureSkyOcclusion),
		// the state and format its pipelines are built for, and the frame whose shadow commit uploaded its occluders.
		PendingView skyView;
		std::uint32_t skyCapturedFrame = ~0u;
		std::uint32_t skyRasterState = 0;
		DXGI_FORMAT skyDsvFormat = DXGI_FORMAT_UNKNOWN;
		std::uint32_t skyCommittedFrame = ~0u;
		std::uint32_t skyInputs = 0, skySkipped = 0;
		// CS_DCLF_SHADOW_OWNERSHIP=static: the claim set built from the inputs of a mode, published once per
		// input rebuild (the views of one frame that share a mode share the inputs and the claims).
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
			std::array<std::uint32_t, kShadowModeCount> rasterStates{};
			bool modesKnown = false;
			std::uint32_t views = 0;  // last frame's view count: the record slots the job stages
			std::uint32_t loggedStale = 0;
			std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>> stagedPool;
		} shadowJob;
		ShadowPayload shadowProbePayload;
		/** @brief Grows the shadow material rows' table to what the last build wanted (render thread, before the inputs are taken). */
		void ReserveShadowRows();
		std::uint32_t shadowRowsWanted = 0;  // the last shadow build's (ShadowPayload::rowsWanted)
		ShadowInputs PrepareShadowInputs(const SceneStore& a_store, const ShadowResources& a_resources, const std::array<bool, kShadowModeCount>& a_modeUsed,
			const std::array<std::uint32_t, kShadowModeCount>& a_modeRasterStates) const;
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
		// The persistent object records: the main epochs' buffer's (both segments'), and the shadow epoch's.
		ObjectRecordStore mainObjects, shadowObjects;
		BonesStore mainBones, shadowBones;  // likewise the bone rows
		ShadowKept shadowKept;  // the shadow epoch's inputs and records (Step 6)
		ShadowKept* ShadowKeptState() { return &shadowKept; }
		BonesStore* MainBones() { return &mainBones; }
		BonesStore* ShadowBones() { return &shadowBones; }
		ObjectRecordStore* MainObjects() { return &mainObjects; }
		ObjectRecordStore* ShadowObjects() { return &shadowObjects; }
		GeometryStore mainGeometries, shadowGeometries;  // likewise the geometry slots' draws
		SunExclusionCache sunExclusionCache;  // the shadow builds', in frame order
		GeometryStore* MainGeometries() { return &mainGeometries; }
		GeometryStore* ShadowGeometries() { return &shadowGeometries; }
		std::array<std::uint32_t, 4> decalWords{};  // the count buffer's decal words, uploaded per colour epoch

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
			std::uint32_t framesLeft = 0;
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
		BuildDrawsLatch sunUpload{};  // the colour epoch's last latch: the cascades its BuildDraws tested against

		/**
		 * @brief The colour commit, render thread: arms a free feedback slot for this frame's copy (FeedbackPass),
		 * with the consumer's tag (PrimaryCull's stood-in entries). A slot armed but never prepared is returned first;
		 * with no free slot the frame is dropped, never waited for.
		 */
		void ArmFeedback(Resources& a_resources, std::uint32_t a_frame, std::uint32_t a_objects)
		{
			auto tag = PrimaryCull::Get().TakeFeedbackTag();
			if (!a_resources.feedback || !a_resources.frustum || !tag)
				return;
			auto& feedback = *a_resources.feedback;
			if (const int stale = feedback.armed.exchange(-1, std::memory_order_acq_rel); stale >= 0) {
				auto expected = static_cast<std::uint32_t>(Resources::Feedback::Recording);
				if (feedback.slots[stale]->state.compare_exchange_strong(expected, Resources::Feedback::Free, std::memory_order_acq_rel))
					feedback.slots[stale]->tag.reset();
			}
			const auto count = static_cast<std::uint32_t>(feedback.slots.size());
			for (std::uint32_t i = 0; i < count; ++i) {
				const std::uint32_t index = (feedback.cursor + i) % count;
				auto& slot = *feedback.slots[index];
				auto expected = static_cast<std::uint32_t>(Resources::Feedback::Free);
				if (!slot.state.compare_exchange_strong(expected, Resources::Feedback::Recording, std::memory_order_acq_rel))
					continue;
				feedback.cursor = (index + 1) % count;
				slot.frame = a_frame;
				slot.stamp = a_frame & 0x0FFFFFFFu;  // BuildDrawsLatch::visibilityStamp
				slot.objects = a_objects;
				slot.tag = std::move(tag);
				slot.fenceValue = 0;
				feedback.armed.store(static_cast<int>(index), std::memory_order_release);
				feedback.statArmed.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			feedback.statDropped.fetch_add(1, std::memory_order_relaxed);
		}

		void ReadCullCounters(const std::shared_ptr<Resources>& a_resources, IndirectDraws::Stats& a_stats, const MainPayload& a_payload);

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
