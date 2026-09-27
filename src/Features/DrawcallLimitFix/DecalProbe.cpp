#include "DecalProbe.h"

#include "EngineStates.h"
#include "Records.h"
#include "SceneStore.h"
#include "Switches.h"

#include <algorithm>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "Deferred.h"
#include "State.h"

namespace DCLF
{
	namespace
	{
		constexpr std::uint64_t Bit(RE::BSShaderProperty::EShaderPropertyFlag8 a_flag)
		{
			return std::uint64_t(1) << static_cast<std::uint32_t>(a_flag);
		}

		// One observed combination, packed so it can be a map key and unpacked for the report.
		struct Observed
		{
			std::uint8_t hint = 0;
			std::uint8_t alphaState = 0;  // bit 0 alpha testing, bit 1 alpha blending (NiAlphaProperty)
			std::uint8_t zWrite = 0;
			std::uint8_t zTest = 0;
			std::uint8_t twoSided = 0;
			std::uint8_t depthMode = 0;
			std::uint8_t cullMode = 0;
			std::uint8_t biasMode = 0;
			std::uint8_t blendMode = 0;
			std::uint8_t alphaToCoverage = 0;
			std::uint8_t writeMode = 0;
			std::uint8_t extra = 0;
			std::uint8_t alphaTestEnabled = 0;
			std::uint8_t fillMode = 0;
			std::uint32_t renderFlags = 0;

			std::uint64_t Key() const
			{
				std::uint64_t key = renderFlags;
				const std::uint8_t bytes[] = { hint, alphaState, zWrite, zTest, twoSided, depthMode, cullMode, biasMode,
					blendMode, alphaToCoverage, writeMode, extra, alphaTestEnabled, fillMode };
				for (std::size_t i = 0; i < std::size(bytes); ++i)
					key = key * 31 + bytes[i];
				return key;
			}
		};

		const char* BlendName(D3D11_BLEND a_blend)
		{
			switch (a_blend) {
			case D3D11_BLEND_ZERO: return "0";
			case D3D11_BLEND_ONE: return "1";
			case D3D11_BLEND_SRC_COLOR: return "srcC";
			case D3D11_BLEND_INV_SRC_COLOR: return "1-srcC";
			case D3D11_BLEND_SRC_ALPHA: return "srcA";
			case D3D11_BLEND_INV_SRC_ALPHA: return "1-srcA";
			case D3D11_BLEND_DEST_ALPHA: return "dstA";
			case D3D11_BLEND_INV_DEST_ALPHA: return "1-dstA";
			case D3D11_BLEND_DEST_COLOR: return "dstC";
			case D3D11_BLEND_INV_DEST_COLOR: return "1-dstC";
			case D3D11_BLEND_SRC_ALPHA_SAT: return "srcAsat";
			case D3D11_BLEND_BLEND_FACTOR: return "factor";
			case D3D11_BLEND_INV_BLEND_FACTOR: return "1-factor";
			case D3D11_BLEND_SRC1_COLOR: return "src1C";
			case D3D11_BLEND_INV_SRC1_COLOR: return "1-src1C";
			case D3D11_BLEND_SRC1_ALPHA: return "src1A";
			case D3D11_BLEND_INV_SRC1_ALPHA: return "1-src1A";
			default: return "?";
			}
		}

		const char* OpName(D3D11_BLEND_OP a_op)
		{
			switch (a_op) {
			case D3D11_BLEND_OP_ADD: return "add";
			case D3D11_BLEND_OP_SUBTRACT: return "sub";
			case D3D11_BLEND_OP_REV_SUBTRACT: return "rsub";
			case D3D11_BLEND_OP_MIN: return "min";
			case D3D11_BLEND_OP_MAX: return "max";
			default: return "?";
			}
		}
	}

