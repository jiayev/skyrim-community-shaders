#include "Features/DrawcallLimitFix/Features/TextureStreaming/TextureStreaming.h"

#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Core/Gpu/TextureFeedback.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Direct.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneGraph.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Lookups.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Publication.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "State.h"
#include "TextureStreaming/Policy.h"
#include "TextureStreaming/Streamer.h"

#include <atomic>
#include <limits>
#include <memory>
#include <vector>

namespace DCLF::Features::Streaming
{
	namespace
	{
		std::atomic<float> cameraX{ 0.0f }, cameraY{ 0.0f }, cameraZ{ 0.0f }, pixelsPerRadian{ 0.0f };
		std::atomic<float> forwardX{ 1.0f }, forwardY{ 0.0f }, forwardZ{ 0.0f }, viewCos{ -1.0f };  // the view's axis, and the cosine of its corner's angle from it

		// The render thread's only part in the demand: a position, a direction and two ratios a frame, read by the worker.
		void Commit(const Api::CommitContext&)
		{
			const auto* root = RE::Main::WorldRootCamera();
			if (!root)
				return;
			cameraX.store(root->world.translate.x, std::memory_order_relaxed);
			cameraY.store(root->world.translate.y, std::memory_order_relaxed);
			cameraZ.store(root->world.translate.z, std::memory_order_relaxed);
			const auto [screenWidth, screenHeight] = RE::BSGraphics::Renderer::GetScreenSize();
			const auto& frustum = root->GetRuntimeData2().viewFrustum;
			const float top = frustum.fTop;
			if (top > 0.0f) {
				pixelsPerRadian.store(static_cast<float>(screenHeight) / (2.0f * top), std::memory_order_relaxed);
				viewCos.store(1.0f / std::sqrt(1.0f + top * top + frustum.fRight * frustum.fRight), std::memory_order_relaxed);
			}
			forwardX.store(root->world.rotate.entry[0][0], std::memory_order_relaxed);
			forwardY.store(root->world.rotate.entry[1][0], std::memory_order_relaxed);
			forwardZ.store(root->world.rotate.entry[2][0], std::memory_order_relaxed);
		}

		// The worker's: the bounds it last took (a producer posts the next one when it runs).
		std::unique_ptr<Direct::SnapshotSpheres> spheres;
		std::vector<float> materialFootprint;  // what only distance asks of each material
		std::vector<float> visibleFootprint;   // what is on screen asks of it
		std::vector<std::uint32_t> touched;
		// What the shaders last reported of each material, kept a while: a surface out of view keeps its resolution until the policy lets it go.
		constexpr std::uint8_t kFeedbackTicks = 32;
		std::vector<float> feedbackFootprint;
		std::vector<std::uint8_t> feedbackAge;
		// Which material each slot last reported for (raw, only compared). One that never has may be one whose shaders cannot: alpha-tested
		// Lighting, grass, Effect. Those have only distance to go by, so while they are in view, that counts as on screen.
		std::vector<const RE::BSShaderMaterial*> reporter;

		template <class Views>
		void NoteViews(TextureStreaming::DemandSink& a_sink, const Views& a_views, float a_footprint, TextureStreaming::DemandKind a_kind)
		{
			for (const auto* view : a_views)
				if (view)
					a_sink.Note(view, a_footprint, a_kind);
		}

