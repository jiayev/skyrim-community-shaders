#include "ShadowViews.h"

#include <cstring>

#include "Deferred.h"
#include "PassCapture.h"

#include "RE/B/BSShadowLight.h"

namespace DCLF
{
	namespace
	{
		constexpr std::uint64_t Bit(std::uint32_t a_bit)
		{
			return std::uint64_t(1) << a_bit;
		}

		/** @brief The byte GetRenderPasses_ShadowMapOrMask tests when kCastShadows is clear (0x142033498). */
		std::uint8_t ShadowGlobal()
		{
			static const REL::Relocation<std::uint8_t*> global{ REL::Offset(0x2033498) };
			return *global.get();
		}
	}

	const char* ShadowRejectName(ShadowReject a_reason)
	{
		constexpr const char* kNames[] = { "eligible", "not-lighting", "decl-0", "faded", "refraction", "alpha-blended", "decal-no-zwrite", "no-cast-shadows", "volumetric-only" };
		static_assert(std::size(kNames) == static_cast<std::size_t>(ShadowReject::Count));
		const auto index = static_cast<std::size_t>(a_reason);
		return index < std::size(kNames) ? kNames[index] : "?";
	}

	ShadowReject ShadowCasterReject(const RE::BSShaderProperty* a_property, const RE::BSGeometry* a_geometry)
	{
		const auto* lighting = netimmerse_cast<const RE::BSLightingShaderProperty*>(a_property);
		if (!lighting || !a_geometry)
			return ShadowReject::NotLighting;
		const std::uint64_t flags = lighting->flags.underlying();
		const auto* alpha = a_geometry->GetGeometryRuntimeData().alphaProperty.get();
		const bool blended = alpha && (alpha->alphaFlags & 1);
		const bool decal = (flags & (Bit(26) | Bit(27))) != 0;
		const bool decalLike = decal && (flags & Bit(18));
		if (decal && !(decalLike && (flags & Bit(32)) && blended))
			return ShadowReject::DecalNoZWrite;
		const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(lighting->material);
		const float fade = lighting->fadeNode ? const_cast<RE::BSFadeNode*>(lighting->fadeNode)->GetRuntimeData().currentFade : 1.0f;
		if (material && fade * material->materialAlpha < 1.0f)
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
		if (const_cast<RE::BSLightingShaderProperty*>(lighting)->DetermineUtilityShaderDecl() == 0)
			return ShadowReject::DeclZero;
		return ShadowReject::None;
	}

	std::uint32_t ShadowUtilityTechnique(const RE::BSShaderProperty* a_property, const RE::BSGeometry* a_geometry)
	{
		auto* lighting = const_cast<RE::BSLightingShaderProperty*>(netimmerse_cast<const RE::BSLightingShaderProperty*>(a_property));
		if (!lighting || !a_geometry)
			return 0;
		const std::uint64_t flags = lighting->flags.underlying();
		std::uint32_t technique = lighting->DetermineUtilityShaderDecl();
		const auto* alpha = a_geometry->GetGeometryRuntimeData().alphaProperty.get();
		if (alpha && (alpha->alphaFlags & (1u << 9)))
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

	void ShadowViews::Clear()
	{
		views.clear();
		batchToView.clear();
		accumulatorToView.clear();
		valid = false;
	}

	void ShadowViews::Rebuild()
	{
		Clear();
		auto* smState = globals::game::smState;
		auto* node = smState ? smState->shadowSceneNode[0] : nullptr;
		if (!node)
			return;
		auto& runtime = node->GetRuntimeData();
		std::uint32_t lightIndex = 0;
		// shadowLightsAccum is the array GetShadowCasterLightArrayEntry indexes, so this is the engine's
		// own order: the same lights, in the same sequence, that it renders a moment later.
		for (auto* light : runtime.shadowLightsAccum) {
			if (!light) {
				++lightIndex;
				continue;
			}
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
			if (data.drawFocusShadows) {
				for (std::uint32_t d = 0; d < 4; ++d)
					add(data.focusShadowmapDescriptors[d], d, true);
			}
			++lightIndex;
		}
		valid = true;
		// The shadow renderers by render mode, for the registration hook's withholding (static shadow
		// ownership). Published whole, as the main camera's set is; the hook runs on the engine's
		// registration threads.
		auto renderers = std::make_shared<PassCapture::ShadowRendererMap>();
		for (const auto& [batch, id] : batchToView) {
			const auto& view = views[id];
			if (view.focus || view.renderMode < PassCapture::kFirstShadowMode || view.renderMode >= PassCapture::kFirstShadowMode + PassCapture::kShadowModes)
				continue;
			renderers->emplace(batch, static_cast<std::uint8_t>(view.renderMode - PassCapture::kFirstShadowMode));
		}
		PassCapture::Get().SetShadowBatchRenderers(std::move(renderers));
	}

	std::uint32_t ShadowViews::ViewOfBatch(const RE::BSBatchRenderer* a_batch) const
	{
		const auto it = batchToView.find(a_batch);
		return it == batchToView.end() ? ~0u : it->second;
	}

	std::uint32_t ShadowViews::ViewOfAccumulator(const void* a_accumulator) const
	{
		const auto it = accumulatorToView.find(a_accumulator);
		return it == accumulatorToView.end() ? ~0u : it->second;
	}
}
