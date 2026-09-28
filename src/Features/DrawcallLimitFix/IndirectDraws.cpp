#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "IndirectDraws.h"

#	include "AsyncWorker.h"

#	include "ConstantMirror.h"
#	include "DrawPipelines.h"
#	include "DrawPipelinesRhi.h"
#	include "EngineStates.h"
#	include "FaceSnapshots.h"
#	include "FrameRecordPatches.h"
#	include "GpuResources.h"
#	include "GpuTextures.h"
#	include "LightingConstants.h"
#	include "PassCapture.h"
#	include "PrimaryCull.h"
#	include "SceneStore.h"
#	include "ShaderPrograms.h"
#	include "ShadowViews.h"
#	include "SunAccumulation.h"
#	include "RE/B/BSShadowLight.h"
#	include "RE/B/BSShadowDirectionalLight.h"
#	include "RE/B/BSCullingProcess.h"
#	include "RE/N/NiCamera.h"
#	include "Toggles.h"
#	include "Switches.h"
#	include "VertexInput.h"

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
#	include <cstddef>
#	include <filesystem>
#	include <fstream>
#	include <iterator>
#	include <map>
#	include <optional>
#	include <cstring>

namespace DCLF
{
	namespace
	{
		constexpr const char* kExtensionId = "cs.dclf.main-opaque";
		constexpr const char* kShadowExtensionId = "cs.dclf.shadow";
		// The shadow views' binding records: one for every draw without alpha testing, and one per material
		// of the alpha-tested casters (their diffuse and texture offset).
		constexpr std::uint32_t kShadowRecordCapacity = 512;  // per view slot (the exterior needs ~160)
		// The views one frame's shadow epoch can hold: the exterior has four (two cascades, twice); an
		// interior with several shadow-casting point lights has two hemispheres per light.
		constexpr std::uint32_t kMaxShadowViews = 16;
		// Render modes 0xD plain, 0xE clamped, 0xF paraboloid, and kSkyMode: Skylighting's occlusion map (render mode 0x1C),
		// whose occluders the frame's shadow build lists too and whose own epoch draws them (ExecuteSkyOcclusion).
		constexpr std::uint32_t kShadowModeCount = 4;
		constexpr std::uint32_t kSkyMode = 3;
		constexpr std::uint32_t kSkyRenderMode = 0x1C;
		// The view slot Skylighting's occlusion map draws through; the shadow views take the others.
		constexpr std::uint32_t kSkySlot = kMaxShadowViews - 1;
		// Key slots a shadow epoch can name (Lookups::shadowSlotKeys): one per caster key and render mode.
		constexpr std::uint32_t kMaxShadowSlots = 1024;
		constexpr std::uint64_t kShadowConstantBytes = 1ull << 20;
		// Fixed slots at the head of the shadow constants: the view's Utility PerTechnique block and its
		// VS_PerFrame block, rewritten per view so that the records built once a frame can point at them.
		// The arena's head holds one slot per view: its PerTechnique block (b0) then its VS_PerFrame copy (b12).
		constexpr std::uint64_t kShadowPerFrameOffset = 256;
		constexpr std::uint64_t kShadowViewSlotBytes = 256 + 1024;
		constexpr std::uint64_t kShadowMaterialBlocksOffset = kShadowViewSlotBytes * kMaxShadowViews;
		// [0] kSHADOWMAPS_ESRAM (cascades, spot lights), [1] kSHADOWMAPS (point and focus lights), and
		// [2] kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM, the volumetric lighting copy: the engine's second draw of
		// each cascade's accumulator, with flag 0x100, draws batch group 15 alone (FUN_1414b44f0) - the passes
		// registered with accumulation hint 8, which are the volumetric-only casters
		// (ShadowReject::VolumetricOnly). DCLF's views of it draw those casters alone (kCullVolumetricOnly).
		// See ExecuteShadowView.
		// [3] depth target 10 as Skylighting swaps it in: its own occlusion map (ExecuteSkyOcclusion).
		constexpr std::uint32_t kShadowDepthTargets = 4;
		constexpr std::uint32_t kSkyDepthTarget = 3;
		constexpr std::uint32_t kMaxDraws = 16384;
		constexpr std::uint64_t kConstantBytes = 48ull << 20;
		// The Z-prepass segment's own constants buffer: its blocks are the colour segment's pipelines' and pairs' (a few MB).
		constexpr std::uint64_t kDepthConstantBytes = 16ull << 20;
		constexpr std::uint64_t kConstantAlignment = 256;  // uniform buffer address alignment (conservative)
		constexpr std::uint32_t kColorTargets = 8;
		constexpr std::uint32_t kMaxGeometries = kMaxDraws;
		// The per-object record table (ObjectRecord) is indexed by a table index, not a draw index, so it
		// covers every candidate the frame tracks rather than only the ones drawn.
		constexpr std::uint32_t kMaxObjects = 32768;
		// Draw inputs and the visibility buffer are sized by the table, not by the draw count: the depth
		// segment submits a cull-only input for every candidate it may not draw, and BuildDraws indexes the
		// visibility word by the object's TABLE index. Sizing either by kMaxDraws is an overrun waiting for
		// a cell with more than kMaxDraws tracked objects.
		constexpr std::uint32_t kMaxInputs = kMaxObjects;
		// Distinct binding records an epoch may hold, and the size of the buffer behind them. Deduplicated
		// they are one per (material, pipeline) pair: 85 behind 727 candidates in the Bannered Mare, 105
		// behind 1616 in Dragonsreach. 2048 is about twenty times the worst seen, and 1.6 MB against the
		// 13.1 MB the undeduplicated buffer needs. Without CS_DCLF_BINDLESS_DRAW there is a record per
		// draw, so the cap has to stay at the draw cap - the record's contents are still per-object then.
		constexpr std::uint32_t kMaxRecordsDeduplicated = 2048;
		// The 32-bit record offset BuildDraws computes (input.y * RecordStride) has to address all of it.
		static_assert(std::uint64_t(kMaxDraws) * sizeof(DrawBindings) < (std::uint64_t(1) << 32));
		constexpr std::uint32_t kNoRecord = ~0u;
		constexpr std::uint32_t kNoSkip = ~0u;
		// BuildDrawsCS's counter words: [0] drawn, [1] culled, [2] tested, [3] engine-culled and gated out,
		// [4] false negatives (the engine kept it, the culling rejected it), [5] rescued.
		constexpr std::uint32_t kCountWords = 28;
		// The sun's bits on the GPU: the colour dispatch's inputs with kObjectSunTest, and those that missed every cascade.
		constexpr std::uint32_t kCountSunTestedWord = 23;
		constexpr std::uint32_t kCountSunMissedWord = 24;
		// The fade test (kObjectFadeTest): the depth segment's first phase's flagged inputs in the frustum, and those it dropped.
		constexpr std::uint32_t kCountFadeTestedWord = 25;
		constexpr std::uint32_t kCountFadeHiddenWord = 26;
		// Decals: the two groups' slot counts, written by the CPU and read by their draws, then the culling's
		// own tallies. Byte offsets must match BuildDrawsCS.hlsl.
		constexpr std::uint32_t kCountDecalGroupWord = 19;  // group 1 at word 19, group 2 at word 20
		constexpr std::uint32_t kCountDecalsCulledWord = 21;
		constexpr std::uint32_t kCountDecalsTestedWord = 22;
		// Byte offsets of the count words the indirect draws read; they must match BuildDrawsCS.hlsl.
		constexpr std::uint64_t kCountDrawnPhaseTwoBytes = 68;
		constexpr const char* kBuildDrawsShader = "DrawcallLimitFix/BuildDrawsCS.hlsl";
		constexpr const char* kHzbShader = "DrawcallLimitFix/HzbCS.hlsl";
		constexpr const char* kSortSequencesShader = "DrawcallLimitFix/SortSequencesCS.hlsl";
		// HzbCS.hlsl's constants: source, target, target size, source size, from-depth, padding.
		constexpr std::uint32_t kHzbConstantWords = 8;

		// The format that reads a depth resource's depth aspect in a shader, as DXGI names it.
		rhi::Format DepthReadFormat(rhi::Format a_format)
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

		// BuildDrawsCS.hlsl's inputs (byte-address buffers).
		struct DrawInput
		{
			std::uint32_t pipelineIndex;  // in the pipeline sets; a shadow input's key slot (Lookups::shadowSlots)
			std::uint32_t recordIndex;    // DrawBindings record
			std::uint32_t geometryIndex;  // GeometryDraw
			std::uint32_t flags;          // object flags, plus kInputDrawable for this epoch
			float boundCentre[3];         // absolute world space; the camera is folded into the matrix
			float boundRadius;
			// Index into the frame's object table. The two segments emit different subsets in different
			// orders, so this is what lets the colour segment look up the visibility the depth segment
			// published for the same object.
			std::uint32_t objectIndex;
			std::uint32_t decalOrdinal;  // decals only: the slot in the group's range
			// Skins of several partitions: bit i draws partition i (Tables::skinPartitions); 0 draws the one
			// geometry. Left 0 by every input that is not such a skin.
			std::uint32_t partitions;
			// A shadow input's second vertex stream: the GeometryDraw holding a face shape's positions (the shadow
			// payload appends them after the geometry slots), or ~0u. The main pass never has one.
			std::uint32_t streamIndex = ~0u;
			// kObjectFadeTest (the depth segment's inputs): the entry root's centre and the fade-out distance
			// (SceneStore::Tables::fadeDistance). Zero on every other input.
			float fade[4] = {};
		};
		static_assert(sizeof(DrawInput) == 64);
		// BuildDrawsCS.hlsl: set on an input the epoch has built a bindings record for. The depth segment
		// submits an input for every candidate so the culling covers them all, but builds records only for
		// the ones it may draw.
		constexpr std::uint32_t kInputDrawable = 1u << 16;
		// A shadow input whose entry is outside the sun's full-frustum processes (OutsideSunEntry): the sun's views
		// skip it (kCullSunEntry); a spot light's views, which share the mode's inputs, do not.
		constexpr std::uint32_t kInputOutsideSunEntry = 1u << 25;
		// Where phase 2 appends its sequences; see BuildDrawsCS.hlsl.
		constexpr std::uint32_t kPhaseTwoSequenceBase = kMaxDraws;
		// The decal ranges: one of kMaxDecalDraws fixed slots per group after phase 2's range. A decal's
		// sequence goes to the slot of its ordinal in the engine's draw order (SceneStore::Tables::
		// decalOrdinal), so the second pass draws decals in that order every frame; a culled one is the
		// same sequence with an index count of zero.
		constexpr std::uint32_t kMaxDecalDraws = 2048;
		// The bones buffer (VS t126): every skinned object's palette rows, current then previous, per epoch.
		// 131,072 float4 rows is 2 MB: each skin keeps its block (SceneStore PlaceBones), so there are holes; the exterior needs
		// ~13,500-18,000 current rows.
		constexpr std::uint32_t kMaxBoneRows = 131072;
		constexpr std::uint32_t kDecalGroups = 2;
		constexpr std::uint32_t kDecalSequenceBase = 2 * kMaxDraws;
		constexpr std::uint32_t kSequenceSlots = kDecalSequenceBase + kDecalGroups * kMaxDecalDraws;

#pragma pack(push, 4)
		struct GeometryDraw
		{
			std::uint64_t vertexBufferAddress;
			std::uint32_t vertexBufferSize;
			std::uint32_t vertexStride;
			std::uint64_t indexBufferAddress;
			std::uint32_t indexBufferSize;
			std::uint32_t indexCount;
			std::uint32_t firstIndex;
			std::uint32_t nextPartition;  // GeometryRecord::nextPartition
		};
#pragma pack(pop)
		static_assert(sizeof(GeometryDraw) == 40);

		// BuildDrawsCS's push constants: only what is fixed for a pass across executions. The execution's own
		// values are in its BuildDrawsLatch (below), so the recorded dispatch never changes with the frame.
		struct BuildDrawsConstants
		{
			std::uint32_t latchIndex;        // the latch block's SRV
			std::uint32_t inputsIndex;
			std::uint32_t geometriesIndex;
			std::uint32_t sequencesIndex;
			std::uint32_t countIndex;
			std::uint32_t recordsAddressLo;
			std::uint32_t recordsAddressHi;
			std::uint32_t recordStride;
			std::uint32_t phaseBits;         // the culling phase in bits 4-7 (CullFlags' phase field)
			std::uint32_t visibilityIndex;   // RWByteAddressBuffer: one uint per object in the tables
			std::uint32_t latchOffset;       // set at record time: the slot's region plus the dispatch's latch
			std::uint32_t frustumIndex;      // RWByteAddressBuffer: per object, the stamp of its last in-frustum frame (depth phase 1 only)
			std::uint32_t hzbIndex;        // 0 when there is no HZB to test against
			std::uint32_t hzbSizePacked;   // mip 0: width in the low 16 bits, height in the high 16
			std::uint32_t hzbMips;
			// The sort by pipeline (SortDraws): the per-pipeline counts, the unsorted sequences and each one's rank among its
			// pipeline's. 0 when the dispatch appends into the sequences themselves (phase 2, or the sort off).
			std::uint32_t sortCountsIndex;
			std::uint32_t sortStagingIndex;
			std::uint32_t sortRanksIndex;
		};
		static_assert(sizeof(BuildDrawsConstants) == 72);
		constexpr std::uint32_t kBuildDrawsConstantWords = sizeof(BuildDrawsConstants) / 4;

		/**
		 * @brief One BuildDrawsCS dispatch's values for one execution, in its slot of a latch block
		 * (org::LatchBlock): written by the render thread in the epoch, read by the dispatch - which takes its
		 * own group count from the first three words (ExecuteIndirect with a dispatch signature).
		 */
		struct BuildDrawsLatch
		{
			std::uint32_t dispatch[3];      // groups x, y, z
			std::uint32_t drawCount;        // inputs to cull
			std::uint32_t cullFlags;        // mode in bits 0-3, RequireNativeVisible at 8, NoNearPlane at 9 (the phase is pushed)
			std::uint32_t visibilityStamp;  // marks the verdicts as this frame's; see BuildDrawsCS.hlsl
			std::uint32_t hzbUvScalePacked; // rendered area over the area the HZB covers, 16-bit fixed point
			std::uint32_t cullPlaneMask;    // which of cullPlanes are tested; 0 for the main camera
			// Row-major, as the shader's float4x4 with mul(M, v), with the camera translation folded in so
			// that an absolute world position projects directly.
			float viewProj[16];
			// A shadow view's caster volume as the engine culls it: the view's culling process's custom planes
			// (NiCullingProcess::customCullPlanes), absolute world space, (normal, constant) with the inside
			// where dot(normal, p) - constant >= 0.
			float cullPlanes[6][4];
			// A shadow view's row of the pipeline map, in bytes into the latch block: the view's inputs name key
			// slots, and the row holds each slot's pipeline under the view's rasterizer state. 0 when the inputs
			// name pipelines themselves (the main camera).
			std::uint32_t pipelineMapOffset;
			// The main colour pass only: kSunTestOn and the number of cascades below, which BuildDraws tests every
			// kObjectSunTest input's bound against (a miss marks the draw kObjectSunMiss). 0 elsewhere.
			std::uint32_t sunState;
			// The depth segment: BSTreeNode::OnVisible's height test for the kObjectHeightTest inputs, the base and the
			// limit (PrimaryCull::TreeHeightTest; +infinity when the test is off). 0 elsewhere.
			float treeHeight[2];
			// Per cascade: the active masks of its planes and its custom planes (0: none), then 6 planes and 6 custom
			// planes as (normal, constant), outside when dot(normal, c) - constant < -r (SunAccumulation's test).
			std::uint32_t sunMasks[4][2];
			float sunPlanes[4][12][4];
			// The depth segment: the main camera's position and LOD factor (NiCamera +0x184), as BSFadeNode::OnVisible
			// measures a fade root's distance (PrimaryCull::FadeEye), for the kObjectFadeTest inputs. 0 elsewhere.
			float fadeEye[4];
			// A view of the sun (kCullSunEntry): the frame's full-frustum culling processes (ShadowInputs::sunEntryPlanes), each
			// 6 planes with its active mask. An input whose entry sphere (its fade row, SetSunEntryRow) is outside every one is
			// none of the sun's casters. A count of 0 leaves the verdict to the input's kInputOutsideSunEntry.
			std::uint32_t sunEntryCount;
			std::uint32_t sunEntryMasks[8];
			std::uint32_t sunEntryPad[3];
			float sunEntryPlanes[8][6][4];
			std::uint8_t reserved[208];
		};
		static_assert(sizeof(BuildDrawsLatch) == 2048 && offsetof(BuildDrawsLatch, viewProj) == 32 && offsetof(BuildDrawsLatch, cullPlanes) == 96 &&
					  offsetof(BuildDrawsLatch, pipelineMapOffset) == 192 && offsetof(BuildDrawsLatch, sunState) == 196 &&
					  offsetof(BuildDrawsLatch, sunMasks) == 208 && offsetof(BuildDrawsLatch, sunPlanes) == 240 && offsetof(BuildDrawsLatch, fadeEye) == 1008 &&
					  offsetof(BuildDrawsLatch, sunEntryCount) == 1024 && offsetof(BuildDrawsLatch, sunEntryMasks) == 1028 &&
					  offsetof(BuildDrawsLatch, sunEntryPlanes) == 1072);

		/** @brief The sun's entry rule on the latch, as BuildDrawsCS applies it: outside every full-frustum process. */
		bool OutsideSunEntryLatch(const BuildDrawsLatch& a_latch, const float a_entry[4])
		{
			if (!a_latch.sunEntryCount)
				return false;
			for (std::uint32_t process = 0; process < a_latch.sunEntryCount; ++process) {
				bool outside = false;
				for (std::uint32_t p = 0; p < 6 && !outside; ++p) {
					if (!(a_latch.sunEntryMasks[process] & (1u << p)))
						continue;
					const float* plane = a_latch.sunEntryPlanes[process][p];
					outside = plane[0] * a_entry[0] + plane[1] * a_entry[1] + plane[2] * a_entry[2] - plane[3] < -a_entry[3];
				}
				if (!outside)
					return false;
			}
			return true;
		}
		constexpr std::uint32_t kSunTestOn = 1u << 31;

		/** @brief SunAccumulation's sphere test against one cascade of the latch (what BuildDrawsCS does). */
		bool InSunCascade(const BuildDrawsLatch& a_latch, std::uint32_t a_cascade, const float a_centre[3], float a_radius)
		{
			for (std::uint32_t set = 0; set < 2; ++set) {
				const std::uint32_t mask = a_latch.sunMasks[a_cascade][set];
				for (std::uint32_t p = 0; p < 6; ++p) {
					if (!(mask & (1u << p)))
						continue;
					const float* plane = a_latch.sunPlanes[a_cascade][set * 6 + p];
					if (plane[0] * a_centre[0] + plane[1] * a_centre[1] + plane[2] * a_centre[2] - plane[3] < -a_radius)
						return false;
				}
			}
			return true;
		}
		// The shadow latch block: the views' latches, then one pipeline map row per view rasterizer state.
		constexpr std::uint32_t kShadowPipelineMapOffset = kMaxShadowViews * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch));
		constexpr std::uint32_t kShadowPipelineMapRowBytes = kMaxShadowSlots * static_cast<std::uint32_t>(sizeof(std::uint32_t));
		constexpr std::uint32_t kShadowLatchBytes = kShadowPipelineMapOffset + DrawPipelines::kMaxShadowRasterStates * kShadowPipelineMapRowBytes;
		// cullFlags: a clamped shadow view (0xE) pancakes casters in front of its near plane onto it
		// (Utility.hlsl: RENDER_SHADOWMAP_CLAMPED), so the near plane rejects nothing there.
		constexpr std::uint32_t kCullNoNearPlane = 0x200;
		// cullFlags: which caster class a shadow view draws (kObjectVolumetricOnly). The mode's inputs hold both
		// classes; a view of the volumetric lighting copy draws the volumetric-only casters alone, every other
		// shadow view everything else. Neither is set for the main camera.
		constexpr std::uint32_t kCullCastersOnly = 0x400;
		constexpr std::uint32_t kCullVolumetricOnly = 0x800;
		// cullFlags: a view of the sun (its cascades and their volumetric copies), which skips kInputOutsideSunEntry.
		constexpr std::uint32_t kCullSunEntry = 0x1000;

		// A draw's ExecuteIndirect max count: a power of two that only grows, so the recording settles while
		// the preprocess memory stays near what the frame draws (the GPU count buffer says how many run).
		std::uint32_t GrowCapacity(std::uint32_t a_current, std::uint32_t a_needed, std::uint32_t a_limit)
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

		bool SameIndirect(const IndirectState& a, const IndirectState& b)
		{
			if (a.valid != b.valid || !SameHandle(a.layout, b.layout))
				return false;
			for (std::size_t i = 0; i < a.sets.size(); ++i)
				if (!SameHandle(a.sets[i], b.sets[i]) || !SameHandle(a.signatures[i], b.signatures[i]))
					return false;
			return SameHandle(a.depthPassSignature, b.depthPassSignature);
		}

		// The dispatch signature every BuildDraws pass records with: one Dispatch argument, read from a latch.
		rhi::CommandSignaturePtr CreateDispatchSignature(rhi::Device a_device, rhi::PipelineLayoutHandle a_layout)
		{
			rhi::IndirectArg args[] = { { .kind = rhi::IndirectArgKind::Dispatch } };
			rhi::CommandSignaturePtr signature;
			if (a_device.CreateCommandSignature(rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(args, 1), sizeof(std::uint32_t) * 3 }, a_layout, signature) != rhi::Result::Ok)
				return {};
			return signature;
		}

		/**
		 * @brief The segment a pass instance serves. With epochs every instance belongs to one epoch and its
		 * segment is fixed when it is registered, so nothing it prepares or records reads the runtime's
		 * current segment (a pass prepared ahead of its epoch would see another one). Without epochs one
		 * instance serves every segment and takes it from the runtime, as before.
		 */
		struct SegmentBinding
		{
			bool fixed = false;
			RenderGraphRuntime::Segment segment = RenderGraphRuntime::Segment::MainOpaque;

			static SegmentBinding Of(RenderGraphRuntime::Segment a_segment)
			{
				return { RenderGraphRuntime::EpochsEnabled(), a_segment };
			}
			RenderGraphRuntime::Segment Now() const { return fixed ? segment : RenderGraphRuntime::Get().CurrentSegment(); }
		};

		// CS_DCLF_BINDLESS_PARITY: the per-object record against the packed constant group, variable by
		// variable. Both are produced from tables.objects and tables.shading by the same rules, so the
		// comparison is exact rather than tolerant - a tolerance here would only hide a layout mistake.
		void CheckBindlessRecord(const SceneStore::Tables& a_tables, std::uint32_t a_objectIndex, const BindlessObject& a_record, const RE::NiPoint3& a_eye,
			const RE::NiPoint3& a_previousEye, const GeometryPatchOffsets& a_offsets, std::span<const std::byte> a_vs, std::span<const std::byte> a_ps,
			IndirectDraws::Stats& a_stats)
		{
			auto compare = [&](std::span<const std::byte> a_group, std::uint32_t a_offset, std::uint32_t a_count, const float* a_expected, const char* a_name) {
				if (a_offset == ~0u)
					return;  // the pipeline's shaders do not declare it, so neither form carries a value
				for (std::uint32_t c = 0; c < a_count; ++c) {
					const std::size_t at = (std::size_t(a_offset) + c) * 4;
					if (at + 4 > a_group.size())
						return;
					float packed = 0.0f;
					std::memcpy(&packed, a_group.data() + at, 4);
					++a_stats.bindlessParityChecks;
					if (std::bit_cast<std::uint32_t>(packed) == std::bit_cast<std::uint32_t>(a_expected[c]))
						continue;
					if (a_stats.bindlessParityMismatches++ == 0)
						logger::warn("[DCLF] bindless record parity: {}[{}] is {} in the record and {} in the constant group", a_name, c, a_expected[c], packed);
				}
			};
			// The record is absolute; the shader makes it relative with the same single subtraction as this.
			float relative[16] = {};
			StoreRelativeTo(relative, a_record.world, a_eye);
			compare(a_vs, a_offsets.vsWorld, std::min(a_offsets.vsWorldSize, 12u), relative, "World");
			StoreRelativeTo(relative, a_record.previousWorld, a_previousEye);
			compare(a_vs, a_offsets.vsPreviousWorld, std::min(a_offsets.vsPreviousWorldSize, 12u), relative, "PreviousWorld");
			compare(a_ps, a_offsets.psMaterialData, std::min(a_offsets.psMaterialDataSize, 4u), a_record.shading.materialData, "MaterialData");
			compare(a_ps, a_offsets.psEmitColor, std::min(a_offsets.psEmitColorSize, 3u), a_record.shading.emitColor, "EmitColor");
			if (a_offsets.psSSRParams != ~0u && a_offsets.psSSRParamsSize > 3)
				compare(a_ps, a_offsets.psSSRParams + 3, 1, &a_record.shading.ssrSpecular, "SSRParams.w");

			// The tail has no constant group to compare against once its buffers are gone, so it is checked
			// against the expressions the epoch used to build those buffers from - which is exactly the
			// thing being replaced.
			const auto& object = a_tables.objects[a_objectIndex];
			const auto& lights = a_tables.lights[a_objectIndex];
			auto expect = [&](bool a_equal, const char* a_name) {
				++a_stats.bindlessParityChecks;
				if (!a_equal && a_stats.bindlessParityMismatches++ == 0)
					logger::warn("[DCLF] bindless record parity: {} differs for object {}", a_name, a_objectIndex);
			};
			expect(a_record.roomIndex == lights.roomIndex, "RoomIndex");
			expect(a_record.shadowBitMask == lights.shadowBitMask, "ShadowBitMask");
			const float threshold = (object.flags & kObjectAlphaTest) ? ((object.flags >> kObjectAlphaThresholdShift) & 0xFF) / 255.0f : 0.0f;
			expect(std::bit_cast<std::uint32_t>(a_record.alphaTestRef) == std::bit_cast<std::uint32_t>(threshold), "AlphaTestRef");
			const bool skinned = (object.flags & kObjectSkinned) && a_objectIndex < a_tables.boneOffset.size();
			expect(a_record.boneOffset == (skinned ? a_tables.boneOffset[a_objectIndex] : 0u), "BoneOffset");
			expect(a_record.boneRows == (skinned ? a_tables.boneRows[a_objectIndex] : 0u), "BoneRows");
			expect(a_record.previousBoneOffset == (skinned ? a_tables.boneOffset[a_objectIndex] + static_cast<std::uint32_t>(a_tables.bones.size() / 4) : 0u), "PreviousBoneOffset");
			const bool extras = a_objectIndex < a_tables.extraOffset.size() && a_tables.extraOffset[a_objectIndex] != kNoExtraRows;
			expect(a_record.extraOffset == (extras ? static_cast<std::uint32_t>(a_tables.bones.size() / 4) * 2 + a_tables.extraOffset[a_objectIndex] : 0u), "ExtraOffset");
			// EmissiveMult is the only one of the four whose SOURCE changes, from the scene graph to the
			// tables, so it was tempting to check it against a live read of the property here. That check
			// was built, and it fired: ~100 components in half a billion, always on flickering emissives.
			// It was the check that was wrong. The multiplier is animated, and the shader divides it back
			// out of the emissive colour before re-applying it - so the record's multiplier has to be the
			// same SAMPLE that produced this object's emitColor, which is why MakeShading hands both back
			// together. A live read at epoch time is a strictly later sample, and matching it would break
			// the cancellation rather than prove anything.
			//
			// What is worth checking is the plumbing, because emissiveMult is a new parallel array and a
			// gap in one of those shifts every later object's index. Whether it is sampled at the right
			// point in the frame is already covered, and better, by capture parity's EmitColor comparison
			// against the engine's own draw - which is the check RefreshFrameConstants exists to satisfy.
			expect(a_tables.emissiveMult.size() == a_tables.objects.size(), "emissiveMult table length");
			expect(a_tables.skinWetness.size() == a_tables.objects.size() &&
					   std::memcmp(a_record.skinPerGeometry, a_tables.skinWetness[a_objectIndex].data(), sizeof(a_record.skinPerGeometry)) == 0,
				"SkinPerGeometry");
		}

		std::shared_ptr<org::Buffer> CreateWords(std::uint64_t a_words, bool a_unorderedAccess, const char* a_name)
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
		constexpr std::uint32_t kPerFrameVertexRegister = 12;  // VS_PerFrame (Lighting.hlsl): ViewProj at c8
		// Bytes per texel, for printing exactly the pixel a readback holds and nothing beyond it.
		std::uint32_t FormatBytes(DXGI_FORMAT a_format)
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
		constexpr const char* kProbeLastLabel = "after colour";

		constexpr std::uint32_t kSharedDataRegister = 5;   // SharedData (SharedData.hlsli), bound by Community Shaders
		constexpr std::uint32_t kFeatureDataRegister = 6;  // FeatureData, likewise
		constexpr std::uint32_t kLinearLightingRegister = 8;  // LLPerGeometry: Linear Lighting's per-object emissive multiplier
		constexpr std::uint32_t kSkinRegister = 7;             // SkinPerGeometry: Advanced Skin's per-object wetness
		constexpr std::uint32_t kStrictLightDataBytes = 1216;  // LightLimitFix.hlsli StrictLightData (15 lights)
		constexpr std::uint32_t kLightsRegister = 35;          // t35-t37: Light Limit Fix's lights, list and grid
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
			bool offscreen = false;  // colour drawn into DCLF's own targets even on the hybrid path
			bool probePixel = false;
			std::uint32_t probeX = 0, probeY = 0;
			std::uint32_t width = 0, height = 0;  // render area: the main pass viewport
			float minDepth = 0.0f, maxDepth = 1.0f;  // its depth range (the engine uses [0, 0.999998])
			bool hybrid = false;
			rhi::DescriptorHeapHandle resourceHeap{};
			rhi::DescriptorHeapHandle samplerHeap{};
			IndirectState indirect{};

			bool SameShape(const PassFrame& o) const
			{
				return drawCapacity == o.drawCapacity && decalCapacity == o.decalCapacity && cullMode == o.cullMode && offscreen == o.offscreen &&
				       probePixel == o.probePixel && probeX == o.probeX && probeY == o.probeY && width == o.width && height == o.height &&
				       minDepth == o.minDepth && maxDepth == o.maxDepth && hybrid == o.hybrid && SameHandle(resourceHeap, o.resourceHeap) &&
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
		 * @brief The state lists of one recording site's explicit DGC preprocesses (DgcPreprocessEnabled), one per frame slot:
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
			std::array<std::shared_ptr<org::PixelBuffer>, kColorTargets> targets;
			std::uint32_t targetCount = 0;
			std::shared_ptr<org::Resource> depth;  // DCLF's own Z-prepass (the objects it draws)
			// The native main-pass targets: overwritten with DCLF's before the composite (CS_DCLF_DEBUG_VIEW),
			// or drawn into directly (CS_DCLF_HYBRID, where the native loop skips DCLF's objects instead).
			std::array<std::shared_ptr<org::ExternalTextureResource>, kColorTargets> native;
			std::shared_ptr<org::ExternalTextureResource> nativeDepth;
			// What the passes bind: the native targets on the hybrid path, DCLF's own copies otherwise.
			std::array<std::shared_ptr<org::Resource>, kColorTargets> drawTargets;
			std::shared_ptr<org::Resource> drawDepth;
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
			// The explicit DGC preprocesses' state list (DgcPreprocessEnabled) of the colour segment's passes. The depth pass
			// has none: IndirectState::depthPassSignature.
			std::shared_ptr<PreprocessStates> preprocessMain;
			std::shared_ptr<org::PixelBuffer> hzb;
			std::shared_ptr<const ComputeProgram> hzbProgram;
			std::uint32_t hzbWidth = 0, hzbHeight = 0, hzbMips = 0;
			bool hybrid = false;
			bool offscreen = false;
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
			std::array<org::ResourceBindingToken, kColorTargets> targets{};
			org::ResourceBindingToken depth, sequences, count, records, constants, objects, bones;
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
				static const bool enabled = SwitchEnabled("CS_DCLF_PASS_STATS");
				return enabled;
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
		std::array<std::uint32_t, kFramePushWords> FramePushWords(std::uint64_t a_frameConstants);

		struct PreparedDraws
		{
			PassStats* stats = nullptr;  // CS_DCLF_PASS_STATS
			PreprocessStates* preprocess = nullptr;  // null: the calls preprocess implicitly (CS_DCLF_DGC_PREPROCESS=0)
			bool framePush = false;
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
		std::shared_ptr<const PassFrame> CurrentFrame(const Resources& a_resources, RenderGraphRuntime::Segment a_segment)
		{
			if (a_segment == RenderGraphRuntime::Segment::ZPrepass)
				return a_resources.frames[kDepthShape].load(std::memory_order_acquire);
			if (a_segment == RenderGraphRuntime::Segment::MainOpaque)
				return a_resources.frames[kColourShape].load(std::memory_order_acquire);
			return nullptr;
		}

		/**
		 * @brief CS_DCLF_SORT_DRAWS (default on; =0 off): the phase-1 and colour draws executed grouped by pipeline. BuildDraws
		 * appends in whatever order its threads finish, which made nearly every sequence of the indirect draw switch pipeline.
		 */
		bool SortDraws()
		{
			static const bool enabled = SwitchValue("CS_DCLF_SORT_DRAWS") != "0";
			return enabled;
		}
		static_assert(DrawPipelines::kMaxPipelines == 4096, "BuildDrawsCS.hlsl's kSortKeys");

		/** @brief Whether BuildDraws runs in the segment (BuildDrawsPass::Prepare's conditions), which is when a sort follows it. */
		bool BuildsDraws(const Resources& a_resources, RenderGraphRuntime::Segment a_segment)
		{
			return a_resources.buildDraws && a_resources.latch && a_resources.dispatchSignature && CurrentFrame(a_resources, a_segment);
		}

		class MainOpaquePass final : public org::TypedRenderGraphPass<MainOpaquePass, PreparedDraws, PassBindings>
		{
		public:
			// phaseTwo: the second depth draw of the two-phase culling, which draws only what the rebuilt HZB
			// brought back, from the reserved part of the sequence buffer.
			MainOpaquePass(std::shared_ptr<Resources> a_resources, SegmentBinding a_segment, bool a_phaseTwo = false) :
				resources(std::move(a_resources)), segment(a_segment), phaseTwo(a_phaseTwo) {}

			PassBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				PassBindings bindings{};
				for (std::uint32_t i = 0; i < resources->targetCount; ++i)
					bindings.targets[i] = a_builder.BindRenderTarget(resources->drawTargets[i]);
				bindings.depth = a_builder.BindDepthReadWrite(resources->drawDepth);
				bindings.sequences = a_builder.BindIndirectArguments(resources->sequences);
				bindings.count = a_builder.BindIndirectArguments(resources->count);
				// Read through device addresses; declared so the graph orders them after their uploads.
				bindings.records = a_builder.BindShaderResource(resources->records);
				bindings.constants = a_builder.BindShaderResource(resources->constants);
				if (resources->recordsDepth) {
					a_builder.BindShaderResource(resources->recordsDepth);
					a_builder.BindShaderResource(resources->constantsDepth);
				}
				if (resources->objects)
					bindings.objects = a_builder.BindShaderResource(resources->objects);
				if (resources->bones)
					bindings.bones = a_builder.BindShaderResource(resources->bones);
				// Read by the input assembler (the face draws' second stream), after the commit's uploads into it.
				if (resources->facePositions)
					a_builder.BindVertexBuffer(resources->facePositions);
				for (const auto& frameBuffer : resources->frameBuffers)
					bindings.frameBuffers.push_back(a_builder.BindShaderResource(frameBuffer.copy));
				if (resources->lightLimitFix) {
					bindings.lights = a_builder.BindShaderResource(org::ResourceIdentifier("cs.llf.lights"));
					bindings.lightIndexList = a_builder.BindShaderResource(org::ResourceIdentifier("cs.llf.light-index-list"));
					bindings.lightGrid = a_builder.BindShaderResource(org::ResourceIdentifier("cs.llf.light-grid"));
				}
				return bindings;
			}

			void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
			{
				const auto now = segment.Now();
				const auto frame = CurrentFrame(*resources, now);
				a_out.push_back(frame ? frame->generation : 0);
				a_out.push_back(static_cast<std::uint64_t>(now));
				a_out.push_back(phaseTwo ? 1 : 0);
			}

			PreparedDraws Prepare(const PassBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
			{
				PreparedDraws prepared{};
				const auto now = segment.Now();
				auto frame = CurrentFrame(*resources, now);
				if (!frame || (!frame->drawCapacity && !frame->decalCapacity[0] && !frame->decalCapacity[1]) || !frame->indirect.valid)
					return prepared;
				// The rescue draw belongs to the depth segment only.
				if (phaseTwo && now != RenderGraphRuntime::Segment::ZPrepass)
					return prepared;
				prepared.phaseTwo = phaseTwo;
				prepared.zPrepass = now == RenderGraphRuntime::Segment::ZPrepass;
				prepared.stats = resources->passStats.get();
				if (!prepared.zPrepass)
					prepared.preprocess = resources->preprocessMain.get();
				prepared.framePush = FramePushEnabled();
				if (prepared.framePush)
					prepared.framePushWords = FramePushWords(resources->frameConstantsAddress);
				prepared.frame = std::move(frame);
				prepared.targetCount = resources->targetCount;
				for (std::uint32_t i = 0; i < prepared.targetCount; ++i)
					prepared.targetViews[i] = a_preparation.CaptureView(a_bindings.targets[i], { org::BindlessViewKind::RenderTarget });
				prepared.depthView = a_preparation.CaptureView(a_bindings.depth, { org::BindlessViewKind::DepthStencil });
				return prepared;
			}

			static void Record(const PassBindings& a_bindings, const PreparedDraws& a_prepared, org::PassRecordContext& a_recording)
			{
				if (!a_prepared.frame)
					return;
				const auto& frame = *a_prepared.frame;
				auto& commands = a_recording.Commands();
				commands.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
				std::array<rhi::ColorAttachment, kColorTargets> colors{};
				for (std::uint32_t i = 0; i < a_prepared.targetCount; ++i) {
					colors[i].rtv = a_recording.Resolve(a_prepared.targetViews[i]);
					colors[i].loadOp = (frame.hybrid && !frame.offscreen) ? rhi::LoadOp::Load : rhi::LoadOp::Clear;
					colors[i].storeOp = rhi::StoreOp::Store;
					colors[i].resource = a_recording.Resolve(a_bindings.targets[i]).GetHandle();
				}
				const bool zPrepass = a_prepared.zPrepass;
				if (zPrepass && !frame.hybrid)
					return;  // off the hybrid path both passes run together in the main segment
				if (a_prepared.phaseTwo && !(zPrepass && frame.hybrid))
					return;
				const auto sequences = a_recording.Resolve(a_bindings.sequences).GetHandle();
				const auto count = a_recording.Resolve(a_bindings.count).GetHandle();
				rhi::PassBeginInfo begin{};
				begin.width = frame.width;
				begin.height = frame.height;
				begin.minDepth = frame.minDepth;
				begin.maxDepth = frame.maxDepth;

				// DCLF's Z-prepass: depth only, like the native one. On the hybrid path it adds DCLF's objects
				// to the depth the native passes already wrote, so they occlude and are occluded correctly.
				rhi::DepthAttachment depth{};
				depth.dsv = a_recording.Resolve(a_prepared.depthView);
				depth.depthLoad = frame.hybrid ? rhi::LoadOp::Load : rhi::LoadOp::Clear;
				depth.depthStore = rhi::StoreOp::Store;
				depth.stencilLoad = frame.hybrid ? rhi::LoadOp::Load : rhi::LoadOp::Clear;
				depth.stencilStore = frame.hybrid ? rhi::StoreOp::Store : rhi::StoreOp::DontCare;
				depth.clear.depthStencil.depth = 1.0f;
				begin.depth = &depth;
				begin.debugName = "DCLF depth";
				// DCLF's Z-prepass. On the hybrid path it runs in its own segment, at the first draw of the
				// native main pass, so that the rest of the frame - the native draws that test depth, the sky
				// and everything that reads the depth buffer afterwards - sees DCLF's objects.
				auto* stats = a_prepared.stats;
				const std::uint32_t statsSlot = a_recording.FrameSlot();
				const auto depthKind = a_prepared.phaseTwo ? PassStats::kDepthPhaseTwo : PassStats::kDepth;
				// A pass of DCLF's draws: its attachments, the layout, the topology and the frame push data - on the command list,
				// and on a preprocess state list, identically.
				auto beginDrawPass = [&](rhi::CommandList& a_list, const rhi::PassBeginInfo& a_begin) {
					a_list.BeginPass(a_begin);
					a_list.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
					a_list.BindLayout(frame.indirect.layout);
					if (a_prepared.framePush)
						a_list.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, a_prepared.framePushWords.data());
				};
				// The frame slot's preprocess state list, with this pass's heaps; null without explicit preprocessing.
				auto preprocessState = [&]() -> rhi::CommandList* {
					if (!a_prepared.preprocess)
						return nullptr;
					auto& state = a_prepared.preprocess->Begin(statsSlot);
					state.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
					return &state;
				};
				if (!frame.hybrid || zPrepass) {
					// CS_DCLF_ZPREPASS_EMPTY=1: begin and end the pass but draw nothing, to tell apart damage
					// done by the depth writes from damage done by the epoch merely running here (its
					// submission, and the layout the attachment is left in).
					static const bool empty = SwitchEnabled("CS_DCLF_ZPREPASS_EMPTY");
					const bool draws = !(zPrepass && empty);
					// Phase 2 draws only the rescues, from the reserved half of the sequence buffer and
					// its own counter word. Its argument offset has to be a constant the CPU knows, which
					// is why the two phases have fixed ranges instead of sharing one.
					const std::uint64_t argumentOffset = a_prepared.phaseTwo ? std::uint64_t(kPhaseTwoSequenceBase) * sizeof(DrawSequence) : 0;
					const std::uint64_t countOffset = a_prepared.phaseTwo ? kCountDrawnPhaseTwoBytes : 0;
					if (stats)
						stats->Start(commands, statsSlot, depthKind);
					beginDrawPass(commands, begin);
					if (draws) {
						if (stats)
							stats->Begin(commands, statsSlot, depthKind);
						commands.ExecuteIndirect(frame.indirect.depthPassSignature, sequences, argumentOffset, count, countOffset, frame.drawCapacity);
						if (stats)
							stats->End(commands, statsSlot, depthKind);
					}
					commands.EndPass();
					if (stats)
						stats->Resolve(commands, statsSlot, depthKind);
				}
				if (a_prepared.phaseTwo)
					return;

				// The main pass: depth test EQUAL against it (its pipelines do not write depth; the attachment
				// stays in the layout the pass declared).
				depth.depthLoad = rhi::LoadOp::Load;
				depth.stencilLoad = rhi::LoadOp::Load;
				if (zPrepass)
					return;  // the colour pass belongs to the main segment

				// The opaque decals' depth (CS_DCLF_DECAL_DEPTH, default on, =0 off), before any colour: the engine's main pass
				// draws its opaque decal group with depth writes and its bias, so this is where the frame's depth has them, and
				// with it every host fragment under an opaque decal texel fails the colour pass's EQUAL test and is not shaded -
				// the decal overwrites every target there (blending off, full masks). The group's depth variants test LESS_EQUAL
				// with the decal's bias and run the alpha test, so a transparent texel leaves the host's depth. The blended
				// group has none: it writes no depth natively, and it blends over its host, which must still be shaded.
				static const bool decalDepth = SwitchValue("CS_DCLF_DECAL_DEPTH") != "0";
				// nvperf ranges around the colour pass's parts (NvPerfBridge::PushPassRange), when a capture selects them.
				void* const nvCommands = NvPerfBridge::Active() ? rhi::vulkan::get_cmd_list(commands) : nullptr;
				auto subRange = [&](const char* a_name) { return nvCommands && NvPerfBridge::PushPassRange(nvCommands, a_name); };
				auto endSubRange = [&](bool a_pushed) { if (nvCommands) NvPerfBridge::PopPassRange(nvCommands, a_pushed); };
				// The colour passes: the main one with the targets as the frame loads them, the decals' with all of them loaded; each
				// with the layout, the topology and the frame push data.
				auto decalColors = colors;
				for (std::uint32_t i = 0; i < a_prepared.targetCount; ++i)
					decalColors[i].loadOp = rhi::LoadOp::Load;
				rhi::PassBeginInfo mainBegin = begin;
				mainBegin.colors = { colors.data(), a_prepared.targetCount };
				mainBegin.debugName = "DCLF main opaque";
				rhi::PassBeginInfo decalBegin = begin;
				decalBegin.colors = { decalColors.data(), a_prepared.targetCount };
				decalBegin.debugName = "DCLF decals";
				rhi::PassBeginInfo decalDepthBegin = begin;  // depth only
				decalDepthBegin.debugName = "DCLF decal depth";
				const auto colourSignature = frame.indirect.signatures[kColorVariant];
				auto decalArguments = [](std::uint32_t a_group) { return std::uint64_t(kDecalSequenceBase + a_group * kMaxDecalDraws) * sizeof(DrawSequence); };
				auto decalCount = [](std::uint32_t a_group) { return std::uint64_t(kCountDecalGroupWord + a_group) * sizeof(std::uint32_t); };

				// Every call below is preprocessed here, before the first pass (CS_DCLF_DGC_PREPROCESS): generated inside the pass
				// instead, by NVIDIA's driver, each colour call cost a fixed ~170 us of idle GPU in these eight-target passes. The
				// preprocess is generated for the state list's state - each call's pass begun and set up exactly as it is below,
				// with the same heaps - which the execution must match; the sequences and counts are final before this pass (the
				// culling wrote them).
				if (auto* preprocessList = preprocessState()) {
					auto& state = *preprocessList;
					if (decalDepth && frame.decalCapacity[0]) {
						beginDrawPass(state, decalDepthBegin);
						commands.PreprocessIndirect(state, frame.indirect.signatures[kDepthVariant], sequences, decalArguments(0), count, decalCount(0), frame.decalCapacity[0]);
						state.EndPass();
					}
					beginDrawPass(state, mainBegin);
					if (frame.drawCapacity)
						commands.PreprocessIndirect(state, colourSignature, sequences, 0, count, 0, frame.drawCapacity);
					state.EndPass();
					if (frame.decalCapacity[0] || frame.decalCapacity[1]) {
						beginDrawPass(state, decalBegin);
						for (std::uint32_t group = 0; group < kDecalGroups; ++group)
							if (frame.decalCapacity[group])
								commands.PreprocessIndirect(state, colourSignature, sequences, decalArguments(group), count, decalCount(group), frame.decalCapacity[group]);
						state.EndPass();
					}
					state.End();
				}

				if (decalDepth && frame.decalCapacity[0]) {
					const bool part = subRange("cs.dclf.colour.decal-depth");
					beginDrawPass(commands, decalDepthBegin);
					commands.ExecuteIndirect(frame.indirect.signatures[kDepthVariant], sequences, decalArguments(0), count, decalCount(0), frame.decalCapacity[0]);
					commands.EndPass();
					endSubRange(part);
				}
				if (stats)
					stats->Start(commands, statsSlot, PassStats::kColour);
				const bool mainPart = subRange("cs.dclf.colour.main");
				beginDrawPass(commands, mainBegin);
				if (stats)
					stats->Begin(commands, statsSlot, PassStats::kColour);
				if (frame.drawCapacity) {
					const bool drawPart = subRange("cs.dclf.colour.main-draws");
					commands.ExecuteIndirect(colourSignature, sequences, 0, count, 0, frame.drawCapacity);
					endSubRange(drawPart);
				}
				if (stats)
					stats->End(commands, statsSlot, PassStats::kColour);
				commands.EndPass();
				endSubRange(mainPart);
				if (stats)
					stats->Resolve(commands, statsSlot, PassStats::kColour);

				// The second pass: decals, after every opaque draw, in the engine's order - its opaque decal
				// group and then its blended one, each from its own fixed-slot range and its own count word.
				// The pipelines test depth LESS_EQUAL with the engine's decal bias and write none (the opaque
				// group's depth is already there, from the decal depth pass). Same attachments, all loaded.
				if (frame.decalCapacity[0] || frame.decalCapacity[1]) {
					const bool decalPart = subRange("cs.dclf.colour.decals");
					beginDrawPass(commands, decalBegin);
					for (std::uint32_t group = 0; group < kDecalGroups; ++group) {
						if (!frame.decalCapacity[group])
							continue;
						const bool groupPart = subRange(group == 0 ? "cs.dclf.colour.decals-opaque" : "cs.dclf.colour.decals-blended");
						commands.ExecuteIndirect(colourSignature, sequences, decalArguments(group), count, decalCount(group), frame.decalCapacity[group]);
						endSubRange(groupPart);
					}
					commands.EndPass();
					endSubRange(decalPart);
				}
			}

		private:
			std::shared_ptr<Resources> resources;
			SegmentBinding segment;
			bool phaseTwo = false;
		};

		/**
		 * @brief Folds the camera translation into the view-projection, so that a bound's absolute world
		 * position can be projected without carrying the camera separately.
		 *
		 * The draws are camera-relative (the engine's posAdjust), so the culling used to subtract the camera
		 * from every bound before projecting. Doing it here instead is exact - it is the same subtraction,
		 * one matrix column earlier - and it freed three words of a push-constant block that had reached
		 * Vulkan's guaranteed 128 bytes.
		 *
		 * Row-major, as the shader's `mul(M, v)` reads it: M'(p,1) = M(p-e,1) needs only column 3 changed,
		 * by subtracting M's upper-left 3x3 applied to the camera.
		 */
		void FoldEyeIntoViewProj(const std::array<float, 16>& a_viewProj, const RE::NiPoint3& a_eye, float (&a_out)[16])
		{
			std::memcpy(a_out, a_viewProj.data(), sizeof(a_out));
			for (std::uint32_t row = 0; row < 4; ++row) {
				a_out[row * 4 + 3] = a_viewProj[row * 4 + 3] -
				                     (a_viewProj[row * 4 + 0] * a_eye.x + a_viewProj[row * 4 + 1] * a_eye.y +
									     a_viewProj[row * 4 + 2] * a_eye.z);
			}
		}

		struct BuildDrawsBindings
		{
			org::ResourceBindingToken inputs, inputsDepth, geometries, sequences, count, hzb, visibility, frustum;
			org::ResourceBindingToken sortCounts, sortStaging, sortRanks;
		};

		struct BuildDrawsFrame
		{
			std::shared_ptr<const ComputeProgram> program;
			BuildDrawsConstants constants{};
			std::shared_ptr<const org::LatchBlock> latch;
			rhi::CommandSignatureHandle signature{};
		};

		// Records one BuildDrawsCS dispatch whose values and group count are the slot's latch at a_latchOffset.
		void RecordLatchedDispatch(BuildDrawsConstants a_constants, const org::LatchBlock& a_latch,
			rhi::CommandSignatureHandle a_signature, std::uint32_t a_latchOffset, org::PassRecordContext& a_recording)
		{
			auto& commands = a_recording.Commands();
			const std::uint64_t offset = a_latch.Offset(a_recording.FrameSlot()) + a_latchOffset;
			a_constants.latchOffset = static_cast<std::uint32_t>(offset);
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kBuildDrawsConstantWords, reinterpret_cast<const std::uint32_t*>(&a_constants));
			commands.ExecuteIndirect(a_signature, a_latch.Resource()->GetAPIResource().GetHandle(), offset, {}, 0, 1);
		}

		// Writes this frame's draw sequences and their count from the draw inputs (BuildDrawsCS.hlsl).
		class BuildDrawsPass final : public org::TypedRenderGraphPass<BuildDrawsPass, BuildDrawsFrame, BuildDrawsBindings>
		{
		public:
			BuildDrawsPass(std::shared_ptr<Resources> a_resources, SegmentBinding a_segment, std::uint32_t a_phase = 0) :
				resources(std::move(a_resources)), segment(a_segment), fixedPhase(a_phase) {}

			BuildDrawsBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				BuildDrawsBindings bindings{};
				bindings.inputs = a_builder.BindShaderResource(resources->inputs);
				if (resources->inputsDepth)
					bindings.inputsDepth = a_builder.BindShaderResource(resources->inputsDepth);
				bindings.geometries = a_builder.BindShaderResource(resources->geometries);
				bindings.sequences = a_builder.BindUnorderedAccess(resources->sequences);
				bindings.count = a_builder.BindUnorderedAccess(resources->count);
				bindings.visibility = a_builder.BindUnorderedAccess(resources->visibility);
				if (resources->frustum)
					bindings.frustum = a_builder.BindUnorderedAccess(resources->frustum);
				// Phase 1 sees the HZB the previous frame left, phase 2 the one just rebuilt from this
				// frame's depth. Both read the same resource; what differs is where they sit relative to
				// the build, which is why the ordering below is the whole design.
				if (resources->hzb)
					bindings.hzb = a_builder.BindShaderResource(resources->hzb);
				if (Sorts()) {
					bindings.sortCounts = a_builder.BindUnorderedAccess(resources->sort->counts);
					bindings.sortStaging = a_builder.BindUnorderedAccess(resources->sort->staging);
					bindings.sortRanks = a_builder.BindUnorderedAccess(resources->sort->ranks);
				}
				return bindings;
			}

			void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
			{
				const auto now = segment.Now();
				const auto frame = CurrentFrame(*resources, now);
				a_out.push_back(frame ? frame->generation : 0);
				a_out.push_back(static_cast<std::uint64_t>(now));
				a_out.push_back(fixedPhase);
			}

			BuildDrawsFrame Prepare(const BuildDrawsBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
			{
				BuildDrawsFrame prepared{};
				const auto now = segment.Now();
				const auto frame = CurrentFrame(*resources, now);
				if (!frame || !resources->buildDraws || !resources->latch || !resources->dispatchSignature)
					return prepared;
				const std::uint32_t phase = Phase(now);
				// The second phase belongs to the depth segment only: it is what re-tests phase 1's rejects
				// against the HZB that has just been rebuilt from this frame's depth.
				if (fixedPhase == 2 && now != RenderGraphRuntime::Segment::ZPrepass)
					return prepared;
				prepared.program = resources->buildDraws;
				prepared.latch = resources->latch;
				prepared.signature = resources->dispatchSignature->GetHandle();
				auto& constants = prepared.constants;
				constants.latchIndex = resources->latch->SrvIndex();
				// The depth segment's phases read its own inputs (Resources::inputsDepth), the colour segment the colour inputs.
				const bool depthInputs = (phase == 1 || phase == 2) && resources->inputsDepth;
				constants.inputsIndex = a_preparation.ResolveView(depthInputs ? a_bindings.inputsDepth : a_bindings.inputs, { org::BindlessViewKind::ShaderResource }).index;
				constants.geometriesIndex = a_preparation.ResolveView(a_bindings.geometries, { org::BindlessViewKind::ShaderResource }).index;
				constants.sequencesIndex = a_preparation.ResolveView(a_bindings.sequences, { org::BindlessViewKind::UnorderedAccess }).index;
				constants.countIndex = a_preparation.ResolveView(a_bindings.count, { org::BindlessViewKind::UnorderedAccess }).index;
				// The depth segment's phases name its own records (Resources::recordsDepth).
				const std::uint64_t recordsAddress = (phase == 1 || phase == 2) && resources->recordsDepth ? resources->recordsDepthAddress : resources->recordsAddress;
				constants.recordsAddressLo = static_cast<std::uint32_t>(recordsAddress);
				constants.recordsAddressHi = static_cast<std::uint32_t>(recordsAddress >> 32);
				constants.recordStride = sizeof(DrawBindings);
				constants.phaseBits = (phase & 0xFu) << 4;
				constants.visibilityIndex = a_preparation.ResolveView(a_bindings.visibility, { org::BindlessViewKind::UnorderedAccess }).index;
				// The frustum stamps: the depth segment's first phase alone tests every candidate's frustum.
				if (resources->frustum && phase == 1)
					constants.frustumIndex = a_preparation.ResolveView(a_bindings.frustum, { org::BindlessViewKind::UnorderedAccess }).index;
				if (resources->hzb && frame->cullMode >= 2 && frame->width && frame->height) {
					constants.hzbIndex = a_preparation.ResolveView(a_bindings.hzb, { org::BindlessViewKind::ShaderResource }).index;
					constants.hzbSizePacked = (resources->hzbWidth & 0xFFFF) | (resources->hzbHeight << 16);
					constants.hzbMips = resources->hzbMips;
				}
				if (Sorts()) {
					constants.sortCountsIndex = a_preparation.ResolveView(a_bindings.sortCounts, { org::BindlessViewKind::UnorderedAccess }).index;
					constants.sortStagingIndex = a_preparation.ResolveView(a_bindings.sortStaging, { org::BindlessViewKind::UnorderedAccess }).index;
					constants.sortRanksIndex = a_preparation.ResolveView(a_bindings.sortRanks, { org::BindlessViewKind::UnorderedAccess }).index;
				}
				return prepared;
			}

			static void Record(const BuildDrawsBindings&, const BuildDrawsFrame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.program || !a_frame.latch)
					return;
				auto& commands = a_recording.Commands();
				commands.BindLayout(a_frame.program->layout->GetHandle());
				commands.BindPipeline(a_frame.program->pipeline->GetHandle());
				RecordLatchedDispatch(a_frame.constants, *a_frame.latch, a_frame.signature, 0, a_recording);
			}

			std::uint32_t Phase(RenderGraphRuntime::Segment a_segment) const
			{
				if (fixedPhase)
					return fixedPhase;
				// Off the hybrid path there is no depth segment to decide anything, so the colour segment
				// has to do its own culling exactly as it did before the phases existed.
				if (!resources->hybrid)
					return 0;
				return a_segment == RenderGraphRuntime::Segment::ZPrepass ? 1u : 3u;
			}

		private:
			// Phase 2 appends its few rescues into its own range, unsorted; the other builds are followed by the sort's passes.
			bool Sorts() const { return resources->sort && fixedPhase != 2; }

			std::shared_ptr<Resources> resources;
			SegmentBinding segment;
			std::uint32_t fixedPhase = 0;
		};

		struct SortSequencesBindings
		{
			org::ResourceBindingToken staging, ranks, offsets, count, sequences;
		};

		struct SortSequencesFrame
		{
			std::shared_ptr<const ComputeProgram> program;
			SortSequencesConstants constants{};
		};

		/**
		 * @brief The sort's scatter (DrawSort), after its segment's BuildDraws and the scan. It covers every slot BuildDraws can
		 * append (kMaxDraws), each thread past the count returning.
		 */
		class SortSequencesPass final : public org::TypedRenderGraphPass<SortSequencesPass, SortSequencesFrame, SortSequencesBindings>
		{
		public:
			SortSequencesPass(std::shared_ptr<Resources> a_resources, SegmentBinding a_segment) :
				resources(std::move(a_resources)), segment(a_segment) {}

			SortSequencesBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				SortSequencesBindings bindings{};
				bindings.staging = a_builder.BindShaderResource(resources->sort->staging);
				bindings.ranks = a_builder.BindShaderResource(resources->sort->ranks);
				bindings.offsets = a_builder.BindShaderResource(resources->sort->offsets);
				bindings.count = a_builder.BindShaderResource(resources->count);
				bindings.sequences = a_builder.BindUnorderedAccess(resources->sequences);
				return bindings;
			}

			void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
			{
				const auto now = segment.Now();
				const auto frame = CurrentFrame(*resources, now);
				a_out.push_back(frame ? frame->generation : 0);
				a_out.push_back(static_cast<std::uint64_t>(now));
			}

			SortSequencesFrame Prepare(const SortSequencesBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
			{
				SortSequencesFrame prepared{};
				if (!BuildsDraws(*resources, segment.Now()))
					return prepared;
				prepared.program = resources->sort->scatter;
				auto& constants = prepared.constants;
				constants.stagingIndex = a_preparation.ResolveView(a_bindings.staging, { org::BindlessViewKind::ShaderResource }).index;
				constants.ranksIndex = a_preparation.ResolveView(a_bindings.ranks, { org::BindlessViewKind::ShaderResource }).index;
				constants.offsetsIndex = a_preparation.ResolveView(a_bindings.offsets, { org::BindlessViewKind::ShaderResource }).index;
				constants.countIndex = a_preparation.ResolveView(a_bindings.count, { org::BindlessViewKind::ShaderResource }).index;
				constants.sequencesIndex = a_preparation.ResolveView(a_bindings.sequences, { org::BindlessViewKind::UnorderedAccess }).index;
				constants.sequenceStride = static_cast<std::uint32_t>(sizeof(DrawSequence));
				return prepared;
			}

			static void Record(const SortSequencesBindings&, const SortSequencesFrame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.program)
					return;
				auto& commands = a_recording.Commands();
				commands.BindLayout(a_frame.program->layout->GetHandle());
				commands.BindPipeline(a_frame.program->pipeline->GetHandle());
				commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kSortSequencesConstantWords, reinterpret_cast<const std::uint32_t*>(&a_frame.constants));
				commands.Dispatch(kMaxDraws / kSortSequencesGroup, 1, 1);
			}

		private:
			std::shared_ptr<Resources> resources;
			SegmentBinding segment;
		};

		struct FeedbackBindings
		{
			org::ResourceBindingToken source;
			std::vector<org::ResourceBindingToken> slots;
		};

		struct FeedbackFrame
		{
			int slot = -1;
			std::uint64_t bytes = 0;
		};

		/**
		 * @brief Copies the frustum stamps into the feedback slot the colour commit armed, and reserves the slot's fence
		 * value on the feedback timeline, which the framework signals after the copy completes (as BasicRenderer's
		 * CLodStructuralStreamingReadbackCopyPass). No invocation revision: a packet with a reservation is prepared
		 * every frame. A copy abandoned before submission returns its slot.
		 */
		class FeedbackPass final : public org::TypedRenderGraphPass<FeedbackPass, FeedbackFrame, FeedbackBindings>
		{
		public:
			FeedbackPass(std::shared_ptr<Resources> a_resources, SegmentBinding a_segment) :
				resources(std::move(a_resources)), segment(a_segment) {}

			FeedbackBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				FeedbackBindings bindings{};
				bindings.source = a_builder.BindCopySource(resources->frustum);
				for (const auto& slot : resources->feedback->slots)
					bindings.slots.push_back(a_builder.BindCopyDestination(slot->staging));
				return bindings;
			}

			FeedbackFrame Prepare(const FeedbackBindings&, const org::PassPrepareContext& a_preparation) const
			{
				FeedbackFrame prepared{};
				if (segment.Now() != RenderGraphRuntime::Segment::MainOpaque)
					return prepared;
				auto feedback = resources->feedback;
				const int index = feedback->armed.exchange(-1, std::memory_order_acq_rel);
				if (index < 0 || static_cast<std::size_t>(index) >= feedback->slots.size())
					return prepared;
				auto& slot = *feedback->slots[index];
				if (slot.state.load(std::memory_order_acquire) != Resources::Feedback::Recording)
					return prepared;
				const std::uint64_t value = feedback->fenceCounter.fetch_add(1, std::memory_order_relaxed) + 1;
				slot.fenceValue = value;
				slot.state.store(Resources::Feedback::Submitted, std::memory_order_release);
				a_preparation.Reserve(std::make_shared<const org::runtime::ExternalSignalReservation>(feedback->timeline, value, [feedback, index] {
					auto expected = static_cast<std::uint32_t>(Resources::Feedback::Submitted);
					if (feedback->slots[index]->state.compare_exchange_strong(expected, Resources::Feedback::Free, std::memory_order_acq_rel)) {
						feedback->slots[index]->tag.reset();
						feedback->statAbandoned.fetch_add(1, std::memory_order_relaxed);
					}
				}));
				prepared.slot = index;
				prepared.bytes = std::uint64_t(slot.objects) * sizeof(std::uint32_t);
				return prepared;
			}

			static void Record(const FeedbackBindings& a_bindings, const FeedbackFrame& a_frame, org::PassRecordContext& a_recording)
			{
				if (a_frame.slot < 0 || !a_frame.bytes)
					return;
				a_recording.Commands().CopyBufferRegion(a_recording.Resolve(a_bindings.slots[a_frame.slot]).GetHandle(), 0,
					a_recording.Resolve(a_bindings.source).GetHandle(), 0, a_frame.bytes);
			}

		private:
			std::shared_ptr<Resources> resources;
			SegmentBinding segment;
		};

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

		struct HzbBindings
		{
			org::ResourceBindingToken depth, hzb;
		};

		struct HzbFrame
		{
			std::shared_ptr<const ComputeProgram> program;
			// One dispatch per mip, each reading the level above (mip 0 reads the scene depth).
			struct Level
			{
				HzbConstants constants{};
				std::uint32_t groupsX = 0, groupsY = 0;
			};
			std::vector<Level> levels;
		};

		/**
		 * @brief Builds the hierarchical depth buffer after the world's depth draws.
		 *
		 * It runs in the ZPrepass segment, after DCLF's own depth draws, so the HZB describes the depth the
		 * frame actually has: the native occluders the engine drew (terrain and everything ineligible) plus
		 * the objects DCLF drew itself. On AE that is inside the depth pass, before the first-person model's
		 * depth and the rooms' stencil draws, which it therefore leaves out: fewer occluders, never more.
		 *
		 * The whole mip chain is one pass with a full memory barrier between the dispatches, rather than one
		 * pass per level. Levels of a single texture are not separate resources to the graph, so a per-level
		 * pass would declare the same resource as both its input and its output and the ordering would not
		 * mean what it reads as.
		 */
		class HzbPass final : public org::TypedRenderGraphPass<HzbPass, HzbFrame, HzbBindings>
		{
		public:
			HzbPass(std::shared_ptr<Resources> a_resources, SegmentBinding a_segment) :
				resources(std::move(a_resources)), segment(a_segment) {}

			HzbBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				HzbBindings bindings{};
				bindings.depth = a_builder.BindShaderResource(resources->nativeDepth);
				bindings.hzb = a_builder.BindUnorderedAccess(resources->hzb);
				return bindings;
			}

			void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
			{
				const auto now = segment.Now();
				const auto frame = CurrentFrame(*resources, now);
				a_out.push_back(frame ? frame->generation : 0);
				a_out.push_back(static_cast<std::uint64_t>(now));
			}

			HzbFrame Prepare(const HzbBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
			{
				HzbFrame prepared{};
				// Only in the Z-prepass segment: anywhere else the depth is not the world's final depth.
				const auto now = segment.Now();
				if (now != RenderGraphRuntime::Segment::ZPrepass)
					return prepared;
				if (!resources->hzb || !resources->hzbProgram)
					return prepared;
				const auto frame = CurrentFrame(*resources, now);
				const std::uint32_t renderWidth = frame && frame->width ? frame->width : resources->width;
				const std::uint32_t renderHeight = frame && frame->height ? frame->height : resources->height;
				prepared.program = resources->hzbProgram;
				const std::uint32_t depthIndex = a_preparation.ResolveView(a_bindings.depth, { org::BindlessViewKind::ShaderResource }).index;
				for (std::uint32_t mip = 0; mip < resources->hzbMips; ++mip) {
					HzbFrame::Level level{};
					level.constants.targetIndex = a_preparation.ResolveView(a_bindings.hzb, { org::BindlessViewKind::UnorderedAccess, UINT32_MAX, mip }).index;
					level.constants.targetSize[0] = std::max(1u, resources->hzbWidth >> mip);
					level.constants.targetSize[1] = std::max(1u, resources->hzbHeight >> mip);
					if (mip == 0) {
						level.constants.fromDepth = 1;
						level.constants.sourceIndex = depthIndex;
						// The rendered area, not the texture: the depth image can be larger than the viewport
						// the draws use, and everything past that viewport is untouched. Bounding the read
						// here makes those texels answer the far plane, which suppresses culling.
						level.constants.sourceSize[0] = renderWidth;
						level.constants.sourceSize[1] = renderHeight;
					} else {
						level.constants.fromDepth = 0;
						level.constants.sourceIndex = prepared.levels.back().constants.targetIndex;
						level.constants.sourceSize[0] = prepared.levels.back().constants.targetSize[0];
						level.constants.sourceSize[1] = prepared.levels.back().constants.targetSize[1];
					}
					level.groupsX = (level.constants.targetSize[0] + 7) / 8;
					level.groupsY = (level.constants.targetSize[1] + 7) / 8;
					prepared.levels.push_back(level);
				}
				return prepared;
			}

			static void Record(const HzbBindings&, const HzbFrame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.program || a_frame.levels.empty())
					return;
				auto& commands = a_recording.Commands();
				commands.BindLayout(a_frame.program->layout->GetHandle());
				commands.BindPipeline(a_frame.program->pipeline->GetHandle());
				for (std::size_t i = 0; i < a_frame.levels.size(); ++i) {
					const auto& level = a_frame.levels[i];
					if (i != 0) {
						// The level below must be complete before this one reads it.
						const rhi::GlobalBarrier global = rhi::FullMemoryBarrier();
						const rhi::BarrierBatch batch{ {}, {}, { &global, 1 } };
						commands.Barriers(batch);
					}
					commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kHzbConstantWords, reinterpret_cast<const std::uint32_t*>(&level.constants));
					commands.Dispatch(level.groupsX, level.groupsY, 1);
				}
			}

		private:
			std::shared_ptr<Resources> resources;
			SegmentBinding segment;
		};

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

		struct ProbeBindings
		{
			std::array<org::ResourceBindingToken, kColorTargets> sources;
			org::ResourceBindingToken depth;
			org::ResourceBindingToken destination;
		};

		struct ProbeFrame
		{
			std::uint32_t count = 0;
			std::uint32_t base = 0;        // first slot this pass writes
			std::uint32_t depthSlot = ~0u;  // where this pass puts the depth texel, if it samples it
			std::uint32_t x = 0, y = 0;
		};

		// Copies one texel of each main-pass target into the probe buffer. Two of these are declared, one
		// either side of the opaque pass, so the same texel is sampled before and after DCLF's colour draws
		// within a single frame.
		class ProbePass final : public org::TypedRenderGraphPass<ProbePass, ProbeFrame, ProbeBindings>
		{
		public:
			ProbePass(std::shared_ptr<Resources> a_resources, SegmentBinding a_segment, bool a_after) :
				resources(std::move(a_resources)), segment(a_segment), after(a_after) {}

			ProbeBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				ProbeBindings bindings{};
				for (std::uint32_t i = 0; i < resources->targetCount; ++i)
					bindings.sources[i] = a_builder.BindCopySource(resources->drawTargets[i]);
				bindings.depth = a_builder.BindCopySource(resources->drawDepth);
				bindings.destination = a_builder.BindCopyDestination(resources->probe);
				return bindings;
			}

			void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
			{
				const auto now = segment.Now();
				const auto frame = CurrentFrame(*resources,
					now == RenderGraphRuntime::Segment::SkyOcclusion || now == RenderGraphRuntime::Segment::LightCulling ? RenderGraphRuntime::Segment::ZPrepass : now);
				a_out.push_back(frame ? frame->generation : 0);
				a_out.push_back(static_cast<std::uint64_t>(now));
			}

			ProbeFrame Prepare(const ProbeBindings&, const org::PassPrepareContext&) const
			{
				ProbeFrame prepared{};
				const auto now = segment.Now();
				const bool zPrepass = now == RenderGraphRuntime::Segment::ZPrepass;
				const bool gapProbe = now == RenderGraphRuntime::Segment::SkyOcclusion || now == RenderGraphRuntime::Segment::LightCulling;
				if (now != RenderGraphRuntime::Segment::MainOpaque && !(zPrepass && after) && !gapProbe)
					return prepared;
				const auto frame = CurrentFrame(*resources, gapProbe ? RenderGraphRuntime::Segment::ZPrepass : now);
				if (!frame || !frame->probePixel)
					return prepared;
				prepared.x = frame->probeX;
				prepared.y = frame->probeY;
				if (zPrepass) {
					prepared.depthSlot = kProbeDepthAfterPrepass;  // what the Z-prepass left in the buffer
					return prepared;
				}
				if (gapProbe) {
					prepared.depthSlot = now == RenderGraphRuntime::Segment::SkyOcclusion ? kProbeDepthAfterSky : kProbeDepthAfterLightCulling;
					return prepared;
				}
				prepared.count = resources->targetCount;
				prepared.base = after ? kColorTargets : 0;
				// What the colour pass tests against, and - with CS_DCLF_COLOUR_DEPTH_WRITE - what it
				// computes, so the two can be compared directly instead of inferred from whether it drew.
				prepared.depthSlot = after ? kProbeDepthAfterColour : kProbeDepthBeforeColour;
				return prepared;
			}

			static void Record(const ProbeBindings& a_bindings, const ProbeFrame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.count && a_frame.depthSlot == ~0u)
					return;
				auto& commands = a_recording.Commands();
				const auto buffer = a_recording.Resolve(a_bindings.destination).GetHandle();
				if (a_frame.depthSlot != ~0u) {
					rhi::BufferTextureCopyFootprint copy{};
					copy.texture = a_recording.Resolve(a_bindings.depth).GetHandle();
					copy.buffer = buffer;
					copy.x = a_frame.x;
					copy.y = a_frame.y;
					copy.footprint.offset = std::uint64_t(a_frame.depthSlot) * kProbeSlotBytes;
					copy.footprint.rowPitch = kProbeSlotBytes;
					copy.footprint.width = 1;
					copy.footprint.height = 1;
					copy.footprint.depth = 1;
					commands.CopyTextureToBuffer(copy);
				}
				for (std::uint32_t i = 0; i < a_frame.count; ++i) {
					rhi::BufferTextureCopyFootprint copy{};
					copy.texture = a_recording.Resolve(a_bindings.sources[i]).GetHandle();
					copy.buffer = buffer;
					copy.mip = 0;
					copy.arraySlice = 0;
					copy.x = a_frame.x;
					copy.y = a_frame.y;
					copy.z = 0;
					copy.footprint.offset = std::uint64_t(a_frame.base + i) * kProbeSlotBytes;
					copy.footprint.rowPitch = kProbeSlotBytes;
					copy.footprint.width = 1;
					copy.footprint.height = 1;
					copy.footprint.depth = 1;
					commands.CopyTextureToBuffer(copy);
				}
			}

		private:
			std::shared_ptr<Resources> resources;
			SegmentBinding segment;
			bool after = false;
		};

		bool DebugViewEnabled()
		{
			return Toggles::Get().Active().debugView;
		}

		// CS_DCLF_CULL=off|frustum: how BuildDrawsCS filters this frame's draws before it writes their
		// sequences. The draw inputs are the whole tracked set, including what the engine's own culling
		// rejected, so frustum mode has real work to do and its rejection count is meaningful. What it must
		// never reject is an object the engine kept (kObjectNativeVisible): those are counted separately as
		// false negatives, and any non-zero count is a defect in the projection here.
		std::uint32_t CullingMode()
		{
			// 0 off, 1 frustum, 2 frustum then the HZB (Toggles: live from the menu).
			return Toggles::Get().Active().cullMode;
		}

		// CS_DCLF_CULL_INPUT=native|tracked: which of the candidates may actually be drawn.
		//
		// native (the default) draws only what the engine's culling kept, so the frame contains exactly what
		// it does today while the culling counters still measure the whole tracked set. tracked hands the
		// decision to the GPU culling alone, which is what Phase 4 is building towards and what Phase 5
		// needs; until the HZB lands it draws everything the engine occluded, so it costs frames and the
		// objects outside the accumulator carry derived per-frame bits rather than measured ones.
		bool RequireNativeVisible()
		{
			return !Toggles::Get().Active().cullTracked;
		}

		// CS_DCLF_HYBRID=1: DCLF draws into the main pass's own targets and depth, and the native loop skips
		// the objects it drew (DrawcallLimitFix's RenderPassImmediately hooks).
		bool HybridEnabled()
		{
			return Toggles::Get().Active().hybrid;
		}

		// CS_DCLF_BUILD_PARITY=1: compare BuildDraws' output with the CPU templates every 300 epochs.
		// MainPayload::objectState values besides the Skip reasons.
		constexpr std::uint8_t kObjectStateDrawable = 0xF0;
		constexpr std::uint8_t kObjectStateDecal = 0xF1;
		constexpr std::uint8_t kObjectStateAbsent = 0xFF;

		/**
		 * CS_DCLF_SET_PARITY=1: every frame, the per-object visibility words read back after the colour epoch,
		 * with BuildDrawsCS's two drawn bits (depth, colour), against what the two builds and the native
		 * withholding decided on the CPU. See CheckSetParity.
		 */
		bool SetParityEnabled()
		{
			static const bool enabled = [] {
				return SwitchEnabled("CS_DCLF_SET_PARITY");
			}();
			return enabled;
		}

		bool BuildParityEnabled()
		{
			static const bool enabled = [] {
				return SwitchEnabled("CS_DCLF_BUILD_PARITY");
			}();
			return enabled;
		}

		struct DebugViewBindings
		{
			std::array<org::ResourceBindingToken, kColorTargets> sources{}, destinations{};
		};

		struct DebugViewFrame
		{
			std::uint32_t count = 0, width = 0, height = 0;
		};

		// CS_DCLF_DEBUG_VIEW=1: copies DCLF's targets over the native ones, so the composited frame shows
		// only what the indirect draws produced.
		class DebugViewPass final : public org::TypedRenderGraphPass<DebugViewPass, DebugViewFrame, DebugViewBindings>
		{
		public:
			DebugViewPass(std::shared_ptr<Resources> a_resources, SegmentBinding a_segment) :
				resources(std::move(a_resources)), segment(a_segment) {}

			DebugViewBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				DebugViewBindings bindings{};
				for (std::uint32_t i = 0; i < resources->targetCount; ++i) {
					bindings.sources[i] = a_builder.BindCopySource(resources->targets[i]);
					bindings.destinations[i] = a_builder.BindCopyDestination(resources->native[i]);
				}
				return bindings;
			}

			void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
			{
				a_out.push_back(segment.Now() == RenderGraphRuntime::Segment::DebugView);
			}

			DebugViewFrame Prepare(const DebugViewBindings&, const org::PassPrepareContext&) const
			{
				if (segment.Now() != RenderGraphRuntime::Segment::DebugView)
					return {};
				return { resources->targetCount, resources->width, resources->height };
			}

			static void Record(const DebugViewBindings& a_bindings, const DebugViewFrame& a_frame, org::PassRecordContext& a_recording)
			{
				auto& commands = a_recording.Commands();
				for (std::uint32_t i = 0; i < a_frame.count; ++i) {
					rhi::TextureCopyRegion destination{};
					destination.texture = a_recording.Resolve(a_bindings.destinations[i]).GetHandle();
					destination.width = a_frame.width;
					destination.height = a_frame.height;
					auto source = destination;
					source.texture = a_recording.Resolve(a_bindings.sources[i]).GetHandle();
					commands.CopyTextureRegion(destination, source);
				}
			}

		private:
			std::shared_ptr<Resources> resources;
			SegmentBinding segment;
		};

		// A DXVK image the graph uses in place: imported without ownership, with simultaneous access (DXVK
		// keeps the images it hands out in GENERAL and uses them between the graph's commands).
		std::shared_ptr<org::ExternalTextureResource> ImportImage(rhi::Device a_device, const DxvkOrgInteropImageInfo& a_image, org::TextureDescription a_desc,
			const char* a_name)
		{
			rhi::vulkan::ImportedImageDesc import{};
			import.image = a_image.image;
			import.createInfo.flags = a_image.flags;
			import.createInfo.imageType = a_image.type;
			import.createInfo.format = a_image.format;
			import.createInfo.extent = a_image.extent;
			import.createInfo.mipLevels = a_image.mipLevels;
			import.createInfo.arrayLayers = a_image.arrayLayers;
			import.createInfo.samples = a_image.samples;
			import.createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
			import.createInfo.usage = a_image.usage;
			import.createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			import.createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			import.currentLayout = a_image.layout;
			import.simultaneousAccess = true;
			import.debugName = a_name;
			rhi::ResourcePtr resource;
			if (a_image.layout != VK_IMAGE_LAYOUT_GENERAL || rhi::vulkan::import_image(a_device, import, resource) != rhi::Result::Ok)
				return nullptr;
			a_desc.imageDimensions.clear();
			a_desc.imageDimensions.push_back({ a_image.extent.width, a_image.extent.height, 0, 0 });
			// An array image publishes a view per slice (ORG builds one DSV per slice for arrays), which is
			// how a shadow view's epoch attaches the one slice the engine gave that view.
			a_desc.isArray = a_image.arrayLayers > 1;
			a_desc.arraySize = a_desc.isArray ? a_image.arrayLayers : 1;
			a_desc.initialLayout = rhi::ResourceLayout::Common;
			auto result = org::ExternalTextureResource::CreateShared(std::move(resource), a_desc, true);
			if (result)
				result->SetName(a_name);
			return result;
		}

		/**
		 * @brief What one shadow view's epoch draws; published before the epoch prepares (as PassFrame is).
		 */
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
			std::uint64_t recordsAddress = 0;  // the slot's copy of the binding records

			bool operator==(const ShadowFrameView&) const = default;
		};

		/**
		 * @brief The frame's shadow epoch: every view the hooks captured, drawn by one graph execution at
		 * AfterShadowMaps. One epoch rather than one per view because the graph's own execution costs
		 * ~0.9 ms of CPU per epoch whatever it draws (the main epochs' "graph execute" part), and the
		 * exterior has four views a frame.
		 */
		// The shape of the frame's shadow epoch: which views, where they draw and with what capacity. Each
		// view's matrices and input count are in its BuildDrawsLatch; this changes only when the engine's view
		// layout or the pipelines do, and its identity is the shadow passes' revision.
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

		/** @brief The shadow views' graph resources: the main path's set, without targets or an HZB, per view slot. */
		struct ShadowResources
		{
			// The explicit DGC preprocesses' state lists (DgcPreprocessEnabled): the shadow views' pass, Skylighting's.
			std::shared_ptr<PreprocessStates> preprocessShadow, preprocessSky;
			std::shared_ptr<org::Buffer> constants, records, objects, bones, geometries, visibility;
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
			// The kept shadow state's versions (ShadowKept) each mode's input buffer and each view slot's records hold.
			std::array<std::uint64_t, kShadowModeCount> inputsUploaded{};
			std::array<std::uint64_t, kMaxShadowViews> recordsUploaded{};
			std::uint64_t constantsAddress = 0, recordsAddress = 0;
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

		// With epochs these passes run only in the shadow epoch; without, every epoch runs them and they are
		// empty outside it.
		std::shared_ptr<const ShadowFrame> CurrentShadowFrame(const ShadowResources& a_resources, bool a_sky)
		{
			const auto segment = a_sky ? RenderGraphRuntime::Segment::SkyOcclusion : RenderGraphRuntime::Segment::ShadowView;
			if (!RenderGraphRuntime::EpochsEnabled() && RenderGraphRuntime::Get().CurrentSegment() != segment)
				return nullptr;
			return (a_sky ? a_resources.skyFrame : a_resources.frame).load(std::memory_order_acquire);
		}

		struct ShadowBuildBindings
		{
			std::array<org::ResourceBindingToken, kShadowModeCount> inputs;
			std::array<org::ResourceBindingToken, kMaxShadowViews> sequences, count;
			org::ResourceBindingToken geometries, visibility;
		};

		struct ShadowBuildPrepared
		{
			std::shared_ptr<const ComputeProgram> program;
			std::shared_ptr<const org::LatchBlock> latch;
			rhi::CommandSignatureHandle signature{};
			struct Dispatch
			{
				BuildDrawsConstants constants{};
				std::uint32_t latchOffset = 0;  // the view's BuildDrawsLatch within the slot's region
			};
			std::vector<Dispatch> dispatches;  // one per captured view
		};

		/** @brief The shadow views' culling: BuildDrawsCS in its single phase, frustum only, one dispatch per view. */
		class ShadowBuildDrawsPass final : public org::TypedRenderGraphPass<ShadowBuildDrawsPass, ShadowBuildPrepared, ShadowBuildBindings>
		{
		public:
			ShadowBuildDrawsPass(std::shared_ptr<ShadowResources> a_resources, bool a_sky) :
				resources(std::move(a_resources)), sky(a_sky) {}

			ShadowBuildBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				ShadowBuildBindings bindings{};
				for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
					bindings.inputs[m] = a_builder.BindShaderResource(resources->inputs[m]);
				for (std::uint32_t s = 0; s < kMaxShadowViews; ++s) {
					bindings.sequences[s] = a_builder.BindUnorderedAccess(resources->sequences[s]);
					bindings.count[s] = a_builder.BindUnorderedAccess(resources->count[s]);
				}
				bindings.geometries = a_builder.BindShaderResource(resources->geometries);
				bindings.visibility = a_builder.BindUnorderedAccess(resources->visibility);
				return bindings;
			}

			void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
			{
				const auto frame = CurrentShadowFrame(*resources, sky);
				a_out.push_back(frame ? frame->generation : 0);
			}

			ShadowBuildPrepared Prepare(const ShadowBuildBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
			{
				ShadowBuildPrepared prepared{};
				const auto frame = CurrentShadowFrame(*resources, sky);
				if (!frame || frame->views.empty() || !resources->buildDraws || !resources->latch || !resources->dispatchSignature)
					return prepared;
				prepared.program = resources->buildDraws;
				prepared.latch = resources->latch;
				prepared.signature = resources->dispatchSignature->GetHandle();
				const auto geometriesIndex = a_preparation.ResolveView(a_bindings.geometries, { org::BindlessViewKind::ShaderResource }).index;
				const auto visibilityIndex = a_preparation.ResolveView(a_bindings.visibility, { org::BindlessViewKind::UnorderedAccess }).index;
				for (const auto& view : frame->views) {
					if (view.slot >= kMaxShadowViews || view.modeIndex >= kShadowModeCount)
						continue;
					ShadowBuildPrepared::Dispatch dispatch{};
					dispatch.latchOffset = view.slot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch));
					auto& constants = dispatch.constants;
					constants.latchIndex = resources->latch->SrvIndex();
					constants.inputsIndex = a_preparation.ResolveView(a_bindings.inputs[view.modeIndex], { org::BindlessViewKind::ShaderResource }).index;
					constants.geometriesIndex = geometriesIndex;
					constants.sequencesIndex = a_preparation.ResolveView(a_bindings.sequences[view.slot], { org::BindlessViewKind::UnorderedAccess }).index;
					constants.countIndex = a_preparation.ResolveView(a_bindings.count[view.slot], { org::BindlessViewKind::UnorderedAccess }).index;
					constants.recordsAddressLo = static_cast<std::uint32_t>(view.recordsAddress);
					constants.recordsAddressHi = static_cast<std::uint32_t>(view.recordsAddress >> 32);
					constants.recordStride = sizeof(DrawBindings);
					// The single phase (the latch holds the frustum-only mode, with no engine-visibility gate).
					constants.phaseBits = 0;
					// The visibility words are written per object by every dispatch; nothing reads them here.
					constants.visibilityIndex = visibilityIndex;
					prepared.dispatches.push_back(dispatch);
				}
				return prepared;
			}

			static void Record(const ShadowBuildBindings&, const ShadowBuildPrepared& a_prepared, org::PassRecordContext& a_recording)
			{
				if (!a_prepared.program || !a_prepared.latch || a_prepared.dispatches.empty())
					return;
				auto& commands = a_recording.Commands();
				commands.BindLayout(a_prepared.program->layout->GetHandle());
				commands.BindPipeline(a_prepared.program->pipeline->GetHandle());
				for (const auto& dispatch : a_prepared.dispatches)
					RecordLatchedDispatch(dispatch.constants, *a_prepared.latch, a_prepared.signature, dispatch.latchOffset, a_recording);
			}

		private:
			std::shared_ptr<ShadowResources> resources;
			bool sky = false;
		};

		struct ShadowPassBindings
		{
			std::array<org::ResourceBindingToken, kShadowDepthTargets> depth{};
			std::array<org::ResourceBindingToken, kMaxShadowViews> sequences, count;
			org::ResourceBindingToken records, constants, objects, bones;
		};

		struct ShadowPrepared
		{
			std::shared_ptr<const ShadowFrame> frame;
			bool sky = false;
			PreprocessStates* preprocess = nullptr;  // null: the calls preprocess implicitly (CS_DCLF_DGC_PREPROCESS=0)
			struct View
			{
				std::uint32_t index = 0;  // into frame->views
				org::PreparedDescriptorReference depthView{};
			};
			std::vector<View> views;
		};

		/** @brief The shadow views' draws: each view's culled sequences into its slice and viewport, in one pass. */
		class ShadowViewPass final : public org::TypedRenderGraphPass<ShadowViewPass, ShadowPrepared, ShadowPassBindings>
		{
		public:
			ShadowViewPass(std::shared_ptr<ShadowResources> a_resources, bool a_sky) :
				resources(std::move(a_resources)), sky(a_sky) {}

			ShadowPassBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				ShadowPassBindings bindings{};
				// The shadow views draw into the shadow maps, Skylighting's epoch into its occlusion map alone.
				for (std::uint32_t i = 0; i < kShadowDepthTargets; ++i) {
					if (resources->depth[i] && (i == kSkyDepthTarget) == sky)
						bindings.depth[i] = a_builder.BindDepthReadWrite(resources->depth[i]);
				}
				for (std::uint32_t s = 0; s < kMaxShadowViews; ++s) {
					bindings.sequences[s] = a_builder.BindIndirectArguments(resources->sequences[s]);
					bindings.count[s] = a_builder.BindIndirectArguments(resources->count[s]);
				}
				bindings.records = a_builder.BindShaderResource(resources->records);
				bindings.constants = a_builder.BindShaderResource(resources->constants);
				bindings.objects = a_builder.BindShaderResource(resources->objects);
				bindings.bones = a_builder.BindShaderResource(resources->bones);
				// The face positions are read by the input assembler (the draws' second stream), after the commit's
				// uploads into them.
				a_builder.BindVertexBuffer(resources->facePositions);
				return bindings;
			}

			void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
			{
				const auto frame = CurrentShadowFrame(*resources, sky);
				a_out.push_back(frame ? frame->generation : 0);
			}

			ShadowPrepared Prepare(const ShadowPassBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
			{
				ShadowPrepared prepared{};
				auto frame = CurrentShadowFrame(*resources, sky);
				if (!frame || frame->views.empty() || !frame->indirect.valid)
					return prepared;
				for (std::uint32_t i = 0; i < frame->views.size(); ++i) {
					const auto& view = frame->views[i];
					if (!view.capacity || view.slot >= kMaxShadowViews || view.target >= kShadowDepthTargets || !resources->depth[view.target] ||
						(view.target == kSkyDepthTarget) != sky)
						continue;
					if (view.slice >= resources->depthLayers[view.target])
						continue;
					org::BindlessViewRequest request{};
					request.kind = org::BindlessViewKind::DepthStencil;
					request.slice = view.slice;
					prepared.views.push_back({ i, a_preparation.CaptureView(a_bindings.depth[view.target], request) });
				}
				if (!prepared.views.empty())
					prepared.frame = std::move(frame);
				prepared.sky = sky;
				prepared.preprocess = (sky ? resources->preprocessSky : resources->preprocessShadow).get();
				return prepared;
			}

			static void Record(const ShadowPassBindings& a_bindings, const ShadowPrepared& a_prepared, org::PassRecordContext& a_recording)
			{
				if (!a_prepared.frame)
					return;
				const auto& frame = *a_prepared.frame;
				auto& commands = a_recording.Commands();
				commands.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
				// A view's pass: its slice and viewport, loaded, added to, stored.
				std::vector<rhi::DepthAttachment> depths(a_prepared.views.size());
				std::vector<rhi::PassBeginInfo> begins(a_prepared.views.size());
				for (std::size_t i = 0; i < a_prepared.views.size(); ++i) {
					const auto& prepared = a_prepared.views[i];
					const auto& view = frame.views[prepared.index];
					auto& begin = begins[i];
					begin.x = view.x;
					begin.y = view.y;
					begin.width = view.width;
					begin.height = view.height;
					begin.minDepth = view.minDepth;
					begin.maxDepth = view.maxDepth;
					// The slice the engine drew its own casters into: loaded, added to, stored.
					auto& depth = depths[i];
					depth.dsv = a_recording.Resolve(prepared.depthView);
					depth.depthLoad = rhi::LoadOp::Load;
					depth.depthStore = rhi::StoreOp::Store;
					depth.stencilLoad = rhi::LoadOp::Load;
					depth.stencilStore = rhi::StoreOp::Store;
					begin.depth = &depth;
					begin.debugName = a_prepared.sky ? "DCLF Skylighting occlusion" : "DCLF shadow view";
				}
				auto beginView = [&](rhi::CommandList& a_list, const rhi::PassBeginInfo& a_begin) {
					a_list.BeginPass(a_begin);
					a_list.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
					a_list.BindLayout(frame.indirect.layout);
				};
				auto sequences = [&](const ShadowPrepared::View& a_view) { return a_recording.Resolve(a_bindings.sequences[frame.views[a_view.index].slot]).GetHandle(); };
				auto counts = [&](const ShadowPrepared::View& a_view) { return a_recording.Resolve(a_bindings.count[frame.views[a_view.index].slot]).GetHandle(); };
				// Every view's call is preprocessed first, against the frame slot's state list with the view's pass set up as below
				// (CS_DCLF_DGC_PREPROCESS; PreprocessStates), and becomes visible at the first view's pass.
				if (a_prepared.preprocess) {
					auto& state = a_prepared.preprocess->Begin(a_recording.FrameSlot());
					state.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
					for (std::size_t i = 0; i < a_prepared.views.size(); ++i) {
						const auto& prepared = a_prepared.views[i];
						beginView(state, begins[i]);
						commands.PreprocessIndirect(state, frame.indirect.signature, sequences(prepared), 0, counts(prepared), 0, frame.views[prepared.index].capacity);
						state.EndPass();
					}
					state.End();
				}
				for (std::size_t i = 0; i < a_prepared.views.size(); ++i) {
					const auto& prepared = a_prepared.views[i];
					beginView(commands, begins[i]);
					commands.ExecuteIndirect(frame.indirect.signature, sequences(prepared), 0, counts(prepared), 0, frame.views[prepared.index].capacity);
					commands.EndPass();
				}
			}

		private:
			std::shared_ptr<ShadowResources> resources;
			bool sky = false;
		};

		class ShadowExtension final : public org::RenderGraph::IRenderGraphExtension
		{
		public:
			explicit ShadowExtension(std::shared_ptr<ShadowResources> a_resources) :
				resources(std::move(a_resources)) {}

			void PrepareForBuild(org::RenderGraph& a_graph) override
			{
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.shadow.constants"), resources->constants);
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.shadow.records"), resources->records);
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.shadow.objects"), resources->objects);
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.shadow.bones"), resources->bones);
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.shadow.face-positions"), resources->facePositions);
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.shadow.geometries"), resources->geometries);
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.shadow.visibility"), resources->visibility);
				for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
					a_graph.RegisterResource(org::ResourceIdentifier(fmt::format("cs.dclf.shadow.draw-inputs{}", m)), resources->inputs[m]);
				for (std::uint32_t s = 0; s < kMaxShadowViews; ++s) {
					a_graph.RegisterResource(org::ResourceIdentifier(fmt::format("cs.dclf.shadow.sequences{}", s)), resources->sequences[s]);
					a_graph.RegisterResource(org::ResourceIdentifier(fmt::format("cs.dclf.shadow.draw-count{}", s)), resources->count[s]);
				}
				for (std::uint32_t i = 0; i < kShadowDepthTargets; ++i) {
					if (resources->depth[i])
						a_graph.RegisterResource(org::ResourceIdentifier(fmt::format("cs.dclf.shadow.depth{}", i)), resources->depth[i]);
				}
			}

			void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
			{
				const auto epoch = RenderGraphRuntime::EpochOf(RenderGraphRuntime::Segment::ShadowView);
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.shadow.build-draws",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<ShadowBuildDrawsPass>(resources, false)))
						.PreferQueue(org::QueueKind::Graphics)
						.Epoch(epoch));
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Render("cs.dclf.shadow.view",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<ShadowViewPass>(resources, false)))
						.Epoch(epoch));
				// Skylighting's occlusion map: the same passes over its one view, in its own epoch after RenderMask.
				const auto skyEpoch = RenderGraphRuntime::EpochOf(RenderGraphRuntime::Segment::SkyOcclusion);
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.sky.build-draws",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<ShadowBuildDrawsPass>(resources, true)))
						.PreferQueue(org::QueueKind::Graphics)
						.Epoch(skyEpoch));
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Render("cs.dclf.sky.view",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<ShadowViewPass>(resources, true)))
						.Epoch(skyEpoch));
			}

		private:
			std::shared_ptr<ShadowResources> resources;
		};

		class MainOpaqueExtension final : public org::RenderGraph::IRenderGraphExtension
		{
		public:
			explicit MainOpaqueExtension(std::shared_ptr<Resources> a_resources) :
				resources(std::move(a_resources)) {}

			void PrepareForBuild(org::RenderGraph& a_graph) override
			{
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.constants"), resources->constants);
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.records"), resources->records);
				if (resources->recordsDepth) {
					a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.constants-depth"), resources->constantsDepth);
					a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.records-depth"), resources->recordsDepth);
				}
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.sequences"), resources->sequences);
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.draw-inputs"), resources->inputs);
				if (resources->inputsDepth)
					a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.draw-inputs-depth"), resources->inputsDepth);
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.geometries"), resources->geometries);
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.draw-count"), resources->count);
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.visibility"), resources->visibility);
				if (resources->frustum)
					a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.frustum"), resources->frustum);
				if (resources->feedback)
					for (std::size_t i = 0; i < resources->feedback->slots.size(); ++i)
						a_graph.RegisterResource(org::ResourceIdentifier(fmt::format("cs.dclf.feedback{}", i)), resources->feedback->slots[i]->staging);
				if (resources->bones)
					a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.bones"), resources->bones);
				if (resources->facePositions)
					a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.face-positions"), resources->facePositions);
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.depth"), resources->depth);
				for (std::uint32_t i = 0; i < resources->targetCount; ++i)
					a_graph.RegisterResource(org::ResourceIdentifier(fmt::format("cs.dclf.target{}", i)), resources->targets[i]);
				for (const auto& frameBuffer : resources->frameBuffers)
					a_graph.RegisterResource(org::ResourceIdentifier(fmt::format("cs.dclf.frame-buffer.t{}", frameBuffer.textureRegister)), frameBuffer.copy);
				for (std::uint32_t i = 0; i < resources->targetCount && resources->native[0]; ++i)
					a_graph.RegisterResource(org::ResourceIdentifier(fmt::format("cs.dclf.native-target{}", i)), resources->native[i]);
				if (resources->nativeDepth)
					a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.native-depth"), resources->nativeDepth);
				if (resources->hzb)
					a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.hzb"), resources->hzb);
				if (resources->sort)
					resources->sort->Register(a_graph);
			}

			void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
			{
				using Segment = RenderGraphRuntime::Segment;
				const auto colour = RenderGraphRuntime::EpochOf(Segment::MainOpaque);
				const auto depth = RenderGraphRuntime::EpochOf(Segment::ZPrepass);
				const auto colourSegment = SegmentBinding::Of(Segment::MainOpaque);
				const auto depthSegment = SegmentBinding::Of(Segment::ZPrepass);
				// With epochs, a pass instance runs only in its own epoch and its segment is fixed here, so the
				// Z-prepass has its own build-draws and draw pass. Without, one instance of each serves both
				// segments and takes its segment from the runtime, as it always has.
				// The sort by pipeline after a build (SortDraws): the counts' prefix sum, then the scatter. The scan runs in every
				// execution, so the counts it clears are zero whether or not a build ran; the scatter only after a build.
				const auto addSort = [&](const char* a_scan, const char* a_scatter, SegmentBinding a_segment, auto a_epoch) {
					if (!resources->sort)
						return;
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute(a_scan, resources->sort->ScanPass())
							.PreferQueue(org::QueueKind::Graphics)
							.Epoch(a_epoch));
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute(a_scatter,
						std::static_pointer_cast<org::RenderPass>(std::make_shared<SortSequencesPass>(resources, a_segment)))
							.PreferQueue(org::QueueKind::Graphics)
							.Epoch(a_epoch));
				};
				if (RenderGraphRuntime::EpochsEnabled() && resources->hybrid) {
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.z.build-draws",
						std::static_pointer_cast<org::RenderPass>(std::make_shared<BuildDrawsPass>(resources, depthSegment)))
							.PreferQueue(org::QueueKind::Graphics)
							.Epoch(depth));
					addSort("cs.dclf.z.sort-scan", "cs.dclf.z.sort-scatter", depthSegment, depth);
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Render("cs.dclf.z.depth",
						std::static_pointer_cast<org::RenderPass>(std::make_shared<MainOpaquePass>(resources, depthSegment)))
							.Epoch(depth));
				}
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.build-draws",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<BuildDrawsPass>(resources, colourSegment)))
						.PreferQueue(org::QueueKind::Graphics)
						.Epoch(colour));
				addSort("cs.dclf.sort-scan", "cs.dclf.sort-scatter", colourSegment, colour);
				if (resources->probe)
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.dclf.probe-before",
						std::static_pointer_cast<org::RenderPass>(std::make_shared<ProbePass>(resources, colourSegment, false)))
							.Epoch(colour));
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Render("cs.dclf.main-opaque",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<MainOpaquePass>(resources, colourSegment)))
						.Epoch(colour));
				if (resources->feedback && resources->frustum && resources->hybrid)
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.dclf.feedback",
						std::static_pointer_cast<org::RenderPass>(std::make_shared<FeedbackPass>(resources, colourSegment)))
							.Epoch(colour));
				if (resources->probe)
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.dclf.probe-after",
						std::static_pointer_cast<org::RenderPass>(std::make_shared<ProbePass>(resources, colourSegment, true)))
							.Epoch(colour));
				// The two-phase tail, all of it inside the depth segment and all of it in this order:
				//
				//   build-draws (phase 1, against the HZB the previous frame left)
				//   main-opaque (the phase-1 depth draw)
				//   hzb         (rebuilt from the depth that draw has just finished)
				//   build-draws-phase2 (phase 1's rejects, re-tested against the rebuilt HZB)
				//   depth-phase2       (the rescues, so their depth is in the frame too)
				//
				// Phase 1 tests against a depth buffer that is a frame old, which is what makes it cheap and
				// also what makes it wrong at the edges: anything that has just come out from behind an
				// occluder was hidden in that buffer. Phase 2 exists to take those back, after a rebuild
				// that can see them.
				if (resources->hzb) {
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.hzb",
						std::static_pointer_cast<org::RenderPass>(std::make_shared<HzbPass>(resources, depthSegment)))
							.PreferQueue(org::QueueKind::Graphics)
							.Epoch(depth));
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.build-draws-phase2",
						std::static_pointer_cast<org::RenderPass>(std::make_shared<BuildDrawsPass>(resources, depthSegment, 2)))
							.PreferQueue(org::QueueKind::Graphics)
							.Epoch(depth));
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Render("cs.dclf.depth-phase2",
						std::static_pointer_cast<org::RenderPass>(std::make_shared<MainOpaquePass>(resources, depthSegment, true)))
							.Epoch(depth));
				}
				if (resources->probe && resources->hybrid && RenderGraphRuntime::EpochsEnabled())
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.dclf.z.probe-after",
						std::static_pointer_cast<org::RenderPass>(std::make_shared<ProbePass>(resources, depthSegment, true)))
							.Epoch(depth));
				if (resources->probe && resources->hybrid && RenderGraphRuntime::EpochsEnabled()) {
					for (const auto segment : { Segment::SkyOcclusion, Segment::LightCulling })
						a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy(segment == Segment::SkyOcclusion ? "cs.dclf.sky.probe-depth" : "cs.dclf.light.probe-depth",
							std::static_pointer_cast<org::RenderPass>(std::make_shared<ProbePass>(resources, SegmentBinding::Of(segment), false)))
								.Epoch(RenderGraphRuntime::EpochOf(segment)));
				}
				if (resources->native[0] && !resources->hybrid)
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Render("cs.dclf.debug-view",
						std::static_pointer_cast<org::RenderPass>(std::make_shared<DebugViewPass>(resources, SegmentBinding::Of(Segment::DebugView))))
							.Epoch(RenderGraphRuntime::EpochOf(Segment::DebugView)));
			}

		private:
			std::shared_ptr<Resources> resources;
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

		Capture CaptureBindings()
		{
			auto* context = globals::d3d::context;
			Capture capture;
			context->VSGetConstantBuffers(0, kConstantBufferRegisters, capture.vsBuffers.data());
			context->PSGetConstantBuffers(0, kConstantBufferRegisters, capture.psBuffers.data());
			context->PSGetShaderResources(0, kTextureRegisters, capture.psViews.data());
			ID3D11RenderTargetView* views[kColorTargets] = {};
			ID3D11DepthStencilView* depth = nullptr;
			context->OMGetRenderTargets(kColorTargets, views, &depth);
			for (std::uint32_t i = 0; i < kColorTargets; ++i) {
				if (!views[i])
					continue;
				winrt::com_ptr<ID3D11Resource> resource;
				views[i]->GetResource(resource.put());
				capture.targets[i] = resource.try_as<ID3D11Texture2D>();
				capture.targetCount = i + 1;
				views[i]->Release();
			}
			if (depth) {
				winrt::com_ptr<ID3D11Resource> resource;
				depth->GetResource(resource.put());
				capture.depth = resource.try_as<ID3D11Texture2D>();
				depth->Release();
			}
			// Community Shaders binds its own per-frame pixel buffers - b5 SharedData and b6 FeatureData -
			// from Renderer_ResetState, so whether they happen to be bound when this capture is taken
			// depends on where the engine last reset its state. The hybrid path changes that, by skipping
			// the native draws in between, and the objects whose shaders read b5 were then dropped for
			// missing constants and rendered untextured. They are CS's own buffers with known identities,
			// so take them from CS instead of from whatever is bound.
			if (auto* state = globals::state) {
				auto adopt = [&](std::uint32_t a_slot, ConstantBuffer* a_buffer) {
					if (capture.psBuffers[a_slot] || !a_buffer || !a_buffer->CB())
						return;
					capture.psBuffers[a_slot] = a_buffer->CB();
					capture.psBuffers[a_slot]->AddRef();  // Capture::Release owns what it holds
				};
				adopt(kSharedDataRegister, state->sharedDataCB);
				adopt(kFeatureDataRegister, state->featureDataCB);
			}
			auto& shadowState = globals::game::shadowState->GetRuntimeData();
			capture.eye = shadowState.posAdjust.getEye();
			capture.previousEye = shadowState.previousPosAdjust.getEye();
			D3D11_VIEWPORT viewport{};
			UINT count = 1;
			context->RSGetViewports(&count, &viewport);
			capture.viewportWidth = static_cast<std::uint32_t>(viewport.Width);
			capture.viewportHeight = static_cast<std::uint32_t>(viewport.Height);
			capture.minDepth = viewport.MinDepth;
			capture.maxDepth = viewport.MaxDepth;
			return capture;
		}

		// The CPU-written structured buffers the main pass binds at t16 and up (Light Limit Fix's come from the graph).
		std::vector<FrameBuffer> FrameBuffersOf(const Capture& a_capture, bool a_lightLimitFix)
		{
			std::vector<FrameBuffer> result;
			for (std::uint32_t t = kPixelTextureSlots; t < kTextureRegisters; ++t) {
				auto* view = a_capture.psViews[t];
				if (!view || (a_lightLimitFix && t >= kLightsRegister && t < kLightsRegister + 3))
					continue;
				D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
				view->GetDesc(&viewDesc);
				if (viewDesc.ViewDimension != D3D11_SRV_DIMENSION_BUFFER && viewDesc.ViewDimension != D3D11_SRV_DIMENSION_BUFFEREX)
					continue;
				winrt::com_ptr<ID3D11Resource> resource;
				view->GetResource(resource.put());
				auto buffer = resource.try_as<ID3D11Buffer>();
				D3D11_BUFFER_DESC bufferDesc{};
				buffer->GetDesc(&bufferDesc);
				if (!(bufferDesc.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE) || !bufferDesc.StructureByteStride)
					continue;  // only CPU-written structured buffers are copied
				FrameBuffer frameBuffer;
				frameBuffer.textureRegister = t;
				frameBuffer.stride = bufferDesc.StructureByteStride;
				frameBuffer.firstElement = viewDesc.ViewDimension == D3D11_SRV_DIMENSION_BUFFER ? viewDesc.Buffer.FirstElement : viewDesc.BufferEx.FirstElement;
				frameBuffer.elements = viewDesc.ViewDimension == D3D11_SRV_DIMENSION_BUFFER ? viewDesc.Buffer.NumElements : viewDesc.BufferEx.NumElements;
				result.push_back(frameBuffer);
			}
			return result;
		}

		ID3D11Buffer* BufferOf(ID3D11ShaderResourceView* a_view)
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

		std::array<std::uint32_t, kFramePushWords> FramePushWords(std::uint64_t a_frameConstants)
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
			std::uint32_t renderFlags = 0;
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

		void UpdateBones(BonesStore* a_store, std::uint64_t a_uploaded, const SceneStore::Tables& a_tables, std::uint32_t a_generation, BonesOut& a_out)
		{
			ZoneScopedN("CS.DCLF.Build.UpdateBones");
			a_out = {};
			a_out.capacity = a_tables.BoneCapacity();
			a_out.extraRows = static_cast<std::uint32_t>(a_tables.extraRows.size() / 4);
			a_out.bones = a_tables.bones.data();
			a_out.previous = a_tables.previousBones.data();
			a_out.extras = a_tables.extraRows.data();
			if (!a_store)
				return;
			auto& s = *a_store;
			++s.updates;
			s.rows.BeginBuild(a_uploaded);
			if (!s.cursor.Continues(a_tables.changeLog, a_generation) || s.capacity != a_out.capacity) {
				s.cursor.Restart(a_generation);
				s.capacity = a_out.capacity;
				s.rows.Resync();
				++s.resyncs;
			} else {
				for (const auto& change : s.cursor.Unread(a_tables.changeLog)) {
					const std::uint32_t o = change.slot;
					if ((change.causes & (kChangePalette | kChangeSkin)) && o < a_tables.boneRows.size() && a_tables.boneRows[o]) {
						s.rows.MarkRange(a_tables.boneOffset[o], a_tables.boneRows[o]);
						s.rows.MarkRange(std::uint64_t(a_out.capacity) + a_tables.boneOffset[o], a_tables.boneRows[o]);
					}
					if ((change.causes & kChangeExtras) && o < a_tables.extraOffset.size() && a_tables.extraOffset[o] != kNoExtraRows)
						s.rows.MarkRange(2ull * a_out.capacity + a_tables.extraOffset[o], kExtraRows);
				}
			}
			s.cursor.Advance(a_tables.changeLog);
			a_out.changes = s.rows.Take();
		}

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
		void CheckBones(BonesStore& a_store, const BonesOut& a_out)
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

		bool PersistentObjectsEnabled()
		{
			static const bool enabled = SwitchValue("CS_DCLF_PERSISTENT_OBJECTS") != "0";
			return enabled;
		}

		bool PersistentParityEnabled()
		{
			static const bool enabled = SwitchEnabled("CS_DCLF_PERSISTENT_PARITY");
			return enabled;
		}

		// What a record is built from (BuildObjectRecord): the placement, the alpha test (bindings), the shading, the lights,
		// the tree animation, the palette and the extras.
		constexpr std::uint32_t kObjectRecordCauses = kChangePlacement | kChangeBindings | kChangeShading | kChangeLights | kChangeTree | kChangeSkin | kChangeExtras;

		/**
		 * @brief The build's object records: brought up to date from the change log in the store (a_uploaded is the version
		 * the buffer holds, as the inputs saw it), or built whole without one.
		 */
		void UpdateObjectRecords(ObjectRecordStore* a_store, std::uint64_t a_uploaded, const SceneStore::Tables& a_tables, std::uint32_t a_generation,
			std::uint32_t a_renderFlags, std::uint32_t a_frame, ObjectRecordsOut& a_out)
		{
			ZoneScopedN("CS.DCLF.Build.UpdateObjectRecords");
			const std::size_t count = std::min<std::size_t>(a_tables.objects.size(), kMaxObjects);
			if (!a_store) {
				auto records = std::make_shared<std::vector<BindlessObject>>(count);
				for (std::size_t r = 0; r < count; ++r)
					BuildObjectRecord(a_tables, static_cast<std::uint32_t>(r), a_renderFlags, (*records)[r]);
				a_out = {};
				a_out.elements = std::move(records);
				return;
			}
			auto& s = *a_store;
			if (s.busy.exchange(1, std::memory_order_acquire) != 0)
				++s.collisions;
			++s.updates;
			s.records.BeginBuild(a_uploaded);
			TracyCZoneN(updateRecordsZone, "CS.DCLF.Build.Objects.ApplyChanges", true);
			if (!s.cursor.Continues(a_tables.changeLog, a_generation) || s.renderFlags != a_renderFlags || s.records.Size() > count) {
				// Every record again: the first build, new tables, or a log this store fell behind.
				auto& records = s.records.Mutable();
				records.resize(count);
				for (std::size_t r = 0; r < count; ++r)
					BuildObjectRecord(a_tables, static_cast<std::uint32_t>(r), a_renderFlags, records[r]);
				s.records.Resync();
				s.cursor.Restart(a_generation);
				s.renderFlags = a_renderFlags;
				++s.resyncs;
			} else {
				// Slots the tables grew by since: records of their own (the log names them too).
				if (s.records.Size() < count) {
					auto& records = s.records.Mutable();
					const auto first = records.size();
					records.resize(count);
					for (std::size_t r = first; r < count; ++r) {
						BuildObjectRecord(a_tables, static_cast<std::uint32_t>(r), a_renderFlags, records[r]);
						s.records.Mark(r);
					}
				}
				BindlessObject fresh;
				s.changedObjects.Clear();
				for (const auto& change : s.cursor.Unread(a_tables.changeLog)) {
					// Every event reads the final immutable table row, not an intermediate value.
					if (!(change.causes & kObjectRecordCauses) || change.slot >= count || (reuseKeptStorage && !s.changedObjects.Add(change.slot)))
						continue;
					BuildObjectRecord(a_tables, change.slot, a_renderFlags, fresh);
					s.rewritten += s.records.Set(change.slot, fresh) ? 1u : 0u;
				}
			}
			s.cursor.Advance(a_tables.changeLog);
			// CS_DCLF_PERSISTENT_PARITY: every record against one built from the tables now.
			TracyCZoneEnd(updateRecordsZone);
			if (PersistentParityEnabled() && ParityDue(a_frame)) {
				BindlessObject fresh;
				const auto& records = s.records.Get();
				for (std::size_t r = 0; r < count; ++r) {
					BuildObjectRecord(a_tables, static_cast<std::uint32_t>(r), a_renderFlags, fresh);
					s.parity.Check(std::memcmp(&fresh, &records[r], sizeof(fresh)) == 0, [&] {
						const auto* geometry = r < a_tables.objectGeometry.size() ? a_tables.objectGeometry[r] : nullptr;
						return fmt::format("record {} '{}'", r, geometry && geometry->name.c_str() ? geometry->name.c_str() : "?");
					});
				}
			}
			{
				ZoneScopedN("CS.DCLF.Build.Objects.Snapshot");
				a_out = s.records.View();
			}
			s.busy.store(0, std::memory_order_release);
		}

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
			bool depthOnly = false, hybrid = false, resolveTextures = true;
			bool bindless = false, bindlessDraws = false, dedupParity = false, bindlessParity = false;
			bool withholding = false, requireNativeVisible = true, linearLighting = false;
			std::uint32_t renderFlags = 0;
			RE::NiPoint3 eye, previousEye;
			std::uint32_t vsFrameMask = 0, psFrameMask = 0;  // the frame slots the commit supplies
			std::array<std::uint32_t, kDecalGroups> decalCount{};
			// The colour epoch's drawn state (render thread's, read while no colour commit can run): the Z-prepass's hybrid gate
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
			std::vector<float> boneRows;                // unused (the rows are uploaded from the tables: bones, below)
			BonesOut bones;                             // the rows: current then previous, then the extras
			// The frame's textures (t16 and up) are the epoch's own descriptor indices, which only the commit
			// can resolve: the build leaves these (record, register) pairs for it.
			std::vector<std::pair<std::uint32_t, std::uint32_t>> framePatches;
			std::vector<std::uint32_t> drawn;  // unused (the drawn state is sent as changes, below)
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
				sequences.clear();
				inputList.clear();
				geometryDraws.Reset();
				faceStreams.clear();
				framePatches.clear();
				drawn.clear();
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

		struct ShadowInputs
		{
			std::uint32_t frameNumber = 0;
			std::uint32_t renderFlags = 0;
			RE::NiPoint3 refEye;
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
			std::uint32_t sceneRebuilds = 0;  // SceneStore::GetSceneRebuilds: the walk the build read was replaced
			TablesHeld tablesHeld;  // ShadowResources::tablesHeld
			// The versions of the kept shadow state the buffers hold (ShadowResources::inputsUploaded), and the oldest of the view
			// slots' copies of the records (recordsUploaded, of the slots holding any): what the journals keep changes for.
			std::array<std::uint64_t, kShadowModeCount> inputsHeld{};
			std::uint64_t recordsOldestHeld = 0;
		};

		struct ShadowPayload
		{
			ShadowInputs inputs;
			ConstantArena arena;  // the view slots' head is reserved; the commit writes the views into it
			std::vector<DrawBindings> records;  // per-view registers (b0, b12) unset
			std::vector<std::uint32_t> objectRecord;  // per object: its binding record, or ~0u when it cannot draw
			std::array<std::vector<DrawInput>, kShadowModeCount> inputList;  // per render mode: the frame's own (after the kept region)
			// The kept state (ShadowKept): per mode the region's inputs at the head of the mode's buffer, and the records' changes,
			// each sent as what changed since the version a buffer holds (a view slot holds its own copy of the records).
			bool kept = false;
			std::array<KeptView<DrawInput>, kShadowModeCount> regionInputs;
			ChangeJournal::Snapshot recordChanges;
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
			std::vector<float> boneRows;  // unused (bones, below)
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
				records.clear();
				objectRecord.clear();
				for (auto& modeInputs : inputList)
					modeInputs.clear();
				objects.Reset();
				bones.Reset();
				kept = false;
				regionInputs = {};
				membership = {};
				recordChanges.Reset();
				boneRows.clear();
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

		std::uint32_t PartitionsOf(const SceneStore::Tables& a_tables, std::uint32_t a_object)
		{
			return a_object < a_tables.skinPartitions.size() ? a_tables.skinPartitions[a_object] : 0u;
		}

		/** @brief The sun's full-frustum processes a shadow view's latch holds (BuildDrawsLatch::sunEntryPlanes). */
		constexpr std::size_t kMaxSunEntryProcesses = 8;

		/**
		 * @brief A shadow input's sun entry (kCullSunEntry): its entry's sphere in the fade row, which BuildDraws tests against the
		 * view's processes (outside every one: no caster of the sun). An entry that is never tested is inside every process.
		 */
		void SetSunEntryRow(DrawInput& a_input, const SceneStore::Tables& a_tables, std::size_t a_object)
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
		void SetFadeRow(DrawInput& a_input, const SceneStore::Tables& a_tables, std::size_t a_object)
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
		void SetSequenceGeometry(DrawSequence& a_sequence, const GeometryRecord& a_geometry, bool a_ownStream = false)
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

		GeometryDraw PackGeometryDraw(const SceneStore::Tables& a_tables, std::uint32_t a_slot, std::size_t a_count)
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
			std::uint32_t a_frame, GeometryDrawsOut& a_out)
		{
			ZoneScopedN("CS.DCLF.Build.UpdateGeometryDraws");
			ScopedScan scan(Scan::PackGeometry);
			a_out = {};
			const std::size_t count = std::min<std::size_t>(a_tables.geometries.size(), kMaxGeometries);
			auto pack = [&](std::size_t a_slot) { return PackGeometryDraw(a_tables, static_cast<std::uint32_t>(a_slot), count); };
			if (!a_store) {
				auto packed = std::make_shared<std::vector<GeometryDraw>>(count);
				for (std::size_t g = 0; g < count; ++g)
					(*packed)[g] = pack(g);
				a_out.slots.elements = std::move(packed);
				return;
			}
			auto& s = *a_store;
			++s.updates;
			s.packed.BeginBuild(a_uploaded);
			if (!s.cursor.Continues(a_tables.geometryLog, a_generation) || s.packed.Size() > count) {
				auto& packed = s.packed.Mutable();
				packed.resize(count);
				for (std::size_t g = 0; g < count; ++g)
					packed[g] = pack(g);
				s.packed.Resync();
				s.cursor.Restart(a_generation);
				++s.resyncs;
			} else {
				// Slots the tables grew by since (the log names them too), then the slots written.
				if (s.packed.Size() < count) {
					auto& packed = s.packed.Mutable();
					const auto first = packed.size();
					packed.resize(count);
					for (std::size_t g = first; g < count; ++g) {
						packed[g] = pack(g);
						s.packed.Mark(g);
					}
				}
				for (const std::uint32_t g : s.cursor.Unread(a_tables.geometryLog))
					if (g < count)
						s.rewritten += s.packed.Set(g, pack(g)) ? 1u : 0u;
			}
			s.cursor.Advance(a_tables.geometryLog);
			// CS_DCLF_PERSISTENT_PARITY: every slot against one packed from the tables now.
			if (PersistentParityEnabled() && ParityDue(a_frame)) {
				const auto& packed = s.packed.Get();
				std::size_t differs = count;
				for (std::size_t g = 0; g < count && differs == count; ++g) {
					const GeometryDraw fresh = pack(g);
					if (std::memcmp(&fresh, &packed[g], sizeof(fresh)) != 0)
						differs = g;
				}
				s.parity.Check(differs == count, [&] { return fmt::format("slot {}", differs); });
			}
			a_out.slots = s.packed.View();
		}

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
		std::uint32_t FaceStreamGeometry(const SceneStore::Tables& a_tables, std::size_t a_object, std::uint64_t a_positions)
		{
			if (!a_positions || a_object >= a_tables.faceStream.size() || a_tables.faceStream[a_object] == kNoFaceStream)
				return ~0u;
			const std::size_t index = std::min<std::size_t>(a_tables.geometries.size(), kMaxGeometries) + a_tables.faceStream[a_object];
			return index < kMaxGeometries ? static_cast<std::uint32_t>(index) : ~0u;
		}

		bool IsFaceObject(const SceneStore::Tables& a_tables, std::size_t a_object)
		{
			return a_object < a_tables.faceStream.size() && a_tables.faceStream[a_object] != kNoFaceStream;
		}

		// After PackGeometryDraws: the face streams' GeometryDraws, in faceStreams order.
		void AppendFaceStreams(const SceneStore::Tables& a_tables, std::uint64_t a_positions, GeometryDrawsOut& a_geometries)
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
		void SetSequenceStream(DrawSequence& a_sequence, const SceneStore::Tables& a_tables, std::size_t a_object, std::uint64_t a_positions)
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
		void PackGeometryTemplate(const GeometryConstants& a_constants, std::span<const std::uint8_t> a_vsTable, std::span<const std::uint8_t> a_psTable, bool a_bindless,
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

		/**
		 * @brief CS_DCLF_BUILD_CACHE (default on, =0 off): what BuildMainPayload derives per (material, pipeline)
		 * pair and per pipeline, kept across frames.
		 *
		 * The pair's texture and sampler indices and its packed PerMaterial groups, and the pipeline's packed
		 * PerTechnique groups and PerGeometry template, were rebuilt by every epoch although they change only
		 * when a slot, a lookup or an evaluated constant does - they were most of the build's cost. An entry
		 * keeps a copy of every input it was derived from, member by member (MaterialRecord has padding that
		 * nothing writes), and is reused only when this build's inputs are byte-identical; anything else
		 * rebuilds it. So the cache cannot serve a stale value, and a build with the cache produces the same
		 * bytes as one without: CS_DCLF_ASYNC=probe compares the worker's (cached) build with an uncached
		 * inline one every epoch.
		 *
		 * One per epoch kind (colour, Z-prepass), used by one build at a time: the worker's, or the render
		 * thread's when it builds inline (the worker's job for that kind is then done or waited for).
		 */
		/** @brief ResidentRegion::indexOf: the object is not in the region. */
		constexpr std::uint32_t kNoRegion = ~0u;
		constexpr std::uint64_t kNoPair = ~0ull;  // a region entry without bindings (a cull-only candidate)
		// What the region leaves the per-frame loop of the input and draw capacity (decals have their own ranges).
		constexpr std::uint32_t kLoopReserve = 2048;

		bool WholeSceneRegionEnabled()
		{
			static const bool enabled = SwitchValue("CS_DCLF_WHOLE_SCENE_REGION") != "0";
			return enabled;
		}

		/**
		 * @brief CS_DCLF_RESIDENT_DRAWS (default on): the resident records' draw inputs persist across frames in each main
		 * segment's input buffer (drawcall-limit-fix.md, "Persistent resident draws").
		 */
		bool ResidentDrawsEnabled()
		{
			static const bool enabled = SwitchValue("CS_DCLF_RESIDENT_DRAWS") != "0";
			return enabled;
		}

		/** @brief CS_DCLF_RESIDENT_DRAW_PARITY=1: every 60 frames, each region entry written again from the tables and compared. */
		bool ResidentDrawParityEnabled()
		{
			static const bool enabled = SwitchEnabled("CS_DCLF_RESIDENT_DRAW_PARITY");
			return enabled;
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

		bool PersistentBindingsEnabled()
		{
			static const bool enabled = SwitchValue("CS_DCLF_PERSISTENT_BINDINGS") != "0";
			return enabled;
		}

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
			bool requireNativeVisible = true;
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
		void PackGroupInto(const ConstantBlock& a_block, const StageLayout& a_layout, std::span<const std::uint8_t> a_table, std::uint64_t a_variables, std::uint32_t a_first,
			BuildCache::PackedGroup& a_out)
		{
			const auto size = ConstantGroupSize(a_layout, a_table, a_variables, a_first);
			a_out.bytes.assign(std::max<std::size_t>(size, 16), std::byte{});
			PackConstantGroup(a_block, a_layout, a_table, a_variables, a_first, a_out.bytes);
			a_out.size = size;
			a_out.valid = true;
		}

		bool BuildCacheEnabled()
		{
			static const bool enabled = SwitchValue("CS_DCLF_BUILD_CACHE") != "0";
			return enabled;
		}

		// The cache's source signatures: plain bytes of trivially copyable values, appended member by member.
		template <class T>
		void AppendSource(std::vector<std::byte>& a_out, const T& a_value)
		{
			static_assert(std::is_trivially_copyable_v<T>);
			const auto* bytes = reinterpret_cast<const std::byte*>(&a_value);
			a_out.insert(a_out.end(), bytes, bytes + sizeof(T));
		}

		void AppendSource(std::vector<std::byte>& a_out, std::span<const std::uint8_t> a_table)
		{
			AppendSource(a_out, a_table.size());
			const auto* bytes = reinterpret_cast<const std::byte*>(a_table.data());
			a_out.insert(a_out.end(), bytes, bytes + a_table.size());
		}

		// Whether the entry's sources equal the ones just written to the scratch; when not, they replace them.
		bool SameSources(std::vector<std::byte>& a_entry, const std::vector<std::byte>& a_current)
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
			const std::array<std::uint32_t, kTextureRegisters>& a_frameTextures, std::uint64_t& a_checks, std::uint64_t& a_mismatches, std::string& a_first)
		{
			if (!a_kept.keptConstants.elements || !a_kept.keptRecords.elements)
				return;
			ankerl::unordered_dense::map<std::uint64_t, std::uint32_t> blockSize;  // offset -> size
			auto note = [&](const PersistentBlock& a_block) {
				if (a_block.offset != ~0ull)
					blockSize[a_block.offset] = a_block.size;
			};
			for (const auto& [key, pair] : a_cache.pairs) {
				note(pair.materialVS);
				note(pair.materialPS);
			}
			for (const auto& [key, pipeline] : a_cache.pipelines)
				for (const auto* block : { &pipeline.techniqueVSBlock, &pipeline.techniquePSBlock, &pipeline.geometryVSBlock, &pipeline.geometryPSBlock, &pipeline.permutationBlock })
					note(*block);
			auto recordsOf = [](const MainPayload& a_payload) {
				ankerl::unordered_dense::map<std::uint32_t, std::uint32_t> out;  // object -> record
				auto add = [&](const DrawInput& a_input) {
					if (a_input.flags & kInputDrawable)
						out.emplace(a_input.objectIndex, a_input.recordIndex);
				};
				if (a_payload.resident.elements)
					for (const auto& input : *a_payload.resident.elements)
						add(input);
				for (const auto& input : a_payload.inputList)
					add(input);
				return out;
			};
			const auto kept = recordsOf(a_kept);
			const auto reference = recordsOf(a_reference);
			const auto& keptBytes = *a_kept.keptConstants.elements;
			const auto& referenceBytes = a_reference.arena.Bytes();
			auto fail = [&](std::uint32_t a_object, const std::string& a_what) {
				if (a_mismatches++ == 0)
					a_first = fmt::format("object {}: {}", a_object, a_what);
			};
			for (const auto& [object, referenceRecord] : reference) {
				++a_checks;
				const auto it = kept.find(object);
				if (it == kept.end()) {
					fail(object, "drawn by the per-frame build only");
					continue;
				}
				if (it->second >= a_kept.keptRecords.Count() || referenceRecord >= a_reference.records.size()) {
					fail(object, "a record index out of range");
					continue;
				}
				const auto& a = (*a_kept.keptRecords.elements)[it->second];
				DrawBindings patched = a;
				if (it->second < a_kept.patchMasks.size())
					for (std::uint32_t word = 0; word < 2; ++word)
						for (std::uint64_t bits = a_kept.patchMasks[it->second][word]; bits; bits &= bits - 1) {
							const auto t = word * 64 + std::countr_zero(bits);
							patched.textures[t] = a_frameTextures[t];
						}
				DrawBindings b = a_reference.records[referenceRecord];
				for (const auto& [record, t] : a_reference.framePatches)
					if (record == referenceRecord)
						b.textures[t] = a_frameTextures[t];
				if (std::memcmp(patched.textures, b.textures, sizeof(a.textures)) != 0 || std::memcmp(a.samplers, b.samplers, sizeof(a.samplers)) != 0) {
					fail(object, "its textures or samplers");
					continue;
				}
				for (std::uint32_t stage = 0; stage < 2; ++stage) {
					const auto* keptAddresses = stage ? a.pixelConstants : a.vertexConstants;
					const auto* referenceAddresses = stage ? b.pixelConstants : b.vertexConstants;
					for (std::uint32_t r = 0; r < kConstantBufferRegisters; ++r) {
						const std::uint64_t x = keptAddresses[r], y = referenceAddresses[r];
						const bool keptBlock = x >= a_constantsBase && x - a_constantsBase < keptBytes.size();
						const bool referenceBlock = y >= a_constantsBase && y - a_constantsBase < referenceBytes.size();
						if (!keptBlock || !referenceBlock) {
							if (x != y)
								fail(object, fmt::format("{} b{}: address {:#x} against {:#x}", stage ? "PS" : "VS", r, x, y));
							continue;
						}
						const auto size = blockSize.find(x - a_constantsBase);
						const std::size_t bytes = size != blockSize.end() ? size->second : 16;
						if (x - a_constantsBase + bytes > keptBytes.size() || y - a_constantsBase + bytes > referenceBytes.size() ||
							std::memcmp(keptBytes.data() + (x - a_constantsBase), referenceBytes.data() + (y - a_constantsBase), bytes) != 0)
							fail(object, fmt::format("{} b{}: the block's {} bytes differ", stage ? "PS" : "VS", r, bytes));
					}
				}
			}
			for (const auto& [object, record] : kept)
				if (!reference.contains(object)) {
					++a_checks;
					fail(object, "drawn by the kept build only");
				}
		}

		/**
		 * @brief The main-pass epoch's draws, from the tables and the lookups: pure.
		 *
		 * Everything the epoch used to fetch from a service as it went - the pipeline set indices, the
		 * shaders' constant tables, the descriptor indices of textures and samplers - comes from the lookups;
		 * an entry the render thread has not resolved yet defers the draw (deferredTextures), and the frame's
		 * own textures are left as patches for the commit. The per-frame constant blocks are addressed by
		 * their fixed slots (FrameSlotOffset) for the slots the inputs say the commit supplies.
		 */
		void BuildMainPayload(const MainInputs& a_in, const SceneStore::Tables& a_tables, const Lookups& a_lookups, MainPayload& a_out,
			BuildCache* a_cache = nullptr, ObjectRecordStore* a_objects = nullptr, BonesStore* a_bones = nullptr, GeometryStore* a_geometries = nullptr)
		{
			ZoneScopedN("CS.DCLF.BuildMainPayload");
			TracyCZoneN(resetMainZone, "CS.DCLF.BuildMain.Reset", true);
			if (a_cache && (a_in.frameNumber % 64) == 0)
				a_cache->Sweep(a_in.frameNumber);
			using Skip = IndirectDraws::Skip;
			a_out.Reset();
			a_out.inputs = a_in;
			auto& arena = a_out.arena;
			auto& records = a_out.records;
			auto& sequences = a_out.sequences;
			auto& drawInputs = a_out.inputList;
			const bool depthOnly = a_in.depthOnly;
			const std::uint64_t base = a_in.addresses.constants;
			const std::uint32_t frameNumber = a_in.frameNumber;
			// The segment's constant blocks and binding records kept across frames (PersistentBindings): only where every record
			// is a pair's (bindless draws), and not under the build parity, which reads the per-build layout.
			const bool persistent = a_cache && a_in.bindless && a_in.bindlessDraws && PersistentBindingsEnabled() && !BuildParityEnabled();
			PersistentBindings* kept = persistent ? &a_cache->persistent : nullptr;
			arena.Reset(depthOnly ? kDepthConstantBytes : kConstantBytes);
			if (kept)
				a_cache->BeginPersistent(a_in.addresses, depthOnly ? kDepthConstantBytes : kConstantBytes, a_in.constantsUploaded, a_in.recordsUploaded);
			TracyCZoneEnd(resetMainZone);

			auto block = [&](const void* a_data, std::size_t a_size) -> std::uint64_t {
				const auto offset = arena.Allocate(a_size);
				if (offset == ~0ull)
					return 0;
				if (a_data)
					std::memcpy(arena.At(offset, a_size).data(), a_data, a_size);
				return base + offset;
			};

			// The frame registers name their slots; a slot the commit does not supply stays at zero, which is
			// what the constants check below rejects for a pipeline that reads it.
			std::array<std::uint64_t, kConstantBufferRegisters> frameVS{}, framePS{};
			for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
				if ((a_in.vsFrameMask >> slot) & 1)
					frameVS[slot] = a_in.addresses.frameConstants + FrameSlotOffset(false, slot);
				if ((a_in.psFrameMask >> slot) & 1)
					framePS[slot] = a_in.addresses.frameConstants + FrameSlotOffset(true, slot);
			}
			const std::uint64_t sharedLightBlock = a_in.addresses.frameConstants + std::uint64_t(kFrameSlotSharedLight) * kFrameSlotBytes;
			const std::uint64_t frameLightingBlock = a_in.addresses.frameConstants + std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes;

			// Blocks shared by many objects.
			struct PipelineBlocks
			{
				std::uint32_t setIndex = Lookups::kNone;
				std::span<const std::uint8_t> vsTable, psTable;
				const Lookups::RegisterUsageBits* usage = nullptr;
				std::uint32_t shadowMaskIndex = Lookups::kNone;
				std::uint64_t techniqueVS = 0, techniquePS = 0;
			};
			std::vector<PipelineBlocks> pipelineBlocks(a_tables.pipelines.size());
			TracyCZoneN(pipelinesZone, "CS.DCLF.BuildMain.Pipelines", true);
			for (std::size_t p = 0; p < a_tables.pipelines.size(); ++p) {
				auto& blocks = pipelineBlocks[p];
				// The pipeline table keeps its slots across frames; only the ones this frame's objects use
				// get blocks (a swept or idle slot has no object pointing at it).
				if (!a_tables.PipelineUsed(p, frameNumber) || p >= a_lookups.pipelines.size())
					continue;
				const auto& entry = a_lookups.pipelines[p];
				if (entry.setIndex == Lookups::kNone || !(entry.key == a_tables.pipelines[p]))
					continue;
				blocks.setIndex = entry.setIndex;
				blocks.vsTable = entry.vsTable;
				blocks.psTable = entry.psTable;
				blocks.usage = &entry.usage[depthOnly ? kDepthVariant : kColorVariant];
				blocks.shadowMaskIndex = entry.shadowMaskIndex;
				const auto& technique = a_tables.TechniqueOf(p);
				const std::uint32_t techniqueVersion = a_tables.TechniqueRowOf(p).constantsVersion;
				BuildCache::Pipeline* cached = nullptr;
				// Kept and clean: the constants and the lookup entry are the versions its blocks were written from.
				if (kept) {
					auto& entryKept = a_cache->pipelines[static_cast<std::uint32_t>(p)];
					entryKept.lastUsed = frameNumber;
					const std::uint32_t constantsVersion = p < a_tables.pipelineConstantsVersion.size() ? a_tables.pipelineConstantsVersion[p] : 0u;
					if (entryKept.clean && entryKept.constantsVersion == constantsVersion && entryKept.techniqueVersion == techniqueVersion &&
						entryKept.lookupVersion == entry.version &&
						entryKept.techniqueVSBlock.offset != ~0ull && entryKept.techniquePSBlock.offset != ~0ull) {
						blocks.techniqueVS = kept->constantsBase + entryKept.techniqueVSBlock.offset;
						blocks.techniquePS = kept->constantsBase + entryKept.techniquePSBlock.offset;
						++a_cache->persistentCleanPipelines;
						continue;
					}
					++a_cache->persistentDirtyPipelines;
				}
				if (a_cache) {
					// Everything the pipeline's technique groups and PerGeometry template are packed from.
					auto& sources = a_cache->scratch;
					sources.clear();
					AppendSource(sources, technique.vs.floats);
					AppendSource(sources, technique.ps.floats);
					AppendSource(sources, a_tables.geometryConstants[p].vs.floats);
					AppendSource(sources, a_tables.geometryConstants[p].ps.floats);
					AppendSource(sources, blocks.vsTable);
					AppendSource(sources, blocks.psTable);
					AppendSource(sources, a_in.bindless);  // PackGeometryTemplate's mask
					cached = &a_cache->pipelines[static_cast<std::uint32_t>(p)];
					cached->lastUsed = frameNumber;
					if (SameSources(cached->sources, sources)) {
						++a_cache->pipelineHits;
					} else {
						++a_cache->pipelineMisses;
						cached->techniqueVS.valid = cached->techniquePS.valid = cached->hasGeometry = false;
					}
				}
				auto pack = [&](const ConstantBlock& a_block, const StageLayout& a_layout, std::span<const std::uint8_t> a_table, std::uint64_t a_variables, std::uint32_t a_first,
								BuildCache::PackedGroup* a_packed, PersistentBlock* a_kept) {
					if (kept && a_packed && a_kept) {
						// Kept: packed only when the sources changed, written only when the bytes did.
						if (!a_packed->valid)
							PackGroupInto(a_block, a_layout, a_table, a_variables, a_first, *a_packed);
						return kept->Place(*a_kept, a_packed->bytes.data(), a_packed->bytes.size(), a_packed->size);
					}
					if (a_packed && a_packed->valid) {
						const auto address = block(nullptr, a_packed->size);
						if (address)
							std::memcpy(arena.At(address - base, a_packed->bytes.size()).data(), a_packed->bytes.data(), a_packed->bytes.size());
						return address;
					}
					const auto size = ConstantGroupSize(a_layout, a_table, a_variables, a_first);
					const auto address = block(nullptr, size);
					if (address) {
						const auto out = arena.At(address - base, std::max<std::size_t>(size, 16));
						PackConstantGroup(a_block, a_layout, a_table, a_variables, a_first, out);
						if (a_packed) {
							a_packed->size = size;
							a_packed->bytes.assign(out.begin(), out.end());
							a_packed->valid = true;
						}
					}
					return address;
				};
				const std::array<std::uint64_t, 5> offsetsBefore = cached ? std::array<std::uint64_t, 5>{ cached->techniqueVSBlock.offset, cached->techniquePSBlock.offset,
					cached->geometryVSBlock.offset, cached->geometryPSBlock.offset, cached->permutationBlock.offset } : std::array<std::uint64_t, 5>{};
				blocks.techniqueVS = pack(technique.vs, LightingVSLayout(), blocks.vsTable, kVSGroups[kPerTechnique], kVSFirstVariable[kPerTechnique],
					cached ? &cached->techniqueVS : nullptr, cached ? &cached->techniqueVSBlock : nullptr);
				blocks.techniquePS = pack(technique.ps, LightingPSLayout(), blocks.psTable, kPSGroups[kPerTechnique], kPSFirstVariable[kPerTechnique],
					cached ? &cached->techniquePS : nullptr, cached ? &cached->techniquePSBlock : nullptr);
				if (kept && cached) {
					// The PerGeometry template and the permutation too: its pairs' records name these blocks, and a clean pair
					// is not looked at again, so they are brought up to date here, with the pipeline's constants.
					if (!cached->hasGeometry) {
						PackGeometryTemplate(a_tables.geometryConstants[p], blocks.vsTable, blocks.psTable, true, cached->geometry);
						cached->hasGeometry = true;
					}
					if (!cached->geometry.vs.empty())
						kept->Place(cached->geometryVSBlock, cached->geometry.vs.data(), cached->geometry.vs.size(), cached->geometry.vs.size());
					if (!cached->geometry.ps.empty())
						kept->Place(cached->geometryPSBlock, cached->geometry.ps.data(), cached->geometry.ps.size(), cached->geometry.ps.size());
					const auto& permutation = a_tables.permutations[p];
					const std::uint32_t data[8] = { permutation.vertexShaderDescriptor, permutation.pixelShaderDescriptor, permutation.extraShaderDescriptor,
						permutation.extraFeatureDescriptor, 0, 0, 0, 0 };
					kept->Place(cached->permutationBlock, reinterpret_cast<const std::byte*>(data), sizeof(data), sizeof(data));
					const std::array<std::uint64_t, 5> offsetsAfter{ cached->techniqueVSBlock.offset, cached->techniquePSBlock.offset, cached->geometryVSBlock.offset,
						cached->geometryPSBlock.offset, cached->permutationBlock.offset };
					if (offsetsAfter != offsetsBefore)
						++cached->addressVersion;
					cached->constantsVersion = p < a_tables.pipelineConstantsVersion.size() ? a_tables.pipelineConstantsVersion[p] : 0u;
					cached->techniqueVersion = techniqueVersion;
					cached->lookupVersion = entry.version;
					cached->clean = blocks.techniqueVS && blocks.techniquePS;
				}
			}
			TracyCZoneEnd(pipelinesZone);

			ankerl::unordered_dense::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> materialBlocks;  // (material, pipeline) -> VS, PS
			// Resolved descriptor heap indices per (material, pipeline) pair (ResolvedBindings, above).
			ankerl::unordered_dense::map<std::uint64_t, ResolvedBindings> resolvedBindings;  // (material, pipeline)
			ankerl::unordered_dense::map<std::uint32_t, GeometryTemplate> geometryTemplates;  // pipeline
			ankerl::unordered_dense::map<std::uint64_t, std::uint64_t> lightBlocks;        // (room, shadow mask)
			ankerl::unordered_dense::map<std::uint64_t, std::uint64_t> permutationBlocks;  // (pipeline, extra bits)
			ankerl::unordered_dense::map<std::uint32_t, std::uint64_t> alphaBlocks;        // threshold
			ankerl::unordered_dense::map<std::uint32_t, std::uint64_t> emissiveBlocks;     // Linear Lighting multiplier bits
			std::map<std::array<float, 4>, std::uint64_t> skinBlocks;                      // Advanced Skin wetness
			const bool linearLighting = a_in.linearLighting;
			const auto renderFlags = a_in.renderFlags;

			// The per-object states are set parity's alone (CS_DCLF_SET_PARITY).
			if (SetParityEnabled())
				a_out.objectState.assign(std::min<std::size_t>(a_tables.objects.size(), kMaxObjects), kObjectStateAbsent);
			// The colour segment's drawn marks (DrawnMarks): kept across builds, sent as changes.
			DrawnMarks* marks = a_cache && !depthOnly ? &a_cache->drawnMarks : nullptr;
			if (marks) {
				auto& m = *marks;
				// What the render thread applied is the version it holds: what changes from here on is sent alone.
				m.changes.BeginBuild(a_in.drawnCommitted);
				if (!m.active || m.generation != a_in.tablesGeneration || m.requireNativeVisible != a_in.requireNativeVisible || a_in.drawnResync) {
					// Every slot again: the first build, new tables, or the render thread asked for it. Whatever the old marks
					// held is withdrawn by the full send, which covers every slot either knows. The journal counts on.
					const auto known = std::max(m.drawn.size(), a_tables.objects.size());
					auto changes = std::move(m.changes);
					m = {};
					m.changes = std::move(changes);
					m.changes.Resync();
					m.active = true;
					m.generation = a_in.tablesGeneration;
					m.requireNativeVisible = a_in.requireNativeVisible;
					m.drawn.assign(known, 0);
					m.geometry.assign(known, nullptr);
					if (a_cache->region.cursor.active)
						a_cache->region.Reset();  // its entries are marked as it reads them again
				}
				++m.serial;
			}
			// Drawn, for the marks: what BuildDraws writes a sequence for, and (under the gate) what the engine kept.
			auto nativeDrawn = [&](std::uint32_t o) { return o < a_tables.objects.size() && (!a_in.requireNativeVisible || (a_tables.objects[o].flags & kObjectNativeVisible)); };
			// The Z-prepass's gate without withholding: what the colour epoch drew last frame.
			auto drewLastFrame = [&](std::uint32_t o) {
				return !a_in.hybrid || a_in.withholding ||
				       (a_in.drawnSlots && o < a_in.drawnSlots->size() && o < a_tables.objectGeometry.size() &&
						   (*a_in.drawnSlots)[o].DrewLast(a_tables.objectGeometry[o], a_in.frameNumber));
			};
			std::uint32_t currentObject = ~0u;
			auto skip = [&](Skip a_reason) {
				++a_out.skipped[static_cast<std::size_t>(a_reason)];
				if (currentObject < a_out.objectState.size())
					a_out.objectState[currentObject] = static_cast<std::uint8_t>(a_reason);
			};
			auto partStart = std::chrono::steady_clock::now();
			auto mark = [&](std::size_t a_part) {
				const auto now = std::chrono::steady_clock::now();
				a_out.partMs[a_part] += std::chrono::duration<double, std::milli>(now - partStart).count();
				partStart = now;
			};
			const bool frameHybrid = a_in.hybrid;
			const bool bindless = a_in.bindless;
			const bool bindlessDraws = a_in.bindlessDraws;
			// Only with bindless draws is the record identical for every draw of a (material, pipeline) pair:
			// without it the light, alpha and emissive blocks still differ per object, and so would the
			// record the cache handed back.
			const bool dedup = bindlessDraws;
			const bool dedupParity = a_in.dedupParity;
			const bool bindlessParity = a_in.bindlessParity;
			// Scratch for that check only, reused across objects so it costs no allocation per draw.
			std::vector<std::byte> parityVS, parityPS;
			const auto& eye = a_in.eye;
			const auto& previousEye = a_in.previousEye;

			// The per-object record table the DCLF_BINDLESS builds read. It is indexed by the object's table
			// index, which the draw carries as its third root constant word, so it is filled for every
			// candidate rather than only the drawn ones - a candidate skipped here still reaches the
			// culling, and an index that addressed nothing would be worse than one that addresses a record
			// no draw reads. Kept across frames in the store (the records the change log names are written again).
			UpdateObjectRecords(a_objects, a_in.tablesHeld.objects, a_tables, a_in.tablesGeneration, renderFlags, frameNumber, a_out.objectRecords);
			const auto& objectRecords = a_out.objectRecords;

			// Everything before the loop: the per-epoch maps and the object records.
			mark(6);
			auto& decalCount = a_out.decalCount;
			auto& decalTemplates = a_out.decalTemplates;
			if (!depthOnly) {
				for (std::uint32_t group = 0; group < kDecalGroups; ++group) {
					decalCount[group] = std::min(a_tables.decalCount[group], kMaxDecalDraws);
					decalTemplates[group].resize(decalCount[group]);
				}
			}
			// The (material, pipeline) pair's bindings record, assembled once per build (deduplicated under bindless draws):
			// its index, or kNoRecord after skip() recorded why. At a_slot when the pair has a stable slot in the resident
			// region (below), appended otherwise.
			auto assembleRecord = [&](std::uint32_t o, const ObjectRecord& object, const PipelineBlocks& blocks, std::uint32_t a_slot) -> std::uint32_t {
					const auto& usage = *blocks.usage;
					const auto& material = a_tables.materials[object.materialIndex];
					const auto& technique = a_tables.TechniqueOf(object.pipelineIndex);
					// A ProjectedUV pipeline binds the engine's four projected textures (SceneStore captured them
					// from a native draw) at the slots SetupGeometry fills, with its wrap/anisotropic modes.
					const bool projectedPipeline = (a_tables.pipelines[object.pipelineIndex].passDescriptor & 0x8000u) != 0;
					auto projectedSlot = [&](std::uint32_t a_slot) -> std::int32_t {
						if (!projectedPipeline)
							return -1;
						for (std::size_t i = 0; i < SceneStore::ProjectedTextures::kSlots.size(); ++i) {
							if (SceneStore::ProjectedTextures::kSlots[i] == a_slot)
								return static_cast<std::int32_t>(i);
						}
						return -1;
					};
					DrawBindings bindings{};

					auto [resolvedIt, newResolved] = resolvedBindings.try_emplace((std::uint64_t(object.materialIndex) << 32) | object.pipelineIndex);
					auto& resolved = resolvedIt->second;
					// Kept: the versions of everything the pair's record and blocks are written from. The same as when they were
					// written, and the pair is its slot: nothing else about it is looked at.
					std::array<std::uint32_t, 12> pairVersions{};
					if (kept) {
						const std::uint32_t m = object.materialIndex, p = object.pipelineIndex;
						const auto& pipelineEntry = a_cache->pipelines[p];
						pairVersions = { m < a_tables.materialVersion.size() ? a_tables.materialVersion[m] : 0u,
							m < a_tables.materialFrameVersion.size() ? a_tables.materialFrameVersion[m] : 0u, m < a_lookups.materials.size() ? a_lookups.materials[m].version : 0u,
							p < a_lookups.pipelines.size() ? a_lookups.pipelines[p].version : 0u, p < a_tables.pipelineBindingVersion.size() ? a_tables.pipelineBindingVersion[p] : 0u,
							a_lookups.sharedVersion, pipelineEntry.addressVersion, a_in.vsFrameMask, a_in.psFrameMask, a_in.resolveTextures ? 1u : 0u,
							pipelineEntry.clean ? 1u : 0u, a_tables.TechniqueRowOf(p).bindingVersion };
						if (newResolved && !dedupParity) {
							if (const auto found = a_cache->pairs.find((std::uint64_t(m) << 32) | p);
								found != a_cache->pairs.end() && found->second.clean && found->second.cleanKey == pairVersions && found->second.recordSlot != kNoRecord) {
								found->second.lastUsed = frameNumber;
								resolved.recordIndex = found->second.recordSlot;
								++a_cache->persistentCleanPairs;
								return found->second.recordSlot;
							}
							++a_cache->persistentDirtyPairs;
						}
					}
					// Assembled once per (material, pipeline) pair when the record no longer varies per object, and per
					// draw otherwise. Everything in here - the texture and sampler heap indices, the material,
					// technique, geometry, permutation, light, alpha and emissive blocks, and the per-frame defaults
					// - is then a property of the pair or of the epoch, so a second draw of the same pair needs
					// nothing but its index.
					auto fail = [&](Skip a_reason) {
						if (dedup)
							resolved.skipReason = static_cast<std::uint32_t>(a_reason);
						if (kept)
							if (const auto found = a_cache->pairs.find((std::uint64_t(object.materialIndex) << 32) | object.pipelineIndex); found != a_cache->pairs.end())
								found->second.clean = false;
						skip(a_reason);
					};
					if (dedup && resolved.skipReason != kNoSkip) {
						skip(static_cast<Skip>(resolved.skipReason));  // per draw, not once per pair
						if (resolved.deferred)
							++a_out.deferredTextures;
						return kNoRecord;
					}
					std::uint32_t recordIndex = resolved.recordIndex;
					// Per DRAW: the loop entry and the resolvedBindings probe above, which every candidate pays
					// whether or not it assembles a record.
					mark(4);
					if (!dedup || recordIndex == kNoRecord || dedupParity) {
						const std::uint64_t pairKey = (std::uint64_t(object.materialIndex) << 32) | object.pipelineIndex;
						bool pairFromCache = false;
						if (newResolved && a_cache) {
							// Everything the pair's resolution and PerMaterial groups are derived from.
							auto& sources = a_cache->scratch;
							sources.clear();
							// The material record by its version (Tables::materialVersion), which is new whenever the record is
							// rewritten; the frame's floats MaterialSources writes into it are not part of it, and a reused
							// group gets them written over below.
							AppendSource(sources, object.materialIndex < a_tables.materialVersion.size() ? a_tables.materialVersion[object.materialIndex] : 0u);
							AppendSource(sources, a_tables.materialSlotKey[object.materialIndex].first);
							AppendSource(sources, a_tables.materialSlotKey[object.materialIndex].second);
							if (object.materialIndex < a_lookups.materials.size()) {
								const auto& entry = a_lookups.materials[object.materialIndex];
								AppendSource(sources, entry.key.first);
								AppendSource(sources, entry.key.second);
								AppendSource(sources, entry.textureIndex);
								AppendSource(sources, entry.featureIndex);
								AppendSource(sources, entry.resolved);
							} else {
								AppendSource(sources, ~0u);
							}
							AppendSource(sources, technique.filterModes);
							AppendSource(sources, technique.shadowMask);
							AppendSource(sources, blocks.shadowMaskIndex);
							AppendSource(sources, usage.vertexConstants);
							AppendSource(sources, usage.pixelConstants);
							AppendSource(sources, usage.textures);
							AppendSource(sources, usage.samplers);
							AppendSource(sources, blocks.vsTable);
							AppendSource(sources, blocks.psTable);
							AppendSource(sources, projectedPipeline);
							AppendSource(sources, a_lookups.nullTexture);
							AppendSource(sources, a_lookups.samplers);
							AppendSource(sources, a_lookups.projectedTextures);
							AppendSource(sources, a_in.addresses.objectsIndex);
							AppendSource(sources, a_in.addresses.bonesIndex);
							AppendSource(sources, a_in.resolveTextures);
							AppendSource(sources, depthOnly);
							auto& entry = a_cache->pairs[pairKey];
							entry.lastUsed = frameNumber;
							if (SameSources(entry.sources, sources) && entry.hasResolved) {
								++a_cache->pairHits;
								resolved.textures = entry.resolved.textures;
								resolved.samplers = entry.resolved.samplers;
								resolved.patchRegisters = entry.resolved.patchRegisters;
								resolved.texturesOk = entry.resolved.texturesOk;
								resolved.samplersOk = entry.resolved.samplersOk;
								resolved.deferred = entry.resolved.deferred;
								resolved.missingTexture = entry.resolved.missingTexture;
								pairFromCache = true;
							} else {
								++a_cache->pairMisses;
								entry.hasResolved = false;
								entry.vs.valid = entry.ps.valid = false;
							}
						}
						if (newResolved && !pairFromCache) {
							// Textures: the material's, the technique's shadow mask, then the frame's.
							resolved.texturesOk = true;
							const Lookups::Material* materialLookup = object.materialIndex < a_lookups.materials.size() ? &a_lookups.materials[object.materialIndex] : nullptr;
							const bool materialResolved = materialLookup && materialLookup->resolved &&
							                              materialLookup->key.first == a_tables.materialSlotKey[object.materialIndex].first &&
							                              materialLookup->key.second == a_tables.materialSlotKey[object.materialIndex].second;
							for (std::uint32_t t = 0; t < kTextureRegisters; ++t) {
								// Slots below 16 the material and technique leave alone read a null view (natively: whatever
								// an earlier draw left bound). A frame register the pipeline reads
								// is the epoch's own descriptor, patched in by the commit; on the Z-prepass the pixel stage's
								// per-frame textures are not bound yet, so a depth pipeline reading one is skipped as before.
								std::uint32_t index = t < kPixelTextureSlots ? a_lookups.nullTexture : kInvalidIndex;
								bool patch = false;
								if (t == kObjectBufferRegister)
									index = a_in.addresses.objectsIndex;
								else if (t == kBonesBufferRegister)
									index = a_in.addresses.bonesIndex;
								else if (!a_in.resolveTextures)
									index = 0;
								else if (t < kPixelTextureSlots && ((material.textureWritten >> t) & 1)) {
									if (!materialResolved) {
										resolved.deferred = true;
										resolved.texturesOk = false;
										resolved.missingTexture = t;
										break;
									}
									index = materialLookup->textureIndex[t];
								} else if (t == kShadowMaskSlot && technique.shadowMask)
									index = blocks.shadowMaskIndex;
								else if (const auto p = projectedSlot(t); p >= 0)
									index = a_lookups.projectedTextures[p];
								else if (const int f = FeatureMaterialSlot(t); f >= 0 && material.featureTextures[f]) {
									// A feature's per-material texture (Advanced Skin's t71, t74).
									if (!materialResolved) {
										resolved.deferred = true;
										resolved.texturesOk = false;
										resolved.missingTexture = t;
										break;
									}
									index = materialLookup->featureIndex[f];
								}
								else if (t >= kPixelTextureSlots && !depthOnly) {
									index = 0;
									patch = usage.UsesTexture(t);
								}
								if (!a_in.resolveTextures && t != kObjectBufferRegister)
									index = 0;
								if (index == kInvalidIndex && usage.UsesTexture(t)) {
									resolved.texturesOk = false;
									resolved.missingTexture = t;
									break;
								}
								resolved.textures[t] = index == kInvalidIndex ? 0 : index;
								if (patch)
									resolved.patchRegisters.push_back(t);
							}

							// Samplers: the modes the material (or, for the shadow mask, the technique) sets.
							resolved.samplersOk = true;
							for (std::uint32_t s = 0; s < kSamplerRegisters; ++s) {
								std::uint32_t address = 0, filter = 0;
								if (s < kPixelTextureSlots && ((material.textureWritten >> s) & 1)) {
									address = material.addressModes[s];
									filter = material.filterModes[s] != kUnwrittenFilterMode ? material.filterModes[s] : technique.filterModes[s];
								} else if (s == kShadowMaskSlot && technique.shadowMask) {
									filter = technique.filterModes[s];
								} else if (projectedSlot(s) >= 0) {
									address = 3;  // SetupGeometry: wrap, anisotropic (engine notes: samplers)
									filter = 1;
								}
								if (filter == kUnwrittenFilterMode)
									filter = 0;
								const auto index = a_in.resolveTextures ? a_lookups.Sampler(address, filter) : 0u;
								if (index == kInvalidIndex && ((usage.samplers >> s) & 1)) {
									resolved.samplersOk = false;
									break;
								}
								resolved.samplers[s] = index == kInvalidIndex ? 0 : index;
							}
							if (a_cache) {
								auto& entry = a_cache->pairs[pairKey];
								entry.resolved = resolved;
								entry.resolved.recordIndex = kNoRecord;
								entry.resolved.skipReason = kNoSkip;
								entry.hasResolved = true;
							}
						}
						if (!resolved.texturesOk) {
							// The sample is recorded per skipped draw, as it was before, not per distinct pair.
							if (a_out.missingNext < a_out.missingTextures.size())
								a_out.missingTextures[a_out.missingNext++] = resolved.missingTexture;
							if (resolved.deferred)
								++a_out.deferredTextures;
							fail(Skip::Texture);
							return kNoRecord;
						}
						if (!resolved.samplersOk) {
							fail(Skip::Sampler);
							return kNoRecord;
						}
						std::copy(resolved.textures.begin(), resolved.textures.end(), bindings.textures);
						std::copy(resolved.samplers.begin(), resolved.samplers.end(), bindings.samplers);
						mark(0);

						// Constant buffers.
						auto& materialBlock = materialBlocks[pairKey];
						if (kept && !materialBlock.first && !materialBlock.second) {
							// Kept: the pair's two blocks, packed when its sources changed, with this frame's floats written over
							// them as below, and written to the buffer only where the bytes differ.
							auto& cachedPair = a_cache->pairs[pairKey];
							auto keep = [&](BuildCache::PackedGroup& a_packed, PersistentBlock& a_block, std::vector<std::uint32_t>& a_positions, const std::vector<std::uint32_t>& a_floats,
											const ConstantBlock& a_group, const StageLayout& a_layout, std::span<const std::uint8_t> a_table, std::uint64_t a_variables,
											std::uint32_t a_first) {
								if (!a_packed.valid || a_positions.size() != a_floats.size()) {
									PackGroupInto(a_group, a_layout, a_table, a_variables, a_first, a_packed);
									a_positions.clear();
									for (const auto index : a_floats)
										a_positions.push_back(PackedPositionOf(a_layout, a_table, a_variables, a_first, index));
								}
								auto& bytes = a_cache->scratch;
								bytes.assign(a_packed.bytes.begin(), a_packed.bytes.end());
								for (std::size_t i = 0; i < a_positions.size(); ++i) {
									const auto position = a_positions[i];
									const auto index = a_floats[i];
									if (position == ~0u || std::size_t(position) * 4 + 4 > bytes.size() || index >= a_group.floats.size())
										continue;
									const float value = a_group.Written(index) ? a_group.floats[index] : 0.0f;
									std::memcpy(bytes.data() + std::size_t(position) * 4, &value, 4);
								}
								return kept->Place(a_block, bytes.data(), bytes.size(), a_packed.size);
							};
							materialBlock.first = keep(cachedPair.vs, cachedPair.materialVS, cachedPair.vsPatchPositions, a_in.materialPatchedVSFloats, material.vs, LightingVSLayout(),
								blocks.vsTable, kVSGroups[kPerMaterial], kVSFirstVariable[kPerMaterial]);
							materialBlock.second = keep(cachedPair.ps, cachedPair.materialPS, cachedPair.psPatchPositions, a_in.materialPatchedFloats, material.ps, LightingPSLayout(),
								blocks.psTable, kPSGroups[kPerMaterial], kPSFirstVariable[kPerMaterial]);
						}
						if (!materialBlock.first && !materialBlock.second) {
							// The pair's entry was checked (or rebuilt) when the pair was first met this build.
							BuildCache::Pair* cachedPair = nullptr;
							if (a_cache) {
								const auto found = a_cache->pairs.find(pairKey);
								cachedPair = found != a_cache->pairs.end() ? &found->second : nullptr;
							}
							auto pack = [&](const ConstantBlock& a_block, const StageLayout& a_layout, std::span<const std::uint8_t> a_table, std::uint64_t a_variables, std::uint32_t a_first,
											BuildCache::PackedGroup* a_packed) {
								if (a_packed && a_packed->valid) {
									const auto address = block(nullptr, a_packed->size);
									if (address)
										std::memcpy(arena.At(address - base, a_packed->bytes.size()).data(), a_packed->bytes.data(), a_packed->bytes.size());
									return address;
								}
								const auto size = ConstantGroupSize(a_layout, a_table, a_variables, a_first);
								const auto address = block(nullptr, size);
								if (address) {
									const auto out = arena.At(address - base, std::max<std::size_t>(size, 16));
									PackConstantGroup(a_block, a_layout, a_table, a_variables, a_first, out);
									if (a_packed) {
										a_packed->size = size;
										a_packed->bytes.assign(out.begin(), out.end());
										a_packed->valid = true;
									}
								}
								return address;
							};
							const bool vsReused = cachedPair && cachedPair->vs.valid && cachedPair->vsPatchPositions.size() == a_in.materialPatchedVSFloats.size();
							materialBlock.first = pack(material.vs, LightingVSLayout(), blocks.vsTable, kVSGroups[kPerMaterial], kVSFirstVariable[kPerMaterial],
								cachedPair ? &cachedPair->vs : nullptr);
							if (cachedPair && !vsReused) {
								cachedPair->vsPatchPositions.clear();
								for (const auto index : a_in.materialPatchedVSFloats)
									cachedPair->vsPatchPositions.push_back(PackedPositionOf(LightingVSLayout(), blocks.vsTable, kVSGroups[kPerMaterial], kVSFirstVariable[kPerMaterial], index));
							}
							if (vsReused && materialBlock.first) {
								auto out = arena.At(materialBlock.first - base, cachedPair->vs.bytes.size());
								for (std::size_t i = 0; i < cachedPair->vsPatchPositions.size(); ++i) {
									const auto position = cachedPair->vsPatchPositions[i];
									const auto index = a_in.materialPatchedVSFloats[i];
									if (position == ~0u || std::size_t(position) * 4 + 4 > out.size() || index >= material.vs.floats.size())
										continue;
									const float value = material.vs.Written(index) ? material.vs.floats[index] : 0.0f;
									std::memcpy(out.data() + std::size_t(position) * 4, &value, 4);
								}
							}
							// The PS group carries the frame's floats (IBLParams): a reused group has this frame's values
							// written over them, as PackConstantGroup would write them (an unwritten float packs as zero).
							const bool psReused = cachedPair && cachedPair->ps.valid && cachedPair->psPatchPositions.size() == a_in.materialPatchedFloats.size();
							materialBlock.second = pack(material.ps, LightingPSLayout(), blocks.psTable, kPSGroups[kPerMaterial], kPSFirstVariable[kPerMaterial],
								cachedPair ? &cachedPair->ps : nullptr);
							if (cachedPair && !psReused) {
								cachedPair->psPatchPositions.clear();
								for (const auto index : a_in.materialPatchedFloats)
									cachedPair->psPatchPositions.push_back(PackedPositionOf(LightingPSLayout(), blocks.psTable, kPSGroups[kPerMaterial], kPSFirstVariable[kPerMaterial], index));
							}
							if (psReused && materialBlock.second) {
								auto out = arena.At(materialBlock.second - base, cachedPair->ps.bytes.size());
								for (std::size_t i = 0; i < cachedPair->psPatchPositions.size(); ++i) {
									const auto position = cachedPair->psPatchPositions[i];
									const auto index = a_in.materialPatchedFloats[i];
									if (position == ~0u || std::size_t(position) * 4 + 4 > out.size() || index >= material.ps.floats.size())
										continue;
									const float value = material.ps.Written(index) ? material.ps.floats[index] : 0.0f;
									std::memcpy(out.data() + std::size_t(position) * 4, &value, 4);
								}
							}
						}
						std::uint64_t geometryVS = 0, geometryPS = 0;
						// A pipeline's PerGeometry template under bindless: one block per stage (kept, or the build's).
						auto uploadTemplate = [&](const std::vector<std::byte>& a_group, BuildCache::Pipeline* a_pipeline, bool a_pixel) -> std::uint64_t {
							if (a_group.empty())
								return 0;  // the stage does not declare the buffer
							if (kept && a_pipeline)
								return kept->Place(a_pixel ? a_pipeline->geometryPSBlock : a_pipeline->geometryVSBlock, a_group.data(), a_group.size(), a_group.size());
							const auto address = block(nullptr, a_group.size());
							if (address)
								std::memcpy(arena.At(address - base, std::max<std::size_t>(a_group.size(), 16)).data(), a_group.data(), a_group.size());
							return address;
						};
						{
							auto [templateIt, newTemplate] = geometryTemplates.try_emplace(object.pipelineIndex);
							auto& geometryTemplate = templateIt->second;
							BuildCache::Pipeline* cachedPipeline = nullptr;
							if (newTemplate && a_cache) {
								const auto found = a_cache->pipelines.find(object.pipelineIndex);
								cachedPipeline = found != a_cache->pipelines.end() ? &found->second : nullptr;
							}
							if (newTemplate && cachedPipeline && cachedPipeline->hasGeometry) {
								geometryTemplate.vs = cachedPipeline->geometry.vs;
								geometryTemplate.ps = cachedPipeline->geometry.ps;
								geometryTemplate.offsets = cachedPipeline->geometry.offsets;
								if (bindless) {
									geometryTemplate.vsAddress = uploadTemplate(geometryTemplate.vs, cachedPipeline, false);
									geometryTemplate.psAddress = uploadTemplate(geometryTemplate.ps, cachedPipeline, true);
								}
							} else if (newTemplate) {
								// The pipeline's own values, which is everything the objects do not override.
								PackGeometryTemplate(a_tables.geometryConstants[object.pipelineIndex], blocks.vsTable, blocks.psTable, bindless, geometryTemplate);
								if (cachedPipeline) {
									cachedPipeline->geometry.vs = geometryTemplate.vs;
									cachedPipeline->geometry.ps = geometryTemplate.ps;
									cachedPipeline->geometry.offsets = geometryTemplate.offsets;
									cachedPipeline->hasGeometry = true;
								}
								if (bindless) {
									geometryTemplate.vsAddress = uploadTemplate(geometryTemplate.vs, cachedPipeline, false);
									geometryTemplate.psAddress = uploadTemplate(geometryTemplate.ps, cachedPipeline, true);
								}
							}
							if (bindless) {
								// One pair of blocks for the whole pipeline, written when the template was built.
								geometryVS = geometryTemplate.vsAddress;
								geometryPS = geometryTemplate.psAddress;
							} else {
								geometryVS = block(nullptr, geometryTemplate.vs.size());
								geometryPS = block(nullptr, geometryTemplate.ps.size());
								if (geometryVS && geometryPS) {
									auto vsOut = arena.At(geometryVS - base, std::max<std::size_t>(geometryTemplate.vs.size(), 16));
									auto psOut = arena.At(geometryPS - base, std::max<std::size_t>(geometryTemplate.ps.size(), 16));
									std::memcpy(vsOut.data(), geometryTemplate.vs.data(), geometryTemplate.vs.size());
									std::memcpy(psOut.data(), geometryTemplate.ps.data(), geometryTemplate.ps.size());
									PatchObjectGeometry(a_tables, o, renderFlags, eye, previousEye, geometryTemplate.offsets,
										vsOut.subspan(0, geometryTemplate.vs.size()), psOut.subspan(0, geometryTemplate.ps.size()));
								}
							}
							// CS_DCLF_BINDLESS_PARITY=1: the record the shaders read against the group the constant
							// buffer path packs for the same object. The two derive from the same inputs through the
							// same unwritten-component rule, so anything but bit equality is a defect in the record's
							// layout or in the way it is filled, caught on the CPU with no readback and without
							// needing both forms in one run.
							if (bindlessParity && o < objectRecords.Count()) {
								parityVS.assign(geometryTemplate.vs.begin(), geometryTemplate.vs.end());
								parityPS.assign(geometryTemplate.ps.begin(), geometryTemplate.ps.end());
								PatchObjectGeometry(a_tables, o, renderFlags, eye, previousEye, geometryTemplate.offsets, parityVS, parityPS);
								IndirectDraws::Stats parityStats{};
								CheckBindlessRecord(a_tables, o, *objectRecords.At(o), eye, previousEye, geometryTemplate.offsets, parityVS, parityPS, parityStats);
								a_out.bindlessParityChecks += parityStats.bindlessParityChecks;
								a_out.bindlessParityMismatches += parityStats.bindlessParityMismatches;
							}
						}
						std::uint64_t lightBlock = 0;
						if (bindlessDraws) {
							// NumStrictLights 0, and nothing else is read: the zeroed frame slot, for every draw.
							lightBlock = sharedLightBlock;
						} else {
							const auto& lights = a_tables.lights[o];
							auto& cached = lightBlocks[(std::uint64_t(static_cast<std::uint32_t>(lights.roomIndex)) << 32) | lights.shadowBitMask];
							if (!cached) {
								const std::uint32_t header[4] = { 0, static_cast<std::uint32_t>(lights.roomIndex), lights.shadowBitMask, 0 };
								cached = block(nullptr, kStrictLightDataBytes);
								if (cached)
									std::memcpy(arena.At(cached - base, sizeof(header)).data(), header, sizeof(header));
							}
							lightBlock = cached;
						}
						const auto& permutation = a_tables.permutations[object.pipelineIndex];
						// SuppressExternalEmittance is the only per-object bit DCLF puts in this block, and it is
						// read at exactly one place in the whole shader tree - Effect.hlsl's GetLightingColor -
						// never by Lighting.hlsl or anything it includes. So for these pipelines it is dead, and
						// under bindless the block keys on the pipeline alone, which is what makes the binding
						// record identical for a (material, pipeline) pair. The non-bindless control keeps the bit,
						// so CaptureParity::ComparePermutation goes on measuring the real thing against the engine.
						const std::uint32_t extra = permutation.extraShaderDescriptor |
						                            ((!bindless && (object.flags & kObjectSuppressExternalEmittance)) ?
													        static_cast<std::uint32_t>(State::ExtraShaderDescriptors::SuppressExternalEmittance) :
													        0u);
						auto& permutationBlock = permutationBlocks[(std::uint64_t(object.pipelineIndex) << 32) | extra];
						if (!permutationBlock) {
							const std::uint32_t data[8] = { permutation.vertexShaderDescriptor, permutation.pixelShaderDescriptor, extra, permutation.extraFeatureDescriptor, 0, 0, 0, 0 };
							// Kept per pipeline: under bindless nothing in it is per object.
							if (kept)
								permutationBlock = kept->Place(a_cache->pipelines[object.pipelineIndex].permutationBlock, reinterpret_cast<const std::byte*>(data), sizeof(data), sizeof(data));
							else
								permutationBlock = block(data, sizeof(data));
						}
						std::uint64_t alphaBlock = 0;
						if (!bindlessDraws) {
							const std::uint32_t threshold = (object.flags & kObjectAlphaTest) ? (object.flags >> kObjectAlphaThresholdShift) & 0xFF : 0;
							auto& cached = alphaBlocks[threshold];
							if (!cached) {
								const float data[4] = { threshold / 255.0f, 0, 0, 0 };
								cached = block(data, sizeof(data));
							}
							alphaBlock = cached;
						}

						mark(1);
						std::copy(frameVS.begin(), frameVS.end(), bindings.vertexConstants);
						std::copy(framePS.begin(), framePS.end(), bindings.pixelConstants);
						bindings.vertexConstants[kPerTechnique] = blocks.techniqueVS;
						bindings.vertexConstants[kPerMaterial] = materialBlock.first;
						bindings.vertexConstants[kPerGeometry] = geometryVS;
						bindings.vertexConstants[4] = permutationBlock;
						bindings.pixelConstants[kPerTechnique] = blocks.techniquePS;
						bindings.pixelConstants[kPerMaterial] = materialBlock.second;
						bindings.pixelConstants[kPerGeometry] = geometryPS;
						bindings.pixelConstants[3] = lightBlock;
						bindings.pixelConstants[4] = permutationBlock;
						bindings.pixelConstants[11] = alphaBlock;
						if (bindless)
							bindings.pixelConstants[kFrameLightingRegister] = frameLightingBlock;
						// Linear Lighting binds its multiplier per draw only while enabled; otherwise the shader
						// does not read it. It comes from the tables rather than off the property: the value is
						// animated and belongs to the same sample as the emissive colour.
						if (!bindlessDraws) {
							const float multiplier = (linearLighting && o < a_tables.emissiveMult.size()) ? a_tables.emissiveMult[o] : 1.0f;
							auto& emissiveBlock = emissiveBlocks[std::bit_cast<std::uint32_t>(multiplier)];
							if (!emissiveBlock) {
								const float data[4] = { multiplier, 0, 0, 0 };
								emissiveBlock = block(data, sizeof(data));
							}
							bindings.pixelConstants[kLinearLightingRegister] = emissiveBlock;
							// Advanced Skin binds its wetness per draw (the owning actor's, zero otherwise), which the
							// DCLF_BINDLESS_DRAW builds read from the object record instead.
							if (globals::features::skin.loaded) {
								const auto wetness = o < a_tables.skinWetness.size() ? a_tables.skinWetness[o] : std::array<float, 4>{};
								auto& wetnessBlock = skinBlocks[wetness];
								if (!wetnessBlock)
									wetnessBlock = block(wetness.data(), sizeof(wetness));
								bindings.pixelConstants[kSkinRegister] = wetnessBlock;
							}
						}
						bool constantsOk = true;
						for (std::uint32_t b = 0; b < kConstantBufferRegisters; ++b) {
							if (((usage.vertexConstants >> b) & 1) && !bindings.vertexConstants[b]) {
								constantsOk = false;
								a_out.missingVertexConstants |= 1u << b;
							}
							if (((usage.pixelConstants >> b) & 1) && !bindings.pixelConstants[b]) {
								constantsOk = false;
								a_out.missingPixelConstants |= 1u << b;
							}
						}
						if (!constantsOk) {
							fail(Skip::Constants);
							return kNoRecord;
						}

						// Kept records are immutable templates. Current-frame descriptor indices live only in
						// commit-owned upload copies, so a frame texture change cannot dirty the scene cache.
						std::array<std::uint64_t, 2> patchMask{};
						if (kept) {
							for (const auto t : resolved.patchRegisters) {
								bindings.textures[t] = 0;
								patchMask[t / 64] |= 1ull << (t % 64);
							}
						}
						if (dedup && recordIndex != kNoRecord) {
							// CS_DCLF_DEDUP_PARITY=1: the pair already has a record and this draw just rebuilt
							// one from scratch, so they must be byte-identical. This is the direct answer to
							// "is the record really the same for every draw of a pair", and the only check that
							// would catch a per-object dependency nobody has noticed.
							++a_out.recordParityChecks;
							const DrawBindings& held = kept ? kept->records.Get()[recordIndex] : records[recordIndex];
							if (std::memcmp(&held, &bindings, sizeof(DrawBindings)) != 0 && a_out.recordParityMismatches++ == 0)
								logger::warn("[DCLF] record dedup parity: object {} rebuilds a different record than its (material {}, pipeline {}) pair holds",
									o, object.materialIndex, object.pipelineIndex);
						} else if (kept) {
							// The pair's slot, its own for as long as the pair is cached; written only when the record changed.
							auto& cachedPair = a_cache->pairs[pairKey];
							if (cachedPair.recordSlot == kNoRecord)
								cachedPair.recordSlot = kept->AcquireRecord();
							if (cachedPair.recordSlot == kNoRecord) {
								fail(Skip::RecordCapacity);
								return kNoRecord;
							}
							kept->WriteRecord(cachedPair.recordSlot, bindings, patchMask);
							cachedPair.cleanKey = pairVersions;
							cachedPair.clean = true;
							recordIndex = cachedPair.recordSlot;
							if (dedup)
								resolved.recordIndex = recordIndex;
						} else {
							if (a_slot != kNoRecord) {
								// A resident region's pair: its stable slot (the records up to the region's slot count are its).
								recordIndex = a_slot;
								records[a_slot] = bindings;
							} else {
								if (records.size() >= a_in.addresses.recordCapacity) {
									fail(Skip::RecordCapacity);
									return kNoRecord;
								}
								recordIndex = static_cast<std::uint32_t>(records.size());
								records.push_back(bindings);
							}
							for (const auto t : resolved.patchRegisters)
								a_out.framePatches.emplace_back(recordIndex, t);
							if (dedup)
								resolved.recordIndex = recordIndex;
						}
					}

				return recordIndex;
			};

			// The resident region (BuildCache::ResidentRegion; drawcall-limit-fix.md, "Persistent resident draws"): the resident
			// objects' draw inputs, kept across frames and changed only by the tables' change log (Tables::changeLog), a
			// pipeline's set index or a pair's record failing. Its pairs' records sit at stable slots, assembled here once per
			// pair; the loop below skips its objects, and the commit uploads the inputs only when their version is new.
			ResidentRegion* region = a_cache && ResidentDrawsEnabled() && dedup && bindless && !BuildParityEnabled() ? &a_cache->region : nullptr;
			if (!region && a_cache && a_cache->region.cursor.active)
				a_cache->region.Reset();
			std::size_t regionInputs = 0, regionDraws = 0;
			// The whole scene with kept bindings (its pairs' records are theirs); the Z-prepass's only where its gate is not the
			// colour epoch's last frame (withholding, or no hybrid): with that gate, what is not resident stays the loop's.
			const bool wholeScene = kept && WholeSceneRegionEnabled() && (!depthOnly || !frameHybrid || a_in.withholding);
			TracyCZoneN(residentRegionZone, "CS.DCLF.BuildMain.ResidentRegion", true);
			if (region) {
				auto& r = *region;
				auto pairKeyOf = [](const ObjectRecord& a_object) { return (std::uint64_t(a_object.materialIndex) << 32) | a_object.pipelineIndex; };
				auto residentAt = [&](std::uint32_t o) { return o < a_tables.residentSlot.size() && a_tables.residentSlot[o] != 0; };
				// What the region can hold: a record the loop would draw as one input with its pair's record (no decal slot, no
				// face stream, no position in the second stream) - a resident's, or with the whole scene anyone's - and with the
				// whole scene, the depth segment's cull-only candidates.
				auto eligible = [&](std::uint32_t o) {
					if (o >= a_tables.objects.size() || o >= kMaxObjects)
						return false;
					const bool resident = residentAt(o);
					if (!resident && !wholeScene)
						return false;
					const auto& object = a_tables.objects[o];
					if (object.flags & (kObjectFree | kObjectShadowOnly))
						return false;
					if (object.flags & kObjectNoBindings)
						return wholeScene && depthOnly && frameHybrid && object.geometryIndex < a_tables.geometries.size();
					if ((resident && !(object.flags & kObjectNativeVisible)) || ObjectDecalGroup(object.flags))
						return false;
					if (object.pipelineIndex >= pipelineBlocks.size() || object.pipelineIndex >= a_tables.pipelines.size() || object.geometryIndex >= a_tables.geometries.size())
						return false;
					const auto& geometry = a_tables.geometries[object.geometryIndex];
					if (!geometry.vertexAddress || !geometry.indexAddress)
						return false;
					return !IsFaceObject(a_tables, o) && FaceStreamGeometry(a_tables, o, a_in.addresses.facePositions) == ~0u &&
					       !(a_tables.pipelines[object.pipelineIndex].vertexLayout & kPositionInSecondStream);
				};
				// The input the loop would write for it, drawable while its pipeline is in the set and its pair's record built.
				auto entryOf = [&](std::uint32_t o, DrawInput& a_input) -> std::uint8_t {
					const auto& object = a_tables.objects[o];
					if (object.flags & kObjectNoBindings) {
						// A cull-only candidate: its bounds for the culling, nothing to draw (the loop's input for it, as it was).
						a_input = { 0, 0, object.geometryIndex, object.flags, { object.boundCenter[0], object.boundCenter[1], object.boundCenter[2] }, object.boundRadius,
							o, 0 };
						SetFadeRow(a_input, a_tables, o);
						return 0;
					}
					const auto& blocks = pipelineBlocks[object.pipelineIndex];
					const auto pair = r.pairs.find(pairKeyOf(object));
					const bool drawable = blocks.setIndex != Lookups::kNone && pair != r.pairs.end() && pair->second.ok;
					const std::uint32_t partitions = PartitionsOf(a_tables, o);
					a_input = { drawable ? blocks.setIndex : 0u, drawable ? pair->second.slot : 0u, object.geometryIndex, object.flags | (drawable ? kInputDrawable : 0u),
						{ object.boundCenter[0], object.boundCenter[1], object.boundCenter[2] }, object.boundRadius, o, 0, partitions };
					if (depthOnly)
						SetFadeRow(a_input, a_tables, o);
					return drawable ? static_cast<std::uint8_t>(partitions ? std::popcount(partitions) : 1) : std::uint8_t{ 0 };
				};
				auto acquire = [&](std::uint64_t a_key) {
					auto [pair, fresh] = r.pairs.try_emplace(a_key);
					if (fresh && kept) {
						// Its record is the pair's kept one, whose slot the assembly below gives it.
						pair->second.slot = kNoRecord;
						pair->second.ok = false;
					} else if (fresh) {
						if (!r.freeSlots.empty()) {
							pair->second.slot = r.freeSlots.back();
							r.freeSlots.pop_back();
						} else {
							pair->second.slot = r.slotCount++;
						}
					}
					++pair->second.count;
					const auto pipeline = static_cast<std::uint32_t>(a_key);
					auto [state, freshPipeline] = r.pipelines.try_emplace(pipeline, std::pair{ pipelineBlocks[pipeline].setIndex, 0u });
					++state->second.second;
				};
				auto release = [&](std::uint64_t a_key) {
					if (a_key == kNoPair)
						return;
					if (const auto pair = r.pairs.find(a_key); pair != r.pairs.end() && --pair->second.count == 0) {
						if (!kept)
							r.freeSlots.push_back(pair->second.slot);
						r.pairs.erase(pair);
					}
					if (const auto state = r.pipelines.find(static_cast<std::uint32_t>(a_key)); state != r.pipelines.end() && --state->second.second == 0)
						r.pipelines.erase(state);
				};
				// An entry's draws, with the region's totals.
				auto setDraws = [&](std::uint32_t i, std::uint8_t a_draws) {
					r.undrawable += (a_draws ? 0 : 1) - (r.drawsOf[i] ? 0 : 1);
					r.draws = r.draws - r.drawsOf[i] + a_draws;
					r.drawsOf[i] = a_draws;
				};
				auto remove = [&](std::uint32_t o) {
					const std::uint32_t i = r.EntryOf(o);
					if (i == kNoRegion)
						return;
					release(r.pairOf[i]);
					r.touched.push_back(o);
					r.undrawable -= r.drawsOf[i] ? 0 : 1;
					r.draws -= r.drawsOf[i];
					// The entry's columns follow the entry the region moves into its place.
					const auto removal = r.Remove(o);
					r.pairOf[removal.at] = r.pairOf[removal.from];
					r.drawsOf[removal.at] = r.drawsOf[removal.from];
					r.pairOf.pop_back();
					r.drawsOf.pop_back();
				};
				auto upsert = [&](std::uint32_t o) {
					if (!eligible(o)) {
						remove(o);
						return;
					}
					r.Cover(a_tables.objects.size());
					const auto& object = a_tables.objects[o];
					const std::uint64_t key = (object.flags & kObjectNoBindings) ? kNoPair : pairKeyOf(object);
					std::uint32_t i = r.indexOf[o];
					if (i == kNoRegion) {
						// Past the region's share of the buffers, or of the record slots, it stays with the loop.
						const std::uint32_t partitions = PartitionsOf(a_tables, o);
						const std::size_t objectDraws = partitions ? static_cast<std::size_t>(std::popcount(partitions)) : 1;
						const bool slot = kept || r.pairs.contains(key) || !r.freeSlots.empty() || r.slotCount < a_in.addresses.recordCapacity / 2;
						const std::size_t inputLimit = wholeScene ? kMaxInputs - kLoopReserve : kMaxInputs / 2;
						const std::size_t drawLimit = wholeScene ? kMaxDraws - kLoopReserve : kMaxDraws / 2;
						if (r.inputs.Size() >= inputLimit || r.draws + objectDraws > drawLimit || !slot)
							return;
						i = r.Add(o);
						r.pairOf.push_back(key);
						r.drawsOf.push_back(0);
						++r.undrawable;
						if (key != kNoPair)
							acquire(key);
					} else if (r.pairOf[i] != key) {
						release(r.pairOf[i]);
						if (key != kNoPair)
							acquire(key);
						r.pairOf[i] = key;
					}
					DrawInput fresh{};
					setDraws(i, entryOf(o, fresh));
					if (reuseKeptStorage)
						r.inputs.Set(i, fresh);
					else {
						r.inputs.Mutable()[i] = fresh;
						r.inputs.Mark(i);
					}
					r.touched.push_back(o);
				};
				// The Z-prepass draws depth only for what the colour epoch drew last frame (the loop's rule below).
				auto drewLast = [&](std::uint32_t o) { return drewLastFrame(o); };
				r.touched.clear();
				// What the buffer holds is the version it was sent: what changes from here on is sent alone.
				r.inputs.BeginBuild(a_in.residentUploaded);
				const bool resync = !r.cursor.Continues(a_tables.changeLog, a_in.tablesGeneration) || r.depth != depthOnly || r.indexOf.size() > a_tables.objects.size() ||
				                    r.wholeScene != wholeScene;
				// A resident the depth segment has not seen the colour epoch draw waits (the loop's rule); anything else is written.
				auto take = [&](std::uint32_t o) {
					if (!residentAt(o) && !wholeScene)
						remove(o);
					else if (depthOnly && residentAt(o) && (o >= r.indexOf.size() || r.indexOf[o] == kNoRegion) && !drewLast(o))
						r.pending.Add(o);
					else
						upsert(o);
					r.touched.push_back(o);
				};
				if (resync) {
					// Every slot read again: the first build, new tables, or a log this segment fell behind.
					r.Reset();
					r.cursor.Restart(a_in.tablesGeneration);
					r.depth = depthOnly;
					r.wholeScene = wholeScene;
					r.indexOf.assign(a_tables.objects.size(), kNoRegion);
					++a_out.residentResyncs;
					for (std::uint32_t o = 0; o < a_tables.objects.size(); ++o)
						take(o);
				} else {
					if (depthOnly) {
						// Joins the colour epoch has drawn since: their depth may be drawn now.
						for (std::size_t k = 0; k < r.pending.Size();) {
							const std::uint32_t slot = r.pending.list[k];
							if (slot >= a_tables.residentSlot.size() || !a_tables.residentSlot[slot] || (slot < r.indexOf.size() && r.indexOf[slot] != kNoRegion)) {
								r.pending.RemoveAt(k);
							} else if (drewLast(slot)) {
								r.pending.RemoveAt(k);
								upsert(slot);
								r.touched.push_back(slot);
							} else {
								++k;
							}
						}
					}
					// The log since this region's last build, whenever its changes were made.
					// What a draw input carries: its placement and fade row, its bindings, its geometry and partitions, its residency.
					constexpr std::uint32_t kInputCauses = kChangePlacement | kChangeBindings | kChangeSkin | kChangeGeometry | kChangeMembership;
					for (const auto& change : r.cursor.Unread(a_tables.changeLog))
						if (change.causes & kInputCauses)
							take(change.slot);
				}
				r.cursor.Advance(a_tables.changeLog);
				// A pipeline whose set index changed, and the pairs whose record could or could no longer be built: their
				// entries are written again (both rare).
				std::vector<std::uint32_t> changedPipelines;
				for (auto& [pipeline, state] : r.pipelines)
					if (pipeline < pipelineBlocks.size() && pipelineBlocks[pipeline].setIndex != state.first) {
						state.first = pipelineBlocks[pipeline].setIndex;
						changedPipelines.push_back(pipeline);
					}
				if (!kept)
					records.resize(r.slotCount);
				std::vector<std::uint64_t> changedPairs;
				currentObject = ~0u;
				for (auto& [key, pair] : r.pairs) {
					const auto pipeline = static_cast<std::uint32_t>(key);
					bool ok = pipeline < pipelineBlocks.size() && pipelineBlocks[pipeline].setIndex != Lookups::kNone;
					std::uint32_t slot = pair.slot;
					if (ok) {
						ObjectRecord object{};
						object.materialIndex = static_cast<std::uint32_t>(key >> 32);
						object.pipelineIndex = pipeline;
						if (kept) {
							slot = assembleRecord(~0u, object, pipelineBlocks[pipeline], kNoRecord);
							ok = slot != kNoRecord;
						} else {
							ok = assembleRecord(~0u, object, pipelineBlocks[pipeline], pair.slot) == pair.slot;
						}
					}
					if (ok != pair.ok || (ok && slot != pair.slot)) {
						pair.ok = ok;
						if (ok)
							pair.slot = slot;
						changedPairs.push_back(key);
					}
				}
				if (!changedPipelines.empty() || !changedPairs.empty()) {
					auto& inputs = r.inputs.Mutable();
					for (std::uint32_t i = 0; i < inputs.size(); ++i) {
						const auto key = r.pairOf[i];
						if (std::find(changedPairs.begin(), changedPairs.end(), key) == changedPairs.end() &&
							std::find(changedPipelines.begin(), changedPipelines.end(), static_cast<std::uint32_t>(key)) == changedPipelines.end())
							continue;
						setDraws(i, entryOf(inputs[i].objectIndex, inputs[i]));
						r.inputs.Mark(i);
						r.touched.push_back(inputs[i].objectIndex);
					}
				}
				// CS_DCLF_RESIDENT_DRAW_PARITY: every entry written again from the tables and compared, and every resident the
				// region should hold looked for.
				if (ResidentDrawParityEnabled() && frameNumber % 60 == 0) {
					const auto& inputs = r.inputs.Get();
					for (std::uint32_t i = 0; i < inputs.size(); ++i) {
						DrawInput expected;
						const std::uint32_t o = inputs[i].objectIndex;
						const bool known = o < a_tables.objects.size() && eligible(o);
						const auto draws = known ? entryOf(o, expected) : std::uint8_t{ 0 };
						++a_out.residentParityChecks;
						if (!known || draws != r.drawsOf[i] || std::memcmp(&expected, &inputs[i], sizeof(DrawInput)) != 0 || r.indexOf[o] != i) {
							// [TEMP] the first few, with why.
							if (a_out.residentParityMismatches++ < 6)
								logger::info("[DCLF][TEMP] {} region parity: entry {} object {}: {} (resident {}, flags {:#x} vs {:#x}, pipeline {} vs {}, record {} vs {}, draws {} vs {}, indexOf {})",
									depthOnly ? "depth" : "colour", i, o, !known ? "not eligible" : "differs", o < a_tables.residentSlot.size() ? a_tables.residentSlot[o] : 9,
									inputs[i].flags, expected.flags, inputs[i].pipelineIndex, expected.pipelineIndex, inputs[i].recordIndex, expected.recordIndex, r.drawsOf[i], draws,
									o < r.indexOf.size() ? r.indexOf[o] : ~0u);
						}
					}
					{
						std::size_t residentCount = 0;
						for (const auto flag : a_tables.residentSlot)
							residentCount += flag;
						logger::info("[DCLF][TEMP] {} region: {} entries, {} pending, {} resident slots, log at {} of {}", depthOnly ? "depth" : "colour", inputs.size(),
							r.pending.Size(), residentCount, r.cursor.position, a_tables.changeLog.End());
					}
					for (std::uint32_t o = 0; o < a_tables.objects.size(); ++o)
						if (eligible(o) && (o >= r.indexOf.size() || r.indexOf[o] == kNoRegion) && !r.pending.Contains(o))
							++a_out.residentMissing;
				}
				// Every slot this build touched: the region's, the loop's (loopList) or nobody's, and whether it is a candidate only.
				if (r.loopIndex.size() < a_tables.objects.size()) {
					r.loopIndex.resize(a_tables.objects.size(), kNoRegion);
					r.candidate.resize(a_tables.objects.size(), 0);
				}
				for (const std::uint32_t o : r.touched) {
					if (o >= r.loopIndex.size())
						continue;
					const auto flags = o < a_tables.objects.size() ? a_tables.objects[o].flags : kObjectFree;
					const bool inRegion = o < r.indexOf.size() && r.indexOf[o] != kNoRegion;
					const bool isCandidate = !(flags & kObjectFree) && (flags & (kObjectNoBindings | kObjectShadowOnly));
					bool loop = false;
					if (!inRegion && !(flags & (kObjectFree | kObjectShadowOnly))) {
						if (flags & kObjectNoBindings)
							loop = depthOnly && frameHybrid;  // the depth segment's cull-only input, where the region has no room
						else
							loop = !(depthOnly && ObjectDecalGroup(flags));  // decals are the colour segment's alone
					}
					if (loop && r.loopIndex[o] == kNoRegion) {
						r.loopIndex[o] = static_cast<std::uint32_t>(r.loopList.size());
						r.loopList.push_back(o);
					} else if (!loop && r.loopIndex[o] != kNoRegion) {
						const std::uint32_t at = r.loopIndex[o];
						const std::uint32_t tail = r.loopList.back();
						r.loopList[at] = tail;
						r.loopIndex[tail] = at;
						r.loopList.pop_back();
						r.loopIndex[o] = kNoRegion;
					}
					if (r.candidate[o] != (isCandidate ? 1 : 0)) {
						r.candidates += isCandidate ? 1 : std::size_t(-1);
						r.candidate[o] = isCandidate ? 1 : 0;
					}
				}
				// What the region draws: the colour segment's marks for the slots this build touched (the rest are as they were),
				// and the build's per-object states for set parity alone.
				const auto& inputs = r.inputs.Get();
				if (marks)
					for (const std::uint32_t o : r.touched) {
						const bool on = o < r.indexOf.size() && r.indexOf[o] != kNoRegion && r.drawsOf[r.indexOf[o]] && nativeDrawn(o);
						marks->Set(o, on && o < a_tables.objectGeometry.size() ? a_tables.objectGeometry[o] : nullptr, on);
					}
				if (!a_out.objectState.empty()) {
					for (std::uint32_t i = 0; i < inputs.size(); ++i)
						if (inputs[i].objectIndex < a_out.objectState.size())
							a_out.objectState[inputs[i].objectIndex] =
								r.drawsOf[i] ? kObjectStateDrawable : static_cast<std::uint8_t>(r.pairOf[i] == kNoPair ? Skip::CandidateOnly : Skip::Pipeline);
					// The candidates nobody submits, as the loop used to record them.
					for (std::uint32_t o = 0; o < r.candidate.size() && o < a_out.objectState.size(); ++o)
						if (r.candidate[o] && a_out.objectState[o] == kObjectStateAbsent)
							a_out.objectState[o] = static_cast<std::uint8_t>(Skip::CandidateOnly);
				}
				// Every candidate is one whether the loop sees it or not (the report's "candidate-only").
				a_out.skipped[static_cast<std::size_t>(Skip::CandidateOnly)] += static_cast<std::uint32_t>(r.candidates);
				a_out.residentUndrawable = static_cast<std::uint32_t>(r.undrawable);
				a_out.resident = r.inputs.View();
				a_out.residentDraws = static_cast<std::uint32_t>(r.draws);
				a_out.residentPairs = static_cast<std::uint32_t>(r.pairs.size());
				regionInputs = inputs.size();
				regionDraws = r.draws;
			}
			TracyCZoneEnd(residentRegionZone);

			std::vector<std::uint32_t> loopDrawn;  // what the loop draws this build (DrawnMarks)
			TracyCZoneN(objectLoopZone, "CS.DCLF.BuildMain.ObjectLoop", true);
			// With a region, the loop visits only what the region leaves it (ResidentRegion::loopList); the candidates were
			// counted by the region.
			const std::vector<std::uint32_t>* loopObjects = region ? &region->loopList : nullptr;
			const std::size_t loopCount = loopObjects ? loopObjects->size() : a_tables.objects.size();
			for (std::size_t n = 0; n < loopCount; ++n) {
				const std::uint32_t o = loopObjects ? (*loopObjects)[n] : static_cast<std::uint32_t>(n);
				if (o >= a_tables.objects.size())
					continue;
				currentObject = o;
				const auto& object = a_tables.objects[o];
				// A resident region's object: its draw input is the region's (above).
				if (region && o < region->indexOf.size() && region->indexOf[o] != kNoRegion)
					continue;
				// A free slot is no object: not even a culling candidate.
				if (object.flags & kObjectFree)
					continue;
				// A shadow-only record is no main-pass object at all, not even a culling candidate.
				if (object.flags & kObjectShadowOnly) {
					if (!region)
						skip(Skip::CandidateOnly);
					continue;
				}
				if (object.flags & kObjectNoBindings) {
					// A culling candidate with no material or pipeline entry; its indices are meaningless.
					// The depth segment still submits it cull-only, with its bounds: that is what the tables
					// carry the whole tracked set for, and what the culling is measured against the engine
					// with.
					if (depthOnly && frameHybrid && o < kMaxObjects && drawInputs.size() + regionInputs < kMaxInputs) {
						drawInputs.push_back({ 0, 0, object.geometryIndex, object.flags,
							{ object.boundCenter[0], object.boundCenter[1], object.boundCenter[2] }, object.boundRadius,
							static_cast<std::uint32_t>(o), 0 });
						SetFadeRow(drawInputs.back(), a_tables, o);
					}
					if (!region)
						skip(Skip::CandidateOnly);
					continue;
				}
				// Decals never reach the depth segment: they are not occluders, and they are drawn by the
				// colour segment's second pass (BuildDrawsCS.hlsl, MainOpaquePass::Record).
				const std::uint32_t decalGroup = ObjectDecalGroup(object.flags);
				if (decalGroup && (depthOnly || o >= a_tables.decalOrdinal.size() || a_tables.decalOrdinal[o] >= decalCount[(decalGroup - 1) & 1]))
					continue;
				// A decal that cannot be drawn this epoch must still reach BuildDraws, so that its slot is
				// written as a zero-count draw rather than left holding whatever a previous frame put there.
				// This guard does that on every `continue` between here and the drawable push below.
				struct DecalSlot
				{
					std::vector<DrawInput>* inputs = nullptr;
					DrawInput blank{};
					~DecalSlot()
					{
						if (inputs)
							inputs->push_back(blank);
					}
				} decalSlot;
				if (decalGroup) {
					decalSlot.inputs = &drawInputs;
					decalSlot.blank = { 0, 0, object.geometryIndex, object.flags,
						{ object.boundCenter[0], object.boundCenter[1], object.boundCenter[2] }, object.boundRadius,
						static_cast<std::uint32_t>(o), a_tables.decalOrdinal[o] };
				}
				const auto& blocks = pipelineBlocks[object.pipelineIndex];
				if (blocks.setIndex == Lookups::kNone) {
					skip(Skip::Pipeline);
					continue;
				}
				const auto& geometry = a_tables.geometries[object.geometryIndex];
				if (!geometry.vertexAddress || !geometry.indexAddress) {
					skip(Skip::Geometry);
					continue;
				}
				// The object's table index addresses its record and its visibility word, and it travels to
				// the shaders as the draw's third root constant. Past the table's capacity it addresses
				// neither, and robust buffer access turns that into zeros - a world matrix of zeros collapses
				// the object to a point at the eye with no other sign. Both caps come before the cull-only
				// push below, which used to reach the inputs buffer without passing any check at all.
				if (o >= kMaxObjects || drawInputs.size() + regionInputs >= kMaxInputs) {
					skip(Skip::Capacity);
					continue;
				}
				// The Z-prepass must write depth for exactly the objects the native loop is leaving to DCLF,
				// which is the set the colour epoch drew in the frame before (SkipNativePass uses the same
				// rule). Writing depth for anything else strands it: the colour epoch may not draw it, and
				// the native draw that would have cannot either, because its own EQUAL test now compares
				// against a depth DCLF computed rather than the one the native prepass wrote. Such an object
				// keeps its depth but is never shaded, which is what left the architecture flat and grey.
				if (depthOnly && !drewLastFrame(o)) {
					// Cull-only: the object still goes to the culling, because the depth segment is where the
					// verdict for every candidate is decided and published, and a candidate left out here
					// would reach the colour segment with no verdict at all. What it does not get is a
					// bindings record, which is the expensive part and the only part a draw needs.
					drawInputs.push_back({ 0, 0, object.geometryIndex, object.flags,
						{ object.boundCenter[0], object.boundCenter[1], object.boundCenter[2] }, object.boundRadius,
						static_cast<std::uint32_t>(o), 0 });
					SetFadeRow(drawInputs.back(), a_tables, o);
					skip(Skip::NotSkippedNatively);
					continue;
				}
				// A skin of several partitions writes one sequence per partition drawn. A decal has one slot,
				// so a decal that is such a skin cannot be drawn by the decal pass.
				const std::uint32_t partitions = PartitionsOf(a_tables, o);
				if (decalGroup && partitions) {
					skip(Skip::Geometry);
					continue;
				}
				// A face shape draws only with its positions as the second stream, and a pipeline that reads its
				// position from the second stream only with them: the slot would fall back to the geometry's own
				// buffer and draw its other attributes as positions.
				const std::uint32_t streamIndex = FaceStreamGeometry(a_tables, o, a_in.addresses.facePositions);
				if (streamIndex == ~0u && (IsFaceObject(a_tables, o) || (a_tables.pipelines[object.pipelineIndex].vertexLayout & kPositionInSecondStream))) {
					skip(Skip::Geometry);
					continue;
				}
				// The draw cap: BuildDraws writes into the first half of the sequence buffer, and the count
				// is ExecuteIndirect's maxCount. Decals have their own ranges.
				const std::size_t objectDraws = partitions ? static_cast<std::size_t>(std::popcount(partitions)) : 1;
				if (!decalGroup && sequences.size() + regionDraws + objectDraws > kMaxDraws) {
					skip(Skip::Capacity);
					continue;
				}
				const std::uint32_t recordIndex = assembleRecord(o, object, blocks, kNoRecord);
				if (recordIndex == kNoRecord)
					continue;
				// The CPU template of what BuildDraws writes (checked with CS_DCLF_BUILD_PARITY).
				mark(2);
				auto sequence = a_tables.draws[o];
				sequence.pipelineIndex = blocks.setIndex;
				sequence.objectIndex = o;
				sequence.bindingsAddress = a_in.addresses.records + std::uint64_t(recordIndex) * sizeof(DrawBindings);
				if (streamIndex != ~0u)
					SetSequenceStream(sequence, a_tables, o, a_in.addresses.facePositions);
				if (decalGroup) {
					const std::uint32_t ordinal = a_tables.decalOrdinal[o];
					decalSlot.inputs = nullptr;  // drawn: the blank is not needed
					drawInputs.push_back({ blocks.setIndex, recordIndex, object.geometryIndex,
						object.flags | kInputDrawable,
						{ object.boundCenter[0], object.boundCenter[1], object.boundCenter[2] }, object.boundRadius,
						static_cast<std::uint32_t>(o), ordinal, 0, streamIndex });
					decalTemplates[(decalGroup - 1) & 1][ordinal] = sequence;
					++a_out.decalsDrawn;
					if (o < a_out.objectState.size())
						a_out.objectState[o] = kObjectStateDecal;
				} else {
					if (o < a_out.objectState.size())
						a_out.objectState[o] = kObjectStateDrawable;
					drawInputs.push_back({ blocks.setIndex, recordIndex, object.geometryIndex,
						object.flags | kInputDrawable,
						{ object.boundCenter[0], object.boundCenter[1], object.boundCenter[2] }, object.boundRadius,
						static_cast<std::uint32_t>(o), 0, partitions, streamIndex });
					if (depthOnly)
						SetFadeRow(drawInputs.back(), a_tables, o);
					if (!partitions) {
						sequences.push_back(sequence);
					} else {
						ForEachDrawnGeometry(a_tables, object.geometryIndex, partitions, [&](std::uint32_t a_slot) {
							auto partitionSequence = sequence;
							SetSequenceGeometry(partitionSequence, a_tables.geometries[a_slot], streamIndex != ~0u);
							sequences.push_back(partitionSequence);
						});
					}
				}
				// Only what BuildDraws will actually write a sequence for counts as drawn. The tables now hold
				// the whole tracked set, so a candidate the gate drops must not be recorded here: the native
				// loop would skip its pass (it has none while the engine culls it, but it regains one the
				// moment the engine sees it again) and, worse, the Z-prepass draws exactly what the colour
				// epoch drew last frame, so a stale mark would write depth for an object nothing then shades.
				// The gate is a per-object flag test and so is predictable here; frustum rejection is not,
				// which is what the false-negative counter exists to catch.
				if (marks && o < a_tables.objectGeometry.size() && nativeDrawn(o)) {
					marks->Set(o, a_tables.objectGeometry[o], true);
					if (marks->loopStamp.size() <= o)
						marks->loopStamp.resize(std::size_t(o) + 1, 0);
					marks->loopStamp[o] = marks->serial;
					loopDrawn.push_back(o);
				}
				// The indirect draw fetches vertices and indices through the buffer's device address and size:
				// a slice that does not cover the draw reads zeros, and the object collapses without any
				// other sign. Checked here because nothing on the D3D11 side sees the Vulkan slice.
				const std::uint64_t vertexNeeded = std::uint64_t(geometry.vertexCount) * geometry.vertexStride;
				const std::uint64_t indexNeeded = (std::uint64_t(geometry.firstIndex) + geometry.indexCount) * sizeof(std::uint16_t);
				if (geometry.vertexBytes < vertexNeeded || geometry.indexBytes < indexNeeded) {
					if (a_out.shortBuffers++ == 0)
						a_out.shortBuffer = { o, vertexNeeded, indexNeeded };
				}
				// Per DRAW: the sequence, the draw input, the drawn mark and the slice check.
				mark(3);
			}
			TracyCZoneEnd(objectLoopZone);

			// The loop's marks: what it drew last build and not now is not drawn, unless the region draws it.
			if (marks) {
				auto& m = *marks;
				for (const std::uint32_t o : m.loopDrawn) {
					if (o < m.loopStamp.size() && m.loopStamp[o] == m.serial)
						continue;
					const auto* regionOf = region && o < region->indexOf.size() && region->indexOf[o] != kNoRegion ? region : nullptr;
					if (regionOf && regionOf->drawsOf[regionOf->indexOf[o]] && nativeDrawn(o))
						continue;
					m.Set(o, nullptr, false);
				}
				m.loopDrawn = std::move(loopDrawn);
				// The changes since the version the render thread applied, or every slot when it holds nothing the journal can
				// build on.
				const auto snapshot = m.changes.Take();
				const std::uint64_t held = a_in.drawnCommitted;
				a_out.drawnValid = true;
				a_out.drawnFull = held < snapshot.floor || held > snapshot.version;
				a_out.drawnVersion = snapshot.version;
				a_out.drawnBase = held;
				snapshot.ForEachRun(held, m.drawn.size(), [&](std::uint64_t a_first, std::uint64_t a_count) {
					for (auto slot = static_cast<std::uint32_t>(a_first); slot < a_first + a_count; ++slot)
						a_out.drawnChanges.push_back({ slot, m.geometry[slot], m.drawn[slot] != 0 });
				});
			}
			UpdateGeometryDraws(a_geometries, a_in.tablesHeld.geometries, a_tables, a_in.tablesGeneration, frameNumber, a_out.geometryDraws);
			AppendFaceStreams(a_tables, a_in.addresses.facePositions, a_out.geometryDraws);
			if (a_in.addresses.facePositions)
				a_out.faceStreams = a_tables.faceStreams;
			UpdateBones(a_bones, a_in.tablesHeld.bones, a_tables, a_in.tablesGeneration, a_out.bones);
			if (kept) {
				a_out.persistent = true;
				a_out.keptConstants = kept->constants.View();
				a_out.keptRecords = kept->records.View();
				a_out.patchMasks = kept->patchMasks;
				a_out.recordsHeld = static_cast<std::uint32_t>(kept->records.Size() - kept->recordFree.size());
				a_out.blocksWritten = kept->blocksWritten;
				a_out.recordsWritten = kept->recordsRewritten;
				++a_cache->persistentBuilds;
				a_cache->persistentBlocks += kept->blocksWritten;
				a_cache->persistentRecords += kept->recordsRewritten;
				if (PersistentParityEnabled() && ParityDue(frameNumber)) {
					MainPayload reference;
					BuildMainPayload(a_in, a_tables, a_lookups, reference);
					CheckPersistentBindings(a_out, reference, *a_cache, base, a_in.frameTextures, a_cache->persistentParityChecks, a_cache->persistentParityMismatches,
						a_cache->persistentParityFirst);
				}
			}
			mark(5);
		}

		/**
		 * @brief The shadow epoch's frame-shared part and its per-mode inputs, from the tables and the lookups:
		 * pure. The per-view blocks are the commit's, because the views are captured while the engine draws
		 * them, after the build may have started.
		 */
		// The objects a mode's inputs draw, as the geometry the native shadow loop withholds (PassCapture).
		std::shared_ptr<const PassCapture::ClaimSet> ShadowClaimSet(const std::vector<DrawInput>& a_inputs, const SceneStore::Tables& a_tables)
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
		std::uint32_t ModeBitsOf(std::uint32_t a_mode)
		{
			return a_mode == kSkyMode ? 0u : ShadowModeBits(PassCapture::kFirstShadowMode + a_mode);
		}

		/**
		 * @brief The next frame's sun entry exclusion, from the candidates and the cascades' mode's inputs, which are
		 * what that mode's claims hold: a candidate stays in the cascade culls when one of its table objects casts (no
		 * kObjectNoShadow) and is not an input, since the engine must then still draw it. Null without candidates.
		 */
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

		std::shared_ptr<SunExclusion> BuildSunExclusion(const std::shared_ptr<const SunCandidates>& a_candidates, const ShadowPayload& a_payload, std::uint32_t a_mode,
			const SceneStore::Tables& a_tables, SunExclusionCache* a_cache = nullptr)
		{
			ZoneScopedN("CS.DCLF.BuildSunExclusion");
			ScopedScan scan(Scan::SunExclusion);
			if (!a_candidates || a_candidates->entries.empty())
				return nullptr;
			auto exclusion = std::make_shared<SunExclusion>();
			exclusion->candidates = a_candidates;
			const std::size_t count = a_candidates->entries.size();
			const std::uint64_t membership = a_payload.kept ? a_payload.membership[a_mode] : 0;
			bool wouldReuse = false;
			if (a_cache) {
				auto& c = *a_cache;
				++c.builds;
				bool reuse = c.valid && membership && c.candidates == a_candidates && c.membership == membership &&
				             c.cursor.Continues(a_tables.changeLog, a_payload.inputs.tablesGeneration);
				if (reuse)
					for (const auto& change : c.cursor.Unread(a_tables.changeLog))
						if (change.causes & (kChangeBindings | kChangeGeometry)) {
							reuse = false;
							break;
						}
				c.cursor.Advance(a_tables.changeLog);
				// CS_DCLF_PERSISTENT_PARITY: a reuse is built in full every 60 frames and compared.
				wouldReuse = reuse;
				if (reuse && PersistentParityEnabled() && ParityDue(a_payload.inputs.frameNumber))
					reuse = false;
				if (reuse) {
					++c.reused;
					exclusion->excluded = c.excluded;
					exclusion->excludedCount = c.excludedCount;
					exclusion->removed = std::make_unique<std::atomic<std::uint32_t>[]>(count);
					exclusion->cleared = std::make_unique<std::atomic<std::uint32_t>[]>(a_candidates->geometryEntry.size());
					return exclusion;
				}
			}
			exclusion->excluded.assign(count, 1);
			std::vector<std::uint8_t> isInput(a_tables.objects.size(), 0);
			a_payload.ForEachInput(a_mode, [&](const DrawInput& a_input) {
				if (a_input.objectIndex < isInput.size())
					isInput[a_input.objectIndex] = 1;
			});
			for (std::size_t o = 0; o < a_tables.objects.size(); ++o) {
				if ((a_tables.objects[o].flags & (kObjectFree | kObjectNoShadow)) || isInput[o])
					continue;
				if (const auto it = a_candidates->geometries.find(a_tables.objectGeometry[o]); it != a_candidates->geometries.end())
					exclusion->excluded[a_candidates->geometryEntry[it->second]] = 0;
			}
			exclusion->excludedCount = static_cast<std::uint32_t>(std::count(exclusion->excluded.begin(), exclusion->excluded.end(), std::uint8_t(1)));
			if (a_cache) {
				auto& c = *a_cache;
				if (wouldReuse) {
					c.parity.Check(c.excluded == exclusion->excluded);
				}
				c.candidates = a_candidates;
				c.cursor.Restart(a_payload.inputs.tablesGeneration);
				c.membership = membership;
				c.excluded = exclusion->excluded;
				c.excludedCount = exclusion->excludedCount;
				c.valid = true;
			}
			exclusion->removed = std::make_unique<std::atomic<std::uint32_t>[]>(count);
			exclusion->cleared = std::make_unique<std::atomic<std::uint32_t>[]>(a_candidates->geometryEntry.size());
			return exclusion;
		}

		// Whether an object's entry is outside every one of the sun's full-frustum processes, so the sun's cascade
		// culls never reach it (ShadowInputs::sunEntryPlanes).
		bool OutsideSunEntry(const ShadowInputs& a_in, const SceneStore::Tables& a_tables, std::size_t a_object)
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

		bool PersistentShadowEnabled()
		{
			static const bool enabled = SwitchValue("CS_DCLF_PERSISTENT_SHADOW") != "0";
			return enabled;
		}

		/**
		 * @brief The shadow epoch's inputs and binding records, kept across frames (drawcall-limit-fix.md, "Persistent draw
		 * state", Step 6). Per render mode, a region of the casters' inputs, changed only by the change log's entries for them,
		 * by a mode's views changing their rasterizer states, and by what was waiting (a pipeline or a diffuse texture not yet
		 * resolved) becoming ready; the per-frame list is only the face shapes. The records: the plain one and one per
		 * alpha-tested material, at slots the materials keep; their texcoord blocks sit at fixed offsets in the arena, which is
		 * written whole every build (it is a few KB), so a record changes only when its texture does. The records and each
		 * mode's inputs are KeptArrays: a buffer (a mode's, or a view slot's copy of the records) is sent what changed since the
		 * version it holds.
		 */
		struct ShadowKept
		{
			static constexpr std::uint32_t kWaiting = ~0u - 1;  // an object's record while its material's texture is not resolved
			static constexpr std::uint32_t kNoRecord = ~0u;
			LogCursor cursor;
			const void* identity = nullptr;
			std::uint64_t build = 0;  // counts the builds: what ShadowKept::Mode::membership stamps
			KeptArray<DrawBindings> records;
			ankerl::unordered_dense::map<const RE::BSShaderMaterial*, std::uint32_t> slotOf;
			std::vector<const RE::BSShaderMaterial*> slotMaterial;
			std::vector<ID3D11ShaderResourceView*> slotDiffuse;
			std::vector<std::uint32_t> slotRefs;
			std::vector<std::uint8_t> slotReady;
			std::vector<std::uint32_t> freeSlots;
			std::vector<std::uint32_t> objectRecord;  // per object: its record slot, 0 the plain one, kNoRecord, kWaiting
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
				auto keptRecords = std::move(records);
				std::array<Mode, kShadowModeCount> keptModes;
				for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
					keptModes[m].inputs = std::move(modes[m].inputs);
				const auto counters = std::tuple{ build, builds, entriesWritten, recordsWritten, resyncs };
				auto keptParity = std::move(parity);
				*this = ShadowKept{};
				records = std::move(keptRecords);
				records.Clear();
				for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
					modes[m].inputs = std::move(keptModes[m].inputs);
					modes[m].inputs.Clear();
				}
				std::tie(build, builds, entriesWritten, recordsWritten, resyncs) = counters;
				parity = std::move(keptParity);
			}
			// Since the last report.
			std::uint64_t builds = 0, entriesWritten = 0, recordsWritten = 0, resyncs = 0;
			ParityCounter parity;
		};

		/** @brief The kept path of BuildShadowPayload: the records and the inputs from the kept state (ShadowKept). */
		template <class Block>
		void BuildKeptShadow(const ShadowInputs& a_in, const SceneStore::Tables& a_tables, const Lookups& a_lookups, ShadowPayload& a_out, ShadowKept& k,
			const DrawBindings& a_plain, Block&& a_block)
		{
			ZoneScopedN("CS.DCLF.BuildShadow.Kept");
			const std::uint32_t objects = static_cast<std::uint32_t>(std::min<std::size_t>(a_tables.objects.size(), kMaxObjects));
			++k.builds;
			// Membership changes of this build carry this stamp.
			const std::uint64_t build = ++k.build;
			// What the buffers hold is the version they were sent: what changes from here on is sent alone.
			k.records.BeginBuild(a_in.recordsOldestHeld);
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
				k.modes[m].inputs.BeginBuild(a_in.inputsHeld[m]);
			bool resync = !k.cursor.Continues(a_tables.changeLog, a_in.tablesGeneration) || k.identity != a_in.addresses.identity || k.objectRecord.size() > objects;
			if (resync) {
				k.Reset();
				k.cursor.Restart(a_in.tablesGeneration);
				k.identity = a_in.addresses.identity;
				++k.resyncs;
			}
			if (k.objectRecord.size() < objects) {
				k.objectRecord.resize(objects, ShadowKept::kNoRecord);
				k.objectMaterial.resize(objects, nullptr);
			}
			auto writeRecord = [&](std::uint32_t a_slot, const DrawBindings& a_record) {
				if (a_slot >= k.records.Size()) {
					auto& records = k.records.Mutable();
					records.resize(std::size_t(a_slot) + 1);
					records[a_slot] = a_record;
					k.records.Mark(a_slot);
					++k.recordsWritten;
				} else if (k.records.Set(a_slot, a_record)) {
					++k.recordsWritten;
				}
			};
			// ---- The records: the plain one, and each material's.
			if (k.slotMaterial.empty()) {
				k.slotMaterial.push_back(nullptr);  // slot 0: the plain record
				k.slotDiffuse.push_back(nullptr);
				k.slotRefs.push_back(0);
				k.slotReady.push_back(1);
			}
			writeRecord(0, a_plain);
			// The texcoord blocks at fixed offsets: one 256-byte block per slot after the arena's shared blocks (allocated once the
			// build's slots are known; the base is the same every build).
			std::uint64_t texcoordBase = 0;
			const std::uint32_t transformBuffer = globals::game::smState ? (globals::game::smState->textureTransformCurrentBuffer & 1) : 0u;
			auto& arena = a_out.arena;
			const std::uint64_t base = a_in.addresses.constants;
			auto materialRecord = [&](std::uint32_t a_slot) {
				// The material's diffuse and texture offset (read off the material now: shader-property controllers move it
				// between the walk and this build), in the slot's fixed block.
				const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(k.slotMaterial[a_slot]);
				const auto textureIt = k.slotDiffuse[a_slot] ? a_lookups.shadowTextures.find(k.slotDiffuse[a_slot]) : a_lookups.shadowTextures.end();
				const bool ready = textureIt != a_lookups.shadowTextures.end() && textureIt->second != Lookups::kNone && texcoordBase &&
				                   a_slot < kShadowRecordCapacity;
				k.slotReady[a_slot] = ready ? 1 : 0;
				if (!ready)
					return;
				const std::uint64_t block = texcoordBase + std::uint64_t(a_slot) * 256;
				const std::array<float, 4> texcoord{ material->texCoordOffset[transformBuffer].x, material->texCoordOffset[transformBuffer].y,
					material->texCoordScale[transformBuffer].x, material->texCoordScale[transformBuffer].y };
				std::memcpy(arena.At(block - base, sizeof(texcoord)).data(), texcoord.data(), sizeof(texcoord));
				DrawBindings bindings = a_plain;
				bindings.vertexConstants[1] = block;
				bindings.textures[0] = textureIt->second;
				writeRecord(a_slot, bindings);
			};
			// An object's record: the plain one, its material's (acquired), or none.
			auto recordOf = [&](std::uint32_t o) -> std::uint32_t {
				const auto& object = a_tables.objects[o];
				if (object.flags & kObjectFree)
					return ShadowKept::kNoRecord;
				const std::uint32_t sky = o < a_tables.skyTechnique.size() && a_in.modeUsed[kSkyMode] ? a_tables.skyTechnique[o] : 0u;
				if ((object.flags & kObjectNoShadow) && !sky)
					return ShadowKept::kNoRecord;
				if (!(((object.flags & kObjectNoShadow) ? 0u : a_tables.shadowTechnique[o]) & 0x80) && !(sky & 0x80))
					return 0;
				const auto* material = o < a_tables.shadowMaterial.size() ? a_tables.shadowMaterial[o] : nullptr;
				if (!material)
					return ShadowKept::kNoRecord;
				auto [it, fresh] = k.slotOf.try_emplace(material, 0u);
				if (fresh) {
					std::uint32_t slot;
					if (!k.freeSlots.empty()) {
						slot = k.freeSlots.back();
						k.freeSlots.pop_back();
					} else {
						slot = static_cast<std::uint32_t>(k.slotMaterial.size());
						k.slotMaterial.push_back(nullptr);
						k.slotDiffuse.push_back(nullptr);
						k.slotRefs.push_back(0);
						k.slotReady.push_back(0);
					}
					it->second = slot;
					k.slotMaterial[slot] = material;
					k.slotDiffuse[slot] = a_tables.shadowDiffuse[o];
					k.slotReady[slot] = 0;
				}
				return it->second;
			};
			auto releaseRecord = [&](std::uint32_t o) {
				const std::uint32_t slot = k.objectRecord[o];
				if (slot != ShadowKept::kNoRecord && slot != 0 && slot < k.slotRefs.size() && k.objectMaterial[o] && --k.slotRefs[slot] == 0) {
					k.slotOf.erase(k.slotMaterial[slot]);
					k.slotMaterial[slot] = nullptr;
					k.slotDiffuse[slot] = nullptr;
					k.slotReady[slot] = 0;
					k.freeSlots.push_back(slot);
				}
				k.objectRecord[o] = ShadowKept::kNoRecord;
				k.objectMaterial[o] = nullptr;
			};
			auto takeRecord = [&](std::uint32_t o) {
				releaseRecord(o);
				const std::uint32_t slot = recordOf(o);
				k.objectRecord[o] = slot;
				if (slot != ShadowKept::kNoRecord && slot != 0) {
					++k.slotRefs[slot];
					k.objectMaterial[o] = k.slotMaterial[slot];
				}
			};

			// ---- The modes: a mode whose views' states changed is read again whole.
			const bool entryOnGpu = true;  // the kept path runs only when the latch holds the sun's processes
			(void)entryOnGpu;
			auto evaluate = [&](std::uint32_t m, std::uint32_t o, DrawInput& a_input) -> int {
				// 0: no input, 1: an input (a_input), 2: waiting (a pipeline or a texture), 3: the frame's list (a face).
				const auto& object = a_tables.objects[o];
				const bool skyMode = m == kSkyMode;
				if (object.flags & kObjectFree)
					return 0;
				if (skyMode ? (o >= a_tables.skyTechnique.size() || !a_tables.skyTechnique[o]) : (object.flags & kObjectNoShadow) != 0)
					return 0;
				const std::uint32_t record = k.objectRecord[o];
				if (record == ShadowKept::kNoRecord)
					return 0;
				if (record != 0 && !(record < k.slotReady.size() && k.slotReady[record]))
					return 2;
				const bool volumetricOnly = !skyMode && (object.flags & kObjectVolumetricOnly) != 0;
				const std::uint32_t classStates = volumetricOnly ? (a_in.modeRasterStates[m] >> 16) : (a_in.modeRasterStates[m] & 0xFFFFu);
				if (classStates == 0)
					return 0;
				const std::uint32_t technique = skyMode ? a_tables.skyTechnique[o] : (a_tables.shadowTechnique[o] | ModeBitsOf(m));
				const ShadowPipelineKey key{ technique, (object.flags & kObjectTwoSided) ? kRasterTwoSided : 0u,
					VertexLayoutOf(a_tables.geometries[object.geometryIndex].vertexDesc) };
				const auto slotIt = a_lookups.shadowSlots.find(key);
				if (slotIt == a_lookups.shadowSlots.end())
					return 2;
				for (std::uint32_t states = classStates; states; states &= states - 1) {
					const auto& row = a_lookups.shadowMapRows[std::countr_zero(states)];
					if ((slotIt->second < row.size() ? row[slotIt->second] : Lookups::kNone) == Lookups::kNone)
						return 2;
				}
				if (IsFaceObject(a_tables, o) || (key.vertexLayout & kPositionInSecondStream))
					return 3;
				a_input = { slotIt->second, record, object.geometryIndex, (object.flags & ~kObjectDecal) | kInputDrawable,
					{ object.boundCenter[0], object.boundCenter[1], object.boundCenter[2] }, object.boundRadius, o, 0, PartitionsOf(a_tables, o), ~0u };
				SetSunEntryRow(a_input, a_tables, o);
				return 1;
			};
			auto removeEntry = [&](ShadowKept::Mode& a_mode, std::uint32_t o) {
				if (!a_mode.Holds(o))
					return;
				if (o < a_mode.claimedOf.size() && a_mode.claimedOf[o]) {
					if (const auto held = a_mode.claims.find(a_mode.claimedOf[o]); held != a_mode.claims.end() && held->second == o)
						a_mode.claims.erase(held);
					a_mode.claimedOf[o] = nullptr;
					a_mode.claimsChanged = true;
				}
				a_mode.Remove(o);
				a_mode.membership = build;
				++k.entriesWritten;
			};
			auto setMark = [](std::vector<std::uint32_t>& a_list, std::vector<std::uint8_t>& a_mark, std::uint32_t o, bool a_on) {
				if (a_mark.size() <= o)
					a_mark.resize(std::size_t(o) + 1, 0);
				if (a_on && !a_mark[o]) {
					a_mark[o] = 1;
					a_list.push_back(o);
				} else if (!a_on && a_mark[o]) {
					a_mark[o] = 0;  // dropped from the list at its next pass
				}
			};
			// Claims hold a geometry: the one a slot draws now (a slot reused by another object removed its entry first).
			auto take = [&](std::uint32_t m, std::uint32_t o) {
				auto& mode = k.modes[m];
				mode.Cover(objects);
				if (mode.claimedOf.size() < objects)
					mode.claimedOf.resize(objects, nullptr);
				DrawInput input{};
				const int result = o < a_tables.objects.size() ? evaluate(m, o, input) : 0;
				setMark(mode.waiting, mode.waitingMark, o, result == 2);
				const bool wasFace = o < mode.faceMark.size() && mode.faceMark[o];
				setMark(mode.faces, mode.faceMark, o, result == 3);
				if (wasFace != (result == 3))
					mode.membership = build;
				if (result != 1) {
					removeEntry(mode, o);
					return;
				}
				std::uint32_t i = mode.indexOf[o];
				if (i == kNoRegion) {
					if (mode.inputs.Size() >= kMaxInputs - kLoopReserve)
						return;
					mode.Add(o, input);
					mode.membership = build;
					++k.entriesWritten;
				} else if (mode.inputs.Set(i, input)) {
					++k.entriesWritten;
				}
				// Its claim: the geometry the slot draws now.
				const auto* geometry = o < a_tables.objectGeometry.size() ? a_tables.objectGeometry[o] : nullptr;
				if (mode.claimedOf[o] != geometry) {
					if (mode.claimedOf[o])
						if (const auto held = mode.claims.find(mode.claimedOf[o]); held != mode.claims.end() && held->second == o)
							mode.claims.erase(held);
					if (geometry)
						mode.claims[geometry] = o;
					mode.claimedOf[o] = geometry;
					mode.claimsChanged = true;
				}
				return;
			};

			// The objects the log names (or every object on a resync, or when a mode's views changed their states - a record
			// depends on whether Skylighting's map is drawn): their record, then their entry in each mode.
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
				resync |= k.modes[m].active && k.modes[m].rasterStates != (a_in.modeUsed[m] ? a_in.modeRasterStates[m] : 0u);
			std::vector<std::uint32_t> changed;
			if (resync) {
				changed.reserve(objects);
				for (std::uint32_t o = 0; o < objects; ++o)
					changed.push_back(o);
			} else {
				constexpr std::uint32_t kShadowCauses = kChangeShadow | kChangeBindings | kChangePlacement | kChangeGeometry | kChangeSkin | kChangeMembership;
				for (const auto& change : k.cursor.Unread(a_tables.changeLog))
					if ((change.causes & kShadowCauses) && change.slot < objects)
						changed.push_back(change.slot);
			}
			k.cursor.Advance(a_tables.changeLog);
			for (const std::uint32_t o : changed)
				takeRecord(o);
			texcoordBase = a_block(nullptr, std::size_t(256) * std::max<std::size_t>(k.slotMaterial.size(), 1));
			// The records of every material slot held (its texture may have been resolved since, its texcoord moves).
			for (std::uint32_t slot = 1; slot < k.slotMaterial.size(); ++slot)
				if (k.slotMaterial[slot])
					materialRecord(slot);
			a_out.records = k.records.Get();
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
				auto& mode = k.modes[m];
				const std::uint32_t states = a_in.modeUsed[m] ? a_in.modeRasterStates[m] : 0u;
				if (!mode.active || mode.rasterStates != states) {
					// A mode's views changed their states: every object again for it.
					mode.Reset();
					mode.active = true;
					mode.rasterStates = states;
					if (states)
						for (std::uint32_t o = 0; o < objects; ++o)
							take(m, o);
				} else if (states) {
					for (const std::uint32_t o : changed)
						take(m, o);
					// What waited: again, while it still waits.
					std::vector<std::uint32_t> waiting;
					waiting.swap(mode.waiting);
					for (const std::uint32_t o : waiting) {
						if (o >= mode.waitingMark.size() || !mode.waitingMark[o])
							continue;
						mode.waitingMark[o] = 0;
						take(m, o);
					}
				}
				if (!a_in.modeUsed[m])
					continue;
				// The frame's list: the face shapes, with their positions of this walk.
				auto& list = a_out.inputList[m];
				std::vector<const RE::BSGeometry*> faceGeometries;
				std::size_t keptFaces = 0;
				for (const std::uint32_t o : mode.faces) {
					if (o >= mode.faceMark.size() || !mode.faceMark[o])
						continue;
					mode.faces[keptFaces++] = o;
					const auto& object = a_tables.objects[o];
					const std::uint32_t streamIndex = FaceStreamGeometry(a_tables, o, a_in.addresses.facePositions);
					if (streamIndex == ~0u)
						continue;  // no positions this walk: no input, the engine's
					const bool skyMode = m == kSkyMode;
					const std::uint32_t technique = skyMode ? a_tables.skyTechnique[o] : (a_tables.shadowTechnique[o] | ModeBitsOf(m));
					const ShadowPipelineKey key{ technique, (object.flags & kObjectTwoSided) ? kRasterTwoSided : 0u,
						VertexLayoutOf(a_tables.geometries[object.geometryIndex].vertexDesc) };
					const auto slotIt = a_lookups.shadowSlots.find(key);
					if (slotIt == a_lookups.shadowSlots.end())
						continue;
					list.push_back({ slotIt->second, k.objectRecord[o], object.geometryIndex, (object.flags & ~kObjectDecal) | kInputDrawable,
						{ object.boundCenter[0], object.boundCenter[1], object.boundCenter[2] }, object.boundRadius, o, 0, PartitionsOf(a_tables, o), streamIndex });
					SetSunEntryRow(list.back(), a_tables, o);
					if (o < a_tables.objectGeometry.size() && a_tables.objectGeometry[o])
						faceGeometries.push_back(a_tables.objectGeometry[o]);
				}
				mode.faces.resize(keptFaces);
				// The claims: the region's, and the frame's list's; a new set only when either changed.
				std::sort(faceGeometries.begin(), faceGeometries.end());
				if (mode.claimsChanged || faceGeometries != mode.lastFaces || !mode.published) {
					auto claims = std::make_shared<PassCapture::ClaimSet>();
					claims->reserve(mode.claims.size() + faceGeometries.size());
					for (const auto& [geometry, owner] : mode.claims)
						claims->insert(geometry);
					claims->insert(faceGeometries.begin(), faceGeometries.end());
					mode.published = std::move(claims);
					mode.lastFaces = std::move(faceGeometries);
					mode.claimsChanged = false;
				}
				if (m != kSkyMode)
					a_out.claims[m] = mode.published;
				a_out.regionInputs[m] = mode.inputs.View();
				a_out.membership[m] = mode.membership;
				// The skip counters, as the loop kept them: what waits.
				a_out.deferredPipelines += static_cast<std::uint32_t>(mode.waiting.size());
				a_out.skippedPipeline += static_cast<std::uint32_t>(mode.waiting.size());
			}
			a_out.kept = true;
			a_out.recordChanges = k.records.View().changes;
		}

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
		 * @brief A view slot's copy of the records (the per-view registers b0 and b12 named): the ones newer than what the slot
		 * holds, or all of them without the kept state. Returns whether it wrote any.
		 */
		template <class Emit>
		bool EmitShadowRecords(const ShadowPayload& a_payload, std::uint64_t a_held, std::uint64_t a_viewBlock, std::uint64_t a_perFrame, Emit&& a_emit)
		{
			const auto& records = a_payload.records;
			auto copy = [&](std::size_t a_first, std::size_t a_end) {
				std::vector<DrawBindings> block(records.begin() + a_first, records.begin() + a_end);
				for (auto& record : block) {
					record.vertexConstants[0] = a_viewBlock;
					record.pixelConstants[0] = a_viewBlock;
					record.vertexConstants[kPerFrameVertexRegister] = a_perFrame;
					record.pixelConstants[kPerFrameVertexRegister] = a_perFrame;
				}
				a_emit(block.data(), block.size() * sizeof(DrawBindings), a_first * sizeof(DrawBindings));
			};
			if (!a_payload.kept) {
				if (!records.empty())
					copy(0, records.size());
				return !records.empty();
			}
			return a_payload.recordChanges.ForEachRun(a_held, records.size(), [&](std::uint64_t a_first, std::uint64_t a_count) {
				copy(static_cast<std::size_t>(a_first), static_cast<std::size_t>(a_first + a_count));
			}) != 0;
		}

		void BuildShadowPayload(const ShadowInputs& a_in, const SceneStore::Tables& a_tables, const Lookups& a_lookups, ShadowPayload& a_out,
			ObjectRecordStore* a_objects = nullptr, BonesStore* a_bones = nullptr, ShadowKept* a_kept = nullptr, GeometryStore* a_geometries = nullptr);

		/**
		 * @brief CS_DCLF_PERSISTENT_PARITY: a kept shadow build against the same build made the per-frame way. Per used mode, the
		 * same casters with the same inputs (the record's number apart), records binding the same textures and samplers with the
		 * same texcoord values, and the same claims.
		 */
		void CheckKeptShadow(const ShadowInputs& a_in, const SceneStore::Tables& a_tables, const Lookups& a_lookups, const ShadowPayload& a_kept, ShadowKept& k)
		{
			ShadowPayload reference;
			BuildShadowPayload(a_in, a_tables, a_lookups, reference);
			const std::uint64_t base = a_in.addresses.constants;
			auto fail = [&](std::uint32_t a_object, const std::string& a_what) {
				if (k.parity.mismatches++ == 0)
					k.parity.first = fmt::format("object {}: {}", a_object, a_what);
			};
			auto texcoordOf = [&](const ShadowPayload& a_payload, const DrawBindings& a_record) {
				std::array<float, 4> values{};
				const auto& bytes = a_payload.arena.Bytes();
				const std::uint64_t address = a_record.vertexConstants[1];
				if (address >= base && address - base + sizeof(values) <= bytes.size())
					std::memcpy(values.data(), bytes.data() + (address - base), sizeof(values));
				return values;
			};
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
				if (!a_in.modeUsed[m])
					continue;
				ankerl::unordered_dense::map<std::uint32_t, DrawInput> keptInputs;
				a_kept.ForEachInput(m, [&](const DrawInput& a_input) { keptInputs.emplace(a_input.objectIndex, a_input); });
				std::size_t matched = 0;
				for (const auto& input : reference.inputList[m]) {
					++k.parity.checks;
					const auto it = keptInputs.find(input.objectIndex);
					if (it == keptInputs.end()) {
						fail(input.objectIndex, fmt::format("mode {}: an input of the per-frame build only", m));
						continue;
					}
					++matched;
					DrawInput x = it->second, y = input;
					const std::uint32_t xr = x.recordIndex, yr = y.recordIndex;
					x.recordIndex = y.recordIndex = 0;
					if (std::memcmp(&x, &y, sizeof(DrawInput)) != 0) {
						fail(input.objectIndex, fmt::format("mode {}: the input differs", m));
						continue;
					}
					if (xr >= a_kept.records.size() || yr >= reference.records.size()) {
						fail(input.objectIndex, "a record index out of range");
						continue;
					}
					const auto& a = a_kept.records[xr];
					const auto& b = reference.records[yr];
					if (std::memcmp(a.textures, b.textures, sizeof(a.textures)) != 0 || std::memcmp(a.samplers, b.samplers, sizeof(a.samplers)) != 0 ||
						texcoordOf(a_kept, a) != texcoordOf(reference, b))
						fail(input.objectIndex, fmt::format("mode {}: its record differs (kept {} against {})", m, xr, yr));
				}
				if (matched != keptInputs.size())
					fail(~0u, fmt::format("mode {}: {} inputs of the kept build only", m, keptInputs.size() - matched));
				if (m != kSkyMode && a_kept.claims[m]) {
					const auto claims = ShadowClaimSet(reference.inputList[m], a_tables);
					if (*claims != *a_kept.claims[m])
						fail(~0u, fmt::format("mode {}: the claims differ ({} against {})", m, a_kept.claims[m]->size(), claims->size()));
				}
			}
		}

		void BuildShadowPayload(const ShadowInputs& a_in, const SceneStore::Tables& a_tables, const Lookups& a_lookups, ShadowPayload& a_out,
			ObjectRecordStore* a_objects, BonesStore* a_bones, ShadowKept* a_kept, GeometryStore* a_geometries)
		{
			ZoneScopedN("CS.DCLF.BuildShadowPayload");
			a_out.Reset();
			a_out.inputs = a_in;
			const std::uint64_t base = a_in.addresses.constants;
			auto& arena = a_out.arena;
			auto& records = a_out.records;
			auto block = [&](const void* a_data, std::size_t a_size) -> std::uint64_t {
				const auto offset = arena.Allocate(a_size);
				if (offset == ~0ull)
					return 0;
				if (a_data)
					std::memcpy(arena.At(offset, a_size).data(), a_data, a_size);
				return base + offset;
			};

			// ---- What every view shares: the object records, bone rows, geometry table, binding records.
			UpdateObjectRecords(a_objects, a_in.tablesHeld.objects, a_tables, a_in.tablesGeneration, a_in.renderFlags, a_in.frameNumber, a_out.objects);
			UpdateBones(a_bones, a_in.tablesHeld.bones, a_tables, a_in.tablesGeneration, a_out.bones);
			UpdateGeometryDraws(a_geometries, a_in.tablesHeld.geometries, a_tables, a_in.tablesGeneration, a_in.frameNumber, a_out.geometries);
			AppendFaceStreams(a_tables, a_in.addresses.facePositions, a_out.geometries);
			if (a_in.addresses.facePositions)
				a_out.faceStreams = a_tables.faceStreams;
			// The binding records, built once with the per-view registers (b0, b12) unset; each view slot
			// uploads its own copy of them naming its blocks at the head of the arena.
			(void)arena.Allocate(kShadowMaterialBlocksOffset);
			DrawBindings plain{};
			// No register the Utility shaders declare may be left at address zero: a pipeline that reads
			// one arrives from the background compiler seconds after the first epoch, and a null read is
			// a device loss. Everything not supplied below reads zeros.
			static constexpr std::size_t kZeroBlockBytes = 1024;
			const std::uint64_t zeros = block(nullptr, kZeroBlockBytes);
			for (auto& address : plain.vertexConstants)
				address = zeros;
			for (auto& address : plain.pixelConstants)
				address = zeros;
			// Community Shaders' SharedData (b5) and FeatureData (b6): the alpha-tested pixel stage samples
			// its diffuse with SharedData::MipBias. Packed from the copies the inputs carry, as the main epochs do.
			if (!a_in.sharedData.empty()) {
				plain.pixelConstants[kSharedDataRegister] = block(a_in.sharedData.data(), a_in.sharedData.size());
				plain.vertexConstants[kSharedDataRegister] = plain.pixelConstants[kSharedDataRegister];
			}
			if (!a_in.featureData.empty()) {
				plain.pixelConstants[kFeatureDataRegister] = block(a_in.featureData.data(), a_in.featureData.size());
				plain.vertexConstants[kFeatureDataRegister] = plain.pixelConstants[kFeatureDataRegister];
			}
			const std::uint32_t nullIndex = a_lookups.nullTexture == Lookups::kNone ? 0u : a_lookups.nullTexture;
			for (auto& index : plain.textures)
				index = nullIndex;
			plain.textures[kObjectBufferRegister] = a_in.addresses.objectsIndex;
			plain.textures[kBonesBufferRegister] = a_in.addresses.bonesIndex;
			const std::uint32_t wrapAnisotropic = a_lookups.Sampler(static_cast<std::uint32_t>(RE::BSGraphics::TextureAddressMode::kWrapSWrapT),
				static_cast<std::uint32_t>(RE::BSGraphics::TextureFilterMode::kAnisotropic));
			for (auto& index : plain.samplers)
				index = wrapAnisotropic == Lookups::kNone ? 0u : wrapAnisotropic;
			if (a_kept && a_in.sunEntryPlaneMasks.size() <= kMaxSunEntryProcesses) {
				BuildKeptShadow(a_in, a_tables, a_lookups, a_out, *a_kept, plain, block);
				if (PersistentParityEnabled() && a_in.frameNumber % 60 == 0)
					CheckKeptShadow(a_in, a_tables, a_lookups, a_out, *a_kept);
				return;
			}
			records.push_back(plain);  // record 0: every caster without alpha testing
			auto& objectRecord = a_out.objectRecord;
			objectRecord.assign(a_tables.objects.size(), ~0u);
			ankerl::unordered_dense::map<const RE::BSShaderMaterial*, std::uint32_t> recordByMaterial;
			const bool haveShadowMaterials = a_tables.shadowMaterial.size() == a_tables.objects.size();
			for (std::size_t o = 0; o < a_tables.objects.size() && o < kMaxObjects; ++o) {
				const auto& object = a_tables.objects[o];
				// A record for every caster and every occluder of Skylighting's map; the alpha-tested ones name their diffuse.
				const std::uint32_t sky = o < a_tables.skyTechnique.size() && a_in.modeUsed[kSkyMode] ? a_tables.skyTechnique[o] : 0u;
				if ((object.flags & kObjectNoShadow) && !sky)
					continue;
				if (!(((object.flags & kObjectNoShadow) ? 0u : a_tables.shadowTechnique[o]) & 0x80) && !(sky & 0x80)) {
					objectRecord[o] = 0;
					continue;
				}
				// Alpha-tested: the material's diffuse and texture offset, one record per material.
				const auto* material = haveShadowMaterials ? a_tables.shadowMaterial[o] : nullptr;
				if (!material) {
					++a_out.skippedTexture;
					continue;
				}
				if (auto it = recordByMaterial.find(material); it != recordByMaterial.end()) {
					objectRecord[o] = it->second;
					continue;
				}
				auto* srv = a_tables.shadowDiffuse[o];
				const auto textureIt = srv ? a_lookups.shadowTextures.find(srv) : a_lookups.shadowTextures.end();
				if (textureIt == a_lookups.shadowTextures.end()) {
					if (srv)
						++a_out.deferredTextures;
					++a_out.skippedTexture;
					continue;
				}
				if (textureIt->second == Lookups::kNone || records.size() >= kShadowRecordCapacity) {
					++a_out.skippedTexture;
					continue;
				}
				DrawBindings bindings = plain;
				// The texture transform off the material now, not from the walk: shader-property controllers
				// (BSLightingShaderPropertyFloatController::Update) move it between Main::Draw, where the walk
				// starts, and BeforeShadowMaps, where this build is kicked. Of the two buffers, the one the frame
				// reads (BSShaderManager::State::textureTransformCurrentBuffer, flipped by Main::Update), as
				// BSUtilityShader::SetupMaterial does; the controllers write the other one.
				const std::uint32_t transformBuffer = globals::game::smState ? (globals::game::smState->textureTransformCurrentBuffer & 1) : 0u;
				const std::array<float, 4> texcoord{ material->texCoordOffset[transformBuffer].x, material->texCoordOffset[transformBuffer].y,
					material->texCoordScale[transformBuffer].x, material->texCoordScale[transformBuffer].y };
				bindings.vertexConstants[1] = block(texcoord.data(), sizeof(texcoord));
				bindings.textures[0] = textureIt->second;
				const auto index = static_cast<std::uint32_t>(records.size());
				records.push_back(bindings);
				recordByMaterial.emplace(material, index);
				objectRecord[o] = index;
			}

			// ---- The inputs per render mode among the captured views: every caster with a ready pipeline
			// for its technique under that mode. The cascades share one set; a spot light has its own.
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
				auto& inputs = a_out.inputList[m];
				if (!a_in.modeUsed[m])
					continue;
				const bool skyMode = m == kSkyMode;
				const std::uint32_t modeBits = ModeBitsOf(m);
				for (std::size_t o = 0; o < a_tables.objects.size() && o < kMaxObjects && inputs.size() < kMaxInputs; ++o) {
					const auto& object = a_tables.objects[o];
					if (skyMode ? (o >= a_tables.skyTechnique.size() || !a_tables.skyTechnique[o]) : (object.flags & kObjectNoShadow) != 0)
						continue;
					if (objectRecord[o] == ~0u) {
						a_out.skySkipped += skyMode ? 1 : 0;
						continue;
					}
					// The states of the views that draw this caster's class. A volumetric-only caster with no view
					// of the copy under this mode is no input at all: it is then the engine's, unclaimed.
					const bool volumetricOnly = !skyMode && (object.flags & kObjectVolumetricOnly) != 0;
					const std::uint32_t classStates = volumetricOnly ? (a_in.modeRasterStates[m] >> 16) : (a_in.modeRasterStates[m] & 0xFFFFu);
					if (volumetricOnly && classStates == 0)
						continue;
					const std::uint32_t technique = skyMode ? a_tables.skyTechnique[o] : (a_tables.shadowTechnique[o] | modeBits);
					const ShadowPipelineKey key{ technique, (object.flags & kObjectTwoSided) ? kRasterTwoSided : 0u,
						VertexLayoutOf(a_tables.geometries[object.geometryIndex].vertexDesc) };
					const auto slotIt = a_lookups.shadowSlots.find(key);
					if (slotIt == a_lookups.shadowSlots.end()) {
						++a_out.deferredPipelines;
						++a_out.skippedPipeline;
						a_out.skySkipped += skyMode ? 1 : 0;
						continue;
					}
					// Every view of the mode has to be able to draw it: the claim withholds the engine's pass
					// from all of them, so a view without the pipeline would leave the caster to nobody.
					bool deferred = false, missing = false;
					for (std::uint32_t states = classStates; states; states &= states - 1) {
						const auto& row = a_lookups.shadowMapRows[std::countr_zero(states)];
						const std::uint32_t pipeline = slotIt->second < row.size() ? row[slotIt->second] : Lookups::kNone;
						if (pipeline == Lookups::kNone) {
							// Not resolved yet, or resolved to nothing: which of the two is in shadowPipelines.
							const auto pipelineIt = a_lookups.shadowPipelines.find({ technique, WithShadowState(key.rasterFlags, std::countr_zero(states)), key.vertexLayout });
							(pipelineIt == a_lookups.shadowPipelines.end() ? deferred : missing) = true;
						}
					}
					if (deferred || missing || classStates == 0) {
						a_out.deferredPipelines += deferred ? 1 : 0;
						++a_out.skippedPipeline;
						a_out.skySkipped += skyMode ? 1 : 0;
						continue;
					}
					// A face shape draws only with its positions: without them (no buffer, the geometry table full) it
					// is no input, and stays the engine's. And a caster whose layout reads its position from the second
					// stream (a dynamic shape's) is never an input without one: the slot would fall back to the
					// geometry's own buffer and draw its other attributes as positions.
					const std::uint32_t streamIndex = FaceStreamGeometry(a_tables, o, a_in.addresses.facePositions);
					if (streamIndex == ~0u && (IsFaceObject(a_tables, o) || (key.vertexLayout & kPositionInSecondStream))) {
						a_out.skySkipped += skyMode ? 1 : 0;
						continue;
					}
					// The sun's entry rule (kCullSunEntry): BuildDraws tests the entry's sphere, carried in the fade row, against the
					// frame's full-frustum processes in the view's latch - so the input does not change with the frame's planes.
					// With more processes than the latch holds, the verdict is taken here, as before.
					const bool entryOnGpu = a_in.sunEntryPlaneMasks.size() <= kMaxSunEntryProcesses;
					inputs.push_back({ slotIt->second, objectRecord[o], object.geometryIndex,
						(object.flags & ~kObjectDecal) | kInputDrawable | (!entryOnGpu && OutsideSunEntry(a_in, a_tables, o) ? kInputOutsideSunEntry : 0u),
						{ object.boundCenter[0], object.boundCenter[1], object.boundCenter[2] }, object.boundRadius, static_cast<std::uint32_t>(o), 0,
						PartitionsOf(a_tables, static_cast<std::uint32_t>(o)), streamIndex });
					if (entryOnGpu)
						SetSunEntryRow(inputs.back(), a_tables, o);
				}
			}
		}

		/**
		 * @brief Resolves the descriptor entries an epoch's build reads (render thread, descriptor service
		 * active): the null texture, the sampler table, the projected textures, and every material slot used
		 * this frame. Also the keep-alive: GpuTextures evicts an entry unused for kEvictFrames, and only the
		 * render thread's Resolve counts as a use, so every used slot is touched here each epoch.
		 */
		void RefreshMaterialLookups(const SceneStore::Tables& a_tables, std::uint32_t a_frame, const SceneStore::ProjectedTextures& a_projected, Lookups& a_lookups)
		{
			ZoneScopedN("CS.DCLF.RefreshMaterialLookups");
			auto& textures = GpuTextures::Get();
			// Any change a build can observe bumps the generation, including an entry resolved for the first
			// time: a job built before it deferred those draws, and must not stand in for a build made after.
			// Every change is also a new version of what it belongs to (Lookups::NextVersion), which the kept bindings key on.
			auto note = [&](std::uint32_t& a_slot, std::uint32_t a_value) {
				const bool changed = a_slot != a_value;
				if (changed)
					++a_lookups.generation;
				a_slot = a_value;
				return changed;
			};
			if (note(a_lookups.nullTexture, textures.NullIndex()))
				a_lookups.sharedVersion = a_lookups.NextVersion();
			if (!a_lookups.samplersResolved) {
				for (std::uint32_t address = 0; address < 4; ++address)
					for (std::uint32_t filter = 0; filter < 5; ++filter)
						a_lookups.samplers[Lookups::SamplerIndex(address, filter)] = textures.Sampler(address, filter);
				a_lookups.samplersResolved = true;
				a_lookups.sharedVersion = a_lookups.NextVersion();
			}
			for (std::size_t i = 0; i < a_lookups.projectedTextures.size(); ++i) {
				const std::uint32_t index = a_projected.valid ? textures.Resolve(a_projected.views[i]) : Lookups::kNone;
				if (note(a_lookups.projectedTextures[i], index))
					a_lookups.sharedVersion = a_lookups.NextVersion();
			}
			a_lookups.materials.resize(a_tables.materials.size());
			for (std::size_t word = 0; word < a_tables.usedMaterialBits.size(); ++word) {
				for (std::uint64_t remaining = a_tables.usedMaterialBits[word]; remaining; remaining &= remaining - 1) {
					const std::uint32_t slot = static_cast<std::uint32_t>(word * 64 + std::countr_zero(remaining));
					if (slot >= a_tables.materialLastUsed.size() || a_tables.materialLastUsed[slot] != a_frame)
						continue;
					auto& entry = a_lookups.materials[slot];
					const auto& key = a_tables.materialSlotKey[slot];
					if (entry.key != key) {
						entry.key = key;
						entry.resolved = false;
						entry.version = a_lookups.NextVersion();
					}
					const auto& material = a_tables.materials[slot];
					// Resolved from these same views, with none evicted since, recently enough that they are still
					// marked used: the indices stand (the shadow, Z-prepass and colour epochs each refresh).
					if (entry.resolved && entry.recordVersion == a_tables.materialVersion[slot] && entry.written == material.textureWritten && entry.texturesGeneration == textures.Generation() &&
						a_frame - entry.resolvedFrame < GpuTextures::kRestampFrames) {
						continue;
					}
					for (std::uint32_t t = 0; t < kPixelTextureSlots; ++t) {
						if (!((material.textureWritten >> t) & 1))
							continue;
						const std::uint32_t index = textures.Resolve(material.textures[t]);
						if (note(entry.textureIndex[t], index))
							entry.version = a_lookups.NextVersion();
						entry.views[t] = material.textures[t];
					}
					for (std::uint32_t f = 0; f < kFeatureMaterialTextures; ++f) {
						const std::uint32_t index = material.featureTextures[f] ? textures.Resolve(material.featureTextures[f]) : Lookups::kNone;
						if (note(entry.featureIndex[f], index))
							entry.version = a_lookups.NextVersion();
						entry.featureViews[f] = material.featureTextures[f];
					}
					if (!entry.resolved) {
						++a_lookups.generation;
						entry.version = a_lookups.NextVersion();
					}
					entry.resolved = true;
					entry.written = material.textureWritten;
					entry.texturesGeneration = textures.Generation();
					entry.recordVersion = a_tables.materialVersion[slot];
					entry.resolvedFrame = a_frame;
				}
			}
			// The technique's shadow mask, per used pipeline: the frame's view, so it is refreshed every epoch.
			a_lookups.pipelines.resize(std::max(a_lookups.pipelines.size(), a_tables.pipelines.size()));
			for (std::size_t word = 0; word < a_tables.usedPipelineBits.size(); ++word) {
				for (std::uint64_t remaining = a_tables.usedPipelineBits[word]; remaining; remaining &= remaining - 1) {
					const std::uint32_t p = static_cast<std::uint32_t>(word * 64 + std::countr_zero(remaining));
					if (p >= a_tables.pipelines.size())
						continue;
					if (!a_tables.PipelineUsed(p, a_frame))
						continue;
					const auto& technique = a_tables.TechniqueOf(p);
					auto& entry = a_lookups.pipelines[p];
					const std::uint32_t index = technique.shadowMask ? textures.Resolve(technique.shadowMaskTexture) : Lookups::kNone;
					if (note(entry.shadowMaskIndex, index))
						entry.version = a_lookups.NextVersion();
				}
			}
		}

		/**
		 * @brief Outside an epoch (render thread): brings resolved material entries up to date where every view
		 * that changed is one GpuTextures already knows. A volatile material's textures can change every frame
		 * - the character light's t11 alternates between two render targets - and a job kicked before the
		 * epoch's own refresh would otherwise be built against the old indices and go stale. Anything that
		 * needs an import is left to the epoch (RefreshMaterialLookups), where the descriptor service is active.
		 */
		void RefreshKnownMaterialTextures(SceneStore& a_store, Lookups& a_lookups)
		{
			ZoneScopedN("CS.DCLF.RefreshKnownMaterialTextures");
			const auto& a_tables = a_store.GetTables();
			const std::uint32_t a_frame = a_store.GetFrame();
			auto& textures = GpuTextures::Get();
			auto refreshSlot = [&](std::size_t slot) {
				if (slot >= a_tables.materialLastUsed.size() || a_tables.materialLastUsed[slot] != a_frame)
					return;
				auto& entry = a_lookups.materials[slot];
				const auto& material = a_tables.materials[slot];
				if (!entry.resolved || entry.key != a_tables.materialSlotKey[slot] || entry.written != material.textureWritten ||
					entry.recordVersion == a_tables.materialVersion[slot])
					return;
				std::array<std::uint32_t, kPixelTextureSlots> indices{};
				bool changed = false, known = true;
				for (std::uint32_t t = 0; t < kPixelTextureSlots && known; ++t) {
					if (!((material.textureWritten >> t) & 1) || entry.views[t] == material.textures[t])
						continue;
					changed = true;
					known = textures.Known(material.textures[t], indices[t]);
				}
				if (!changed || !known)
					return;
				for (std::uint32_t t = 0; t < kPixelTextureSlots; ++t) {
					if (!((material.textureWritten >> t) & 1) || entry.views[t] == material.textures[t])
						continue;
					if (entry.textureIndex[t] != indices[t]) {
						++a_lookups.generation;
						entry.version = a_lookups.NextVersion();
					}
					entry.textureIndex[t] = indices[t];
					entry.views[t] = material.textures[t];
				}
				if (entry.featureViews == material.featureTextures)
					entry.recordVersion = a_tables.materialVersion[slot];
			};
			std::vector<std::uint32_t> changed;
			a_store.TakeMaterialTextureChanges(changed);
			for (const std::uint32_t slot : changed)
				if (slot < a_tables.materials.size() && slot < a_lookups.materials.size())
					refreshSlot(slot);
		}

		/** @brief The shadow epoch's entries: the alpha-tested casters' diffuse textures, and the pipelines of the modes in use. */
		void RefreshShadowLookups(const SceneStore::Tables& a_tables, const std::array<bool, kShadowModeCount>& a_modeUsed,
			const std::array<std::uint32_t, kShadowModeCount>& a_modeRasterStates, DXGI_FORMAT a_dsvFormat, DXGI_FORMAT a_skyFormat, Lookups& a_lookups)
		{
			ZoneScopedN("CS.DCLF.RefreshShadowLookups");
			auto& textures = GpuTextures::Get();
			auto& pipelines = DrawPipelines::Get();
			auto& programs = ShaderPrograms::Get();
			auto* utility = globals::game::utilityShader;
			TracyCZoneN(shadowTexturesZone, "CS.DCLF.RefreshShadow.Textures", true);
			for (auto* srv : a_tables.shadowTextureSet) {
				const std::uint32_t index = textures.Resolve(srv);
				auto [it, inserted] = a_lookups.shadowTextures.try_emplace(srv, index);
				if (inserted) {
					++a_lookups.generation;
				} else if (it->second != index) {
					it->second = index;
					++a_lookups.generation;
				}
			}
			TracyCZoneEnd(shadowTexturesZone);
			if (!utility)
				return;
			// Request setup hashes the shader dependency tree and may take 10+ ms. Missing pipelines leave their
			// casters on the native path, so introduce at most one new Utility technique per frame instead of
			// multiplying a render-thread hitch when a scene exposes several techniques at once.
			bool mayRequestProgram = true;
			TracyCZoneN(shadowPipelinesZone, "CS.DCLF.RefreshShadow.Pipelines", true);
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
				if (!a_modeUsed[m])
					continue;
				// Skylighting's map takes its own keys (complete techniques) and its target's format.
				const auto& keys = m == kSkyMode ? a_tables.skyKeysUsed : a_tables.shadowKeysUsed;
				const DXGI_FORMAT format = m == kSkyMode ? a_skyFormat : a_dsvFormat;
				for (const auto& key : keys) {
					const std::uint32_t modeBits = ModeBitsOf(m);
					const ShadowPipelineKey slotKey{ key.technique | modeBits, key.rasterFlags, key.vertexLayout };
					auto slotIt = a_lookups.shadowSlots.find(slotKey);
					if (slotIt == a_lookups.shadowSlots.end()) {
						if (a_lookups.shadowSlotKeys.size() >= kMaxShadowSlots)
							continue;  // its casters stay native
						slotIt = a_lookups.shadowSlots.emplace(slotKey, static_cast<std::uint32_t>(a_lookups.shadowSlotKeys.size())).first;
						a_lookups.shadowSlotKeys.push_back(slotKey);
						++a_lookups.generation;
					}
					const std::uint32_t slot = slotIt->second;
					const auto* program = [&] {
						ZoneScopedN("CS.DCLF.RefreshShadow.FindProgram");
						bool requested = false;
						const auto* found = programs.FindShadow(slotKey.technique, *utility, mayRequestProgram, &requested);
						mayRequestProgram &= !requested;
						return found;
					}();
					// The key under each rasterizer state its mode's views draw with.
					for (std::uint32_t states = (a_modeRasterStates[m] & 0xFFFFu) | (a_modeRasterStates[m] >> 16); states; states &= states - 1) {
						const auto state = static_cast<std::uint32_t>(std::countr_zero(states));
						const ShadowPipelineKey viewKey{ slotKey.technique, WithShadowState(slotKey.rasterFlags, state), slotKey.vertexLayout };
						const std::uint32_t set = [&] {
							ZoneScopedN("CS.DCLF.RefreshShadow.FindPipeline");
							return program ? pipelines.FindShadow(viewKey, *program, format) : DrawPipelines::kNotReady;
						}();
						const std::uint32_t index = set == DrawPipelines::kNotReady ? Lookups::kNone : set;
						auto [it, inserted] = a_lookups.shadowPipelines.try_emplace(viewKey, index);
						if (inserted || it->second != index) {
							++a_lookups.generation;
							it->second = index;
						}
						auto& row = a_lookups.shadowMapRows[state];
						if (row.size() <= slot)
							row.resize(slot + 1, Lookups::kNone);
						row[slot] = index;
					}
				}
			}
			TracyCZoneEnd(shadowPipelinesZone);
		}

		constexpr std::size_t kAsyncColour = 0;
		constexpr std::size_t kAsyncZPrepass = 1;
		constexpr std::size_t kAsyncShadow = 2;

		// Whether a build made for `a_job` serves an epoch whose inputs are `a_epoch`: every scalar the build
		// read has to agree, the lookup generation is checked separately after the commit's refresh.
		// Whether BuildMainPayload reads MainInputs::eye/previousEye: the non-bindless path writes eye-relative
		// PerGeometry groups; a bindless build only for CS_DCLF_BINDLESS_PARITY.
		bool BuildReadsEye(const MainInputs& a_in)
		{
			return !a_in.bindless || a_in.bindlessParity;
		}

		bool SameInputs(const MainInputs& a_job, const MainInputs& a_epoch)
		{
			auto sameEye = [](const RE::NiPoint3& a, const RE::NiPoint3& b) { return std::memcmp(&a, &b, sizeof(RE::NiPoint3)) == 0; };
			return a_job.frameNumber == a_epoch.frameNumber && a_job.depthOnly == a_epoch.depthOnly && a_job.hybrid == a_epoch.hybrid &&
			       a_job.resolveTextures == a_epoch.resolveTextures && a_job.bindless == a_epoch.bindless && a_job.bindlessDraws == a_epoch.bindlessDraws &&
			       a_job.dedupParity == a_epoch.dedupParity && a_job.bindlessParity == a_epoch.bindlessParity && a_job.withholding == a_epoch.withholding &&
			       a_job.requireNativeVisible == a_epoch.requireNativeVisible && a_job.linearLighting == a_epoch.linearLighting &&
			       a_job.renderFlags == a_epoch.renderFlags && sameEye(a_job.eye, a_epoch.eye) && sameEye(a_job.previousEye, a_epoch.previousEye) &&
			       a_job.vsFrameMask == a_epoch.vsFrameMask && a_job.psFrameMask == a_epoch.psFrameMask && a_job.decalCount == a_epoch.decalCount &&
			       a_job.drawnCommitted == a_epoch.drawnCommitted && a_job.drawnResync == a_epoch.drawnResync &&
			       a_job.materialPatchedFloats == a_epoch.materialPatchedFloats &&
			       a_job.materialPatchedVSFloats == a_epoch.materialPatchedVSFloats &&
			       a_job.addresses == a_epoch.addresses &&
			       a_job.lookupGeneration == a_epoch.lookupGeneration && a_job.tablesGeneration == a_epoch.tablesGeneration;
		}

		bool SameShadowInputs(const ShadowInputs& a_job, const ShadowInputs& a_epoch)
		{
			return a_job.frameNumber == a_epoch.frameNumber && a_job.renderFlags == a_epoch.renderFlags &&
			       std::memcmp(&a_job.refEye, &a_epoch.refEye, sizeof(RE::NiPoint3)) == 0 && a_job.modeUsed == a_epoch.modeUsed &&
			       a_job.modeRasterStates == a_epoch.modeRasterStates && a_job.sunEntryPlanes == a_epoch.sunEntryPlanes &&
			       a_job.sunEntryPlaneMasks == a_epoch.sunEntryPlaneMasks && a_job.sunCandidates == a_epoch.sunCandidates &&
			       a_job.addresses == a_epoch.addresses && a_job.sharedData == a_epoch.sharedData && a_job.featureData == a_epoch.featureData &&
			       a_job.lookupGeneration == a_epoch.lookupGeneration && a_job.tablesGeneration == a_epoch.tablesGeneration &&
			       a_job.sceneRebuilds == a_epoch.sceneRebuilds;
		}

		// CS_DCLF_ASYNC=probe: two builds of the same inputs, compared byte for byte. The first difference is
		// named by buffer and offset.
		bool SamePayload(const MainPayload& a, const MainPayload& b, std::string& a_difference)
		{
			auto bytesDiffer = [&](const char* a_name, const void* a_lhs, std::size_t a_lhsBytes, const void* a_rhs, std::size_t a_rhsBytes) {
				if (a_lhsBytes != a_rhsBytes) {
					a_difference = fmt::format("{}: {} vs {} bytes", a_name, a_lhsBytes, a_rhsBytes);
					return true;
				}
				const auto* l = static_cast<const std::uint8_t*>(a_lhs);
				const auto* r = static_cast<const std::uint8_t*>(a_rhs);
				for (std::size_t i = 0; i < a_lhsBytes; ++i) {
					if (l[i] != r[i]) {
						a_difference = fmt::format("{}: byte {} of {} ({:#x} vs {:#x})", a_name, i, a_lhsBytes, l[i], r[i]);
						return true;
					}
				}
				return false;
			};
			auto vectorDiffers = [&](const char* a_name, const auto& a_lhs, const auto& a_rhs) {
				using T = std::remove_cvref_t<decltype(a_lhs[0])>;
				return bytesDiffer(a_name, a_lhs.data(), a_lhs.size() * sizeof(T), a_rhs.data(), a_rhs.size() * sizeof(T));
			};
			if (vectorDiffers("constants", a.arena.Bytes(), b.arena.Bytes()) || vectorDiffers("records", a.records, b.records) ||
				vectorDiffers("sequences", a.sequences, b.sequences) || vectorDiffers("inputs", a.inputList, b.inputList) ||
				vectorDiffers("geometries", a.geometryDraws.Flat(), b.geometryDraws.Flat()) || bytesDiffer("objects", a.objectRecords.At(0), a.objectRecords.Count() * sizeof(BindlessObject), b.objectRecords.At(0),
					b.objectRecords.Count() * sizeof(BindlessObject)) ||
				a.bones.Rows() != b.bones.Rows() || vectorDiffers("patches", a.framePatches, b.framePatches) ||
				a.drawnChanges.size() != b.drawnChanges.size())
				return false;
			for (std::uint32_t group = 0; group < kDecalGroups; ++group) {
				if (vectorDiffers("decal templates", a.decalTemplates[group], b.decalTemplates[group]))
					return false;
			}
			if (a.decalCount != b.decalCount || a.skipped != b.skipped || a.missingVertexConstants != b.missingVertexConstants ||
				a.missingPixelConstants != b.missingPixelConstants || a.decalsDrawn != b.decalsDrawn || a.shortBuffers != b.shortBuffers ||
				a.deferredTextures != b.deferredTextures) {
				a_difference = "counts";
				return false;
			}
			return true;
		}

		bool SameShadowPayload(const ShadowPayload& a, const ShadowPayload& b, std::string& a_difference)
		{
			auto vectorDiffers = [&](const char* a_name, const auto& a_lhs, const auto& a_rhs) {
				using T = std::remove_cvref_t<decltype(a_lhs[0])>;
				const std::size_t lhsBytes = a_lhs.size() * sizeof(T), rhsBytes = a_rhs.size() * sizeof(T);
				if (lhsBytes != rhsBytes) {
					a_difference = fmt::format("{}: {} vs {} bytes", a_name, lhsBytes, rhsBytes);
					return true;
				}
				if (lhsBytes && std::memcmp(a_lhs.data(), a_rhs.data(), lhsBytes) != 0) {
					const auto* l = reinterpret_cast<const std::uint8_t*>(a_lhs.data());
					const auto* r = reinterpret_cast<const std::uint8_t*>(a_rhs.data());
					std::size_t k = 0;
					while (l[k] == r[k])
						++k;
					a_difference = fmt::format("{}: byte {} of {} ({:#x} vs {:#x})", a_name, k, lhsBytes, l[k], r[k]);
					return true;
				}
				return false;
			};
			// The object records by content: the worker's may be the store's.
			if (a.objects.Count() != b.objects.Count() || (a.objects.Count() && std::memcmp(a.objects.At(0), b.objects.At(0), a.objects.Count() * sizeof(BindlessObject)) != 0)) {
				a_difference = "objects";
				return false;
			}
			if (vectorDiffers("constants", a.arena.Bytes(), b.arena.Bytes()) || vectorDiffers("records", a.records, b.records) ||
				vectorDiffers("object records", a.objectRecord, b.objectRecord) || a.bones.Rows() != b.bones.Rows() ||
				vectorDiffers("geometries", a.geometries.Flat(), b.geometries.Flat()))
				return false;
			for (std::size_t m = 0; m < a.inputList.size(); ++m) {
				if (vectorDiffers("inputs", a.Flat(static_cast<std::uint32_t>(m)), b.Flat(static_cast<std::uint32_t>(m))))
					return false;
			}
			if (a.skippedTexture != b.skippedTexture || a.skippedPipeline != b.skippedPipeline || a.deferredTextures != b.deferredTextures ||
				a.deferredPipelines != b.deferredPipelines) {
				a_difference = "the skip counters";
				return false;
			}
			return true;
		}
	}

	namespace
	{
		// A commit's own uploads on the render thread, staged directly (StagedUploadBatch) and submitted when the
		// commit ends: each is one memcpy into mapped staging here and one copy in the submission, instead of a
		// trip through the upload pass's per-upload bookkeeping. The batch comes from a pool of released ones.
		class CommitUploads
		{
		public:
			explicit CommitUploads(std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& a_pool)
			{
				for (const auto& candidate : a_pool) {
					if (candidate.use_count() == 1) {
						batch = candidate;
						break;
					}
				}
				if (!batch) {
					batch = org::runtime::StagedUploadBatch::Create(std::size_t{ 1 } << 20);
					a_pool.push_back(batch);
				}
				batch->Reset();
			}
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

	}

	void StageMainPayload(MainPayload& a_payload, const Resources& a_resources, std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& a_pool);
	void StageShadowPayload(ShadowPayload& a_payload, const ShadowResources& a_resources, std::uint32_t a_slots,
		std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& a_pool);

	struct IndirectDraws::Impl
	{
		std::shared_ptr<Resources> resources;
		// What the resources were created for; a change rebuilds them (and the graph).
		TargetFormats formats{};
		std::uint32_t width = 0, height = 0;
		std::uint32_t pipelineGeneration = ~0u;
		std::uint64_t serial = 0;
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
		/** @brief A view the hook captured for the frame's epoch (ExecuteShadowView, ExecuteShadowFrame). */
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
		std::vector<PendingView> pendingViews;
		std::vector<DrawBindings> shadowSlotRecords;  // one slot's copy of the records, while it uploads
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
		RE::NiPoint3 shadowRefEye;
		std::uint64_t shadowSerial = 0;
		ShadowPayload shadowPayload;
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
		// Per main job: the staged upload batches its builds write (StageMainPayload), reused once released.
		std::array<std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>, 2> stagedPools;
		// The commits' own uploads on the render thread (CommitUploads).
		std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>> commitStagedPool;
		std::array<BuildCache, 2> buildCaches;  // indexed like mainPayload
		BuildCache* CacheFor(std::size_t a_job) { return BuildCacheEnabled() ? &buildCaches[a_job] : nullptr; }
		// The persistent object records: the main epochs' buffer's (both segments'), and the shadow epoch's.
		ObjectRecordStore mainObjects, shadowObjects;
		BonesStore mainBones, shadowBones;  // likewise the bone rows
		ShadowKept shadowKept;  // the shadow epoch's inputs and records (Step 6)
		ShadowKept* ShadowKeptState() { return PersistentShadowEnabled() && PersistentObjectsEnabled() ? &shadowKept : nullptr; }
		BonesStore* MainBones() { return PersistentObjectsEnabled() ? &mainBones : nullptr; }
		BonesStore* ShadowBones() { return PersistentObjectsEnabled() ? &shadowBones : nullptr; }
		ObjectRecordStore* MainObjects() { return PersistentObjectsEnabled() ? &mainObjects : nullptr; }
		ObjectRecordStore* ShadowObjects() { return PersistentObjectsEnabled() ? &shadowObjects : nullptr; }
		GeometryStore mainGeometries, shadowGeometries;  // likewise the geometry slots' draws
		SunExclusionCache sunExclusionCache;  // the shadow builds', in frame order
		GeometryStore* MainGeometries() { return PersistentObjectsEnabled() ? &mainGeometries : nullptr; }
		GeometryStore* ShadowGeometries() { return PersistentObjectsEnabled() ? &shadowGeometries : nullptr; }
		std::array<std::uint32_t, 4> decalWords{};  // the count buffer's decal words, uploaded per colour epoch

		/** @brief The per-frame constant blocks of an epoch from the capture's mirrors (render thread; records the Z-prepass's bytes for the replay). */
		void PackFrameBlocks(const Capture& a_capture, bool a_depthOnly, const Resources& a_resources, FrameBlocks& a_out);
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
			const std::shared_ptr<Resources>& a_resources, SceneStore& a_store, IndirectDraws::Stats& a_stats);

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
		// The viewport transform the depth was written with. Skyrim's depth pass and its main pass do not
		// use the same depth range - 0.999968 against 0.999998 - and the range scales the value that lands
		// in the buffer, so the same vertex is written about 500 D24 units apart in the two passes. The
		// colour pass tests EQUAL against that depth, so it has to rasterise with the prepass's range.
		float prepassMinDepth = 0.0f, prepassMaxDepth = 1.0f;
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

		bool Setup(const Capture& a_capture, bool a_depthOnly = false)
		{
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host)
				return NotReady(0, "no render graph");
			if (a_depthOnly) {
				// The native depth pass binds no colour targets, so there is nothing to build from here. The
				// Z-prepass reuses what the main pass built last frame; on the first frame there is none yet
				// and the prepass simply does not run.
				return resources != nullptr;
			}
			if (a_capture.targetCount == 0)
				return NotReady(1, "no render targets bound");
			D3D11_TEXTURE2D_DESC target{};
			a_capture.targets[0]->GetDesc(&target);
			const auto& targets = DrawPipelines::Get().Targets();
			std::uint32_t lights, list, grid;
			const bool lightLimitFix = ORGLightCulling::Get().GetShaderResourceIndices(lights, list, grid);
			auto frameBuffers = FrameBuffersOf(a_capture, lightLimitFix);
			const bool sameFrameBuffers = resources && resources->frameBuffers.size() == frameBuffers.size() &&
			                              std::equal(frameBuffers.begin(), frameBuffers.end(), resources->frameBuffers.begin(), [](const auto& a, const auto& b) { return a.SameShape(b); });
			if (resources && targets == formats && target.Width == width && target.Height == height && sameFrameBuffers &&
				resources->lightLimitFix == lightLimitFix)
				return true;

			auto device = host->GetDesc().device;
			auto state = std::make_shared<Resources>();
			auto buffer = [&](std::uint64_t a_bytes, const char* a_name) {
				auto created = org::Buffer::CreateShared(rhi::HeapType::DeviceLocal, a_bytes, false);
				created->SetName(a_name);
				return created;
			};
			state->constants = buffer(kConstantBytes, "cs.dclf.constants");
			state->recordCapacity = BindlessDraws() ? kMaxRecordsDeduplicated : kMaxDraws;
			state->records = buffer(std::uint64_t(state->recordCapacity) * sizeof(DrawBindings), "cs.dclf.records");
			state->constantsDepth = buffer(kDepthConstantBytes, "cs.dclf.constants-depth");
			state->recordsDepth = buffer(std::uint64_t(state->recordCapacity) * sizeof(DrawBindings), "cs.dclf.records-depth");
			// Structured rather than raw, because the shaders read it through an SRV at t127 instead of
			// through a device address the way the constants and the binding records are read.
			state->objects = org::Buffer::CreateUnmaterializedStructuredBuffer(kMaxObjects, sizeof(BindlessObject), false);
			state->objects->SetName("cs.dclf.objects");
			state->objects->Materialize();
			state->objectsIndex = state->objects->GetSRVInfo(0).slot.index;
			state->bones = org::Buffer::CreateUnmaterializedStructuredBuffer(kMaxBoneRows, 16, false);
			state->bones->SetName("cs.dclf.bones");
			state->bones->Materialize();
			state->bonesIndex = state->bones->GetSRVInfo(0).slot.index;
			state->facePositions = buffer(std::uint64_t(kFacePositionVertices) * 16, "cs.dclf.face-positions");
			state->facePositionsAddress = device.GetBufferDeviceAddress({ state->facePositions->GetAPIResource().GetHandle(), 0 });
			// Twice kMaxDraws: phase 1 and the colour segment write the first half, phase 2 the second. The
			// CPU records where phase 2's draw starts, so the two need ranges fixed in advance rather than
			// one range shared through an atomic counter.
			state->sequences = CreateWords(std::uint64_t(kSequenceSlots) * sizeof(DrawSequence) / 4, true, "cs.dclf.sequences");
			state->count = CreateWords(kCountWords, true, "cs.dclf.draw-count");
			// One word per object in the frame's tables: what the depth segment's culling decided, read by
			// the colour segment so that it draws exactly the same set.
			state->visibility = CreateWords(kMaxObjects, true, "cs.dclf.visibility");
			state->frustum = CreateWords(kMaxObjects, true, "cs.dclf.frustum");
			{
				// The feedback ring: at least as many slots as frames in flight, and at least 4.
				auto feedback = std::make_shared<Resources::Feedback>();
				auto timeline = std::make_shared<rhi::TimelinePtr>();
				if (rhi::Failed(device.CreateTimeline(*timeline, 0, "cs.dclf.feedback")) || !*timeline) {
					logger::warn("[DCLF] visibility feedback: no timeline; the feedback is off");
				} else {
					feedback->timeline = std::move(timeline);
					const std::uint32_t slots = std::max<std::uint32_t>(host->FrameSlots(), 4u);
					for (std::uint32_t i = 0; i < slots; ++i) {
						auto slot = std::make_unique<Resources::Feedback::Slot>();
						slot->staging = org::Buffer::CreateShared(rhi::HeapType::Readback, std::uint64_t(kMaxObjects) * sizeof(std::uint32_t));
						slot->staging->SetName(fmt::format("cs.dclf.feedback{}", i).c_str());
						feedback->slots.push_back(std::move(slot));
					}
					state->feedback = std::move(feedback);
				}
			}
			if (PassStats::Enabled()) {
				auto passStats = std::make_shared<PassStats>();
				const std::uint32_t slots = host->FrameSlots();
				rhi::QueryPoolDesc desc{};
				desc.type = rhi::QueryType::PipelineStatistics;
				desc.count = slots * PassStats::kKinds;
				desc.statsMask = rhi::PipelineStatBits::PS_IAVertices | rhi::PipelineStatBits::PS_IAPrimitives | rhi::PipelineStatBits::PS_VSInvocations |
				                 rhi::PipelineStatBits::PS_PSInvocations;
				desc.requireAllStats = true;
				if (device.CreateQueryPool(desc, passStats->pool) != rhi::Result::Ok || !passStats->pool) {
					logger::warn("[DCLF] pass statistics: no pipeline statistics query pool");
				} else {
					for (std::uint32_t i = 0; i < slots; ++i)
						passStats->readback.push_back(org::Buffer::CreateShared(rhi::HeapType::Readback, std::uint64_t(PassStats::kKinds) * PassStats::kFields * sizeof(std::uint64_t)));
					passStats->written.resize(slots);
					state->passStats = std::move(passStats);
					logger::info("[DCLF] pass statistics on: {} frame slots", slots);
				}
			}
			if (DgcPreprocessEnabled()) {
				state->preprocessMain = PreprocessStates::Create(device, host->FrameSlots(), "the main segment");
			}
			state->inputs = CreateWords(std::uint64_t(kMaxInputs) * sizeof(DrawInput) / 4, false, "cs.dclf.draw-inputs");
			state->inputsDepth = CreateWords(std::uint64_t(kMaxInputs) * sizeof(DrawInput) / 4, false, "cs.dclf.draw-inputs-depth");
			state->geometries = CreateWords(std::uint64_t(kMaxGeometries) * sizeof(GeometryDraw) / 4, false, "cs.dclf.geometries");
			state->buildDraws = ComputeProgram::Load(device, { .source = kBuildDrawsShader, .constantWords = kBuildDrawsConstantWords });
			if (!state->buildDraws)
				return NotReady(7, "the BuildDraws program could not be created");
			state->dispatchSignature = CreateDispatchSignature(device, state->buildDraws->layout->GetHandle());
			if (!state->dispatchSignature)
				return NotReady(7, "the BuildDraws dispatch signature could not be created");
			state->latch = std::make_shared<org::LatchBlock>("cs.dclf.latch", static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), host->FrameSlots());
			if (SortDraws())
				state->sort = DrawSort::Create(device);
			if (BuildParityEnabled()) {
				auto wrap = [](org::Buffer& a_buffer, std::uint64_t a_bytes) {
					D3D11_BUFFER_DESC desc{};
					desc.ByteWidth = static_cast<UINT>(a_bytes);
					desc.Usage = D3D11_USAGE_DEFAULT;
					desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
					desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
					desc.StructureByteStride = sizeof(std::uint32_t);
					return RenderGraphRuntime::Get().WrapBuffer(a_buffer, desc);
				};
				state->sequencesD3D11 = wrap(*state->sequences, std::uint64_t(kSequenceSlots) * sizeof(DrawSequence));

			}
			if (!SwitchValue("CS_DCLF_GBUFFER_PROBE").empty()) {
				state->probe = org::Buffer::CreateShared(rhi::HeapType::DeviceLocal, std::uint64_t(kProbeSlots) * kProbeSlotBytes, false);
				state->probe->SetName("cs.dclf.gbuffer-probe");
				D3D11_BUFFER_DESC desc{};
				desc.ByteWidth = kProbeSlots * kProbeSlotBytes;
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
				desc.StructureByteStride = sizeof(std::uint32_t);
				state->probeD3D11 = RenderGraphRuntime::Get().WrapBuffer(*state->probe, desc);
			}
			{
				// The draw-count buffer is read back for the culling counters whether or not parity is on. The
				// width must cover every counter word: a view that stops short reads zeros for the rest, and
				// a counter that is always zero reads exactly like a clean result.
				D3D11_BUFFER_DESC desc{};
				desc.ByteWidth = kCountWords * sizeof(std::uint32_t);
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
				desc.StructureByteStride = sizeof(std::uint32_t);
				state->countD3D11 = RenderGraphRuntime::Get().WrapBuffer(*state->count, desc);
			}
			if (SetParityEnabled()) {
				D3D11_BUFFER_DESC desc{};
				desc.ByteWidth = kMaxObjects * sizeof(std::uint32_t);
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
				desc.StructureByteStride = sizeof(std::uint32_t);
				state->visibilityD3D11 = RenderGraphRuntime::Get().WrapBuffer(*state->visibility, desc);
			}
			state->frameConstants = buffer(kFrameConstantBytes, "cs.dclf.frame-constants");
			state->constantsAddress = device.GetBufferDeviceAddress({ state->constants->GetAPIResource().GetHandle(), 0 });
			state->recordsAddress = device.GetBufferDeviceAddress({ state->records->GetAPIResource().GetHandle(), 0 });
			state->constantsDepthAddress = device.GetBufferDeviceAddress({ state->constantsDepth->GetAPIResource().GetHandle(), 0 });
			state->recordsDepthAddress = device.GetBufferDeviceAddress({ state->recordsDepth->GetAPIResource().GetHandle(), 0 });
			state->frameConstantsAddress = device.GetBufferDeviceAddress({ state->frameConstants->GetAPIResource().GetHandle(), 0 });

			if (!state->constantsAddress || !state->recordsAddress || !state->frameConstantsAddress) {
				logger::error("[DCLF] Draw data buffers have no device address");
				return false;
			}

			// Off-screen copies of the main pass's targets.
			state->targetCount = targets.colorCount;
			for (std::uint32_t i = 0; i < targets.colorCount; ++i) {
				org::TextureDescription desc{};
				desc.imageDimensions.push_back({ target.Width, target.Height, 0, 0 });
				desc.format = rhi::helpers::ToRHI(targets.colors[i]);
				desc.channels = 4;
				desc.hasRTV = true;
				desc.rtvFormat = desc.format;
				desc.hasSRV = true;
				desc.srvFormat = desc.format;
				desc.clearColor[3] = 0.0f;
				state->targets[i] = org::PixelBuffer::CreateSharedUnmaterialized(desc);  // the graph materializes them
				state->targets[i]->SetName(fmt::format("cs.dclf.target{}", i));
			}

			// DCLF's own depth, with the main depth's format.
			org::TextureDescription depthDesc{};
			depthDesc.imageDimensions.push_back({ target.Width, target.Height, 0, 0 });
			depthDesc.format = rhi::helpers::ToRHI(targets.depth);
			depthDesc.channels = 1;
			depthDesc.hasDSV = true;
			depthDesc.dsvFormat = depthDesc.format;
			{
				auto depth = org::PixelBuffer::CreateSharedUnmaterialized(depthDesc);
				depth->SetName("cs.dclf.depth");
				state->depth = std::move(depth);
			}
			state->width = target.Width;
			state->height = target.Height;

			state->hybrid = HybridEnabled();
			if (DebugViewEnabled() || state->hybrid) {
				for (std::uint32_t i = 0; i < targets.colorCount; ++i) {
					DxvkOrgInteropResourceInfo info{};
					if (!a_capture.targets[i] || !RenderGraphRuntime::Get().DescribeResource(a_capture.targets[i].get(), info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
						return NotReady(5, "a main-pass target cannot be described");
					org::TextureDescription desc{};
					desc.format = rhi::helpers::ToRHI(targets.colors[i]);
					desc.channels = 4;
					desc.hasRTV = true;
					desc.rtvFormat = desc.format;
					state->native[i] = ImportImage(device, info.image, desc, "DCLF native target");
					if (!state->native[i])
						return NotReady(6, "a main-pass target could not be imported");
				}
			}
			if (state->hybrid) {
				DxvkOrgInteropResourceInfo info{};
				if (!a_capture.depth || !RenderGraphRuntime::Get().DescribeResource(a_capture.depth.get(), info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
					return NotReady(10, "the main-pass depth cannot be described (hybrid)");
				// Attachable and readable, from one import. The HZB build reads the same image the draws
				// write, and importing it twice would give the graph two resources it believes are unrelated:
				// it would order nothing between the depth draws and the read, and insert no barrier, so the
				// build would reduce whatever happened to be there - in practice the cleared far plane.
				org::TextureDescription nativeDepthDesc = depthDesc;
				nativeDepthDesc.hasSRV = true;
				nativeDepthDesc.srvFormat = DepthReadFormat(nativeDepthDesc.format);
				state->nativeDepth = ImportImage(device, info.image, nativeDepthDesc, "DCLF native depth");
				if (!state->nativeDepth)
					return NotReady(11, "the main-pass depth could not be imported (hybrid)");
			}
			// Bisect: with the debug view on as well, the hybrid path keeps its native depth and its skipping
			// but draws colour into DCLF's own targets, which are then copied over the native ones. If that
			// is correct while drawing straight into the native targets is not, the difference is the
			// destination rather than anything DCLF assembles or draws.
			const bool offscreenColour = !state->hybrid || DebugViewEnabled();
			state->offscreen = offscreenColour;
			for (std::uint32_t i = 0; i < targets.colorCount; ++i)
				state->drawTargets[i] = offscreenColour ? std::static_pointer_cast<org::Resource>(state->targets[i]) : std::static_pointer_cast<org::Resource>(state->native[i]);
			state->drawDepth = state->hybrid ? std::static_pointer_cast<org::Resource>(state->nativeDepth) : state->depth;

			// Phase 4: the hierarchical depth buffer.
			//
			// Mip 0 is half the next power of two of the depth, so the chain is a clean sequence of halvings
			// and a mip level can be chosen from a screen-space extent by log2 alone. Padding to a power of
			// two is what makes that true; the padded texels are outside the real depth and the build fills
			// them with the far plane, which suppresses culling rather than causing it.
			if (state->hybrid) {
				auto nextPowerOfTwo = [](std::uint32_t a_value) {
					std::uint32_t result = 1;
					while (result < a_value)
						result <<= 1;
					return result;
				};
				state->hzbWidth = std::max(1u, nextPowerOfTwo(state->width) / 2);
				state->hzbHeight = std::max(1u, nextPowerOfTwo(state->height) / 2);
				state->hzbMips = 1;
				for (std::uint32_t size = std::max(state->hzbWidth, state->hzbHeight); size > 1; size >>= 1)
					++state->hzbMips;

				org::TextureDescription hzbDesc{};
				for (std::uint32_t mip = 0; mip < state->hzbMips; ++mip)
					hzbDesc.imageDimensions.push_back({ std::max(1u, state->hzbWidth >> mip), std::max(1u, state->hzbHeight >> mip), 0, 0 });
				hzbDesc.format = rhi::Format::R32_Float;
				hzbDesc.channels = 1;
				hzbDesc.hasUAV = true;
				hzbDesc.uavFormat = hzbDesc.format;
				hzbDesc.hasSRV = true;
				hzbDesc.srvFormat = hzbDesc.format;
				state->hzb = org::PixelBuffer::CreateSharedUnmaterialized(hzbDesc);
				state->hzb->SetName("cs.dclf.hzb");

				state->hzbProgram = ComputeProgram::Load(device, { .source = kHzbShader, .constantWords = kHzbConstantWords });
				if (!state->hzbProgram) {
					// The culling falls back to frustum only; the frame is unaffected.
					logger::warn("[DCLF] The HZB compute program could not be created; occlusion culling stays off");
					state->hzb.reset();
				}
			}

			state->lightLimitFix = lightLimitFix;
			for (auto& frameBuffer : frameBuffers) {
				frameBuffer.copy = org::Buffer::CreateUnmaterializedStructuredBuffer(frameBuffer.elements, frameBuffer.stride, false);
				frameBuffer.copy->SetName(fmt::format("cs.dclf.frame-buffer.t{}", frameBuffer.textureRegister));
				frameBuffer.copy->Materialize();
			}
			state->frameBuffers = std::move(frameBuffers);

			resources = state;
			formats = targets;
			width = target.Width;
			height = target.Height;
			// The graph is rebuilt with the new resources on its next epoch.
			host->AddExtension(kExtensionId, [state] { return std::make_unique<MainOpaqueExtension>(state); });
			logger::info("[DCLF] Main-pass graph resources: {} targets {}x{}, depth format {}", targets.colorCount, width, height, static_cast<int>(targets.depth));
			return true;
		}
	};

	bool IndirectDraws::Impl::SetupShadow()
	{
		if (shadow)
			return true;
		if (shadowSetupFailed)
			return false;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return ShadowNotReady(0, "no render graph");
		auto device = host->GetDesc().device;
		auto state = std::make_shared<ShadowResources>();
		auto buffer = [&](std::uint64_t a_bytes, const char* a_name) {
			auto created = org::Buffer::CreateShared(rhi::HeapType::DeviceLocal, a_bytes, false);
			created->SetName(a_name);
			return created;
		};
		state->constants = buffer(kShadowConstantBytes, "cs.dclf.shadow.constants");
		state->records = buffer(std::uint64_t(kMaxShadowViews) * kShadowRecordCapacity * sizeof(DrawBindings), "cs.dclf.shadow.records");
		state->objects = org::Buffer::CreateUnmaterializedStructuredBuffer(kMaxObjects, sizeof(BindlessObject), false);
		state->objects->SetName("cs.dclf.shadow.objects");
		state->objects->Materialize();
		state->objectsIndex = state->objects->GetSRVInfo(0).slot.index;
		state->bones = org::Buffer::CreateUnmaterializedStructuredBuffer(kMaxBoneRows, 16, false);
		state->bones->SetName("cs.dclf.shadow.bones");
		state->bones->Materialize();
		state->bonesIndex = state->bones->GetSRVInfo(0).slot.index;
		state->facePositions = buffer(std::uint64_t(kFacePositionVertices) * 16, "cs.dclf.shadow.face-positions");
		for (std::uint32_t s = 0; s < kMaxShadowViews; ++s) {
			state->sequences[s] = CreateWords(std::uint64_t(kMaxDraws) * sizeof(DrawSequence) / 4, true, fmt::format("cs.dclf.shadow.sequences{}", s).c_str());
			state->count[s] = CreateWords(kCountWords, true, fmt::format("cs.dclf.shadow.draw-count{}", s).c_str());
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = kCountWords * sizeof(std::uint32_t);
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = sizeof(std::uint32_t);
			state->countD3D11[s] = RenderGraphRuntime::Get().WrapBuffer(*state->count[s], desc);
		}
		state->visibility = CreateWords(kMaxObjects, true, "cs.dclf.shadow.visibility");
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
			state->inputs[m] = CreateWords(std::uint64_t(kMaxInputs) * sizeof(DrawInput) / 4, false, fmt::format("cs.dclf.shadow.draw-inputs{}", m).c_str());
		state->geometries = CreateWords(std::uint64_t(kMaxGeometries) * sizeof(GeometryDraw) / 4, false, "cs.dclf.shadow.geometries");
		state->buildDraws = ComputeProgram::Load(device, { .source = kBuildDrawsShader, .constantWords = kBuildDrawsConstantWords });
		if (!state->buildDraws) {
			shadowSetupFailed = true;
			return ShadowNotReady(1, "the BuildDraws compute program could not be created");
		}
		state->dispatchSignature = CreateDispatchSignature(device, state->buildDraws->layout->GetHandle());
		if (!state->dispatchSignature) {
			shadowSetupFailed = true;
			return ShadowNotReady(1, "the BuildDraws dispatch signature could not be created");
		}
		state->latch = std::make_shared<org::LatchBlock>("cs.dclf.shadow.latch", kShadowLatchBytes, host->FrameSlots());
		if (DgcPreprocessEnabled()) {
			state->preprocessShadow = PreprocessStates::Create(device, host->FrameSlots(), "the shadow views");
			state->preprocessSky = PreprocessStates::Create(device, host->FrameSlots(), "Skylighting's occlusion map");
		}
		state->facePositionsAddress = device.GetBufferDeviceAddress({ state->facePositions->GetAPIResource().GetHandle(), 0 });
		state->constantsAddress = device.GetBufferDeviceAddress({ state->constants->GetAPIResource().GetHandle(), 0 });
		state->recordsAddress = device.GetBufferDeviceAddress({ state->records->GetAPIResource().GetHandle(), 0 });
		if (!state->constantsAddress || !state->recordsAddress) {
			shadowSetupFailed = true;
			return ShadowNotReady(2, "no device address for the shadow buffers");
		}
		shadow = state;
		host->AddExtension(kShadowExtensionId, [state] { return std::make_unique<ShadowExtension>(state); });
		logger::info("[DCLF] shadow view graph resources created");
		return true;
	}

	bool IndirectDraws::Impl::ImportShadowDepth(std::uint32_t a_index, std::uint32_t a_target)
	{
		auto* renderer = globals::game::renderer;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!renderer || !host || !shadow || a_index >= kShadowDepthTargets)
			return false;
		const auto& data = renderer->GetDepthStencilData().depthStencils[a_target];
		if (!data.texture || !data.views[0])
			return ShadowNotReady(3, "the shadow map has no texture");
		if (shadow->depth[a_index] && shadow->depthTexture[a_index] == data.texture)
			return true;
		DxvkOrgInteropResourceInfo info{};
		if (!RenderGraphRuntime::Get().DescribeResource(data.texture, info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
			return ShadowNotReady(4, "the shadow map cannot be described");
		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
		data.views[0]->GetDesc(&dsvDesc);
		org::TextureDescription desc{};
		desc.format = rhi::helpers::ToRHI(dsvDesc.Format);
		desc.channels = 1;
		desc.hasDSV = true;
		desc.dsvFormat = desc.format;
		static constexpr const char* kNames[kShadowDepthTargets] = { "DCLF shadow maps (ESRAM)", "DCLF shadow maps", "DCLF volumetric shadow maps (ESRAM)",
			"DCLF Skylighting occlusion map" };
		auto imported = ImportImage(host->GetDesc().device, info.image, desc, kNames[a_index]);
		if (!imported)
			return ShadowNotReady(5, "the shadow map could not be imported (not in the general layout?)");
		shadow->depth[a_index] = std::move(imported);
		shadow->depthTexture[a_index] = data.texture;
		shadow->depthLayers[a_index] = info.image.arrayLayers;
		// A new resource for the passes to bind: the graph is rebuilt on the next epoch.
		host->AddExtension(kShadowExtensionId, [state = shadow] { return std::make_unique<ShadowExtension>(state); });
		logger::info("[DCLF] shadow map {} imported: {}x{}, {} slices, format {}", a_target, info.image.extent.width, info.image.extent.height,
			info.image.arrayLayers, static_cast<int>(dsvDesc.Format));
		return true;
	}

	bool IndirectDraws::ShadowsEnabled()
	{
		return Toggles::Get().Active().shadows;
	}

	void IndirectDraws::BeginShadowFrame(const RE::NiPoint3& a_eye)
	{
		impl->shadowRefEye = a_eye;
		impl->pendingViews.clear();
	}

	void IndirectDraws::ExecuteShadowView(std::uint32_t a_viewId, std::uint32_t a_renderMode)
	{
		// The hook's half: capture the view - where the engine has just drawn, and the constants it drew
		// with - for the frame's single epoch (ExecuteShadowFrame). Nothing is drawn here.
		if (!ShadowsEnabled() || failed)
			return;
		ScopedPerfEvent event("CS DCLF: shadow view capture");
		const auto start = std::chrono::steady_clock::now();
		++shadowStats.views;
		auto& pipelines = DrawPipelines::Get();
		auto* utility = globals::game::utilityShader;
		auto notReady = [&](ShadowNotReady a_reason) {
			++shadowStats.notReady;
			++shadowStats.notReadyReasons[static_cast<std::size_t>(a_reason)];
		};
		if (!pipelines.Enabled() || !utility || !impl->SetupShadow())
			return notReady(ShadowNotReady::Setup);
		// The tables are not read here: under CS_DCLF_ASYNC the scene walk is still writing them while the
		// engine draws the shadow maps. ExecuteShadowFrame checks them after the join.
		const auto* shadowView = ShadowViews::Get().At(a_viewId);
		if (!shadowView)
			return notReady(ShadowNotReady::Tables);
		if (a_renderMode < PassCapture::kFirstShadowMode || a_renderMode >= PassCapture::kFirstShadowMode + kShadowModeCount)
			return notReady(ShadowNotReady::Tables);
		// A focus shadow holds one actor's casters, not the scene's: it stays native until S4 decides its set.
		if (shadowView->focus) {
			++shadowStats.focusSkipped;
			return;
		}
		// Where the engine has just drawn: the target and slice come from the renderer's state, because the
		// descriptor's own fields are filled only when the draw allocates them (engine notes: shadow maps).
		auto& shadowState = globals::game::shadowState->GetRuntimeData();
		const std::uint32_t target = shadowState.depthStencil;
		const std::uint32_t slice = shadowState.depthStencilSlice;
		const std::uint32_t targetIndex = target == RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM                     ? 0u :
		                                  target == RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS                           ? 1u :
		                                  target == RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM ? 2u :
		                                                                                                                     ~0u;
		// The volumetric lighting copy holds only the volumetric-only casters (batch group 15, which is all the
		// engine's flag-0x100 draw of the view renders): the view draws those alone (casterClass 1). Without the
		// hook that withholds their passes (PassCapture::VolumetricClaimsAvailable) it is left to the engine whole.
		const bool volumetricCopy = target == RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM;
		if (volumetricCopy && !PassCapture::VolumetricClaimsAvailable()) {
			++shadowStats.volumetricSkipped;
			return;
		}
		if (targetIndex == ~0u || !impl->ImportShadowDepth(targetIndex, target)) {
			// Which target, once per target: anything here is a view the design has not met.
			if (target < 32 && !((impl->shadowLoggedTargets >> target) & 1)) {
				impl->shadowLoggedTargets |= 1u << target;
				logger::info("[DCLF] shadow view {} ({}, light {} descriptor {}, mode {:#x}) draws into depth target {} slice {}; it stays native",
					a_viewId, ShadowViews::KindName(shadowView->kind), shadowView->lightIndex, shadowView->descriptor, a_renderMode, target, slice);
			}
			return notReady(ShadowNotReady::Depth);
		}
		if (impl->pendingViews.size() >= kSkySlot)
			return notReady(ShadowNotReady::Capacity);
		// The rasterizer state the engine draws this view with: its table entry for the renderer's modes, read
		// now, while the view is drawn - Community Shaders' ShadowmapCascadeRasterizerFix swaps per-cascade
		// copies with their own depth bias into that table for exactly this window, and the volumetric copy
		// draws with culling off. DCLF's pipelines for the view are built with it.
		std::uint32_t rasterState = 0;
		{
			const std::uint32_t fill = shadowState.rasterStateFillMode, cull = shadowState.rasterStateCullMode;
			const std::uint32_t bias = shadowState.rasterStateDepthBiasMode, scissor = shadowState.rasterStateScissorMode;
			if (fill < 2 && cull < 3 && bias < 12 && scissor < 2)
				if (auto* engineState = EngineRasterStates()[fill][cull][bias][scissor]) {
					D3D11_RASTERIZER_DESC desc{};
					engineState->GetDesc(&desc);
					rasterState = pipelines.ShadowRasterStateId(desc, a_renderMode);
				}
		}
		if (rasterState == 0)
			return notReady(ShadowNotReady::Pipelines);

		auto& view = impl->pendingViews.emplace_back();
		view.viewId = a_viewId;
		view.renderMode = a_renderMode;
		view.modeIndex = a_renderMode - PassCapture::kFirstShadowMode;
		view.targetIndex = targetIndex;
		view.slice = slice;
		view.rasterState = rasterState;
		view.casterClass = volumetricCopy ? 1u : 0u;
		view.sunView = shadowView->kind == ShadowViews::Kind::Directional;
		if (auto* dsv = globals::game::renderer->GetDepthStencilData().depthStencils[target].views[0]) {
			D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
			dsv->GetDesc(&dsvDesc);
			view.dsvFormat = dsvDesc.Format;
		}
		view.x = static_cast<std::uint32_t>(std::max(0.0f, shadowState.viewPort.TopLeftX));
		view.y = static_cast<std::uint32_t>(std::max(0.0f, shadowState.viewPort.TopLeftY));
		view.width = static_cast<std::uint32_t>(shadowState.viewPort.Width);
		view.height = static_cast<std::uint32_t>(shadowState.viewPort.Height);
		view.minDepth = shadowState.viewPort.MinDepth;
		view.maxDepth = shadowState.viewPort.MaxDepth;
		view.eye = shadowState.posAdjust.getEye();
		// The caster volume the engine culled this view's casters against: BSShadowDirectionalLight::UpdateCamera
		// builds it from the main camera frustum's corners and the light direction into the descriptor's culling
		// process, and the accumulation's cull (FUN_1414f0920) tests it on top of the shadow camera's frustum. It
		// is much tighter than the orthographic box: without it DCLF drew into the far cascade the casters of a
		// whole slice's worth of terrain that no visible receiver can see a shadow from.
		if (auto& lightData = const_cast<RE::BSShadowLight*>(shadowView->light)->GetRuntimeData(); shadowView->descriptor < lightData.shadowmapDescriptors.size()) {
			const auto* process = lightData.shadowmapDescriptors[shadowView->descriptor].cullingProcess;
			if (process && process->doCustomCullPlanes) {
				const auto& planes = process->customCullPlanes;
				for (std::uint32_t p = 0; p < RE::NiFrustumPlanes::Planes::kTotal; ++p) {
					view.cullPlanes[p][0] = planes.cullingPlanes[p].normal.x;
					view.cullPlanes[p][1] = planes.cullingPlanes[p].normal.y;
					view.cullPlanes[p][2] = planes.cullingPlanes[p].normal.z;
					view.cullPlanes[p][3] = planes.cullingPlanes[p].constant;
				}
				view.cullPlaneMask = planes.activePlanes.underlying() & 0x3Fu;
			}
		}
		// The view's PerTechnique block (b0): HighDetailRange (LOD landscape only; zero until owned),
		// ParabolaParam from the engine's two globals as its SetupTechnique reads them, and the eye delta
		// from the frame's reference eye to this view's.
		{
			static const REL::Relocation<float*> parabolaRadius{ REL::Offset(0x2035df8) };
			static const REL::Relocation<float*> parabolaSide{ REL::Offset(0x2035dfc) };
			const float radius = *parabolaRadius.get();
			view.viewBlock[4] = radius != 0.0f ? 1.0f / radius : 0.0f;
			view.viewBlock[5] = *parabolaSide.get();
			// c2 (DCLFEyeDelta) is no longer read: the records are absolute and Utility.hlsl subtracts the view's
			// own CameraPosAdjust. Left zero.
			view.viewBlock[8] = view.viewBlock[9] = view.viewBlock[10] = 0.0f;
		}
		// VS_PerFrame (b12) as the engine wrote it for this view, taken now because the next view rewrites
		// it: from the mirror, or from Community Shaders' copy of the same buffer (Globals: CacheFramebuffer)
		// until the mirror has seen a write.
		auto& mirror = ConstantMirror::Get();
		if (auto* perFrame = *globals::game::perFrame.get()) {
			mirror.Watch(perFrame);
			const auto contents = mirror.Contents(perFrame);
			if (contents.size() >= 48 * sizeof(float)) {
				view.perFrameBytes = static_cast<std::uint32_t>(std::min<std::size_t>(contents.size(), view.perFrame.size()));
				std::memcpy(view.perFrame.data(), contents.data(), view.perFrameBytes);
			}
		}
		if (!view.perFrameBytes) {
			const auto& cached = globals::game::frameBufferCached.data;
			static_assert(sizeof(cached) >= 48 * sizeof(float) && sizeof(cached) <= 1024);
			view.perFrameBytes = sizeof(cached);
			std::memcpy(view.perFrame.data(), &cached, sizeof(cached));
		}
		std::memcpy(view.viewProj.data(), reinterpret_cast<const float*>(view.perFrame.data()) + 32, sizeof(float) * 16);
		view.hasViewProj = true;
		shadowStats.captureMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	}

	void IndirectDraws::CaptureSkyOcclusion()
	{
		// The hook's half, as ExecuteShadowView: where the engine's RenderMask has just drawn Skylighting's map (the
		// clear, and whatever SetupMask registered), with which camera and state. Taken whether or not DCLF draws the
		// map this frame: the state and format are what next frame's build prepares the pipelines for.
		if (!ShadowsEnabled() || failed || !SceneStore::SkyOcclusionEnabled() || !impl->SetupShadow())
			return;
		auto& shadowState = globals::game::shadowState->GetRuntimeData();
		const std::uint32_t target = shadowState.depthStencil;
		// The renderer's state at this hook is what the view's last pass left, or whatever came before when nothing
		// drew; the Utility shader sets the cull mode per pass (engine notes, shadow maps: 0 for a two-sided
		// property, 1 otherwise). So the view's state is back-face culling at the renderer's fill, bias and scissor
		// modes, and a two-sided occluder's key draws without culling, as a two-sided caster's does.
		std::uint32_t rasterState = 0;
		{
			const std::uint32_t fill = shadowState.rasterStateFillMode, cull = 1;
			const std::uint32_t bias = shadowState.rasterStateDepthBiasMode, scissor = shadowState.rasterStateScissorMode;
			if (fill < 2 && bias < 12 && scissor < 2)
				if (auto* engineState = EngineRasterStates()[fill][cull][bias][scissor]) {
					D3D11_RASTERIZER_DESC desc{};
					engineState->GetDesc(&desc);
					rasterState = DrawPipelines::Get().ShadowRasterStateId(desc, kSkyRenderMode);
				}
		}
		if (!rasterState || !impl->ImportShadowDepth(kSkyDepthTarget, target))
			return;
		auto& view = impl->skyView;
		view = {};
		view.viewId = ~0u;
		view.renderMode = kSkyRenderMode;
		view.modeIndex = kSkyMode;
		view.targetIndex = kSkyDepthTarget;
		view.slice = shadowState.depthStencilSlice;
		view.rasterState = rasterState;
		if (auto* dsv = globals::game::renderer->GetDepthStencilData().depthStencils[target].views[0]) {
			D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
			dsv->GetDesc(&dsvDesc);
			view.dsvFormat = dsvDesc.Format;
		}
		view.x = static_cast<std::uint32_t>(std::max(0.0f, shadowState.viewPort.TopLeftX));
		view.y = static_cast<std::uint32_t>(std::max(0.0f, shadowState.viewPort.TopLeftY));
		view.width = static_cast<std::uint32_t>(shadowState.viewPort.Width);
		view.height = static_cast<std::uint32_t>(shadowState.viewPort.Height);
		view.minDepth = shadowState.viewPort.MinDepth;
		view.maxDepth = shadowState.viewPort.MaxDepth;
		view.eye = shadowState.posAdjust.getEye();
		// VS_PerFrame (b12) as the engine wrote it for this view: its view-projection is what the draws use.
		auto& mirror = ConstantMirror::Get();
		if (auto* perFrame = *globals::game::perFrame.get()) {
			mirror.Watch(perFrame);
			const auto contents = mirror.Contents(perFrame);
			if (contents.size() >= 48 * sizeof(float)) {
				view.perFrameBytes = static_cast<std::uint32_t>(std::min<std::size_t>(contents.size(), view.perFrame.size()));
				std::memcpy(view.perFrame.data(), contents.data(), view.perFrameBytes);
			}
		}
		if (!view.perFrameBytes) {
			const auto& cached = globals::game::frameBufferCached.data;
			view.perFrameBytes = sizeof(cached);
			std::memcpy(view.perFrame.data(), &cached, sizeof(cached));
		}
		std::memcpy(view.viewProj.data(), reinterpret_cast<const float*>(view.perFrame.data()) + 32, sizeof(float) * 16);
		view.hasViewProj = true;
		impl->skyRasterState = rasterState;
		impl->skyDsvFormat = view.dsvFormat;
		impl->skyCapturedFrame = SceneStore::Get().GetFrame();
	}

	bool IndirectDraws::SkyOcclusionReady() const
	{
		// This frame's shadow commit uploaded every occluder (none left out for a pipeline or a texture not yet
		// resolved), and the map's target is imported.
		return !failed && ShadowsEnabled() && SceneStore::SkyOcclusionEnabled() && impl->shadow && impl->skyRasterState &&
		       impl->skyCommittedFrame == SceneStore::Get().GetFrame() && impl->skySkipped == 0 && impl->shadow->depth[kSkyDepthTarget];
	}

	bool IndirectDraws::ExecuteSkyOcclusion(bool a_diagnose)
	{
		ZoneScopedN("CS.DCLF.ExecuteSkyOcclusion");
		const auto start = std::chrono::steady_clock::now();
		auto& store = SceneStore::Get();
		const std::uint32_t frameNumber = store.GetFrame();
		if (!SkyOcclusionReady() || impl->skyCapturedFrame != frameNumber || impl->skyView.rasterState != impl->skyRasterState) {
			++shadowStats.skyNotReady;
			return false;
		}
		const auto indirect = GetShadowIndirectState();
		if (!indirect.valid) {
			++shadowStats.skyNotReady;
			return false;
		}
		ScopedPerfEvent event("CS DCLF: Skylighting occlusion (CPU)");
		auto resources = impl->shadow;
		auto& payload = impl->shadowPayload;
		const auto& view = impl->skyView;
		const auto inputCount = static_cast<std::uint32_t>(payload.ModeInputs(kSkyMode));
		// [TEMP] CS_DCLF_SKYLIGHT_PARITY: the occluders DCLF's frustum test keeps that the engine did not register this map.
		if (a_diagnose) {
			const auto engineList = SunAccumulation::Get().TakeSkyRegistrations();
			const ankerl::unordered_dense::set<const RE::BSGeometry*> engine(engineList.begin(), engineList.end());
			float m[16];
			FoldEyeIntoViewProj(view.viewProj, view.eye, m);
			const auto& tables = store.GetTables();
			std::vector<std::pair<float, std::string>> extra;
			std::uint32_t kept = 0, both = 0;
			skyFootprints.clear();
			for (const auto& input : payload.Flat(kSkyMode)) {
				const float* c = input.boundCentre;
				const float r = input.boundRadius;
				bool outside = false;
				// Rows of the (row-major) view-projection: the clip-space planes x, y and z against w.
				for (int p = 0; p < 6 && !outside; ++p) {
					const int axis = p / 2;
					const float sign = (p & 1) ? -1.0f : 1.0f;
					float plane[4];
					for (int k = 0; k < 4; ++k)
						plane[k] = m[12 + k] + sign * m[axis * 4 + k];
					if (axis == 2 && !(p & 1))
						for (int k = 0; k < 4; ++k)
							plane[k] = m[8 + k];  // z >= 0
					const float length = std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
					outside = length > 0 && (plane[0] * c[0] + plane[1] * c[1] + plane[2] * c[2] + plane[3]) / length < -r;
				}
				if (outside)
					continue;
				++kept;
				const auto* geometry = input.objectIndex < tables.objectGeometry.size() ? tables.objectGeometry[input.objectIndex] : nullptr;
				// Its bound's footprint in the map's texels (the corners of its box, projected; y down).
				{
					float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
					for (std::uint32_t corner = 0; corner < 8; ++corner) {
						const float p[3] = { c[0] + ((corner & 1) ? r : -r), c[1] + ((corner & 2) ? r : -r), c[2] + ((corner & 4) ? r : -r) };
						const float x = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
						const float y = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
						const float w = m[12] * p[0] + m[13] * p[1] + m[14] * p[2] + m[15];
						const float nx = w != 0 ? x / w : x, ny = w != 0 ? y / w : y;
						x0 = std::min(x0, nx), x1 = std::max(x1, nx), y0 = std::min(y0, ny), y1 = std::max(y1, ny);
					}
					auto texel = [&](float a_ndc, float a_size, bool a_flip) {
						const float t = a_flip ? (1.0f - a_ndc) * 0.5f : (a_ndc + 1.0f) * 0.5f;
						return static_cast<std::int32_t>(std::clamp(t * a_size, -1.0f, a_size + 1.0f));
					};
					SkyFootprint footprint;
					footprint.x0 = texel(x0, float(view.width), false) + std::int32_t(view.x);
					footprint.x1 = texel(x1, float(view.width), false) + std::int32_t(view.x);
					footprint.y0 = texel(y1, float(view.height), true) + std::int32_t(view.y);
					footprint.y1 = texel(y0, float(view.height), true) + std::int32_t(view.y);
					if (geometry) {
						auto* g = const_cast<RE::BSGeometry*>(geometry);
						footprint.label = fmt::format("'{}' ({}) r {:.0f} technique {:#x} layout {:#x} flags {:#x} skin {}", g->name.c_str() ? g->name.c_str() : "?",
							g->GetRTTI() ? g->GetRTTI()->name : "?", r, tables.skyTechnique[input.objectIndex],
							VertexLayoutOf(tables.geometries[tables.objects[input.objectIndex].geometryIndex].vertexDesc), tables.objects[input.objectIndex].flags,
							tables.skinPartitions[input.objectIndex]);
					}
					skyFootprints.push_back(std::move(footprint));
				}
				if (engine.contains(geometry)) {
					++both;
					continue;
				}
				if (!geometry)
					continue;
				auto* g = const_cast<RE::BSGeometry*>(geometry);
				std::string chain;
				int depth = 0;
				for (auto* node = g->parent; node && depth < 5; node = node->parent, ++depth)
					chain += fmt::format("/{}{}", node->name.c_str() && *node->name.c_str() ? node->name.c_str() : (node->GetRTTI() ? node->GetRTTI()->name : "?"),
						(node->GetFlags().underlying() & 1) ? "(culled)" : "");
				const auto* reference = g->GetUserData();
				extra.emplace_back(r, fmt::format("'{}' r {:.0f} at ({:.0f} {:.0f} {:.0f}) technique {:#x} ref {:X} {} flags {:#x}{}", g->name.c_str() ? g->name.c_str() : "?", r, c[0], c[1], c[2],
					tables.skyTechnique[input.objectIndex], reference ? reference->GetFormID() : 0, reference ? static_cast<int>(reference->GetFormType()) : -1,
					g->GetFlags().underlying(), chain));
			}
			std::sort(extra.begin(), extra.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
			logger::info("[DCLF][TEMP] Skylighting occlusion diagnosis: {} inputs, {} in DCLF's frustum, {} of them registered by the engine, {} not; engine registered {}",
				inputCount, kept, both, extra.size(), engine.size());
			for (std::size_t i = 0; i < extra.size() && i < 20; ++i)
				logger::info("[DCLF][TEMP]   DCLF only: {}", extra[i].second);
		}
		const bool ok = RenderGraphRuntime::Get().ExecuteEpoch(RenderGraphRuntime::Segment::SkyOcclusion, [&](org::RenderGraph&) {
			resources->skyFrame.store(nullptr, std::memory_order_release);
			CommitUploads uploads(impl->commitStagedPool);
			// The view slot's blocks, its copy of the frame's binding records naming them, and its count zeroed. The
			// occluders, records, objects and geometries were uploaded by this frame's shadow commit.
			const std::uint64_t base = resources->constantsAddress;
			const std::uint64_t viewBlockOffset = std::uint64_t(kSkySlot) * kShadowViewSlotBytes;
			const std::uint64_t perFrameOffset = viewBlockOffset + kShadowPerFrameOffset;
			uploads(resources->constants, view.viewBlock, sizeof(view.viewBlock), viewBlockOffset);
			uploads(resources->constants, view.perFrame.data(), view.perFrameBytes, perFrameOffset);
			auto& slotRecords = impl->shadowSlotRecords;
			slotRecords = payload.records;
			for (auto& record : slotRecords) {
				record.vertexConstants[0] = base + viewBlockOffset;
				record.pixelConstants[0] = base + viewBlockOffset;
				record.vertexConstants[kPerFrameVertexRegister] = base + perFrameOffset;
				record.pixelConstants[kPerFrameVertexRegister] = base + perFrameOffset;
			}
			const std::uint64_t recordsOffset = std::uint64_t(kSkySlot) * kShadowRecordCapacity * sizeof(DrawBindings);
			uploads(resources->records, slotRecords.data(), slotRecords.size() * sizeof(DrawBindings), recordsOffset);
			static const std::uint32_t zero[kCountWords] = {};
			uploads(resources->count[kSkySlot], zero, sizeof(zero), 0);
			// Its latch: frustum culling alone, near plane included as the rasterizer clips, every input drawn.
			const std::uint32_t latchSlot = RenderGraphRuntime::Get().Host()->CurrentFrameSlot();
			BuildDrawsLatch latch{};
			latch.dispatch[0] = (inputCount + 63) / 64;
			latch.dispatch[1] = 1;
			latch.dispatch[2] = 1;
			latch.drawCount = inputCount;
			latch.cullFlags = 1u;
			latch.visibilityStamp = frameNumber & 0x0FFFFFFFu;
			FoldEyeIntoViewProj(view.viewProj, view.eye, latch.viewProj);
			const std::uint32_t mapRowOffset = kShadowPipelineMapOffset + (view.rasterState - 1) * kShadowPipelineMapRowBytes;
			latch.pipelineMapOffset = static_cast<std::uint32_t>(resources->latch->Offset(latchSlot)) + mapRowOffset;
			const auto& row = store.GetLookups().shadowMapRows[view.rasterState];
			if (!row.empty())
				resources->latch->Write(latchSlot, mapRowOffset, std::as_bytes(std::span(row.data(), std::min<std::size_t>(row.size(), kMaxShadowSlots))));
			resources->latch->WriteValue(latchSlot, kSkySlot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), latch);
			auto frame = std::make_shared<ShadowFrame>();
			frame->resourceHeap = org::runtime::GetActiveSRVDescriptorHeap().GetHandle();
			frame->samplerHeap = org::runtime::GetActiveSamplerDescriptorHeap().GetHandle();
			frame->indirect = indirect;
			ShadowFrameView out{};
			out.slot = kSkySlot;
			out.modeIndex = kSkyMode;
			const auto& previousShape = resources->skyPublished;
			out.capacity = GrowCapacity(previousShape && !previousShape->views.empty() ? previousShape->views.front().capacity : 0u, inputCount, kMaxDraws);
			out.x = view.x;
			out.y = view.y;
			out.width = view.width;
			out.height = view.height;
			out.minDepth = view.minDepth;
			out.maxDepth = view.maxDepth;
			out.target = kSkyDepthTarget;
			out.slice = view.slice;
			out.recordsAddress = resources->recordsAddress + recordsOffset;
			frame->views.push_back(out);
			if (previousShape && previousShape->SameShape(*frame)) {
				resources->skyFrame.store(previousShape, std::memory_order_release);
			} else {
				frame->generation = ++resources->shapeGenerations;
				resources->skyPublished = frame;
				resources->skyFrame.store(std::move(frame), std::memory_order_release);
			}
		});
		shadowStats.skyMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		if (!ok) {
			++shadowStats.skyNotReady;
			return false;
		}
		++shadowStats.skyDrawn;
		shadowStats.skyInputs = inputCount;
		return true;
	}

	void IndirectDraws::ExecuteShadowFrame()
	{
		ZoneScopedN("CS.DCLF.ExecuteShadowFrame");
		auto& pending = impl->pendingViews;
		if (!ShadowsEnabled() || failed || pending.empty()) {
			pending.clear();
			impl->DropShadowJob(stats);
			return;
		}
		ScopedPerfEvent event("CS DCLF: shadow views (CPU)");
		const auto start = std::chrono::steady_clock::now();
		auto notReady = [&](ShadowNotReady a_reason) {
			shadowStats.notReady += static_cast<std::uint32_t>(pending.size());
			shadowStats.notReadyReasons[static_cast<std::size_t>(a_reason)] += static_cast<std::uint32_t>(pending.size());
			pending.clear();
			impl->DropShadowJob(stats);
		};
		auto& pipelines = DrawPipelines::Get();
		auto* utility = globals::game::utilityShader;
		if (!pipelines.Enabled() || !utility || !impl->shadow)
			return notReady(ShadowNotReady::Setup);
		const auto indirect = GetShadowIndirectState();
		if (!indirect.valid) {
			impl->ShadowNotReady(6, "no shadow pipeline in the set yet");
			return notReady(ShadowNotReady::Pipelines);
		}
		auto& store = SceneStore::Get();
		const auto& tables = store.GetTables();
		if (tables.objects.empty() || tables.shadowTechnique.size() != tables.objects.size())
			return notReady(ShadowNotReady::Tables);
		const std::uint32_t frameNumber = store.GetFrame();
		auto resources = impl->shadow;
		auto& textures = GpuTextures::Get();
		std::array<bool, kShadowModeCount> modeUsed{};
		std::array<std::uint32_t, kShadowModeCount> modeRasterStates{};
		for (const auto& view : pending) {
			modeUsed[view.modeIndex] = true;
			modeRasterStates[view.modeIndex] |= 1u << (view.rasterState + (view.casterClass ? 16u : 0u));
		}
		// Skylighting's occlusion map is drawn later in the frame, by its own epoch, from this build: its occluders
		// under the state its view drew with last (known once the engine's own draw of the map has been captured).
		if (SceneStore::SkyOcclusionEnabled() && impl->skyRasterState && impl->skyDsvFormat != DXGI_FORMAT_UNKNOWN) {
			modeUsed[kSkyMode] = true;
			modeRasterStates[kSkyMode] = 1u << impl->skyRasterState;
		}
		// One pipeline set per shadow map format: every target the engine has is D16 (engine notes), and a
		// view whose target differed would rebuild the set on every epoch, so the first view's is taken.
		const DXGI_FORMAT dsvFormat = pending.front().dsvFormat;
		double prepareMs = 0.0, inputsMs = 0.0, blocksMs = 0.0, bodyMs = 0.0;

		ShadowInputs in = impl->PrepareShadowInputs(store, *resources, modeUsed, modeRasterStates);
		impl->shadowJob.modes = modeUsed;
		impl->shadowJob.rasterStates = modeRasterStates;
		impl->shadowJob.modesKnown = true;
		impl->shadowJob.views = static_cast<std::uint32_t>(pending.size());
		auto& payload = impl->shadowPayload;
		auto& async = stats.async[kAsyncShadow];
		bool usedWorkerBuild = false;

		const bool ok = RenderGraphRuntime::Get().ExecuteEpoch(RenderGraphRuntime::Segment::ShadowView, [&](org::RenderGraph&) {
			ZoneScopedN("CS.DCLF.ShadowInputs");
			struct BodyTimer
			{
				std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
				double& out;
				~BodyTimer() { out = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); }
			} bodyTimer{ {}, bodyMs };
			bodyTimer.start = std::chrono::steady_clock::now();

			// The worker's build if one was kicked and it was built for exactly these inputs, else the build
			// here. Joined before the lookups are refreshed: the worker reads them until it is done.
			bool useAsync = false;
			TracyCZoneN(shadowPrepareZone, "CS.DCLF.ShadowInputs.Prepare", true);
			const auto prepareStart = std::chrono::steady_clock::now();
			textures.BeginFrame(frameNumber);
			auto& job = impl->shadowJob;
			auto& worker = AsyncWorker::Get();
			AsyncWorker::WaitResult joined = AsyncWorker::WaitResult::None;
			if (job.handle) {
				joined = worker.Wait(job.handle, AsyncWaitBudget());
				if (joined == AsyncWorker::WaitResult::Late)
					worker.Cancel(job.handle);
			}
			auto& lookups = store.MutableLookups();
			RefreshMaterialLookups(tables, frameNumber, store.GetProjectedTextures(), lookups);
			RefreshShadowLookups(tables, modeUsed, modeRasterStates, dsvFormat, impl->skyDsvFormat, lookups);
			in.lookupGeneration = lookups.generation;
			if (job.handle) {
				switch (joined) {
				case AsyncWorker::WaitResult::Done:
					if (SameShadowInputs(job.inputs, in)) {
						useAsync = true;
					} else {
						++async.stale;
						if (job.loggedStale++ < 4) {
							const auto& k = job.inputs;
							logger::info("[DCLF] async shadow: the job's inputs are stale (frame {} vs {}, render flags {:#x} vs {:#x}, modes {}{}{} vs {}{}{}, shared data {}, feature data {}, tables {} vs {}, lookups {} vs {}, resources {})",
								k.frameNumber, in.frameNumber, k.renderFlags, in.renderFlags, int(k.modeUsed[0]), int(k.modeUsed[1]), int(k.modeUsed[2]),
								int(in.modeUsed[0]), int(in.modeUsed[1]), int(in.modeUsed[2]), k.sharedData == in.sharedData ? "same" : "differs",
								k.featureData == in.featureData ? "same" : "differs", k.tablesGeneration, in.tablesGeneration, k.lookupGeneration,
								in.lookupGeneration, k.addresses == in.addresses ? "same" : "changed");
						}
					}
					break;
				case AsyncWorker::WaitResult::Late:
					++async.late;
					break;
				case AsyncWorker::WaitResult::Failed:
					++async.failed;
					break;
				default:
					++async.cancelled;
					break;
				}
				job.handle = {};
			}
			usedWorkerBuild = useAsync;
			if (useAsync) {
				++async.used;
				if (AsyncModeSetting() == AsyncMode::Probe) {
					BuildShadowPayload(job.inputs, tables, lookups, impl->shadowProbePayload);
					++async.probeCompared;
					std::string difference;
					if (!SameShadowPayload(payload, impl->shadowProbePayload, difference) && async.probeDiffer++ == 0)
						logger::warn("[DCLF] async shadow probe: the worker's build differs from the inline one: {}", difference);
				}
			} else {
				++async.builtInline;
				BuildShadowPayload(in, tables, lookups, payload, impl->ShadowObjects(), impl->ShadowBones(), impl->ShadowKeptState(), impl->ShadowGeometries());
			}
			prepareMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepareStart).count();
			TracyCZoneEnd(shadowPrepareZone);

			// [TEMP] CS_DCLF_CASCADE_PROBE: per view, the casters DCLF's frustum test keeps (a CPU replica of
			// BuildDrawsCS Culled against the view's latch matrix) against the ones the engine registered into
			// that view's batch renderers this frame, and what the difference is made of.
			if (PassCapture::CascadeProbeEnabled()) {
				auto registrations = PassCapture::Get().TakeShadowRegistrations();
				static std::uint32_t probeFrames = 0;
				if ((probeFrames++ % 300) == 150) {
					const auto& shadowViews = ShadowViews::Get();
					ankerl::unordered_dense::map<std::uint32_t, ankerl::unordered_dense::set<const RE::BSGeometry*>> engineByView;
					ankerl::unordered_dense::set<const RE::BSGeometry*> withheldSet;
					std::uint32_t unattributed = 0;
					for (const auto& r : registrations) {
						const auto id = shadowViews.ViewOfBatch(r.batch);
						if (id == ~0u) {
							++unattributed;
							continue;
						}
						engineByView[id].insert(r.geometry);
						if (r.withheld)
							withheldSet.insert(r.geometry);
					}
					logger::info("[DCLF] cascade probe: {} shadow registrations this frame ({} not attributed to a view)", registrations.size(), unattributed);
					ankerl::unordered_dense::set<std::uint32_t> seenViews;
					for (const auto& view : pending) {
						if (!seenViews.insert(view.viewId).second)
							continue;
						float m[16];
						FoldEyeIntoViewProj(view.viewProj, view.eye, m);
						struct Clip { float x, y, z, w; };
						auto project = [&](float px, float py, float pz) {
							return Clip{ m[0] * px + m[1] * py + m[2] * pz + m[3], m[4] * px + m[5] * py + m[6] * pz + m[7],
								m[8] * px + m[9] * py + m[10] * pz + m[11], m[12] * px + m[13] * py + m[14] * pz + m[15] };
						};
						// 0 kept, 1 rejected by x/y, 2 in front of near, 3 beyond far, 4 kept because a corner has w <= 0,
						// 5 outside the engine's caster volume (the latch's planes), as BuildDrawsCS Culled now does
						const bool noNear = view.renderMode == 0xE;
						auto cull = [&](const DrawInput& a_in) -> int {
							for (std::uint32_t p = 0; p < 6; ++p) {
								if ((view.cullPlaneMask & (1u << p)) &&
									view.cullPlanes[p][0] * a_in.boundCentre[0] + view.cullPlanes[p][1] * a_in.boundCentre[1] + view.cullPlanes[p][2] * a_in.boundCentre[2] - view.cullPlanes[p][3] < -a_in.boundRadius)
									return 5;
							}
							bool nx = true, px = true, ny = true, py = true, zn = !noNear, zf = true;
							for (std::uint32_t c = 0; c < 8; ++c) {
								const float r = a_in.boundRadius;
								const Clip clip = project(a_in.boundCentre[0] + ((c & 1) ? r : -r), a_in.boundCentre[1] + ((c & 2) ? r : -r), a_in.boundCentre[2] + ((c & 4) ? r : -r));
								if (clip.w <= 1e-4f)
									return 4;
								nx &= clip.x < -clip.w;
								px &= clip.x > clip.w;
								ny &= clip.y < -clip.w;
								py &= clip.y > clip.w;
								zn &= clip.z < 0.0f;
								zf &= clip.z > clip.w;
							}
							return (nx || px || ny || py) ? 1 : zn ? 2 : zf ? 3 : 0;
						};
						const auto& engine = engineByView[view.viewId];
						// The engine's cull volumes for this descriptor: its clipPlanes (the cascade's split slab) and
						// the culling process's custom planes (+0xAC, 6 NiPlanes, active mask at +0x10C, used when
						// +0x11F is set; UpdateCamera builds them from the main camera frustum's corners).
						const RE::NiPlane* clipPlanes = nullptr;
						const RE::NiCamera* camera = nullptr;
						std::uint32_t clipMask = 0;
						const RE::NiPlane* customPlanes = nullptr;
						std::uint32_t customMask = 0, customFlag = 0;
						if (const auto* sv = shadowViews.At(view.viewId); sv && sv->light && !sv->focus) {
							auto& lightData = const_cast<RE::BSShadowLight*>(sv->light)->GetRuntimeData();
							if (sv->descriptor < lightData.shadowmapDescriptors.size()) {
								const auto& descriptor = lightData.shadowmapDescriptors[sv->descriptor];
								clipPlanes = descriptor.clipPlanes.cullingPlanes;
								camera = descriptor.camera.get();
								clipMask = descriptor.clipPlanes.activePlanes.underlying();
								if (const auto* process = reinterpret_cast<const std::byte*>(descriptor.cullingProcess)) {
									customPlanes = reinterpret_cast<const RE::NiPlane*>(process + 0xAC);
									customMask = *reinterpret_cast<const std::uint32_t*>(process + 0x10C);
									customFlag = *reinterpret_cast<const std::uint8_t*>(process + 0x11F);
								}
							}
						}
						// The engine camera's orthographic frustum: per set (0 both, 1 DCLF only), per bound (0 DCLF's,
						// 1 the geometry's worldBound), rejections by near, far, left/right, bottom/top.
						std::uint32_t cameraRejects[2][2][4] = {};
						auto testCamera = [&](const float* a_c, float a_r, std::uint32_t (&a_out)[4]) {
							if (!camera)
								return;
							const auto& world = camera->world;
							const auto& f = camera->GetRuntimeData2().viewFrustum;
							const float d[3] = { a_c[0] - world.translate.x, a_c[1] - world.translate.y, a_c[2] - world.translate.z };
							auto axis = [&](int k) { return world.rotate.entry[0][k] * d[0] + world.rotate.entry[1][k] * d[1] + world.rotate.entry[2][k] * d[2]; };
							const float depth = axis(0), up = axis(1), right = axis(2);
							a_out[0] += depth < f.fNear - a_r;
							a_out[1] += depth > f.fFar + a_r;
							a_out[2] += right < f.fLeft - a_r || right > f.fRight + a_r;
							a_out[3] += up < f.fBottom - a_r || up > f.fTop + a_r;
						};
						// The ancestors: per set, how many have an ancestor whose worldBound the custom planes reject,
						// one flagged hidden (NiAVObject flag bit 0), and the OR of the ancestors' and the geometry's flags.
						std::uint32_t ancestorOutside[2] = {}, ancestorHidden[2] = {}, ancestorDepthSum[2] = {};
						std::uint64_t flagsOr[2] = {}, flagsAnd[2] = { ~0ull, ~0ull };
						std::string ancestry[2];
						std::uint32_t ancestryLogged[2] = {};
						auto testAncestors = [&](const RE::BSGeometry* a_geometry, std::uint32_t a_set) {
							if (!a_geometry)
								return;
							std::uint32_t depth = 0;
							bool outside = false, hidden = false;
							std::uint64_t flags = a_geometry->GetFlags().underlying();
							std::string chain;
							for (const RE::NiAVObject* node = a_geometry->parent; node; node = node->parent, ++depth) {
								const auto& wb = node->worldBound;
								bool out = false;
								for (std::uint32_t p = 0; p < 6 && customPlanes; ++p) {
									if (!(customMask & (1u << p)))
										continue;
									const auto& plane = customPlanes[p];
									if (plane.normal.x * wb.center.x + plane.normal.y * wb.center.y + plane.normal.z * wb.center.z - plane.constant < -wb.radius)
										out = true;
								}
								outside |= out;
								const std::uint64_t nodeFlags = node->GetFlags().underlying();
								hidden |= (nodeFlags & 1) != 0;
								flags |= nodeFlags;
								if (ancestryLogged[a_set] < 3 && depth < 6)
									chain += fmt::format(" > '{}' {} flags {:#x} r {:.0f}{}", node->name.c_str() ? node->name.c_str() : "", node->GetRTTI() ? node->GetRTTI()->name : "?", nodeFlags, wb.radius, out ? " OUT" : "");
							}
							ancestorOutside[a_set] += outside;
							ancestorHidden[a_set] += hidden;
							ancestorDepthSum[a_set] += depth;
							flagsOr[a_set] |= flags;
							flagsAnd[a_set] &= flags;
							if (ancestryLogged[a_set] < 3) {
								++ancestryLogged[a_set];
								ancestry[a_set] += fmt::format(" | '{}' flags {:#x}:{}", a_geometry->name.c_str() ? a_geometry->name.c_str() : "", a_geometry->GetFlags().underlying(), chain);
							}
						};
						// The caster rule's inputs per set: kCastShadows (bit 9), the fade, the material alpha, and
						// property flag bits that differ between the sets (OR and AND).
						std::uint32_t noCast[2] = {}, faded[2] = {}, sharedProperty[2] = {};
						std::uint64_t propOr[2] = {}, propAnd[2] = { ~0ull, ~0ull };
						std::string ruleSamples[2];
						std::uint32_t ruleLogged[2] = {};
						ankerl::unordered_dense::map<const RE::BSShaderProperty*, std::uint32_t> propertyUses;
						for (const auto& input : payload.Flat(view.modeIndex))
							if (input.objectIndex < tables.objectGeometry.size())
								if (const auto* g = tables.objectGeometry[input.objectIndex])
									if (const auto* prop = g->GetGeometryRuntimeData().shaderProperty.get())
										++propertyUses[static_cast<const RE::BSShaderProperty*>(prop)];
						auto testRule = [&](const RE::BSGeometry* a_geometry, std::uint32_t a_set) {
							if (!a_geometry)
								return;
							const auto* prop = static_cast<const RE::BSShaderProperty*>(a_geometry->GetGeometryRuntimeData().shaderProperty.get());
							const auto* lighting = netimmerse_cast<const RE::BSLightingShaderProperty*>(prop);
							if (!lighting)
								return;
							const std::uint64_t flags = lighting->flags.underlying();
							noCast[a_set] += !(flags & (1ull << 9));
							const float fade = lighting->fadeNode ? const_cast<RE::BSFadeNode*>(lighting->fadeNode)->GetRuntimeData().currentFade : 1.0f;
							const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(lighting->material);
							const float alpha = material ? material->materialAlpha : 1.0f;
							faded[a_set] += fade * alpha < 1.0f;
							propOr[a_set] |= flags;
							propAnd[a_set] &= flags;
							const auto uses = propertyUses[prop];
							sharedProperty[a_set] += uses > 1;
							if (ruleLogged[a_set] < 6) {
								++ruleLogged[a_set];
								ruleSamples[a_set] += fmt::format(" | '{}' flags {:#x} fade {:.2f} alpha {:.2f} reject {} property uses {} material {}", a_geometry->name.c_str() ? a_geometry->name.c_str() : "", flags,
									fade, alpha, ShadowRejectName(ShadowCasterReject(prop, a_geometry)), uses, static_cast<const void*>(lighting->material));
							}
						};
						// [set: 0 both, 1 DCLF only][volume: 0 clip, 1 custom][sign: 0 n.c-d < -r, 1 n.c-d > r]
						std::uint32_t planeRejects[2][2][2] = {};
						auto testPlanes = [&](const RE::NiPlane* a_planes, std::uint32_t a_mask, const float* a_c, float a_r, std::uint32_t (&a_out)[2][2], std::uint32_t a_volume) {
							if (!a_planes)
								return;
							bool outA = false, outB = false;
							for (std::uint32_t p = 0; p < 6; ++p) {
								if (!(a_mask & (1u << p)))
									continue;
								const auto& plane = a_planes[p];
								const float d = plane.normal.x * a_c[0] + plane.normal.y * a_c[1] + plane.normal.z * a_c[2] - plane.constant;
								outA |= d < -a_r;
								outB |= d > a_r;
							}
							a_out[a_volume][0] += outA;
							a_out[a_volume][1] += outB;
						};
						const auto inputs = payload.Flat(view.modeIndex);
						std::uint32_t kept = 0, both = 0, dclfOnly = 0, byReason[6] = {};
						std::uint32_t onlyByRadius[5] = {};  // <64, <256, <1024, <4096, more
						std::uint32_t onlyZ[3] = {};         // centre depth: in front of near, inside, beyond far
						std::uint32_t onlyInOtherView = 0;
						double onlyDistance = 0.0, bothDistance = 0.0;
						std::string onlyNames;
						std::uint32_t named = 0;
						// The engine's candidates: the cascade cull (FUN_140e305c0) walks each full-frustum culling
						// process's objectArray, skipping hidden entries, and culls every entry recursively. A geometry
						// not under an entry is never a candidate. Mapped to the entry that covers it.
						ankerl::unordered_dense::map<const RE::BSGeometry*, const RE::NiAVObject*> candidateEntry;
						std::uint32_t candidateEntries = 0, candidateProcesses = 0, candidateEntryGeometries = 0;
						std::vector<const RE::BSCullingProcess*> fullFrustumProcesses;
						std::string processInfo;
						ankerl::unordered_dense::map<std::string, std::uint32_t> entryTypes;
						if (const auto* sv = shadowViews.At(view.viewId); sv && sv->light && !sv->focus &&
																		  const_cast<RE::BSShadowLight*>(sv->light)->GetIsDirectionalLight()) {
							auto& directional = static_cast<RE::BSShadowDirectionalLight*>(const_cast<RE::BSShadowLight*>(sv->light))->GetShadowDirectionalLightRuntimeData();
							std::function<void(const RE::NiAVObject*, const RE::NiAVObject*)> collect = [&](const RE::NiAVObject* a_object, const RE::NiAVObject* a_entry) {
								if (!a_object || (a_object->GetFlags().underlying() & 1))
									return;
								if (const auto* g = const_cast<RE::NiAVObject*>(a_object)->AsGeometry()) {
									candidateEntry.emplace(g, a_entry);
									return;
								}
								if (auto* node = const_cast<RE::NiAVObject*>(a_object)->AsNode())
									for (const auto& child : node->GetChildren())
										collect(child.get(), a_entry);
							};
							for (const auto& process : directional.fullFrustumCullingProcessArray) {
								if (!process)
									continue;
								++candidateProcesses;
								fullFrustumProcesses.push_back(process.get());
								processInfo += fmt::format(" [{} entries, cullMode {}, compound {}, portal {}, planes {:#x}, custom {} {:#x}, ignorePreprocess {}, camera {}]", process->objectArray.size(),
									static_cast<int>(process->cullMode.get()), static_cast<const void*>(process->compoundFrustum), static_cast<const void*>(process->portalGraphEntry),
									process->planes.activePlanes.underlying(), process->doCustomCullPlanes, process->customCullPlanes.activePlanes.underlying(), process->ignorePreprocess,
									static_cast<const void*>(process->camera));
								for (const auto& entry : process->objectArray) {
									++candidateEntries;
									if (entry && candidateEntries < 20000) {
										const auto* parent = entry->parent;
										const auto* grand = parent ? parent->parent : nullptr;
										++entryTypes[fmt::format("{} {:#x} user {} < {} {:#x} user {} r {:.0f} < {}", entry->GetRTTI() ? entry->GetRTTI()->name : "?", entry->GetFlags().underlying(),
											const_cast<RE::NiAVObject*>(entry.get())->GetUserData() != nullptr, parent && parent->GetRTTI() ? parent->GetRTTI()->name : "-",
											parent ? parent->GetFlags().underlying() : 0u, parent && const_cast<RE::NiNode*>(parent)->GetUserData() != nullptr, parent ? parent->worldBound.radius : 0.0f,
											grand && grand->GetRTTI() ? grand->GetRTTI()->name : "-")];
										// The entry's siblings that are not entries: what the list left out at the same level.
										if (parent)
											for (const auto& sibling : const_cast<RE::NiNode*>(parent)->GetChildren())
												if (sibling && sibling.get() != entry.get())
													++entryTypes[fmt::format("  sibling {} {:#x} user {}", sibling->GetRTTI() ? sibling->GetRTTI()->name : "?", sibling->GetFlags().underlying(),
														sibling->GetUserData() != nullptr)];
									}
									candidateEntryGeometries += entry && const_cast<RE::NiAVObject*>(entry.get())->AsGeometry() != nullptr;
									collect(entry.get(), entry.get());
								}
							}
						}
						// Per set (0 both, 1 DCLF only): not a candidate; a candidate whose path from its entry has a node
						// the custom planes reject; one the camera's far/left/right/bottom/top reject; neither.
						std::uint32_t candidateResult[2][4] = {};
						std::string candidateSamples;
						std::uint32_t candidateLogged = 0;
						auto testCandidate = [&](const RE::BSGeometry* a_geometry, std::uint32_t a_set) {
							const auto it = candidateEntry.find(a_geometry);
							if (it == candidateEntry.end()) {
								++candidateResult[a_set][0];
								if (a_set == 1 && candidateLogged < 6) {
									++candidateLogged;
									const RE::NiAVObject* top = a_geometry;
									std::string chain;
									for (const RE::NiAVObject* n = a_geometry; n && chain.size() < 900; n = n->parent) {
										// Per full-frustum process: the plane indices of its own frustum planes (what
										// TestBaseVisibility3 tests) that reject this node's worldBound.
										std::string outs;
										for (std::size_t k = 0; k < fullFrustumProcesses.size(); ++k) {
											const auto& fp = fullFrustumProcesses[k]->planes;
											const auto& wb = n->worldBound;
											std::string planesOut;
											for (std::uint32_t q = 0; q < 6; ++q)
												if ((fp.activePlanes.underlying() & (1u << q)) &&
													fp.cullingPlanes[q].normal.x * wb.center.x + fp.cullingPlanes[q].normal.y * wb.center.y + fp.cullingPlanes[q].normal.z * wb.center.z - fp.cullingPlanes[q].constant < -wb.radius)
													planesOut += std::to_string(q);
											if (!planesOut.empty())
												outs += fmt::format(" p{}:{}", k, planesOut);
										}
										chain += fmt::format(" > '{}' {} flags {:#x} r {:.0f}{}", n->name.c_str() ? n->name.c_str() : "", n->GetRTTI() ? n->GetRTTI()->name : "?", n->GetFlags().underlying(), n->worldBound.radius,
											outs.empty() ? "" : " OUT" + outs);
									}
									(void)top;
									candidateSamples += fmt::format(" | '{}' not a candidate:{}", a_geometry->name.c_str() ? a_geometry->name.c_str() : "", chain);
								}
								return;
							}
							const char* planeFail = nullptr;
							const char* cameraFail = nullptr;
							std::string failName;
							for (const RE::NiAVObject* n = a_geometry; n; n = n->parent) {
								const auto& wb = n->worldBound;
								if (!planeFail && customPlanes)
									for (std::uint32_t p = 0; p < 6; ++p)
										if ((customMask & (1u << p)) && customPlanes[p].normal.x * wb.center.x + customPlanes[p].normal.y * wb.center.y + customPlanes[p].normal.z * wb.center.z - customPlanes[p].constant < -wb.radius) {
											planeFail = n->GetRTTI() ? n->GetRTTI()->name : "?";
											failName = n->name.c_str() ? n->name.c_str() : "";
											break;
										}
								if (!cameraFail && camera) {
									std::uint32_t r[4] = {};
									const float c[3] = { wb.center.x, wb.center.y, wb.center.z };
									testCamera(c, wb.radius, r);
									if (r[1] || r[2] || r[3]) {
										cameraFail = n->GetRTTI() ? n->GetRTTI()->name : "?";
										if (failName.empty())
											failName = n->name.c_str() ? n->name.c_str() : "";
									}
								}
								if (n == it->second)
									break;
							}
							++candidateResult[a_set][planeFail ? 1 : cameraFail ? 2 : 3];
							if (a_set == 1 && candidateLogged < 6) {
								++candidateLogged;
								candidateSamples += fmt::format(" | '{}' candidate under '{}' {}: planes fail at {}, camera fails at {} ('{}')", a_geometry->name.c_str() ? a_geometry->name.c_str() : "",
									it->second->name.c_str() ? it->second->name.c_str() : "", it->second->GetRTTI() ? it->second->GetRTTI()->name : "?", planeFail ? planeFail : "-", cameraFail ? cameraFail : "-", failName);
							}
						};
						// The full-frustum volume on the geometry's own bound: per set (0 both, 1 DCLF only), outside
						// the first process's planes / customCullPlanes; and how far planes and customCullPlanes differ.
						std::uint32_t fullOut[2][2] = {};
						// On the property's fade node's bound instead: per set, outside; and, for candidates, whether the
						// entry covering the geometry is that fade node, an ancestor above it, or neither / no fade node.
						std::uint32_t fadeOut[2] = {}, entryIsFade[2] = {}, entryAboveFade[2] = {}, entryOther[2] = {}, noFade[2] = {};
						std::string fadeOutBoth, entryOtherSamples;
						// The reference root (the nearest ancestor, or the geometry, with userData): outside the
						// full-frustum planes per set, and whether it is the candidate's entry.
						std::uint32_t rootOut[2] = {}, noRoot[2] = {}, entryIsRoot[2] = {}, entryNotRoot[2] = {};
						std::string rootOutBoth, entryNotRootSamples;
						float planesDiff = 0.0f;
						std::string fullOutBoth;
						if (!fullFrustumProcesses.empty()) {
							const auto& a = fullFrustumProcesses[0]->planes;
							const auto& b = fullFrustumProcesses[0]->customCullPlanes;
							for (std::uint32_t q = 0; q < 6; ++q) {
								planesDiff = (std::max)(planesDiff, std::abs(a.cullingPlanes[q].normal.x - b.cullingPlanes[q].normal.x) + std::abs(a.cullingPlanes[q].normal.y - b.cullingPlanes[q].normal.y) +
																		 std::abs(a.cullingPlanes[q].normal.z - b.cullingPlanes[q].normal.z));
								planesDiff = (std::max)(planesDiff, std::abs(a.cullingPlanes[q].constant - b.cullingPlanes[q].constant));
							}
						}
						auto testFull = [&](const DrawInput& a_in, const RE::BSGeometry* a_geometry, std::uint32_t a_set) {
							if (fullFrustumProcesses.empty())
								return;
							const RE::NiAVObject* fade = nullptr;
							if (a_geometry)
								if (const auto* lighting = netimmerse_cast<const RE::BSLightingShaderProperty*>(const_cast<RE::BSGeometry*>(a_geometry)->GetGeometryRuntimeData().shaderProperty.get()))
									fade = lighting->fadeNode;
							if (!fade) {
								++noFade[a_set];
							} else {
								const auto& fp = fullFrustumProcesses[0]->planes;
								const auto& wb = fade->worldBound;
								for (std::uint32_t q = 0; q < 6; ++q)
									if ((fp.activePlanes.underlying() & (1u << q)) &&
										fp.cullingPlanes[q].normal.x * wb.center.x + fp.cullingPlanes[q].normal.y * wb.center.y + fp.cullingPlanes[q].normal.z * wb.center.z - fp.cullingPlanes[q].constant < -wb.radius) {
										++fadeOut[a_set];
										if (a_set == 0 && fadeOutBoth.size() < 400)
											fadeOutBoth += fmt::format(" '{}'", fade->name.c_str() ? fade->name.c_str() : "");
										break;
									}
							}
							const RE::NiAVObject* root = nullptr;
							{
								// The topmost ancestor carrying the same reference as the nearest one that carries any.
								const void* reference = nullptr;
								for (const RE::NiAVObject* n = a_geometry; n; n = n->parent) {
									const void* user = const_cast<RE::NiAVObject*>(n)->GetUserData();
									if (!reference && user)
										reference = user;
									if (reference && user == reference)
										root = n;
									else if (reference && user && user != reference)
										break;
								}
							}
							if (!root) {
								++noRoot[a_set];
							} else {
								const auto& fp = fullFrustumProcesses[0]->planes;
								const auto& wb = root->worldBound;
								for (std::uint32_t q = 0; q < 6; ++q)
									if ((fp.activePlanes.underlying() & (1u << q)) &&
										fp.cullingPlanes[q].normal.x * wb.center.x + fp.cullingPlanes[q].normal.y * wb.center.y + fp.cullingPlanes[q].normal.z * wb.center.z - fp.cullingPlanes[q].constant < -wb.radius) {
										++rootOut[a_set];
										if (a_set == 0 && rootOutBoth.size() < 400)
											rootOutBoth += fmt::format(" '{}' root '{}' {}", a_geometry->name.c_str() ? a_geometry->name.c_str() : "", root->name.c_str() ? root->name.c_str() : "", root->GetRTTI() ? root->GetRTTI()->name : "?");
										break;
									}
							}
							if (const auto it = candidateEntry.find(a_geometry); it != candidateEntry.end()) {
								if (it->second == root)
									++entryIsRoot[a_set];
								else {
									++entryNotRoot[a_set];
									if (entryNotRootSamples.size() < 600)
										entryNotRootSamples += fmt::format(" '{}' entry '{}' {} r {:.0f} children {} user {} parent '{}' {} r {:.0f}, is the root's parent {}; root '{}' {} r {:.0f}", a_geometry->name.c_str() ? a_geometry->name.c_str() : "", it->second->name.c_str() ? it->second->name.c_str() : "",
											it->second->GetRTTI() ? it->second->GetRTTI()->name : "?", it->second->worldBound.radius,
											const_cast<RE::NiAVObject*>(it->second)->AsNode() ? const_cast<RE::NiAVObject*>(it->second)->AsNode()->GetChildren().size() : 0,
											static_cast<const void*>(const_cast<RE::NiAVObject*>(it->second)->GetUserData()),
											it->second->parent && it->second->parent->name.c_str() ? it->second->parent->name.c_str() : "-", it->second->parent && it->second->parent->GetRTTI() ? it->second->parent->GetRTTI()->name : "-",
											it->second->parent ? it->second->parent->worldBound.radius : 0.0f, root && root->parent == it->second,
											root && root->name.c_str() ? root->name.c_str() : "-", root && root->GetRTTI() ? root->GetRTTI()->name : "-", root ? root->worldBound.radius : 0.0f);
								}
							}
							if (const auto it = candidateEntry.find(a_geometry); it != candidateEntry.end()) {
								if (it->second == fade)
									++entryIsFade[a_set];
								else {
									bool above = false;
									for (const RE::NiAVObject* n = fade ? fade->parent : nullptr; n; n = n->parent)
										if (n == it->second)
											above = true;
									++(above ? entryAboveFade : entryOther)[a_set];
									if (entryOtherSamples.size() < 600)
										entryOtherSamples += fmt::format(" '{}' entry '{}' {} fade '{}'", a_geometry->name.c_str() ? a_geometry->name.c_str() : "", it->second->name.c_str() ? it->second->name.c_str() : "",
											it->second->GetRTTI() ? it->second->GetRTTI()->name : "?", fade && fade->name.c_str() ? fade->name.c_str() : "-");
								}
							}
							for (std::uint32_t k = 0; k < 2; ++k) {
								const auto& fp = k == 0 ? fullFrustumProcesses[0]->planes : fullFrustumProcesses[0]->customCullPlanes;
								for (std::uint32_t q = 0; q < 6; ++q)
									if ((fp.activePlanes.underlying() & (1u << q)) &&
										fp.cullingPlanes[q].normal.x * a_in.boundCentre[0] + fp.cullingPlanes[q].normal.y * a_in.boundCentre[1] + fp.cullingPlanes[q].normal.z * a_in.boundCentre[2] - fp.cullingPlanes[q].constant < -a_in.boundRadius) {
										++fullOut[a_set][k];
										if (k == 0 && a_set == 0 && fullOutBoth.size() < 600 && a_geometry)
											fullOutBoth += fmt::format(" '{}' r {:.0f} plane {} by {:.0f}", a_geometry->name.c_str() ? a_geometry->name.c_str() : "", a_in.boundRadius, q,
												-(fp.cullingPlanes[q].normal.x * a_in.boundCentre[0] + fp.cullingPlanes[q].normal.y * a_in.boundCentre[1] + fp.cullingPlanes[q].normal.z * a_in.boundCentre[2] - fp.cullingPlanes[q].constant) - a_in.boundRadius);
										break;
									}
							}
						};
						ankerl::unordered_dense::set<const RE::BSGeometry*> dclfSet;
						ankerl::unordered_dense::map<const RE::BSGeometry*, int> reasonOf;
						std::uint32_t sunEntryRejected = 0;
						for (const auto& input : inputs) {
							// The sun's entry rule, as BuildDrawsCS applies it for the view (kCullSunEntry).
							if (view.sunView && ((input.flags & kInputOutsideSunEntry) || OutsideSunEntry(payload.inputs, tables, input.objectIndex))) {
								++sunEntryRejected;
								continue;
							}
							const int reason = cull(input);
							++byReason[reason];
							if (input.objectIndex < tables.objectGeometry.size())
								reasonOf[tables.objectGeometry[input.objectIndex]] = reason;
							if (reason == 1 || reason == 2 || reason == 3 || reason == 5)
								continue;
							++kept;
							const auto* geometry = input.objectIndex < tables.objectGeometry.size() ? tables.objectGeometry[input.objectIndex] : nullptr;
							dclfSet.insert(geometry);
							const float dx = input.boundCentre[0] - view.eye.x, dy = input.boundCentre[1] - view.eye.y;
							const double distance = std::sqrt(double(dx) * dx + double(dy) * dy);
							const std::uint32_t set = engine.contains(geometry) ? 0u : 1u;
							testPlanes(clipPlanes, clipMask, input.boundCentre, input.boundRadius, planeRejects[set], 0);
							testPlanes(customPlanes, customMask, input.boundCentre, input.boundRadius, planeRejects[set], 1);
							testCamera(input.boundCentre, input.boundRadius, cameraRejects[set][0]);
							testAncestors(geometry, set);
							testRule(geometry, set);
							if (geometry)
								testCandidate(geometry, set);
							testFull(input, geometry, set);
							if (geometry) {
								const auto& wb = geometry->worldBound;
								const float wc[3] = { wb.center.x, wb.center.y, wb.center.z };
								testCamera(wc, wb.radius, cameraRejects[set][1]);
							}
							if (engine.contains(geometry)) {
								++both;
								bothDistance += distance;
								continue;
							}
							++dclfOnly;
							onlyDistance += distance;
							const float r = input.boundRadius;
							++onlyByRadius[r < 64 ? 0 : r < 256 ? 1 : r < 1024 ? 2 : r < 4096 ? 3 : 4];
							const Clip centre = project(input.boundCentre[0], input.boundCentre[1], input.boundCentre[2]);
							++onlyZ[centre.z < 0 ? 0 : centre.z > centre.w ? 2 : 1];
							for (const auto& [otherId, otherSet] : engineByView)
								if (otherId != view.viewId && otherSet.contains(geometry)) {
									++onlyInOtherView;
									break;
								}
							if (geometry && named < 12) {
								++named;
								onlyNames += fmt::format("{}'{}' r {:.0f} d {:.0f} z {:.2f}", onlyNames.empty() ? "" : ", ", geometry->name.c_str() ? geometry->name.c_str() : "", r, distance,
									centre.w != 0 ? centre.z / centre.w : 0.0f);
							}
						}
						std::uint32_t engineOnly = 0, engineOnlyWithheld = 0, engineOnlyReason[7] = {};  // reasons 1-5 as cull(), 6 not an input
						std::string engineOnlyNames;
						for (const auto* geometry : engine) {
							if (dclfSet.contains(geometry))
								continue;
							++engineOnly;
							if (!withheldSet.contains(geometry))
								continue;
							++engineOnlyWithheld;
							const auto it = reasonOf.find(geometry);
							const int reason = it == reasonOf.end() ? 6 : it->second;
							++engineOnlyReason[reason];
							if (geometry && engineOnlyWithheld <= 8) {
								// Where the geometry is against the box: the worldBound's NDC x/y range, and for a skinned
								// part the NDC x/y range of its bones' translations (the palette is absolute world).
								const auto& wb = geometry->worldBound;
								float bx[2] = { 1e30f, -1e30f }, by[2] = { 1e30f, -1e30f };
								for (std::uint32_t c = 0; c < 8; ++c) {
									const float r = wb.radius;
									const Clip clip = project(wb.center.x + ((c & 1) ? r : -r), wb.center.y + ((c & 2) ? r : -r), wb.center.z + ((c & 4) ? r : -r));
									const float w = clip.w != 0 ? clip.w : 1.0f;
									bx[0] = (std::min)(bx[0], clip.x / w), bx[1] = (std::max)(bx[1], clip.x / w);
									by[0] = (std::min)(by[0], clip.y / w), by[1] = (std::max)(by[1], clip.y / w);
								}
								std::string bones = "unskinned";
								if (const auto* skin = const_cast<RE::BSGeometry*>(geometry)->GetGeometryRuntimeData().skinInstance.get(); skin && skin->boneMatrices && skin->numMatrices) {
									float kx[2] = { 1e30f, -1e30f }, ky[2] = { 1e30f, -1e30f };
									const auto* rows = static_cast<const float*>(skin->boneMatrices);
									for (std::uint32_t b = 0; b < skin->numMatrices; ++b) {
										const float* m3 = rows + b * 12;
										const Clip clip = project(m3[3], m3[7], m3[11]);
										const float w = clip.w != 0 ? clip.w : 1.0f;
										kx[0] = (std::min)(kx[0], clip.x / w), kx[1] = (std::max)(kx[1], clip.x / w);
										ky[0] = (std::min)(ky[0], clip.y / w), ky[1] = (std::max)(ky[1], clip.y / w);
									}
									const float* m0 = rows;
									bones = fmt::format("{} bones x {:.2f}..{:.2f} y {:.2f}..{:.2f}, bone0 ({:.0f} {:.0f} {:.0f})", skin->numMatrices, kx[0], kx[1], ky[0], ky[1], m0[3], m0[7], m0[11]);
								}
								engineOnlyNames += fmt::format("{}'{}' ({}; centre ({:.0f} {:.0f} {:.0f}) r {:.0f} x {:.2f}..{:.2f} y {:.2f}..{:.2f}; {}; world ({:.0f} {:.0f} {:.0f}))", engineOnlyNames.empty() ? "" : ", ",
									geometry->name.c_str() ? geometry->name.c_str() : "",
									reason == 1 ? "x/y" : reason == 2 ? "before near" : reason == 3 ? "beyond far" : reason == 5 ? "engine volume" : reason == 6 ? "not an input" : "?",
									wb.center.x, wb.center.y, wb.center.z, wb.radius, bx[0], bx[1], by[0], by[1], bones, geometry->world.translate.x, geometry->world.translate.y, geometry->world.translate.z);
							}
						}
						logger::info("[DCLF] cascade probe view {}: engine only {}, of them withheld (drawn by nobody) {}: rejected by DCLF x/y {}, before near {}, beyond far {}, engine volume {}, not an input {}",
							view.viewId, engineOnly, engineOnlyWithheld, engineOnlyReason[1], engineOnlyReason[2], engineOnlyReason[3], engineOnlyReason[5], engineOnlyReason[6]);
						logger::info("[DCLF] cascade probe view {} (slice {}, mode {:#x}): {} inputs; DCLF keeps {} (sun entry rejected {}, engine volume rejected {}, x/y rejected {}, before near {}, beyond far {}, w<=0 kept {}); engine registered {}; both {}, DCLF only {}, engine only {}",
							view.viewId, view.slice, view.renderMode, inputs.size(), kept, sunEntryRejected, byReason[5], byReason[1], byReason[2], byReason[3], byReason[4], engine.size(), both, dclfOnly, engineOnly);
						logger::info("[DCLF] cascade probe view {}: DCLF-only by radius <64 {} <256 {} <1024 {} <4096 {} more {}; centre depth before near {} inside {} beyond far {}; {} of them in another view's engine set; mean horizontal distance DCLF-only {:.0f}, both {:.0f}",
							view.viewId, onlyByRadius[0], onlyByRadius[1], onlyByRadius[2], onlyByRadius[3], onlyByRadius[4], onlyZ[0], onlyZ[1], onlyZ[2], onlyInOtherView,
							dclfOnly ? onlyDistance / dclfOnly : 0.0, both ? bothDistance / both : 0.0);
						logger::info("[DCLF] cascade probe view {}: engine volumes: clip mask {:#x}, custom flag {} mask {:#x}; rejected (sign A / sign B) - both: clip {}/{}, custom {}/{}; DCLF only: clip {}/{}, custom {}/{}",
							view.viewId, clipMask, customFlag, customMask, planeRejects[0][0][0], planeRejects[0][0][1], planeRejects[0][1][0], planeRejects[0][1][1],
							planeRejects[1][0][0], planeRejects[1][0][1], planeRejects[1][1][0], planeRejects[1][1][1]);
						if (camera) {
							const auto& f = camera->GetRuntimeData2().viewFrustum;
							logger::info("[DCLF] cascade probe view {}: engine camera at ({:.0f} {:.0f} {:.0f}) frustum l {:.0f} r {:.0f} t {:.0f} b {:.0f} n {:.0f} f {:.0f} ortho {}; rejected by near/far/lr/bt - both, DCLF bound: {}/{}/{}/{}, worldBound: {}/{}/{}/{}; DCLF only, DCLF bound: {}/{}/{}/{}, worldBound: {}/{}/{}/{}",
								view.viewId, camera->world.translate.x, camera->world.translate.y, camera->world.translate.z, f.fLeft, f.fRight, f.fTop, f.fBottom, f.fNear, f.fFar, f.bOrtho,
								cameraRejects[0][0][0], cameraRejects[0][0][1], cameraRejects[0][0][2], cameraRejects[0][0][3], cameraRejects[0][1][0], cameraRejects[0][1][1], cameraRejects[0][1][2], cameraRejects[0][1][3],
								cameraRejects[1][0][0], cameraRejects[1][0][1], cameraRejects[1][0][2], cameraRejects[1][0][3], cameraRejects[1][1][0], cameraRejects[1][1][1], cameraRejects[1][1][2], cameraRejects[1][1][3]);
						}
						logger::info("[DCLF] cascade probe view {}: ancestors - both: {} with an ancestor outside the custom planes, {} hidden, mean depth {:.1f}, flags or {:#x} and {:#x}; DCLF only: {} outside, {} hidden, mean depth {:.1f}, flags or {:#x} and {:#x}",
							view.viewId, ancestorOutside[0], ancestorHidden[0], both ? double(ancestorDepthSum[0]) / both : 0.0, flagsOr[0], flagsAnd[0], ancestorOutside[1], ancestorHidden[1],
							dclfOnly ? double(ancestorDepthSum[1]) / dclfOnly : 0.0, flagsOr[1], flagsAnd[1]);
						logger::info("[DCLF] cascade probe view {}: rule - both: {} without kCastShadows, {} faded, {} on a shared property, property flags or {:#x} and {:#x}; DCLF only: {} without kCastShadows, {} faded, {} shared, or {:#x} and {:#x}",
							view.viewId, noCast[0], faded[0], sharedProperty[0], propOr[0], propAnd[0], noCast[1], faded[1], sharedProperty[1], propOr[1], propAnd[1]);
						logger::info("[DCLF] cascade probe view {}: candidates - {} processes, {} entries ({} of them geometry), {} geometries; both: {} not a candidate, {} a node outside the planes, {} outside the camera, {} inside; DCLF only: {}/{}/{}/{};{}",
							view.viewId, candidateProcesses, candidateEntries, candidateEntryGeometries, candidateEntry.size(), candidateResult[0][0], candidateResult[0][1], candidateResult[0][2], candidateResult[0][3],
							candidateResult[1][0], candidateResult[1][1], candidateResult[1][2], candidateResult[1][3], candidateSamples);
						{
							std::string types;
							for (const auto& [name, count] : entryTypes)
								types += fmt::format(" | {} x{}", name, count);
							logger::info("[DCLF] cascade probe view {}: full-frustum processes:{}; entry types:{}", view.viewId, processInfo, types);
							std::string planes;
							for (std::size_t k = 0; k < fullFrustumProcesses.size() && k < 2; ++k)
								for (std::uint32_t q = 0; q < 6; ++q) {
									const auto& pl = fullFrustumProcesses[k]->planes.cullingPlanes[q];
									planes += fmt::format(" p{}.{} ({:.3f} {:.3f} {:.3f} {:.0f})", k, q, pl.normal.x, pl.normal.y, pl.normal.z, pl.constant);
								}
							logger::info("[DCLF] cascade probe view {}: full-frustum planes:{}", view.viewId, planes);
							logger::info("[DCLF] cascade probe view {}: full-frustum volume on the geometry bound - planes vs customCullPlanes differ by {:.4f}; both: {} outside planes, {} outside custom; DCLF only: {} / {};{}",
								view.viewId, planesDiff, fullOut[0][0], fullOut[0][1], fullOut[1][0], fullOut[1][1], fullOutBoth);
							logger::info("[DCLF] cascade probe view {}: full-frustum volume on the fade node's bound - both: {} outside, DCLF only: {} outside; no fade node: {} / {}; entry is the fade node {} / {}, above it {} / {}, other {} / {};{} | entries not the fade node:{}",
								view.viewId, fadeOut[0], fadeOut[1], noFade[0], noFade[1], entryIsFade[0], entryIsFade[1], entryAboveFade[0], entryAboveFade[1], entryOther[0], entryOther[1], fadeOutBoth, entryOtherSamples);
							logger::info("[DCLF] cascade probe view {}: full-frustum volume on the reference root's bound - both: {} outside, DCLF only: {} outside; no root {} / {}; entry is the root {} / {}, not {} / {};{} | entries not the root:{}",
								view.viewId, rootOut[0], rootOut[1], noRoot[0], noRoot[1], entryIsRoot[0], entryIsRoot[1], entryNotRoot[0], entryNotRoot[1], rootOutBoth, entryNotRootSamples);
						}
						logger::info("[DCLF] cascade probe view {}: rule samples both:{}", view.viewId, ruleSamples[0]);
						logger::info("[DCLF] cascade probe view {}: rule samples DCLF only:{}", view.viewId, ruleSamples[1]);
						logger::info("[DCLF] cascade probe view {}: ancestry both:{}", view.viewId, ancestry[0]);
						logger::info("[DCLF] cascade probe view {}: ancestry DCLF only:{}", view.viewId, ancestry[1]);
						if (clipPlanes && customPlanes) {
							std::string planes;
							for (std::uint32_t p = 0; p < 6; ++p)
								planes += fmt::format(" clip{} ({:.3f} {:.3f} {:.3f} {:.0f}) custom{} ({:.3f} {:.3f} {:.3f} {:.0f})", p, clipPlanes[p].normal.x, clipPlanes[p].normal.y,
									clipPlanes[p].normal.z, clipPlanes[p].constant, p, customPlanes[p].normal.x, customPlanes[p].normal.y, customPlanes[p].normal.z, customPlanes[p].constant);
							logger::info("[DCLF] cascade probe view {} planes (eye {:.0f} {:.0f} {:.0f}):{}", view.viewId, view.eye.x, view.eye.y, view.eye.z, planes);
						}
						logger::info("[DCLF] cascade probe view {}: DCLF only e.g. {}; withheld engine only e.g. {}", view.viewId, onlyNames, engineOnlyNames);
					}
				}
			}

			// Nothing to draw until this commit publishes the shape again (so a failed one draws nothing, rather
			// than a reused recording reading latch values this execution never wrote).
			resources->frame.store(nullptr, std::memory_order_release);
			CommitUploads uploads(impl->commitStagedPool);
			// ---- The commit: the shared uploads, the per-mode inputs, then per view its blocks at its slot of
			// the arena's head, its copy of the records naming them, its count buffer zeroed, and the view.
			const std::uint64_t base = resources->constantsAddress;
			auto& arena = payload.arena;
			auto& records = payload.records;
			bool staged = false;
			std::uint32_t stagedSlots = 0;
			TracyCZoneN(shadowCommitZone, "CS.DCLF.ShadowInputs.CommitShared", true);
			const auto inputsStart = std::chrono::steady_clock::now();
			// The worker's build staged what does not depend on the views (StageShadowPayload): one submission,
			// ahead of this commit's own uploads. A build made here, or staged against resources since recreated,
			// is uploaded from its vectors.
			staged = useAsync && payload.staged && payload.stagedFor == resources.get();
			stagedSlots = staged ? payload.stagedSlots : 0;
			if (staged) {
				org::runtime::GetActiveUploadService()->SubmitStagedUploads(std::move(payload.staged));
			} else {
				payload.objects.Emit(resources->tablesHeld.objects, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
					uploads(resources->objects, a_data, a_bytes, a_offset);
				});
				EmitBones(payload.bones, resources->tablesHeld.bones, nullptr, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
					uploads(resources->bones, a_data, a_bytes, a_offset);
				});
				EmitGeometryDraws(payload.geometries, resources->tablesHeld.geometries, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
					uploads(resources->geometries, a_data, a_bytes, a_offset);
				});
			}
			// Either path uploaded the object records, the bone rows and the geometry slots' draws the buffers did not hold.
			if (payload.objects.Version())
				resources->tablesHeld.objects = payload.objects.Version();
			if (payload.geometries.Version())
				resources->tablesHeld.geometries = payload.geometries.Version();
			if (PersistentParityEnabled() && payload.bones.Version()) {
				auto& bonesStore = impl->shadowBones;
				EmitBones(payload.bones, resources->tablesHeld.bones, &bonesStore, [](const void*, std::size_t, std::size_t) {});
				if (payload.inputs.frameNumber % 60 == 0)
					CheckBones(bonesStore, payload.bones);
			}
			if (payload.bones.Version())
				resources->tablesHeld.bones = payload.bones.Version();
			shadowStats.faceUploads += UploadFaceStreams(payload.faceStreams, resources->facePositions, resources->faceUploaded, uploads);
			shadowStats.records = static_cast<std::uint32_t>(records.size());
			shadowStats.skippedTexture = payload.skippedTexture;
			shadowStats.skippedPipeline = payload.skippedPipeline;
			shadowStats.deferredTextures = payload.deferredTextures;
			shadowStats.deferredPipelines = payload.deferredPipelines;
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
				if (!modeUsed[m])
					continue;
				if (!staged)
					EmitShadowInputs(payload, m, payload.kept ? resources->inputsUploaded[m] : 0,
						[&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) { uploads(resources->inputs[m], a_data, a_bytes, a_offset); });
				// Either path wrote what the buffer did not hold.
				if (payload.kept)
					resources->inputsUploaded[m] = payload.regionInputs[m].Version();
				shadowStats.inputs = static_cast<std::uint32_t>(payload.ModeInputs(m));
			}
			inputsMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - inputsStart).count();
			TracyCZoneEnd(shadowCommitZone);

			TracyCZoneN(shadowViewsZone, "CS.DCLF.ShadowInputs.BuildViews", true);
			const auto blocksStart = std::chrono::steady_clock::now();
			auto frame = std::make_shared<ShadowFrame>();
			const auto& previousShape = resources->published;
			const std::uint32_t latchSlot = RenderGraphRuntime::Get().Host()->CurrentFrameSlot();
			resources->labels.clear();
			frame->resourceHeap = org::runtime::GetActiveSRVDescriptorHeap().GetHandle();
			frame->samplerHeap = org::runtime::GetActiveSamplerDescriptorHeap().GetHandle();
			frame->indirect = indirect;
			auto& slotRecords = impl->shadowSlotRecords;
			static const std::uint32_t zero[kCountWords] = {};
			std::uint32_t mapRowsWritten = 0;  // bit per view rasterizer state whose map row is in the latch
			for (std::uint32_t slot = 0; slot < pending.size(); ++slot) {
				const auto& view = pending[slot];
				const std::uint64_t viewBlockOffset = std::uint64_t(slot) * kShadowViewSlotBytes;
				const std::uint64_t perFrameOffset = viewBlockOffset + kShadowPerFrameOffset;
				std::memcpy(arena.At(viewBlockOffset, sizeof(view.viewBlock)).data(), view.viewBlock, sizeof(view.viewBlock));
				std::memcpy(arena.At(perFrameOffset, view.perFrameBytes).data(), view.perFrame.data(), view.perFrameBytes);
				const std::uint64_t recordsOffset = std::uint64_t(slot) * kShadowRecordCapacity * sizeof(DrawBindings);
				if (slot >= stagedSlots) {
					if (payload.kept) {
						EmitShadowRecords(payload, resources->recordsUploaded[slot], base + viewBlockOffset, base + perFrameOffset,
							[&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) { uploads(resources->records, a_data, a_bytes, recordsOffset + a_offset); });
					} else {
						slotRecords = records;
						for (auto& record : slotRecords) {
							record.vertexConstants[0] = base + viewBlockOffset;
							record.pixelConstants[0] = base + viewBlockOffset;
							record.vertexConstants[kPerFrameVertexRegister] = base + perFrameOffset;
							record.pixelConstants[kPerFrameVertexRegister] = base + perFrameOffset;
						}
						uploads(resources->records, slotRecords.data(), slotRecords.size() * sizeof(DrawBindings), recordsOffset);
					}
					uploads(resources->count[slot], zero, sizeof(zero), 0);
				}
				// Either path wrote what the slot did not hold.
				if (payload.kept)
					resources->recordsUploaded[slot] = payload.recordChanges.version;
				const auto inputCount = static_cast<std::uint32_t>(payload.ModeInputs(view.modeIndex));
				// The view's values into its latch: frustum culling alone (mode 1), the single phase, and no
				// engine-visibility gate - a caster is drawn whether or not the main camera kept it.
				BuildDrawsLatch latch{};
				latch.dispatch[0] = (inputCount + 63) / 64;
				latch.dispatch[1] = 1;
				latch.dispatch[2] = 1;
				latch.drawCount = inputCount;
				latch.cullFlags = (view.hasViewProj ? (1u | (view.renderMode == 0xE ? kCullNoNearPlane : 0u)) : 0u) |
				                  (view.casterClass ? kCullVolumetricOnly : kCullCastersOnly) | (view.sunView ? kCullSunEntry : 0u);
				latch.cullPlaneMask = view.cullPlaneMask;
				std::memcpy(latch.cullPlanes, view.cullPlanes, sizeof(latch.cullPlanes));
				latch.visibilityStamp = frameNumber & 0x0FFFFFFFu;  // 28 bits: BuildDrawsCS keeps flags below it
				// A sun view's entry rule on the GPU: the frame's full-frustum processes (six at the sun's usual setup), which
				// BuildDraws tests every input's entry sphere against (SetSunEntryRow).
				const auto& entryMasks = payload.inputs.sunEntryPlaneMasks;
				if (view.sunView && entryMasks.size() <= kMaxSunEntryProcesses) {
					latch.sunEntryCount = static_cast<std::uint32_t>(entryMasks.size());
					for (std::size_t process = 0; process < entryMasks.size(); ++process) {
						latch.sunEntryMasks[process] = entryMasks[process];
						for (std::uint32_t plane = 0; plane < 6; ++plane)
							std::memcpy(latch.sunEntryPlanes[process][plane], payload.inputs.sunEntryPlanes[process * 6 + plane].data(), 4 * sizeof(float));
					}
					// CS_DCLF_PERSISTENT_PARITY: the GPU's test on the latch as written, against the CPU's verdict, per input.
					if (PersistentParityEnabled() && ParityDue(frameNumber)) {
						const auto& tablesNow = store.GetTables();
						for (const auto& input : payload.Flat(view.modeIndex)) {
							const bool cpu = OutsideSunEntry(payload.inputs, tablesNow, input.objectIndex);
							++shadowStats.sunEntryChecks;
							shadowStats.sunEntryMismatches += cpu != OutsideSunEntryLatch(latch, input.fade) ? 1 : 0;
						}
					}
				}
				FoldEyeIntoViewProj(view.viewProj, view.eye, latch.viewProj);
				// The view's rasterizer state picks its row of the pipeline map, written once per state below. The
				// shader reads the row at an offset into the whole latch block, so it carries this slot's base: a
				// slot-relative offset read slot 0's rows, which async epochs never write (every draw got pipeline 0).
				const std::uint32_t mapRowOffset = kShadowPipelineMapOffset + (view.rasterState - 1) * kShadowPipelineMapRowBytes;
				latch.pipelineMapOffset = static_cast<std::uint32_t>(resources->latch->Offset(latchSlot)) + mapRowOffset;
				if (!((mapRowsWritten >> view.rasterState) & 1)) {
					mapRowsWritten |= 1u << view.rasterState;
					const auto& row = store.GetLookups().shadowMapRows[view.rasterState];
					if (!row.empty())
						resources->latch->Write(latchSlot, mapRowOffset,
							std::as_bytes(std::span(row.data(), std::min<std::size_t>(row.size(), kMaxShadowSlots))));
				}
				resources->latch->WriteValue(latchSlot, slot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), latch);
				resources->labels.push_back({ view.viewId, view.renderMode, slot });
				ShadowFrameView out{};
				out.slot = slot;
				out.modeIndex = view.modeIndex;
				std::uint32_t previousCapacity = 0;
				if (previousShape)
					for (const auto& previous : previousShape->views)
						if (previous.slot == slot)
							previousCapacity = previous.capacity;
				out.capacity = GrowCapacity(previousCapacity, inputCount, kMaxDraws);
				out.x = view.x;
				out.y = view.y;
				out.width = view.width;
				out.height = view.height;
				out.minDepth = view.minDepth;
				out.maxDepth = view.maxDepth;
				out.target = view.targetIndex;
				out.slice = view.slice;
				out.recordsAddress = resources->recordsAddress + recordsOffset;
				frame->views.push_back(out);
			}
			// Staged, only the view head this epoch wrote goes up from here; the rest of the arena is the worker's.
			const auto& bytes = arena.Bytes();
			const std::size_t arenaBytes = staged ? std::min<std::size_t>(bytes.size(), pending.size() * kShadowViewSlotBytes) : bytes.size();
			if (arenaBytes)
				uploads(resources->constants, bytes.data(), arenaBytes, 0);
			blocksMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - blocksStart).count();
			static std::uint32_t logged = 0;
			if (logged++ % 600 == 0) {
				std::string views;
				for (std::size_t v = 0; v < frame->views.size() && v < pending.size(); ++v) {
					const auto& view = frame->views[v];
					views += fmt::format("{}view {} mode {:#x} target {} slice {} at ({} {}) {}x{} {} inputs (capacity {})", views.empty() ? "" : "; ", pending[v].viewId,
						pending[v].renderMode, view.target, view.slice, view.x, view.y, view.width, view.height, payload.ModeInputs(view.modeIndex), view.capacity);
				}
				logger::info("[DCLF] shadow epoch: {} views ({} without a pipeline, {} without a texture), {} records: {}", frame->views.size(),
					shadowStats.skippedPipeline, shadowStats.skippedTexture, shadowStats.records, views);
			}
			if (previousShape && previousShape->SameShape(*frame)) {
				resources->frame.store(previousShape, std::memory_order_release);
			} else {
				frame->generation = ++resources->shapeGenerations;
				resources->published = frame;
				resources->frame.store(std::move(frame), std::memory_order_release);
			}
			TracyCZoneEnd(shadowViewsZone);
		});
		const double totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		shadowStats.prepareMs += prepareMs;
		shadowStats.inputsMs += inputsMs;
		shadowStats.blocksMs += blocksMs;
		shadowStats.executeMs += totalMs - bodyMs;  // the graph's own compile, prepare and record
		if (ok) {
			++shadowStats.epochs;
			shadowStats.viewsDrawn += static_cast<std::uint32_t>(pending.size());
			// Skylighting's occluders are uploaded: its map can be DCLF's this frame if none was left out.
			if (modeUsed[kSkyMode]) {
				impl->skyCommittedFrame = frameNumber;
				impl->skyInputs = static_cast<std::uint32_t>(payload.ModeInputs(kSkyMode));
				impl->skySkipped = payload.skySkipped;
			}
			impl->ReadShadowCullCounters(frameNumber, shadowStats);
			// Static shadow ownership: what this frame's epoch drew for a mode is what that mode's views'
			// registrations are withheld for, from the next frame on.
			if (PassCapture::ShadowWithholdingEnabled()) {
				for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
					if (modeUsed[m] && m != kSkyMode)
						impl->PublishShadowClaims(PassCapture::kFirstShadowMode + m, payload.inputList[m], usedWorkerBuild || payload.kept ? payload.claims[m] : nullptr,
							shadowStats);
				}
				// The cascades' claims decide which sun entries the next frame's cascade culls may skip.
				if (modeUsed[kSunShadowMode])
					SunAccumulation::Get().PublishExclusion(usedWorkerBuild ? payload.sunExclusion :
					                                                          BuildSunExclusion(payload.inputs.sunCandidates, payload, kSunShadowMode, SceneStore::Get().GetTables(), &impl->sunExclusionCache));
			}
			pending.clear();
		} else {
			logger::error("[DCLF] the shadow epoch failed; the render graph is disabled");
			notReady(ShadowNotReady::Epoch);
		}
		shadowStats.cpuMs += totalMs;
	}

	ShadowInputs IndirectDraws::Impl::PrepareShadowInputs(const SceneStore& a_store, const ShadowResources& a_resources, const std::array<bool, kShadowModeCount>& a_modeUsed,
		const std::array<std::uint32_t, kShadowModeCount>& a_modeRasterStates) const
	{
		ZoneScopedN("CS.DCLF.PrepareShadowInputs");
		ShadowInputs in;
		in.frameNumber = a_store.GetFrame();
		in.renderFlags = a_store.GetMainPassRenderFlags();
		in.refEye = shadowRefEye;
		in.modeUsed = a_modeUsed;
		in.modeRasterStates = a_modeRasterStates;
		// The sun's full-frustum planes, read on the render thread after the full-frustum cull has run
		// (NiCamera::CalculateAndDrawShadowCasterLights precedes both the build's kick and the views).
		if (auto* node = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr)
			if (auto* sun = node->GetRuntimeData().sunShadowDirLight)
				for (const auto& process : sun->GetShadowDirectionalLightRuntimeData().fullFrustumCullingProcessArray) {
					if (!process)
						continue;
					const auto& planes = process->planes;
					for (std::uint32_t p = 0; p < 6; ++p)
						in.sunEntryPlanes.push_back({ planes.cullingPlanes[p].normal.x, planes.cullingPlanes[p].normal.y, planes.cullingPlanes[p].normal.z,
							planes.cullingPlanes[p].constant });
					in.sunEntryPlaneMasks.push_back(planes.activePlanes.underlying() & 0x3Fu);
				}
		in.sunCandidates = a_store.GetSunCandidates();
		in.addresses.constants = a_resources.constantsAddress;
		in.addresses.records = a_resources.recordsAddress;
		in.addresses.objectsIndex = a_resources.objectsIndex;
		in.addresses.bonesIndex = a_resources.bonesIndex;
		in.addresses.facePositions = FaceSnapshots::Enabled() ? a_resources.facePositionsAddress : 0;
		in.addresses.recordCapacity = kShadowRecordCapacity;
		in.addresses.identity = &a_resources;
		in.tablesGeneration = a_store.GetTablesGeneration();
		in.lookupGeneration = a_store.GetLookups().generation;
		in.sceneRebuilds = a_store.GetSceneRebuilds();
		in.tablesHeld = a_resources.tablesHeld;
		in.inputsHeld = a_resources.inputsUploaded;
		for (const auto held : a_resources.recordsUploaded)
			if (held && (!in.recordsOldestHeld || held < in.recordsOldestHeld))
				in.recordsOldestHeld = held;
		if (auto* csState = globals::state) {
			const auto* shared = reinterpret_cast<const std::byte*>(&csState->lastSharedData);
			in.sharedData.assign(shared, shared + sizeof(State::SharedDataCB));
			if (!csState->lastFeatureData.empty()) {
				const auto* feature = reinterpret_cast<const std::byte*>(csState->lastFeatureData.data());
				in.featureData.assign(feature, feature + csState->lastFeatureData.size());
			}
		}
		return in;
	}

	void IndirectDraws::KickShadowBuild()
	{
		// The shadow epoch's inputs are final from here to AfterShadowMaps: the scene phase has just built the
		// tables and the frame's reference eye is set. The build runs on the worker while the engine draws
		// the shadow maps, for last frame's render modes (a change is stale, and built inline).
		impl->DropShadowJob(stats);
		if (!ShadowsEnabled() || failed || !AsyncJobEnabled("shadow"))
			return;
		auto& async = stats.async[kAsyncShadow];
		auto& store = SceneStore::Get();
		const auto& tables = store.GetTables();
		auto& job = impl->shadowJob;
		// With the scene walk on the worker the tables are its until the join, and this build is queued behind
		// it: they are checked by the epoch instead.
		const bool tablesReady = store.ScenePending() || (!tables.objects.empty() && tables.shadowTechnique.size() == tables.objects.size());
		if (!impl->shadow || !job.modesKnown || !DrawPipelines::Get().Enabled() || !globals::game::utilityShader || !tablesReady) {
			++async.notKicked;
			return;
		}
		job.inputs = impl->PrepareShadowInputs(store, *impl->shadow, job.modes, job.rasterStates);
		++async.kicked;
		const auto* tablesPtr = &tables;
		const auto* lookups = &store.GetLookups();
		auto* payload = &impl->shadowPayload;
		auto* pool = &job.stagedPool;
		auto* objects = impl->ShadowObjects();
		auto* bonesStore = impl->ShadowBones();
		auto* geometriesStore = impl->ShadowGeometries();
		auto* exclusionCache = &impl->sunExclusionCache;
		auto* kept = impl->ShadowKeptState();
		const ShadowInputs inputs = job.inputs;
		const bool claims = PassCapture::ShadowWithholdingEnabled();
		job.handle = AsyncWorker::Get().Submit("shadow", [inputs, tablesPtr, lookups, payload, pool, objects, bonesStore, kept, geometriesStore, exclusionCache, target = impl->shadow, slots = job.views, claims](std::stop_token) {
			BuildShadowPayload(inputs, *tablesPtr, *lookups, *payload, objects, bonesStore, kept, geometriesStore);
			StageShadowPayload(*payload, *target, slots, *pool);
			if (claims) {
				ZoneScopedN("CS.DCLF.BuildShadow.Claims");
				for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
					if (inputs.modeUsed[m] && m != kSkyMode && !payload->kept)
						payload->claims[m] = ShadowClaimSet(payload->inputList[m], *tablesPtr);
				if (inputs.modeUsed[kSunShadowMode])
					payload->sunExclusion = BuildSunExclusion(inputs.sunCandidates, *payload, kSunShadowMode, *tablesPtr, exclusionCache);
			}
		});
	}

	void IndirectDraws::Impl::DropShadowJob(IndirectDraws::Stats& a_stats)
	{
		if (!shadowJob.handle)
			return;
		AsyncWorker::Get().Cancel(shadowJob.handle);
		++a_stats.async[kAsyncShadow].dropped;
		shadowJob.handle = {};
	}

	void IndirectDraws::Impl::ReadShadowCullCounters(std::uint32_t a_frame, IndirectDraws::ShadowStats& a_stats)
	{
		auto* context = globals::d3d::context;
		if (shadowCullReadback) {
			// Frames, not epochs: several views run per frame, and the copy needs the GPU to have finished
			// the sampled view's epoch, which three Presents later it has.
			if (a_frame - shadowCullReadback->copiedFrame < 3)
				return;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(context->Map(shadowCullReadback->count.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
				const auto* words = static_cast<const std::uint32_t*>(mapped.pData);
				a_stats.cullDrawn = words[0];
				a_stats.cullRejected = words[1];
				a_stats.cullTested = words[2];
				a_stats.cullSampledView = shadowCullReadback->view;
				a_stats.cullSampledMode = shadowCullReadback->mode;
				context->Unmap(shadowCullReadback->count.get(), 0);
			}
			shadowCullReadback.reset();
			return;
		}
		// One epoch in 127, and within it the views in turn, so every view of the frame gets sampled.
		const auto epoch = shadowCullEpochs++;
		if ((epoch % 127) != 0 || !shadow || shadow->labels.empty())
			return;
		const auto& view = shadow->labels[(epoch / 127) % shadow->labels.size()];
		if (view.slot >= kMaxShadowViews || !shadow->countD3D11[view.slot])
			return;
		D3D11_BUFFER_DESC desc{};
		shadow->countD3D11[view.slot]->GetDesc(&desc);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.MiscFlags = 0;
		desc.StructureByteStride = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ShadowCullReadback readback;
		if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, readback.count.put())))
			return;
		ScopedPerfEvent event("CS DCLF: shadow culling readback");
		context->CopyResource(readback.count.get(), shadow->countD3D11[view.slot].get());
		readback.copiedFrame = a_frame;
		readback.view = view.viewId;
		readback.mode = view.renderMode;
		shadowCullReadback = std::move(readback);
	}

	void IndirectDraws::Impl::PublishShadowClaims(std::uint32_t a_renderMode, const std::vector<DrawInput>& a_inputs, std::shared_ptr<const PassCapture::ClaimSet> a_built,
		IndirectDraws::ShadowStats& a_stats)
	{
		if (a_renderMode < PassCapture::kFirstShadowMode || a_renderMode >= PassCapture::kFirstShadowMode + PassCapture::kShadowModes)
			return;
		const auto start = std::chrono::steady_clock::now();
		const auto modeIndex = a_renderMode - PassCapture::kFirstShadowMode;
		// The worker's set when its build was the one drawn, else built here from the same inputs.
		auto claims = a_built ? std::move(a_built) : ShadowClaimSet(a_inputs, SceneStore::Get().GetTables());
		a_stats.claimed[modeIndex] = static_cast<std::uint32_t>(claims->size());
		PassCapture::Get().PublishShadowClaims(modeIndex, std::move(claims));
		a_stats.claimMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	}

	IndirectDraws::IndirectDraws() :
		impl(std::make_unique<Impl>())
	{}

	IndirectDraws::~IndirectDraws() = default;

	IndirectDraws& IndirectDraws::Get()
	{
		static IndirectDraws draws;
		return draws;
	}

	bool IndirectDraws::Enabled() const
	{
		return DrawPipelines::Get().Enabled();
	}

	bool IndirectDraws::Hybrid()
	{
		return HybridEnabled();
	}

	void IndirectDraws::PublishClaims()
	{
		if (!PassCapture::WithholdingEnabled())
			return;
		ScopedScan scan(Scan::Claims);
		const auto frame = SceneStore::Get().GetFrame();
		auto& capture = PassCapture::Get();

		// Hole detector. Everything claimed when this frame's passes were registered should have been
		// drawn by the colour epoch that has just run; the native loop was told not to draw it. Counted on
		// the CPU, exactly, with no readback.
		// A hole is an object that was withheld *and* not drawn. Being claimed is not enough on its own:
		// an object the engine culled this frame is never registered, so it is never withheld either, and
		// nobody was going to draw it. The test for "the engine would have drawn it" is that its pass was
		// captured this frame, which is exactly what the tables are built from.
		auto& store = SceneStore::Get();
		auto& captureStats = capture.MutableStats();
		captureStats.holes = 0;
		// The report's own interval, so a hole in any frame is seen rather than only in the last one.
		struct HoleReport
		{
			std::uint32_t frames = 0, framesWithHoles = 0, holes = 0, samples = 0;
			std::array<std::uint32_t, static_cast<std::size_t>(Ineligible::Count)> byReason{};
			std::uint32_t inTables = 0, handedBack = 0;
		};
		static HoleReport report;
		std::uint32_t frameHoles = 0;
		std::optional<ScopedScan> scanClaims(std::in_place, Scan::HoleClaims);
		// A hole was withheld, and withholding is a registration's (claimed when it registered): walked from this frame's
		// withheld registrations, which are far fewer than the claims.
		static std::vector<const RE::BSGeometry*> holeGeometries;
		holeGeometries.clear();
		for (const auto& entry : capture.LastDrain())
			if (entry.withheld && entry.geometry && !capture.HandedBack(entry.geometry) && !impl->DrawnThisFrame(entry.geometry, frame))
				holeGeometries.push_back(entry.geometry);
		std::sort(holeGeometries.begin(), holeGeometries.end());
		holeGeometries.erase(std::unique(holeGeometries.begin(), holeGeometries.end()), holeGeometries.end());
		for (const auto* geometry : holeGeometries) {
			const auto* accumulated = store.FindAccumulatedPass(geometry);
			++captureStats.holes;
			++frameHoles;
			bool fromAccumulate = false;
			const Ineligible reason = store.ReasonThisFrame(geometry, &fromAccumulate);
			++report.byReason[static_cast<std::size_t>(reason)];
			const std::int32_t object = store.FindObject(geometry);
			report.inTables += object >= 0;
			if (report.samples++ < 30) {
				logger::info("[DCLF] hole, frame {}: '{}' withheld and not drawn - {} ({}), {}, pass technique {:#x} list {}, last drawn by DCLF {}", frame,
					geometry->name.c_str(), kIneligibleNames[static_cast<std::size_t>(reason)], fromAccumulate ? "this frame's accumulate phase" : "the scene phase",
					object >= 0 ? fmt::format("object {} in the tables", object) : std::string("not in the tables"), accumulated ? accumulated->technique : 0u,
					accumulated ? accumulated->subPass : 0u,
					impl->drawnGeometry.contains(geometry) ? fmt::format("{} frames ago", frame - impl->drawnGeometry.find(geometry)->second.last) : std::string("never"));
			}
		}
		// The claimed objects with a pass this frame (registered, synthetic or resident), for the reports alone: every 16th
		// frame, held in between.
		if (frame % 16 == 0) {
			captureStats.claimed = 0;
			if (const auto previous = capture.CurrentClaims())
				for (const auto& [geometry, pass] : store.GetAccumulatedPasses())
					captureStats.claimed += previous->contains(geometry) ? 1u : 0u;
		}
		// The primary's left-out objects (PrimaryCull): nothing registered them, so a synthetic pass the colour epoch did
		// not draw is a hole whatever the claims say.
		scanClaims.reset();
		std::optional<ScopedScan> scanSynthetic(std::in_place, Scan::HoleSynthetic);
		for (const auto& [geometry, pass] : PrimaryCull::Get().SyntheticPasses()) {
			if (impl->DrawnThisFrame(geometry, frame))
				continue;
			PrimaryCull::Get().CountHole();
			++frameHoles;
			bool fromAccumulate = false;
			const Ineligible reason = store.ReasonThisFrame(geometry, &fromAccumulate);
			++report.byReason[static_cast<std::size_t>(reason)];
			if (report.samples++ < 30)
				logger::info("[DCLF] hole, frame {}: '{}' left out of the primary's cull and not drawn - {} ({}), synthetic technique {:#x} list {}", frame,
					geometry->name.c_str(), kIneligibleNames[static_cast<std::size_t>(reason)], fromAccumulate ? "this frame's accumulate phase" : "the scene phase",
					pass.technique, pass.subPass);
		}
		// The entries the primary's cull reached in view and DCLF drew in full are left out from the next frame on.
		scanSynthetic.reset();
		{
			ScopedScan scanAdmit(Scan::Admit);
			PrimaryCull::Get().Admit([&](const RE::BSGeometry* a_geometry) { return impl->DrawnThisFrame(a_geometry, frame); });
		}
		++report.frames;
		report.handedBack += captureStats.handedBack;
		report.holes += frameHoles;
		report.framesWithHoles += frameHoles != 0;
		if (report.frames == 300) {
			std::string reasons;
			for (std::size_t r = 0; r < report.byReason.size(); ++r)
				if (report.byReason[r])
					reasons += fmt::format(" {}={}", kIneligibleNames[r], report.byReason[r]);
			logger::info("[DCLF] holes over {} frames: {} in {} frames ({} of them in the tables); by reason:{}; {} withheld passes handed back to the native loop",
				report.frames, report.holes, report.framesWithHoles, report.inTables, reasons.empty() ? " -" : reasons, report.handedBack);
			report = {};
		}

		// The claims, kept (Impl::claims): a geometry is added when the colour epoch starts drawing it, and dropped a frame
		// after its last draw (the same one-frame tolerance the native skip uses). Published only when they changed.
		std::size_t kept = 0;
		for (const auto& [geometry, from] : impl->pendingUnclaims) {
			if (from > frame) {
				impl->pendingUnclaims[kept++] = { geometry, from };
				continue;
			}
			const auto held = impl->drawnGeometry.find(geometry);
			if (held != impl->drawnGeometry.end() && (held->second.drawn || frame - held->second.last <= 1))
				continue;  // drawn again since
			if (held != impl->drawnGeometry.end())
				impl->drawnGeometry.erase(held);
			if (impl->claimSet.erase(geometry)) {
				impl->claimsChanged = true;
				++impl->claimsDropped;
				// Still registered by the engine, so the engine will draw it from now on: this is the signature of
				// culling undoing itself.
				if (store.FindAccumulatedPass(geometry))
					++impl->claimsDroppedAfterCull;
			}
		}
		impl->pendingUnclaims.resize(kept);
		captureStats.claimsAdded = std::exchange(impl->claimsAdded, 0);
		captureStats.claimsDropped = std::exchange(impl->claimsDropped, 0);
		captureStats.droppedAfterCull = std::exchange(impl->claimsDroppedAfterCull, 0);
		// Republished when they changed, or when someone else published over them (a load publishes an empty set).
		if (impl->claimsChanged || capture.CurrentClaims() != impl->publishedClaims) {
			impl->publishedClaims = std::make_shared<const PassCapture::ClaimSet>(impl->claimSet);
			capture.PublishClaims(impl->publishedClaims);
			impl->claimsChanged = false;
		}
	}

	bool IndirectDraws::DrewLastFrame(const RE::BSGeometry* a_geometry, std::uint32_t a_frame) const
	{
		const auto drawn = impl->drawnGeometry.find(a_geometry);
		return drawn != impl->drawnGeometry.end() && (drawn->second.drawn || a_frame - drawn->second.last <= 1);
	}

	std::uint32_t IndirectDraws::DrainVisibilityFeedback(const std::function<void(const VisibilityFeedbackFrame&)>& a_consume)
	{
		const auto resources = impl->resources;
		if (!resources || !resources->feedback || !resources->feedback->timeline)
			return 0;
		auto& feedback = *resources->feedback;
		const std::uint64_t completed = feedback.timeline->Get().GetCompletedValue();
		// The completed slots, oldest first.
		std::vector<std::pair<std::uint64_t, std::size_t>> ready;
		for (std::size_t i = 0; i < feedback.slots.size(); ++i) {
			const auto& slot = *feedback.slots[i];
			if (slot.state.load(std::memory_order_acquire) == Resources::Feedback::Submitted && slot.fenceValue && slot.fenceValue <= completed)
				ready.emplace_back(slot.fenceValue, i);
		}
		std::sort(ready.begin(), ready.end());
		std::uint32_t decoded = 0;
		for (const auto& [value, index] : ready) {
			auto& slot = *feedback.slots[index];
			auto expected = static_cast<std::uint32_t>(Resources::Feedback::Submitted);
			if (!slot.state.compare_exchange_strong(expected, Resources::Feedback::Decoding, std::memory_order_acq_rel))
				continue;
			auto resource = slot.staging->GetAPIResource();
			void* mapped = nullptr;
			resource.Map(&mapped);
			if (mapped) {
				VisibilityFeedbackFrame frame{ slot.frame, slot.stamp, slot.objects, static_cast<const std::uint32_t*>(mapped), slot.tag };
				a_consume(frame);
				resource.Unmap(0, 0);
				++decoded;
			}
			slot.tag.reset();
			slot.state.store(Resources::Feedback::Free, std::memory_order_release);
		}
		feedback.statDecoded.fetch_add(decoded, std::memory_order_relaxed);
		return decoded;
	}

	IndirectDraws::FeedbackStats IndirectDraws::TakeFeedbackStats()
	{
		FeedbackStats out;
		if (const auto resources = impl->resources; resources && resources->feedback) {
			auto& feedback = *resources->feedback;
			out.armed = feedback.statArmed.exchange(0);
			out.dropped = feedback.statDropped.exchange(0);
			out.abandoned = feedback.statAbandoned.exchange(0);
			out.decoded = feedback.statDecoded.exchange(0);
		}
		return out;
	}

	void IndirectDraws::CaptureMainPass()
	{
		if (impl->pending)
			impl->pending->Release();
		impl->pending = CaptureBindings();
		impl->mainPassDepth = impl->pending->depth;
		impl->mainMinDepth = impl->pending->minDepth;
		impl->mainMaxDepth = impl->pending->maxDepth;
		impl->probeTargetCount = std::min<std::uint32_t>(impl->pending->targetCount, kColorTargets);
		for (std::uint32_t i = 0; i < impl->probeTargetCount; ++i)
			impl->probeTargets[i] = impl->pending->targets[i];
		impl->probeDepth = impl->pending->depth;
	}


	void IndirectDraws::CheckCapturePoint()
	{
		if (!impl->pending)
			return;
		const auto& captured = *impl->pending;
		auto now = CaptureBindings();
		// Only what the colour epoch takes from a capture: the frame registers, the frame textures, the targets, the
		// viewport and the eye (the per-draw registers and textures are the draw's own).
		auto& p = impl->captureParity;
		bool any = false;
		auto note = [&](std::string a_what) {
			++p.differ[std::move(a_what)];
			any = true;
		};
		for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
			if (!((kPerDrawVS >> slot) & 1) && captured.vsBuffers[slot] != now.vsBuffers[slot])
				note(fmt::format("VS b{}", slot));
			if (!((kPerDrawPS >> slot) & 1) && captured.psBuffers[slot] != now.psBuffers[slot])
				note(fmt::format("PS b{}", slot));
		}
		for (std::uint32_t t = kPixelTextureSlots; t < kTextureRegisters; ++t)
			if (captured.psViews[t] != now.psViews[t])
				note(fmt::format("t{}", t));
		for (std::uint32_t i = 0; i < kColorTargets; ++i)
			if (captured.targets[i] != now.targets[i])
				note(fmt::format("rt{}", i));
		if (captured.depth != now.depth)
			note("depth");
		if (captured.viewportWidth != now.viewportWidth || captured.viewportHeight != now.viewportHeight || captured.minDepth != now.minDepth ||
			captured.maxDepth != now.maxDepth)
			note("viewport");
		if (std::memcmp(&captured.eye, &now.eye, sizeof(captured.eye)) != 0 || std::memcmp(&captured.previousEye, &now.previousEye, sizeof(captured.previousEye)) != 0)
			note("eye");
		now.Release();
		p.frames += any ? 1u : 0u;
		if (++p.checks == 300) {
			std::string text;
			for (const auto& [what, count] : p.differ)
				text += fmt::format(" {}={}", what, count);
			logger::info("[DCLF] capture point parity: {} frames' captures against the first lighting draw's bindings, {} differ{}", p.checks, p.frames,
				p.frames ? " <- DIFFER:" + text : std::string(" <- OK"));
			p = {};
		}
	}

	void IndirectDraws::CaptureDepthPass()
	{
		// CS_DCLF_NO_ZPREPASS=1: leave the depth to the native pass, so the hybrid path runs a single epoch
		// per frame again. Its objects are then missing from the depth the rest of the frame reads, which is
		// only useful for telling a one-epoch frame apart from a two-epoch one.
		const bool skip = Toggles::Get().Active().noZPrepass;
		if (!Hybrid() || skip)
			return;
		// Main::RenderDepth sets the world depth target and viewport in the engine's shadow state,
		// but only a native draw flushes that state to D3D11. When ownership withheld all such draws,
		// RSGetViewports can still return the last shadow map's 4096x4096 viewport. Pairing that with
		// the main depth texture below records an out-of-bounds Vulkan render area and can lose the
		// device. Apply the pending world state at this hook, before the first-person camera replaces
		// it, exactly as BeforeOpaquePass does for the colour capture. This also applies pending clears
		// and preserves the engine's dynamic-resolution viewport instead of using the texture's extent.
		static REL::Relocation<void (*)(bool)> SetDirtyStates{ REL::RelocationID(75580, 77386) };
		SetDirtyStates(false);
		auto capture = CaptureBindings();
		// The engine's main depth, not whatever is bound: inside the depth pass Terrain Blending alternates the
		// bound target between it and its own terrain depth while terrain draws.
		if (auto* renderer = globals::game::renderer) {
			if (auto* main = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].texture) {
				capture.depth = nullptr;
				capture.depth.copy_from(main);
			}
		}
		// The Z-prepass writes into the depth the native pass just finished, which has to be the one the
		// main pass then tests against; otherwise DCLF's objects would be written somewhere nothing reads.
		if (!capture.depth || (impl->mainPassDepth && capture.depth.get() != impl->mainPassDepth.get())) {
			if (!impl->loggedDepthMismatch) {
				impl->loggedDepthMismatch = true;
				logger::warn("[DCLF] The depth pass and the main pass bind different depth textures; the Z-prepass stays with the main pass");
			}
			capture.Release();
			return;
		}
		if (impl->pending)
			impl->pending->Release();
		impl->pending = std::move(capture);
		impl->probeDepth = impl->pending->depth;
		ProbeTargets(kProbeFirstLabel);
		RunEpoch(RenderGraphRuntime::Segment::ZPrepass);
		ProbeTargets("after z-prepass");
	}

	void IndirectDraws::Execute()
	{
		if (Hybrid())
			return;  // the hybrid path runs the two segments separately
		RunEpoch(RenderGraphRuntime::Segment::MainOpaque);
	}

	void IndirectDraws::ProbeTargets(const char* a_label)
	{
		impl->ProbeGBuffer(a_label);
	}

	void IndirectDraws::ExecuteColour()
	{
		// The colour epoch assembles from the main pass's own capture: the Z-prepass ran off the depth
		// pass's, where the pixel-stage bindings were not available. Both draw the same tables with the
		// same camera, so the depths agree and the colour pass can test EQUAL.
		if (Hybrid())
			RunEpoch(RenderGraphRuntime::Segment::MainOpaque);
	}

	void IndirectDraws::RunEpoch(RenderGraphRuntime::Segment a_segment)
	{
		ZoneScopedN("CS.DCLF.RunEpoch");
		RenderGraphRuntime::EpochBodyScope body(a_segment);
		ScopedPerfEvent event(a_segment == RenderGraphRuntime::Segment::ZPrepass ? "CS DCLF: Z-prepass (CPU)" : "CS DCLF: main opaque (CPU)");
		const auto start = std::chrono::steady_clock::now();
		const bool depthOnly = a_segment == RenderGraphRuntime::Segment::ZPrepass;
		const std::size_t jobIndex = depthOnly ? kAsyncZPrepass : kAsyncColour;
		if (!impl->pending) {
			impl->DropMainJob(jobIndex, stats);
			return;
		}
		auto capture = std::move(*impl->pending);
		impl->pending.reset();
		auto& pipelines = DrawPipelines::Get();
		auto* lighting = ConstantEvaluator::Get().GetLightingShader();
		if (failed || !lighting || !pipelines.Enabled() || !GetIndirectState().valid) {
			capture.Release();
			impl->DropMainJob(jobIndex, stats);
			return;
		}
		bool ready = false;
		try {
			ready = impl->Setup(capture, depthOnly);
		} catch (const std::exception& e) {
			// Never retried: the feature stays on the native path.
			logger::error("[DCLF] Main-pass graph resources could not be created: {}", e.what());
			failed = true;
		}
		if (!ready) {
			capture.Release();
			++stats.notReady;
			impl->DropMainJob(jobIndex, stats);
			return;
		}

		auto& store = SceneStore::Get();
		const auto& tables = store.GetTables();
		auto resources = impl->resources;
		stats.skipped = {};
		stats.missingTextures = {};
		stats.missingVertexConstants = stats.missingPixelConstants = 0;

		// The per-frame constant blocks, from the capture's mirrors: render thread, before the epoch. Their
		// slot mask is an input of the build; their bytes are uploaded by the commit.
		FrameBlocks blocks;
		impl->PackFrameBlocks(capture, depthOnly, *resources, blocks);
		MainInputs in = impl->PrepareMainInputs(&capture, depthOnly, *resources, blocks.vsMask, blocks.psMask, store);
		auto& job = impl->mainJobs[jobIndex];
		job.vsMask = blocks.vsMask;
		job.psMask = blocks.psMask;
		job.masksKnown = true;
		auto& payload = impl->mainPayload[jobIndex];
		auto& async = stats.async[jobIndex];
		bool builtOnWorker = false;

		const bool ok = RenderGraphRuntime::Get().ExecuteEpoch(a_segment, [&](org::RenderGraph&) {
			// The descriptor entries the build reads, resolved now that the descriptor service is active.
			if (in.resolveTextures)
				GpuTextures::Get().BeginFrame(in.frameNumber);

			// The worker's build, if one was kicked for this epoch and it was built for exactly these inputs;
			// otherwise the build runs here. A late or stale job is dropped (its payload is the one this
			// build overwrites, so a late job is waited for before the inline build; counted). Joined before
			// the lookup refresh below: the worker reads the lookups until it is done.
			const auto joinStart = std::chrono::steady_clock::now();
			auto& worker = AsyncWorker::Get();
			AsyncWorker::WaitResult joined = AsyncWorker::WaitResult::None;
			if (job.handle) {
				joined = worker.Wait(job.handle, AsyncWaitBudget());
				if (joined == AsyncWorker::WaitResult::Late)
					worker.Cancel(job.handle);
			}
			const auto lookupsStart = std::chrono::steady_clock::now();
			stats.commitUs[0] += std::chrono::duration<double, std::micro>(lookupsStart - joinStart).count();
			auto& lookups = store.MutableLookups();
			RefreshMaterialLookups(tables, in.frameNumber, store.GetProjectedTextures(), lookups);
			in.lookupGeneration = lookups.generation;
			stats.commitUs[1] += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - lookupsStart).count();

			bool useAsync = false;
			if (job.handle) {
				switch (joined) {
				case AsyncWorker::WaitResult::Done:
					// A refresh that added or changed an entry the build read bumps the generation: stale.
					if (SameInputs(job.inputs, in)) {
						useAsync = true;
					} else {
						++async.stale;
						impl->LogStaleMainJob(jobIndex, in);
					}
					break;
				case AsyncWorker::WaitResult::Late:
					++async.late;
					break;
				case AsyncWorker::WaitResult::Failed:
					++async.failed;
					break;
				default:
					++async.cancelled;
					break;
				}
				// The Z-prepass job's eye is a prediction: counted apart from the other reasons for staleness.
				if (depthOnly && joined == AsyncWorker::WaitResult::Done) {
					const bool eyeDiffers = std::memcmp(&job.inputs.eye, &in.eye, sizeof(RE::NiPoint3)) != 0;
					const bool previousDiffers = std::memcmp(&job.inputs.previousEye, &in.previousEye, sizeof(RE::NiPoint3)) != 0;
					if (eyeDiffers || previousDiffers)
						++async.eyeMismatches;
					if (!eyeDiffers && previousDiffers)
						++async.previousEyeMismatches;
				}
				job.handle = {};
			}
			builtOnWorker = useAsync;
			if (useAsync) {
				++async.used;
				if (AsyncModeSetting() == AsyncMode::Probe) {
					// The same build on this thread, against the same inputs, compared byte for byte.
					BuildMainPayload(job.inputs, tables, lookups, impl->probePayload);
					++async.probeCompared;
					std::string difference;
					if (!SamePayload(payload, impl->probePayload, difference)) {
						if (async.probeDiffer++ == 0)
							logger::warn("[DCLF] async {} probe: the worker's build differs from the inline one: {}", depthOnly ? "zprepass" : "colour", difference);
					}
				}
			} else {
				++async.builtInline;
				BuildMainPayload(in, tables, lookups, payload, impl->CacheFor(jobIndex), impl->MainObjects(), impl->MainBones(), impl->MainGeometries());
			}
			impl->CommitMainPayload(capture, blocks, payload, resources, store, stats);
		});
		capture.Release();
		++stats.epochs;
		if (ok && BuildParityEnabled())
			impl->CheckBuildParity(resources, payload, stats);
		// Only the colour epoch's counters. Both epochs run BuildDraws into the same count buffer, and the
		// Z-prepass builds from the far smaller set the colour epoch drew last frame, so sampling whichever
		// ran most recently alternates between two unrelated populations.
		if (ok && !depthOnly)
			impl->ReadCullCounters(resources, stats, payload);
		if (ok && !depthOnly && SetParityEnabled())
			impl->CheckSetParity(resources, impl->mainPayload[1], payload);

		stats.cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		// The remainder is the epoch execution itself and the commit. It stays a subtraction, but it is now
		// a small one rather than the bucket the per-draw work hid in. When the worker built the payload, the
		// build's parts are the worker's time and the render thread's is all remainder (the join included).
		stats.partMs[7] = builtOnWorker ? stats.cpuMs :
		                                  stats.cpuMs - (stats.partMs[0] + stats.partMs[1] + stats.partMs[2] +
		                                                    stats.partMs[3] + stats.partMs[4] + stats.partMs[5] + stats.partMs[6]);
		if (!ok)
			logger::error("[DCLF] The main-pass epoch failed; the render graph is disabled");
	}

	void IndirectDraws::KickColourBuild()
	{
		// The colour epoch's inputs are final from here (RefreshFrameConstants was the frame's last writer of
		// the tables), and the epoch itself is ~1.5-2 ms of native rendering away: the build runs on the
		// worker in between, so the render thread only commits when the epoch comes. Only the hybrid path
		// replays the Z-prepass's eye and constants; without the replay the eye comes from a capture that
		// does not exist yet.
		impl->DropMainJob(kAsyncColour, stats);
		if (!AsyncJobEnabled("colour"))
			return;
		if (!impl->resources || !impl->resources->hybrid || !impl->prepassInputs) {
			++stats.async[kAsyncColour].notKicked;
			return;
		}
		// RefreshFrameMaterials has just written this frame's t11 into the character-lit records: the lookups
		// follow before the kick, or the epoch's own refresh would leave the job built against last frame's.
		auto& store = SceneStore::Get();
		RefreshKnownMaterialTextures(store, store.MutableLookups());
		impl->KickMainJob(false, nullptr, nullptr, stats);
	}

	void IndirectDraws::KickZPrepassBuild()
	{
		// The Z-prepass epoch's inputs are final from here to the Z-prepass in Main_RenderDepth - the accumulate
		// phase was the tables' last writer, RefreshFrameConstants runs after the epoch - except the eye,
		// which the epoch captures from posAdjust. posAdjust still holds the shadow cameras' here, so the eye
		// is predicted: the main camera's world position, and last frame's captured eye as the previous one.
		// The epoch compares both exactly with its capture.
		impl->DropMainJob(kAsyncZPrepass, stats);
		if (!AsyncJobEnabled("zprepass"))
			return;
		auto* camera = RE::Main::WorldRootCamera();
		if (!impl->resources || !impl->resources->hybrid || !impl->prepassInputs || !camera) {
			++stats.async[kAsyncZPrepass].notKicked;
			return;
		}
		const RE::NiPoint3 eye = camera->world.translate;
		const RE::NiPoint3 previousEye = impl->prepassEye;
		// The material lookups, as final as they can be made before the kick: a volatile material's textures
		// can change every frame (the character light's t11 alternates between two render targets), and
		// resolving them only in the epoch's own preparation bumped the lookups' generation under a job already
		// built against them - it went stale in 290 frames of 300.
		auto& store = SceneStore::Get();
		RefreshKnownMaterialTextures(store, store.MutableLookups());
		impl->KickMainJob(true, &eye, &previousEye, stats);
	}

	void IndirectDraws::Impl::KickMainJob(bool a_depthOnly, const RE::NiPoint3* a_eye, const RE::NiPoint3* a_previousEye, IndirectDraws::Stats& a_stats)
	{
		ZoneScopedN("CS.DCLF.KickMainJob");
		// Only the bindless records are per pair, which is what keeps the build a function of the tables.
		const std::size_t index = a_depthOnly ? kAsyncZPrepass : kAsyncColour;
		auto& job = mainJobs[index];
		auto& async = a_stats.async[index];
		auto* lighting = ConstantEvaluator::Get().GetLightingShader();
		if (!lighting || !DrawPipelines::Get().Enabled() || !GetIndirectState().valid || !job.masksKnown || !BindlessObjects() || !BindlessDraws()) {
			++async.notKicked;
			return;
		}
		auto& store = SceneStore::Get();
		job.inputs = PrepareMainInputs(nullptr, a_depthOnly, *resources, job.vsMask, job.psMask, store);
		// A bindless build without parity checks does not read the eye (PrepareMainInputs leaves it zero), so
		// the prediction only matters where it does.
		if (a_eye && BuildReadsEye(job.inputs))
			job.inputs.eye = *a_eye;
		if (a_previousEye && BuildReadsEye(job.inputs))
			job.inputs.previousEye = *a_previousEye;
		++async.kicked;
		const auto* tables = &store.GetTables();
		const auto* lookups = &store.GetLookups();
		auto* payload = &mainPayload[index];
		auto* cache = CacheFor(index);
		auto* objects = MainObjects();
		auto* bonesStore = MainBones();
		auto* geometriesStore = MainGeometries();
		auto* pool = &stagedPools[index];
		const MainInputs inputs = job.inputs;
		job.handle = AsyncWorker::Get().Submit(a_depthOnly ? "zprepass" : "colour", [inputs, tables, lookups, payload, cache, objects, bonesStore, geometriesStore, pool, target = resources](std::stop_token) {
			BuildMainPayload(inputs, *tables, *lookups, *payload, cache, objects, bonesStore, geometriesStore);
			if (target)
				StageMainPayload(*payload, *target, *pool);
		});
	}

	void IndirectDraws::EndFrame()
	{
		// A job kicked this frame and never joined (the epoch did not run: a load screen, a failed setup) must
		// not outlive the frame: its inputs name this frame's tables.
		for (std::size_t j = 0; j < impl->mainJobs.size(); ++j) {
			if (impl->mainJobs[j].handle) {
				++stats.async[j].leaked;
				impl->DropMainJob(j, stats);
			}
		}
		if (impl->shadowJob.handle) {
			++stats.async[kAsyncShadow].leaked;
			impl->DropShadowJob(stats);
		}
	}

	void IndirectDraws::DrainAsync()
	{
		for (std::size_t j = 0; j < impl->mainJobs.size(); ++j)
			impl->DropMainJob(j, stats);
		impl->DropShadowJob(stats);
		AsyncWorker::Get().Drain();
	}

	void IndirectDraws::Impl::DropMainJob(std::size_t a_job, IndirectDraws::Stats& a_stats)
	{
		auto& job = mainJobs[a_job];
		if (!job.handle)
			return;
		AsyncWorker::Get().Cancel(job.handle);
		++a_stats.async[a_job].dropped;
		job.handle = {};
	}

	void IndirectDraws::Impl::LogStaleMainJob(std::size_t a_job, const MainInputs& a_actual)
	{
		auto& job = mainJobs[a_job];
		if (job.loggedStale++ >= 4)
			return;
		const auto& k = job.inputs;
		logger::info("[DCLF] async {}: the job's inputs are stale (frame {} vs {}, VS mask {:#x} vs {:#x}, PS mask {:#x} vs {:#x}, eye ({:.3f} {:.3f} {:.3f}) vs ({:.3f} {:.3f} {:.3f}), previous eye ({:.3f} {:.3f} {:.3f}) vs ({:.3f} {:.3f} {:.3f}), render flags {:#x} vs {:#x}, drew last frame {}, tables {} vs {}, lookups {} vs {}, resources {})",
			a_job == kAsyncZPrepass ? "zprepass" : "colour",
			k.frameNumber, a_actual.frameNumber, k.vsFrameMask, a_actual.vsFrameMask, k.psFrameMask, a_actual.psFrameMask,
			k.eye.x, k.eye.y, k.eye.z, a_actual.eye.x, a_actual.eye.y, a_actual.eye.z,
			k.previousEye.x, k.previousEye.y, k.previousEye.z, a_actual.previousEye.x, a_actual.previousEye.y, a_actual.previousEye.z,
			k.renderFlags, a_actual.renderFlags, k.drawnCommitted == a_actual.drawnCommitted ? "same" : "differs",
			k.tablesGeneration, a_actual.tablesGeneration, k.lookupGeneration, a_actual.lookupGeneration,
			k.addresses == a_actual.addresses ? "same" : "changed");
	}

	void IndirectDraws::Impl::PackFrameBlocks(const Capture& a_capture, bool a_depthOnly, const Resources& a_resources, FrameBlocks& a_out)
	{
		// Per-frame constant buffers: whatever the main pass binds outside the per-draw slots.
		// On the Z-prepass the pixel-stage per-frame bindings are skipped entirely: the native depth pass
		// has not bound the main pass's yet, and the DCLF_DEPTH_ONLY build of the pixel stage compiles
		// away everything that would read them.
		auto& mirror = ConstantMirror::Get();
		// On the hybrid path the colour epoch replays the vertex-stage bytes the Z-prepass used, so the
		// two agree to the bit and the colour pass's EQUAL test passes.
		const bool replayVertexInputs = !a_depthOnly && a_resources.hybrid && prepassInputs;
		if (a_depthOnly)
			for (auto& bytes : prepassVS)
				bytes.clear();
		for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
			if (replayVertexInputs && !((kPerDrawVS >> slot) & 1)) {
				const auto& bytes = prepassVS[slot];
				if (!bytes.empty())
					a_out.vs[slot].assign(bytes.begin(), bytes.end());
			}
			for (auto [buffers, perDraw, out] : { std::tuple{ &a_capture.vsBuffers, kPerDrawVS, &a_out.vs }, std::tuple{ &a_capture.psBuffers, kPerDrawPS, &a_out.ps } }) {
				if (a_depthOnly && out == &a_out.ps)
					continue;
				if (replayVertexInputs && out == &a_out.vs)
					continue;
				auto* buffer = (*buffers)[slot];
				if (!buffer || ((perDraw >> slot) & 1))
					continue;
				mirror.Watch(buffer);
				const auto contents = mirror.Contents(buffer);
				if (!contents.empty())
					(*out)[slot].assign(contents.begin(), contents.end());
				if (a_depthOnly && out == &a_out.vs && !contents.empty())
					prepassVS[slot].assign(contents.begin(), contents.end());
			}
		}

		// b12 is the game's PerFrame buffer. The constant mirror does not always hold its contents - the
		// engine does not rewrite it through the hooked context every frame - and an empty slot drops
		// every object whose shaders read it, which left distant buildings unshaded. Community Shaders
		// already caches this buffer between Map and Unmap for its own use, so take it from there.
		{
			const auto& cached = globals::game::frameBufferCached.data;
			const auto* bytes = reinterpret_cast<const std::byte*>(&cached);
			if (a_out.vs[kPerFrameVertexRegister].empty())
				a_out.vs[kPerFrameVertexRegister].assign(bytes, bytes + sizeof(cached));
			const bool psFromMirror = !a_out.ps[kPerFrameVertexRegister].empty();
			if (a_out.ps[kPerFrameVertexRegister].empty())
				a_out.ps[kPerFrameVertexRegister].assign(bytes, bytes + sizeof(cached));
			// [TEMP] The colour epoch's PerFrame blocks against the engine's current one (CS's cache).
			static std::uint32_t tempEpochs = 0;
			if (!a_depthOnly && (tempEpochs++ % 240) == 0) {
				auto describe = [&](const std::vector<std::byte>& a_block) {
					if (a_block.size() < 164 * sizeof(float))
						return fmt::format("{} bytes", a_block.size());
					const auto* f = reinterpret_cast<const float*>(a_block.data());
					const auto* c = reinterpret_cast<const float*>(bytes);
					std::uint32_t differ = 0, first = ~0u;
					for (std::uint32_t i = 0; i < std::min<std::size_t>(a_block.size() / 4, sizeof(cached) / 4); ++i)
						if (std::memcmp(&f[i], &c[i], 4) != 0) {
							++differ;
							first = std::min(first, i);
						}
					return fmt::format("posAdjust ({:.2f} {:.2f} {:.2f}), {} floats differ from the cache (first c{}.{})", f[160], f[161], f[162], differ, first / 4, first % 4);
				};
				const auto* c = reinterpret_cast<const float*>(bytes);
				logger::info("[TEMP] colour PerFrame: cache posAdjust ({:.2f} {:.2f} {:.2f}); VS {} [{}]; PS {} [{}]", c[160], c[161], c[162],
					describe(a_out.vs[kPerFrameVertexRegister]), replayVertexInputs ? "replayed" : "mirror",
					describe(a_out.ps[kPerFrameVertexRegister]), psFromMirror ? "mirror" : "cache");
			}
		}

		// b5 is Community Shaders' own SharedData, written through its ConstantBuffer helper rather than
		// through the hooked device context, so the constant mirror never observes the write and the
		// slot would stay empty - which dropped every object whose shader reads it (the untextured
		// architecture in Dragonsreach). CS keeps the struct it uploaded, so pack that directly.
		// These are supplied in the Z-prepass too. Its pixel stage is the DCLF_DEPTH_ONLY build,
		// which still reads them in the code that runs before the alpha test, and they all come from
		// Community Shaders or the game's own cache rather than from the main pass's bindings - so they
		// are available during the native depth pass, where the main pass's bindings are not.
		if (auto* state = globals::state) {
			const auto* shared = reinterpret_cast<const std::byte*>(&state->lastSharedData);
			a_out.ps[kSharedDataRegister].assign(shared, shared + sizeof(State::SharedDataCB));
			if (!state->lastFeatureData.empty()) {
				const auto* feature = reinterpret_cast<const std::byte*>(state->lastFeatureData.data());
				a_out.ps[kFeatureDataRegister].assign(feature, feature + state->lastFeatureData.size());
			}
		}
		for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
			if (!a_out.vs[slot].empty())
				a_out.vsMask |= 1u << slot;
			if (!a_out.ps[slot].empty())
				a_out.psMask |= 1u << slot;
		}

		// One line saying, per pixel register, whether the pass had a buffer bound and whether the
		// constant mirror could supply its contents: a slot that is bound but not mirrored ends up as a
		// null address and drops every object whose shader reads it.
		if (!a_depthOnly && !loggedFrameConstants) {
			loggedFrameConstants = true;
			std::string text;
			for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
				if ((kPerDrawPS >> slot) & 1)
					continue;
				text += fmt::format("{}b{}={}/{}", text.empty() ? "" : " ", slot, a_capture.psBuffers[slot] ? "bound" : "unbound",
					((a_out.psMask >> slot) & 1) ? "supplied" : "empty");
			}
			logger::info("[DCLF] main-pass per-frame pixel constants: {}", text);
		}
	}

	MainInputs IndirectDraws::Impl::PrepareMainInputs(const Capture* a_capture, bool a_depthOnly, const Resources& a_resources, std::uint32_t a_vsMask, std::uint32_t a_psMask, const SceneStore& a_store)
	{
		// CS_DCLF_NO_PREPASS_TEXTURES=1: the Z-prepass epoch does not touch the texture system at all, to
		// tell apart a colour epoch whose descriptors are its own from one that inherits slots the depth
		// epoch allocated earlier in the same frame.
		static const bool bareDepthTextures = SwitchEnabled("CS_DCLF_NO_PREPASS_TEXTURES");
		static const bool dedupParity = SwitchEnabled("CS_DCLF_DEDUP_PARITY");
		static const bool bindlessParity = SwitchEnabled("CS_DCLF_BINDLESS_PARITY");
		MainInputs in;
		in.frameNumber = a_store.GetFrame();
		in.depthOnly = a_depthOnly;
		in.hybrid = a_resources.hybrid;
		in.residentUploaded = a_resources.residentUploaded[a_depthOnly && a_resources.inputsDepth ? 0 : 1];
		in.tablesHeld = a_resources.tablesHeld;
		in.resolveTextures = !a_depthOnly || !bareDepthTextures;
		in.bindless = BindlessObjects();
		in.bindlessDraws = BindlessDraws();
		in.dedupParity = dedupParity;
		in.bindlessParity = bindlessParity;
		in.withholding = PassCapture::WithholdingEnabled();
		in.requireNativeVisible = RequireNativeVisible();
		in.linearLighting = globals::features::linearLighting.loaded && globals::features::linearLighting.settings.enableLinearLighting;
		in.renderFlags = a_store.GetMainPassRenderFlags();
		// Camera-relative world matrices: the colour epoch must use the eye the Z-prepass used, or the
		// same vertex lands somewhere else and the EQUAL test rejects it. Without a capture (the job kicked
		// ahead of the epoch) the replay is the only source, which KickColourBuild requires.
		const bool replayVertexInputs = !a_depthOnly && a_resources.hybrid && prepassInputs;
		if (replayVertexInputs || !a_capture) {
			in.eye = prepassEye;
			in.previousEye = prepassPreviousEye;
		} else {
			in.eye = a_capture->eye;
			in.previousEye = a_capture->previousEye;
		}
		// The records are absolute and the shaders subtract the eye, so a bindless build reads it only for its
		// parity checks against the constant-group form, which is relative. Where it is not read it is not an
		// input: the Z-prepass job needs no eye prediction, and nothing goes stale on it.
		if (!BuildReadsEye(in))
			in.eye = in.previousEye = {};
		in.vsFrameMask = a_vsMask;
		in.psFrameMask = a_psMask;
		// Each segment's constants and records are its own (the Z-prepass's in Resources::constantsDepth, recordsDepth).
		const bool depthBuffers = a_depthOnly && a_resources.recordsDepth;
		in.addresses.constants = depthBuffers ? a_resources.constantsDepthAddress : a_resources.constantsAddress;
		in.addresses.records = depthBuffers ? a_resources.recordsDepthAddress : a_resources.recordsAddress;
		in.constantsUploaded = a_resources.constantsUploaded[a_depthOnly ? 0 : 1];
		in.recordsUploaded = a_resources.recordsUploaded[a_depthOnly ? 0 : 1];
		in.frameTextures = a_resources.committedFrameTextures[a_depthOnly ? 0 : 1];
		in.addresses.frameConstants = a_resources.frameConstantsAddress;
		in.addresses.objectsIndex = a_resources.objectsIndex;
		in.addresses.bonesIndex = a_resources.bonesIndex;
		in.addresses.facePositions = FaceSnapshots::Enabled() ? a_resources.facePositionsAddress : 0;
		in.addresses.recordCapacity = a_resources.recordCapacity;
		in.addresses.identity = &a_resources;
		in.tablesGeneration = a_store.GetTablesGeneration();
		in.lookupGeneration = a_store.GetLookups().generation;
		in.materialPatchedFloats = a_store.GetMaterialPatchedFloats();
		in.materialPatchedVSFloats = a_store.GetMaterialPatchedVSFloats();
		// The Z-prepass's gate without withholding reads the colour epoch's drawn state, which only a colour commit writes
		// (none runs between here and the Z-prepass); the colour build sends its drawn changes relative to what is applied.
		in.drawnSlots = &slotDrawn;
		in.drawnCommitted = drawnCommitted;
		in.drawnResync = drawnResync;
		return in;
	}

	std::string IndirectDraws::AsyncReport()
	{
		std::string text = AsyncWorker::Get().Report();
		for (std::size_t i = 0; i < impl->buildCaches.size(); ++i) {
			auto& cache = impl->buildCaches[i];
			if (!(cache.pairHits + cache.pairMisses + cache.pipelineHits + cache.pipelineMisses))
				continue;
			text += fmt::format("[DCLF] build cache ({}): pairs {} reused, {} rebuilt; pipelines {} reused, {} rebuilt; {} pairs and {} pipelines held\n",
				i == kAsyncZPrepass ? "zprepass" : "colour", cache.pairHits, cache.pairMisses, cache.pipelineHits, cache.pipelineMisses, cache.pairs.size(),
				cache.pipelines.size());
			cache.pairHits = cache.pairMisses = cache.pipelineHits = cache.pipelineMisses = 0;
			if (const double n = static_cast<double>(cache.persistentBuilds)) {
				const auto& kept = cache.persistent;
				text += fmt::format("[DCLF] persistent bindings ({}): {} builds; a build: {:.1f} pipelines and {:.1f} pairs clean, {:.1f} and {:.1f} looked at, {:.1f} blocks and {:.1f} records written; {} records held, {:.0f} KB of blocks, {} resets; parity {} draws checked, {} differ{}{}\n",
					i == kAsyncZPrepass ? "zprepass" : "colour", cache.persistentBuilds, cache.persistentCleanPipelines / n, cache.persistentCleanPairs / n,
					cache.persistentDirtyPipelines / n, cache.persistentDirtyPairs / n, cache.persistentBlocks / n, cache.persistentRecords / n,
					kept.records.Size() - kept.recordFree.size(), kept.constants.Size() / 1024.0, cache.persistentResets, cache.persistentParityChecks,
					cache.persistentParityMismatches, cache.persistentParityChecks ? (cache.persistentParityMismatches ? " <- DIFFER; first: " : " <- OK") : "",
					cache.persistentParityFirst);
				cache.persistentBuilds = cache.persistentBlocks = cache.persistentRecords = cache.persistentResets = 0;
				cache.persistentCleanPipelines = cache.persistentCleanPairs = cache.persistentDirtyPipelines = cache.persistentDirtyPairs = 0;
				cache.persistentParityChecks = cache.persistentParityMismatches = 0;
				cache.persistentParityFirst.clear();
			}
		}
		for (auto [name, store] : { std::pair{ "main", &impl->mainObjects }, std::pair{ "shadow", &impl->shadowObjects } }) {
			if (!store->updates)
				continue;
			text += fmt::format("[DCLF] persistent object records ({}): {} updates, {:.1f} records rewritten an update, {} records held, {} resyncs, {} collisions; parity {} checked, {} differ{}\n",
				name, store->updates, static_cast<double>(store->rewritten) / store->updates, store->records.Size(), store->resyncs, store->collisions, store->parity.checks,
				store->parity.mismatches, store->collisions && store->parity.checks ? std::string(" <- DIFFER") : store->parity.Verdict(true));
			store->updates = store->rewritten = store->resyncs = store->collisions = 0;
			store->parity.Reset();
		}
		if (auto& k = impl->shadowKept; k.builds) {
			std::size_t entries = 0;
			for (const auto& mode : k.modes)
				entries += mode.inputs.Size();
			text += fmt::format("[DCLF] persistent shadow state: {} builds, {:.1f} entries and {:.1f} records written a build, {} entries and {} records held, {} resyncs; parity {} inputs checked, {} differ{}{}\n",
				k.builds, static_cast<double>(k.entriesWritten) / k.builds, static_cast<double>(k.recordsWritten) / k.builds, entries, k.records.Size(), k.resyncs,
				k.parity.checks, k.parity.mismatches, k.parity.Verdict(true), "");
			k.builds = k.entriesWritten = k.recordsWritten = k.resyncs = 0;
			k.parity.Reset();
		}
		for (auto [name, store] : { std::pair{ "main", &impl->mainBones }, std::pair{ "shadow", &impl->shadowBones } }) {
			if (!store->updates)
				continue;
			text += fmt::format("[DCLF] persistent bone rows ({}): {} updates, {} resyncs, {} capacity rows; parity {} checked, {} differ{}\n", name, store->updates, store->resyncs,
				store->capacity, store->parity.checks, store->parity.mismatches, store->parity.Verdict());
			store->updates = store->resyncs = 0;
			store->parity.Reset();
		}
		for (auto [name, store] : { std::pair{ "main", &impl->mainGeometries }, std::pair{ "shadow", &impl->shadowGeometries } }) {
			if (!store->updates)
				continue;
			text += fmt::format("[DCLF] persistent geometry table ({}): {} updates, {:.2f} slots repacked an update, {} slots held, {} resyncs; parity {} checked, {} differ{}\n", name,
				store->updates, static_cast<double>(store->rewritten) / store->updates, store->packed.Size(), store->resyncs, store->parity.checks, store->parity.mismatches,
				store->parity.Verdict(true));
			store->updates = store->rewritten = store->resyncs = 0;
			store->parity.Reset();
		}
		if (auto& c = impl->sunExclusionCache; c.builds) {
			text += fmt::format("[DCLF] sun exclusion: {} builds, {} reused; parity {} checked, {} differ{}\n", c.builds, c.reused, c.parity.checks, c.parity.mismatches,
				c.parity.Verdict());
			c.builds = c.reused = 0;
			c.parity.Reset();
		}
		static constexpr const char* kNames[3] = { "colour", "zprepass", "shadow" };
		for (std::size_t i = 0; i < stats.async.size(); ++i) {
			auto& a = stats.async[i];
			if (!a.kicked && !a.notKicked && !a.builtInline && !a.leaked)
				continue;
			text += fmt::format("[DCLF] async {} epochs: {} used the worker's build, {} built inline ({} not kicked, {} stale, {} late, {} failed, {} cancelled), {} dropped, {} leaked; probe: {} compared, {} differ\n",
				kNames[i], a.used, a.builtInline, a.notKicked, a.stale, a.late, a.failed, a.cancelled, a.dropped, a.leaked, a.probeCompared, a.probeDiffer);
			if (i == kAsyncZPrepass && a.kicked)
				text += fmt::format("[DCLF] async zprepass eye: the predicted eye pair missed the captured one {} times ({} the previous eye only)\n", a.eyeMismatches, a.previousEyeMismatches);
			a = {};
		}
		AsyncWorker::Get().ResetStats();
		return text;
	}

	namespace
	{
		// The payload's own uploads: the constant arena, the per-object records and bone rows, the binding records,
		// and the draw inputs with their geometry. Three buffers, three conditions for the last ones: they used to
		// share one, and a depth epoch where every candidate is cull-only has no records and plenty of inputs, so
		// BuildDraws dispatched inputCount work items over whatever the previous epoch left in the buffer.
		template <class Emit>
		void ForEachMainPayloadUpload(const MainPayload& a_payload, const Resources& a_resources, Emit&& a_emit)
		{
			// Each segment's constants and records are its own buffers (the Z-prepass's: Resources::constantsDepth, recordsDepth).
			const bool depthBuffers = a_payload.inputs.depthOnly && a_resources.recordsDepth;
			const auto& constantsTarget = depthBuffers ? a_resources.constantsDepth : a_resources.constants;
			const auto& recordsTarget = depthBuffers ? a_resources.recordsDepth : a_resources.records;
			const std::size_t segment = a_payload.inputs.depthOnly ? 0 : 1;
			const auto& bytes = a_payload.arena.Bytes();
			if (!bytes.empty())
				a_emit(constantsTarget, bytes.data(), bytes.size(), false, 0);
			if (a_payload.persistent) {
				ZoneScopedN("CS.DCLF.UploadRanges.Bindings");
				// The kept blocks and records: what changed since the version the buffers hold, else all of them.
				a_payload.keptConstants.Emit(a_resources.constantsUploaded[segment],
					[&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) { a_emit(constantsTarget, a_data, a_bytes, false, a_offset); });
				a_payload.keptRecords.Emit(a_resources.recordsUploaded[segment],
					[&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) { a_emit(recordsTarget, a_data, a_bytes, false, a_offset); });
			}
			TracyCZoneN(objectUploadZone, "CS.DCLF.UploadRanges.Objects", true);
			a_payload.objectRecords.Emit(a_resources.tablesHeld.objects, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
				a_emit(a_resources.objects, a_data, a_bytes, false, a_offset);
			});
			TracyCZoneEnd(objectUploadZone);
			if (a_resources.bones)
				EmitBones(a_payload.bones, a_resources.tablesHeld.bones, nullptr, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
					a_emit(a_resources.bones, a_data, a_bytes, false, a_offset);
				});
			if (!a_payload.records.empty())
				a_emit(recordsTarget, a_payload.records.data(), a_payload.records.size() * sizeof(DrawBindings), true, 0);
			// The segment's input buffer: the resident region at its head when the buffer does not hold this version of it,
			// then the frame's own inputs after it.
			const bool depth = a_payload.inputs.depthOnly && a_resources.inputsDepth;
			const auto& inputs = depth ? a_resources.inputsDepth : a_resources.inputs;
			const std::size_t regionCount = a_payload.resident.Count();
			TracyCZoneN(residentUploadZone, "CS.DCLF.UploadRanges.Resident", true);
			a_payload.resident.Emit(a_resources.residentUploaded[depth ? 0 : 1],
				[&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) { a_emit(inputs, a_data, a_bytes, false, a_offset); });
			if (!a_payload.inputList.empty())
				a_emit(inputs, a_payload.inputList.data(), a_payload.inputList.size() * sizeof(DrawInput), false, regionCount * sizeof(DrawInput));
			TracyCZoneEnd(residentUploadZone);
			EmitGeometryDraws(a_payload.geometryDraws, a_resources.tablesHeld.geometries, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
				a_emit(a_resources.geometries, a_data, a_bytes, false, a_offset);
			});
		}

		// Render thread: the payload through the commit's uploads, copied now.
		void UploadMainPayload(const MainPayload& a_payload, const Resources& a_resources, CommitUploads& a_uploads)
		{
			ForEachMainPayloadUpload(a_payload, a_resources, [&](const auto& a_target, const void* a_data, std::size_t a_bytes, bool, std::size_t a_offset) {
				a_uploads(a_target, a_data, a_bytes, a_offset);
			});
		}
	}

	// On the worker, after the build: the payload's uploads into a staged batch (a released one from the job's
	// pool, or a new one), so the commit copies nothing. The records' staging is kept for the commit's patches.
	namespace
	{
		// A released batch from a job's pool (neither a payload nor the upload service still holds it), or a new one.
		std::shared_ptr<org::runtime::StagedUploadBatch> AcquireStagedBatch(std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& a_pool)
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
				batch = org::runtime::StagedUploadBatch::Create();
				a_pool.push_back(batch);
			}
			batch->Reset();
			return batch;
		}
	}

	void StageMainPayload(MainPayload& a_payload, const Resources& a_resources, std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& a_pool)
	{
		ZoneScopedN("CS.DCLF.StageMainPayload");
		auto batch = AcquireStagedBatch(a_pool);
		ForEachMainPayloadUpload(a_payload, a_resources, [&](const auto& a_target, const void* a_data, std::size_t a_bytes, bool a_records, std::size_t a_offset) {
			auto* staging = batch->Stage(org::runtime::UploadTarget::FromShared(a_target), a_offset, a_data, a_bytes);
			if (a_records)
				a_payload.stagedRecords = reinterpret_cast<DrawBindings*>(staging);
		});
		a_payload.stagedFor = &a_resources;
		a_payload.staged = std::move(batch);
	}

	// On the worker, after the shadow build: what the commit would upload that does not depend on the views it
	// captures - the shared tables, the used modes' inputs, the arena past its view head - and, per view slot the
	// job expects, the records naming that slot's blocks and its zeroed counters. The commit uploads the view
	// head and any slot past a_slots itself.
	void StageShadowPayload(ShadowPayload& a_payload, const ShadowResources& a_resources, std::uint32_t a_slots,
		std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& a_pool)
	{
		ZoneScopedN("CS.DCLF.StageShadowPayload");
		using org::runtime::UploadTarget;
		auto batch = AcquireStagedBatch(a_pool);
		a_payload.objects.Emit(a_resources.tablesHeld.objects, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
			batch->Stage(UploadTarget::FromShared(a_resources.objects), a_offset, a_data, a_bytes);
		});
		EmitBones(a_payload.bones, a_resources.tablesHeld.bones, nullptr, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
			batch->Stage(UploadTarget::FromShared(a_resources.bones), a_offset, a_data, a_bytes);
		});
		EmitGeometryDraws(a_payload.geometries, a_resources.tablesHeld.geometries, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
			batch->Stage(UploadTarget::FromShared(a_resources.geometries), a_offset, a_data, a_bytes);
		});
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
			if (!a_payload.inputs.modeUsed[m])
				continue;
			EmitShadowInputs(a_payload, m, a_payload.kept ? a_resources.inputsUploaded[m] : 0, [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
				batch->Stage(UploadTarget::FromShared(a_resources.inputs[m]), a_offset, a_data, a_bytes);
			});
		}
		const auto& bytes = a_payload.arena.Bytes();
		if (bytes.size() > kShadowMaterialBlocksOffset)
			batch->Stage(UploadTarget::FromShared(a_resources.constants), kShadowMaterialBlocksOffset, bytes.data() + kShadowMaterialBlocksOffset,
				bytes.size() - kShadowMaterialBlocksOffset);
		const std::uint32_t slots = std::min<std::uint32_t>(a_slots, kMaxShadowViews);
		const std::uint64_t base = a_payload.inputs.addresses.constants;
		static const std::uint32_t zero[kCountWords] = {};
		for (std::uint32_t slot = 0; slot < slots; ++slot) {
			const std::uint64_t viewBlockOffset = std::uint64_t(slot) * kShadowViewSlotBytes;
			const std::uint64_t perFrameOffset = viewBlockOffset + kShadowPerFrameOffset;
			const std::uint64_t recordsOffset = std::uint64_t(slot) * kShadowRecordCapacity * sizeof(DrawBindings);
			if (a_payload.kept) {
				// The kept records: what the slot does not hold yet.
				EmitShadowRecords(a_payload, a_resources.recordsUploaded[slot], base + viewBlockOffset, base + perFrameOffset,
					[&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
						batch->Stage(UploadTarget::FromShared(a_resources.records), recordsOffset + a_offset, a_data, a_bytes);
					});
			} else if (auto* staging = batch->Stage(UploadTarget::FromShared(a_resources.records), recordsOffset, a_payload.records.size() * sizeof(DrawBindings))) {
				// Whole records into write-combined staging, in order: written, never read.
				for (std::size_t r = 0; r < a_payload.records.size(); ++r) {
					DrawBindings record = a_payload.records[r];
					record.vertexConstants[0] = base + viewBlockOffset;
					record.pixelConstants[0] = base + viewBlockOffset;
					record.vertexConstants[kPerFrameVertexRegister] = base + perFrameOffset;
					record.pixelConstants[kPerFrameVertexRegister] = base + perFrameOffset;
					std::memcpy(staging + r * sizeof(DrawBindings), &record, sizeof(DrawBindings));
				}
			}
			batch->Stage(UploadTarget::FromShared(a_resources.count[slot]), 0, zero, sizeof(zero));
		}
		a_payload.stagedSlots = slots;
		a_payload.stagedFor = &a_resources;
		a_payload.staged = std::move(batch);
	}

	bool IndirectDraws::Impl::CommitMainPayload(const Capture& a_capture, const FrameBlocks& a_blocks, MainPayload& a_payload,
		const std::shared_ptr<Resources>& a_resources, SceneStore& a_store, IndirectDraws::Stats& a_stats)
	{
		ZoneScopedN("CS.DCLF.CommitMainPayload");
		const auto& in = a_payload.inputs;
		const auto& tables = a_store.GetTables();
		const bool depthOnly = in.depthOnly;
		const std::uint32_t frameNumber = in.frameNumber;
		auto& textures = GpuTextures::Get();
		auto& mirror = ConstantMirror::Get();
		const bool replayVertexInputs = !depthOnly && a_resources->hybrid && prepassInputs;
		CommitUploads uploads(commitStagedPool);
		auto lap = [&, last = std::chrono::steady_clock::now()](std::size_t a_part) mutable {
			const auto now = std::chrono::steady_clock::now();
			a_stats.commitUs[a_part] += std::chrono::duration<double, std::micro>(now - last).count();
			last = now;
		};
		// Nothing to draw until this commit publishes the segment's shape again (a failed commit leaves it so).
		const std::size_t shapeIndex = depthOnly ? kDepthShape : kColourShape;
		a_resources->frames[shapeIndex].store(nullptr, std::memory_order_release);

		// The frame's textures (t16 and up), resolved now and patched into the records that read them.
		std::array<std::uint32_t, kTextureRegisters> frameTextures;
		frameTextures.fill(kInvalidIndex);
		for (std::uint32_t t = kPixelTextureSlots; t < kTextureRegisters && !depthOnly; ++t)
			frameTextures[t] = textures.Resolve(a_capture.psViews[t]);
		if (a_resources->lightLimitFix && !depthOnly)
			ORGLightCulling::Get().GetShaderResourceIndices(frameTextures[kLightsRegister], frameTextures[kLightsRegister + 1], frameTextures[kLightsRegister + 2]);
		for (const auto& frameBuffer : depthOnly ? decltype(a_resources->frameBuffers){} : a_resources->frameBuffers) {
			auto* buffer = BufferOf(a_capture.psViews[frameBuffer.textureRegister]);
			mirror.Watch(buffer);
			const auto contents = mirror.Contents(buffer);
			const std::size_t offset = std::size_t(frameBuffer.firstElement) * frameBuffer.stride;
			const std::size_t bytes = std::size_t(frameBuffer.elements) * frameBuffer.stride;
			if (contents.size() < offset + bytes) {
				// [TEMP] Which structured buffers the commit leaves unfilled (their copy then reads zero).
				static std::array<std::uint32_t, kTextureRegisters> unfilled{};
				if ((unfilled[frameBuffer.textureRegister]++ % 600) == 0)
					logger::warn("[TEMP] frame buffer t{} ({} elements of {} bytes) not filled: the mirror holds {} bytes of it ({} times)", frameBuffer.textureRegister,
						frameBuffer.elements, frameBuffer.stride, contents.size(), unfilled[frameBuffer.textureRegister]);
				continue;  // not written since it is watched
			}
			uploads(frameBuffer.copy, contents.data() + offset, bytes, 0);
			frameTextures[frameBuffer.textureRegister] = frameBuffer.copy->GetSRVInfo(0).slot.index;
		}
		std::uint32_t frameTexturesMissing = 0;
		std::array<std::uint64_t, 2> missingRegisters{};
		for (const auto& [record, t] : a_payload.framePatches) {
			const std::uint32_t index = frameTextures[t];
			// A frame texture the pipeline reads but the pass did not bind reads zero, as an unbound view does
			// natively; counted, because the build could not skip the draw for it. A view bound natively that
			// could not be resolved (a buffer that is not a CPU-written structured buffer) lands here too, and
			// reads zero where the native draw reads the resource.
			if (index == kInvalidIndex) {
				++frameTexturesMissing;
				missingRegisters[(t >> 6) & 1] |= 1ull << (t & 63);
				static std::array<bool, kTextureRegisters> described{};
				if (t < kTextureRegisters && !std::exchange(described[t], true)) {
					if (auto* view = a_capture.psViews[t]) {
						D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
						view->GetDesc(&viewDesc);
						winrt::com_ptr<ID3D11Resource> resource;
						view->GetResource(resource.put());
						std::string what = fmt::format("view dimension {}, format {}", static_cast<std::uint32_t>(viewDesc.ViewDimension), static_cast<std::uint32_t>(viewDesc.Format));
						if (auto buffer = resource.try_as<ID3D11Buffer>()) {
							D3D11_BUFFER_DESC bufferDesc{};
							buffer->GetDesc(&bufferDesc);
							what += fmt::format(", buffer of {} bytes, stride {}, usage {}, CPU access {:X}, bind {:X}, misc {:X}", bufferDesc.ByteWidth, bufferDesc.StructureByteStride,
								static_cast<std::uint32_t>(bufferDesc.Usage), bufferDesc.CPUAccessFlags, bufferDesc.BindFlags, bufferDesc.MiscFlags);
						}
						char name[128]{};
						UINT size = sizeof(name) - 1;
						if (SUCCEEDED(resource->GetPrivateData(WKPDID_D3DDebugObjectName, &size, name)))
							what += fmt::format(", '{}'", name);
						logger::warn("[DCLF] frame texture t{} is bound natively but not resolved: {}", t, what);
					} else {
						logger::warn("[DCLF] frame texture t{} is read but nothing is bound there at the capture", t);
					}
				}
			}
			const std::uint32_t value = index == kInvalidIndex ? (in.resolveTextures && textures.NullIndex() != kInvalidIndex ? textures.NullIndex() : 0u) : index;
			a_payload.records[record].textures[t] = value;
			if (a_payload.stagedRecords)
				a_payload.stagedRecords[record].textures[t] = value;  // write-combined: written, never read
		}
		a_stats.frameTexturesMissing = frameTexturesMissing;
		a_stats.frameTexturesMissingRegisters = missingRegisters;
		lap(2);

		// The frame slots: each block into its slot, and the zeroed light block every bindless draw's b3 reads.
		for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
			if (!a_blocks.vs[slot].empty())
				uploads(a_resources->frameConstants, a_blocks.vs[slot].data(), a_blocks.vs[slot].size(), FrameSlotOffset(false, slot));
			if (!a_blocks.ps[slot].empty())
				uploads(a_resources->frameConstants, a_blocks.ps[slot].data(), a_blocks.ps[slot].size(), FrameSlotOffset(true, slot));
		}
		if (in.bindlessDraws) {
			static const std::array<std::uint32_t, kStrictLightDataBytes / 4> zeroLight{};
			uploads(a_resources->frameConstants, zeroLight.data(), sizeof(zeroLight), std::uint64_t(kFrameSlotSharedLight) * kFrameSlotBytes);
		}
		// The frame lighting, only when it changed (RefreshFrameConstants versions it).
		if (const auto& lightingTables = a_store.GetTables(); in.bindless && a_resources->frameLightingUploaded != lightingTables.frameLightingVersion) {
			uploads(a_resources->frameConstants, lightingTables.frameLighting.data(), sizeof(lightingTables.frameLighting), std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes);
			a_resources->frameLightingUploaded = lightingTables.frameLightingVersion;
		}

		lap(3);
		// Upload (the graph's upload pass runs ahead of every pass of this epoch). The worker's build staged its
		// payload itself: one submission, no copies here. A build made here, or staged against resources since
		// recreated, is uploaded from its vectors.
		const auto& bytes = a_payload.arena.Bytes();
		const bool staged = a_payload.staged && a_payload.stagedFor == a_resources.get();
		if (staged) {
			org::runtime::GetActiveUploadService()->SubmitStagedUploads(std::move(a_payload.staged));
			a_payload.stagedRecords = nullptr;
		} else if (!bytes.empty()) {
			uploads(depthOnly && a_resources->constantsDepth ? a_resources->constantsDepth : a_resources->constants, bytes.data(), bytes.size(), 0);
		}
		if (a_resources->facePositions)
			UploadFaceStreams(a_payload.faceStreams, a_resources->facePositions, a_resources->faceUploaded, uploads);
		// The depth segment clears every counter; the colour segment clears only the word its own draws
		// append through. On the hybrid path the culling happens in the depth segment, so clearing the
		// whole buffer again here would erase the phase 1 and phase 2 numbers before anything read them
		// - they are written earlier in the same frame.
		static const std::uint32_t zero[kCountWords] = {};
		const std::size_t zeroBytes = (depthOnly || !a_resources->hybrid) ? sizeof(zero) : sizeof(std::uint32_t);
		uploads(a_resources->count, zero, zeroBytes, 0);
		// The sort's counts start at zero; from then on the scan that reads them clears them.
		if (a_resources->sort)
			a_resources->sort->ZeroCountsOnce(uploads);
		// The decal words: each group's slot count for its draw, and the tallies zeroed. Written by the
		// colour segment only, which is the one that submits decals.
		if (!depthOnly) {
			decalWords = { a_payload.decalCount[0], a_payload.decalCount[1], 0u, 0u };
			uploads(a_resources->count, decalWords.data(), decalWords.size() * sizeof(std::uint32_t), kCountDecalGroupWord * sizeof(std::uint32_t));
		}
		if (!staged)
			UploadMainPayload(a_payload, *a_resources, uploads);
		// The kept records are immutable templates. Patch only commit-owned copies, after the worker's
		// base uploads. A rewritten base record also needs a patch even when the frame texture did not
		// change: its upload replaced the GPU's previously resolved descriptor with the template's zero.
		if (a_payload.persistent && !depthOnly && a_payload.keptRecords.elements) {
			auto& committed = a_resources->committedFrameTextures[1];
			std::array<std::uint64_t, 2> used{}, changed{}, missing{};
			for (const auto& mask : a_payload.patchMasks) {
				used[0] |= mask[0];
				used[1] |= mask[1];
			}
			for (std::uint32_t t = kPixelTextureSlots; t < kTextureRegisters; ++t) {
				const std::uint32_t index = frameTextures[t];
				const std::uint32_t value = index == kInvalidIndex ? (in.resolveTextures && textures.NullIndex() != kInvalidIndex ? textures.NullIndex() : 0u) : index;
				const bool indexChanged = committed[t] != value;
				committed[t] = value;
				const std::uint64_t bit = 1ull << (t % 64);
				if (!(used[t / 64] & bit))
					continue;
				if (index == kInvalidIndex)
					missing[t / 64] |= bit;
				if (indexChanged)
					changed[t / 64] |= bit;
			}
			std::uint32_t keptMissing = 0;
			const auto& recordsTarget = a_resources->records;
			for (std::uint32_t slot = 0; slot < a_payload.patchMasks.size() && slot < a_payload.keptRecords.Count(); ++slot) {
				const auto& mask = a_payload.patchMasks[slot];
				keptMissing += static_cast<std::uint32_t>(std::popcount(mask[0] & missing[0]) + std::popcount(mask[1] & missing[1]));
			}
			const std::uint32_t patched = EmitFrameRecordPatches(a_payload.keptRecords, a_resources->recordsUploaded[1],
				std::span<const std::array<std::uint64_t, 2>>(a_payload.patchMasks), changed, committed,
				[&](std::size_t slot, const DrawBindings& record) { uploads(recordsTarget, &record, sizeof(record), slot * sizeof(DrawBindings)); });
			a_stats.frameTexturesMissing = keptMissing;
			a_stats.frameTexturesMissingRegisters = missing;
			a_stats.framePatchedRecords += patched;
		}
		if (a_payload.persistent) {
			a_resources->constantsUploaded[depthOnly ? 0 : 1] = a_payload.keptConstants.Version();
			a_resources->recordsUploaded[depthOnly ? 0 : 1] = a_payload.keptRecords.Version();
		}
		// Either path uploaded the resident region when the buffer held another version of it, and the object records.
		a_resources->residentUploaded[depthOnly && a_resources->inputsDepth ? 0 : 1] = a_payload.resident.Version();
		const std::size_t objectBytes = a_payload.objectRecords.Emit(a_resources->tablesHeld.objects, [](const void*, std::size_t, std::size_t) {});
		if (a_payload.objectRecords.Version())
			a_resources->tablesHeld.objects = a_payload.objectRecords.Version();
		const std::size_t geometryBytes = EmitGeometryDraws(a_payload.geometryDraws, a_resources->tablesHeld.geometries, [](const void*, std::size_t, std::size_t) {});
		if (a_payload.geometryDraws.Version())
			a_resources->tablesHeld.geometries = a_payload.geometryDraws.Version();
		std::size_t boneRowsSent = 0;
		if (a_payload.bones.Version()) {
			BonesStore* bonesParity = PersistentParityEnabled() ? &mainBones : nullptr;
			boneRowsSent = EmitBones(a_payload.bones, a_resources->tablesHeld.bones, bonesParity, [](const void*, std::size_t, std::size_t) {});
			if (bonesParity && frameNumber % 60 == 0)
				CheckBones(*bonesParity, a_payload.bones);
			a_resources->tablesHeld.bones = a_payload.bones.Version();
			mainBones.rowsSent += boneRowsSent;
		}
		a_stats.residentInputs = static_cast<std::uint32_t>(a_payload.resident.Count());
		if (!depthOnly) {
			a_stats.residentDraws = a_payload.residentDraws;
			a_stats.residentPairs = a_payload.residentPairs;
			a_stats.residentUndrawable = a_payload.residentUndrawable;
		}
		a_stats.residentVersions += a_payload.resident.Version() != a_stats.residentLastVersion[depthOnly ? 0 : 1] ? 1 : 0;
		a_stats.residentLastVersion[depthOnly ? 0 : 1] = a_payload.resident.Version();
		a_stats.residentResyncs += a_payload.residentResyncs;
		a_stats.residentParityChecks += a_payload.residentParityChecks;
		a_stats.residentParityMismatches += a_payload.residentParityMismatches;
		a_stats.residentMissing += a_payload.residentMissing;
		a_stats.boneRows = a_payload.bones.Version() ? static_cast<std::uint32_t>(boneRowsSent) : a_payload.bones.Rows();

		lap(4);
		// The build's stats.
		a_stats.skipped = a_payload.skipped;
		a_stats.missingTextures = a_payload.missingTextures;
		a_stats.missingVertexConstants = a_payload.missingVertexConstants;
		a_stats.missingPixelConstants = a_payload.missingPixelConstants;
		a_stats.deferredTextures = a_payload.deferredTextures;
		a_stats.bindlessParityChecks += a_payload.bindlessParityChecks;
		a_stats.bindlessParityMismatches += a_payload.bindlessParityMismatches;
		a_stats.recordParityChecks += a_payload.recordParityChecks;
		a_stats.recordParityMismatches += a_payload.recordParityMismatches;
		for (std::size_t i = 0; i < a_payload.partMs.size(); ++i)
			a_stats.partMs[i] = a_payload.partMs[i];
		if (!depthOnly)
			a_stats.decalsDrawn = a_payload.decalsDrawn;
		if (a_payload.shortBuffers) {
			const bool first = a_stats.shortBuffers == 0;
			a_stats.shortBuffers += a_payload.shortBuffers;
			const auto& shortBuffer = a_payload.shortBuffer;
			if (first && shortBuffer.object < tables.objects.size()) {
				const auto& geometry = tables.geometries[tables.objects[shortBuffer.object].geometryIndex];
				logger::warn("[DCLF] '{}' draws past its buffers: {} vertices x {} bytes needs {}, the slice holds {}; indices to {} need {}, the slice holds {}",
					tables.objectGeometry[shortBuffer.object] ? tables.objectGeometry[shortBuffer.object]->name.c_str() : "?", geometry.vertexCount, geometry.vertexStride,
					shortBuffer.vertexNeeded, geometry.vertexBytes, geometry.firstIndex + geometry.indexCount, shortBuffer.indexNeeded, geometry.indexBytes);
			}
		}
		a_stats.uploadBytes = bytes.size() + a_payload.records.size() * sizeof(DrawBindings) + a_payload.inputList.size() * sizeof(DrawInput) +
		                      geometryBytes + objectBytes;
		a_stats.records = a_payload.persistent ? a_payload.recordsHeld : static_cast<std::uint32_t>(a_payload.records.size());
		lap(6);
		// What the colour epoch drew: the native loop's skip set and the claims, from the build's changes alone. A build made
		// against another applied version than this one asks for every slot again.
		if (!depthOnly && a_payload.drawnValid) {
			if (a_payload.drawnFull || a_payload.drawnBase == drawnCommitted) {
				if (a_payload.drawnFull) {
					++drawnResyncs;
					// Slots past the full send's end were not known to the build: nothing draws them.
					for (std::uint32_t slot = static_cast<std::uint32_t>(a_payload.drawnChanges.size()); slot < slotDrawn.size(); ++slot)
						if (slotDrawn[slot].drawn)
							ApplyDrawn(slot, nullptr, false, frameNumber);
				}
				for (const auto& change : a_payload.drawnChanges)
					ApplyDrawn(change.slot, change.geometry, change.drawn, frameNumber);
				drawnCommitted = a_payload.drawnVersion;
				drawnResync = false;
			} else {
				drawnResync = true;
			}
		}
		if (!depthOnly)
			drawnCommitFrame = frameNumber;
		lap(5);

		// The resident region's draws and inputs lead the frame's own (ResidentRegion).
		const std::uint32_t drawCount = static_cast<std::uint32_t>(a_payload.sequences.size() + a_payload.residentDraws);
		const auto decalCount = depthOnly ? std::array<std::uint32_t, kDecalGroups>{} : a_payload.decalCount;
		// Inputs the culling dispatch covers. In the depth segment this exceeds drawCount, because that
		// segment submits a cull-only input for every candidate it is not allowed to draw.
		const std::uint32_t inputCount = static_cast<std::uint32_t>(a_payload.inputList.size() + (a_payload.resident.Count()));
		const auto& previousShape = a_resources->published[shapeIndex];
		auto frame = std::make_shared<PassFrame>();
		frame->drawCapacity = GrowCapacity(previousShape ? previousShape->drawCapacity : 0u, drawCount, kMaxDraws);
		for (std::uint32_t group = 0; group < kDecalGroups; ++group)
			frame->decalCapacity[group] = GrowCapacity(previousShape ? previousShape->decalCapacity[group] : 0u, decalCount[group], kMaxDecalDraws);
		frame->width = a_capture.viewportWidth;
		frame->height = a_capture.viewportHeight;
		// Both epochs rasterise with the main pass's depth range; see Impl::mainMinDepth.
		const bool useMainRange = a_resources->hybrid && mainMaxDepth > 0.0f;
		frame->minDepth = useMainRange ? mainMinDepth : a_capture.minDepth;
		frame->maxDepth = useMainRange ? mainMaxDepth : a_capture.maxDepth;
		frame->resourceHeap = org::runtime::GetActiveSRVDescriptorHeap().GetHandle();
		frame->samplerHeap = org::runtime::GetActiveSamplerDescriptorHeap().GetHandle();
		frame->indirect = GetIndirectState();
		frame->cullMode = CullingMode();
		// The main pass's ViewProj (VS_PerFrame c8), which the draws project with: the culling has to
		// use the same matrix or it would reject what the draws would have put on screen.
		std::array<float, 16> viewProj{};
		bool hasViewProj = false;
		if (auto* buffer = a_capture.vsBuffers[kPerFrameVertexRegister]) {
			mirror.Watch(buffer);
			const auto contents = mirror.Contents(buffer);
			if (contents.size() >= 48 * sizeof(float)) {
				std::memcpy(viewProj.data(), reinterpret_cast<const float*>(contents.data()) + 32, sizeof(float) * 16);
				hasViewProj = true;
			}
		}
		if (!loggedNoViewProj) {
			loggedNoViewProj = true;
			logger::info("[DCLF] culling setup: mode {}, ViewProj {}, VS_PerFrame b{} {}", frame->cullMode, hasViewProj ? "yes" : "no",
				kPerFrameVertexRegister, a_capture.vsBuffers[kPerFrameVertexRegister] ? "bound" : "not bound");
		}
		frame->hybrid = a_resources->hybrid;
		frame->offscreen = a_resources->offscreen;
		if (const auto pixel = SwitchValue("CS_DCLF_GBUFFER_PROBE"); !pixel.empty()) {
			if (const auto sep = pixel.find_first_of(",x"); sep != std::string::npos) {
				frame->probeX = static_cast<std::uint32_t>(std::strtoul(pixel.substr(0, sep).c_str(), nullptr, 10));
				frame->probeY = static_cast<std::uint32_t>(std::strtoul(pixel.substr(sep + 1).c_str(), nullptr, 10));
				frame->probePixel = frame->probeX < frame->width && frame->probeY < frame->height;
			}
		}
		// The two epochs must rasterise into the same pixels for the colour pass's EQUAL test to have any
		// chance: a different viewport or depth range between the depth pass and the main pass puts the
		// same vertex on a different pixel, at a different depth.
		if ((depthOnly ? loggedDepthViewport : loggedColourViewport)++ % 480 == 0) {
			// The z row of the ViewProj each epoch actually packs: this is what turns a vertex into the
			// depth the test compares, so if the two epochs differ it shows up here.
			std::string viewProjZ = "(none)";
			{
				std::span<const std::byte> perFrame;
				if (replayVertexInputs)
					perFrame = prepassVS[kPerFrameVertexRegister];
				else if (auto* buffer = a_capture.vsBuffers[kPerFrameVertexRegister])
					perFrame = mirror.Contents(buffer);
				if (perFrame.size() >= 48 * sizeof(float)) {
					const auto* floats = reinterpret_cast<const float*>(perFrame.data());
					viewProjZ = fmt::format("({:.6f} {:.6f} {:.6f} {:.6f})", floats[40], floats[41], floats[42], floats[43]);
				}
			}
			logger::info("[DCLF] {} epoch: tables frame {} holding {} objects; render area {}x{}, depth range [{}, {}] (captured [{}, {}]), replay {}, eye ({:.2f} {:.2f} {:.2f}), ViewProj z row {}",
				depthOnly ? "z-prepass" : "colour", frameNumber, tables.liveObjects, frame->width, frame->height, frame->minDepth, frame->maxDepth,
				a_capture.minDepth, a_capture.maxDepth, replayVertexInputs ? "on" : "off", a_capture.eye.x, a_capture.eye.y, a_capture.eye.z, viewProjZ);
		}
		if (depthOnly) {
			prepassEye = a_capture.eye;
			prepassPreviousEye = a_capture.previousEye;
			prepassMinDepth = a_capture.minDepth;
			prepassMaxDepth = a_capture.maxDepth;
			prepassInputs = true;
		}
		a_stats.drawn = drawCount;
		++a_stats.commitEpochs;

		// The execution's values, into its slot of the latch (this runs inside the epoch, after the host
		// waited for the slot): what the BuildDraws dispatches of the segment read instead of push constants.
		BuildDrawsLatch latch{};
		latch.dispatch[0] = (inputCount + 63) / 64;
		latch.dispatch[1] = 1;
		latch.dispatch[2] = 1;
		latch.drawCount = inputCount;
		latch.cullFlags = (hasViewProj ? frame->cullMode : 0u) | (in.requireNativeVisible ? 0x100u : 0u);
		// The frame number, not the epoch: the depth segment publishes and the colour segment reads within one
		// frame, so the stamp has to be the thing they share.
		latch.visibilityStamp = frameNumber & 0x0FFFFFFFu;  // 28 bits: BuildDrawsCS keeps flags below it
		if (a_resources->hzb && frame->width && frame->height) {
			// Mip 0 covers twice its own size in source pixels, of which only the rendered area holds real
			// depth. A texture coordinate in the image scales by that ratio to reach the HZB.
			const auto scale = [](std::uint32_t a_rendered, std::uint32_t a_covered) {
				const double ratio = a_covered ? std::min(1.0, double(a_rendered) / double(a_covered)) : 0.0;
				return static_cast<std::uint32_t>(std::lround(ratio * 65535.0)) & 0xFFFFu;
			};
			latch.hzbUvScalePacked = scale(frame->width, a_resources->hzbWidth * 2) | (scale(frame->height, a_resources->hzbHeight * 2) << 16);
		}
		FoldEyeIntoViewProj(viewProj, a_capture.eye, latch.viewProj);
		// The depth segment: the camera the fade roots' distances are measured from (kObjectFadeTest).
		if (depthOnly) {
			const auto eye = PrimaryCull::Get().FadeEye();
			std::copy(eye.begin(), eye.end(), latch.fadeEye);
			const auto treeHeight = PrimaryCull::Get().TreeHeightTest();
			latch.treeHeight[0] = treeHeight[0];
			latch.treeHeight[1] = treeHeight[1];
		}
		// The colour pass: this frame's cascades, for the synthetic passes' sun test. The sun's Accumulate has run.
		if (!depthOnly) {
			latch.sunState = kSunTestOn | SunAccumulation::Get().GpuCascades(latch.sunMasks, latch.sunPlanes);
			sunUpload = latch;
			ArmFeedback(*a_resources, frameNumber, static_cast<std::uint32_t>(std::min<std::size_t>(tables.objects.size(), kMaxObjects)));
		}
		a_resources->latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentFrameSlot(), 0, latch);

		// The shape: the previous one while nothing the recordings depend on changed, so the passes reuse
		// their invocations; a new generation otherwise.
		if (previousShape && previousShape->SameShape(*frame)) {
			a_resources->frames[shapeIndex].store(previousShape, std::memory_order_release);
		} else {
			frame->generation = ++a_resources->shapeGenerations;
			a_resources->published[shapeIndex] = frame;
			a_resources->frames[shapeIndex].store(std::move(frame), std::memory_order_release);
		}
		lap(6);
		return true;
	}

	void IndirectDraws::Impl::ProbeGBuffer(const char* a_label)
	{
		(void)a_label;
		if (!resources || !resources->probeD3D11)
			return;
		auto* context = globals::d3d::context;
		if (gbufferStaging) {
			if (--gbufferFramesLeft)
				return;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(context->Map(gbufferStaging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
				const auto* bytes = static_cast<const std::uint8_t*>(mapped.pData);
				auto slots = [&](std::uint32_t a_base) {
					std::string text;
					for (std::uint32_t i = 0; i < gbufferCount; ++i) {
						const auto* texel = bytes + std::size_t(a_base + i) * kProbeSlotBytes;
						std::string hex;
						for (std::uint32_t b = 0; b < gbufferBytes[i]; ++b)
							hex += fmt::format("{:02X}", texel[b]);
						text += fmt::format("{}rt{}={}", text.empty() ? "" : " | ", i, hex);
					}
					return text;
				};
				auto depthAt = [&](std::uint32_t a_slot) {
					std::uint32_t packed = 0;
					std::memcpy(&packed, bytes + std::size_t(a_slot) * kProbeSlotBytes, sizeof(packed));
					return packed & 0x00FFFFFFu;
				};
				logger::info("[DCLF] G-buffer at ({}, {}) before DCLF's colour draws: {}", gbufferX, gbufferY, slots(0));
				logger::info("[DCLF] G-buffer at ({}, {}) after  DCLF's colour draws: {}", gbufferX, gbufferY, slots(kColorTargets));
				logger::info("[DCLF] depth at ({}, {}): {:06X} after the z-prepass, {:06X} when the colour pass tests it, {:06X} after it",
					gbufferX, gbufferY, depthAt(kProbeDepthAfterPrepass), depthAt(kProbeDepthBeforeColour), depthAt(kProbeDepthAfterColour));
				logger::info("[DCLF] depth gap at ({}, {}): {:06X} after sky occlusion, {:06X} after light culling",
					gbufferX, gbufferY, depthAt(kProbeDepthAfterSky), depthAt(kProbeDepthAfterLightCulling));
				context->Unmap(gbufferStaging.get(), 0);
			}
			gbufferStaging = nullptr;
			return;
		}
		if ((gbufferEpochs++ % 480) != 0)
			return;
		D3D11_BUFFER_DESC desc{};
		resources->probeD3D11->GetDesc(&desc);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.MiscFlags = 0;
		desc.StructureByteStride = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		winrt::com_ptr<ID3D11Buffer> staging;
		if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, staging.put())))
			return;
		ScopedPerfEvent event("CS DCLF: G-buffer probe readback");
		context->CopyResource(staging.get(), resources->probeD3D11.get());
		gbufferStaging = std::move(staging);
		gbufferFramesLeft = 4;
		gbufferCount = probeTargetCount;
		for (std::uint32_t i = 0; i < gbufferCount; ++i) {
			D3D11_TEXTURE2D_DESC textureDesc{};
			if (probeTargets[i])
				probeTargets[i]->GetDesc(&textureDesc);
			gbufferBytes[i] = FormatBytes(textureDesc.Format);
		}
		if (const auto pixel = SwitchValue("CS_DCLF_GBUFFER_PROBE"); !pixel.empty()) {
			if (const auto sep = pixel.find_first_of(",x"); sep != std::string::npos) {
				gbufferX = static_cast<std::uint32_t>(std::strtoul(pixel.substr(0, sep).c_str(), nullptr, 10));
				gbufferY = static_cast<std::uint32_t>(std::strtoul(pixel.substr(sep + 1).c_str(), nullptr, 10));
			}
		}
	}

	void IndirectDraws::Impl::CheckSetParity(const std::shared_ptr<Resources>& a_resources, const MainPayload& a_depth, const MainPayload& a_colour)
	{
		auto* context = globals::d3d::context;
		auto& store = SceneStore::Get();
		auto& counts = setParity;
		auto stateName = [](std::uint8_t a_state) -> std::string {
			if (a_state == kObjectStateDrawable)
				return "drawable";
			if (a_state == kObjectStateDecal)
				return "decal";
			if (a_state == kObjectStateAbsent)
				return "absent";
			return a_state < kSkipNames.size() ? std::string("skipped: ") + kSkipNames[a_state] : fmt::format("state {}", a_state);
		};

		// The oldest snapshot, once its frame's copy has had time to land.
		for (auto& waiting : setParityFrames)
			if (waiting.framesLeft)
				--waiting.framesLeft;
		while (!setParityFrames.empty() && setParityFrames.front().framesLeft == 0) {
			auto snapshot = std::move(setParityFrames.front());
			setParityFrames.pop_front();
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(context->Map(snapshot.staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
				++counts.skipped;
				continue;
			}
			const auto* words = static_cast<const std::uint32_t*>(mapped.pData);
			const std::uint32_t stamp = snapshot.frame & 0x0FFFFFFFu;
			bool damaged = false;
			for (std::size_t o = 0; o < snapshot.flags.size(); ++o) {
				const std::uint32_t word = words[o];
				const bool current = (word >> 4) == stamp;
				const std::uint32_t verdict = current ? (word & 3) : ~0u;
				const bool depthDrawn = current && (word & 4);
				const bool colourDrawn = current && (word & 8);
				const std::uint8_t flags = snapshot.flags[o];
				const bool withheld = flags & 1;
				const bool alpha = flags & 4;
				counts.depthDrawnTotal += depthDrawn;
				counts.colourDrawnTotal += colourDrawn;
				const char* kind = nullptr;
				const bool decal = o < snapshot.colourState.size() && snapshot.colourState[o] == kObjectStateDecal;
				// A decal is drawn by the colour segment's decal pass only (the depth segment never draws one), so it
				// is only checked for being withheld and undrawn.
				if (!decal && depthDrawn && !colourDrawn) {
					kind = "depth without colour";
					++counts.depthOnly;
					counts.alphaDepthOnly += alpha;
				} else if (!decal && colourDrawn && !depthDrawn) {
					kind = verdict == 3 ? "colour without depth (no verdict this frame)" : "colour without depth";
					++counts.colourOnly;
					counts.colourUnpublished += verdict == 3;
					counts.alphaColourOnly += alpha;
				} else if (withheld && !colourDrawn) {
					if (verdict == 0 || verdict == 2) {
						++counts.withheldCulled;  // the GPU culling rejected it: not drawn by anyone, as intended
					} else {
						kind = "withheld natively, drawn by nobody";
						++counts.withheldUndrawn;
						counts.alphaWithheldUndrawn += alpha;
					}
				}
				// The gap detector, by geometry (object indices are rebuilt every frame).
				if (const auto* geometry = o < snapshot.geometry.size() ? snapshot.geometry[o] : nullptr) {
					const bool kept = flags & 2;
					const std::uint8_t state = !kept ? 0 : (withheld && !colourDrawn) ? static_cast<std::uint8_t>(2 | (verdict << 4)) : 1;
					auto& history = gapHistory[geometry];
					if (history.frame + 1 == snapshot.frame && state == 1 && (history.last & 3) == 2 && history.before == 1) {
						++counts.gaps;
						const std::uint32_t gapVerdict = history.last >> 4;
						counts.gapsRetest += gapVerdict == 0;
						counts.gapsRejected += gapVerdict == 2;
						const RE::TESObjectREFR* owner = nullptr;
						for (const RE::NiAVObject* node = geometry; node && !owner; node = node->parent)
							owner = node->GetUserData();
						const auto* base = owner ? owner->GetBaseObject() : nullptr;
						const bool tree = base && base->GetFormType() == RE::FormType::Tree;
						counts.gapsTree += tree;
						if (counts.gapSamples++ < 12)
							logger::info("[DCLF] set parity, frame {}: one-frame gap - '{}' ({}{}), culled in frame {} with verdict {}", snapshot.frame,
								geometry->name.c_str() ? geometry->name.c_str() : "?", base ? RE::FormTypeToString(base->GetFormType()) : "no ref",
								geometry->GetGeometryRuntimeData().skinInstance ? ", skinned" : "", snapshot.frame - 1,
								gapVerdict == 0 ? "occluded (retest)" : gapVerdict == 2 ? "rejected" : "other");
					}
					history.before = history.frame + 1 == snapshot.frame ? history.last : 0;
					history.last = state;
					history.frame = snapshot.frame;
				}
				if (!kind)
					continue;
				damaged = true;
				if (counts.samples++ < 40) {
					const auto* geometry = o < snapshot.geometry.size() ? snapshot.geometry[o] : nullptr;
					const bool alive = geometry && store.IsTracked(geometry);
					static constexpr const char* kVerdicts[] = { "occluded (retest)", "visible", "rejected", "no verdict" };
					logger::info("[DCLF] set parity, frame {}: {} - object {} '{}'{}: verdict {}, depth build {}, colour build {}, {}{}{}", snapshot.frame, kind, o,
						alive ? geometry->name.c_str() : "?", alpha ? " (alpha tested)" : "", current ? kVerdicts[verdict] : "not this frame's",
						stateName(o < snapshot.depthState.size() ? snapshot.depthState[o] : kObjectStateAbsent),
						stateName(o < snapshot.colourState.size() ? snapshot.colourState[o] : kObjectStateAbsent),
						(flags & 2) ? "engine kept it" : "engine culled it", (flags & 8) ? ", claimed" : "", withheld ? ", withheld" : "");
				}
			}
			context->Unmap(snapshot.staging.get(), 0);
			setParityStaging.push_back(std::move(snapshot.staging));
			++counts.frames;
			counts.framesWithDamage += damaged;
			if (counts.frames == 300) {
				logger::info("[DCLF] set parity over {} frames ({} with damage, {} unread): depth without colour {} ({} alpha tested), colour without depth {} ({} alpha tested, {} with no verdict), withheld and drawn by nobody {} ({} alpha tested); withheld and GPU-culled {}; per frame {:.0f} depth draws, {:.0f} colour draws",
					counts.frames, counts.framesWithDamage, counts.skipped, counts.depthOnly, counts.alphaDepthOnly, counts.colourOnly, counts.alphaColourOnly,
					counts.colourUnpublished, counts.withheldUndrawn, counts.alphaWithheldUndrawn, counts.withheldCulled, double(counts.depthDrawnTotal) / counts.frames,
					double(counts.colourDrawnTotal) / counts.frames);
				if (counts.gaps)
					logger::info("[DCLF] set parity over {} frames: {} one-frame gaps (kept, drawn, withheld and GPU-culled, drawn again): {} occluded (retest), {} rejected; {} of them trees",
						counts.frames, counts.gaps, counts.gapsRetest, counts.gapsRejected, counts.gapsTree);
				counts = {};
				// Forget geometries not seen for a while, so the history does not keep every object ever drawn.
				std::erase_if(gapHistory, [&](const auto& a_entry) { return a_entry.second.frame + 8 < snapshot.frame; });
			}
		}

		// This frame: the words as the colour epoch left them, and the CPU side that explains them. Only when
		// the depth build is this frame's too.
		if (!a_resources->visibilityD3D11 || a_depth.inputs.frameNumber != a_colour.inputs.frameNumber || setParityFrames.size() >= 8)
			return;
		SetParityFrame snapshot;
		if (!setParityStaging.empty()) {
			snapshot.staging = std::move(setParityStaging.back());
			setParityStaging.pop_back();
		} else {
			D3D11_BUFFER_DESC desc{};
			a_resources->visibilityD3D11->GetDesc(&desc);
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = 0;
			desc.MiscFlags = 0;
			desc.StructureByteStride = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, snapshot.staging.put())))
				return;
		}
		context->CopyResource(snapshot.staging.get(), a_resources->visibilityD3D11.get());
		snapshot.frame = a_colour.inputs.frameNumber;
		snapshot.framesLeft = 3;
		snapshot.depthState = a_depth.objectState;
		snapshot.colourState = a_colour.objectState;
		const auto& tables = store.GetTables();
		const auto claims = PassCapture::Get().CurrentClaims();
		const std::size_t objects = std::min<std::size_t>(tables.objects.size(), kMaxObjects);
		snapshot.flags.assign(objects, 0);
		snapshot.geometry.assign(objects, nullptr);
		for (std::size_t o = 0; o < objects && o < tables.objectGeometry.size(); ++o) {
			const auto* geometry = tables.objectGeometry[o];
			snapshot.geometry[o] = geometry;
			const auto objectFlags = tables.objects[o].flags;
			std::uint8_t flags = 0;
			if (objectFlags & kObjectNativeVisible)
				flags |= 2;
			if (objectFlags & kObjectAlphaTest)
				flags |= 4;
			if (geometry && claims && claims->contains(geometry)) {
				flags |= 8;
				if (PassCapture::Get().WithheldThisFrame(geometry))
					flags |= 1;
			}
			snapshot.flags[o] = flags;
		}
		setParityFrames.push_back(std::move(snapshot));
	}

	void IndirectDraws::Impl::ReadCullCounters(const std::shared_ptr<Resources>& a_resources, IndirectDraws::Stats& a_stats, const MainPayload& a_payload)
	{
		auto* context = globals::d3d::context;
		if (cullReadback) {
			if (--cullReadback->framesLeft)
				return;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(context->Map(cullReadback->count.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
				const auto* words = static_cast<const std::uint32_t*>(mapped.pData);
				a_stats.cullDrawn = words[0];
				a_stats.cullRejected = words[1];
				a_stats.cullTested = words[2];
				a_stats.cullEngineCulled = words[3];
				a_stats.cullFalseNegatives = words[4];
				a_stats.cullRescued = words[5];
				a_stats.cullOccluded = words[6];
				a_stats.hzbNear = words[7];
				a_stats.hzbFar = words[8];
				a_stats.hzbSampled = words[9];
				if (words[10]) {
					a_stats.hzbSample.valid = true;
					std::memcpy(&a_stats.hzbSample.farthest, &words[11], sizeof(float));
					std::memcpy(&a_stats.hzbSample.nearestZ, &words[12], sizeof(float));
					a_stats.hzbSample.uvMin[0] = (words[13] & 0xFFFF) / 65535.0f;
					a_stats.hzbSample.uvMin[1] = (words[13] >> 16) / 65535.0f;
					a_stats.hzbSample.uvMax[0] = (words[14] & 0xFFFF) / 65535.0f;
					a_stats.hzbSample.uvMax[1] = (words[14] >> 16) / 65535.0f;
					a_stats.hzbSample.mip = words[15] & 0xFF;
					a_stats.hzbSample.nativeVisible = (words[15] >> 8) != 0;
				} else {
					a_stats.hzbSample.valid = false;
				}
				a_stats.cullOccludedVisible = words[16];
				a_stats.cullDrawnPhaseTwo = words[17];
				a_stats.cullRescuedByPhaseTwo = words[18];
				a_stats.decalsCulled = words[kCountDecalsCulledWord];
				a_stats.decalsTested = words[kCountDecalsTestedWord];
				a_stats.sunTested = words[kCountSunTestedWord];
				a_stats.sunMissed = words[kCountSunMissedWord];
				a_stats.fadeTested = words[kCountFadeTestedWord];
				a_stats.fadeHidden = words[kCountFadeHiddenWord];
				a_stats.sunCpuTested = cullReadback->sunCpuTested;
				a_stats.sunCpuMissed = cullReadback->sunCpuMissed;
				context->Unmap(cullReadback->count.get(), 0);
			}
			cullReadback.reset();
			return;
		}
		if ((cullEpochs++ % 120) != 0 || !a_resources->countD3D11)
			return;
		D3D11_BUFFER_DESC desc{};
		a_resources->countD3D11->GetDesc(&desc);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.MiscFlags = 0;
		desc.StructureByteStride = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		CullReadback readback;
		if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, readback.count.put())))
			return;
		ScopedPerfEvent event("CS DCLF: culling readback");
		context->CopyResource(readback.count.get(), a_resources->countD3D11.get());
		readback.framesLeft = 3;
		// The CPU's side of the sun test, over the same frame's colour inputs and the same planes.
		const std::uint32_t cascades = sunUpload.sunState & ~kSunTestOn;
		auto sunTest = [&](const DrawInput& input) {
			if (!(input.flags & kObjectSunTest) || !(sunUpload.sunState & kSunTestOn))
				return;
			++readback.sunCpuTested;
			bool inside = false;
			for (std::uint32_t c = 0; c < cascades && !inside; ++c)
				inside = InSunCascade(sunUpload, c, input.boundCentre, input.boundRadius);
			readback.sunCpuMissed += inside ? 0 : 1;
		};
		if (a_payload.resident.elements)
			for (const auto& input : *a_payload.resident.elements)
				sunTest(input);
		for (const auto& input : a_payload.inputList)
			sunTest(input);
		cullReadback = std::move(readback);
	}

	void IndirectDraws::Impl::CheckBuildParity(const std::shared_ptr<Resources>& a_resources, const MainPayload& a_payload, IndirectDraws::Stats& a_stats)
	{
		const auto& sequences = a_payload.sequences;
		const auto& decalTemplates = a_payload.decalTemplates;
		const auto& inputs = a_payload.inputList;
		auto* context = globals::d3d::context;
		if (parity) {
			if (--parity->framesLeft)
				return;
			D3D11_MAPPED_SUBRESOURCE countMap{}, sequencesMap{};
			if (FAILED(context->Map(parity->count.get(), 0, D3D11_MAP_READ, 0, &countMap)) ||
				FAILED(context->Map(parity->sequences.get(), 0, D3D11_MAP_READ, 0, &sequencesMap))) {
				parity.reset();
				return;
			}
			const std::uint32_t count = static_cast<const std::uint32_t*>(countMap.pData)[0];
			const std::uint32_t culled = static_cast<const std::uint32_t*>(countMap.pData)[1];
			const auto* gpuSequences = static_cast<const DrawSequence*>(sequencesMap.pData);
			std::vector<DrawSequence> built(gpuSequences, gpuSequences + std::min<std::size_t>(count, kMaxDraws));
			// The decal slots: fixed, so they compare in place. A culled slot is the template with an index
			// count of zero; anything else differing is a defect.
			std::size_t decalDiffering = 0, decalCulled = 0, decalSlots = 0;
			for (std::uint32_t group = 0; group < kDecalGroups; ++group) {
				const auto& expectedDecals = parity->expectedDecals[group];
				const auto* slots = gpuSequences + kDecalSequenceBase + group * kMaxDecalDraws;
				for (std::size_t slot = 0; slot < expectedDecals.size() && slot < kMaxDecalDraws; ++slot, ++decalSlots) {
					// A zero-count slot draws nothing whatever its other fields hold: it is either a decal
					// the culling rejected or a blank the epoch pushed for one it could not build a record
					// for (whose template is then empty). Either way only the count matters.
					if (slots[slot].indexCount == 0) {
						++decalCulled;
						continue;
					}
					if (std::memcmp(&slots[slot], &expectedDecals[slot], sizeof(DrawSequence)) != 0)
						++decalDiffering;
				}
			}
			context->Unmap(parity->count.get(), 0);
			context->Unmap(parity->sequences.get(), 0);
			// BuildDraws appends in any order: compare as sets. The key has to be unique per sequence, which
			// the object index is and the record address is not - once records deduplicate, dozens of
			// sequences share an address, the sort stops being a total order, and equal-key runs land in
			// arbitrary relative order on the two sides. That reports mismatches that are not mismatches.
			// A skin of several partitions writes one sequence per partition, which its index buffer tells apart.
			// A draw that missed every sun cascade carries kObjectSunMiss in its object word; the CPU's template does not.
			for (auto& sequence : built)
				sequence.objectIndex &= ~kObjectSunMiss;
			auto byObject = [](const DrawSequence& a, const DrawSequence& b) {
				return a.objectIndex != b.objectIndex ? a.objectIndex < b.objectIndex : a.indexBufferAddress < b.indexBufferAddress;
			};
			std::sort(built.begin(), built.end(), byObject);
			auto& expected = parity->expected;
			std::sort(expected.begin(), expected.end(), byObject);
			// What the GPU writes is a SUBSET of the CPU's templates whenever the culling rejects anything, so
			// this is a subsequence check, not an element-wise one: every sequence BuildDraws wrote must
			// appear in the CPU list under the same object index, byte for byte. Comparing position by
			// position instead reported every sequence past the first culled object as differing - a
			// mismatch that says nothing, on the configuration DCLF actually ships.
			std::size_t differing = 0, missing = 0;
			std::size_t first = SIZE_MAX, firstExpected = 0;
			for (std::size_t b = 0, e = 0; b < built.size(); ++b) {
				while (e < expected.size() && expected[e].objectIndex < built[b].objectIndex)
					++e;  // the CPU built a template the culling rejected
				if (e == expected.size() || expected[e].objectIndex != built[b].objectIndex) {
					++missing;  // a sequence with no template at all, which no culling can explain
					continue;
				}
				if (std::memcmp(&built[b], &expected[e], sizeof(DrawSequence)) != 0) {
					if (first == SIZE_MAX) {
						first = b;
						firstExpected = e;
					}
					++differing;
				}
				++e;
			}
			++a_stats.buildParityChecks;
			if (decalSlots)
				logger::info("[DCLF] BuildDraws decal parity {}: {} slots, {} culled, {} differ", decalDiffering ? "MISMATCH" : "OK", decalSlots, decalCulled, decalDiffering);
			if (decalDiffering)
				++a_stats.buildParityMismatches;
			if (!differing && !missing) {
				logger::info("[DCLF] BuildDraws parity OK: {} of {} sequences match the CPU templates ({} rejected by the culling)", count, expected.size(),
					expected.size() - count);
			} else {
				++a_stats.buildParityMismatches;
				logger::warn("[DCLF] BuildDraws parity MISMATCH: GPU wrote {}, CPU templated {} ({} counted culled); {} differ, {} have no template{}", count,
					expected.size(), culled, differing, missing,
					first != SIZE_MAX ? fmt::format(" (first: object {}, pipeline {} vs {}, index count {} vs {})", built[first].objectIndex, built[first].pipelineIndex,
											expected[firstExpected].pipelineIndex, built[first].indexCount, expected[firstExpected].indexCount) :
										"");
			}
			parity.reset();
			return;
		}
		if ((parityEpochs++ % 300) != 0 || !a_resources->sequencesD3D11 || !a_resources->countD3D11)
			return;
		// After the epoch in D3D11 stream order: the copies see what BuildDraws wrote.
		auto staging = [&](ID3D11Buffer* a_source) {
			D3D11_BUFFER_DESC desc{};
			a_source->GetDesc(&desc);
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = 0;
			desc.MiscFlags = 0;
			desc.StructureByteStride = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			winrt::com_ptr<ID3D11Buffer> buffer;
			if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, buffer.put())))
				return buffer;
			context->CopyResource(buffer.get(), a_source);
			return buffer;
		};
		ParityReadback readback;
		readback.sequences = staging(a_resources->sequencesD3D11.get());
		readback.count = staging(a_resources->countD3D11.get());
		// The templates cover every candidate; the gate drops the ones the engine culled before BuildDraws
		// writes a sequence for them, and it is a pure per-object flag test, so the expectation can apply it
		// exactly. Frustum culling cannot be predicted here, which is why parity and culling are separate
		// switches. inputs is parallel to sequences.
		readback.expected.clear();
		readback.expected.reserve(sequences.size());
		for (std::uint32_t group = 0; group < kDecalGroups; ++group)
			readback.expectedDecals[group] = decalTemplates[group];
		{
			// Cull-only inputs carry no sequence, so the two run at different rates and the drawable ones
			// have to be counted off rather than indexed in step. Decals have their own slots and templates.
			// A skin of several partitions has one template per partition drawn, consecutive.
			std::size_t sequence = 0;
			for (const auto& input : inputs) {
				if (!(input.flags & kInputDrawable) || (input.flags & kObjectDecal))
					continue;
				const std::size_t templates = input.partitions ? static_cast<std::size_t>(std::popcount(input.partitions)) : 1;
				if (sequence + templates > sequences.size())
					break;
				const std::size_t first = sequence;
				sequence += templates;
				if (RequireNativeVisible() && !(input.flags & kObjectNativeVisible))
					continue;
				readback.expected.insert(readback.expected.end(), sequences.begin() + first, sequences.begin() + sequence);
			}
		}
		readback.framesLeft = 3;
		if (readback.sequences && readback.count)
			parity = std::move(readback);
	}

	void IndirectDraws::ShowDebugView()
	{
		// The hybrid path imports the native targets too, so their presence does not mean the view is on: an
		// epoch here cost ~0.3 ms of render thread and three queue submissions a frame for nothing.
		if (failed || !DebugViewEnabled() || !impl->resources || !impl->resources->native[0])
			return;
		if (!RenderGraphRuntime::Get().ExecuteEpoch(RenderGraphRuntime::Segment::DebugView))
			logger::error("[DCLF] The debug view epoch failed; the render graph is disabled");
	}
}

#else  // no render graph

#	include "IndirectDraws.h"

namespace DCLF
{
	struct IndirectDraws::Impl
	{};
	IndirectDraws::IndirectDraws() = default;
	IndirectDraws::~IndirectDraws() = default;
	IndirectDraws& IndirectDraws::Get()
	{
		static IndirectDraws draws;
		return draws;
	}
	bool IndirectDraws::Enabled() const { return false; }
	bool IndirectDraws::Hybrid() { return false; }
	void IndirectDraws::PublishClaims() {}
	bool IndirectDraws::DrewLastFrame(const RE::BSGeometry*, std::uint32_t) const { return false; }
	std::uint32_t IndirectDraws::DrainVisibilityFeedback(const std::function<void(const VisibilityFeedbackFrame&)>&) { return 0; }
	IndirectDraws::FeedbackStats IndirectDraws::TakeFeedbackStats() { return {}; }
	void IndirectDraws::CaptureMainPass() {}
	void IndirectDraws::CheckCapturePoint() {}
	void IndirectDraws::Execute() {}
	void IndirectDraws::CaptureDepthPass() {}
	void IndirectDraws::ExecuteColour() {}
	void IndirectDraws::ProbeTargets(const char*) {}
	void IndirectDraws::RunEpoch(RenderGraphRuntime::Segment) {}
	void IndirectDraws::ShowDebugView() {}
	bool IndirectDraws::ShadowsEnabled() { return false; }
	void IndirectDraws::BeginShadowFrame(const RE::NiPoint3&) {}
	void IndirectDraws::ExecuteShadowView(std::uint32_t, std::uint32_t) {}
	void IndirectDraws::ExecuteShadowFrame() {}
	void IndirectDraws::CaptureSkyOcclusion() {}
	bool IndirectDraws::SkyOcclusionReady() const { return false; }
	bool IndirectDraws::ExecuteSkyOcclusion(bool) { return false; }
	void IndirectDraws::KickColourBuild() {}
	void IndirectDraws::KickZPrepassBuild() {}
	void IndirectDraws::KickShadowBuild() {}
	void IndirectDraws::EndFrame() {}
	void IndirectDraws::DrainAsync() {}
	std::string IndirectDraws::AsyncReport() { return {}; }
}

#endif
