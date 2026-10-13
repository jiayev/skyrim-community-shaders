#include "Features/DrawcallLimitFix/Diagnostics/Install.h"

#include "Skylight.h"
#include "Features/DrawcallLimitFix/Runtime/Switches.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/CaptureParity.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Diagnostics/Policy.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"
#include "Deferred.h"
#include "Features/DrawcallLimitFix/Diagnostics/Feature/OpenDefectProbes.h"
#include "Features/DrawcallLimitFix/Diagnostics/Feature/TargetDump.h"
#include "Features/DrawcallLimitFix/Engine/Views/ReflectionFaces.h"
#include "Features/DrawcallLimitFix/Scene/Ports/LightingDescriptors.h"
#include "Features/DrawcallLimitFix/Scene/FrameView/FrameView.h"
#include "Features/TerrainBlending.h"

#include <ankerl/unordered_dense.h>

// The feature file's observers: the frame-level parities and probes the feature's hooks announce (Hooks.h's frame events).

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
			if (leaks.samples.size() < 8)
				leaks.samples.push_back(fmt::format("'{}' ({}, hint {}, technique {:08X})", pass->geometry->name.c_str() ? pass->geometry->name.c_str() : "?",
					a_offered.inDepthPass ? "depth" : "opaque", static_cast<std::uint32_t>(pass->accumulationHint), a_offered.technique));
		}

		/** @brief CS_DCLF_DRAW_CENSUS: the native draws by pass, shader type and technique, reported every 300 frames. */
		struct DrawCensus
		{
			ankerl::unordered_dense::map<std::uint64_t, std::uint32_t> counts;
			std::uint32_t frames = 0;

			void Note(const NativeDraw& a_draw) { ++counts[(std::uint64_t(a_draw.pass) << 48) | (std::uint64_t(a_draw.shaderType) << 40) | a_draw.technique]; }

			void Report()
			{
				if (Scene::FrameView::IsLoadingScreenUp() || ++frames < 300)
					return;
				std::vector<std::pair<std::uint32_t, std::uint64_t>> rows;
				std::uint64_t total = 0;
				for (const auto& [key, count] : counts) {
					rows.emplace_back(count, key);
					total += count;
				}
				std::sort(rows.begin(), rows.end(), std::greater<>());
				const auto passName = [](std::uint32_t a_pass) -> std::string {
					switch (a_pass) {
					case 0x100: return "depth prepass";
					case 0x101: return "deferred main";
					case 0xFF: return "no accumulator";
					default: return fmt::format("render mode {:#x}", a_pass);
					}
				};
				std::string text;
				for (std::size_t i = 0; i < rows.size() && i < 30; ++i) {
					const auto [count, key] = rows[i];
					const auto type = static_cast<std::uint32_t>((key >> 40) & 0xFF);
					const auto technique = static_cast<std::uint32_t>(key & 0xFFFFFFFFull);
					const auto typeName = type < magic_enum::enum_count<RE::BSShader::Type>() ? magic_enum::enum_name(static_cast<RE::BSShader::Type>(type)) : std::string_view("none");
					text += fmt::format("\n    {:7.1f}  {}, {} {}", static_cast<double>(count) / frames, passName(static_cast<std::uint32_t>(key >> 48)), typeName,
						type == static_cast<std::uint32_t>(RE::BSShader::Type::Lighting) ? std::string(LightingTechniqueName(technique)) : fmt::format("{:08X}", technique));
				}
				logger::info("[DCLF] draw census: {:.1f} native draws a frame over {} frames, {} groups; the largest:{}", static_cast<double>(total) / frames, frames, rows.size(), text);
				counts.clear();
				frames = 0;
			}
		};
		DrawCensus drawCensus;

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
		// Main::Draw's early hook, DCLF running or not.
		if (SwitchEnabled(Switch::TreeLodAudit))
			Subscribe<FrameBegin>([](const FrameBegin&) { AuditTreeLod(); });
		if (o.claims)
			Subscribe<ClaimsInstalled>([](const ClaimsInstalled&) { ObservePlayerClaims(); });
		// Prepass: the occlusion maps' compare (Skylight), then the open defects' probes, DCLF running or not.
		Skylight::Install();
		if (!SwitchValue(Switch::ShadowMaskProbe).empty())
			Subscribe<PrepassBegin>([](const PrepassBegin& a_event) { ProbeShadowMask(a_event.running); });
		if (SwitchEnabled(Switch::ShadowMapProbe))
			Subscribe<PrepassBegin>([](const PrepassBegin& a_event) { ProbeShadowMaps(a_event.running); });
		if (o.capture)
			Subscribe<PrepassEnd>([](const PrepassEnd& a_event) { CaptureParity::Get().Report(a_event.frame, 300); });
		// The native lighting draws: the capture parity (the capture point's is the draws area's).
		if (o.capture)
			Subscribe<NativeLightingDraw>([](const NativeLightingDraw& a_event) {
				if (ReflectionFaces::InFace())
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
			Subscribe<Present>([](const Present&) { TargetDump::Present(); });
		}
		if (SwitchEnabled(Switch::DrawCensus)) {
			Subscribe<NativeDraw>([](const NativeDraw& a_draw) { drawCensus.Note(a_draw); });
			Subscribe<Present>([](const Present&) { drawCensus.Report(); });
		}
	}
}
