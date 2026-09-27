#include "VolumetricProbe.h"

#include "LightingDescriptors.h"
#include "SceneStore.h"
#include "ShadowViews.h"
#include "Switches.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include <ankerl/unordered_dense.h>

namespace DCLF
{
	namespace
	{
		constexpr std::uint32_t kInterval = 240;
		// Targets as OnFinish is given them: the depth-stencil index the renderer's shadow state names.
		constexpr std::uint32_t kCascades = RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM;
		constexpr std::uint32_t kVolumetric = RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM;

		const char* RttiOf(const RE::NiObject* a_object)
		{
			const auto* rtti = a_object ? a_object->GetRTTI() : nullptr;
			return rtti && rtti->GetName() ? rtti->GetName() : "?";
		}

		double Milliseconds(std::int64_t a_ticks)
		{
			static const double frequency = [] {
				LARGE_INTEGER f{};
				QueryPerformanceFrequency(&f);
				return static_cast<double>(f.QuadPart);
			}();
			return static_cast<double>(a_ticks) * 1000.0 / frequency;
		}
	}

	struct VolumetricProbe::Impl
	{
		struct Registration
		{
			const RE::BSBatchRenderer* batch;
			const RE::BSGeometry* geometry;
			std::int64_t ticks;
			std::uint8_t hint;
			std::uint8_t reject;
			bool castShadows;
		};
		struct Finish
		{
			const RE::BSBatchRenderer* batch;
			std::uint32_t target;
			std::uint32_t flags;
			std::int64_t ticks;
		};

		struct Draw
		{
			std::uint32_t target;
			const RE::BSGeometry* geometry;
			std::uint32_t passEnum;
			std::uint8_t hint;
			std::uint8_t reject;
			bool castShadows;
		};
		std::int64_t sunRenderTicks = 0, sunRenders = 0;  // BSShadowDirectionalLight::Render, whole
		std::int64_t sunFinishTicks = 0;
		std::int64_t sunAccumulateTicks = 0, sunFullFrustumTicks = 0;  // Accumulate (cascade culls + registration), the full-frustum cull                  // the FinishAccumulating calls inside it
		bool inSun = false;
		std::uint32_t currentTarget = ~0u;  // render thread, inside a shadow view's FinishAccumulating
		std::vector<Draw> draws;
		std::uint64_t volumetricDraws = 0, cascadeDraws = 0, otherDraws = 0, volumetricDrawGeometries = 0;
		std::uint64_t volumetricDrawCastShadows = 0, volumetricDrawsAlsoCascade = 0;
		ankerl::unordered_dense::map<std::uint32_t, std::uint64_t> volumetricDrawHints;
		ankerl::unordered_dense::map<std::string, std::uint64_t> drawCategories;
		ankerl::unordered_dense::map<std::string, std::uint64_t> drawRejects;
		ankerl::unordered_dense::map<std::string, std::uint64_t> cascadeResidue;
		ankerl::unordered_dense::map<std::string, std::uint32_t> residueSamples;
		ankerl::unordered_dense::map<std::string, std::uint64_t> sunOtherViews;  // views inside the sun's Render besides the cascades and the copy
		ankerl::unordered_dense::map<std::string, std::uint64_t> sunOtherDraws;  // the engine's cascade draws, by DCLF's rule and class
		ankerl::unordered_dense::map<std::string, std::uint32_t> drawSamples;

		std::mutex lock;  // registrations may come from job threads; diagnostics only
		std::vector<Registration> registrations;
		std::vector<Finish> finishes;  // render thread

		// Totals over the interval.
		std::uint32_t frames = 0, runningFrames = 0;
		std::uint64_t volumetricViews = 0, cascadeViews = 0, otherViews = 0;
		std::uint64_t hint8Passes = 0, hint8Geometries = 0, sunOtherPasses = 0, sunOtherGeometries = 0;
		std::uint64_t hint8CastShadows = 0, hint8AlsoCascade = 0, hint8Elsewhere = 0;
		std::int64_t hint8RegisterTicks = 0, sunOtherRegisterTicks = 0;
		std::int64_t volumetricFinishTicks = 0, cascadeFinishTicks = 0, otherFinishTicks = 0;
		std::uint32_t maxHint8PerFrame = 0;
		ankerl::unordered_dense::map<std::uint32_t, std::uint64_t> sunHints;       // every Utility pass in the sun's renderers
		ankerl::unordered_dense::map<std::uint32_t, std::uint64_t> volumetricFlags;  // FinishAccumulating flags of the copy's views
		ankerl::unordered_dense::map<std::string, std::uint64_t> categories;       // hint-8 geometries per frame
		ankerl::unordered_dense::map<std::string, std::uint64_t> rejects;          // DCLF's rule at registration
		ankerl::unordered_dense::map<std::string, std::uint32_t> samples;
	};