		// The finest footprint of each material's objects, by distance: the floor. Off the frame's path, from immutable tables.
		void Demand(TextureStreaming::DemandSink& a_sink)
		{
			auto& values = FrameValues::Get();
			if (auto taken = values.TakeStreamingSpheres())
				spheres = std::move(taken);
			values.RequestStreamingSpheres();

			const auto settings = TextureStreaming::GetSettings();
			const bool useDistance = settings.distance, useFeedback = settings.feedback;
			const float distanceScale = settings.distanceScale;
			// What the shaders reported of the last place is over.
			static std::uint64_t worldChanges = 0;
			if (const auto changes = TextureStreaming::WorldChanges(); changes != worldChanges) {
				worldChanges = changes;
				std::fill(feedbackAge.begin(), feedbackAge.end(), kFeedbackTicks);
			}
			auto* words = TextureFeedback::Words();
			if (words)
				words[0] = useFeedback ? 1u : 0u;
			const auto scene = LatestPublishedScene();
			const float k = pixelsPerRadian.load(std::memory_order_relaxed);
			if (!scene.tables || !scene.lookups || k <= 0.0f)
				return;
			const float eye[3] = { cameraX.load(std::memory_order_relaxed), cameraY.load(std::memory_order_relaxed), cameraZ.load(std::memory_order_relaxed) };
			const float forward[3] = { forwardX.load(std::memory_order_relaxed), forwardY.load(std::memory_order_relaxed), forwardZ.load(std::memory_order_relaxed) };
			const float cosine = viewCos.load(std::memory_order_relaxed);
			const auto touch = [](std::vector<float>& a_footprints, std::uint32_t a_material, float a_footprint) {
				if (materialFootprint[a_material] == std::numeric_limits<float>::infinity() && visibleFootprint[a_material] == std::numeric_limits<float>::infinity())
					touched.push_back(a_material);
				a_footprints[a_material] = std::min(a_footprints[a_material], a_footprint);
			};
			const auto& tables = *scene.tables;
			const auto& lookups = *scene.lookups;
			const std::size_t materials = std::min<std::size_t>(lookups.materials.size(), TextureFeedback::kSlots);
			if (materialFootprint.size() < materials) {
				materialFootprint.resize(materials, std::numeric_limits<float>::infinity());
				visibleFootprint.resize(materials, std::numeric_limits<float>::infinity());
				reporter.resize(materials, nullptr);
				feedbackFootprint.resize(materials, std::numeric_limits<float>::infinity());
				feedbackAge.resize(materials, kFeedbackTicks);
			}
			touched.clear();
			const std::size_t objects = useDistance && spheres ? std::min({ tables.objects.size(), tables.objectGeometry.size(), spheres->sphere.size(), spheres->owner.size() }) : 0;
			for (std::size_t slot = 0; slot < objects; ++slot) {
				// A slot whose row is another geometry's has no bound of this object's yet.
				if (!tables.objectGeometry[slot] || spheres->owner[slot] != tables.objectGeometry[slot])
					continue;
				const auto material = tables.objects[slot].materialIndex;
				const auto& sphere = spheres->sphere[slot];
				if (material >= materials || sphere[3] <= 0.0f)
					continue;
				const float dx = sphere[0] - eye[0], dy = sphere[1] - eye[1], dz = sphere[2] - eye[2];
				const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
				const float footprint = TextureStreaming::Policy::FootprintForDistance(distance, sphere[3], k, distanceScale);
				// Can it report? And is it in view (the view's corner angle, which errs towards in)?
				const bool blind = !useFeedback || reporter[material] != lookups.materials[material].key.first;
				const bool inView = blind && dx * forward[0] + dy * forward[1] + dz * forward[2] >= distance * cosine - sphere[3];
				touch(inView ? visibleFootprint : materialFootprint, material, footprint);
			}
			// The correction: each material's report since the last tick, on top of the floor.
			if (words && useFeedback) {
				std::uint64_t reported = 0, finer = 0;
				for (std::uint32_t material = 0; material < materials; ++material) {
					const std::uint32_t value = words[material + 1];
					if (value != TextureFeedback::kEmpty) {
						words[material + 1] = TextureFeedback::kEmpty;
						feedbackFootprint[material] = static_cast<float>(value) / TextureFeedback::kFootprintScale - TextureFeedback::kFootprintOffset;
						feedbackAge[material] = 0;
						reporter[material] = lookups.materials[material].key.first;
						++reported;
						finer += feedbackFootprint[material] < materialFootprint[material] ? 1 : 0;
					} else if (feedbackAge[material] < kFeedbackTicks) {
						++feedbackAge[material];
					}
					if (feedbackAge[material] < kFeedbackTicks)
						touch(visibleFootprint, material, feedbackFootprint[material]);
				}
				TextureStreaming::NoteFeedback(reported, finer);
			}
			for (const auto material : touched) {
				const auto& entry = lookups.materials[material];
				for (const auto kind : { TextureStreaming::DemandKind::Distance, TextureStreaming::DemandKind::Visible }) {
					auto& footprints = kind == TextureStreaming::DemandKind::Visible ? visibleFootprint : materialFootprint;
					if (footprints[material] != std::numeric_limits<float>::infinity()) {
						NoteViews(a_sink, entry.views, footprints[material], kind);
						NoteViews(a_sink, entry.featureViews, footprints[material], kind);
						footprints[material] = std::numeric_limits<float>::infinity();
					}
				}
			}
		}

