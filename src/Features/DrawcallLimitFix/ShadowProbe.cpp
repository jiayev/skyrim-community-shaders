#include "ShadowProbe.h"

#include "ConstantMirror.h"
#include "PassCapture.h"
#include "Records.h"
#include "SceneStore.h"
#include "ShadowViews.h"
#include "Switches.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "Deferred.h"
#include "State.h"

#include "RE/B/BSShadowDirectionalLight.h"
#include "RE/B/BSShadowFrustumLight.h"
#include "RE/B/BSShadowLight.h"
#include "RE/B/BSShadowParabolicLight.h"
#include "RE/B/BSUtilityShader.h"

namespace DCLF
{
	namespace
	{
		constexpr std::uint64_t Bit(std::uint32_t a_bit)
		{
			return std::uint64_t(1) << a_bit;
		}

		// BSShaderAccumulator render modes (engine notes: shadow maps).
		constexpr std::uint32_t kRenderDepth = 0xC;
		constexpr std::uint32_t kShadowMapPlain = 0xD;
		constexpr std::uint32_t kShadowMapClamped = 0xE;
		constexpr std::uint32_t kShadowMapPb = 0xF;

		constexpr std::uint32_t kPassEnumBase = 0x2B;  // passEnum = utility technique + 0x2B

		using LightKind = ShadowViews::Kind;

		using Reject = ShadowReject;

		/** @brief The byte the caster rule tests when kCastShadows is clear, for the log line below. */
		std::uint8_t ShadowGlobal()
		{
			static const REL::Relocation<std::uint8_t*> global{ REL::Offset(0x2033498) };
			return *global.get();
		}

		constexpr std::uint32_t kMaxViewsReported = 14;
		constexpr std::uint32_t kParitySamplesPerReport = 6;

		// What the probe measures about a view, beside its identity (ShadowViews::View).
		struct View
		{
			bool drawn = false;
			std::uint32_t renderMode = 0, renderFlags = 0;
			D3D11_VIEWPORT viewport{};
			std::uint32_t depthStencil = 0, depthSlice = 0, depthMode = 0, biasMode = 0, cullMode = 0;
			float parabola[2] = { 0.0f, 0.0f };
			RE::NiPoint3 eye;
			float viewProj[16] = {};
			double ms = 0.0;
			std::uint32_t draws = 0;
			std::uint32_t registrations = 0;
			bool perFrameChecked = false;
			std::uint32_t biasModes = 0, cullModes = 0, fillModes = 0;  // bit sets of the states the draws used
			std::uint32_t alphaTestedDraws = 0;
		};
	}

	struct ShadowProbe::Impl
	{
		std::vector<View> views;  // parallel to ShadowViews::All()
		std::int32_t currentView = -1;  // inside a shadow accumulator's FinishAccumulatingPreResolveDepth
		std::chrono::steady_clock::time_point shadowStart;
		bool enumerated = false;
		RE::NiPoint3 mainEye;  // posAdjust before the shadow maps: the main camera's

		// Per report interval.
		std::uint32_t frames = 0;
		double shadowMs = 0.0, shadowMaxMs = 0.0;
		std::array<double, 4> lightMs{};
		std::array<std::uint32_t, 4> lightRenders{};
		std::array<std::uint32_t, 4> viewsByKind{};
		std::uint32_t viewsDrawn = 0, viewsNotDrawn = 0, unmappedShadowAccumulations = 0, depthAccumulations = 0;
		std::uint32_t portsPartial = 0, portsChecked = 0;
		std::string partialPortSample;
		std::uint32_t clearsFalse = 0;
		std::uint32_t volumetricViews = 0;  // render target 3
		// Registrations, from the drain at EarlyPrepass.
		std::uint32_t utilityTotal = 0, utilityIntoViews = 0, utilityIntoMain = 0, utilityElsewhere = 0;
		ankerl::unordered_dense::map<std::uint64_t, std::uint32_t> techniques;  // technique << 32 | passEnum
		ankerl::unordered_dense::map<std::uint32_t, std::uint32_t> hints;
		std::uint32_t derivationChecked = 0, derivationDiffers = 0;
		std::array<std::uint32_t, 32> derivationBits{};
		std::string derivationSample;
		std::array<std::uint32_t, static_cast<std::size_t>(Reject::Count)> registeredRejected{};
		std::array<std::uint32_t, static_cast<std::size_t>(Reject::Count)> visibleUnregistered{};
		std::uint32_t visibleChecked = 0, visibleRegistered = 0;
		ankerl::unordered_dense::map<const RE::NiRTTI*, std::uint32_t> propertyTypes;
		// Utility PerGeometry parity at SetupGeometry.
		std::uint64_t worldChecks = 0, worldMismatches = 0, worldTransposed = 0;
		std::uint32_t samplesLogged = 0, alphaSamplesLogged = 0;
		float maxEyeDelta = 0.0f;
		std::uint32_t perFrameChecks = 0, perFrameMismatches = 0;
		std::uint32_t eyeMismatches = 0;
		bool globalsLogged = false;
	};

