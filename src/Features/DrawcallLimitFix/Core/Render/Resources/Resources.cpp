// IndirectDraws' GPU resources: the main, scene, shadow and view resources created on the render thread (Setup, EnsureSceneBuffers,
// SetupShadow, SetupView), each versioned buffer registered with the scene's set (its extent, growth policy and contents), the
// scene's draw bound and the main latch the revision code keeps.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/GpuStages.h"
#include "Features/LightLimitFix/ORGLightCulling.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#include "Features/TerrainBlending.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"

namespace DCLF::Draws
{
	namespace
	{
		/** @brief A device-local buffer the shaders read by device address. */
		std::shared_ptr<org::Buffer> DeviceBuffer(std::uint64_t a_bytes, const char* a_name)
		{
			auto created = org::Buffer::CreateShared(rhi::HeapType::DeviceLocal, a_bytes, false);
			created->SetName(a_name);
			return created;
		}

		/** @brief A structured buffer the shaders read through its SRV; a_index is the SRV's descriptor slot. */
		std::shared_ptr<org::Buffer> StructuredBuffer(std::uint32_t a_elements, std::uint32_t a_stride, const char* a_name, std::uint32_t& a_index, bool a_unorderedAccess = false)
		{
			auto created = org::Buffer::CreateUnmaterializedStructuredBuffer(a_elements, a_stride, a_unorderedAccess);
			created->SetName(a_name);
			created->Materialize();
			a_index = created->GetSRVInfo(0).slot.index;
			return created;
		}

		/** @brief A D3D11 view of a word buffer's first a_bytes, for immediate-context readbacks; words past it read as zero. */
		auto WrapWords(org::Buffer& a_buffer, std::uint64_t a_bytes)
		{
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = static_cast<UINT>(a_bytes);
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = sizeof(std::uint32_t);
			return RenderGraphRuntime::Get().WrapBuffer(a_buffer, desc);
		}

		std::uint64_t AddressOf(rhi::Device a_device, org::Buffer& a_buffer)
		{
			return a_device.GetBufferDeviceAddress({ a_buffer.GetAPIResource().GetHandle(), 0 });
		}

		// Tree LOD's table sizes in words (TreeLod.h): per shape slot its row, records and visible/retest list entries; per mesh slot
		// its row; plus the list header.
		std::uint32_t TreeLodShapeWords(std::uint32_t a_slots) { return a_slots * static_cast<std::uint32_t>(sizeof(TreeLod::ShapeRow) / 4); }
		std::uint32_t TreeLodInstanceWords(std::uint32_t a_slots) { return a_slots * TreeLod::kMaxGroupInstances * static_cast<std::uint32_t>(sizeof(TreeLod::Instance) / 4); }
		std::uint32_t TreeLodMeshWords(std::uint32_t a_slots) { return a_slots * static_cast<std::uint32_t>(sizeof(TreeLod::MeshRow) / 4); }
		std::uint32_t TreeLodVisibleWords(std::uint32_t a_slots) { return TreeLod::kVisibleHeaderWords + 2 * a_slots * TreeLod::kMaxGroupInstances; }

		/** @brief The units a buffer's current version holds (a buffer made at another's size: a requirement grows it from there). */
		std::uint32_t CurrentUnits(const Versioned& a_buffer, std::uint64_t a_unitBytes)
		{
			const auto buffer = a_buffer ? a_buffer->Get() : nullptr;
			return buffer ? static_cast<std::uint32_t>(std::min<std::uint64_t>(buffer->GetSize() / a_unitBytes, UINT32_MAX)) : 0u;
		}

		/** @brief A per-object buffer's first units (CS_DCLF_TABLE_START=small: a few, to test growth); RequireSceneTables grows it. */
		std::uint32_t InitialObjectUnits()
		{
			return SwitchValue(Switch::TableStart) == "small" ? 64u : kInitialObjects;
		}
	}
}

namespace DCLF::Draws
{
	ShadowLatchLayout InitialShadowLatchLayout()
	{
		const bool smallSlots = SwitchValue(Switch::TableStart) == "small";
		return { smallSlots ? 2u : kInitialShadowViewSlots, smallSlots ? 16u : kInitialShadowKeySlots, smallSlots ? 1u : kInitialShadowRasterStates,
			smallSlots ? 1u : kInitialSunProcesses };
	}

	MainLayout InitialMainLayout()
	{
		const bool smallTables = SwitchValue(Switch::TableStart) == "small";
		MainLayout layout;
		layout.sequenceDraws = smallTables ? 64u : kInitialSequenceDraws;
		layout.sequenceDecals = smallTables ? 16u : kInitialDecalDraws;
		return layout;
	}

