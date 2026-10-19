// Fades' scene resources ("fade roots", Api::SceneResources): FadeStateCS's program and buffers made with the scene's tables, their graph
// registration, each revision's requirements, the fit of the roots they hold, and the write-back's staging for each sealed set.

#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"
#include "Features/DrawcallLimitFix/Core/Extensions/TableFamilies.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBufferSet.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeScene.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalPrograms.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalViews.h"

#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include <OpenRenderGraph/PersistentGraphHost.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>

namespace DCLF::Features::Fades
{
	namespace
	{
		Api::SceneResourcesId sceneId;  // install
		// The revision inputs' words (postInputs): the event list's ask, so a growth is a new revision's inputs (a kept make, Impl::MakeRevision's
		// reuse, runs the requirements again for it).
		constexpr std::uint32_t kEventsWantedInput = 0, kBucketsInput = 1, kInputWords = 2;
		// The scene's write-back (render thread: Setup, PostInputs), for the ask PostInputs posts; the scene holds it.
		std::weak_ptr<FadeWriteBack> postedWriteBack;

		std::shared_ptr<void> Setup(const Api::SceneResourcesSetup& a_setup)
		{
			using org::services::GrowableContents;
			// Without the fade state program, fades stay the CPU's.
			auto fadeState = ComputeProgram::Load(a_setup.device, { .source = Draws::kFadeStateShader, .constantWords = Draws::kFadeStateConstantWords });
			if (!fadeState) {
				logger::warn("[DCLF] The fade state program could not be created; fades stay the CPU's");
				return nullptr;
			}
			auto scene = std::make_shared<FadeScene>();
			scene->portalBatch = Portals::CreateBatch(a_setup);
			scene->portalQueries = ComputeProgram::Load(a_setup.device, { .source = Draws::kFadeStateShader, .entry = L"PreparePortalQueries", .constantWords = Draws::kFadeStateConstantWords });
			if (!scene->portalBatch || !scene->portalQueries) {
				logger::error("[DCLF] Fade root portal query pipeline is not ready");
				return nullptr;
			}
			scene->fadeState = std::move(fadeState);
			// By root slot. New state is reseeded from the static rows; the frame-ahead outputs are zero-filled.
			const std::uint32_t roots = a_setup.smallStart ? 4u : Draws::kInitialFadeRoots;
			scene->fadeRoots = Draws::MakeVersioned(a_setup.StructuredBuffer(roots, sizeof(FadeRootStatic), "cs.dclf.fade-roots"),
				{ .extent = { 0, sizeof(FadeRootStatic), sizeof(FadeRootStatic) } });
			scene->fadeStates = Draws::MakeVersioned(a_setup.StructuredBuffer(roots, sizeof(FadeNodeState), "cs.dclf.fade-states", true),
				{ .extent = { 0, sizeof(FadeNodeState), sizeof(FadeNodeState) } });
			for (std::uint32_t h = 0; h < 2; ++h)
				scene->fadeStatesOut[h] = Draws::MakeVersioned(a_setup.StructuredBuffer(roots, sizeof(FadeNodeState), h ? "cs.dclf.fade-states-out1" : "cs.dclf.fade-states-out0", true),
					{ .extent = { 0, sizeof(FadeNodeState), sizeof(FadeNodeState) }, .contents = GrowableContents::Zero });
			scene->fadeFrameBuffer = a_setup.StructuredBuffer(1, sizeof(FadeFrame), "cs.dclf.fade-frame");
			scene->fadeRootPrograms = Draws::MakeVersioned(a_setup.StructuredBuffer(roots, sizeof(std::uint32_t), "cs.dclf.fade-root-programs"), { .extent = { 0, 4, 4 } });
			scene->fadeAnimated = Draws::MakeVersioned(a_setup.StructuredBuffer(roots, sizeof(std::uint32_t), "cs.dclf.fade-animated"), { .extent = { 0, 4, 4 } });
			scene->fadeLog = a_setup.StructuredBuffer(kFadeLogEntries, sizeof(FadeLogEntry), "cs.dclf.fade-log", true);
			// The fade write-back: events, the roots' reported milestones, and a staging set (a host buffer per frame slot).
			if (auto* host = RenderGraphRuntime::Get().Host()) {
				const std::uint32_t events = a_setup.smallStart ? 4u : kInitialFadeEvents;
				scene->fadeEvents = Draws::MakeVersioned(Draws::CreateWords(FadeWriteBack::BytesFor(events) / 4, true, "cs.dclf.fade-events"),
					{ .extent = { kFadeEventHeaderWords * 4, kFadeEventWords * 4, 4 } });
				scene->fadeReported = Draws::MakeVersioned(a_setup.StructuredBuffer(roots, 2 * sizeof(std::uint32_t), "cs.dclf.fade-reported", true),
					{ .extent = { 0, 8, 8 }, .contents = GrowableContents::Zero });
				auto writeBack = std::make_shared<FadeWriteBack>();
				writeBack->frameSlots = host->FrameSlots();
				writeBack->armed.resize(writeBack->frameSlots);
				writeBack->AddSet(events);
				postedWriteBack = writeBack;
				scene->fadeWriteBack = std::move(writeBack);
			}
			return scene;
		}

