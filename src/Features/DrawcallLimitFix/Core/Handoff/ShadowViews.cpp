// ShadowViews: the caster verdicts (ShadowCasterReject, on whichever thread classifies) and the frame's shadow view list (render thread:
// BeginCasters and AddCaster as the engine selects its shadow lights, Rebuild once the selection is done).

#include "Features/DrawcallLimitFix/Core/Handoff/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"

#include <bit>
#include <cstring>

#include "Deferred.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"

#include "RE/B/BSShadowLight.h"

namespace DCLF
{
	namespace
	{
		// PredictTargets' engine state (Engine::Offset: Address Library ids).
		constexpr Engine::Offset kShadowFreeMask{ { 391715 }, { 513748 } };   // std::uint32_t: RenderShadowmap's free target slices
		constexpr Engine::Offset kVolumetricCopy{ { 391108 }, { 513334 } };   // std::uint32_t: 2 when the cascades take a volumetric lighting copy
		constexpr Engine::Offset kFocusTargetBase{ { 255253 }, { 305513 } };  // std::uint32_t: the focus views' first slice in target 4

		constexpr std::uint64_t Bit(std::uint32_t a_bit)
		{
			return std::uint64_t(1) << a_bit;
		}

		/** @brief The byte GetRenderPasses_ShadowMapOrMask tests when kCastShadows is clear (0x142033498). */
		// GetRenderPasses_ShadowMapOrMask's global (0x2033498), as the frame captured it (FrameGlobals).
		std::uint8_t ShadowGlobal()
		{
			return FrameGlobals::Current().shadowGlobal;
		}
	}

	const char* ShadowRejectName(ShadowReject a_reason)
	{
		constexpr const char* kNames[] = { "eligible", "not-lighting", "decl-0", "faded", "refraction", "alpha-blended", "decal-no-zwrite", "no-cast-shadows", "volumetric-only", "decal-point-light", "layer", "lod" };
		static_assert(std::size(kNames) == static_cast<std::size_t>(ShadowReject::Count));
		const auto index = static_cast<std::size_t>(a_reason);
		return index < std::size(kNames) ? kNames[index] : "?";
	}