	bool GrowableRows::Create(std::uint32_t a_stride, std::uint32_t a_rows, const char* a_name, org::services::GrowthPolicy a_policy, org::services::GrowableContents a_contents,
		org::services::GrowableCapture a_capture)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return false;
		stride = a_stride;
		name = a_name;
		buffer = MakeVersioned(DeviceBuffer(std::uint64_t(std::max(a_rows, 1u)) * stride, a_name),
			{ .extent = { 0, a_stride, 0 }, .policy = a_policy, .contents = a_contents, .capture = std::move(a_capture) });
		return AddressOf(host->GetDesc().device, *buffer->Get()) != 0;
	}

	namespace
	{
		/**
		 * @brief One object slot's share of the scene draw bound (one per object, or per skin partition; any epoch's draws fit in
		 * it): its draws and the pipeline slot whose Z-prepass bucket they count toward. Decals count toward none: the depth
		 * segment gets no decals.
		 */
		std::pair<std::uint32_t, std::uint32_t> DrawShareOf(const SceneTables& a_tables, std::size_t a_slot, bool a_partitioned)
		{
			const auto& object = a_tables.objects[a_slot];
			if (object.flags & kObjectFree)
				return { 0u, DrawBoundStore::kNoPipeline };
			const std::uint32_t produced = PartitionDraws(a_partitioned ? a_tables.skinPartitions[a_slot] : 0u);
			return { produced, (object.flags & kObjectDecal) ? DrawBoundStore::kNoPipeline : object.pipelineIndex };
		}

		/** @brief What a_slot casts with (DrawBoundStore::ShadowShare), given its draws. */
		DrawBoundStore::ShadowShare ShadowShareOf(const SceneTables& a_tables, std::size_t a_slot, std::uint32_t a_produced)
		{
			DrawBoundStore::ShadowShare out;
			const auto& object = a_tables.objects[a_slot];
			if ((object.flags & kObjectFree) || !a_produced)
				return out;
			out.produced = a_produced;
			out.caster = !(object.flags & kObjectNoShadow);
			out.technique = a_slot < a_tables.shadowTechnique.size() ? a_tables.shadowTechnique[a_slot] : 0u;
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
				out.occlusion[v] = a_slot < a_tables.occlusionTechnique[v].size() ? a_tables.occlusionTechnique[v][a_slot] : 0u;
			out.rasterFlags = (object.flags & kObjectTwoSided) ? kRasterTwoSided : 0u;
			out.vertexLayout = object.geometryIndex < a_tables.geometries.size() ? VertexLayoutOf(a_tables.geometries[object.geometryIndex].vertexDesc) : 0ull;
			return out;
		}

		/** @brief Adds a_share's draws to its key in each mode it draws in, or takes them out. */
		void CountShadowShare(ModeKeyDraws& a_out, const DrawBoundStore::ShadowShare& a_share, bool a_add)
		{
			if (!a_share.produced)
				return;
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
				ShadowPipelineKey key;
				if (!a_share.KeyOf(m, key))
					continue;
				if (a_add) {
					a_out[m][key] += a_share.produced;
				} else if (const auto it = a_out[m].find(key); it != a_out[m].end() && (it->second -= a_share.produced) == 0) {
					a_out[m].erase(it);
				}
			}
		}

		/**
		 * @brief Adds shadow view slot buffers up to a_slots at the sizes the revision code asked for (their requirements grow them with
		 * the other slots'). Render thread: the passes declare them (a graph build).
		 */
		void AddShadowViewSlots(ShadowResources& a_state, std::uint32_t a_slots, std::uint32_t a_draws, std::uint32_t a_words)
		{
			const std::uint32_t draws = a_draws;
			for (auto s = static_cast<std::uint32_t>(a_state.sequences.size()); s < a_slots; ++s) {
				// A unit: a draw of every class (kShadowClasses ranges).
				a_state.sequences.push_back(MakeVersioned(CreateWords(kShadowClasses * std::uint64_t(draws) * sizeof(DrawSequence) / 4, true,
					fmt::format("cs.dclf.shadow.sequences{}", s).c_str()), { .extent = { 0, kShadowClasses * sizeof(DrawSequence), 4 } }));
				a_state.count.push_back(CreateWords(kCountWords, true, fmt::format("cs.dclf.shadow.draw-count{}", s).c_str()));
				// A word per key slot: the latch layout's key slots exactly (doubled there).
				a_state.bucketCounts.push_back(MakeVersioned(CreateWords(std::max(a_words, 64u), true, fmt::format("cs.dclf.shadow.bucket-counts{}", s).c_str()),
					{ .extent = { 0, 4, 4 }, .policy = org::services::GrowthPolicy::Headroom(0) }));
			}
		}

		/** @brief The pipeline statistics query pool and its per-slot readbacks (PassStats::Enabled); none without the pool. */
		void CreatePassStats(Resources& a_state, org::PersistentGraphHost& a_host, rhi::Device a_device)
		{
			auto passStats = std::make_shared<PassStats>();
			const std::uint32_t slots = a_host.FrameSlots();
			rhi::QueryPoolDesc desc{};
			desc.type = rhi::QueryType::PipelineStatistics;
			desc.count = slots * PassStats::kKinds;
			desc.statsMask = rhi::PipelineStatBits::PS_IAVertices | rhi::PipelineStatBits::PS_IAPrimitives | rhi::PipelineStatBits::PS_VSInvocations |
			                 rhi::PipelineStatBits::PS_PSInvocations;
			desc.requireAllStats = true;
			if (a_device.CreateQueryPool(desc, passStats->pool) != rhi::Result::Ok || !passStats->pool) {
				logger::warn("[DCLF] pass statistics: no pipeline statistics query pool");
			} else {
				for (std::uint32_t i = 0; i < slots; ++i)
					passStats->readback.push_back(org::Buffer::CreateShared(rhi::HeapType::Readback, std::uint64_t(PassStats::kKinds) * PassStats::kFields * sizeof(std::uint64_t)));
				passStats->written.resize(slots);
				a_state.passStats = std::move(passStats);
				logger::info("[DCLF] pass statistics on: {} frame slots", slots);
			}
		}

		/** @brief The foliage pixel record at the target's size (Policy::RecordFoliagePixels). */
		void CreateFoliageRecord(Resources& a_state, org::PersistentGraphHost& a_host, rhi::Device a_device, const D3D11_TEXTURE2D_DESC& a_target)
		{
			auto foliage = std::make_shared<Resources::FoliageRecord>();
			foliage->width = a_target.Width;
			foliage->height = a_target.Height;
			const std::uint64_t pixels = std::uint64_t(a_target.Width) * a_target.Height;
			for (std::uint32_t h = 0; h < 2; ++h) {
				foliage->ids[h] = CreateWords(pixels, true, fmt::format("cs.dclf.foliage-ids{}", h).c_str());
				foliage->colours[h] = CreateWords(pixels * kFoliageColourWords, true, fmt::format("cs.dclf.foliage-colours{}", h).c_str());
				foliage->idsAddress[h] = AddressOf(a_device, *foliage->ids[h]);
				foliage->coloursAddress[h] = AddressOf(a_device, *foliage->colours[h]);
			}
			foliage->results = CreateWords(kFoliageResultWords, true, "cs.dclf.foliage-results");
			foliage->owners = CreateWords(pixels * 2, true, "cs.dclf.foliage-owners");
			foliage->ownersIndex = foliage->owners->GetUAVShaderVisibleInfo(0).slot.index;
			foliage->resultsAddress = AddressOf(a_device, *foliage->results);
			// The record stands only with the observers' compare pass.
			bool ready = false;
			Diagnostics::Notify(Diagnostics::FoliageRecordMade{ *foliage, a_device, a_host.FrameSlots(), ready });
			if (ready)
				a_state.foliage = std::move(foliage);
		}

		/**
		 * @brief The engine's refraction normals (kREFRACTION_NORMALS): FUN_1414b4860 binds them as RT0 for group 5, ISRefraction samples
		 * them (Src1Tex). Imported in the engine RTV's format; without them DCLF draws no refraction.
		 */
		void ImportRefractionNormals(Resources& a_state, rhi::Device a_device)
		{
			if (auto* renderer = globals::game::renderer) {
				const auto& normals = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kREFRACTION_NORMALS];
				DxvkOrgInteropResourceInfo normalsInfo{};
				if (normals.texture && normals.RTV && RenderGraphRuntime::Get().DescribeResource(normals.texture, normalsInfo) && normalsInfo.kind == DXVK_ORG_INTEROP_RESOURCE_IMAGE) {
					D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
					normals.RTV->GetDesc(&rtvDesc);
					D3D11_TEXTURE2D_DESC textureDesc{};
					normals.texture->GetDesc(&textureDesc);
					org::TextureDescription normalsDesc{};
					normalsDesc.imageDimensions.push_back({ textureDesc.Width, textureDesc.Height, 0, 0 });
					normalsDesc.format = rhi::helpers::ToRHI(rtvDesc.Format);
					normalsDesc.channels = 4;
					normalsDesc.hasRTV = true;
					normalsDesc.rtvFormat = normalsDesc.format;
					a_state.refractionNormals = ImportImage(a_device, normalsInfo.image, normalsDesc, "DCLF refraction normals");
					a_state.refractionFormat = a_state.refractionNormals ? rtvDesc.Format : DXGI_FORMAT_UNKNOWN;
				}
				if (!a_state.refractionNormals)
					logger::warn("[DCLF] the refraction normals (kREFRACTION_NORMALS) could not be imported; DCLF draws no refraction");
			}
		}

		/**
		 * @brief The HZB: mip 0 is half the depth's next power of two, so a mip is chosen from a screen extent by log2 alone. Padded
		 * texels are filled with the far plane, which suppresses culling rather than causing it.
		 */
		void CreateHzb(Resources& a_state, rhi::Device a_device)
		{
			auto nextPowerOfTwo = [](std::uint32_t a_value) {
				std::uint32_t result = 1;
				while (result < a_value)
					result <<= 1;
				return result;
			};
			a_state.hzbWidth = std::max(1u, nextPowerOfTwo(a_state.width) / 2);
			a_state.hzbHeight = std::max(1u, nextPowerOfTwo(a_state.height) / 2);
			a_state.hzbMips = 1;
			for (std::uint32_t size = std::max(a_state.hzbWidth, a_state.hzbHeight); size > 1; size >>= 1)
				++a_state.hzbMips;

			org::TextureDescription hzbDesc{};
			for (std::uint32_t mip = 0; mip < a_state.hzbMips; ++mip)
				hzbDesc.imageDimensions.push_back({ std::max(1u, a_state.hzbWidth >> mip), std::max(1u, a_state.hzbHeight >> mip), 0, 0 });
			hzbDesc.format = rhi::Format::R32_Float;
			hzbDesc.channels = 1;
			hzbDesc.hasUAV = true;
			hzbDesc.uavFormat = hzbDesc.format;
			hzbDesc.hasSRV = true;
			hzbDesc.srvFormat = hzbDesc.format;
			a_state.hzb = org::PixelBuffer::CreateSharedUnmaterialized(hzbDesc);
			a_state.hzb->SetName("cs.dclf.hzb");
			a_state.hzbCounter = CreateWords(1, true, "cs.dclf.hzb-counter");

			a_state.hzbProgram = ComputeProgram::Load(a_device, { .source = kHzbShader, .constantWords = kHzbConstantWords });
			if (!a_state.hzbProgram) {
				logger::warn("[DCLF] The HZB compute program could not be created; occlusion culling stays off");
				a_state.hzb.reset();
			}
		}
	}
}