		void RegisterGraph(const Api::SceneResourcesGraph& a_graph)
		{
			const auto& fades = a_graph.State<FadeScene>();
			const auto& batch = *fades.portalBatch;
			Portals::RegisterBatch(a_graph.graph, batch);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.fade-roots"), fades.fadeRoots);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.fade-states"), fades.fadeStates);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.fade-states-out0"), fades.fadeStatesOut[0]);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.fade-states-out1"), fades.fadeStatesOut[1]);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.fade-frame"), fades.fadeFrameBuffer);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.fade-root-programs"), fades.fadeRootPrograms);
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.fade-log"), fades.fadeLog);
			if (fades.fadeEvents) {
				Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.fade-events"), fades.fadeEvents);
				Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.fade-reported"), fades.fadeReported);
			}
		}

		void Require(const Api::SceneResourcesRequirements& a_requirements)
		{
			const auto& s = a_requirements.State<FadeScene>();
			// By root slot, to the root states' family's rows: the static rows, the GPU state, the programs, the animation stamps and the
			// write-back's words.
			Draws::UnitGroup roots;
			roots.Add(s.fadeRoots).Add(s.fadeRootPrograms).Add(s.fadeAnimated).Add(s.fadeStates).Add(s.fadeReported);
			roots.Add(s.fadeStatesOut[0]).Add(s.fadeStatesOut[1]);
			const auto& batch = *s.portalBatch;
			roots.Add(batch.queries).Add(batch.results).Add(batch.ranks).Add(batch.indices).Add(batch.tasks);
			roots.Require(a_requirements.build, Extensions::TableFamilyRows(a_requirements.tables.families, Extensions::RootStatesFamily()));
			Draws::SceneBufferSet::Get().Require(a_requirements.build, batch.counts, 2ull * std::max(1u, a_requirements.inputs[kBucketsInput]));
			Draws::SceneBufferSet::Get().Require(a_requirements.build, batch.offsets, 2ull * std::max(1u, a_requirements.inputs[kBucketsInput]));
			// The event list, to the most a frame wanted (dropped roots append again), as posted with the revision's inputs; its staging sets
			// follow the named capacity.
			if (s.fadeEvents && s.fadeWriteBack)
				Draws::SceneBufferSet::Get().Require(a_requirements.build, s.fadeEvents, a_requirements.inputs[kEventsWantedInput]);
		}

		// The event list's ask (the most a frame wanted: the write-back's harvest grows it).
		void PostInputs(std::span<std::uint32_t> a_words)
		{
			const auto& encoded = PortalViews::Get().Encoded();
			a_words[kBucketsInput] = encoded.size() > kPortalSetBytes / 4 ? encoded[kPortalSetBytes / 4] : 0u;
			if (const auto writeBack = postedWriteBack.lock())
				a_words[kEventsWantedInput] = writeBack->wanted.load(std::memory_order_acquire);
		}

		// What the versions a set names hold of the roots (the root states' family's rows): one past them waits for their growth. Without
		// the fade program no draw reads them.
		void Fit(Api::SceneResourcesFit& a_fit)
		{
			const auto& fades = a_fit.State<FadeScene>();
			const auto family = Extensions::RootStatesFamily();
			if (!fades.fadeState || !fades.fadeRoots || family.index >= Extensions::kTableFamilySlots)
				return;
			const auto units = [&](const Draws::Versioned& a_buffer) {
				return static_cast<std::uint32_t>(std::min<std::uint64_t>(Draws::NamedUnits(a_fit.sealed, a_buffer), UINT32_MAX));
			};
			a_fit.fit.familyRows[family.index] = std::min({ units(fades.fadeRoots), units(fades.fadeStatesOut[0]), units(fades.fadeStatesOut[1]) });
		}

		// The write-back's staging set for the capacity the sealed set names, before any shape of it.
		void Sealed(const Api::SceneResourcesSealed& a_sealed)
		{
			if (const auto& fades = a_sealed.State<FadeScene>(); fades.fadeEvents && fades.fadeWriteBack)
				fades.fadeWriteBack->AddSet(static_cast<std::uint32_t>(a_sealed.sealed.Of(fades.fadeEvents).units));
		}
	}

	FadeScene* SceneOf(const Draws::SceneBuffers& a_scene)
	{
		return Extensions::SceneResourcesOf<FadeScene>(a_scene, sceneId);
	}

	std::shared_ptr<FadeScene> SharedSceneOf(const Draws::SceneBuffers& a_scene)
	{
		return Extensions::SharedSceneResourcesOf<FadeScene>(a_scene, sceneId);
	}

	void RegisterResources(Api::InstallContext& a_context)
	{
		sceneId = a_context.RegisterSceneResources({ .name = "fade roots", .setup = &Setup, .registerGraph = &RegisterGraph, .require = &Require, .fit = &Fit, .postInputs = &PostInputs, .inputWords = kInputWords, .sealed = &Sealed });
	}
}