	bool ShadowProbe::Enabled()
	{
		static const bool enabled = SwitchEnabled("CS_DCLF_SHADOW_PROBE");
		return enabled;
	}

	ShadowProbe& ShadowProbe::Get()
	{
		static ShadowProbe probe;
		return probe;
	}

	ShadowProbe::ShadowProbe() :
		impl(std::make_unique<Impl>()) {}

	ShadowProbe::~ShadowProbe() = default;

	namespace
	{
		bool Close(float a, float b)
		{
			return std::fabs(a - b) <= 1e-3f * std::max(1.0f, std::max(std::fabs(a), std::fabs(b)));
		}
	}

	struct ShadowProbe::Hooks
	{
		/** @brief BSShaderAccumulator::FinishAccumulatingPreResolveDepth (vfunc 0x2A): the draw of one view. */
		struct FinishAccumulating
		{
			static void thunk(RE::BSGraphics::BSShaderAccumulator* a_accumulator, std::uint32_t a_renderFlags)
			{
				auto& impl = *ShadowProbe::Get().impl;
				const auto mode = static_cast<std::uint32_t>(a_accumulator->GetRuntimeData().renderMode);
				if (mode == kRenderDepth)
					++impl.depthAccumulations;
				if (mode < kShadowMapPlain || mode > kShadowMapPb || !impl.enumerated) {
					func(a_accumulator, a_renderFlags);
					return;
				}
				const auto id = ShadowViews::Get().ViewOfAccumulator(a_accumulator);
				if (id >= impl.views.size()) {
					++impl.unmappedShadowAccumulations;
					func(a_accumulator, a_renderFlags);
					return;
				}
				auto& view = impl.views[id];
				auto& state = globals::game::shadowState->GetRuntimeData();
				view.drawn = true;
				view.renderMode = mode;
				view.renderFlags = a_renderFlags;
				view.viewport = state.viewPort;
				view.depthStencil = state.depthStencil;
				view.depthSlice = state.depthStencilSlice;
				view.depthMode = state.setDepthStencilMode;
				view.biasMode = state.rasterStateDepthBiasMode;
				view.cullMode = state.rasterStateCullMode;
				view.eye = state.posAdjust.getEye();
				const auto cameraData = state.cameraData.getEye();
				std::memcpy(view.viewProj, &cameraData.viewProjMat, sizeof(view.viewProj));
				static const REL::Relocation<float*> parabolaRadius{ REL::Offset(0x2035df8) };
				static const REL::Relocation<float*> parabolaSide{ REL::Offset(0x2035dfc) };
				view.parabola[0] = *parabolaRadius.get();
				view.parabola[1] = *parabolaSide.get();
				const float dx = view.eye.x - impl.mainEye.x, dy = view.eye.y - impl.mainEye.y, dz = view.eye.z - impl.mainEye.z;
				impl.maxEyeDelta = std::max(impl.maxEyeDelta, std::sqrt(dx * dx + dy * dy + dz * dz));
				impl.currentView = static_cast<std::int32_t>(id);
				const auto start = std::chrono::steady_clock::now();
				func(a_accumulator, a_renderFlags);
				view.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
				impl.currentView = -1;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief BSUtilityShader::SetupGeometry (vfunc 0x6): one Utility draw's constants are complete. */
		struct UtilitySetupGeometry
		{
			static void thunk(RE::BSShader* a_shader, RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags)
			{
				func(a_shader, a_pass, a_renderFlags);
				auto& impl = *ShadowProbe::Get().impl;
				if (impl.currentView < 0 || !a_pass || !a_pass->geometry)
					return;
				auto& view = impl.views[impl.currentView];
				++view.draws;
				auto& mirror = ConstantMirror::Get();

				// The view's ViewProj as the draw reads it (VS_PerFrame b12 c8) against the shadow state's
				// cameraData at the accumulator hook: the same matrix, or the epoch cannot take it there.
				if (!view.perFrameChecked) {
					view.perFrameChecked = true;
					if (auto* perFrame = *globals::game::perFrame.get()) {
						mirror.Watch(perFrame);
						const auto contents = mirror.Contents(perFrame);
						if (contents.size() >= 48 * 4) {
							++impl.perFrameChecks;
							const auto* floats = reinterpret_cast<const float*>(contents.data());
							bool same = true;
							for (std::uint32_t i = 0; i < 16 && same; ++i)
								same = Close(floats[32 + i], view.viewProj[(i % 4) * 4 + i / 4]);  // b12 holds the transpose
							if (!same) {
								if (impl.perFrameMismatches++ == 0)
									logger::info("[DCLF] shadow probe: b12 c8 ({:.4f} {:.4f} {:.4f} {:.4f} | {:.4f} {:.4f} {:.4f} {:.4f}) differs from cameraData.viewProj ({:.4f} {:.4f} {:.4f} {:.4f} | {:.4f} {:.4f} {:.4f} {:.4f})",
										floats[32], floats[33], floats[34], floats[35], floats[44], floats[45], floats[46], floats[47],
										view.viewProj[0], view.viewProj[1], view.viewProj[2], view.viewProj[3], view.viewProj[12], view.viewProj[13], view.viewProj[14], view.viewProj[15]);
							}
						}
					}
					const auto eye = globals::game::shadowState->GetRuntimeData().posAdjust.getEye();
					if (eye.x != view.eye.x || eye.y != view.eye.y || eye.z != view.eye.z)
						++impl.eyeMismatches;
				}

				{
					auto& state = globals::game::shadowState->GetRuntimeData();
					view.biasModes |= 1u << std::min(state.rasterStateDepthBiasMode, 31u);
					view.cullModes |= 1u << std::min(state.rasterStateCullMode, 31u);
					view.fillModes |= 1u << std::min(state.rasterStateFillMode, 31u);
				}
				const bool alphaTested = ((a_pass->passEnum - kPassEnumBase) & 0x80) != 0;
				if (alphaTested)
					++view.alphaTestedDraws;

				// PerGeometry parity for a few draws per report: World (VS b2 c1..c4) against the engine's own
				// NiTransform-to-matrix routine over the geometry's world transform, which subtracts the
				// current posAdjust; AlphaTestRef (PS b2 c2) and TexcoordOffset (VS b1 c0) logged beside it.
				if ((alphaTested ? impl.alphaSamplesLogged : impl.samplesLogged) >= kParitySamplesPerReport / 2)
					return;
				auto* vs = *globals::game::currentVertexShader;
				auto* ps = *globals::game::currentPixelShader;
				if (!vs)
					return;
				auto* geometryBuffer = reinterpret_cast<ID3D11Buffer*>(vs->constantBuffers[2].buffer);
				auto* materialBuffer = reinterpret_cast<ID3D11Buffer*>(vs->constantBuffers[1].buffer);
				auto* pixelGeometryBuffer = ps ? reinterpret_cast<ID3D11Buffer*>(ps->constantBuffers[2].buffer) : nullptr;
				if (!geometryBuffer)
					return;
				mirror.Watch(geometryBuffer);
				const auto geometryBytes = mirror.Contents(geometryBuffer);
				if (geometryBytes.size() < 5 * 16)
					return;
				const auto* world = reinterpret_cast<const float*>(geometryBytes.data()) + 4;  // c1
				using ToMatrix = void (*)(float*, const RE::NiTransform*);
				static const REL::Relocation<ToMatrix> toMatrix{ REL::Offset(0x14aaf10) };
				float expected[16];
				toMatrix(expected, &a_pass->geometry->world);
				bool direct = true, transposed = true;
				for (std::uint32_t r = 0; r < 4 && (direct || transposed); ++r) {
					for (std::uint32_t c = 0; c < 4; ++c) {
						direct = direct && Close(world[r * 4 + c], expected[r * 4 + c]);
						transposed = transposed && Close(world[r * 4 + c], expected[c * 4 + r]);
					}
				}
				++impl.worldChecks;
				if (transposed && !direct)
					++impl.worldTransposed;
				if (!direct && !transposed)
					++impl.worldMismatches;
				++(alphaTested ? impl.alphaSamplesLogged : impl.samplesLogged);
				std::string extra = fmt::format("; VS b1 {}, PS {} b2 {}", materialBuffer ? "bound" : "null", ps ? "bound" : "null", pixelGeometryBuffer ? "bound" : "null");
				if (materialBuffer) {
					mirror.Watch(materialBuffer);
					const auto bytes = mirror.Contents(materialBuffer);
					if (bytes.size() >= 16) {
						const auto* f = reinterpret_cast<const float*>(bytes.data());
						const auto* material = a_pass->shaderProperty ? a_pass->shaderProperty->material : nullptr;
						extra += fmt::format("; TexcoordOffset ({:.3f} {:.3f} {:.3f} {:.3f}) material ({:.3f} {:.3f} {:.3f} {:.3f})", f[0], f[1], f[2], f[3],
							material ? material->texCoordOffset[0].x : 0.0f, material ? material->texCoordOffset[0].y : 0.0f,
							material ? material->texCoordScale[0].x : 0.0f, material ? material->texCoordScale[0].y : 0.0f);
					}
				}
				if (pixelGeometryBuffer) {
					mirror.Watch(pixelGeometryBuffer);
					const auto bytes = mirror.Contents(pixelGeometryBuffer);
					if (bytes.size() >= 3 * 16) {
						const auto* f = reinterpret_cast<const float*>(bytes.data()) + 8;  // c2
						const auto* alpha = a_pass->geometry->GetGeometryRuntimeData().alphaProperty.get();
						extra += fmt::format("; AlphaTestRef ({:.4f} {:.4f} {:.4f} {:.4f}) threshold {}", f[0], f[1], f[2], f[3], alpha ? alpha->alphaThreshold : 0);
					}
				}
				const auto& t = a_pass->geometry->world;
				logger::info("[DCLF] shadow probe sample: view {} ({}) '{}' passEnum {:#x} hint {} flags {:#x}: World {} (row0 {:.4f} {:.4f} {:.4f} {:.4f}; translation {:.2f} {:.2f} {:.2f} vs world {:.2f} {:.2f} {:.2f} eye {:.2f} {:.2f} {:.2f}){}",
					impl.currentView, ShadowViews::KindName(ShadowViews::Get().At(static_cast<std::uint32_t>(impl.currentView))->kind), a_pass->geometry->name.c_str() ? a_pass->geometry->name.c_str() : "?",
					a_pass->passEnum, a_pass->accumulationHint, a_renderFlags,
					direct ? "matches (direct)" : transposed ? "matches (transposed)" : "DIFFERS",
					world[0], world[1], world[2], world[3], world[12], world[13], world[14],
					t.translate.x, t.translate.y, t.translate.z, view.eye.x, view.eye.y, view.eye.z, extra);
				if (!direct && !transposed)
					logger::info("[DCLF] shadow probe sample: expected rows ({:.4f} {:.4f} {:.4f} {:.4f}) ({:.4f} {:.4f} {:.4f} {:.4f}) ({:.4f} {:.4f} {:.4f} {:.4f}) ({:.4f} {:.4f} {:.4f} {:.4f}); bound ({:.4f} {:.4f} {:.4f} {:.4f}) ({:.4f} {:.4f} {:.4f} {:.4f}) ({:.4f} {:.4f} {:.4f} {:.4f}) ({:.4f} {:.4f} {:.4f} {:.4f})",
						expected[0], expected[1], expected[2], expected[3], expected[4], expected[5], expected[6], expected[7], expected[8], expected[9], expected[10], expected[11], expected[12], expected[13], expected[14], expected[15],
						world[0], world[1], world[2], world[3], world[4], world[5], world[6], world[7], world[8], world[9], world[10], world[11], world[12], world[13], world[14], world[15]);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The three lights' Render (vfunc 0xA): CPU per light kind. */
		template <LightKind Kind>
		struct LightRender
		{
			static void thunk(RE::BSShadowLight* a_light, std::uint32_t& a_index)
			{
				auto& impl = *ShadowProbe::Get().impl;
				const auto start = std::chrono::steady_clock::now();
				func(a_light, a_index);
				impl.lightMs[static_cast<std::size_t>(Kind)] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
				++impl.lightRenders[static_cast<std::size_t>(Kind)];
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	};

	void ShadowProbe::Install()
	{
		static bool installed = false;
		if (installed || !Enabled())
			return;
		installed = true;
		stl::write_vfunc<0x2A, Hooks::FinishAccumulating>(RE::VTABLE_BSShaderAccumulator[0]);
		stl::write_vfunc<0x6, Hooks::UtilitySetupGeometry>(RE::VTABLE_BSUtilityShader[0]);
		stl::write_vfunc<0xA, Hooks::LightRender<LightKind::Directional>>(RE::VTABLE_BSShadowDirectionalLight[0]);
		stl::write_vfunc<0xA, Hooks::LightRender<LightKind::Frustum>>(RE::VTABLE_BSShadowFrustumLight[0]);
		stl::write_vfunc<0xA, Hooks::LightRender<LightKind::Parabolic>>(RE::VTABLE_BSShadowParabolicLight[0]);
		logger::info("[DCLF] shadow probe installed (accumulator vfunc 0x2A, BSUtilityShader::SetupGeometry, the three lights' Render)");
	}

	void ShadowProbe::OnBeforeShadowMaps()
	{
		auto& i = *impl;
		// The registry is rebuilt by the feature, before this runs: the probe only measures.
		i.views.assign(ShadowViews::Get().All().size(), View{});
		i.currentView = -1;
		i.mainEye = globals::game::shadowState->GetRuntimeData().posAdjust.getEye();
		if (!i.globalsLogged) {
			i.globalsLogged = true;
			logger::info("[DCLF] shadow probe: the shadow global at 0x142033498 is {} (1 = kCastShadows required; 2 = volumetric copies)", ShadowGlobal());
		}
		i.enumerated = true;
		i.shadowStart = std::chrono::steady_clock::now();
	}

	void ShadowProbe::OnAfterShadowMaps()
	{
		auto& i = *impl;
		if (!i.enumerated)
			return;
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - i.shadowStart).count();
		i.shadowMs += ms;
		i.shadowMaxMs = std::max(i.shadowMaxMs, ms);
		++i.frames;
	}

	void ShadowProbe::OnFrameBuilt()
	{
		auto& i = *impl;
		auto& capture = PassCapture::Get();
		const auto entries = capture.DrainUtility();
		if (!i.enumerated)
			return;

		// The views as drawn.
		auto& store = SceneStore::Get();
		const auto identities = ShadowViews::Get().All();
		for (std::size_t v = 0; v < i.views.size() && v < identities.size(); ++v) {
			const auto& view = i.views[v];
			const auto& identity = identities[v];
			++i.viewsByKind[static_cast<std::size_t>(identity.kind)];
			++(view.drawn ? i.viewsDrawn : i.viewsNotDrawn);
			if (identity.renderTarget == 3)
				++i.volumetricViews;
			if (!identity.clear)
				++i.clearsFalse;
			if (view.drawn) {
				++i.portsChecked;
				const bool full = view.viewport.TopLeftX == 0.0f && view.viewport.TopLeftY == 0.0f &&
				                  static_cast<float>(std::abs(identity.port.right - identity.port.left)) == view.viewport.Width &&
				                  static_cast<float>(std::abs(identity.port.bottom - identity.port.top)) == view.viewport.Height;
				if (!full) {
					++i.portsPartial;
					if (i.partialPortSample.empty())
						i.partialPortSample = fmt::format("{} light {} descriptor {}: port ({} {})-({} {}), viewport ({:.0f} {:.0f}) {:.0f}x{:.0f}",
							ShadowViews::KindName(identity.kind), identity.lightIndex, identity.descriptor, identity.port.left, identity.port.top, identity.port.right, identity.port.bottom,
							view.viewport.TopLeftX, view.viewport.TopLeftY, view.viewport.Width, view.viewport.Height);
				}
			}
		}

		// The Utility registrations of the frame against the views, the derivation and the rule.
		const auto& mainRenderers = store.GetMainBatchRenderers();
		ankerl::unordered_dense::set<const RE::BSGeometry*> inDirectionalView;
		i.utilityTotal += static_cast<std::uint32_t>(entries.size());
		for (const auto& entry : entries) {
			if (!entry.pass || !entry.geometry)
				continue;
			++i.techniques[(std::uint64_t(entry.technique) << 32) | entry.passEnum];
			++i.hints[entry.pass->accumulationHint];
			const auto viewId = ShadowViews::Get().ViewOfBatch(entry.batch);
			if (viewId >= i.views.size()) {
				++(mainRenderers.contains(entry.batch) ? i.utilityIntoMain : i.utilityElsewhere);
				continue;
			}
			++i.utilityIntoViews;
			auto& view = i.views[viewId];
			const auto& identity = *ShadowViews::Get().At(viewId);
			++view.registrations;
			if (identity.kind == LightKind::Directional)
				inDirectionalView.insert(entry.geometry);
			auto* property = entry.pass->shaderProperty ? entry.pass->shaderProperty : entry.geometry->GetGeometryRuntimeData().shaderProperty.get();
			if (!property)
				continue;
			++i.propertyTypes[property->GetRTTI()];
			const auto reason = ShadowCasterReject(property, entry.geometry);
			++i.registeredRejected[static_cast<std::size_t>(reason)];
			if (reason == Reject::NotLighting || !view.drawn)
				continue;
			const std::uint32_t expected = ShadowUtilityTechnique(property, entry.geometry) | ShadowModeBits(view.renderMode);
			const std::uint32_t actual = entry.passEnum - kPassEnumBase;
			++i.derivationChecked;
			if (expected != actual) {
				++i.derivationDiffers;
				for (std::uint32_t bit = 0; bit < 32; ++bit) {
					if (((expected ^ actual) >> bit) & 1)
						++i.derivationBits[bit];
				}
				if (i.derivationSample.empty())
					i.derivationSample = fmt::format("'{}' expected {:#x} got {:#x} (mode {:#x}, flags {:#x})", entry.geometry->name.c_str() ? entry.geometry->name.c_str() : "?",
						expected, actual, view.renderMode, property->flags.underlying());
			}
		}

		// The main pass's kept objects without a directional registration, by the rule's reason: what
		// the rule says about the objects the sun would otherwise shadow. Reason 0 there is a caster the
		// engine's cascade culling left out (beyond the shadow distance, or outside every cascade).
		bool anyDirectional = false;
		for (std::size_t v = 0; v < i.views.size() && v < identities.size(); ++v)
			anyDirectional = anyDirectional || (identities[v].kind == LightKind::Directional && i.views[v].drawn);
		if (anyDirectional) {
			const auto& tables = store.GetTables();
			for (std::size_t o = 0; o < tables.objects.size() && o < tables.objectGeometry.size(); ++o) {
				if (!(tables.objects[o].flags & kObjectNativeVisible))
					continue;
				const auto* geometry = tables.objectGeometry[o];
				if (!geometry)
					continue;
				++i.visibleChecked;
				if (inDirectionalView.contains(geometry)) {
					++i.visibleRegistered;
					continue;
				}
				const auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
				++i.visibleUnregistered[static_cast<std::size_t>(property ? ShadowCasterReject(property, geometry) : Reject::NotLighting)];
			}
		}
	}

	void ShadowProbe::Report(std::uint32_t a_frame, std::uint32_t a_interval)
	{
		if ((a_frame % a_interval) != 0)
			return;
		auto& i = *impl;
		const double frames = std::max(1u, i.frames);

		// The last frame's views, one line each.
		logger::info("[DCLF] shadow probe (last frame): {} views ({} dir, {} spot, {} point, {} other); {} drawn, {} not drawn, {} shadow accumulations outside the list, {} RenderDepth accumulations; {} volumetric (target 3), {} without clear",
			i.views.size(), i.viewsByKind[0] / frames, i.viewsByKind[1] / frames, i.viewsByKind[2] / frames, i.viewsByKind[3] / frames,
			i.viewsDrawn / frames, i.viewsNotDrawn / frames, i.unmappedShadowAccumulations / frames, i.depthAccumulations / frames, i.volumetricViews / frames, i.clearsFalse / frames);
		std::uint32_t reported = 0;
		const auto reportIdentities = ShadowViews::Get().All();
		for (std::size_t v = 0; v < i.views.size() && v < reportIdentities.size() && reported < kMaxViewsReported; ++v, ++reported) {
			const auto& view = i.views[v];
			const auto& identity = reportIdentities[v];
			logger::info("[DCLF] shadow view {}: {} light {} descriptor {}{} target {} slice {} port ({} {})-({} {}) clear {} enabled {}; {}; {} registrations",
				v, ShadowViews::KindName(identity.kind), identity.lightIndex, identity.descriptor, identity.focus ? " (focus)" : "", identity.renderTarget, identity.slice,
				identity.port.left, identity.port.top, identity.port.right, identity.port.bottom, identity.clear, identity.enabled,
				view.drawn ? fmt::format("drawn: mode {:#x} flags {:#x} viewport ({:.0f} {:.0f}) {:.0f}x{:.0f} depth {:.3f}-{:.3f}, depth target {} slice {} (mode {}), bias {} cull {}, parabola ({:.5f} {:+.0f}), eye ({:.1f} {:.1f} {:.1f}), viewProj row3 ({:.4f} {:.4f} {:.4f} {:.4f}), {} draws, {:.3f} ms",
								 view.renderMode, view.renderFlags, view.viewport.TopLeftX, view.viewport.TopLeftY, view.viewport.Width, view.viewport.Height, view.viewport.MinDepth, view.viewport.MaxDepth,
								 view.depthStencil, view.depthSlice, view.depthMode, view.biasMode, view.cullMode, view.parabola[0], view.parabola[1],
								 view.eye.x, view.eye.y, view.eye.z, view.viewProj[12], view.viewProj[13], view.viewProj[14], view.viewProj[15], view.draws, view.ms) + fmt::format(" ({} alpha-tested; bias set {:#x} cull set {:#x} fill set {:#x})", view.alphaTestedDraws, view.biasModes, view.cullModes, view.fillModes) :
				             std::string("not drawn"),
				view.registrations);
		}
		logger::info("[DCLF] shadow probe ports: {} of {} drawn views had a port or viewport smaller than the slice{}{}", i.portsPartial, i.portsChecked,
			i.partialPortSample.empty() ? "" : "; first: ", i.partialPortSample);
		logger::info("[DCLF] shadow probe CPU: shadow maps {:.3f} ms per frame (max {:.3f}); per light kind: dir {} renders {:.3f} ms, spot {} renders {:.3f} ms, point {} renders {:.3f} ms",
			i.shadowMs / frames, i.shadowMaxMs, i.lightRenders[0] / frames, i.lightMs[0] / frames, i.lightRenders[1] / frames, i.lightMs[1] / frames, i.lightRenders[2] / frames, i.lightMs[2] / frames);

		std::vector<std::pair<std::uint64_t, std::uint32_t>> techniques(i.techniques.begin(), i.techniques.end());
		std::sort(techniques.begin(), techniques.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
		std::string histogram;
		for (std::size_t t = 0; t < techniques.size() && t < 12; ++t)
			histogram += fmt::format("{} technique {:#x} passEnum {:#x}={}", histogram.empty() ? "" : ",", techniques[t].first >> 32, techniques[t].first & 0xffffffffu, techniques[t].second);
		std::string hints;
		for (const auto& [hint, count] : i.hints)
			hints += fmt::format(" {}={}", hint, count);
		logger::info("[DCLF] shadow probe registrations: {} Utility per frame: {} into a view, {} into a main renderer, {} elsewhere; techniques:{}; hints:{}",
			i.utilityTotal / frames, i.utilityIntoViews / frames, i.utilityIntoMain / frames, i.utilityElsewhere / frames, histogram.empty() ? " none" : histogram, hints);
		std::string bits;
		for (std::uint32_t bit = 0; bit < 32; ++bit) {
			if (i.derivationBits[bit])
				bits += fmt::format(" bit{}={}", bit, i.derivationBits[bit]);
		}
		std::string types;
		for (const auto& [rtti, count] : i.propertyTypes)
			types += fmt::format(" {}={}", rtti && rtti->name ? rtti->name : "?", count);
		logger::info("[DCLF] shadow probe derivation: {} registrations compared, {} differ{}{}{}; property types:{}", i.derivationChecked, i.derivationDiffers,
			bits.empty() ? "" : ";", bits, i.derivationSample.empty() ? "" : fmt::format("; first: {}", i.derivationSample), types);
		std::string registered, unregistered;
		for (std::size_t r = 0; r < static_cast<std::size_t>(Reject::Count); ++r) {
			if (i.registeredRejected[r])
				registered += fmt::format(" {}={}", ShadowRejectName(static_cast<Reject>(r)), i.registeredRejected[r]);
			if (i.visibleUnregistered[r])
				unregistered += fmt::format(" {}={}", ShadowRejectName(static_cast<Reject>(r)), i.visibleUnregistered[r]);
		}
		std::uint32_t falsePositives = 0;
		for (std::size_t r = 2; r < static_cast<std::size_t>(Reject::Count); ++r)
			falsePositives += i.registeredRejected[r];
		logger::info("[DCLF] shadow probe rule: registered casters by the rule's verdict:{}{}; main-visible objects: {} checked, {} in a directional view, the rest by verdict:{}",
			registered.empty() ? " none" : registered, falsePositives ? " <- RULE REJECTS A CASTER" : "", i.visibleChecked, i.visibleRegistered, unregistered.empty() ? " none" : unregistered);
		logger::info("[DCLF] shadow probe parity: World {} checked, {} matched transposed, {} differ{}; b12 c8 {} checked, {} differ{}; posAdjust mismatches {}; max shadow-eye offset from the main eye {:.1f}",
			i.worldChecks, i.worldTransposed, i.worldMismatches, i.worldMismatches ? " <- WORLD DIFFERS" : "", i.perFrameChecks, i.perFrameMismatches,
			i.perFrameMismatches ? " <- VIEWPROJ DIFFERS" : "", i.eyeMismatches, i.maxEyeDelta);

		// Reset the interval's counters; the views themselves are rebuilt every frame.
		i.frames = 0;
		i.shadowMs = i.shadowMaxMs = 0.0;
		i.lightMs = {};
		i.lightRenders = {};
		i.viewsByKind = {};
		i.viewsDrawn = i.viewsNotDrawn = i.unmappedShadowAccumulations = i.depthAccumulations = 0;
		i.portsPartial = i.portsChecked = 0;
		i.partialPortSample.clear();
		i.clearsFalse = i.volumetricViews = 0;
		i.utilityTotal = i.utilityIntoViews = i.utilityIntoMain = i.utilityElsewhere = 0;
		i.techniques.clear();
		i.hints.clear();
		i.derivationChecked = i.derivationDiffers = 0;
		i.derivationBits = {};
		i.derivationSample.clear();
		i.registeredRejected = {};
		i.visibleUnregistered = {};
		i.visibleChecked = i.visibleRegistered = 0;
		i.propertyTypes.clear();
		i.worldChecks = i.worldMismatches = i.worldTransposed = 0;
		i.samplesLogged = i.alphaSamplesLogged = 0;
		i.maxEyeDelta = 0.0f;
		i.perFrameChecks = i.perFrameMismatches = i.eyeMismatches = 0;
	}
}
