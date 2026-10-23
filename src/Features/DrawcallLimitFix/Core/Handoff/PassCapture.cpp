// PassCapture: the registration hooks (RegisterPass and the direct group/list insertions; any registering thread), the withholding
// tests by view, the per-pass record the render thread drains, and the renderer sets refreshed on the render thread.
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"

#include "Features/DrawcallLimitFix/Diagnostics/Feature/OpenDefectProbes.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/RegistrationCensus.h"

#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"

#include <span>
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Policy.h"

namespace DCLF
{
	namespace
	{
		// BSShaderProperty::EShaderPropertyFlag bits the engine's batch-group classifier reads.
		constexpr std::uint64_t kFlagList4 = 1ull << 54;  // -> list 4 on its own
		constexpr std::uint64_t kFlagList2 = 1ull << 36;  // -> adds 2
		constexpr std::uint16_t kAlphaTestingFlag = 1u << 9;

		// A draw family's engine shapes are known by their shader property's vtable (DrawFamilyEntry::engineShapeVtable, resolved at
		// install; an untaken family's is 0, which no property's is).
		const void* ShaderPropertyOf(const RE::BSGeometry* a_geometry) { return a_geometry->GetGeometryRuntimeData().shaderProperty.get(); }

		/** @brief The draw family whose engine shape a_geometry is (kDrawFamilies: none). */
		std::size_t EngineShapeFamily(const RE::BSGeometry* a_geometry)
		{
			const void* property = ShaderPropertyOf(a_geometry);
			const auto& families = Extensions::DrawFamilies();
			for (std::size_t f = 0; f < Extensions::kDrawFamilies; ++f)
				if (Engine::VtableIs(property, families[f].engineShapeVtable))
					return f;
			return Extensions::kDrawFamilies;
		}

		// A draw family's faces' root in a view's roots (Api::ViewRootBit; 0: none).
		std::uint32_t FacesRootBit(std::size_t a_family)
		{
			const auto root = Extensions::DrawFamilies()[a_family].contribution.faces.root;
			return root < Api::ViewRoot::Count ? Api::ViewRootBit(root) : 0u;
		}
	}

	PassCapture& PassCapture::Get()
	{
		static PassCapture capture;
		return capture;
	}

	PassCapture::PassCapture() = default;

	std::uint32_t PassCapture::SubPassOf(bool a_alphaTesting, std::uint64_t a_propertyFlags)
	{
		if (a_propertyFlags & kFlagList4)
			return 4;
		return ((a_propertyFlags & kFlagList2) ? 2u : 0u) | (a_alphaTesting ? 1u : 0u);
	}

	std::uint32_t PassCapture::SubPassOf(const RE::BSGeometry* a_geometry, std::uint64_t a_propertyFlags)
	{
		if (a_propertyFlags & kFlagList4)
			return 4;
		std::uint32_t subPass = (a_propertyFlags & kFlagList2) ? 2u : 0u;
		if (a_geometry) {
			const auto* alpha = a_geometry->GetGeometryRuntimeData().alphaProperty.get();
			if (alpha && (alpha->alphaFlags & kAlphaTestingFlag))
				subPass |= 1u;
		}
		return subPass;
	}