	struct DecalProbe::Impl
	{
		ankerl::unordered_dense::map<std::uint64_t, std::pair<Observed, std::uint32_t>> observed;
		ankerl::unordered_dense::set<std::uint32_t> rasterLogged;  // (fill, cull, bias, scissor) packed
		ankerl::unordered_dense::set<std::uint32_t> blendLogged;   // (mode, atc, write, extra) packed
		std::uint32_t draws = 0;
		std::uint32_t offeredDepth = 0;   // decal passes (hint 2 or 3) offered to the thunks in the depth pass
		std::uint32_t offeredOpaque = 0;  // ... in the opaque pass
		std::uint32_t offeredOther = 0;   // decal passes offered outside both (shadows, reflections)
		// State parity: for a decal DCLF has in its tables, the state indices its pipeline key derived
		// against the ones the native draw was issued with. Nonzero mismatches mean the decal pass would
		// draw with the wrong fixed-function state; the first one is logged in full.
		std::uint32_t stateChecked = 0;
		std::uint32_t stateMismatched = 0;
		std::uint32_t stateMismatchedFading = 0;  // of which the decal was fading (the native loop's, PassCapture)
		bool stateLogged = false;

		void LogRaster(const Observed& a_seen)
		{
			const std::uint32_t packed = (a_seen.fillMode << 16) | (a_seen.cullMode << 8) | a_seen.biasMode;
			if (!rasterLogged.insert(packed).second)
				return;
			if (a_seen.fillMode >= 2 || a_seen.cullMode >= 3 || a_seen.biasMode >= 12) {
				logger::warn("[DCLF] decal probe: rasterizer indices out of range (fill {}, cull {}, bias {})", a_seen.fillMode, a_seen.cullMode, a_seen.biasMode);
				return;
			}
			auto* state = EngineRasterStates()[a_seen.fillMode][a_seen.cullMode][a_seen.biasMode][0];
			if (!state) {
				logger::warn("[DCLF] decal probe: no rasterizer state at [{}][{}][{}][0]", a_seen.fillMode, a_seen.cullMode, a_seen.biasMode);
				return;
			}
			D3D11_RASTERIZER_DESC desc{};
			state->GetDesc(&desc);
			logger::info("[DCLF] decal probe: rasterizer [fill {}][cull {}][bias {}]: DepthBias {}, DepthBiasClamp {}, SlopeScaledDepthBias {}, CullMode {}, DepthClip {}",
				a_seen.fillMode, a_seen.cullMode, a_seen.biasMode, desc.DepthBias, desc.DepthBiasClamp, desc.SlopeScaledDepthBias,
				static_cast<int>(desc.CullMode), desc.DepthClipEnable);
		}

		void LogBlend(const Observed& a_seen)
		{
			const std::uint32_t packed = (a_seen.blendMode << 24) | (a_seen.alphaToCoverage << 16) | (a_seen.writeMode << 8) | a_seen.extra;
			if (!blendLogged.insert(packed).second)
				return;
			if (a_seen.blendMode >= 7 || a_seen.alphaToCoverage >= 2 || a_seen.writeMode >= 13 || a_seen.extra >= 2) {
				logger::warn("[DCLF] decal probe: blend indices out of range (mode {}, atc {}, write {}, extra {})", a_seen.blendMode, a_seen.alphaToCoverage, a_seen.writeMode, a_seen.extra);
				return;
			}
			auto* state = EngineBlendStates()[a_seen.blendMode][a_seen.alphaToCoverage][a_seen.writeMode][a_seen.extra];
			if (!state) {
				logger::warn("[DCLF] decal probe: no blend state at [{}][{}][{}][{}]", a_seen.blendMode, a_seen.alphaToCoverage, a_seen.writeMode, a_seen.extra);
				return;
			}
			D3D11_BLEND_DESC desc{};
			state->GetDesc(&desc);
			std::string targets;
			const std::uint32_t count = desc.IndependentBlendEnable ? 8u : 1u;
			for (std::uint32_t i = 0; i < count; ++i) {
				const auto& rt = desc.RenderTarget[i];
				targets += fmt::format(" rt{}[{}{} {}*{} {} {}*{} mask {:X}]", i, rt.BlendEnable ? "blend " : "", rt.BlendEnable ? "" : "off",
					BlendName(rt.SrcBlend), BlendName(rt.DestBlend), OpName(rt.BlendOp), BlendName(rt.SrcBlendAlpha), BlendName(rt.DestBlendAlpha),
					rt.RenderTargetWriteMask);
			}
			logger::info("[DCLF] decal probe: blend [mode {}][atc {}][write {}][extra {}] ({}, {}):{}",
				a_seen.blendMode, a_seen.alphaToCoverage, a_seen.writeMode, a_seen.extra,
				desc.IndependentBlendEnable ? "independent" : "shared", desc.AlphaToCoverageEnable ? "a2c on" : "a2c off", targets);
		}
	};

