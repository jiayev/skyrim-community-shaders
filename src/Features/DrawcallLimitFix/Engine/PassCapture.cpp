#include "PassCapture.h"
#include "Features/DrawcallLimitFix/Common/FrameTrace.h"

#include "Features/DrawcallLimitFix/Diagnostics/OpenDefectProbes.h"

#include "LocalLightCull.h"

#include "Features/DrawcallLimitFix/Common/Switches.h"
#include "Features/DrawcallLimitFix/Common/Toggles.h"
#include "Features/DrawcallLimitFix/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Scene/TreeLod.h"

#include <span>

namespace DCLF
{
	namespace
	{
		// BSShaderProperty::EShaderPropertyFlag bits the engine's batch-group classifier reads.
		constexpr std::uint64_t kFlagList4 = 1ull << 54;  // -> list 4 on its own
		constexpr std::uint64_t kFlagList2 = 1ull << 36;  // -> adds 2
		constexpr std::uint16_t kAlphaTestingFlag = 1u << 9;
	}

	PassCapture& PassCapture::Get()
	{
		static PassCapture capture;
		return capture;
	}

	PassCapture::PassCapture() = default;

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

	bool PassCapture::FadingAtRegistration(const RE::BSRenderPass* a_pass)
	{
		const auto* geometry = a_pass ? a_pass->geometry : nullptr;
		if (!geometry)
			return false;
		// Accumulation hint 10 is always the native loop's: BSLightingShader::SetupGeometry draws it with the
		// stencil dither (stencil mode 0xB, reference fade * 31) and, for a LOD cross-fade's single-level copy,
		// MaterialData.z scaled by the fade node's cross-fade factor. Neither is modelled.
		if (a_pass->accumulationHint == 10)
			return true;
		const auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
		const auto* fadeNode = property ? property->fadeNode : nullptr;
		if (!fadeNode)
			return false;
		const auto& fade = fadeNode->GetRuntimeData();
		// A fade the engine draws in an opaque group is DCLF's (CS_DCLF_FADING): the pass is registered as usual
		// and the fade reaches the shader in MaterialData.z. One it draws blended (accumulation hint 9, drawn
		// with the transparent objects after the composite) is not, and neither is a fading decal (hints 2 and
		// 3). That one is a precaution, not a measurement: the decal probe's state mismatches turned out not to
		// depend on the fade.
		const auto hint = a_pass->accumulationHint;
		if (fade.currentFade < 1.0f && (!ActiveToggles().fading || hint == 9 || hint == 2 || hint == 3))
			return true;
		// A LOD cross-fade (kMeshLOD, fade node LOD state +0x153 & 0x70 not 0x20) is not a fade of the object:
		// GetRenderPasses (AE 1414adfb0) keeps its pass as it is - the new level, drawn as any settled object
		// is - and adds the old level as a hint-10 copy, which is the native loop's (above). DCLF keeps the
		// object through the crossing and the native loop draws only the copy (CS_DCLF_LOD_CROSSFADE); off,
		// the whole object is the native loop's until the crossing ends.
		return !ActiveToggles().lodCrossfade && geometry->GetFlags().any(RE::NiAVObject::Flag::kMeshLOD) && (fade.unk153 & 0x70) != 0x20;
	}