	bool VolumetricProbe::Enabled()
	{
		static const bool enabled = SwitchEnabled("CS_DCLF_VOLUMETRIC_PROBE");
		return enabled;
	}

	VolumetricProbe& VolumetricProbe::Get()
	{
		static VolumetricProbe probe;
		return probe;
	}

	VolumetricProbe::VolumetricProbe() :
		impl(std::make_unique<Impl>()) {}

	VolumetricProbe::~VolumetricProbe() = default;

	void VolumetricProbe::OnRegister(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, std::int64_t a_ticks)
	{
		if (!a_pass || !a_pass->geometry)
			return;
		auto* property = a_pass->geometry->GetGeometryRuntimeData().shaderProperty.get();
		// The caster rule reads the shadow global, which is the sun's only while its passes are registered:
		// evaluated here, not at the end of the frame.
		const auto reject = static_cast<std::uint8_t>(ShadowCasterReject(property, a_pass->geometry));
		const bool castShadows = property && (property->flags.underlying() & (std::uint64_t(1) << 9));
		std::lock_guard guard(impl->lock);
		if (impl->registrations.size() < 262144)
			impl->registrations.push_back({ a_batch, a_pass->geometry, a_ticks, static_cast<std::uint8_t>(a_pass->accumulationHint), reject, castShadows });
	}