	bool DecalProbe::Enabled()
	{
		static const bool enabled = SwitchEnabled("CS_DCLF_DECAL_PROBE");
		return enabled;
	}

	DecalProbe& DecalProbe::Get()
	{
		static DecalProbe probe;
		return probe;
	}

	DecalProbe::DecalProbe() :
		impl(std::make_unique<Impl>()) {}

	DecalProbe::~DecalProbe() = default;

	void DecalProbe::OnNativeLightingDraw(const RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags)
	{
		if (!a_pass || !a_pass->geometry || !a_pass->shaderProperty || !globals::deferred->deferredPass)
			return;
		const std::uint64_t f = a_pass->shaderProperty->flags.underlying();
		// Decals, and the engine's blended fading group (hint 9), whose state decides whether DCLF's decal
		// pass could draw it too.
		if (!(f & (Bit(RE::BSShaderProperty::EShaderPropertyFlag8::kDecal) | Bit(RE::BSShaderProperty::EShaderPropertyFlag8::kDynamicDecal))) &&
			a_pass->accumulationHint != 9)
			return;
		const auto& state = globals::game::shadowState->GetRuntimeData();
		Observed seen{};
		seen.hint = a_pass->accumulationHint;
		if (const auto* alpha = a_pass->geometry->GetGeometryRuntimeData().alphaProperty.get())
			seen.alphaState = (alpha->GetAlphaTesting() ? 1u : 0u) | (alpha->GetAlphaBlending() ? 2u : 0u);
		seen.zWrite = (f & Bit(RE::BSShaderProperty::EShaderPropertyFlag8::kZBufferWrite)) ? 1 : 0;
		seen.zTest = (f & Bit(RE::BSShaderProperty::EShaderPropertyFlag8::kZBufferTest)) ? 1 : 0;
		seen.twoSided = (f & Bit(RE::BSShaderProperty::EShaderPropertyFlag8::kTwoSided)) ? 1 : 0;
		seen.depthMode = static_cast<std::uint8_t>(state.depthStencilDepthMode);
		seen.cullMode = static_cast<std::uint8_t>(state.rasterStateCullMode);
		seen.biasMode = static_cast<std::uint8_t>(state.rasterStateDepthBiasMode);
		seen.blendMode = static_cast<std::uint8_t>(state.alphaBlendMode);
		seen.alphaToCoverage = static_cast<std::uint8_t>(state.alphaBlendAlphaToCoverage);
		seen.writeMode = static_cast<std::uint8_t>(state.alphaBlendWriteMode);
		seen.extra = static_cast<std::uint8_t>(state.alphaBlendModeExtra);
		seen.alphaTestEnabled = state.alphaTestEnabled ? 1 : 0;
		seen.fillMode = static_cast<std::uint8_t>(state.rasterStateFillMode);
		seen.renderFlags = a_renderFlags;
		auto [it, inserted] = impl->observed.try_emplace(seen.Key(), seen, 0u);
		++it->second.second;
		++impl->draws;
		impl->LogRaster(seen);
		impl->LogBlend(seen);

		// State parity against the tables (CS_DCLF_DECALS=1 with ownership off, so the native draw still
		// happens and can be compared with).
		const auto& store = SceneStore::Get();
		const auto index = store.FindObject(a_pass->geometry);
		if (index < 0)
			return;
		const auto& tables = store.GetTables();
		const auto& object = tables.objects[static_cast<std::size_t>(index)];
		if (!(object.flags & kObjectDecal) || object.pipelineIndex >= tables.pipelines.size())
			return;
		const std::uint32_t flags = tables.pipelines[object.pipelineIndex].rasterFlags;
		++impl->stateChecked;
		const bool same = RasterDepthBiasMode(flags) == seen.biasMode && RasterBlendMode(flags) == seen.blendMode &&
		                  RasterWriteMode(flags) == seen.writeMode && ((flags & kRasterAlphaToCoverage) != 0) == (seen.alphaToCoverage != 0) &&
		                  ((flags & kRasterBlendExtra) != 0) == (seen.extra != 0) && ((object.flags & kObjectAlphaTest) != 0) == (seen.alphaTestEnabled != 0);
		if (same)
			return;
		++impl->stateMismatched;
		if (const auto* fadeNode = a_pass->shaderProperty->fadeNode; fadeNode && fadeNode->GetRuntimeData().currentFade < 1.0f)
			++impl->stateMismatchedFading;
		if (!impl->stateLogged) {
			impl->stateLogged = true;
			logger::warn("[DCLF] decal probe: state MISMATCH for '{}' (hint {}): derived bias {} blend {} write {} alphaTest {} against native bias {} blend {} write {} alphaTest {} (zWrite {}, alpha {})",
				a_pass->geometry->name.c_str() ? a_pass->geometry->name.c_str() : "?", seen.hint, RasterDepthBiasMode(flags), RasterBlendMode(flags), RasterWriteMode(flags),
				(object.flags & kObjectAlphaTest) ? 1 : 0, seen.biasMode, seen.blendMode, seen.writeMode, seen.alphaTestEnabled, seen.zWrite, seen.alphaState);
		}
	}