		void Install(Api::InstallContext& a_context)
		{
			const auto master = a_context.RegisterSwitch({ .name = "CS_DCLF_TEX_STREAM", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::Zero, .description = "0: the game's textures stay whole, not shells with streamed mip windows; read at the first texture load", .after = "CS_DCLF_GRASS" });
			a_context.RegisterSwitch({ .name = "CS_DCLF_TEX_STREAM_FULL", .kind = Api::SwitchKind::Test, .reduced = Api::SwitchReduced::Never, .description = "1: every window the full chain, made as the texture loads (the baseline)", .after = "CS_DCLF_TEX_STREAM" });
			a_context.RegisterSwitch({ .name = "CS_DCLF_TEX_STREAM_BASE", .kind = Api::SwitchKind::Other, .reduced = Api::SwitchReduced::Never, .description = "the base window's largest side (default 128)", .after = "CS_DCLF_TEX_STREAM_FULL" });
			a_context.RegisterSwitch({ .name = "CS_DCLF_TEX_STREAM_BUDGET_MB", .kind = Api::SwitchKind::Other, .reduced = Api::SwitchReduced::Never, .description = "the memory streaming keeps to, in MiB, above or below the driver's budget (default: the driver's budget)", .after = "CS_DCLF_TEX_STREAM_BASE" });
			a_context.RegisterSwitch({ .name = "CS_DCLF_TEX_STREAM_RATE_MB", .kind = Api::SwitchKind::Other, .reduced = Api::SwitchReduced::Never, .description = "upgrades started per second, in MiB (default 256)", .after = "CS_DCLF_TEX_STREAM_BUDGET_MB" });
			a_context.RegisterSwitch({ .name = "CS_DCLF_TEX_STREAM_VISIBLE_OFFSET", .kind = Api::SwitchKind::Other, .reduced = Api::SwitchReduced::Never, .description = "over the budget, how many mips what is on screen may step down (default 0: it loads what it asks for)", .after = "CS_DCLF_TEX_STREAM_RATE_MB" });
			a_context.RegisterSwitch({ .name = "CS_DCLF_TEX_STREAM_DELAY_MS", .kind = Api::SwitchKind::Test, .reduced = Api::SwitchReduced::Never, .description = "a delay before each background window build, to see late windows", .after = "CS_DCLF_TEX_STREAM_RATE_MB" });
			a_context.RegisterSwitch({ .name = "CS_DCLF_TEX_STREAM_DISTANCE_SCALE", .kind = Api::SwitchKind::Other, .reduced = Api::SwitchReduced::Never, .description = "the distance floor's sharpness: above 1 finer mips (default 1)", .after = "CS_DCLF_TEX_STREAM_RATE_MB" });
			a_context.RegisterSwitch({ .name = "CS_DCLF_TEX_DISTANCE", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::Never, .description = "0: no distance floor (over the menu's setting)", .after = "CS_DCLF_TEX_STREAM_DISTANCE_SCALE" });
			a_context.RegisterSwitch({ .name = "CS_DCLF_TEX_FEEDBACK", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::Never, .description = "0: no shader feedback (over the menu's setting)", .after = "CS_DCLF_TEX_DISTANCE" });
			if (!TextureStreaming::Enabled() || TextureStreaming::FullMode() || !master)
				return;
			a_context.RegisterCommitHook({ .name = "texture-streaming", .commit = &Commit });

			TextureStreaming::SetScheduler({ [](TextureStreaming::TextureId a_id, std::uint64_t a_revision, std::uint32_t a_top) {
												SceneScheduler::Graph().PostTextureWindow(a_id, a_revision, [a_id, a_top] { TextureStreaming::BuildWindow(a_id, a_top); });
											},
				[](TextureStreaming::TextureId a_id) { SceneScheduler::Graph().RetireTextureWindow(a_id); } });
			TextureStreaming::SetWindowListener([](const ID3D11ShaderResourceView* a_shell) { GpuTextures::Get().NoteWindowChanged(a_shell); });
			TextureStreaming::SetDemandSource(&Demand);
		}
	}

	Api::Feature Feature()
	{
		return { "texture-streaming", &Install };
	}

	void ReportBindings()
	{
		const auto scene = LatestPublishedScene();
		if (!scene.lookups)
			return;
		std::uint64_t views = 0, current = 0, supersededWaiting = 0, missed = 0, unresolved = 0;
		std::vector<std::string> samples;
		const auto& materials = scene.lookups->materials;
		for (std::size_t slot = 0; slot < materials.size(); ++slot) {
			const auto& entry = materials[slot];
			if (!entry.key.first)
				continue;
			for (std::size_t t = 0; t < entry.views.size(); ++t) {
				const auto* view = entry.views[t];
				std::string path;
				std::uint32_t resident = 0, wanted = 0;
				if (!view || !TextureStreaming::Describe(view, path, resident, wanted))
					continue;
				++views;
				const void* owner = entry.bindingBlock ? entry.bindingBlock->textures[t].get() : nullptr;
				if (!entry.resolved || !owner) {
					++unresolved;
					continue;
				}
				winrt::com_ptr<ID3D11ShaderResourceView> window;
				window.attach(TextureStreaming::CurrentWindow(const_cast<ID3D11ShaderResourceView*>(view)));
				if (GpuTextures::ImportedWindow(owner) == window.get()) {
					++current;
					continue;
				}
				const bool superseded = GpuTextures::Superseded(owner);
				++(superseded ? supersededWaiting : missed);
				if (samples.size() < 24)
					samples.push_back(std::format("{} slot {} tex {} resident {} wanted {} {}", path, slot, t, resident, wanted, superseded ? "superseded" : "MISSED"));
			}
		}
		logger::info("[TextureStreaming] materials' views of shells: {}: {} draw the current window, {} a window a swap replaced and not re-resolved yet, {} a replaced window with no swap seen, {} unresolved",
			views, current, supersededWaiting, missed, unresolved);
		for (const auto& sample : samples)
			logger::info("[TextureStreaming]   stale: {}", sample);
	}
}
