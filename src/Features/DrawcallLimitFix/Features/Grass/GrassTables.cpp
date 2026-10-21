// Grass's scene resources (the Grass family's tables, its cull and arguments programs and counts' readbacks, made with the scene's
// tables), what they post with the revision's inputs and latch in the depth commit, and the depth commit's uploads into them (the commit
// hook): the mirror's changes, the buckets' bindings, the draw row with the engine's per-frame grass values.

// Helpers.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#include "Features/DrawcallLimitFix/Features/Grass/GrassTables.h"
#include "Features/DrawcallLimitFix/Features/Grass/GrassMirror.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Uploads.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBufferSet.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBuffers.h"
#include "Features/DrawcallLimitFix/Features/Grass/Diagnostics/GrassEvents.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"

#include "GrassShared/GrassFrame.h"

#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include <OpenRenderGraph/PersistentGraphHost.h>
#include <Resources/Buffers/Buffer.h>
#include <rhi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace DCLF::Features::Grass
{
	namespace
	{
		using namespace GrassInstances;

		Api::SceneResourcesId tablesId;  // install

		// The tables' first capacities (instance, shape, bucket and mesh slots), doubled as the mirror needs.
		constexpr std::uint32_t kInitialInstances = 256 * 1024, kInitialShapes = 4096, kInitialBuckets = 256, kInitialMeshes = 256;

		/*
		 * Stage 0's values (reverse-engineered: BSGrassShader::SetupGeometry's and SetupTechnique's): what the engine's grass passes bind
		 * per frame. The family's write mode is GrassDraws.cpp's kWriteMode.
		 */
		// stage 0: the vertex stage's WindTimer is this times the shape's wave period: the shader manager's timer at 0x142033090, scaled
		// as BSGrassShader::SetupGeometry does (1/600 of a turn).
		float WindTimeBase()
		{
			return Engine::Global<float>(Address::kWindTimer) * (1.0f / 600.0f) * 6.28318f;
		}

		// stage 0: WindVector: the shape's model +Y in world space (grass shapes are unrotated: (0, 1)), its strength the grass wind's
		// (clamped to 60) times fWindGrassMultiplier:Display.
		void SampleWind(float (&a_wind)[4])
		{
			a_wind[0] = 0.0f;
			a_wind[1] = 1.0f;
			a_wind[2] = std::min(Engine::Global<float>(Address::kWindSpeed), 60.0f) * Engine::Global<float>(Address::kWindGrassMultiplier);
			a_wind[3] = 0.0f;
		}

		// stage 0: the sun as SetupGeometry reads it: ShadowSceneNode 0's sun light (an NiDirectionalLight): -direction, diffuse, ambient;
		// ShadowClampValue (fShadowClampValue:Display) and FogNearColor.w beside them.
		void SampleLight(DrawRow& a_row)
		{
			a_row.dirLightDirection[3] = Engine::Global<float>(Address::kShadowClampValue);
			a_row.dirLightColor[3] = Engine::Global<float>(Address::kFogNearColorW);
			auto* state = globals::game::smState;
			auto* scene = state ? state->shadowSceneNode[0] : nullptr;
			auto* sun = scene ? scene->GetRuntimeData().sunLight : nullptr;
			auto* light = sun ? static_cast<RE::NiDirectionalLight*>(sun->light.get()) : nullptr;
			if (!light)
				return;
			const auto& direction = light->GetWorldDirection();
			const auto& data = light->GetLightRuntimeData();
			a_row.dirLightDirection[0] = -direction.x;
			a_row.dirLightDirection[1] = -direction.y;
			a_row.dirLightDirection[2] = -direction.z;
			a_row.dirLightColor[0] = data.diffuse.red;
			a_row.dirLightColor[1] = data.diffuse.green;
			a_row.dirLightColor[2] = data.diffuse.blue;
			a_row.ambientColor[0] = data.ambient.red;
			a_row.ambientColor[1] = data.ambient.green;
			a_row.ambientColor[2] = data.ambient.blue;
		}

		float IniFloat(const char* a_name, float a_fallback)
		{
			auto* setting = RE::GetINISetting(a_name);
			return setting ? setting->GetFloat() : a_fallback;
		}

		/**
		 * @brief The cull's values (Grass Optimizations' CullParams, GrassOptimizations.cpp: its settings, or the vanilla-equivalent
		 * defaults without it) and the eye; false without a camera.
		 */
		bool SampleCull(DrawRow& a_row)
		{
			auto* camera = RE::Main::WorldRootCamera();
			auto* renderer = globals::game::renderer;
			if (!camera || !renderer)
				return false;
			const auto settings = GrassShared::Settings();
			static const float startFade = IniFloat("fGrassStartFadeDistance:Grass", 6000.0f);
			static const float vanillaMax = startFade + IniFloat("fGrassFadeRange:Grass", 2000.0f);
			const float maxDistance = settings.RenderDistanceOverride > 0.0f ? settings.RenderDistanceOverride : vanillaMax;
			const auto& translate = camera->world.translate;
			a_row.eye[0] = translate.x;
			a_row.eye[1] = translate.y;
			a_row.eye[2] = translate.z;
			auto& cull = a_row.cull;
			cull.minPixelSize = settings.MinPixelSize;
			cull.fullDetailPx = settings.FullDetailPixelSize;
			cull.minKeep = settings.MinDensity;
			cull.fadeBand = 0.15f;
			cull.costBias = settings.MeshCostBias;
			cull.costBiasStart = std::max(0.0f, settings.CostBiasStartDistance);
			const auto [screenWidth, screenHeight] = renderer->GetScreenSize();
			const float top = std::abs(camera->GetRuntimeData2().viewFrustum.fTop);
			cull.projScale = top > 0.0f ? static_cast<float>(screenHeight) / (2.0f * top) : 0.0f;
			cull.maxDistSq = maxDistance * maxDistance;
			cull.edgeFadeStart = std::clamp(settings.EdgeFadeStart, 0.0f, 1.0f);
			// stage 0: the vertex stage's distance fade, read where SetupGeometry reads it.
			cull.alphaParam1 = Engine::Global<float>(Address::kAlphaParams);
			cull.alphaParam2 = Engine::Global<float>(Address::kAlphaParams + 4);
			cull.invisibleFadeCull = settings.InvisibleFadeCull;
			cull.simpleShadingPx = std::max(0.0f, settings.SimpleShadingPixelSize);
			const float collision = std::max(0.0f, settings.CollisionDistance);
			cull.collisionDistSq = collision * collision;
			cull.midLodPx = settings.MidLODPixelSize;
			cull.farLodPx = settings.EnableMidLOD ? std::min(settings.FarLODPixelSize, settings.MidLODPixelSize) : settings.FarLODPixelSize;
			cull.lodBandPx = std::max(0.0f, settings.MeshLODBandPixels);
			cull.flags = (settings.EnableMeshLOD && settings.EnableMidLOD ? kCullMidLod : 0u) | (settings.EnableMeshLOD && settings.EnableFarLOD ? kCullFarLod : 0u) |
			             (settings.EnableOcclusionCulling ? kCullOcclusion : 0u);
			return true;
		}

		ID3D11ShaderResourceView* ViewOf(const RE::NiSourceTexture* a_texture)
		{
			return a_texture && a_texture->rendererTexture ? reinterpret_cast<ID3D11ShaderResourceView*>(a_texture->rendererTexture->resourceView) : nullptr;
		}

		// The engine's default textures (a PBR bucket's missing maps; the shadow mask without bShadowsOnGrass).
		ID3D11ShaderResourceView* DefaultView(bool a_normal)
		{
			auto* state = globals::game::graphicsState;
			if (!state)
				return nullptr;
			auto& data = state->GetRuntimeData();
			return ViewOf(a_normal ? data.defaultTextureNormalMap.get() : data.defaultTextureWhite.get());
		}

		/**
		 * @brief The texture a bound row names: its binding's index into a_index, its owner into a_owners; false when there is no view
		 * or it cannot be bound.
		 */
		bool Bind(ID3D11ShaderResourceView* a_view, std::uint32_t& a_index, std::vector<std::shared_ptr<const void>>& a_owners)
		{
			if (!a_view)
				return false;
			auto binding = GpuTextures::Get().ResolveBinding(a_view, 0);
			if (binding.index == GpuTextures::kInvalid)
				return false;
			a_index = binding.index;
			a_owners.push_back(std::move(binding.owner));
			return true;
		}

		bool BindSampler(std::uint32_t a_address, std::uint32_t a_filter, std::uint32_t& a_index, std::vector<std::shared_ptr<const void>>& a_owners)
		{
			auto binding = GpuTextures::Get().SamplerBinding(a_address, a_filter);
			if (binding.index == GpuTextures::kInvalid)
				return false;
			a_index = binding.index;
			a_owners.push_back(std::move(binding.owner));
			return true;
		}

		/**
		 * @brief A bucket's texture and sampler bindings into its row, held by the bucket until it goes or the bindings' generation
		 * moves (the bindings it held then go to the commit's execution: a_retired). The diffuse texture with the material's address
		 * mode, trilinear for the depth draws and anisotropic for the colour draws (BSGrassShader's); with TruePBR its maps (the
		 * engine's defaults where it has none) and their sampler. False while the diffuse texture has no view.
		 */
		bool BindBucket(Bucket& a_bucket, std::uint32_t a_generation, std::vector<std::shared_ptr<const void>>& a_retired)
		{
			if (a_bucket.bound && a_bucket.bindingGeneration == a_generation)
				return true;
			for (auto& owner : a_bucket.owners)
				a_retired.push_back(std::move(owner));
			a_bucket.owners.clear();
			a_bucket.bound = false;
			auto& row = a_bucket.row;
			auto& source = a_bucket.source;
			std::vector<std::shared_ptr<const void>> owners;
			const std::uint32_t address = std::min(a_bucket.key.clampMode, 3u);
			// The diffuse is sampled clamped whatever the material's mode, as the engine's grass technique sets sampler 0 (address mode 0,
			// BSGrassShader::SetupTechnique; Grass Optimizations forces it too): a card's UVs reach the texture's edge, and wrapping filters
			// the opposite edge's opaque stems into its corners, which then pass the alpha test.
			constexpr std::uint32_t kDiffuseAddress = 0;
			bool bound = Bind(ViewOf(source.diffuse.get()), row.diffuseIndex, owners) && BindSampler(kDiffuseAddress, 2, row.depthSampler, owners) &&
			             BindSampler(kDiffuseAddress, 3, row.colourSampler, owners);
			if (bound && a_bucket.key.pbr) {
				auto viewOr = [](const RE::NiPointer<RE::NiSourceTexture>& a_texture, bool a_normal) {
					auto* view = ViewOf(a_texture.get());
					return view ? view : DefaultView(a_normal);
				};
				bound = Bind(viewOr(source.normal, true), row.normalIndex, owners) && Bind(viewOr(source.rmaos, false), row.rmaosIndex, owners) &&
				        Bind(viewOr(source.features0, false), row.featuresIndex, owners) && BindSampler(address, 3, row.pbrSampler, owners);
			}
			if (!bound) {
				for (auto& owner : owners)
					a_retired.push_back(std::move(owner));
				return false;
			}
			a_bucket.owners = std::move(owners);
			a_bucket.bindingGeneration = a_generation;
			a_bucket.bound = true;
			return true;
		}

		// The scene's tables are made (the first frame with a device): the programs, then the tables.
		std::shared_ptr<void> Setup(const Api::SceneResourcesSetup& a_setup)
		{
			auto tables = std::make_shared<GrassTables>();
			tables->cull = ComputeProgram::Load(a_setup.device, { .source = kGrassCullShader, .constantWords = kGrassCullConstantWords });
			tables->args = ComputeProgram::Load(a_setup.device, { .source = kGrassArgsShader, .constantWords = kGrassArgsConstantWords });
			if (!tables->cull || !tables->args) {
				logger::warn("[DCLF] The grass cull or arguments program could not be created; grass stays native");
				return nullptr;
			}
			const bool smallStart = a_setup.smallStart;
			const std::uint32_t instances = smallStart ? 4u : kInitialInstances;
			auto make = [](std::uint32_t a_units, std::uint32_t a_unitBytes, bool a_uav, const char* a_name) {
				return Draws::MakeVersioned(Draws::CreateWords(std::uint64_t(a_units) * a_unitBytes / 4, a_uav, a_name), { .extent = { 0, a_unitBytes, 4 } });
			};
			tables->instances = make(instances, sizeof(Instance), false, "cs.dclf.grass-instances");
			tables->owners = make(instances, 4, false, "cs.dclf.grass-owners");
			tables->frame = make(instances, sizeof(InstanceFrame), true, "cs.dclf.grass-frame");
			tables->visible = make(instances, kVisibleUnitBytes, true, "cs.dclf.grass-visible");
			tables->shapes = make(smallStart ? 4u : kInitialShapes, sizeof(ShapeRow), false, "cs.dclf.grass-shapes");
			tables->buckets = make(smallStart ? 4u : kInitialBuckets, sizeof(BucketRow), false, "cs.dclf.grass-buckets");
			tables->meshes = make(smallStart ? 4u : kInitialMeshes, sizeof(MeshRow), false, "cs.dclf.grass-meshes");
			tables->draw = Draws::CreateWords(sizeof(DrawRow) / 4, false, "cs.dclf.grass-draw");
			tables->draws = Draws::CreateWords(kDrawsBytes / 4, true, "cs.dclf.grass-draws");
			auto device = a_setup.device;
			auto address = [&](org::Buffer& a_buffer) { return device.GetBufferDeviceAddress({ a_buffer.GetAPIResource().GetHandle(), 0 }); };
			tables->drawAddress = address(*tables->draw);
			bool addressed = tables->drawAddress != 0;
			for (const auto* table : { &tables->instances, &tables->owners, &tables->frame, &tables->visible, &tables->shapes, &tables->buckets, &tables->meshes })
				addressed = addressed && address(*(*table)->Get()) != 0;
			if (!addressed) {
				logger::warn("[DCLF] Grass's tables have no device address; grass stays native");
				return nullptr;
			}
			if (auto* host = RenderGraphRuntime::Get().Host()) {
				auto counts = std::make_shared<GrassCounts>();
				for (std::uint32_t i = 0; i < host->FrameSlots(); ++i)
					counts->readback.push_back(org::Buffer::CreateShared(rhi::HeapType::Readback, sizeof(DrawsHeader)));
				counts->filled.assign(host->FrameSlots(), 0u);
				tables->counts = std::move(counts);
			}
			return tables;
		}

		void RegisterGraph(const Api::SceneResourcesGraph& a_graph)
		{
			const auto& tables = a_graph.State<GrassTables>();
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.grass-instances"), tables.instances);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.grass-owners"), tables.owners);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.grass-frame"), tables.frame);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.grass-visible"), tables.visible);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.grass-shapes"), tables.shapes);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.grass-buckets"), tables.buckets);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.grass-meshes"), tables.meshes);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.grass-draw"), tables.draw);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.grass-draws"), tables.draws);
		}

		// The tables by the mirror's posted slots; the buckets no further than the draws buffer's slots (kMaxBuckets: past them, the decision
		// keeps grass the engine's).
		void Require(const Api::SceneResourcesRequirements& a_requirements)
		{
			const auto& tables = a_requirements.State<GrassTables>();
			const auto& inputs = a_requirements.inputs;
			auto& set = Draws::SceneBufferSet::Get();
			Draws::UnitGroup{}.Add(tables.instances).Add(tables.owners).Add(tables.frame).Add(tables.visible).Require(a_requirements.build, inputs[kInstanceSlotsInput]);
			set.Require(a_requirements.build, tables.shapes, inputs[kShapeSlotsInput]);
			set.Require(a_requirements.build, tables.buckets, std::min(inputs[kBucketSlotsInput], kMaxBuckets));
			set.Require(a_requirements.build, tables.meshes, inputs[kMeshSlotsInput]);
		}

		// The mirror's slots: what the tables must hold.
		void PostInputs(std::span<std::uint32_t> a_words)
		{
			const auto& mirror = RenderMirror();
			a_words[kInstanceSlotsInput] = mirror.InstanceSlots();
			a_words[kShapeSlotsInput] = mirror.ShapeSlots();
			a_words[kBucketSlotsInput] = mirror.BucketSlots();
			a_words[kMeshSlotsInput] = mirror.MeshSlots();
		}

		// The depth commit's draw row and the draws buffer's header (its counts and counters, zero).
		void Latched(Api::SceneResourcesLatch& a_latch)
		{
			const auto& tables = a_latch.State<GrassTables>();
			a_latch.Latch(tables.draw, sizeof(DrawRow));
			a_latch.Latch(tables.draws, sizeof(DrawsHeader));
		}

		/** @brief The units every table holds in the versions a_sealed names: whether the mirror fits them (and the draws buffer's slots). */
		bool Fits(const Draws::NamedVersions& a_sealed, const Mirror& a_mirror, const GrassTables& a_tables)
		{
			const std::uint64_t instances = std::min({ a_sealed.Of(*a_tables.instances).units, a_sealed.Of(*a_tables.owners).units, a_sealed.Of(*a_tables.frame).units,
				a_sealed.Of(*a_tables.visible).units });
			return a_mirror.InstanceSlots() <= instances && a_mirror.ShapeSlots() <= a_sealed.Of(*a_tables.shapes).units &&
			       a_mirror.BucketSlots() <= std::min<std::uint64_t>(a_sealed.Of(*a_tables.buckets).units, kMaxBuckets) &&
			       a_mirror.MeshSlots() <= a_sealed.Of(*a_tables.meshes).units;
		}

		/**
		 * @brief The mirror's changes into the versions a_sealed names: freed instance ranges' owners (~0u) first, then each changed shape
		 * slot's row, records and owners, each changed mesh's row and each changed bucket's row with its bindings (a bucket whose texture
		 * has no view yet is sent with no instances and asked again at the next commit).
		 */
		void SendChanges(const Draws::NamedVersions& a_sealed, Mirror& a_mirror, GrassTables& a_tables, Draws::LatchedUploads& a_uploads,
			std::vector<std::shared_ptr<const void>>& a_owners)
		{
			auto& filled = a_tables.filled;
			const auto& instances = a_sealed.Of(*a_tables.instances);
			const auto& owners = a_sealed.Of(*a_tables.owners);
			const auto& shapes = a_sealed.Of(*a_tables.shapes);
			const auto& buckets = a_sealed.Of(*a_tables.buckets);
			const auto& meshes = a_sealed.Of(*a_tables.meshes);
			const std::uint32_t generation = GpuTextures::Get().Generation();
			if (!filled[0].Holds(instances) || !filled[1].Holds(owners) || !filled[2].Holds(shapes) || !filled[3].Holds(buckets) || !filled[4].Holds(meshes) ||
				generation != a_tables.bindingGeneration)
				a_mirror.MarkAllChanged();
			a_tables.bindingGeneration = generation;
			for (const std::uint32_t slot : std::exchange(a_tables.unbound, {}))
				a_mirror.MarkBucket(slot);
			Mirror::Changes changes;
			a_mirror.TakeChanges(changes);
			for (const auto& [first, count] : changes.freedRanges) {
				const std::vector<std::uint32_t> none(count, ~0u);
				a_uploads(a_tables.owners, none.data(), none.size() * 4, std::uint64_t(first) * 4);
			}
			for (const std::uint32_t slot : changes.shapes) {
				const auto& row = a_mirror.SlotRow(slot);
				a_uploads(a_tables.shapes, &row, sizeof(row), std::uint64_t(slot) * sizeof(ShapeRow));
				const auto* records = a_mirror.SlotInstances(slot);
				if (!records || records->empty())
					continue;
				a_uploads(a_tables.instances, records->data(), records->size() * sizeof(Instance), std::uint64_t(row.firstInstance) * sizeof(Instance));
				const std::vector<std::uint32_t> owner(records->size(), slot);
				a_uploads(a_tables.owners, owner.data(), owner.size() * 4, std::uint64_t(row.firstInstance) * 4);
			}
			for (const std::uint32_t mesh : changes.meshes) {
				const auto& row = a_mirror.MeshSlotRow(mesh);
				a_uploads(a_tables.meshes, &row, sizeof(row), std::uint64_t(mesh) * sizeof(MeshRow));
			}
			for (const std::uint32_t slot : changes.buckets) {
				BucketRow row{};
				if (auto* bucket = a_mirror.SlotBucket(slot)) {
					if (!BindBucket(*bucket, generation, a_owners)) {
						a_tables.unbound.push_back(slot);
						++a_tables.unboundBuckets;
					}
					row = bucket->row;
					if (!bucket->bound)
						row.instanceCount = 0;
				}
				a_uploads(a_tables.buckets, &row, sizeof(row), std::uint64_t(slot) * sizeof(BucketRow));
			}
			for (std::size_t i = 0; const auto* named : { &instances, &owners, &shapes, &buckets, &meshes })
				filled[i++].Filled(*named);
			a_tables.shapesSent += changes.shapes.size();
			a_tables.bucketsSent += changes.buckets.size();
		}

		/** @brief The frame's shadow mask (bShadowsOnGrass: the engine's, else its white default) and Grass Collision's published state. */
		bool BindFrameTextures(DrawRow& a_row, std::vector<std::shared_ptr<const void>>& a_owners)
		{
			static auto* shadowsOnGrass = RE::GetINISetting("bShadowsOnGrass:Display");
			ID3D11ShaderResourceView* mask = DefaultView(false);
			if ((!shadowsOnGrass || shadowsOnGrass->GetBool()) && globals::game::renderer)
				mask = reinterpret_cast<ID3D11ShaderResourceView*>(globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kSHADOW_MASK].SRV);
			if (!Bind(mask, a_row.shadowMaskIndex, a_owners))
				return false;
			// The collision of the frames DCLF owns grass (its stood-down update, from the decision): none without Grass Collision.
			GrassShared::Collision collision;
			if (GrassShared::LatestCollision(collision) && Bind(collision.texture, a_row.collisionIndex, a_owners))
				a_row.collision = collision.perFrame;
			else
				a_row.collisionIndex = ~0u;
			return true;
		}

		/**
		 * @brief The depth commit's draw row (the adopted versions' addresses, the engine's frame values, the cull's), the draws buffer's
		 * zero header (and, once after the tables were made or grass became DCLF's, its zero counts: grass.args-colour zeroes them every
		 * frame after). The cull walks the slots only while a_draw and the frame's textures are bound.
		 */
		void WriteDrawRow(const Draws::NamedVersions& a_sealed, const Mirror& a_mirror, GrassTables& a_tables, Draws::LatchedUploads& a_uploads,
			std::vector<std::shared_ptr<const void>>& a_owners, bool a_draw)
		{
			DrawRow row{};
			row.instances = a_sealed.Of(*a_tables.instances).deviceAddress;
			row.shapes = a_sealed.Of(*a_tables.shapes).deviceAddress;
			row.buckets = a_sealed.Of(*a_tables.buckets).deviceAddress;
			row.meshes = a_sealed.Of(*a_tables.meshes).deviceAddress;
			row.owners = a_sealed.Of(*a_tables.owners).deviceAddress;
			row.frame = a_sealed.Of(*a_tables.frame).deviceAddress;
			row.visible = a_sealed.Of(*a_tables.visible).deviceAddress;
			SampleWind(row.windVector);
			row.timeBase = WindTimeBase();
			row.prevTimeBase = std::exchange(a_tables.prevTimeBase, row.timeBase);
			row.fadeNow = a_mirror.FadeClock();
			static const float fadeIn = IniFloat("fGrassFadeInTime:Grass", 0.0f);
			row.fadeInTimeRcp = fadeIn > 0.0f ? 1.0f / fadeIn : 1e6f;
			SampleLight(row);
			const bool sampled = SampleCull(row) && BindFrameTextures(row, a_owners);
			if (a_draw && sampled && a_mirror.InstanceSlots()) {
				row.instanceSlots = a_mirror.InstanceSlots();
				row.shapeSlots = a_mirror.ShapeSlots();
				row.bucketSlots = a_mirror.BucketSlots();
			}
			a_uploads(a_tables.draw, &row, sizeof(row), 0);
			a_tables.row = row;
			const DrawsHeader header{};
			a_uploads(a_tables.draws, &header, sizeof(header), 0);
			if (!a_tables.countsZeroed) {
				const std::vector<std::uint32_t> zero(2 * kMaxDrawSlots, 0u);
				a_uploads(a_tables.draws, zero.data(), zero.size() * 4, kCountsOffset);
				a_tables.countsZeroed = true;
			}
			if (row.instanceSlots)
				a_tables.ready.store(true, std::memory_order_release);
		}

		/**
		 * @brief The depth commit's grass uploads, into the versions a_sealed (the adopted set) names: their capacities and the draw row's
		 * addresses are theirs. Retired mesh leases and bindings go to the execution (a_owners).
		 */
		void UploadGrass(const Draws::NamedVersions& a_sealed, Mirror& a_mirror, GrassTables& a_tables, Draws::LatchedUploads& a_uploads,
			std::vector<std::shared_ptr<const void>>& a_owners, bool a_owned)
		{
			a_tables.ready.store(false, std::memory_order_release);
			if (!a_tables.cull || !a_tables.instances)
				return;
			for (auto& owner : a_mirror.TakeRetired())
				a_owners.push_back(std::move(owner));
			if (a_owned && !a_tables.owned)
				a_tables.countsZeroed = false;
			a_tables.owned = a_owned;
			// Capacities are reserved before the epoch; a mirror past them keeps its changes and draws nothing this commit.
			const bool fits = Fits(a_sealed, a_mirror, a_tables);
			if (fits)
				SendChanges(a_sealed, a_mirror, a_tables, a_uploads, a_owners);
			else if (a_mirror.BucketSlots() > kMaxBuckets)
				++a_tables.overBuckets;
			++a_tables.uploads;
			WriteDrawRow(a_sealed, a_mirror, a_tables, a_uploads, a_owners, a_owned && fits);
		}

		// Grass (dclf-architecture.md, "Grass: the draws"): the mirror's changes and the draw row, for the depth cull, the arguments and the
		// passes' draws; the depth commit's only, after the other features' hooks (its uploads go through the commit's latched uploads).
		void Commit(const Api::CommitContext& a_commit)
		{
			if (a_commit.kind != Api::CommitKind::Depth)
				return;
			auto* tables = TablesOf(a_commit.scene);
			if (!tables)
				return;
			auto& mirror = RenderMirror();
			const bool owned = IndirectDraws::Get().DrawFamilyOwned(Api::DrawFamily::Grass);
			UploadGrass(a_commit.adopted, mirror, *tables, *a_commit.latched, a_commit.bindingOwners, owned);
			// The engine's passes were withheld: a commit that cannot draw leaves a hole.
			if (owned && !tables->ready.load(std::memory_order_acquire))
				Diagnostics::Notify(GrassMissed{ a_commit.frame, mirror.InstanceSlots(), a_commit.adopted.Of(*tables->instances).units, mirror.BucketSlots(),
					a_commit.adopted.Of(*tables->buckets).units });
		}
	}

	GrassTables* TablesOf(const Draws::SceneBuffers& a_scene)
	{
		return Extensions::SceneResourcesOf<GrassTables>(a_scene, tablesId);
	}

	std::shared_ptr<GrassTables> SharedTablesOf(const Draws::SceneBuffers& a_scene)
	{
		return Extensions::SharedSceneResourcesOf<GrassTables>(a_scene, tablesId);
	}

	Api::SceneResourcesId TablesId()
	{
		return tablesId;
	}

	void RegisterTables(Api::InstallContext& a_context)
	{
		tablesId = a_context.RegisterSceneResources({ .name = "grass", .setup = &Setup, .registerGraph = &RegisterGraph, .require = &Require, .postInputs = &PostInputs,
			.inputWords = kInputWords, .latched = &Latched });
		a_context.RegisterCommitHook({ .name = "grass", .commit = &Commit });
	}
}