	void DecalProbe::OnPassOffered(const RE::BSRenderPass* a_pass, bool a_inDepthPass)
	{
		if (!a_pass || (a_pass->accumulationHint != 2 && a_pass->accumulationHint != 3))
			return;
		if (a_inDepthPass)
			++impl->offeredDepth;
		else if (globals::deferred->deferredPass)
			++impl->offeredOpaque;
		else
			++impl->offeredOther;
	}

	void DecalProbe::Report(std::uint32_t a_frame, std::uint32_t a_interval)
	{
		if ((a_frame % a_interval) != 0)
			return;
		const double frames = static_cast<double>(std::max(1u, a_interval));
		std::vector<std::pair<Observed, std::uint32_t>> sorted;
		sorted.reserve(impl->observed.size());
		for (const auto& [key, value] : impl->observed)
			sorted.push_back(value);
		std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
		logger::info("[DCLF] decal probe: {:.1f} native decal draws a frame in the deferred pass; hint-2/3 passes offered to the thunks: {:.1f} depth, {:.1f} opaque, {:.1f} elsewhere a frame; state parity: {} checked, {} mismatched ({} of them fading, which the native loop draws){}",
			impl->draws / frames, impl->offeredDepth / frames, impl->offeredOpaque / frames, impl->offeredOther / frames,
			impl->stateChecked, impl->stateMismatched, impl->stateMismatchedFading,
			impl->stateMismatched > impl->stateMismatchedFading ? " <- STATE MISMATCH" : "");
		for (const auto& [seen, count] : sorted) {
			logger::info("[DCLF] decal probe: {:.1f}/frame hint {} alpha {} zWrite {} zTest {} twoSided {} flags {:X} | depth {} cull {} bias {} blend {} a2c {} write {} extra {} alphaTest {} fill {}",
				count / frames, seen.hint, seen.alphaState == 0 ? "none" : seen.alphaState == 1 ? "test" : seen.alphaState == 2 ? "blend" : "test+blend",
				seen.zWrite, seen.zTest, seen.twoSided, seen.renderFlags, seen.depthMode, seen.cullMode, seen.biasMode, seen.blendMode,
				seen.alphaToCoverage, seen.writeMode, seen.extra, seen.alphaTestEnabled, seen.fillMode);
		}
		impl->observed.clear();
		impl->draws = impl->offeredDepth = impl->offeredOpaque = impl->offeredOther = 0;
		impl->stateChecked = impl->stateMismatched = impl->stateMismatchedFading = 0;
	}
}
