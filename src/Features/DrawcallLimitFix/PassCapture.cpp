#include "PassCapture.h"

#include "Switches.h"
#include "Toggles.h"
#include "VolumetricProbe.h"

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

	PassCapture::PassCapture()
	{
		if (SwitchEnabled("CS_DCLF_SHADOW_PROBE"))
			utilityEntries.resize(kCapacity);
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
		if (fade.currentFade < 1.0f && (!FadingEnabled() || hint == 9 || hint == 2 || hint == 3))
			return true;
		// A LOD cross-fade (kMeshLOD, fade node LOD state +0x153 & 0x70 not 0x20) is not a fade of the object:
		// GetRenderPasses (AE 1414adfb0) keeps its pass as it is - the new level, drawn as any settled object
		// is - and adds the old level as a hint-10 copy, which is the native loop's (above). DCLF keeps the
		// object through the crossing and the native loop draws only the copy (CS_DCLF_LOD_CROSSFADE); off,
		// the whole object is the native loop's until the crossing ends.
		return !LodCrossfadeEnabled() && geometry->GetFlags().any(RE::NiAVObject::Flag::kMeshLOD) && (fade.unk153 & 0x70) != 0x20;
	}

	void PassCapture::Record(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_fading, bool a_withheld)
	{
		// CS_DCLF_REGISTER_PROBE=1: what else comes through RegisterPass, so that owning the depth pass
		// can be planned against evidence rather than against the shape of GetRenderDepthPass. Reports the
		// shader type and whether the batch renderer is one of the main camera's.
		static const bool probe = SwitchEnabled("CS_DCLF_REGISTER_PROBE");
		if (probe && a_pass && a_pass->shader) {
			const auto type = static_cast<std::uint32_t>(a_pass->shader->shaderType.get());
			if (type < probeCounts.size()) {
				probeCounts[type].fetch_add(1, std::memory_order_relaxed);
				const auto renderers = std::atomic_load(&mainRenderers);
				if (renderers && renderers->contains(a_batch))
					probeMain[type].fetch_add(1, std::memory_order_relaxed);
			}
		}
		if (!a_pass || !a_pass->geometry || !a_pass->shader)
			return;
		if (a_pass->shader->shaderType.get() == RE::BSShader::Type::Utility) {
			// The shadow probe's ring: allocated on first use under the switch, never otherwise.
			static const bool shadowProbe = SwitchEnabled("CS_DCLF_SHADOW_PROBE");
			if (!shadowProbe)
				return;
			const auto slot = utilityCursor.fetch_add(1, std::memory_order_relaxed);
			if (slot >= kCapacity) {
				utilityOverflow.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			utilityEntries[slot] = Entry{ a_pass->geometry, a_pass, a_batch, a_technique, 0, a_pass->passEnum };
			return;
		}
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
		stats.withheld = withheld.exchange(0, std::memory_order_relaxed);
		for (std::uint32_t m = 0; m < kShadowModes; ++m)
			stats.shadowWithheld[m] = shadowWithheld[m].exchange(0, std::memory_order_relaxed);
		stats.volumetricWithheld = volumetricWithheld.exchange(0, std::memory_order_relaxed);
		stats.directWithheld = directWithheld.exchange(0, std::memory_order_relaxed);
		lastDrain = { entries.data(), count };
		handedBack.clear();
		withheldThisFrame.clear();
		withheldBuilt = false;
		stats.handedBack = 0;
		return lastDrain;
	}

	std::span<const PassCapture::Entry> PassCapture::DrainUtility()
	{
		const auto count = std::min(utilityCursor.exchange(0, std::memory_order_relaxed), kCapacity);
		if (const auto overflowed = utilityOverflow.exchange(0, std::memory_order_relaxed))
			logger::warn("[DCLF] shadow probe: {} Utility registrations overflowed the capture ring", overflowed);
		return { utilityEntries.data(), count };
	}

	bool PassCapture::WithholdingEnabled()
	{
		return Toggles::Get().Active().ownership;
	}

	void PassCapture::PublishClaims(std::shared_ptr<const ClaimSet> a_claims)
	{
		std::atomic_store(&claims, std::move(a_claims));
	}

	void PassCapture::SetMainBatchRenderers(std::shared_ptr<const ankerl::unordered_dense::set<const RE::BSBatchRenderer*>> a_renderers)
	{
		std::atomic_store(&mainRenderers, std::move(a_renderers));
	}

	bool PassCapture::ShadowWithholdingEnabled()
	{
		const auto toggles = Toggles::Get().Active();
		return toggles.shadows && toggles.shadowOwnership;
	}

	bool PassCapture::CascadeProbeEnabled()
	{
		static const bool enabled = SwitchEnabled("CS_DCLF_CASCADE_PROBE");
		return enabled;
	}

	std::vector<PassCapture::ShadowRegistration> PassCapture::TakeShadowRegistrations()
	{
		std::lock_guard lock(shadowRegistrationsLock);
		return std::exchange(shadowRegistrations, {});
	}

	void PassCapture::SetShadowBatchRenderers(std::shared_ptr<const ShadowRendererMap> a_renderers)
	{
		std::atomic_store(&shadowRenderers, std::move(a_renderers));
	}

	void PassCapture::PublishShadowClaims(std::uint32_t a_modeIndex, std::shared_ptr<const ClaimSet> a_claims)
	{
		if (a_modeIndex < kShadowModes)
			std::atomic_store(&shadowClaims[a_modeIndex], std::move(a_claims));
	}

	std::shared_ptr<const PassCapture::ClaimSet> PassCapture::ShadowClaimsForBatch(const RE::BSBatchRenderer* a_batch) const
	{
		if (!ShadowWithholdingEnabled())
			return nullptr;
		const auto renderers = std::atomic_load(&shadowRenderers);
		if (!renderers)
			return nullptr;
		const auto it = renderers->find(a_batch);
		if (it == renderers->end() || it->second >= kShadowModes)
			return nullptr;
		return std::atomic_load(&shadowClaims[it->second]);
	}

	bool PassCapture::Withhold(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, bool a_fading)
	{
		if (!a_pass || !a_pass->geometry)
			return false;
		const auto toggles = Toggles::Get().Active();
		// The main camera's renderers: the colour epoch's claims. A shadow camera's renderers are a
		// separate set with claims of their own, below; a pass into any other renderer (reflections,
		// cubemaps, the focus shadows until S4) is never withheld.
		if (toggles.ownership) {
			const auto renderers = std::atomic_load(&mainRenderers);
			if (renderers && renderers->contains(a_batch)) {
				const auto owned = std::atomic_load(&claims);
				// A fading object is not DCLF's this frame (the accumulate phase gives it no bindings, from this
				// same value): withholding it would leave it drawn by nobody.
				if (owned && owned->contains(a_pass->geometry) && !a_fading) {
					withheld.fetch_add(1, std::memory_order_relaxed);
					return true;
				}
				return false;
			}
		}
		if (toggles.shadows && toggles.shadowOwnership) {
			const auto renderers = std::atomic_load(&shadowRenderers);
			if (!renderers)
				return false;
			const auto it = renderers->find(a_batch);
			if (it == renderers->end() || it->second >= kShadowModes)
				return false;
			const auto owned = std::atomic_load(&shadowClaims[it->second]);
			const bool claimed = owned && owned->contains(a_pass->geometry);
			if (CascadeProbeEnabled()) {
				std::lock_guard lock(shadowRegistrationsLock);
				if (shadowRegistrations.size() < 65536)
					shadowRegistrations.push_back({ a_batch, a_pass->geometry, claimed });
			}
			if (claimed) {
				shadowWithheld[it->second].fetch_add(1, std::memory_order_relaxed);
				return true;
			}
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
			++passesOnThisThread;
			auto& capture = PassCapture::Get();
			// [TEMP] CS_DCLF_VOLUMETRIC_PROBE: every Utility registration, and what the engine's took.
			const bool probe = VolumetricProbe::Enabled() && a_pass && a_pass->shader && a_pass->shader->shaderType.get() == RE::BSShader::Type::Utility;
			auto engine = [&] {
				if (!probe) {
					func(a_this, a_pass, a_techniqueID);
					return;
				}
				LARGE_INTEGER start{}, end{};
				QueryPerformanceCounter(&start);
				func(a_this, a_pass, a_techniqueID);
				QueryPerformanceCounter(&end);
				VolumetricProbe::Get().OnRegister(a_this, a_pass, end.QuadPart - start.QuadPart);
			};
			if (capture.bypassed.load(std::memory_order_acquire)) {
				engine();
				return;
			}
			// Per-frame state is read once, here, and both decisions below use it.
			const bool fading = FadingAtRegistration(a_pass);
			// Withholding is the whole of static ownership: the pass is built, lit and shadowed exactly as
			// before - only the batch renderer never receives it, so the native loop has nothing to draw
			// and DCLF owns the object outright. Everything the tables need is taken by Record.
			const bool withheld = capture.Withhold(a_this, a_pass, fading);
			capture.Record(a_this, a_pass, a_techniqueID, fading, withheld);
			if (withheld) {
				if (probe)
					VolumetricProbe::Get().OnRegister(a_this, a_pass, 0);
				return;
			}
			engine();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	std::uint32_t PassCapture::HandBackUndrawable(const std::function<bool(const RE::BSGeometry*)>& a_drawable)
	{
		std::uint32_t count = 0;
		for (const auto& entry : lastDrain) {
			if (!entry.withheld || !entry.geometry || !entry.batch || !entry.pass || handedBack.contains(entry.geometry) || a_drawable(entry.geometry))
				continue;
			Hook::func(const_cast<RE::BSBatchRenderer*>(entry.batch), const_cast<RE::BSRenderPass*>(entry.pass), entry.technique);
			handedBack.insert(entry.geometry);
			++count;
		}
		stats.handedBack = count;
		return count;
	}

	bool PassCapture::WithheldThisFrame(const RE::BSGeometry* a_geometry)
	{
		if (!withheldBuilt) {
			for (const auto& entry : lastDrain)
				if (entry.withheld && entry.geometry)
					withheldThisFrame.insert(entry.geometry);
			withheldBuilt = true;
		}
		return withheldThisFrame.contains(a_geometry) && !handedBack.contains(a_geometry);
	}

	namespace
	{
		bool volumetricHookInstalled = false;
	}

	bool PassCapture::VolumetricClaimsAvailable()
	{
		return volumetricHookInstalled;
	}

	/**
	 * @brief The shadow modes' registration inserting an accumulation hint 8 pass (a volumetric-only caster)
	 * into batch group 15: AE FUN_1414b2a60's direct call to FUN_1414f5090(batch, pass, 15, 0) at +0xFF. The
	 * pass is withheld when the batch renderer is a shadow view's and DCLF's epoch drew the geometry under that
	 * view's render mode; the claim set is the mode's one, which holds these casters only while DCLF draws the
	 * copy's views (the inputs of a mode carry them only then).
	 */
	bool PassCapture::WithholdAtGroup(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, std::atomic<std::uint32_t>& a_counter)
	{
		if (!a_pass || !a_pass->geometry || bypassed.load(std::memory_order_acquire) || !ShadowWithholdingEnabled())
			return false;
		const auto renderers = std::atomic_load(&shadowRenderers);
		if (!renderers)
			return false;
		const auto it = renderers->find(a_batch);
		if (it == renderers->end() || it->second >= kShadowModes)
			return false;
		const auto owned = std::atomic_load(&shadowClaims[it->second]);
		const bool claimed = owned && owned->contains(a_pass->geometry);
		// [TEMP] CS_DCLF_CASCADE_PROBE: these registrations belong to the view's caster set too, so the probe can
		// compare them with DCLF's.
		if (CascadeProbeEnabled()) {
			std::lock_guard lock(shadowRegistrationsLock);
			if (shadowRegistrations.size() < 65536)
				shadowRegistrations.push_back({ a_batch, a_pass->geometry, claimed });
		}
		if (claimed)
			a_counter.fetch_add(1, std::memory_order_relaxed);
		return claimed;
	}

	struct PassCapture::VolumetricGroupHook
	{
		static void thunk(RE::BSBatchRenderer* a_batch, RE::BSRenderPass* a_pass, std::uint32_t a_group, std::uint32_t a_arg)
		{
			++passesOnThisThread;
			auto& capture = PassCapture::Get();
			if (capture.WithholdAtGroup(a_batch, a_pass, capture.volumetricWithheld))
				return;
			func(a_batch, a_pass, a_group, a_arg);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/**
	 * @brief The same registration's other direct insertions (FUN_1414b2a60): accumulation hint 11 into batch
	 * group 9 (+0xC9), hint 7 into group 1 (+0xDB) and hint 3, a decal-flagged caster such as an NPC's face
	 * part, into group 4 (+0xED). None of them reaches RegisterPass, so a claimed caster registered with one of
	 * these hints was drawn by both DCLF and the engine until they were withheld here as well.
	 */
	template <std::uint32_t Hint>
	struct PassCapture::DirectGroupHook
	{
		static void thunk(RE::BSBatchRenderer* a_batch, RE::BSRenderPass* a_pass, std::uint32_t a_group, std::uint32_t a_arg)
		{
			++passesOnThisThread;
			auto& capture = PassCapture::Get();
			if (capture.WithholdAtGroup(a_batch, a_pass, capture.directWithheld))
				return;
			func(a_batch, a_pass, a_group, a_arg);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	void PassCapture::Install()
	{
		if (installed)
			return;
		stl::write_vfunc<0x2, Hook>(RE::VTABLE_BSBatchRenderer[0]);
		// AE only: the SE and VR offsets of the call are unverified (no database for them).
		if (REL::Module::IsAE()) {
			stl::write_thunk_call<VolumetricGroupHook>(REL::Offset(0x14b2b5f).address());
			stl::write_thunk_call<DirectGroupHook<11>>(REL::Offset(0x14b2b29).address());
			stl::write_thunk_call<DirectGroupHook<7>>(REL::Offset(0x14b2b3b).address());
			stl::write_thunk_call<DirectGroupHook<3>>(REL::Offset(0x14b2b4d).address());
			volumetricHookInstalled = true;
		}
		installed = true;
		logger::info("[DCLF] pass capture installed on BSBatchRenderer::RegisterPass");
	}
}
