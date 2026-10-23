// InstallFeature: the observers of the feature file's frame events (Hooks.h): the frame-level parities, probes, leak count and
// target dump. Each runs on the render thread at its hook point.
#include "Features/DrawcallLimitFix/Diagnostics/Install.h"

#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/CaptureParity.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Diagnostics/PlayerParity.h"
#include "Features/DrawcallLimitFix/Diagnostics/Policy.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"
#include "Deferred.h"
#include "Features/DrawcallLimitFix/Diagnostics/Feature/OpenDefectProbes.h"
#include "Features/DrawcallLimitFix/Diagnostics/Feature/TargetDump.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/TerrainBlending.h"

namespace DCLF::Reports
{
	LeakStats& Leaks()
	{
		static LeakStats leaks;
		return leaks;
	}
}

namespace DCLF::Diagnostics
{
	namespace
	{
		/**
		 * @brief A native pass of the main camera's depth or opaque range: a member of the frame's DCLF set is a leak (its registration
		 * should have withheld it), counted with the first few for the frame report. Not while the capture check has the engine draw all.
		 */
		void NoteLeak(const NativePassOffered& a_offered)
		{
			const auto* pass = a_offered.pass;
			if (!pass || !pass->geometry || !Policy::WithholdFromEngine())
				return;
			if (!a_offered.inDepthPass && !globals::deferred->deferredPass)
				return;  // shadows, reflections and cubemaps: not the main camera's views
			auto& leaks = Reports::Leaks();
			++leaks.offered;
			const auto set = Scene::FrameView::Get().FrameClaims();
			if (!set || !(set->PhasesOf(pass->geometry) & kSetMain))
				return;
			++(a_offered.inDepthPass ? leaks.leakedInDepth : leaks.leakedInOpaque);
			// The compatibility monitor's: by the geometry's reference (another plugin's registration is the likely cause).
			if (PlayerParity::Active()) {
				std::uint32_t form = 0;
				for (const RE::NiAVObject* at = pass->geometry; at && !form; at = at->parent)
					if (const auto* reference = at->GetUserData())
						form = reference->GetFormID();
				PlayerParity::NoteLeak(form, fmt::format("'{}' ({} pass, technique {:08X})", pass->geometry->name.c_str() ? pass->geometry->name.c_str() : "?",
												 a_offered.inDepthPass ? "depth" : "opaque", a_offered.technique));
			}
			if (leaks.samples.size() < 8)
				leaks.samples.push_back(fmt::format("'{}' ({}, hint {}, technique {:08X})", pass->geometry->name.c_str() ? pass->geometry->name.c_str() : "?",
					a_offered.inDepthPass ? "depth" : "opaque", static_cast<std::uint32_t>(pass->accumulationHint), a_offered.technique));
		}

		/**
		 * @brief CS_DCLF_TARGET_PROBE: the blend state a native Lighting draw of the main pass left bound (its write masks per target), and
		 * the renderer's alpha blend indices, once for each distinct write mode.
		 */
		void DumpNativeBlend(const NativePassDrawn& a_drawn)
		{
			const auto* pass = a_drawn.pass;
			if (a_drawn.inDepthPass || !pass->shader || pass->shader->shaderType.get() != RE::BSShader::Type::Lighting)
				return;
			static std::uint32_t loggedModes = 0;
			auto& state = globals::game::shadowState->GetRuntimeData();
			const std::uint32_t writeMode = state.alphaBlendWriteMode;
			if (writeMode >= 32 || ((loggedModes >> writeMode) & 1))
				return;
			loggedModes |= 1u << writeMode;
			winrt::com_ptr<ID3D11BlendState> blend;
			float factor[4];
			UINT mask = 0;
			globals::d3d::context->OMGetBlendState(blend.put(), factor, &mask);
			std::string masks;
			if (blend) {
				D3D11_BLEND_DESC desc{};
				blend->GetDesc(&desc);
				for (std::uint32_t i = 0; i < 8; ++i) {
					const auto& rt = desc.RenderTarget[desc.IndependentBlendEnable ? i : 0];
					masks += fmt::format(" rt{} {}{:X}", i, rt.BlendEnable ? "blend " : "", rt.RenderTargetWriteMask);
				}
			}
			ID3D11ShaderResourceView* t55 = nullptr;
			globals::d3d::context->PSGetShaderResources(55, 1, &t55);
			const auto& terrain = globals::features::terrainBlending;
			logger::info("[DCLF] native main-pass blend: alphaBlendMode {}, alphaBlendWriteMode {}, alpha to coverage {}, '{}':{}; t55 {} (Terrain Blending's mask view {})",
				state.alphaBlendMode, writeMode, state.alphaBlendAlphaToCoverage, pass->geometry && pass->geometry->name.c_str() ? pass->geometry->name.c_str() : "?", masks,
				static_cast<const void*>(t55), static_cast<const void*>(terrain.terrainDepth.depthSRV));
			if (t55)
				t55->Release();
		}
	}

	void InstallFeature()
	{
		const auto& o = Observe();
		// (Main::Draw's early hook: Features/TreeLod's audit subscribes from its install.)
		if (o.claims)
			Subscribe<FramePoint::ClaimsInstalled>([](const On<FramePoint::ClaimsInstalled>&) { ObservePlayerClaims(); });
		// (Prepass: the occlusion maps' compare subscribes from Features/OcclusionMaps' install.)
		if (o.capture)
			Subscribe<FramePoint::PrepassEnd>([](const On<FramePoint::PrepassEnd>&) { CaptureParity::Get().Report(Scene::FrameView::Get().GetFrame(), 300); });
		// The native lighting draws: the capture parity (the capture point's is the draws area's).
		if (o.capture)
			Subscribe<NativeLightingDraw>([](const NativeLightingDraw& a_event) {
				// Inside a reflection face render (an OwnRenderers view's: Features/Reflection's face render hook says so).
				if (PassCapture::Get().OwnViewRendering())
					CaptureParity::Get().OnFaceLightingDraw(a_event.pass, a_event.renderFlags);
				else
					CaptureParity::Get().OnNativeLightingDraw(a_event.pass, a_event.renderFlags);
			});
		// Every run: the frame report's leak line.
		Subscribe<NativePassOffered>(&NoteLeak);
		if (!o.targetProbe.empty())
			Subscribe<NativePassDrawn>(&DumpNativeBlend);
		// CS_DCLF_TARGET_DUMP: the frame's points, and at Present the count advances and finished copies are written.
		if (!SwitchValue(Switch::TargetDump).empty()) {
			Subscribe<TargetPoint>([](const TargetPoint& a_point) { TargetDump::Point(a_point.label, a_point.extra); });
			Subscribe<FramePoint::Present>([](const On<FramePoint::Present>&) { TargetDump::Present(); });
		}
	}
}