namespace DCLF
{
	bool IndirectDraws::Impl::Setup(const Capture& a_capture, bool a_depthOnly)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return NotReady(0, "no render graph");
		if (a_depthOnly) {
			// The native depth pass binds no colour targets: the Z-prepass reuses the main pass's resources (none on the first frame).
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
		if (!EnsureSceneBuffers(device)) {
			logger::error("[DCLF] The face positions buffer has no device address");
			return false;
		}
		state->scene = scene;
		if (!CreateMainBuffers(*state, *host, device))
			return false;

		state->width = target.Width;
		state->height = target.Height;
		if (Policy::RecordFoliagePixels())
			CreateFoliageRecord(*state, *host, device, target);

		if (!ImportMainTargets(*state, a_capture, device, target, targets))
			return false;
		ImportRefractionNormals(*state, device);

		CreateHzb(*state, device);

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
		// The views draw from the main resources: rebuilt at their next import, before the graph is built (on the render thread).
		for (const auto kind : { ViewKind::Reflection, ViewKind::FirstPerson })
			if (const auto& view = View(kind).resources) {
				view->main = state;
				view->rebuildPending = true;
			}
		PostRevisionInputs();
		// The graph is rebuilt with the new resources on its next epoch.
		host->AddExtension(kExtensionId, [state] { return MakeMainOpaqueExtension(state); });
		logger::info("[DCLF] Main-pass graph resources: {} targets {}x{}, depth format {}", targets.colorCount, width, height, static_cast<int>(targets.depth));
		return true;
	}

	bool IndirectDraws::Impl::CreateMainBuffers(Resources& a_state, org::PersistentGraphHost& a_host, rhi::Device a_device)
	{
		// The sequence ranges (SequenceSlots) at the first layout's sizes, required to every draw the scene can produce (RequireMain);
		// sized to the layout exactly (its ranges grow by doubling).
		const bool smallTables = SwitchValue(Switch::TableStart) == "small";
		const MainLayout firstLayout = InitialMainLayout();
		a_state.sequences = MakeVersioned(CreateWords(SequenceSlots(firstLayout.sequenceDraws, firstLayout.sequenceDecals) * sizeof(DrawSequence) / 4, true, "cs.dclf.sequences"),
			{ .extent = { 0, sizeof(DrawSequence), 4 }, .policy = org::services::GrowthPolicy::Headroom(0) });
		a_state.count = CreateWords(kCountWords, true, "cs.dclf.draw-count");
		// One word per object: the depth segment's culling verdict, so the colour segment draws exactly the same set. The per-object
		// buffers start at the scene's objects (and grow with them: RequireSceneTables).
		const std::uint32_t objects = InitialObjectUnits();
		a_state.visibility = MakeVersioned(CreateWords(objects, true, "cs.dclf.visibility"), { .extent = { 0, 4, 4 } });
		a_state.frustum = MakeVersioned(CreateWords(objects, true, "cs.dclf.frustum"), { .extent = { 0, 4, 4 } });
		if (PassStats::Enabled())
			CreatePassStats(a_state, a_host, a_device);
		a_state.preprocessMain = PreprocessStates::Create(a_device, a_host.FrameSlots(), "the main segment");
		// The forward epoch records in its own epoch, so it needs its own state list.
		a_state.preprocessForward = PreprocessStates::Create(a_device, a_host.FrameSlots(), "the forward epoch");
		a_state.buildDraws = ComputeProgram::Load(a_device, { .source = kBuildDrawsShader, .constantWords = kBuildDrawsConstantWords });
		if (!a_state.buildDraws)
			return NotReady(7, "the BuildDraws program could not be created");
		// Without the forward sort the forward epoch draws nothing.
		a_state.forwardSort = ComputeProgram::Load(a_device, { .source = kForwardSortShader, .constantWords = kForwardSortConstantWords });
		if (!a_state.forwardSort)
			logger::warn("[DCLF] the forward sort program could not be created: the forward epoch draws nothing");
		a_state.dispatchSignature = CreateDispatchSignature(a_device, a_state.buildDraws->layout->GetHandle());
		if (!a_state.dispatchSignature)
			return NotReady(7, "the BuildDraws dispatch signature could not be created");
		// The latch is the revision producer's. Sorting by pipeline: BuildDraws appends in thread order, so unsorted nearly every
		// sequence would switch pipeline.
		a_state.sort = DrawSort::Create(a_device, firstLayout.sequenceDraws);
		// The Z-prepass's bucket count words (required to the layout's buckets) and the scene's index pool.
		const GrowableSpec bucketWords{ .extent = { 0, 4, 4 }, .policy = org::services::GrowthPolicy::Doubling(64) };
		a_state.zBucketCounts[0] = MakeVersioned(CreateWords(64, true, "cs.dclf.z.bucket-counts"), bucketWords);
		a_state.zBucketCounts[1] = MakeVersioned(CreateWords(64, true, "cs.dclf.z.bucket-counts2"), bucketWords);
		a_state.pool = EnsureIndexPool(*scene, a_device, smallTables);
		if (!a_state.pool)
			return NotReady(7, "the index pool's copy program could not be created");
		// The draw-count buffer is read back for the culling counters, parity or not; the view covers every word (a short view
		// would read zeros that look clean).
		a_state.countD3D11 = WrapWords(*a_state.count, kCountWords * sizeof(std::uint32_t));
		a_state.frameConstants = DeviceBuffer(kFrameConstantBytes, "cs.dclf.frame-constants");
		a_state.frameConstantsAddress = AddressOf(a_device, *a_state.frameConstants);

		if (!a_state.frameConstantsAddress) {
			logger::error("[DCLF] Draw data buffers have no device address");
			return false;
		}
		return true;
	}