	void PassCapture::Record(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_fading, bool a_withheld)
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
			SubPassOf(a_pass->geometry, property ? property->flags.underlying() : 0ull), a_pass->passEnum, a_fading, a_withheld,
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
		stats.treeLodWithheld = treeLodWithheld.exchange(0, std::memory_order_relaxed);
		stats.reflectionWithheld = reflectionWithheld.exchange(0, std::memory_order_relaxed);
		stats.reflectionTreeLodWithheld = reflectionTreeLodWithheld.exchange(0, std::memory_order_relaxed);
		stats.mainCrossfadeCopies = mainCrossfadeCopies.exchange(0, std::memory_order_relaxed);
		stats.mainUnmodelledFades = mainUnmodelledFades.exchange(0, std::memory_order_relaxed);
		stats.occlusionWithheld = occlusionWithheld.exchange(0, std::memory_order_relaxed);
		lastDrain = { entries.data(), count };
		return lastDrain;
	}

	bool PassCapture::ParityBoth()
	{
		static const bool both = SwitchEnabled(Switch::ParityBoth);
		return both;
	}

	void PassCapture::RefreshMainRenderers()
	{
		// The Z-prepass's accumulator (render mode 0xC) and the main camera's (render mode 0), as the registration jobs
		// (FUN_1414cbff0) take them (skyrim-engine-notes.md, "The primary's cull: the scene lists").
		static REL::Relocation<RE::BSGraphics::BSShaderAccumulator**> depthAccumulator{ REL::Offset(0x338c828) };
		static REL::Relocation<RE::BSGraphics::BSShaderAccumulator**> mainAccumulator{ REL::Offset(0x338c830) };
		const std::array<const void*, 2> accumulators{ *depthAccumulator, *mainAccumulator };
		if (accumulators == mainAccumulators && std::atomic_load(&mainRenderers))
			return;
		mainAccumulators = accumulators;
		auto renderers = std::make_shared<RendererSet>();
		for (auto* accumulator : { *depthAccumulator, *mainAccumulator }) {
			auto* batch = accumulator ? accumulator->GetRuntimeData().batchRenderer : nullptr;
			if (!batch)
				continue;
			renderers->insert(batch);
			for (auto* group : batch->geometryGroups)
				if (group && group->batchRenderer)
					renderers->insert(group->batchRenderer);
		}
		logger::info("[DCLF] the main camera's views: {} batch renderers (Z-prepass accumulator {}, main {})", renderers->size(), accumulators[0], accumulators[1]);
		std::atomic_store(&mainRenderers, std::shared_ptr<const RendererSet>(std::move(renderers)));
	}

	bool PassCapture::WithholdMain(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass)
	{
		if (!a_pass || !a_pass->geometry || ParityBoth() || !IsMainRenderer(a_batch))
			return false;
		// Tree LOD (dclf-lod.md, "Tree LOD: the draws"): DCLF draws every instance the mirror holds this frame, so the engine's
		// passes of every tree LOD shape (its Z-prepass's, hint 7 into group 1 through the shadow modes' registration; its colour
		// pass's, hint 7 into group 1) are withheld.
		if (treeLodOwned.load(std::memory_order_acquire) && TreeLod::IsTreeLodShape(a_pass->geometry)) {
			treeLodWithheld.fetch_add(1, std::memory_order_relaxed);
			return true;
		}
		const auto set = std::atomic_load(&frameSet);
		if (!set || !(set->PhasesOf(a_pass->geometry) & kSetMain))
			return false;
		// A LOD cross-fade's copy of the old level is another draw than the member's own: the engine's.
		if (a_pass->accumulationHint == 10) {
			mainCrossfadeCopies.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		// A fade DCLF does not model (blended, or a decal's): the engine draws it, and the member leaves the set at the next commit
		// until the fade ends (SceneStore::CommitSet).
		if (FadingAtRegistration(a_pass)) {
			unmodelledFades.Push(a_pass->geometry);
			mainUnmodelledFades.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		mainWithheld.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	void PassCapture::SetReflectionCamera(const RE::NiCamera* a_camera)
	{
		// BSCubeMapCamera's two accumulators (+0x1A0, +0x1A8; skyrim-engine-notes.md, "Water reflections: the cube map"): the faces
		// cull into the first.
		if (!a_camera)
			return;
		const auto* bytes = reinterpret_cast<const std::byte*>(a_camera);
		const std::array<const void*, 2> accumulators{ *reinterpret_cast<void* const*>(bytes + 0x1A0), *reinterpret_cast<void* const*>(bytes + 0x1A8) };
		if (accumulators == reflectionAccumulators && std::atomic_load(&reflectionRenderers))
			return;
		reflectionAccumulators = accumulators;
		auto renderers = std::make_shared<RendererSet>();
		for (const void* entry : accumulators) {
			const auto* accumulator = static_cast<const RE::BSGraphics::BSShaderAccumulator*>(entry);
			auto* batch = accumulator ? accumulator->GetRuntimeData().batchRenderer : nullptr;
			if (!batch)
				continue;
			renderers->insert(batch);
			for (auto* group : batch->geometryGroups)
				if (group && group->batchRenderer)
					renderers->insert(group->batchRenderer);
		}
		logger::info("[DCLF] the reflection faces: {} batch renderers (accumulators {}, {})", renderers->size(), accumulators[0], accumulators[1]);
		std::atomic_store(&reflectionRenderers, std::shared_ptr<const RendererSet>(std::move(renderers)));
	}

	bool PassCapture::WithholdReflection(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass)
	{
		if (!a_pass || !a_pass->geometry || ParityBoth() || !reflectionFace.load(std::memory_order_acquire))
			return false;
		const auto renderers = std::atomic_load(&reflectionRenderers);
		if (!renderers || !renderers->contains(a_batch))
			return false;
		if (reflectionTreeLodOwned.load(std::memory_order_acquire) && TreeLod::IsTreeLodShape(a_pass->geometry)) {
			reflectionTreeLodWithheld.fetch_add(1, std::memory_order_relaxed);
			return true;
		}
		const auto set = std::atomic_load(&frameSet);
		if (!set || !(set->drawn & kSetReflection) || !(set->PhasesOf(a_pass->geometry) & kSetReflection))
			return false;
		reflectionWithheld.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	bool PassCapture::ShadowWithholdingEnabled()
	{
		return ActiveToggles().shadows && !ParityBoth();
	}

	void PassCapture::SetShadowBatchRenderers(std::shared_ptr<const ShadowRendererMap> a_renderers)
	{
		std::atomic_store(&shadowRenderers, std::move(a_renderers));
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

	bool PassCapture::Withhold(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, bool a_fading)
	{
		if (!a_pass || !a_pass->geometry)
			return false;
		// A shadow view's renderers: the set's casters. A pass into any other renderer (reflections, cubemaps, the focus shadows)
		// is never withheld; the main camera's are WithholdMain's.
		(void)a_fading;
		std::uint32_t mode = 0;
		std::uint8_t phase = 0;
		if (const auto set = CastersForBatch(a_batch, &phase); set && ShadowModeOfBatch(a_batch, mode) && (set->PhasesOf(a_pass->geometry) & phase)) {
			shadowWithheld[mode].fetch_add(1, std::memory_order_relaxed);
			return true;
		}
		return false;
	}

	namespace
	{
		thread_local std::uint32_t passesOnThisThread = 0;
	}

	std::uint32_t PassCapture::PassesOnThisThread()
	{
		return passesOnThisThread;
	}

	struct PassCapture::Hook
	{
		static void thunk(RE::BSBatchRenderer* a_this, RE::BSRenderPass* a_pass, std::uint32_t a_techniqueID)
		{
			DCLF_FRAME_TRACE("PassCapture.cpp:293");  // TEMP frame trace
			++passesOnThisThread;
			auto& capture = PassCapture::Get();
			if (capture.bypassed.load(std::memory_order_acquire)) {
				func(a_this, a_pass, a_techniqueID);
				return;
			}
			// Per-frame state is read once, here, and both decisions below use it.
			const bool fading = FadingAtRegistration(a_pass);
			// Withholding is the whole of ownership: the pass is built, lit and shadowed exactly as before - only the batch
			// renderer never receives it, so the native loop has nothing to draw. Everything the tables need is taken by Record.
			const bool withheld = capture.WithholdMain(a_this, a_pass) || capture.WithholdReflection(a_this, a_pass) || capture.Withhold(a_this, a_pass, fading);
			capture.Record(a_this, a_pass, a_techniqueID, fading, withheld);
			LocalLightCull::NoteRegistration(a_this, a_pass, withheld);
			if (std::uint32_t mode = 0; !withheld && capture.ShadowModeOfBatch(a_this, mode))
				CensusNativePass(a_pass, a_techniqueID, mode, false);
			if (!withheld)
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
			DCLF_FRAME_TRACE("PassCapture.cpp:336");  // TEMP frame trace
			++passesOnThisThread;
			auto& capture = PassCapture::Get();
			const bool withheld = (!capture.bypassed.load(std::memory_order_acquire) && capture.WithholdMain(a_batch, a_pass)) ||
			                      capture.WithholdAtGroup(a_batch, a_pass, capture.volumetricWithheld);
			LocalLightCull::NoteRegistration(a_batch, a_pass, withheld, 8);
			if (withheld)
				return;
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
			DCLF_FRAME_TRACE("PassCapture.cpp:359");  // TEMP frame trace
			++passesOnThisThread;
			auto& capture = PassCapture::Get();
			const bool withheld = (!capture.bypassed.load(std::memory_order_acquire) && capture.WithholdMain(a_batch, a_pass)) ||
			                      capture.WithholdAtGroup(a_batch, a_pass, capture.directWithheld);
			LocalLightCull::NoteRegistration(a_batch, a_pass, withheld, Hint);
			if (withheld)
				return;
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
			DCLF_FRAME_TRACE("PassCapture.cpp:383");  // TEMP frame trace
			++passesOnThisThread;
			auto& capture = PassCapture::Get();
			if (!capture.bypassed.load(std::memory_order_acquire) && (capture.WithholdMain(a_batch, a_pass) || capture.WithholdReflection(a_batch, a_pass)))
				return;
			func(a_batch, a_pass, a_group, a_arg);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/** @brief The same registration's hint 1, into a pass list of the batch renderer (FUN_1414f50b0(batch, pass, list)). */
	struct PassCapture::MainListHook
	{
		static void thunk(RE::BSBatchRenderer* a_batch, RE::BSRenderPass* a_pass, void* a_list)
		{
			DCLF_FRAME_TRACE("PassCapture.cpp:397");  // TEMP frame trace
			++passesOnThisThread;
			auto& capture = PassCapture::Get();
			if (!capture.bypassed.load(std::memory_order_acquire) && (capture.WithholdMain(a_batch, a_pass) || capture.WithholdReflection(a_batch, a_pass)))
				return;
			func(a_batch, a_pass, a_list);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/**
	 * @brief The occlusion maps' registration (FUN_1414b2c20, render mode 0x1C: Precipitation::SetupMask's accumulator) inserting a
	 * pass into geometry group 14 (FUN_1414f5090(batch, pass, 14), its one insertion): a member of the map's occluder phase is
	 * withheld, DCLF drawing it into the map (DrawcallLimitFix::DrawOcclusion); the engine draws every other occluder.
	 */
	struct PassCapture::OcclusionGroupHook
	{
		static void thunk(RE::BSBatchRenderer* a_batch, RE::BSRenderPass* a_pass, std::uint32_t a_group, std::uint32_t a_arg)
		{
			DCLF_FRAME_TRACE("PassCapture.cpp:415");  // TEMP frame trace
			++passesOnThisThread;
			auto& capture = PassCapture::Get();
			const std::uint8_t phase = capture.occlusionPhase.load(std::memory_order_acquire);
			if (phase && a_pass && a_pass->geometry && !ParityBoth() && !capture.bypassed.load(std::memory_order_acquire))
				if (const auto set = std::atomic_load(&capture.frameSet); set && (set->drawn & phase) && (set->PhasesOf(a_pass->geometry) & phase)) {
					capture.occlusionWithheld.fetch_add(1, std::memory_order_relaxed);
					return;
				}
			func(a_batch, a_pass, a_group, a_arg);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	namespace
	{
		template <std::uint32_t Group, class Hook>
		bool InstallMainGroup(std::uintptr_t a_site, std::uintptr_t a_callee)
		{
			if (!Engine::CallsTo(REL::Offset(a_site).address(), REL::Offset(a_callee).address()))
				return false;
			stl::write_thunk_call<Hook>(REL::Offset(a_site).address());
			return true;
		}
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
		// The main modes' registration's (FUN_1414b2330) group insertions, by the group each inserts into, and its hint 1 list
		// insertion. Each call site is checked against its callee before it is patched.
		constexpr std::uintptr_t kInsertGroup = 0x14f5090, kInsertList = 0x14f50b0;
		const bool all = InstallMainGroup<3, MainGroupHook<3>>(0x14b24a2, kInsertGroup) & InstallMainGroup<4, MainGroupHook<4>>(0x14b24bd, kInsertGroup) &
		                 InstallMainGroup<5, MainGroupHook<5>>(0x14b24d8, kInsertGroup) & InstallMainGroup<6, MainGroupHook<6>>(0x14b24f3, kInsertGroup) &
		                 InstallMainGroup<0, MainGroupHook<0>>(0x14b250d, kInsertGroup) & InstallMainGroup<1, MainGroupHook<1>>(0x14b2528, kInsertGroup) &
		                 InstallMainGroup<7, MainGroupHook<7>>(0x14b2543, kInsertGroup) & InstallMainGroup<9, MainGroupHook<9>>(0x14b255e, kInsertGroup) &
		                 InstallMainGroup<10, MainGroupHook<10>>(0x14b2579, kInsertGroup) & InstallMainGroup<11, MainGroupHook<11>>(0x14b2591, kInsertGroup) &
		                 InstallMainGroup<8, MainGroupHook<8>>(0x14b25a9, kInsertGroup) & InstallMainGroup<12, MainGroupHook<12>>(0x14b25c1, kInsertGroup) &
		                 InstallMainGroup<13, MainGroupHook<13>>(0x14b25db, kInsertGroup) & InstallMainGroup<2, MainGroupHook<2>>(0x14b2667, kInsertGroup) &
		                 InstallMainGroup<1, MainListHook>(0x14b2478, kInsertList) & InstallMainGroup<14, OcclusionGroupHook>(0x14b2c71, kInsertGroup);
		if (!all)
			stl::report_and_fail("Drawcall Limit Fix: the main registration's group insertions are not where expected (AE 1.6.1170 only)");
		installed = true;
		logger::info("[DCLF] pass capture installed on BSBatchRenderer::RegisterPass");
	}
}