	struct VolumetricProbe::UtilitySetupGeometry
	{
		static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, std::uint32_t a_flags)
		{
			auto& i = *VolumetricProbe::Get().impl;
			if (i.currentTarget != ~0u && a_pass && a_pass->geometry && i.draws.size() < 262144) {
				auto* property = a_pass->geometry->GetGeometryRuntimeData().shaderProperty.get();
				i.draws.push_back({ i.currentTarget | (i.inSun ? 0x80000000u : 0u), a_pass->geometry, a_pass->passEnum, static_cast<std::uint8_t>(a_pass->accumulationHint),
					static_cast<std::uint8_t>(ShadowCasterReject(property, a_pass->geometry)), property && (property->flags.underlying() & (std::uint64_t(1) << 9)) });
			}
			func(a_shader, a_pass, a_flags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct VolumetricProbe::SunRender
	{
		static void thunk(RE::BSShadowLight* a_light, std::uint32_t& a_index)
		{
			LARGE_INTEGER start{}, end{};
			VolumetricProbe::Get().impl->inSun = true;
			QueryPerformanceCounter(&start);
			func(a_light, a_index);
			QueryPerformanceCounter(&end);
			VolumetricProbe::Get().impl->inSun = false;
			VolumetricProbe::Get().OnSunRender(end.QuadPart - start.QuadPart);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// BSShadowDirectionalLight::Accumulate (vfunc 9): per cascade, FUN_1414f0920 -> FUN_1414bf320, the cull of
	// the full-frustum entries and the registration of their passes (the volumetric copy's among them).
	struct VolumetricProbe::SunAccumulate
	{
		static void thunk(RE::BSShadowLight* a_light, std::uint32_t& a_count, std::uint32_t a_channel, RE::NiAVObject* a_scene, std::uint8_t a_vr)
		{
			LARGE_INTEGER start{}, end{};
			QueryPerformanceCounter(&start);
			func(a_light, a_count, a_channel, a_scene, a_vr);
			QueryPerformanceCounter(&end);
			VolumetricProbe::Get().impl->sunAccumulateTicks += end.QuadPart - start.QuadPart;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// AE FUN_141511f30, called from NiCamera::CalculateAndDrawShadowCasterLights: the scene cull that fills the
	// full-frustum culling processes' object arrays the cascade culls walk.
	struct VolumetricProbe::SunFullFrustum
	{
		static void thunk(RE::BSShadowLight* a_light, void* a_arg1, void* a_arg2)
		{
			LARGE_INTEGER start{}, end{};
			QueryPerformanceCounter(&start);
			func(a_light, a_arg1, a_arg2);
			QueryPerformanceCounter(&end);
			VolumetricProbe::Get().impl->sunFullFrustumTicks += end.QuadPart - start.QuadPart;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	void VolumetricProbe::OnSunRender(std::int64_t a_ticks)
	{
		impl->sunRenderTicks += a_ticks;
		++impl->sunRenders;
	}

	void VolumetricProbe::Install()
	{
		static bool installed = false;
		if (installed || !Enabled())
			return;
		installed = true;
		stl::write_vfunc<0x6, UtilitySetupGeometry>(RE::VTABLE_BSUtilityShader[0]);
		stl::write_vfunc<0xA, SunRender>(RE::VTABLE_BSShadowDirectionalLight[0]);
		stl::write_vfunc<0x9, SunAccumulate>(RE::VTABLE_BSShadowDirectionalLight[0]);
		if (REL::Module::IsAE())
			stl::write_thunk_call<SunFullFrustum>(REL::Offset(0x14cbcb6).address());
		logger::info("[DCLF] volumetric probe installed (BSUtilityShader::SetupGeometry)");
	}

	void VolumetricProbe::BeginFinish(std::uint32_t a_target)
	{
		impl->currentTarget = a_target;
	}

	void VolumetricProbe::OnFinish(const RE::BSGraphics::BSShaderAccumulator* a_accumulator, std::uint32_t a_renderFlags, std::uint32_t a_target, std::int64_t a_ticks)
	{
		impl->currentTarget = ~0u;
		if (impl->inSun && a_target != kCascades && a_target != kVolumetric) {
			const auto& shadow = globals::game::shadowState->GetRuntimeData();
			++impl->sunOtherViews[fmt::format("target {} slice {} mode {:#x} flags {:#x} viewport {}x{}", a_target, shadow.depthStencilSlice,
				static_cast<std::uint32_t>(a_accumulator->GetRuntimeData().renderMode), a_renderFlags, static_cast<std::uint32_t>(shadow.viewPort.Width), static_cast<std::uint32_t>(shadow.viewPort.Height))];
		}
		if (impl->inSun)
			impl->sunFinishTicks += a_ticks;
		impl->finishes.push_back({ a_accumulator->GetRuntimeData().batchRenderer, a_target, a_renderFlags, a_ticks });
	}

	void VolumetricProbe::EndFrame(bool a_running)
	{
		auto& i = *impl;
		std::vector<Impl::Registration> registrations;
		{
			std::lock_guard guard(i.lock);
			registrations.swap(i.registrations);
		}
		std::vector<Impl::Finish> finishes;
		finishes.swap(i.finishes);

		// The sun's renderers: those whose accumulator was finished into the volumetric copy this frame.
		ankerl::unordered_dense::set<const RE::BSBatchRenderer*> sun;
		for (const auto& finish : finishes) {
			if (finish.target == kVolumetric) {
				sun.insert(finish.batch);
				++i.volumetricViews;
				i.volumetricFinishTicks += finish.ticks;
				++i.volumetricFlags[finish.flags];
			} else if (finish.target == kCascades) {
				++i.cascadeViews;
				i.cascadeFinishTicks += finish.ticks;
			} else {
				++i.otherViews;
				i.otherFinishTicks += finish.ticks;
			}
		}

		ankerl::unordered_dense::set<const RE::BSGeometry*> hint8, other;
		std::uint32_t hint8Frame = 0;
		for (const auto& r : registrations) {
			if (!sun.contains(r.batch)) {
				if (r.hint == 8)
					++i.hint8Elsewhere;
				continue;
			}
			++i.sunHints[r.hint];
			if (r.hint != 8) {
				++i.sunOtherPasses;
				i.sunOtherRegisterTicks += r.ticks;
				other.insert(r.geometry);
				continue;
			}
			++hint8Frame;
			i.hint8RegisterTicks += r.ticks;
			if (!hint8.insert(r.geometry).second)
				continue;
			if (r.castShadows)
				++i.hint8CastShadows;
			++i.rejects[ShadowRejectName(static_cast<ShadowReject>(r.reject))];

			// What it is: the leaf's and property's classes, and the base form of the reference that owns it.
			const auto* geometry = r.geometry;
			auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
			std::string form = "no-ref";
			for (const RE::NiAVObject* object = geometry; object; object = object->parent) {
				if (auto* ref = object->GetUserData()) {
					const auto* base = ref->GetBaseObject();
					form = base ? std::string(RE::FormTypeToString(base->GetFormType())) : "ref";
					break;
				}
			}
			const bool skinned = geometry->GetGeometryRuntimeData().skinInstance.get() != nullptr;
			++i.categories[fmt::format("{} | {} | {}{}", form, RttiOf(geometry), RttiOf(property), skinned ? " | skinned" : "")];
			if (i.samples.size() < 4096)
				++i.samples[fmt::format("{} ({})", geometry->name.c_str() ? geometry->name.c_str() : "?", form)];
		}
		for (const auto* geometry : hint8)
			if (other.contains(geometry))
				++i.hint8AlsoCascade;

		// What the engine actually drew, per view target.
		std::vector<Impl::Draw> draws;
		draws.swap(i.draws);
		// Why a cascade caster the engine drew is not DCLF's: the scene store's verdict, read here on the render
		// thread once the walk has joined (Prepass), with the form behind it.
		const auto& store = SceneStore::Get();
		const bool storeReadable = a_running && !store.ScenePending();
		auto whyNative = [&](const RE::BSGeometry* a_geometry) -> std::string {
			if (!storeReadable)
				return "store busy";
			if (store.FindObject(a_geometry) >= 0)
				return "in tables";
			if (!store.IsTracked(a_geometry))
				return "untracked";
			bool accumulate = false;
			const auto reason = store.ReasonThisFrame(a_geometry, &accumulate);
			return fmt::format("{}{}", kIneligibleNames[static_cast<std::size_t>(reason)], accumulate ? " (accumulate)" : "");
		};
		auto formOf = [](const RE::BSGeometry* a_geometry) -> std::string {
			for (const RE::NiAVObject* object = a_geometry; object; object = object->parent) {
				if (auto* ref = object->GetUserData()) {
					const auto* base = ref->GetBaseObject();
					return base ? std::string(RE::FormTypeToString(base->GetFormType())) : "ref";
				}
			}
			return "no-ref";
		};
		ankerl::unordered_dense::set<const RE::BSGeometry*> cascadeDrawn, volumetricDrawn;
		for (auto& d : draws) {
			const bool inSun = (d.target & 0x80000000u) != 0;
			d.target &= ~0x80000000u;
			if (inSun && d.target != kCascades && d.target != kVolumetric)
				++i.sunOtherDraws[fmt::format("{} | {} | {}", whyNative(d.geometry), formOf(d.geometry), RttiOf(d.geometry))];
		}
		for (const auto& d : draws)
			if (d.target == kCascades)
				cascadeDrawn.insert(d.geometry);
		for (const auto& d : draws) {
			if (d.target == kCascades) {
				++i.cascadeDraws;
				auto* property = d.geometry->GetGeometryRuntimeData().shaderProperty.get();
				const auto key = fmt::format("{} | {} | {} | {} | {} | hint {}", whyNative(d.geometry), ShadowRejectName(static_cast<ShadowReject>(d.reject)), formOf(d.geometry),
					RttiOf(d.geometry), RttiOf(property), d.hint);
				++i.cascadeResidue[key];
				if (i.residueSamples.size() < 4096)
					++i.residueSamples[fmt::format("{} ({})", d.geometry->name.c_str() ? d.geometry->name.c_str() : "?", key.substr(0, key.find(" |")))];
				continue;
			}
			if (d.target != kVolumetric) {
				++i.otherDraws;
				continue;
			}
			++i.volumetricDraws;
			++i.volumetricDrawHints[d.hint];
			if (!volumetricDrawn.insert(d.geometry).second)
				continue;
			if (d.castShadows)
				++i.volumetricDrawCastShadows;
			if (cascadeDrawn.contains(d.geometry))
				++i.volumetricDrawsAlsoCascade;
			++i.drawRejects[ShadowRejectName(static_cast<ShadowReject>(d.reject))];
			const auto* geometry = d.geometry;
			auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
			std::string form = "no-ref";
			for (const RE::NiAVObject* object = geometry; object; object = object->parent) {
				if (auto* ref = object->GetUserData()) {
					const auto* base = ref->GetBaseObject();
					form = base ? std::string(RE::FormTypeToString(base->GetFormType())) : "ref";
					break;
				}
			}
			const bool skinned = geometry->GetGeometryRuntimeData().skinInstance.get() != nullptr;
			++i.drawCategories[fmt::format("{} | {} | {}{} | pass {:#x}", form, RttiOf(geometry), RttiOf(property), skinned ? " | skinned" : "", d.passEnum)];
			if (i.drawSamples.size() < 4096)
				++i.drawSamples[fmt::format("{} ({})", geometry->name.c_str() ? geometry->name.c_str() : "?", form)];
		}
		i.volumetricDrawGeometries += volumetricDrawn.size();
		i.hint8Passes += hint8Frame;
		i.hint8Geometries += hint8.size();
		i.sunOtherGeometries += other.size();
		i.maxHint8PerFrame = std::max(i.maxHint8PerFrame, hint8Frame);

		++i.frames;
		if (a_running)
			++i.runningFrames;
		if (i.frames < kInterval)
			return;

		const double frames = i.frames;
		auto top = [](const auto& a_map, std::size_t a_count) {
			std::vector<std::pair<std::string, std::uint64_t>> sorted(a_map.begin(), a_map.end());
			std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
			sorted.resize(std::min(sorted.size(), a_count));
			return sorted;
		};
		std::string hints, flags, rejectText;
		for (const auto& [hint, count] : i.sunHints)
			hints += fmt::format(" {}={:.1f}", hint, count / frames);
		for (const auto& [flag, count] : i.volumetricFlags)
			flags += fmt::format(" {:#x}={}", flag, count);
		for (const auto& [name, count] : i.rejects)
			rejectText += fmt::format(" {}={:.1f}", name, count / frames);

		logger::info("[DCLF] volumetric probe ({} frames, DCLF running {}): views a frame: {:.2f} volumetric copy, {:.2f} cascade, {:.2f} other shadow maps; FinishAccumulating flags of the copy's views:{}",
			i.frames, i.runningFrames, i.volumetricViews / frames, i.cascadeViews / frames, i.otherViews / frames, flags.empty() ? " -" : flags);
		logger::info("[DCLF] volumetric probe: the sun's renderers a frame: {:.1f} hint-8 passes (max {}) from {:.1f} geometries; {:.1f} other Utility passes from {:.1f} geometries; per hint:{}",
			i.hint8Passes / frames, i.maxHint8PerFrame, i.hint8Geometries / frames, i.sunOtherPasses / frames, i.sunOtherGeometries / frames, hints.empty() ? " -" : hints);
		logger::info("[DCLF] volumetric probe: hint-8 geometries a frame with kCastShadows {:.1f}, also registered for the cascades {:.1f}; hint-8 passes outside the sun's renderers {:.1f}; DCLF's caster rule at registration:{}",
			i.hint8CastShadows / frames, i.hint8AlsoCascade / frames, i.hint8Elsewhere / frames, rejectText.empty() ? " -" : rejectText);
		logger::info("[DCLF] volumetric probe: engine CPU a frame: FinishAccumulating {:.3f} ms volumetric copy, {:.3f} ms cascades, {:.3f} ms other shadow maps; RegisterPass {:.3f} ms hint 8, {:.3f} ms the sun's other Utility passes; the sun's Render {:.3f} ms ({:.2f} calls), of it outside the two draws {:.3f} ms",
			Milliseconds(i.volumetricFinishTicks) / frames, Milliseconds(i.cascadeFinishTicks) / frames, Milliseconds(i.otherFinishTicks) / frames,
			Milliseconds(i.hint8RegisterTicks) / frames, Milliseconds(i.sunOtherRegisterTicks) / frames, Milliseconds(i.sunRenderTicks) / frames, i.sunRenders / frames,
			Milliseconds(i.sunRenderTicks - i.sunFinishTicks) / frames);
		logger::info("[DCLF] volumetric probe: the sun's CPU before its draws a frame: full-frustum cull {:.3f} ms, Accumulate (cascade culls and registration) {:.3f} ms",
			Milliseconds(i.sunFullFrustumTicks) / frames, Milliseconds(i.sunAccumulateTicks) / frames);
		i.sunRenderTicks = i.sunRenders = i.sunFinishTicks = 0;
		i.sunAccumulateTicks = i.sunFullFrustumTicks = 0;
		for (const auto& [key, count] : top(i.categories, 12))
			logger::info("[DCLF] volumetric probe: category {:.1f}/frame: {}", count / frames, key);
		std::string drawHints, drawRejectText;
		for (const auto& [hint, count] : i.volumetricDrawHints)
			drawHints += fmt::format(" {}={:.1f}", hint, count / frames);
		for (const auto& [name, count] : i.drawRejects)
			drawRejectText += fmt::format(" {}={:.1f}", name, count / frames);
		logger::info("[DCLF] volumetric probe: Utility draws the engine issued a frame: {:.1f} into the volumetric copy ({:.1f} geometries; {:.1f} with kCastShadows, {:.1f} also drawn into a cascade), {:.1f} into the cascades, {:.1f} into other shadow maps; the copy's draws per hint:{}; DCLF's caster rule at the draw:{}",
			i.volumetricDraws / frames, i.volumetricDrawGeometries / frames, i.volumetricDrawCastShadows / frames, i.volumetricDrawsAlsoCascade / frames, i.cascadeDraws / frames, i.otherDraws / frames,
			drawHints.empty() ? " -" : drawHints, drawRejectText.empty() ? " -" : drawRejectText);
		for (const auto& [key, count] : top(i.drawCategories, 12))
			logger::info("[DCLF] volumetric probe: drawn into the copy {:.1f}/frame: {}", count / frames, key);
		std::string drawSampleText;
		for (const auto& [key, count] : top(i.drawSamples, 24))
			drawSampleText += fmt::format(" | {} x{}", key, count);
		logger::info("[DCLF] volumetric probe: most frequent geometries drawn into the copy:{}", drawSampleText.empty() ? " -" : drawSampleText);
		for (const auto& [key, count] : top(i.cascadeResidue, 16))
			logger::info("[DCLF] volumetric probe: the engine's cascade draws {:.1f}/frame: {}", count / frames, key);
		for (const auto& [key, count] : top(i.sunOtherViews, 6))
			logger::info("[DCLF] volumetric probe: other view inside the sun's Render {:.2f}/frame: {}", count / frames, key);
		for (const auto& [key, count] : top(i.sunOtherDraws, 8))
			logger::info("[DCLF] volumetric probe: drawn in those views {:.1f}/frame: {}", count / frames, key);
		i.sunOtherViews.clear();
		i.sunOtherDraws.clear();
		std::string residueText;
		for (const auto& [key, count] : top(i.residueSamples, 24))
			residueText += fmt::format(" | {} x{}", key, count);
		logger::info("[DCLF] volumetric probe: most frequent engine cascade casters:{}", residueText.empty() ? " -" : residueText);
		i.cascadeResidue.clear();
		i.residueSamples.clear();
		i.volumetricDraws = i.cascadeDraws = i.otherDraws = i.volumetricDrawGeometries = 0;
		i.volumetricDrawCastShadows = i.volumetricDrawsAlsoCascade = 0;
		i.volumetricDrawHints.clear();
		i.drawCategories.clear();
		i.drawRejects.clear();
		i.drawSamples.clear();

		std::string sampleText;
		for (const auto& [key, count] : top(i.samples, 24))
			sampleText += fmt::format(" | {} x{}", key, count);
		logger::info("[DCLF] volumetric probe: most frequent hint-8 geometries:{}", sampleText.empty() ? " -" : sampleText);

		i.frames = i.runningFrames = 0;
		i.volumetricViews = i.cascadeViews = i.otherViews = 0;
		i.hint8Passes = i.hint8Geometries = i.sunOtherPasses = i.sunOtherGeometries = 0;
		i.hint8CastShadows = i.hint8AlsoCascade = i.hint8Elsewhere = 0;
		i.hint8RegisterTicks = i.sunOtherRegisterTicks = 0;
		i.volumetricFinishTicks = i.cascadeFinishTicks = i.otherFinishTicks = 0;
		i.maxHint8PerFrame = 0;
		i.sunHints.clear();
		i.volumetricFlags.clear();
		i.categories.clear();
		i.rejects.clear();
		i.samples.clear();
	}
}
