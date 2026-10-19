// IndirectDraws' GPU resources: the main, scene, shadow and view resources created on the render thread (Setup, EnsureSceneBuffers,
// SetupShadow, SetupView), each versioned buffer registered with the scene's set (its extent, growth policy and contents), the
// scene's draw bound and the main latch the revision code keeps.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"

#include "Features/DrawcallLimitFix/Core/Extensions/DepthPyramid.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MainRanges.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"
#include "Features/DrawcallLimitFix/Core/Extensions/VertexStreams.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/ScheduleDump.h"

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

		/**
		 * @brief A view with frame constants of its own (its frame record's): its copies of the main frame buffers (a_main's shape), made
		 * again only when that shape changes, replaced whole (the revision producer reads them).
		 */
		void MatchViewFrameBuffers(ViewResources& a_view, const std::vector<FrameBuffer>& a_main)
		{
			if (!a_view.frameConstants)
				return;
			const auto held = a_view.frameBuffers.load(std::memory_order_acquire);
			if (held && held->size() == a_main.size() &&
				std::equal(a_main.begin(), a_main.end(), held->begin(), [](const auto& a, const auto& b) { return a.SameShape(b); }))
				return;
			auto copies = std::make_shared<std::vector<FrameBuffer>>(a_main);
			for (auto& frameBuffer : *copies) {
				std::uint32_t index = 0;
				frameBuffer.copy = StructuredBuffer(frameBuffer.elements, frameBuffer.stride,
					fmt::format("{}.frame-buffer.t{}", a_view.traits->prefix, frameBuffer.textureRegister).c_str(), index);
			}
			a_view.frameBuffers.store(std::move(copies), std::memory_order_release);
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
															  fmt::format("cs.dclf.shadow.sequences{}", s).c_str()),
					{ .extent = { 0, kShadowClasses * sizeof(DrawSequence), 4 } }));
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
		 * @brief The HZB, from the depth pyramid's producer (Api/DepthPyramid.h: its setup's texture, extent, levels and scratch, and its
		 * state). Without one, or without its texture, none: culling stays frustum-only, with no phase 2 passes.
		 */
		void SetUpHzb(Resources& a_state, rhi::Device a_device)
		{
			Api::DepthPyramid pyramid{};
			a_state.hzbState = Extensions::SetUpDepthPyramid(a_device, a_state.width, a_state.height, pyramid);
			a_state.hzb = std::move(pyramid.texture);
			a_state.hzbWidth = pyramid.width;
			a_state.hzbHeight = pyramid.height;
			a_state.hzbMips = pyramid.mips;
			a_state.hzbScratch = std::move(pyramid.scratch);
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
		// The frame graph buffers on the graph now (Api::FrameGraphBuffers: Integrations/LightLimitFix's), a bit per registration.
		std::uint32_t graphBuffers = 0;
		for (std::size_t b = 0; b < Extensions::RegisteredFrameGraphBuffers().size(); ++b) {
			std::array<std::uint32_t, Extensions::kFrameGraphBufferRegisters> indices;
			if (Extensions::FrameGraphBufferIndices(b, indices))
				graphBuffers |= 1u << b;
		}
		auto frameBuffers = FrameBuffersOf(a_capture, graphBuffers);
		const bool sameFrameBuffers = resources && resources->frameBuffers.size() == frameBuffers.size() &&
		                              std::equal(frameBuffers.begin(), frameBuffers.end(), resources->frameBuffers.begin(), [](const auto& a, const auto& b) { return a.SameShape(b); });
		if (resources && targets == formats && target.Width == width && target.Height == height && sameFrameBuffers &&
			resources->graphBuffers == graphBuffers)
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

		SetUpHzb(*state, device);

		state->graphBuffers = graphBuffers;
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
		// The views draw from the main resources: rebuilt at their next import, before the graph is built (on the render thread). A view with
		// frame constants of its own copies the main frame buffers at their shape.
		for (const auto& slot : kViewSlots)
			if (const auto& view = View(slot.view).resources) {
				view->main = state;
				view->rebuildPending = true;
				MatchViewFrameBuffers(*view, state->frameBuffers);
			}
		PostRevisionInputs();
		// The graph is rebuilt with the new resources on its next epoch.
		host->AddExtension(kExtensionId, [state] { return Diagnostics::ScheduleDumped(kExtensionId, MakeMainOpaqueExtension(state)); });
		logger::info("[DCLF] Main-pass graph resources: {} targets {}x{}, depth format {}", targets.colorCount, width, height, static_cast<int>(targets.depth));
		return true;
	}

	bool IndirectDraws::Impl::CreateMainBuffers(Resources& a_state, org::PersistentGraphHost& a_host, rhi::Device a_device)
	{
		// The sequence ranges (SequenceSlots) at the first layout's sizes, required to every draw the scene can produce (RequireMain);
		// sized to the layout exactly (its ranges grow by doubling).
		const bool smallTables = SwitchValue(Switch::TableStart) == "small";
		const MainLayout firstLayout = InitialMainLayout();
		a_state.sequences = MakeVersioned(CreateWords(SequenceSlots(firstLayout.sequenceDraws, firstLayout.sequenceDecals, firstLayout.decalRanges) * sizeof(DrawSequence) / 4,
											  true, "cs.dclf.sequences"),
			{ .extent = { 0, sizeof(DrawSequence), 4 }, .policy = org::services::GrowthPolicy::Headroom(0) });
		a_state.count = CreateWords(kCountWords, true, "cs.dclf.draw-count");
		// The state class sub-ranges' count words, required to the class plan's (RequireMainClasses).
		a_state.classCounts = MakeVersioned(CreateWords(64, true, "cs.dclf.class-counts"), { .extent = { 0, 4, 4 }, .policy = org::services::GrowthPolicy::Doubling(64) });
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
		a_state.clearPortalQueries = ComputeProgram::Load(a_device, { .source = kBuildDrawsShader, .entry = L"ClearPortalQueries", .constantWords = kBuildDrawsConstantWords });
		a_state.preparePortalQueries = ComputeProgram::Load(a_device, { .source = kBuildDrawsShader, .entry = L"PreparePortalQueries", .constantWords = kBuildDrawsConstantWords });
		if (!a_state.clearPortalQueries || !a_state.preparePortalQueries)
			return NotReady(7, "the main member portal query programs could not be created");
		a_state.portalQuerySignature = CreateDispatchSignature(a_device, a_state.preparePortalQueries->layout->GetHandle());
		if (!a_state.portalQuerySignature)
			return NotReady(7, "the main member portal query signature could not be created");

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
		// The near twin's target (Api::NearTwinContribution: Integrations/TerrainBlending's terrain depth) shares the main depth's
		// description; the Z-prepass draws Appended0's near objects into it as that feature's depth hook does.
		const auto& nearTwin = Extensions::NearTwin();
		if (auto* const twinTarget = static_cast<ID3D11Texture2D*>(nearTwin.target ? nearTwin.target() : nullptr)) {
			DxvkOrgInteropResourceInfo terrainInfo{};
			if (!RenderGraphRuntime::Get().DescribeResource(twinTarget, terrainInfo) || terrainInfo.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
				return NotReady(12, nearTwin.cannotDescribe.c_str());
			a_state.terrainDepth = ImportImage(a_device, terrainInfo.image, depthDesc, nearTwin.debugName.c_str());
			if (!a_state.terrainDepth)
				return NotReady(12, nearTwin.cannotImport.c_str());
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

	void ImplProducer::UpdateDrawBound(const SceneTables& a_tables, std::uint32_t a_generation)
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
		// Every table grows by doubling (RequireSceneTables). The object records, extras and geometry table are the payload's
		// (PayloadVersions).
		std::uint32_t unused = 0;
		// The features' scene resources (Api::SceneResources::setup: their programs and buffers, registered with the set in this order),
		// the vertex stream's first: its buffer is the second vertex stream, whose address and capacity are the sealed set's. Without it
		// (its setup gave none: no device address) the tables are not made.
		buffers->sceneResources = Extensions::SetUpSceneResources(a_device, smallStart);
		buffers->vertexStream = Extensions::SelectVertexStream(buffers->sceneResources);
		if (Extensions::VertexStream().registered && !buffers->vertexStream)
			return false;
		// The root states' buffers are their producer's scene resources, made above (Features/Fades'). The visibility readback reads their
		// rows (the cull binding RootRows): without them it is not made.
		if (Extensions::CullBindingBuffer(Api::CullBindingSlot::RootRows, *buffers)) {
			// By root slot, as the root states' buffers start.
			const std::uint32_t roots = smallStart ? 4u : kInitialVisibilityRoots;
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
		}
		// The passes' setups (Api::PassDef::setup: their programs; a null state keeps a pass out of the graph), after every table: a pass
		// may take its feature's scene resources (Features/TreeLod's cull and counts take its tables').
		buffers->passStates = Draws::SetUpPassStates(a_device, *buffers);
		scene = std::move(buffers);
		return true;
	}

	void IndirectDraws::Impl::ReserveMainLatch(std::uint32_t a_cascades, std::uint32_t a_shadowVolumes, std::uint32_t a_buckets, std::uint32_t a_classes,
		std::uint32_t a_classMapWords)
	{
		auto& latches = producer.latches;
		auto& layout = latches.main;
		if (!latches.mainLaidOut || (a_cascades <= layout.cascades && a_shadowVolumes <= layout.shadowVolumes && a_buckets <= layout.buckets && a_classes <= layout.classes &&
										a_classMapWords <= layout.classMapWords))
			return;
		if (a_classes > layout.classes) {
			const std::uint32_t grown = org::services::GrowthPolicy::Doubling(16).NextAs(layout.classes, a_classes);
			logger::info("[DCLF] state classes in the main latch: {} grown to {}", layout.classes, grown);
			layout.classes = grown;
		}
		if (a_classMapWords > layout.classMapWords) {
			const std::uint32_t grown = org::services::GrowthPolicy::Doubling(256).NextAs(layout.classMapWords, a_classMapWords);
			logger::info("[DCLF] set indices in the main latch's class map: {} grown to {}", layout.classMapWords, grown);
			layout.classMapWords = grown;
		}
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

	bool ShadowEngine::SetupShadow()
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
		auto scene = shadowHost.ShadowSceneBuffers(device);
		if (!scene) {
			shadowSetupFailed = true;
			return ShadowNotReady(2, "no device address for the face positions");
		}
		state->scene = std::move(scene);
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
		host->AddExtension(kShadowExtensionId, [state] { return Diagnostics::ScheduleDumped(kShadowExtensionId, MakeShadowExtension(state)); });
		logger::info("[DCLF] shadow view graph resources created");
		shadowHost.PostRevisionInputs();
		return true;
	}

	void IndirectDraws::Impl::ServeRevisionRequests()
	{
		// Makes the shadow view slots the revision code asked for (render thread) and rebuilds the graph; the revision code waits.
		auto* host = RenderGraphRuntime::Get().Host();
		auto& shadow = shadowEngine->shadow;
		if (const auto request = shadowSlotsSlot.Take(); request && host && shadow && request->slots > shadow->sequences.size()) {
			AddShadowViewSlots(*shadow, request->slots, request->sequenceDraws, request->bucketCountWords);
			host->AddExtension(kShadowExtensionId, [state = shadow] { return Diagnostics::ScheduleDumped(kShadowExtensionId, MakeShadowExtension(state)); });
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
		// The faces' draw families' rows and lists (each family with faces: its rowName and listName, the face's index appended), at the
		// family's units (its faces.lists: tree LOD's shape slots): the lists grow with its tables (RequireSceneTables).
		for (std::size_t d = 0; d < Extensions::kDrawFamilies && traits.trees; ++d) {
			const auto family = static_cast<Api::DrawFamily>(d);
			if (!FamilyHasFaces(family))
				continue;
			const auto& entry = Extensions::DrawFamilyOf(family);
			const auto& faces = entry.contribution.faces;
			const std::uint32_t headerWords = faces.headerBytes / 4;
			for (std::uint32_t f = 0; f < traits.faces; ++f) {
				state->faceRows[d][f] = CreateWords(faces.rowBytes / 4, false, name(fmt::format("{}{}", entry.rowName, f)).c_str());
				state->faceRowsAddress[d][f] = AddressOf(device, *state->faceRows[d][f]);
				if (!state->faceRowsAddress[d][f])
					return fail(fmt::format("{} rows have no device address", entry.label).c_str());
				if (faces.lists && FamilySetUp(*scene, family)) {
					Api::DrawFamilyLists lists{ *scene };
					faces.lists(lists);
					const std::uint32_t words = headerWords + lists.current * faces.unitWords;
					state->faceLists[d][f] = MakeVersioned(CreateWords(words, true, name(fmt::format("{}{}", entry.listName, f)).c_str()),
						{ .extent = { faces.headerBytes, faces.unitWords * 4, 4 } });
				}
			}
		}
		state->blocks = DeviceBuffer(std::uint64_t(traits.faces) * kViewBlockBytes, name(traits.blocks).c_str());
		state->blocksAddress = AddressOf(device, *state->blocks);
		if (!state->blocksAddress)
			return fail("blocks have no device address");
		// A view drawing with frame constants of its own (the faces'): laid out as the main ones, written by its commit, with its copies of
		// the main frame buffers.
		if (OwnsFrameConstants(traits)) {
			state->frameConstants = DeviceBuffer(kFrameConstantBytes, name("frame-constants").c_str());
			state->frameConstantsAddress = AddressOf(device, *state->frameConstants);
			if (!state->frameConstantsAddress)
				return fail("frame constants have no device address");
			MatchViewFrameBuffers(*state, resources->frameBuffers);
		}
		// The forward part draws through the main set's state classes' colour signatures with an explicit preprocess, as ForwardDrawPass.
		if (traits.forwardPart)
			state->preprocessForward = PreprocessStates::Create(device, host->FrameSlots(), fmt::format("the {} forward part", traits.name).c_str());
		v.resources = std::move(state);
		logger::info("[DCLF] {} graph resources created", traits.name);
		PostRevisionInputs();
		return true;
	}

	bool IndirectDraws::Impl::ImportViewColourTarget(ViewKind a_kind, ID3D11Texture2D* a_texture)
	{
		// A ColourWithOwnDepth view's (the reflection's: the engine's cube target, its faces the epoch's); its labels and log lines are its
		// shape's name and target.
		auto* host = RenderGraphRuntime::Get().Host();
		auto& view = View(a_kind);
		if (!view.resources || !host || !a_texture)
			return false;
		const auto& traits = TraitsOf(a_kind);
		auto& r = *view.resources;
		if (r.target && r.targetTexture == a_texture)
			return true;
		DxvkOrgInteropResourceInfo info{};
		if (!RenderGraphRuntime::Get().DescribeResource(a_texture, info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
			return false;
		org::TextureDescription desc{};
		desc.format = rhi::helpers::ToRHI(view.targets.colour);
		desc.channels = 4;
		desc.hasRTV = true;
		desc.rtvFormat = desc.format;
		auto cube = ImportImage(host->GetDesc().device, info.image, desc, fmt::format("DCLF {} {}", traits.name, traits.target).c_str());
		if (!cube) {
			static std::uint32_t logged = 0;
			if (logged++ < 4)
				logger::warn("[DCLF] the {} {} could not be imported (not in the general layout?); the faces stay native", traits.name, traits.target);
			return false;
		}
		// DCLF's own depth for the faces, at the engine's precision (its depth target 6 is R24G8).
		org::TextureDescription depthDesc{};
		depthDesc.imageDimensions.push_back({ info.image.extent.width, info.image.extent.height, 0, 0 });
		depthDesc.format = rhi::helpers::ToRHI(view.targets.depth);
		depthDesc.channels = 1;
		depthDesc.hasDSV = true;
		depthDesc.dsvFormat = depthDesc.format;
		r.depth = org::PixelBuffer::CreateSharedUnmaterialized(depthDesc);
		r.depth->SetName(fmt::format("{}.depth", traits.prefix));
		r.target = std::move(cube);
		r.targetTexture = a_texture;
		r.width = info.image.extent.width;
		r.height = info.image.extent.height;
		host->AddExtension(kViewSlots[ViewIndex(a_kind)].extensionId, [state = view.resources, id = kViewSlots[ViewIndex(a_kind)].extensionId] { return Diagnostics::ScheduleDumped(id, MakeViewExtension(state)); });
		logger::info("[DCLF] {} {} imported: {}x{}, {} faces, format {}", traits.name, traits.target, r.width, r.height, info.image.arrayLayers, static_cast<int>(view.targets.colour));
		PostRevisionInputs();
		return true;
	}

	bool IndirectDraws::Impl::ImportViewDepthTarget(ViewKind a_kind)
	{
		// A DepthTarget view's: kPOST_ZPREPASS_COPY (the first person's: its colour pass's depth, FUN_1414cb6d0: FUN_140e4fe00(manager, 7,
		// ...)), imported in its DSV's format; the extension is added with it and again when the main resources change. Its labels and log
		// lines are its shape's name and its slot's key group name.
		auto* host = RenderGraphRuntime::Get().Host();
		auto* renderer = globals::game::renderer;
		auto& view = View(a_kind);
		if (!view.resources || !host || !renderer)
			return false;
		auto& r = *view.resources;
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
			const auto& traits = TraitsOf(a_kind);
			auto imported = ImportImage(host->GetDesc().device, info.image, desc, fmt::format("DCLF {} depth (kPOST_ZPREPASS_COPY)", traits.name).c_str());
			if (!imported) {
				static std::uint32_t logged = 0;
				if (logged++ < 4)
					logger::warn("[DCLF] kPOST_ZPREPASS_COPY could not be imported (not in the general layout?); {} stays native",
						kKeyGroupNames[kViewSlots[ViewIndex(a_kind)].keyGroup]);
				return false;
			}
			r.target = std::move(imported);
			r.targetTexture = copy.texture;
			logger::info("[DCLF] {} depth imported: {}x{}, format {}", traits.name, info.image.extent.width, info.image.extent.height, static_cast<int>(dsvDesc.Format));
		}
		r.rebuildPending = false;
		host->AddExtension(kViewSlots[ViewIndex(a_kind)].extensionId, [state = view.resources, id = kViewSlots[ViewIndex(a_kind)].extensionId] { return Diagnostics::ScheduleDumped(id, MakeViewExtension(state)); });
		PostRevisionInputs();
		return true;
	}

	bool ShadowEngine::ImportShadowDepth(std::uint32_t a_index, std::uint32_t a_target)
	{
		auto* renderer = globals::game::renderer;
		if (!renderer)
			return false;
		const auto& data = renderer->GetDepthStencilData().depthStencils[a_target];
		return ImportDepthTexture(a_index, data.texture, data.views[0], a_target);
	}

	bool ShadowEngine::ImportDepthTexture(std::uint32_t a_index, ID3D11Texture2D* a_texture, ID3D11DepthStencilView* a_view, std::uint32_t a_label)
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
		// The shadow maps', then each occlusion map's (its entry's target label).
		static constexpr const char* kNames[kFirstOcclusionTarget] = { "DCLF shadow maps (ESRAM)", "DCLF shadow maps", "DCLF volumetric shadow maps (ESRAM)" };
		const char* name = IsOcclusionTarget(a_index) ? Extensions::OcclusionMapOf(a_index - kFirstOcclusionTarget).targetName.c_str() : kNames[a_index];
		auto imported = ImportImage(host->GetDesc().device, info.image, desc, name);
		if (!imported)
			return ShadowNotReady(5, "the shadow map could not be imported (not in the general layout?)");
		shadow->depth[a_index] = std::move(imported);
		shadow->depthTexture[a_index] = a_texture;
		shadow->depthLayers[a_index] = info.image.arrayLayers;
		// A new resource for the passes to bind: the graph is rebuilt on the next epoch.
		host->AddExtension(kShadowExtensionId, [state = shadow] { return Diagnostics::ScheduleDumped(kShadowExtensionId, MakeShadowExtension(state)); });
		logger::info("[DCLF] shadow map {} imported: {}x{}, {} slices, format {}", a_label, info.image.extent.width, info.image.extent.height,
			info.image.arrayLayers, static_cast<int>(dsvDesc.Format));
		return true;
	}
}