	bool IndirectDraws::Impl::ImportMainTargets(Resources& a_state, const Capture& a_capture, rhi::Device a_device, const D3D11_TEXTURE2D_DESC& a_target,
		const TargetFormats& a_targets)
	{
		// The main pass's targets and depth, imported.
		a_state.targetCount = a_targets.colorCount;
		for (std::uint32_t i = 0; i < a_targets.colorCount; ++i) {
			DxvkOrgInteropResourceInfo info{};
			if (!a_capture.targets[i] || !RenderGraphRuntime::Get().DescribeResource(a_capture.targets[i].get(), info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
				return NotReady(5, "a main-pass target cannot be described");
			org::TextureDescription desc{};
			desc.format = rhi::helpers::ToRHI(a_targets.colors[i]);
			desc.channels = 4;
			desc.hasRTV = true;
			desc.rtvFormat = desc.format;
			a_state.native[i] = ImportImage(a_device, info.image, desc, "DCLF native target");
			if (!a_state.native[i])
				return NotReady(6, "a main-pass target could not be imported");
		}
		DxvkOrgInteropResourceInfo info{};
		if (!a_capture.depth || !RenderGraphRuntime::Get().DescribeResource(a_capture.depth.get(), info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
			return NotReady(10, "the main-pass depth cannot be described");
		// Attachable and readable from one import: two imports would be unrelated resources to the graph, with no barrier
		// between the depth draws and the HZB read (which would then reduce the cleared far plane).
		org::TextureDescription depthDesc{};
		depthDesc.imageDimensions.push_back({ a_target.Width, a_target.Height, 0, 0 });
		depthDesc.format = rhi::helpers::ToRHI(a_targets.depth);
		depthDesc.channels = 1;
		depthDesc.hasDSV = true;
		depthDesc.dsvFormat = depthDesc.format;
		depthDesc.hasSRV = true;
		depthDesc.srvFormat = DepthReadFormat(depthDesc.format);
		a_state.nativeDepth = ImportImage(a_device, info.image, depthDesc, "DCLF native depth");
		if (!a_state.nativeDepth)
			return NotReady(11, "the main-pass depth could not be imported");
		// Terrain Blending's terrain depth shares the main depth's description; the Z-prepass draws near terrain blocks into it
		// as that feature's depth hook does.
		if (const auto& terrainBlending = globals::features::terrainBlending; terrainBlending.loaded && terrainBlending.terrainDepth.texture) {
			DxvkOrgInteropResourceInfo terrainInfo{};
			if (!RenderGraphRuntime::Get().DescribeResource(terrainBlending.terrainDepth.texture, terrainInfo) || terrainInfo.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
				return NotReady(12, "Terrain Blending's terrain depth cannot be described");
			a_state.terrainDepth = ImportImage(a_device, terrainInfo.image, depthDesc, "DCLF terrain blending depth");
			if (!a_state.terrainDepth)
				return NotReady(12, "Terrain Blending's terrain depth could not be imported");
		}
		return true;
	}

	ScannedDrawBound Draws::ScanDrawBound(const SceneTables& a_tables)
	{
		ScannedDrawBound out;
		std::uint64_t draws = 0;
		const std::size_t objects = a_tables.objects.size();
		const bool partitioned = a_tables.skinPartitions.size() >= objects;
		out.perPipeline.assign(a_tables.pipelines.size(), 0u);
		for (std::size_t o = 0; o < objects; ++o) {
			const auto [produced, pipeline] = DrawShareOf(a_tables, o, partitioned);
			draws += produced;
			if (pipeline < out.perPipeline.size())
				out.perPipeline[pipeline] += produced;
			CountShadowShare(out.modeKeys, ShadowShareOf(a_tables, o, produced), true);
		}
		out.draws = static_cast<std::uint32_t>(std::min<std::uint64_t>(draws, UINT32_MAX));
		return out;
	}

	void IndirectDraws::Impl::UpdateDrawBound(const SceneTables& a_tables, std::uint32_t a_generation)
	{
		auto& s = producer.drawBound;
		const std::size_t count = a_tables.objects.size();
		const bool partitioned = a_tables.skinPartitions.size() >= count;
		const std::uint32_t generation = a_generation;  // the tables' own
		constexpr auto kNoPipeline = DrawBoundStore::kNoPipeline;
		auto set = [&](std::size_t a_slot) {
			const auto [produced, pipeline] = DrawShareOf(a_tables, a_slot, partitioned);
			// The casting share is unchanged by most logged changes (placement, shading).
			if (auto share = ShadowShareOf(a_tables, a_slot, produced); !(share == s.shadow[a_slot])) {
				CountShadowShare(s.modeKeyDraws, s.shadow[a_slot], false);
				CountShadowShare(s.modeKeyDraws, share, true);
				s.shadow[a_slot] = share;
				++s.shadowVersion;
			}
			s.draws -= s.produced[a_slot];
			if (s.pipeline[a_slot] != kNoPipeline)
				s.perPipeline[s.pipeline[a_slot]] -= s.produced[a_slot];
			s.produced[a_slot] = produced;
			s.pipeline[a_slot] = pipeline;
			s.draws += produced;
			if (pipeline != kNoPipeline) {
				if (pipeline >= s.perPipeline.size())
					s.perPipeline.resize(std::size_t(pipeline) + 1, 0u);
				s.perPipeline[pipeline] += produced;
			}
		};
		++s.updates;
		// Every slot again on new tables, a lost log, fewer slots, or the partition column coming or going (unlogged).
		if (!s.cursor.Continues(a_tables.changeLog, generation) || s.produced.size() > count || s.partitioned != partitioned) {
			s.produced.assign(count, 0u);
			s.pipeline.assign(count, kNoPipeline);
			s.perPipeline.clear();
			s.shadow.assign(count, {});
			for (auto& keys : s.modeKeyDraws)
				keys.clear();
			s.draws = 0;
			s.partitioned = partitioned;
			for (std::size_t o = 0; o < count; ++o)
				set(o);
			s.cursor.Restart(generation);
			++s.resyncs;
			++s.shadowVersion;
		} else {
			// Slots the tables grew by.
			if (const std::size_t first = s.produced.size(); first < count) {
				s.produced.resize(count, 0u);
				s.pipeline.resize(count, kNoPipeline);
				s.shadow.resize(count);
				for (std::size_t o = first; o < count; ++o)
					set(o);
			}
			// Any change kind: re-counting an unchanged share costs nothing.
			for (const auto& change : s.cursor.Unread(a_tables.changeLog)) {
				if (change.slot >= count)
					continue;
				set(change.slot);
				++s.changes;
			}
		}
		s.cursor.Advance(a_tables.changeLog);
		Diagnostics::Notify(Diagnostics::StoreUpdated<DrawBoundStore>{ s, a_tables, producer.frame });
	}

	bool IndirectDraws::Impl::EnsureSceneBuffers(rhi::Device a_device)
	{
		if (scene)
			return true;
		auto buffers = std::make_shared<SceneBuffers>();
		const bool smallStart = SwitchValue(Switch::TableStart) == "small";
		using org::services::GrowableContents;
		const std::uint32_t objects = InitialObjectUnits();
		// Face positions are a vertex buffer; its address and capacity are the sealed set's. Every table grows by doubling
		// (RequireSceneTables). The object records, extras and geometry table are the payload's (PayloadVersions).
		std::uint32_t unused = 0;
		buffers->facePositions = MakeVersioned(DeviceBuffer(std::uint64_t(smallStart ? 1024u : kInitialFaceVertices) * 16, "cs.dclf.face-positions"), { .extent = { 0, 16, 0 } });
		if (!AddressOf(a_device, *buffers->facePositions->Get()))
			return false;
		// Without the tree wind program, trees keep the wind they joined with (the wind buffers, t124, exist and stay zero).
		buffers->treeWind = ComputeProgram::Load(a_device, { .source = kTreeWindShader, .constantWords = kTreeWindConstantWords });
		std::uint32_t trees = 0;
		if (buffers->treeWind) {
			trees = smallStart ? 4u : kInitialTrees;
			buffers->trees = MakeVersioned(StructuredBuffer(trees, sizeof(TreeStatic), "cs.dclf.trees", unused), { .extent = { 0, sizeof(TreeStatic), sizeof(TreeStatic) } });
			// New clocks restart from each tree's static row.
			buffers->treeClocks = MakeVersioned(StructuredBuffer(trees, sizeof(TreeClock), "cs.dclf.tree-clocks", unused, true),
				{ .extent = { 0, sizeof(TreeClock), sizeof(TreeClock) } });
			buffers->treeFrameBuffer = StructuredBuffer(1, sizeof(TreeWindFrameRow), "cs.dclf.tree-frame", unused);
		} else {
			logger::warn("[DCLF] The tree wind program could not be created; trees keep the wind they joined with");
		}
		// The nodeless trees' entry, then a unit per tree slot; every version zero-filled (a zero generation is never a listing's).
		for (std::uint32_t h = 0; h < 2; ++h)
			buffers->treeWindRows[h] = MakeVersioned(StructuredBuffer((trees + 1) * kTreeWindEntryRows, 16, h ? "cs.dclf.tree-wind1" : "cs.dclf.tree-wind0", unused, true),
				{ .extent = { kTreeWindEntryRows * 16, kTreeWindEntryRows * 16, 16 }, .contents = GrowableContents::Zero });
		// The features' GPU stages (Api::GpuStage::setup: their programs; a null state keeps a stage out of the graph).
		buffers->stages = Extensions::SetUpGpuStages(a_device);
		// Without the fade state program, fades stay the CPU's.
		buffers->fadeState = ComputeProgram::Load(a_device, { .source = kFadeStateShader, .constantWords = kFadeStateConstantWords });
		if (buffers->fadeState) {
			// By root slot. New state is reseeded from the static rows; the frame-ahead outputs are zero-filled.
			const std::uint32_t roots = smallStart ? 4u : kInitialFadeRoots;
			buffers->fadeRoots = MakeVersioned(StructuredBuffer(roots, sizeof(FadeRootStatic), "cs.dclf.fade-roots", unused),
				{ .extent = { 0, sizeof(FadeRootStatic), sizeof(FadeRootStatic) } });
			buffers->fadeStates = MakeVersioned(StructuredBuffer(roots, sizeof(FadeNodeState), "cs.dclf.fade-states", unused, true),
				{ .extent = { 0, sizeof(FadeNodeState), sizeof(FadeNodeState) } });
			for (std::uint32_t h = 0; h < 2; ++h)
				buffers->fadeStatesOut[h] = MakeVersioned(StructuredBuffer(roots, sizeof(FadeNodeState), h ? "cs.dclf.fade-states-out1" : "cs.dclf.fade-states-out0", unused, true),
					{ .extent = { 0, sizeof(FadeNodeState), sizeof(FadeNodeState) }, .contents = GrowableContents::Zero });
			buffers->fadeFrameBuffer = StructuredBuffer(1, sizeof(FadeFrame), "cs.dclf.fade-frame", unused);
			buffers->portalPrograms = MakeVersioned(CreateWords(smallStart ? kPortalHeaderBytes / 4 : 4096u, false, "cs.dclf.portal-programs"), { .extent = { 0, 4, 4 } });
			buffers->fadeRootPrograms = MakeVersioned(StructuredBuffer(roots, sizeof(std::uint32_t), "cs.dclf.fade-root-programs", unused), { .extent = { 0, 4, 4 } });
			buffers->fadeAnimated = MakeVersioned(StructuredBuffer(roots, sizeof(std::uint32_t), "cs.dclf.fade-animated", unused), { .extent = { 0, 4, 4 } });
			buffers->fadeLog = StructuredBuffer(kFadeLogEntries, sizeof(FadeLogEntry), "cs.dclf.fade-log", unused, true);
			// The fade write-back: events, the roots' reported milestones, and a staging set (a host buffer per frame slot).
			if (auto* host = RenderGraphRuntime::Get().Host()) {
				const std::uint32_t events = smallStart ? 4u : kInitialFadeEvents;
				buffers->fadeEvents = MakeVersioned(CreateWords(FadeWriteBack::BytesFor(events) / 4, true, "cs.dclf.fade-events"),
					{ .extent = { kFadeEventHeaderWords * 4, kFadeEventWords * 4, 4 } });
				buffers->fadeReported = MakeVersioned(StructuredBuffer(roots, 2 * sizeof(std::uint32_t), "cs.dclf.fade-reported", unused, true),
					{ .extent = { 0, 8, 8 }, .contents = GrowableContents::Zero });
				auto writeBack = std::make_shared<FadeWriteBack>();
				writeBack->frameSlots = host->FrameSlots();
				writeBack->armed.resize(writeBack->frameSlots);
				writeBack->AddSet(events);
				buffers->fadeWriteBack = std::move(writeBack);
			}
			// The visibility readback (the normal path writes the listed roots' kAccumulated from it): events, each root's and object's
			// reported verdict, and a staging set.
			if (auto* host = RenderGraphRuntime::Get().Host()) {
				buffers->visibilityEventsProgram = ComputeProgram::Load(a_device, { .source = kVisibilityEventsShader, .constantWords = kVisibilityEventsConstantWords });
				if (buffers->visibilityEventsProgram) {
					const std::uint32_t events = smallStart ? 4u : kInitialVisibilityEvents;
					buffers->visibilityEvents = MakeVersioned(CreateWords(VisibilityEventBytes(events) / 4, true, "cs.dclf.visibility-events"),
						{ .extent = { kVisibilityEventHeaderWords * 4, 16, 4 } });
					buffers->visibilityRootReported = MakeVersioned(StructuredBuffer(roots, 2 * sizeof(std::uint32_t), "cs.dclf.visibility-root-reported", unused, true),
						{ .extent = { 0, 8, 8 }, .contents = GrowableContents::Zero });
					buffers->visibilityObjectReported = MakeVersioned(StructuredBuffer(objects, sizeof(std::uint32_t), "cs.dclf.visibility-object-reported", unused, true),
						{ .extent = { 0, 4, 4 }, .contents = GrowableContents::Zero });
					auto readback = std::make_shared<VisibilityReadback>();
					readback->frameSlots = host->FrameSlots();
					readback->armed.resize(readback->frameSlots);
					readback->AddSet(events);
					buffers->visibilityReadback = std::move(readback);
				} else {
					logger::warn("[DCLF] The visibility events program could not be created; the stood-in roots' kAccumulated is not written (T7b2)");
				}
			}
		} else {
			logger::warn("[DCLF] The fade state program could not be created; fades stay the CPU's");
		}
		// Without the tree LOD cull program, tree LOD stays the engine's.
		buffers->treeLodCull = ComputeProgram::Load(a_device, { .source = kTreeLodCullShader, .constantWords = kTreeLodCullConstantWords });
		if (buffers->treeLodCull) {
			// By shape slot: a row, its records, and a phase 1 and a retest entry per record after the visible list's header.
			const std::uint32_t shapes = smallStart ? 4u : kInitialTreeLodShapes;
			buffers->treeLodShapes = MakeVersioned(CreateWords(TreeLodShapeWords(shapes), false, "cs.dclf.tree-lod-shapes"), { .extent = { 0, sizeof(TreeLod::ShapeRow), 4 } });
			buffers->treeLodInstances = MakeVersioned(CreateWords(TreeLodInstanceWords(shapes), false, "cs.dclf.tree-lod-instances"),
				{ .extent = { 0, TreeLod::kMaxGroupInstances * sizeof(TreeLod::Instance), 4 } });
			buffers->treeLodMeshes = MakeVersioned(CreateWords(TreeLodMeshWords(smallStart ? 4u : kInitialTreeLodMeshes), false, "cs.dclf.tree-lod-meshes"),
				{ .extent = { 0, sizeof(TreeLod::MeshRow), 4 } });
			buffers->treeLodDraw = CreateWords(sizeof(TreeLod::DrawRow) / 4, false, "cs.dclf.tree-lod-draw");
			buffers->treeLodVisible = MakeVersioned(CreateWords(TreeLodVisibleWords(shapes), true, "cs.dclf.tree-lod-visible"),
				{ .extent = { TreeLod::kVisibleHeaderWords * 4, 2 * TreeLod::kMaxGroupInstances * 4, 4 } });
			buffers->treeLodDrawAddress = AddressOf(a_device, *buffers->treeLodDraw);
			if (!AddressOf(a_device, *buffers->treeLodShapes->Get()) || !AddressOf(a_device, *buffers->treeLodInstances->Get()) ||
				!AddressOf(a_device, *buffers->treeLodMeshes->Get()) || !buffers->treeLodDrawAddress || !AddressOf(a_device, *buffers->treeLodVisible->Get())) {
				logger::warn("[DCLF] Tree LOD's tables have no device address; tree LOD stays native");
				buffers->treeLodCull = nullptr;
			}
			if (auto* host = RenderGraphRuntime::Get().Host(); host && buffers->treeLodCull) {
				auto counts = std::make_shared<SceneBuffers::TreeLodCounts>();
				for (std::uint32_t i = 0; i < host->FrameSlots(); ++i)
					counts->readback.push_back(org::Buffer::CreateShared(rhi::HeapType::Readback, sizeof(TreeLod::VisibleHeader)));
				counts->filled.assign(host->FrameSlots(), 0u);
				buffers->treeLodCounts = std::move(counts);
			}
		} else {
			logger::warn("[DCLF] The tree LOD cull program could not be created; tree LOD stays native");
		}
		scene = std::move(buffers);
		return true;
	}

	void IndirectDraws::Impl::ReserveMainLatch(std::uint32_t a_cascades, std::uint32_t a_shadowVolumes, std::uint32_t a_buckets)
	{
		auto& latches = producer.latches;
		auto& layout = latches.main;
		if (!latches.mainLaidOut || (a_cascades <= layout.cascades && a_shadowVolumes <= layout.shadowVolumes && a_buckets <= layout.buckets))
			return;
		if (a_buckets > layout.buckets) {
			const std::uint32_t grown = org::services::GrowthPolicy::Doubling(64).NextAs(layout.buckets, a_buckets);
			logger::info("[DCLF] Z-prepass buckets in the main latch: {} grown to {}", layout.buckets, grown);
			layout.buckets = grown;
		}
		if (a_cascades > layout.cascades) {
			const std::uint32_t grown = org::services::GrowthPolicy::Doubling().NextAs(layout.cascades, a_cascades);
			logger::info("[DCLF] sun cascades in the main latch: {} grown to {}", layout.cascades, grown);
			layout.cascades = grown;
		}
		if (a_shadowVolumes > layout.shadowVolumes) {
			const std::uint32_t grown = org::services::GrowthPolicy::Doubling().NextAs(layout.shadowVolumes, a_shadowVolumes);
			logger::info("[DCLF] local shadow light volumes in the main latch: {} grown to {}", layout.shadowVolumes, grown);
			layout.shadowVolumes = grown;
		}
		// A new layout: the next seal makes a new block (RequireLatches). Every execution writes its frame slot's region whole; frames in
		// flight keep the block their set names.
	}

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
		state->constants = DeviceBuffer(kShadowConstantBytes, "cs.dclf.shadow.constants");
		// The shadow commit latches into the constants alone, known before any graph declares the pass.
		state->latchedTargets.store(std::make_shared<const std::vector<const void*>>(1, state->constants.get()), std::memory_order_release);
		if (!EnsureSceneBuffers(device)) {
			shadowSetupFailed = true;
			return ShadowNotReady(2, "no device address for the face positions");
		}
		state->scene = scene;
		// The first view slots, as many as the initial latch layout (the latch is the producer's).
		const bool smallSlots = SwitchValue(Switch::TableStart) == "small";
		const auto firstLayout = InitialShadowLatchLayout();
		AddShadowViewSlots(*state, firstLayout.viewSlots, 64u, 64u);
		// A row per view slot: the latch layout's slots exactly (doubled there).
		if (!state->viewBlocks.Create(static_cast<std::uint32_t>(kShadowViewSlotBytes), firstLayout.viewSlots, "cs.dclf.shadow.view-blocks",
				org::services::GrowthPolicy::Headroom(0))) {
			shadowSetupFailed = true;
			return ShadowNotReady(2, "no device address for the shadow view blocks");
		}
		// At the scene's objects; they grow with them (RequireSceneTables).
		const std::uint32_t objects = InitialObjectUnits();
		state->visibility = MakeVersioned(CreateWords(objects, true, "cs.dclf.shadow.visibility"), { .extent = { 0, 4, 4 } });
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
		state->pool = EnsureIndexPool(*state->scene, device, smallSlots);
		if (!state->pool) {
			shadowSetupFailed = true;
			return ShadowNotReady(1, "the index pool's copy program could not be created");
		}
		state->constantsAddress = AddressOf(device, *state->constants);
		if (!state->constantsAddress) {
			shadowSetupFailed = true;
			return ShadowNotReady(2, "no device address for the shadow buffers");
		}
		shadow = state;
		host->AddExtension(kShadowExtensionId, [state] { return MakeShadowExtension(state); });
		logger::info("[DCLF] shadow view graph resources created");
		PostRevisionInputs();
		return true;
	}

	void IndirectDraws::Impl::ServeRevisionRequests()
	{
		// Makes the shadow view slots the revision code asked for (render thread) and rebuilds the graph; the revision code waits.
		auto* host = RenderGraphRuntime::Get().Host();
		if (const auto request = shadowSlotsSlot.Take(); request && host && shadow && request->slots > shadow->sequences.size()) {
			AddShadowViewSlots(*shadow, request->slots, request->sequenceDraws, request->bucketCountWords);
			host->AddExtension(kShadowExtensionId, [state = shadow] { return MakeShadowExtension(state); });
			PostRevisionInputs();
		}
	}

	bool IndirectDraws::Impl::SetupView(ViewKind a_kind)
	{
		// A view's buffers, once the main resources and index pool exist; its latch, zeros and growth are the revision code's.
		auto& v = View(a_kind);
		if (v.resources)
			return true;
		if (v.setupFailed || !resources || !scene || !scene->pool)
			return false;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return false;
		const auto& traits = TraitsOf(a_kind);
		auto device = host->GetDesc().device;
		const auto name = [&](std::string_view a_name) { return fmt::format("{}.{}", traits.prefix, a_name); };
		auto fail = [&](const char* a_what) {
			v.setupFailed = true;
			logger::warn("[DCLF] the {} view's {}; it stays native", traits.name, a_what);
			return false;
		};
		auto state = std::make_shared<ViewResources>();
		state->traits = &traits;
		state->main = resources;
		state->buildDraws = ComputeProgram::Load(device, { .source = kBuildDrawsShader, .constantWords = kBuildDrawsConstantWords });
		if (state->buildDraws)
			state->dispatchSignature = CreateDispatchSignature(device, state->buildDraws->layout->GetHandle());
		if (!state->dispatchSignature)
			return fail("BuildDraws program could not be created");
		state->count = CreateWords(kCountWords, true, name("draw-count").c_str());
		// At their first sizes: the revision producer requires them to hold the view's plan (RequireView). Bucket count words: the latch
		// layout's buckets exactly (doubled there); a face's range of the sequences by doubling, from 64 draws.
		const std::uint32_t buckets = org::services::GrowthPolicy::Doubling(16).NextAs(0u, ViewLatchLayout::Initial(traits).buckets);
		for (std::uint32_t f = 0; f < traits.faces; ++f)
			state->bucketCounts[f] = MakeVersioned(CreateWords(buckets, true, name(traits.faces > 1 ? fmt::format("bucket-counts{}", f) : "bucket-counts").c_str()),
				{ .extent = { 0, 4, 4 }, .policy = org::services::GrowthPolicy::Headroom(0) });
		state->sequences = MakeVersioned(CreateWords(std::uint64_t(traits.faces) * 64u * sizeof(DrawSequence) / 4, true, name("sequences").c_str()),
			{ .extent = { 0, traits.faces * sizeof(DrawSequence), 4 }, .policy = org::services::GrowthPolicy::Doubling(64) });
		// The faces' tree LOD rows and lists, at the scene's shape slots: the lists grow with the scene's tables (RequireSceneTables).
		for (std::uint32_t f = 0; f < traits.faces && traits.trees; ++f) {
			state->treeRows[f] = CreateWords(sizeof(TreeLod::DrawRow) / 4, false, name(fmt::format("tree-row{}", f)).c_str());
			state->treeRowsAddress[f] = AddressOf(device, *state->treeRows[f]);
			if (!state->treeRowsAddress[f])
				return fail("tree LOD rows have no device address");
			if (scene->treeLodCull) {
				const std::uint32_t shapes = CurrentUnits(scene->treeLodShapes, sizeof(TreeLod::ShapeRow));
				const std::uint32_t words = TreeLod::kVisibleHeaderWords + shapes * TreeLod::kMaxGroupInstances;
				state->treeVisible[f] = MakeVersioned(CreateWords(words, true, name(fmt::format("tree-visible{}", f)).c_str()),
					{ .extent = { TreeLod::kVisibleHeaderWords * 4, TreeLod::kMaxGroupInstances * 4, 4 } });
			}
		}
		state->blocks = DeviceBuffer(std::uint64_t(traits.faces) * kViewBlockBytes, name(traits.blocks).c_str());
		state->blocksAddress = AddressOf(device, *state->blocks);
		if (!state->blocksAddress)
			return fail("blocks have no device address");
		// The forward part draws through the main colour signature with an explicit preprocess, as ForwardDrawPass.
		if (traits.forwardPart)
			state->preprocessForward = PreprocessStates::Create(device, host->FrameSlots(), "the first-person forward part");
		v.resources = std::move(state);
		logger::info("[DCLF] {} graph resources created", traits.name);
		PostRevisionInputs();
		return true;
	}

	bool IndirectDraws::Impl::ImportReflectionCube(ID3D11Texture2D* a_texture)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!reflection.resources || !host || !a_texture)
			return false;
		auto& r = *reflection.resources;
		if (r.target && r.targetTexture == a_texture)
			return true;
		DxvkOrgInteropResourceInfo info{};
		if (!RenderGraphRuntime::Get().DescribeResource(a_texture, info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
			return false;
		org::TextureDescription desc{};
		desc.format = rhi::helpers::ToRHI(reflection.targets.colour);
		desc.channels = 4;
		desc.hasRTV = true;
		desc.rtvFormat = desc.format;
		auto cube = ImportImage(host->GetDesc().device, info.image, desc, "DCLF reflection cube");
		if (!cube) {
			static std::uint32_t logged = 0;
			if (logged++ < 4)
				logger::warn("[DCLF] the reflection cube could not be imported (not in the general layout?); the faces stay native");
			return false;
		}
		// DCLF's own depth for the faces, at the engine's precision (its depth target 6 is R24G8).
		org::TextureDescription depthDesc{};
		depthDesc.imageDimensions.push_back({ info.image.extent.width, info.image.extent.height, 0, 0 });
		depthDesc.format = rhi::helpers::ToRHI(reflection.targets.depth);
		depthDesc.channels = 1;
		depthDesc.hasDSV = true;
		depthDesc.dsvFormat = depthDesc.format;
		r.depth = org::PixelBuffer::CreateSharedUnmaterialized(depthDesc);
		r.depth->SetName("cs.dclf.reflection.depth");
		r.target = std::move(cube);
		r.targetTexture = a_texture;
		r.width = info.image.extent.width;
		r.height = info.image.extent.height;
		host->AddExtension(kReflectionExtensionId, [state = reflection.resources] { return MakeViewExtension(state); });
		logger::info("[DCLF] reflection cube imported: {}x{}, {} faces, format {}", r.width, r.height, info.image.arrayLayers, static_cast<int>(reflection.targets.colour));
		PostRevisionInputs();
		return true;
	}

	bool IndirectDraws::Impl::ImportFirstPersonDepth()
	{
		// kPOST_ZPREPASS_COPY, the first-person colour pass's depth (FUN_1414cb6d0: FUN_140e4fe00(manager, 7, ...)), imported in its
		// DSV's format; the extension is added with it and again when the main resources change.
		auto* host = RenderGraphRuntime::Get().Host();
		auto* renderer = globals::game::renderer;
		if (!firstPerson.resources || !host || !renderer)
			return false;
		auto& r = *firstPerson.resources;
		const auto& copy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
		if (!copy.texture || !copy.views[0])
			return false;
		if (r.target && r.targetTexture == copy.texture && !r.rebuildPending)
			return true;
		if (!r.target || r.targetTexture != copy.texture) {
			DxvkOrgInteropResourceInfo info{};
			if (!RenderGraphRuntime::Get().DescribeResource(copy.texture, info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
				return false;
			D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
			copy.views[0]->GetDesc(&dsvDesc);
			org::TextureDescription desc{};
			desc.imageDimensions.push_back({ info.image.extent.width, info.image.extent.height, 0, 0 });
			desc.format = rhi::helpers::ToRHI(dsvDesc.Format);
			desc.channels = 1;
			desc.hasDSV = true;
			desc.dsvFormat = desc.format;
			auto imported = ImportImage(host->GetDesc().device, info.image, desc, "DCLF first-person depth (kPOST_ZPREPASS_COPY)");
			if (!imported) {
				static std::uint32_t logged = 0;
				if (logged++ < 4)
					logger::warn("[DCLF] kPOST_ZPREPASS_COPY could not be imported (not in the general layout?); first person stays native");
				return false;
			}
			r.target = std::move(imported);
			r.targetTexture = copy.texture;
			logger::info("[DCLF] first-person depth imported: {}x{}, format {}", info.image.extent.width, info.image.extent.height, static_cast<int>(dsvDesc.Format));
		}
		r.rebuildPending = false;
		host->AddExtension(kFirstPersonExtensionId, [state = firstPerson.resources] { return MakeViewExtension(state); });
		PostRevisionInputs();
		return true;
	}

	bool IndirectDraws::Impl::ImportShadowDepth(std::uint32_t a_index, std::uint32_t a_target)
	{
		auto* renderer = globals::game::renderer;
		if (!renderer)
			return false;
		const auto& data = renderer->GetDepthStencilData().depthStencils[a_target];
		return ImportDepthTexture(a_index, data.texture, data.views[0], a_target);
	}

	bool IndirectDraws::Impl::ImportDepthTexture(std::uint32_t a_index, ID3D11Texture2D* a_texture, ID3D11DepthStencilView* a_view, std::uint32_t a_label)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host || !shadow || a_index >= kShadowDepthTargets)
			return false;
		if (!a_texture || !a_view)
			return ShadowNotReady(3, "the shadow map has no texture");
		if (shadow->depth[a_index] && shadow->depthTexture[a_index] == a_texture)
			return true;
		DxvkOrgInteropResourceInfo info{};
		if (!RenderGraphRuntime::Get().DescribeResource(a_texture, info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
			return ShadowNotReady(4, "the shadow map cannot be described");
		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
		a_view->GetDesc(&dsvDesc);
		org::TextureDescription desc{};
		desc.format = rhi::helpers::ToRHI(dsvDesc.Format);
		desc.channels = 1;
		desc.hasDSV = true;
		desc.dsvFormat = desc.format;
		static constexpr const char* kNames[kShadowDepthTargets] = { "DCLF shadow maps (ESRAM)", "DCLF shadow maps", "DCLF volumetric shadow maps (ESRAM)",
			"DCLF Skylighting occlusion map", "DCLF precipitation occlusion mask" };
		auto imported = ImportImage(host->GetDesc().device, info.image, desc, kNames[a_index]);
		if (!imported)
			return ShadowNotReady(5, "the shadow map could not be imported (not in the general layout?)");
		shadow->depth[a_index] = std::move(imported);
		shadow->depthTexture[a_index] = a_texture;
		shadow->depthLayers[a_index] = info.image.arrayLayers;
		// A new resource for the passes to bind: the graph is rebuilt on the next epoch.
		host->AddExtension(kShadowExtensionId, [state = shadow] { return MakeShadowExtension(state); });
		logger::info("[DCLF] shadow map {} imported: {}x{}, {} slices, format {}", a_label, info.image.extent.width, info.image.extent.height,
			info.image.arrayLayers, static_cast<int>(dsvDesc.Format));
		return true;
	}
}