	std::uint32_t UtilityShaderDecl(std::uint64_t a_flags)
	{
		// BSLightingShaderProperty::DetermineUtilityShaderDecl (vtable slot 0x3D, AE 0x1414adf00), ported (the scene work
		// calls no engine code): a function of the property's flags (+0x38) alone, never 0.
		std::uint32_t decl = (a_flags & 0x1000) ? 0x2u : 0x1Au;
		if (a_flags & 0x2)
			decl |= 0x4;
		if (a_flags & 0x20'0000'0000ull)
			decl |= 0x1;
		if (a_flags & (0x4000ull | 0x4000'0000'0000ull))
			decl |= 0x20;
		if (a_flags & 0x2'0000'0000ull)
			decl |= 0x100;
		if (a_flags & 0x2'0000ull)
			decl |= 0x40;
		if (a_flags & 0x2000'0000'0000'0000ull)
			decl |= 1u << 26;
		return decl;
	}

	ShadowReject ShadowCasterReject(const SceneCapture::LeafView& a_leaf)
	{
		const auto* lighting = SceneCapture::LeafView::Lighting(a_leaf.property);
		if (!lighting || !a_leaf.geometry)
			return ShadowReject::NotLighting;
		const std::uint64_t flags = lighting->flags;
		if (IsLodObject(flags, a_leaf.Type()) || IsLodLand(flags, a_leaf.Type()))
			return ShadowReject::Lod;
		const bool blended = a_leaf.AlphaBlending();
		const bool decal = (flags & (Bit(26) | Bit(27))) != 0;
		const bool decalLike = decal && (flags & Bit(18));
		if (decal && !(decalLike && (flags & Bit(32)) && blended))
			return decalLike ? ShadowReject::DecalNoZWrite : ShadowReject::DecalPointLight;
		// The fade node's share (fade * materialAlpha < 1) is the GPU's: a shadow view drops a caster while its root fades.
		if (lighting->materialAlpha < 1.0f)
			return ShadowReject::Faded;
		if (flags & 0x8004ull)
			return ShadowReject::Refraction;
		if (blended && !decalLike)
			return ShadowReject::AlphaBlended;
		// GetRenderPasses_ShadowMapOrMask, for a shadow mode and kCastShadows clear: global 1 registers no pass;
		// global 2 registers only the volumetric copy's pass (see VolumetricOnly); global 0 casts as usual.
		if (!(flags & Bit(9))) {
			if (ShadowGlobal() == 1)
				return ShadowReject::NoCastShadows;
			if (ShadowGlobal() == 2)
				return ShadowReject::VolumetricOnly;
		}
		if (UtilityShaderDecl(flags) == 0)
			return ShadowReject::DeclZero;
		return ShadowReject::None;
	}

	bool CastsNoShadow(const SceneCapture::LeafView& a_leaf)
	{
		const auto* lighting = SceneCapture::LeafView::Lighting(a_leaf.property);
		if (!lighting || !a_leaf.geometry)
			return false;
		const std::uint64_t flags = lighting->flags;
		if (IsLodObject(flags, a_leaf.Type()) || IsLodLand(flags, a_leaf.Type()))
			return true;  // Lod
		const bool blended = a_leaf.AlphaBlending();
		const bool decal = (flags & (Bit(26) | Bit(27))) != 0;
		const bool decalLike = decal && (flags & Bit(18));
		if (decal && !(decalLike && (flags & Bit(32)) && blended))
			return true;  // DecalNoZWrite, DecalPointLight
		if (flags & 0x8004ull)
			return true;  // Refraction: GetRenderPasses_ShadowMapOrMask rejects the flags themselves
		if (blended && !decalLike)
			return true;  // AlphaBlended
		return UtilityShaderDecl(flags) == 0;
	}

	std::uint32_t ShadowUtilityTechnique(const SceneCapture::LeafView& a_leaf)
	{
		const auto* lighting = SceneCapture::LeafView::Lighting(a_leaf.property);
		if (!lighting || !a_leaf.geometry)
			return 0;
		const std::uint64_t flags = lighting->flags;
		std::uint32_t technique = UtilityShaderDecl(flags);
		if (a_leaf.AlphaTesting())
			technique |= 0x80;
		if (flags & (Bit(34) | Bit(63)))
			technique |= 0x8000000;
		if (flags & (Bit(26) | Bit(27)))
			technique |= 0x20080;
		return technique;
	}

	std::uint32_t ShadowModeBits(std::uint32_t a_renderMode)
	{
		switch (a_renderMode) {
		case 0xC: return 0x2000;   // RenderDepth, the main Z-prepass
		case 0xE: return 0xC000;   // ShadowMapClamped
		case 0xF: return 0x14000;  // ShadowMapPb
		default: return 0x4000;    // ShadowMapPlain
		}
	}

	// Core's view of the kinds (Extensions::ShadowViewSet::sourceOf is by them).
	static_assert(Extensions::kShadowViewKinds == ShadowViews::kKindCount && static_cast<std::size_t>(ShadowViews::Kind::Directional) == 0 &&
				  static_cast<std::size_t>(ShadowViews::Kind::Frustum) == 1 && static_cast<std::size_t>(ShadowViews::Kind::Parabolic) == 2 &&
				  static_cast<std::size_t>(ShadowViews::Kind::Other) == 3);

	const char* ShadowViews::KindName(Kind a_kind)
	{
		switch (a_kind) {
		case Kind::Directional: return "dir";
		case Kind::Frustum: return "spot";
		case Kind::Parabolic: return "point";
		default: return "other";
		}
	}

	ShadowViews& ShadowViews::Get()
	{
		static ShadowViews views;
		return views;
	}

	LightSlotFrame& ShadowViews::LightSlots()
	{
		static LightSlotFrame frame;
		return frame;
	}

	void ShadowViews::Clear()
	{
		views.clear();
		captures.clear();
		batchToView.clear();
		accumulatorToView.clear();
		valid = false;
		candidates = 0;
		unrecorded = false;
	}

	void ShadowViews::BeginCasters()
	{
		// Before the first shadow light's Accumulate: none of the frame's lights is in yet, so nothing registered from here on is withheld
		// by the last frame's renderers. The frame's verdict is DCLF's until the coverage decision says otherwise (SetDrawable).
		Clear();
		drawable = true;
		PublishRenderers();
	}

	void ShadowViews::AddCaster(const RE::BSShadowLight* a_light, std::uint32_t a_firstSlot, std::uint32_t a_slots)
	{
		// The engine has just kept a_light (FUN_1414a3fb0, its slots in shadowLightsAccum), its UpdateCamera run and its focus flag set, and
		// is about to Accumulate it: its views, as Rebuild will list them from DCLF's selection (a slot each, the focus views once, after
		// the first's), are covered and their renderers published before any of its registrations. Not before the selection has published
		// once (LightSlotFrame::published): without it Rebuild lists no view, so nothing may be withheld.
		auto* light = const_cast<RE::BSShadowLight*>(a_light);
		if (!light || !LightSlots().published)
			return;
		const std::size_t first = views.size();
		const std::uint32_t focusCount = std::min<std::uint32_t>(Engine::Global<std::uint32_t>(Engine::Address::kFocusTargetCount), 4u);
		for (std::uint32_t s = 0; s < a_slots; ++s)
			AddLightViews(*light, a_firstSlot + s, s == 0 && light->GetRuntimeData().drawFocusShadows ? focusCount : 0u);
		if (CoverFrom(first))
			PublishRenderers();
	}

	void ShadowViews::AddLightViews(RE::BSShadowLight& a_light, std::uint32_t a_lightIndex, std::uint32_t a_focusCount)
	{
		auto* light = &a_light;
		const std::uint32_t lightIndex = a_lightIndex;
		const Kind kind = light->GetIsDirectionalLight() ? Kind::Directional :
		                  light->GetIsFrustumLight()     ? Kind::Frustum :
		                  light->GetIsParabolicLight()   ? Kind::Parabolic :
		                                                   Kind::Other;
		auto& data = light->GetRuntimeData();
		const auto add = [&](const RE::BSShadowLight::ShadowmapDescriptor& a_descriptor, std::uint32_t a_index, bool a_focus) {
			View view;
			view.light = light;
			view.lightIndex = lightIndex;
			view.kind = kind;
			view.descriptor = a_index;
			view.focus = a_focus;
			auto* accumulator = a_descriptor.shaderAccumulator.get();
			view.accumulator = accumulator;
			auto* accumulatorData = accumulator ? accumulator->GetRuntimeData() : nullptr;
			view.batch = accumulatorData ? accumulatorData->batchRenderer : nullptr;
			view.renderMode = accumulatorData ? static_cast<std::uint32_t>(accumulatorData->renderMode) : 0u;
			view.renderTarget = static_cast<std::uint32_t>(a_descriptor.renderTarget);
			view.slice = a_descriptor.shadowmapIndex;
			static_assert(sizeof(a_descriptor.port) == sizeof(Port));
			std::memcpy(&view.port, &a_descriptor.port, sizeof(view.port));
			view.clear = a_descriptor.clearRenderTarget;
			view.enabled = a_descriptor.isEnabled;
			const auto id = static_cast<std::uint32_t>(views.size());
			if (accumulator)
				accumulatorToView.try_emplace(accumulator, id);
			if (view.batch) {
				batchToView.try_emplace(view.batch, id);
				// A geometry group sorts its passes in a batch renderer of its own, as the main
				// camera's does; a registration into one of those belongs to this view too.
				for (auto* group : view.batch->geometryGroups) {
					if (group && group->batchRenderer)
						batchToView.try_emplace(group->batchRenderer, id);
				}
			}
			views.push_back(view);
		};
		for (std::uint32_t d = 0; d < data.shadowmapDescriptors.size(); ++d)
			add(data.shadowmapDescriptors[d], d, false);
		// A focus host's focus views (Render draws descriptors 0..count-1).
		for (std::uint32_t d = 0; d < a_focusCount; ++d)
			add(data.focusShadowmapDescriptors[d], d, true);
	}

	bool ShadowViews::CoverFrom(std::size_t a_first)
	{
		// DCLF's views: its candidates in the engine's order, within the slots its buffers hold (SetViewCapacity); a view of a source no
		// feature takes (Extensions::ShadowViewSet::sourceOf) is the engine's. On a frame whose views are not DCLF's (SetDrawable), none
		// is covered: the engine draws every one whole and nothing is withheld.
		bool covered = false;
		const auto& shadowViews = Extensions::ShadowViewsSet();
		for (std::size_t i = a_first; i < views.size(); ++i) {
			auto& view = views[i];
			const auto source = shadowViews.sourceOf[static_cast<std::size_t>(view.kind)][view.focus ? 1 : 0];
			const bool candidate = view.renderMode >= PassCapture::kFirstShadowMode &&
			                       view.renderMode < PassCapture::kFirstShadowMode + PassCapture::kShadowModes &&
			                       (source == Api::ShadowSource::Count || shadowViews.sources[static_cast<std::size_t>(source)].registered);
			const bool wouldCover = candidate && candidates < viewCapacity;
			view.covered = wouldCover && drawable;
			unrecorded = unrecorded || (wouldCover && !drawable);
			covered = covered || view.covered;
			candidates += candidate ? 1u : 0u;
		}
		return covered;
	}

	void ShadowViews::Rebuild()
	{
		Clear();
		// DCLF's selection's slots (the LightSlots board, Features/LocalLights' selection publishes it): shadowLightsAccum as
		// CalculateActiveShadowCasterLights fills it, the array GetShadowCasterLightArrayEntry indexes - the lights, in the sequence,
		// the engine renders a moment later. None this frame (no selection ran): no views, and the engine draws them, with no renderer
		// published (not the last list's, nor the ones AddCaster published).
		const auto* selection = LightSlots().Current(Scene::FrameView::Get().GetFrame());
		if (!selection) {
			PublishRenderers();
			return;
		}
		std::uint32_t lightIndex = 0;
		std::vector<const RE::BSShadowLight*> focusAdded;
		for (const auto* selected : selection->slots) {
			auto* light = const_cast<RE::BSShadowLight*>(selected);
			if (!light) {
				++lightIndex;
				continue;
			}
			// A focus host's focus views (the LightSlots board: DCLF's hosts and target count), once per light.
			const bool focusHost = std::find(selection->focusHosts.begin(), selection->focusHosts.end(), light) != selection->focusHosts.end() &&
			                       std::find(focusAdded.begin(), focusAdded.end(), light) == focusAdded.end();
			if (focusHost)
				focusAdded.push_back(light);
			AddLightViews(*light, lightIndex, focusHost ? selection->focusCount : 0u);
			++lightIndex;
		}
		PredictTargets();
		captures.assign(views.size(), 0);
		valid = true;
		(void)CoverFrom(0);
		PublishRenderers();
	}

	void ShadowViews::PublishRenderers()
	{
		// The shadow renderers by render mode, for the registration hook's withholding (the set's shadow phases): DCLF's views'.
		// Published whole; the hook runs on the engine's registration threads.
		auto renderers = std::make_shared<PassCapture::ShadowRendererMap>();
		auto focus = std::make_shared<PassCapture::ShadowRendererMap>();  // the covered focus views' renderers, by focus descriptor (the focus membership observer's)
		for (const auto& [batch, id] : batchToView) {
			const auto& view = views[id];
			if (!view.covered)
				continue;
			renderers->emplace(batch, static_cast<std::uint8_t>(view.renderMode - PassCapture::kFirstShadowMode));
			if (view.focus)
				focus->emplace(batch, static_cast<std::uint8_t>(view.descriptor));
		}
		PassCapture::Get().SetShadowBatchRenderers(std::move(renderers));
		PassCapture::Get().SetFocusBatchRenderers(std::move(focus));
	}

	void ShadowViews::PredictTargets()
	{
		// The engine's Renders, in their order (the views' own): BSShadowDirectionalLight::Render draws each cascade into target 2 at
		// its index, after its volumetric lighting copy (target 3) when the dword 0x142033498 is 2; any host's focus views go to target
		// 4 at 0x141ac07b0 + i. A spot light's descriptor 0 and a point light's hemisphere 0 keep the target and slice they hold;
		// one with target -1 is given target 4 and the free mask's lowest set bit (0x142035798) by RenderShadowmap (0x1414f0cf0),
		// which clears it. A point light's hemisphere 1 takes hemisphere 0's target and keeps its own slice. But RenderShadowmap binds
		// the target only for a descriptor that clears (clearRenderTarget, +0xE8; the focus views always do): one that does not draws
		// where the last draw bound - a point light's hemisphere 1 into hemisphere 0's slice.
		using T = RE::RENDER_TARGETS_DEPTHSTENCIL;
		const auto base = REL::Module::get().base();
		std::uint32_t freeMask = *reinterpret_cast<const std::uint32_t*>(base + kShadowFreeMask);
		const bool volumetric = *reinterpret_cast<const std::uint32_t*>(base + kVolumetricCopy) == 2;
		const std::uint32_t focusBase = *reinterpret_cast<const std::uint32_t*>(base + kFocusTargetBase);
		// Hemisphere 0's target, by light, for hemisphere 1; and the descriptors given a slice this frame (a light's views may repeat).
		ankerl::unordered_dense::map<const RE::BSShadowLight*, std::uint32_t> firstTarget;
		ankerl::unordered_dense::map<const void*, std::pair<std::uint32_t, std::uint32_t>> given;
		// The target and slice the last clearing draw bound.
		bool bound = false;
		std::uint32_t boundTarget = 0, boundSlice = 0;
		// A light's views repeat for each slot it takes (the sun's cascades); the engine renders them once, at the first.
		ankerl::unordered_dense::map<const RE::BSShadowLight*, std::uint32_t> firstSlot;
		bool repeat = false;
		const auto draw = [&](View& a_view, bool a_clear) {
			if (repeat)
				return;
			if (!a_clear && bound) {
				a_view.drawTarget = boundTarget;
				a_view.drawSlice = boundSlice;
			}
			bound = true;
			boundTarget = a_view.drawTarget;
			boundSlice = a_view.drawSlice;
		};
		for (auto& view : views) {
			repeat = firstSlot.try_emplace(view.light, view.lightIndex).first->second != view.lightIndex;
			auto& data = const_cast<RE::BSShadowLight*>(view.light)->GetRuntimeData();
			if (view.focus) {
				view.drawTarget = static_cast<std::uint32_t>(T::kSHADOWMAPS);
				view.drawSlice = focusBase + view.descriptor;
				draw(view, true);
				continue;
			}
			if (view.descriptor >= data.shadowmapDescriptors.size())
				continue;
			const auto& descriptor = data.shadowmapDescriptors[view.descriptor];
			if (view.kind == Kind::Directional) {
				view.drawTarget = static_cast<std::uint32_t>(T::kSHADOWMAPS_ESRAM);
				view.drawSlice = view.descriptor;
				view.volumetricCopy = volumetric;
				draw(view, descriptor.clearRenderTarget);
				continue;
			}
			if (view.kind == Kind::Parabolic && view.descriptor == 1) {
				const auto it = firstTarget.find(view.light);
				view.drawTarget = it != firstTarget.end() ? it->second : static_cast<std::uint32_t>(descriptor.renderTarget);
				view.drawSlice = descriptor.shadowmapIndex;
				draw(view, descriptor.clearRenderTarget);
				continue;
			}
			if (const auto it = given.find(&descriptor); it != given.end()) {
				view.drawTarget = it->second.first;
				view.drawSlice = it->second.second;
			} else if (static_cast<std::int32_t>(descriptor.renderTarget) != -1) {
				view.drawTarget = static_cast<std::uint32_t>(descriptor.renderTarget);
				view.drawSlice = descriptor.shadowmapIndex;
			} else {
				std::uint32_t slice = 0;
				if (freeMask) {
					slice = static_cast<std::uint32_t>(std::countr_zero(freeMask));
					freeMask &= ~(1u << slice);
				}
				view.drawTarget = static_cast<std::uint32_t>(T::kSHADOWMAPS);
				view.drawSlice = slice;
				given.emplace(&descriptor, std::make_pair(view.drawTarget, slice));
			}
			if (view.descriptor == 0)
				firstTarget.try_emplace(view.light, view.drawTarget);
			draw(view, descriptor.clearRenderTarget);
		}
	}

	void ShadowViews::SetDrawable(bool a_drawable)
	{
		// Decided at BeginCasters, before any light is in: the views AddCaster and Rebuild list from here are covered by it.
		drawable = a_drawable;
	}

	std::uint32_t ShadowViews::ViewOfAccumulator(const void* a_accumulator) const
	{
		const auto it = accumulatorToView.find(a_accumulator);
		return it == accumulatorToView.end() ? ~0u : it->second;
	}
}