	void PassCapture::Record(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_withheld)
	{
		if (!a_pass || !a_pass->geometry || !a_pass->shader)
			return;
		if (a_pass->shader->shaderType.get() != RE::BSShader::Type::Lighting)
			return;

		// How many threads register, for the record: the engine classifier takes a mutex and the scene
		// lists are built on a job list, so more than one is expected.
		const auto thread = static_cast<std::uint32_t>(GetCurrentThreadId());
		if (lastThread.exchange(thread, std::memory_order_relaxed) != thread)
			threadCount.fetch_add(1, std::memory_order_relaxed);

		const auto slot = cursor.fetch_add(1, std::memory_order_relaxed);
		if (slot >= kCapacity) {
			overflow.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		auto* property = a_pass->geometry->GetGeometryRuntimeData().shaderProperty.get();
		entries[slot] = Entry{ a_pass->geometry, a_pass, a_batch, a_technique,
			SubPassOf(a_pass->geometry, property ? property->flags.underlying() : 0ull), a_pass->passEnum, a_withheld,
			property && property->fadeNode ? property->fadeNode->GetRuntimeData().unk153 : std::uint8_t{ 0xFF }, static_cast<std::uint8_t>(a_pass->accumulationHint) };
	}

	std::span<const PassCapture::Entry> PassCapture::Drain()
	{
		const auto count = std::min(cursor.exchange(0, std::memory_order_relaxed), kCapacity);
		stats.captured = static_cast<std::uint32_t>(count);
		stats.overflowed = overflow.exchange(0, std::memory_order_relaxed);
		stats.threads = threadCount.exchange(0, std::memory_order_relaxed);
		for (std::uint32_t m = 0; m < kShadowModes; ++m)
			stats.shadowWithheld[m] = shadowWithheld[m].exchange(0, std::memory_order_relaxed);
		stats.volumetricWithheld = volumetricWithheld.exchange(0, std::memory_order_relaxed);
		stats.directWithheld = directWithheld.exchange(0, std::memory_order_relaxed);
		stats.mainWithheld = mainWithheld.exchange(0, std::memory_order_relaxed);
		for (std::size_t f = 0; f < Extensions::kDrawFamilies; ++f)
			stats.familyWithheld[f] = familyWithheld[f].exchange(0, std::memory_order_relaxed);
		const auto& views = Extensions::Views();
		for (std::uint32_t i = 0; i < views.ownRendererCount; ++i) {
			const auto v = static_cast<std::size_t>(views.ownRendererViews[i]);
			stats.viewWithheld[v] = viewRenderers[v].ownWithheld.exchange(0, std::memory_order_relaxed);
			for (std::size_t f = 0; f < Extensions::kDrawFamilies; ++f)
				stats.viewFamilyWithheld[v][f] = viewRenderers[v].ownFamilyWithheld[f].exchange(0, std::memory_order_relaxed);
		}
		stats.occlusionWithheld = occlusionWithheld.exchange(0, std::memory_order_relaxed);
		lastDrain = { entries.data(), count };
		return lastDrain;
	}

	namespace
	{
		// An accumulator's batch renderers: its own, its geometry groups' and its alpha group's, in that order. The engine makes groups as
		// geometry first uses them, so a set made from these is made again whenever the list changes, not only when the accumulators do.
		void CollectRenderers(const void* a_accumulator, std::vector<const RE::BSBatchRenderer*>& a_out)
		{
			const auto* accumulator = static_cast<const RE::BSGraphics::BSShaderAccumulator*>(a_accumulator);
			const auto* batch = accumulator ? accumulator->GetRuntimeData().batchRenderer : nullptr;
			if (!batch)
				return;
			a_out.push_back(batch);
			for (const auto* group : batch->geometryGroups)
				if (group && group->batchRenderer)
					a_out.push_back(group->batchRenderer);
			if (batch->alphaGroup && batch->alphaGroup->batchRenderer)
				a_out.push_back(batch->alphaGroup->batchRenderer);
		}
	}

	void PassCapture::RefreshMainRenderers()
	{
		// The Z-prepass's accumulator (render mode 0xC) and the main camera's (render mode 0), as the registration jobs
		// (FUN_1414cbff0) take them (skyrim-engine-notes.md, "The primary's cull: the scene lists").
		static REL::Relocation<RE::BSGraphics::BSShaderAccumulator**> depthAccumulator{ REL::Offset(Engine::Address::kDepthAccumulator) };
		static REL::Relocation<RE::BSGraphics::BSShaderAccumulator**> mainAccumulator{ REL::Offset(Engine::Address::kMainAccumulator) };
		const std::array<const void*, 2> accumulators{ *depthAccumulator, *mainAccumulator };
		// The InMainRenderers views' accumulators (their callbacks: the first person's, Main::unk_FPCulling culls the first-person root
		// alone into it and FUN_1414cb6d0 renders it). Their registrations are their members' alone, so a member's there is the view's
		// member's (WithholdMain).
		const auto& views = Extensions::Views();
		std::array<Api::ViewAccumulators, Extensions::kEngineViews> viewAccumulators{};
		std::vector<const RE::BSBatchRenderer*> sources;
		for (const void* accumulator : accumulators)
			CollectRenderers(accumulator, sources);
		std::array<std::size_t, Extensions::kEngineViews + 1> viewSourcesAt{};
		for (std::uint32_t i = 0; i < views.mainRendererCount; ++i) {
			const auto v = static_cast<std::size_t>(views.mainRendererViews[i]);
			views.views[v].accumulators(viewAccumulators[v]);
			viewSourcesAt[i] = sources.size();
			for (std::uint32_t a = 0; a < viewAccumulators[v].count; ++a)
				CollectRenderers(viewAccumulators[v].accumulators[a], sources);
		}
		viewSourcesAt[views.mainRendererCount] = sources.size();
		if (sources == mainSources && std::atomic_load(&mainRenderers))
			return;
		mainAccumulators = accumulators;
		mainSources = sources;
		auto renderers = std::make_shared<RendererSet>(sources.begin(), sources.end());
		for (std::uint32_t i = 0; i < views.mainRendererCount; ++i) {
			const auto v = static_cast<std::size_t>(views.mainRendererViews[i]);
			viewRenderers[v].accumulators = viewAccumulators[v];
			auto viewSet = std::make_shared<RendererSet>(sources.begin() + viewSourcesAt[i], sources.begin() + viewSourcesAt[i + 1]);
			std::atomic_store(&viewRenderers[v].renderers, std::shared_ptr<const RendererSet>(std::move(viewSet)));
		}
		logger::info("[DCLF] the main camera's views: {} batch renderers (Z-prepass accumulator {}, main {})", renderers->size(), accumulators[0], accumulators[1]);
		std::atomic_store(&mainRenderers, std::shared_ptr<const RendererSet>(std::move(renderers)));
	}

	bool PassCapture::WithholdMain(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass)
	{
		if (!a_pass || !a_pass->geometry || !Policy::WithholdFromEngine() || !IsMainRenderer(a_batch))
			return false;
		// Tree LOD (dclf-architecture.md, "Tree LOD: the draws"): DCLF draws every instance the mirror holds this frame, so the engine's
		// passes of every tree LOD shape (its Z-prepass's, hint 7 into group 1 through the shadow modes' registration; its colour
		// pass's, hint 7 into group 1) are withheld. So for every draw family DCLF draws this frame: its engine shapes' passes.
		for (std::size_t f = 0; f < Extensions::kDrawFamilies; ++f)
			if (familyOwned[f].load(std::memory_order_acquire) && Engine::VtableIs(ShaderPropertyOf(a_pass->geometry), Extensions::DrawFamilies()[f].engineShapeVtable)) {
				familyWithheld[f].fetch_add(1, std::memory_order_relaxed);
				return true;
			}
		const auto set = std::atomic_load(&frameSet);
		const std::uint8_t phases = set ? set->PhasesOf(a_pass->geometry) : std::uint8_t{ 0 };
		const auto& views = Extensions::Views();
		for (std::uint32_t i = 0; i < views.mainRendererCount; ++i)
			if (IsViewRenderer(views.mainRendererViews[i], a_batch))
				viewRenderers[static_cast<std::size_t>(views.mainRendererViews[i])].registered.fetch_add(1, std::memory_order_relaxed);
		// An InMainRenderers view's member (the first person's: kSetFirstPerson alone, no world view registers it, its root hidden there):
		// its passes into the view's accumulators and into the main views' (the first person's RenderDepth block culls into the
		// Z-prepass's) are the view's epochs' while they draw this frame (SetViewCovered); every fade of the skeleton is the engine's
		// root's (alpha 1).
		for (std::uint32_t i = 0; i < views.mainRendererCount; ++i) {
			const auto v = static_cast<std::size_t>(views.mainRendererViews[i]);
			const std::uint8_t phase = views.views[v].phase;
			if (!(phases & phase))
				continue;
			auto& view = viewRenderers[v];
			if (ViewCovered(views.mainRendererViews[i]) && (set->drawn & phase)) {
				view.withheld.fetch_add(1, std::memory_order_relaxed);
				return true;
			}
			view.memberKept.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		if (!(phases & kSetMain))
			return false;
		// Every pass of a member, a fade's and a LOD cross-fade's hint-10 copy of the old level included: DCLF draws them (the
		// draw's alpha takes its root's FadeStateCS fade, and BuildDraws adds the copy partitions while the root crosses).
		mainWithheld.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	void PassCapture::SetViewRenderers(Api::EngineView a_view, const Api::ViewAccumulators& a_accumulators)
	{
		// The view's accumulators (the reflection's: BSCubeMapCamera's two, the faces cull into the first): their batch renderers, their
		// geometry groups' and their alpha groups', made again whenever any of them changes.
		auto& view = viewRenderers[static_cast<std::size_t>(a_view)];
		std::vector<const RE::BSBatchRenderer*> sources;
		for (std::uint32_t a = 0; a < a_accumulators.count; ++a)
			CollectRenderers(a_accumulators.accumulators[a], sources);
		if (a_accumulators == view.accumulators && sources == view.sources && std::atomic_load(&view.renderers))
			return;
		view.accumulators = a_accumulators;
		view.sources = sources;
		auto renderers = std::make_shared<RendererSet>(sources.begin(), sources.end());
		std::string accumulators;
		for (std::uint32_t a = 0; a < a_accumulators.count; ++a)
			accumulators += fmt::format("{}{}", a ? ", " : "", a_accumulators.accumulators[a]);
		std::string batches;
		for (const auto* batch : sources)
			batches += fmt::format("{}{}", batches.empty() ? "" : ", ", static_cast<const void*>(batch));
		logger::info("[DCLF] the {} faces: {} batch renderers (accumulators {}; renderers {})", Extensions::ViewEntryOf(a_view).name, renderers->size(), accumulators, batches);
		std::atomic_store(&view.renderers, std::shared_ptr<const RendererSet>(std::move(renderers)));
	}

	bool PassCapture::OwnViewRendering() const
	{
		const auto& views = Extensions::Views();
		for (std::uint32_t i = 0; i < views.ownRendererCount; ++i)
			if (viewRenderers[static_cast<std::size_t>(views.ownRendererViews[i])].rendering.load(std::memory_order_acquire))
				return true;
		return false;
	}

	bool PassCapture::OwnViewWithholding() const
	{
		const auto& views = Extensions::Views();
		for (std::uint32_t i = 0; i < views.ownRendererCount; ++i)
			if (viewRenderers[static_cast<std::size_t>(views.ownRendererViews[i])].withholdActive.load(std::memory_order_acquire) && ViewCovered(views.ownRendererViews[i]))
				return true;
		return false;
	}

	bool PassCapture::WithholdOwn(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass)
	{
		if (!a_pass || !a_pass->geometry || !Policy::WithholdFromEngine())
			return false;
		const auto& views = Extensions::Views();
		for (std::uint32_t i = 0; i < views.ownRendererCount; ++i) {
			const auto v = static_cast<std::size_t>(views.ownRendererViews[i]);
			auto& view = viewRenderers[v];
			if (!view.withholdActive.load(std::memory_order_acquire) || !ViewCovered(views.ownRendererViews[i]))
				continue;
			const auto renderers = std::atomic_load(&view.renderers);
			if (!renderers || !renderers->contains(a_batch))
				continue;
			// A draw family's engine shape (tree LOD's), withheld while the view's faces draw the family. One of a family without faces
			// (grass) is never a view's: the view's own renderers draw it, and it is no residue of the view's roots.
			const std::size_t family = EngineShapeFamily(a_pass->geometry);
			if (family < Extensions::kDrawFamilies && !Extensions::DrawFamilyHasFaces(static_cast<Api::DrawFamily>(family)))
				return false;
			const bool treeLod = family < Extensions::kDrawFamilies;
			if (treeLod && (view.familyOwned.load(std::memory_order_acquire) & (1u << family))) {
				view.ownFamilyWithheld[family].fetch_add(1, std::memory_order_relaxed);
				return true;
			}
			const auto set = std::atomic_load(&frameSet);
			const std::uint8_t phase = views.views[v].phase;
			if (!set || !(set->drawn & phase) || !(set->PhasesOf(a_pass->geometry) & phase)) {
				// The residue watch: a draw family's engine shape under its faces' root, anything else but the sky under the view root
				// contributions' roots (the LOD land and objects').
				if (const auto watch = view.residueWatch.load(std::memory_order_acquire); watch & (treeLod ? FacesRootBit(family) : views.rootBits))
					NoteViewResidue(view, a_pass, treeLod);
				return false;
			}
			view.ownWithheld.fetch_add(1, std::memory_order_relaxed);
			return true;
		}
		return false;
	}

	void PassCapture::NoteViewResidue(ViewRenderers& a_view, const RE::BSRenderPass* a_pass, bool a_treeLod)
	{
		const auto* sky = RE::Sky::GetSingleton();
		const RE::NiNode* skyRoot = sky ? sky->root.get() : nullptr;
		for (const RE::NiAVObject* node = a_pass->geometry; node; node = node->parent)
			if (node == skyRoot)
				return;
		if (!a_treeLod) {
			std::scoped_lock lock(a_view.residueLock);
			if (a_view.residueGeometries.size() < 4096)
				a_view.residueGeometries.push_back(a_pass->geometry);
		}
		if ((a_treeLod ? a_view.residueTree : a_view.residueLod).fetch_add(1, std::memory_order_relaxed) == 0) {
			std::scoped_lock lock(a_view.residueLock);
			if (a_view.residueFirst.empty()) {
				const auto* name = a_pass->geometry->name.c_str();
				a_view.residueFirst = fmt::format("'{}' (technique {:#x}, hint {})", name ? name : "?", a_pass->passEnum, static_cast<std::uint32_t>(a_pass->accumulationHint));
			}
		}
	}

	std::vector<const RE::BSGeometry*> PassCapture::TakeViewResidueGeometries(Api::EngineView a_view)
	{
		auto& view = viewRenderers[static_cast<std::size_t>(a_view)];
		std::scoped_lock lock(view.residueLock);
		return std::exchange(view.residueGeometries, {});
	}

	PassCapture::ViewResidue PassCapture::TakeViewResidue(Api::EngineView a_view)
	{
		auto& view = viewRenderers[static_cast<std::size_t>(a_view)];
		ViewResidue out;
		out.lodPasses = view.residueLod.exchange(0, std::memory_order_relaxed);
		out.treePasses = view.residueTree.exchange(0, std::memory_order_relaxed);
		std::scoped_lock lock(view.residueLock);
		out.first = std::move(view.residueFirst);
		view.residueFirst.clear();
		return out;
	}

	bool PassCapture::ShadowWithholdingEnabled()
	{
		return ActiveToggles().shadows && !!Policy::WithholdFromEngine();
	}

	void PassCapture::SetShadowBatchRenderers(std::shared_ptr<const ShadowRendererMap> a_renderers)
	{
		std::atomic_store(&shadowRenderers, std::move(a_renderers));
	}

	void PassCapture::SetFocusBatchRenderers(std::shared_ptr<const ShadowRendererMap> a_renderers)
	{
		std::atomic_store(&focusRenderers, std::move(a_renderers));
	}

	bool PassCapture::ShadowModeOfBatch(const RE::BSBatchRenderer* a_batch, std::uint32_t& a_mode) const
	{
		const auto renderers = std::atomic_load(&shadowRenderers);
		if (!renderers)
			return false;
		const auto it = renderers->find(a_batch);
		if (it == renderers->end() || it->second >= kShadowModes)
			return false;
		a_mode = it->second;
		return true;
	}

	bool PassCapture::CastersWithheld(std::uint8_t a_phase) const
	{
		const auto set = std::atomic_load(&frameSet);
		return set && (set->drawn & a_phase) && ShadowWithholdingEnabled();
	}

	std::shared_ptr<const SetSnapshot> PassCapture::CastersForBatch(const RE::BSBatchRenderer* a_batch, std::uint8_t* a_phase) const
	{
		std::uint32_t mode = 0;
		if (!ShadowModeOfBatch(a_batch, mode) || !ShadowWithholdingEnabled())
			return nullptr;
		const std::uint8_t phase = SetPhaseOfMode(mode);
		if (a_phase)
			*a_phase = phase;
		auto set = std::atomic_load(&frameSet);
		return set && (set->drawn & phase) ? set : nullptr;
	}

	bool PassCapture::Withhold(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass)
	{
		if (!a_pass || !a_pass->geometry)
			return false;
		// A shadow view's renderers (the focus views' among them): the set's casters. A pass into any other renderer
		// (reflections, cubemaps) is never withheld here; the main camera's are WithholdMain's, an OwnRenderers view's WithholdOwn's.
		// What the engine registers into a covered focus view, for its observer (Features/Shadows/Local's focus parity: against its target's fade root).
		if (Diagnostics::Armed<Diagnostics::FocusRegistration>())
			if (const auto focus = std::atomic_load(&focusRenderers); focus && !focus->empty())
				if (const auto it = focus->find(a_batch); it != focus->end())
					Diagnostics::Notify(Diagnostics::FocusRegistration{ it->second, a_pass->geometry });
		std::uint32_t mode = 0;
		std::uint8_t phase = 0;
		if (const auto set = CastersForBatch(a_batch, &phase); set && ShadowModeOfBatch(a_batch, mode) && (set->PhasesOf(a_pass->geometry) & phase)) {
			shadowWithheld[mode].fetch_add(1, std::memory_order_relaxed);
			return true;
		}
		return false;
	}

	void PassCapture::NoteKept(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, std::uint8_t a_site, std::uint8_t a_occlusionPhase) const
	{
		// The census's per-pass values, read here as the pass registers (the frame's end has neither the pass, a pooled object, nor
		// these states): whether the reflection faces withhold now (WithholdOwn's own gates).
		const bool face = OwnViewWithholding();
		Diagnostics::Notify(Diagnostics::PassKept{ a_batch, a_pass, static_cast<RegistrationCensus::Site>(a_site), face, a_occlusionPhase });
	}

	namespace
	{
		thread_local std::uint32_t passesOnThisThread = 0;
		constexpr auto Census(RegistrationCensus::Site a_site) { return static_cast<std::uint8_t>(a_site); }
	}

	std::uint32_t PassCapture::PassesOnThisThread()
	{
		return passesOnThisThread;
	}

	struct PassCapture::Hook
	{
		static void thunk(RE::BSBatchRenderer* a_this, RE::BSRenderPass* a_pass, std::uint32_t a_techniqueID)
		{
			++passesOnThisThread;
			auto& capture = PassCapture::Get();
			if (capture.bypassed.load(std::memory_order_acquire)) {
				func(a_this, a_pass, a_techniqueID);
				return;
			}
			// Withholding is the whole of ownership: the pass is built, lit and shadowed as the engine would - only the batch
			// renderer never receives it, so the native loop has nothing to draw. Record notes it for the observers.
			const bool withheld = capture.WithholdMain(a_this, a_pass) || capture.WithholdOwn(a_this, a_pass) || capture.Withhold(a_this, a_pass);
			capture.Record(a_this, a_pass, a_techniqueID, withheld);
			Diagnostics::Notify(Diagnostics::PointLightPass{ a_this, a_pass, withheld, 0 });
			if (withheld)
				return;
			if (Diagnostics::Armed<Diagnostics::PassKept>())
				capture.NoteKept(a_this, a_pass, Census(RegistrationCensus::Site::RegisterPass));
			func(a_this, a_pass, a_techniqueID);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/**
	 * @brief The shadow modes' registration inserting an accumulation hint 8 pass (a volumetric-only caster)
	 * into batch group 15: AE FUN_1414b2a60's direct call to FUN_1414f5090(batch, pass, 15, 0) at +0xFF. The
	 * pass is withheld when the batch renderer is a shadow view's and the geometry is in the frame's set with the phase of
	 * that view's render mode (SetPhaseOfMode).
	 */
	bool PassCapture::WithholdAtGroup(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, std::atomic<std::uint32_t>& a_counter)
	{
		if (!a_pass || !a_pass->geometry || bypassed.load(std::memory_order_acquire))
			return false;
		std::uint8_t phase = 0;
		const auto set = CastersForBatch(a_batch, &phase);
		const bool member = set && (set->PhasesOf(a_pass->geometry) & phase);
		if (member)
			a_counter.fetch_add(1, std::memory_order_relaxed);
		return member;
	}

	struct PassCapture::VolumetricGroupHook
	{
		static void thunk(RE::BSBatchRenderer* a_batch, RE::BSRenderPass* a_pass, std::uint32_t a_group, std::uint32_t a_arg)
		{
			++passesOnThisThread;
			auto& capture = PassCapture::Get();
			const bool withheld = (!capture.bypassed.load(std::memory_order_acquire) && capture.WithholdMain(a_batch, a_pass)) ||
			                      capture.WithholdAtGroup(a_batch, a_pass, capture.volumetricWithheld);
			Diagnostics::Notify(Diagnostics::PointLightPass{ a_batch, a_pass, withheld, 8 });
			if (withheld)
				return;
			if (Diagnostics::Armed<Diagnostics::PassKept>() && !capture.bypassed.load(std::memory_order_acquire))
				capture.NoteKept(a_batch, a_pass, Census(RegistrationCensus::Site::Volumetric));
			func(a_batch, a_pass, a_group, a_arg);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/**
	 * @brief The same registration's other direct insertions (FUN_1414b2a60): accumulation hint 11 into batch
	 * group 9 (+0xC9), hint 7 into group 1 (+0xDB) and hint 3, a decal-flagged caster such as an NPC's face
	 * part, into group 4 (+0xED). None of them reaches RegisterPass, so a member registered with one of these hints is
	 * withheld here.
	 */
	template <std::uint32_t Hint>
	struct PassCapture::DirectGroupHook
	{
		static void thunk(RE::BSBatchRenderer* a_batch, RE::BSRenderPass* a_pass, std::uint32_t a_group, std::uint32_t a_arg)
		{
			++passesOnThisThread;
			auto& capture = PassCapture::Get();
			const bool withheld = (!capture.bypassed.load(std::memory_order_acquire) && capture.WithholdMain(a_batch, a_pass)) ||
			                      capture.WithholdAtGroup(a_batch, a_pass, capture.directWithheld);
			Diagnostics::Notify(Diagnostics::PointLightPass{ a_batch, a_pass, withheld, Hint });
			if (withheld)
				return;
			if (Diagnostics::Armed<Diagnostics::PassKept>() && !capture.bypassed.load(std::memory_order_acquire))
				capture.NoteKept(a_batch, a_pass, Census(RegistrationCensus::Site::ShadowDirect));
			func(a_batch, a_pass, a_group, a_arg);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/**
	 * @brief The main modes' registration (FUN_1414b2330, render modes 0-11: the main camera's accumulator among them) inserting a
	 * pass straight into a geometry group by its accumulation hint (FUN_1414f5090(batch, pass, group, flag)): hints 2-7, 9, 11,
	 * 13-18, and a multi-index shape's layer (hint 12, group 2). None of them reaches RegisterPass, so a member's pass with one of
	 * these hints (terrain LOD's 6 into group 0, object LOD's 7 into group 1) is withheld here. The Z-prepass's accumulator
	 * (render mode 0xC) registers through the shadow modes' function (FUN_1414b2a60), whose insertions are the hooks above.
	 */
	template <std::uint32_t Group>
	struct PassCapture::MainGroupHook
	{
		static void thunk(RE::BSBatchRenderer* a_batch, RE::BSRenderPass* a_pass, std::uint32_t a_group, std::uint32_t a_arg)
		{
			++passesOnThisThread;
			auto& capture = PassCapture::Get();
			if (!capture.bypassed.load(std::memory_order_acquire) && capture.WithholdMain(a_batch, a_pass)) {
				if constexpr (Group == 5)
					Diagnostics::Notify(Diagnostics::MemberGroup5PassWithheld{ a_pass });
				return;
			}
			if (!capture.bypassed.load(std::memory_order_acquire) && capture.WithholdOwn(a_batch, a_pass))
				return;
			if (Diagnostics::Armed<Diagnostics::PassKept>() && !capture.bypassed.load(std::memory_order_acquire))
				capture.NoteKept(a_batch, a_pass, Census(RegistrationCensus::Site::MainGroup));
			func(a_batch, a_pass, a_group, a_arg);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/** @brief The same registration's hint 1, into a pass list of the batch renderer (FUN_1414f50b0(batch, pass, list)). */
	struct PassCapture::MainListHook
	{
		static void thunk(RE::BSBatchRenderer* a_batch, RE::BSRenderPass* a_pass, void* a_list)
		{
			++passesOnThisThread;
			auto& capture = PassCapture::Get();
			if (!capture.bypassed.load(std::memory_order_acquire) && capture.WithholdMain(a_batch, a_pass)) {
				Diagnostics::Notify(Diagnostics::MemberListPassWithheld{ a_pass });
				return;
			}
			if (!capture.bypassed.load(std::memory_order_acquire) && capture.WithholdOwn(a_batch, a_pass))
				return;
			if (Diagnostics::Armed<Diagnostics::PassKept>() && !capture.bypassed.load(std::memory_order_acquire))
				capture.NoteKept(a_batch, a_pass, Census(RegistrationCensus::Site::MainList));
			func(a_batch, a_pass, a_list);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/**
	 * @brief The occlusion maps' registration (FUN_1414b2c20, render mode 0x1C: Precipitation::SetupMask's accumulator) inserting a
	 * pass into geometry group 14 (FUN_1414f5090(batch, pass, 14), its one insertion): a member of the map's occluder phase is
	 * withheld, DCLF drawing it into the map (its shadow epoch, FrameDriver::AfterShadowMaps); the engine draws every other occluder.
	 */
	struct PassCapture::OcclusionGroupHook
	{
		static void thunk(RE::BSBatchRenderer* a_batch, RE::BSRenderPass* a_pass, std::uint32_t a_group, std::uint32_t a_arg)
		{
			++passesOnThisThread;
			auto& capture = PassCapture::Get();
			const std::uint8_t phase = capture.occlusionPhase.load(std::memory_order_acquire);
			if (phase && a_pass && a_pass->geometry && !!Policy::WithholdFromEngine() && !capture.bypassed.load(std::memory_order_acquire))
				if (const auto set = std::atomic_load(&capture.frameSet); set && (set->drawn & phase) && (set->PhasesOf(a_pass->geometry) & phase)) {
					capture.occlusionWithheld.fetch_add(1, std::memory_order_relaxed);
					return;
				}
			if (Diagnostics::Armed<Diagnostics::PassKept>() && !capture.bypassed.load(std::memory_order_acquire))
				capture.NoteKept(a_batch, a_pass, Census(RegistrationCensus::Site::Occlusion), phase);
			func(a_batch, a_pass, a_group, a_arg);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	namespace
	{
		// The main modes' registration's (FUN_1414b2330) group insertions and its hint 1 list insertion, by call site.
		constexpr std::uintptr_t kInsertGroup = 0x14f5090, kInsertList = 0x14f50b0;
		constexpr std::array<std::pair<std::uintptr_t, std::uintptr_t>, 16> kMainGroupSites{ {
			{ 0x14b24a2, kInsertGroup }, { 0x14b24bd, kInsertGroup }, { 0x14b24d8, kInsertGroup }, { 0x14b24f3, kInsertGroup },
			{ 0x14b250d, kInsertGroup }, { 0x14b2528, kInsertGroup }, { 0x14b2543, kInsertGroup }, { 0x14b255e, kInsertGroup },
			{ 0x14b2579, kInsertGroup }, { 0x14b2591, kInsertGroup }, { 0x14b25a9, kInsertGroup }, { 0x14b25c1, kInsertGroup },
			{ 0x14b25db, kInsertGroup }, { 0x14b2667, kInsertGroup }, { 0x14b2478, kInsertList }, { 0x14b2c71, kInsertGroup },
		} };

		template <class Hook>
		void InstallMainGroup(std::size_t a_site)
		{
			stl::write_thunk_call<Hook>(REL::Offset(kMainGroupSites[a_site].first).address());
		}
	}

	std::string PassCapture::SitesMissing()
	{
		for (const auto& [site, callee] : kMainGroupSites)
			if (!Engine::CallsTo(REL::Offset(site).address(), REL::Offset(callee).address())) {
				logger::warn("[DCLF] pass capture: the call at {:#x} is not the engine's group insertion", 0x140000000 + site);
				return "the main registration's group insertions";
			}
		return {};
	}

	void PassCapture::Install()
	{
		if (installed)
			return;
		stl::write_vfunc<0x2, Hook>(RE::VTABLE_BSBatchRenderer[0]);
		// The registration's (FUN_1414b2a60) group insertions: the volumetric copy's, and the direct hints 11, 7 and 3.
		constexpr std::uintptr_t kVolumetricGroupCall = 0x14b2b5f;
		constexpr std::uintptr_t kDirectGroupCalls[3] = { 0x14b2b29, 0x14b2b3b, 0x14b2b4d };
		stl::write_thunk_call<VolumetricGroupHook>(REL::Offset(kVolumetricGroupCall).address());
		stl::write_thunk_call<DirectGroupHook<11>>(REL::Offset(kDirectGroupCalls[0]).address());
		stl::write_thunk_call<DirectGroupHook<7>>(REL::Offset(kDirectGroupCalls[1]).address());
		stl::write_thunk_call<DirectGroupHook<3>>(REL::Offset(kDirectGroupCalls[2]).address());
		// The main modes' group insertions (kMainGroupSites, in order), by the group each inserts into, and the hint 1 list insertion.
		// PostPostLoad checked every site against its callee (SitesMissing) before any hook was written.
		if (!SitesMissing().empty())
			stl::report_and_fail("Drawcall Limit Fix: the main registration's group insertions changed after their sites were checked");
		InstallMainGroup<MainGroupHook<3>>(0);
		InstallMainGroup<MainGroupHook<4>>(1);
		InstallMainGroup<MainGroupHook<5>>(2);
		InstallMainGroup<MainGroupHook<6>>(3);
		InstallMainGroup<MainGroupHook<0>>(4);
		InstallMainGroup<MainGroupHook<1>>(5);
		InstallMainGroup<MainGroupHook<7>>(6);
		InstallMainGroup<MainGroupHook<9>>(7);
		InstallMainGroup<MainGroupHook<10>>(8);
		InstallMainGroup<MainGroupHook<11>>(9);
		InstallMainGroup<MainGroupHook<8>>(10);
		InstallMainGroup<MainGroupHook<12>>(11);
		InstallMainGroup<MainGroupHook<13>>(12);
		InstallMainGroup<MainGroupHook<2>>(13);
		InstallMainGroup<MainListHook>(14);
		InstallMainGroup<OcclusionGroupHook>(15);
		installed = true;
		logger::info("[DCLF] pass capture installed on BSBatchRenderer::RegisterPass");
	}
}
