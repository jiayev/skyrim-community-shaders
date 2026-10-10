#include "GeometryPort.h"

#include "Features/DrawcallLimitFix/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Scene/LightingDescriptors.h"
#include "LightingConstants.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <type_traits>

#include "Globals.h"

namespace DCLF::GeometryPort
{
	namespace
	{
		using Engine::At;
		using Engine::Global;

		static_assert(std::is_same_v<FrameLighting, std::array<float, 24>>);

		// AE 1.6.1170, module offsets (SetupGeometry 0x1414dd040 reads each).
		constexpr std::uintptr_t kVec3NormalizeImport = 0x17502e0;  // D3DXVec3Normalize's IAT slot (the thunk at 0x14153d3da)
		constexpr std::uintptr_t kImageSpaceManager = 0x332a6a0;    // ImageSpaceManager*; the HDR multiplier at +0xe0
		constexpr std::size_t kImageSpaceHdr = 0xe0;
		constexpr std::size_t kBSLightNiLight = 0x48;     // BSLight -> NiLight*
		constexpr std::size_t kNiLightDiffuse = 0x11c;    // NiColor
		constexpr std::size_t kNiLightFade = 0x134;
		constexpr std::size_t kNiLightDirection = 0x140;  // NiDirectionalLight's world direction
		constexpr std::uintptr_t kDirectionalAmbient = 0x2033128;  // BSShaderManager::State+0xc8
		constexpr std::uintptr_t kCurrentAccumulator = 0x332a3a8;  // FUN_141480b20's
		constexpr std::size_t kAccumulatorEye = 0x16c;
		constexpr std::uintptr_t kPosAdjust = 0x202aecc;  // RendererShadowState (0x14202ab70) +0x35c
		constexpr std::uintptr_t kSsrX = 0x2034e80;       // Setting values (ConfigureR)
		constexpr std::uintptr_t kSsrY = 0x2034e98;
		constexpr std::uintptr_t kSsrAuxiliary = 0x338ca08;
		constexpr std::uintptr_t kWorldMapVS = 0x332a4f0;
		constexpr std::uintptr_t kWorldMapPS = 0x332a500;
		constexpr std::uintptr_t kAmbientSpecularGlobal = 0x203315c;  // BSShaderManager::State+0xfc: tint xyz, Fresnel power w

		/**
		 * @brief Whether SSE Engine Fixes' BSLightingAmbientSpecular fix is in SetupGeometry (EngineFixesSkyrim64,
		 * src/fixes/bslightingambientspecular.h): a jmp5 over SetupGeometry's Specular test (`test dword [r13+0x94], 0x200`, 41 F7 85
		 * 94 00 00 00 00 02 00 00; in AE 1.6.1170 at 0x1414de2b1, SetupGeometry +0x1271: the offset a fix build uses varies) into code
		 * that, for an AmbientSpecular pass, copies the 16 bytes at BSShaderManager::State's ambient specular (0x14203315c) into
		 * PerGeometry PS 6. Found by the instruction itself: the test whole, the fix absent; the jmp5 and the test's last six bytes,
		 * present. Fixed after load (SKSE plugins patch at load).
		 */
		bool AmbientSpecularFixInstalled()
		{
			static const bool installed = [] {
				constexpr std::uint8_t kTest[] = { 0x41, 0xf7, 0x85, 0x94, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00 };
				constexpr std::size_t kScan = 0x1600;  // SetupGeometry's length
				const auto* code = reinterpret_cast<const std::uint8_t*>(REL::RelocationID(100565, 107300).address());
				int vanilla = 0, patched = 0;
				for (std::size_t i = 0; i + sizeof(kTest) <= kScan; ++i) {
					if (std::memcmp(code + i, kTest, sizeof(kTest)) == 0)
						++vanilla;
					else if (code[i] == 0xE9 && std::memcmp(code + i + 5, kTest + 5, sizeof(kTest) - 5) == 0)
						++patched;
				}
				logger::info("[DCLF] Engine Fixes' ambient specular in SetupGeometry: {} ({} Specular tests whole, {} patched)",
					patched ? "installed" : "absent", vanilla, patched);
				return patched > 0;
			}();
			return installed;
		}

		// Lighting variable indices (ShaderConstants::LightingVS / LightingPS) LightingConstants.h does not name.
		constexpr std::uint32_t kVSWorldMapOverlay = 8;
		constexpr std::uint32_t kPSDirLightColor = 4;
		constexpr std::uint32_t kPSDirectionalAmbient = 5;
		constexpr std::uint32_t kPSWorldMapOverlay = 17;
		constexpr std::uint32_t kPSAmbientSpecular = 6;

		// Pass descriptor flags (ShaderCache.h LightingShaderFlags).
		constexpr std::uint32_t kTruePbr = 1u << 3;
		constexpr std::uint32_t kSpecular = 1u << 9;
		constexpr std::uint32_t kAnisoLighting = 1u << 16;
		constexpr std::uint32_t kAmbientSpecular = 1u << 17;
		constexpr std::uint32_t kWorldMap = 1u << 18;
		constexpr std::uint32_t kEyeFlags = 0x21c00u;  // SoftLighting, RimLighting, BackLighting, AmbientSpecular

		// The template pass's numLights: the sun alone (SceneStore::TemplatePassOf), as the engine's main passes carry it.
		constexpr int kTemplateLights = 1;

		/**
		 * @brief The descriptor SetupGeometry reads at BSLightingShader+0x94 for a pass descriptor: SetupTechnique's
		 * (SetupTechniqueDescriptor), then True PBR's SetupGeometry hook (TruePBR.cpp): a TruePbr pass gains AmbientSpecular and flips
		 * AnisoLighting, and bits 3-5 become min(numLights - 1, 7), the template pass's 0.
		 */
		std::uint32_t RawTechniqueOf(std::uint32_t a_passDescriptor)
		{
			std::uint32_t raw = SetupTechniqueDescriptor(a_passDescriptor);
			if (raw & kTruePbr) {
				raw |= kAmbientSpecular;
				raw ^= kAnisoLighting;
			}
			raw &= ~0x38u;
			raw |= static_cast<std::uint32_t>(std::min(kTemplateLights - 1, 7)) << 3;
			return raw;
		}

		/** @brief Whether SetupGeometry writes EyePosition for a raw technique (its bVar13): Envmap, MultilayerParallax, Eye, Specular, 0x21c00. */
		bool WritesEye(std::uint32_t a_raw)
		{
			const std::uint32_t technique = (a_raw >> 24) & 0x3f;
			return technique == 1 || technique == 0xb || technique == 0x10 || (a_raw & kSpecular) || (a_raw & kEyeFlags);
		}

		float* VariableAt(ConstantBlock& a_block, const StageLayout& a_layout, std::uint32_t a_variable)
		{
			return &a_block.floats[a_layout.offset[a_variable]];
		}
	}

	bool PipelineFrame::SamePipelineInputs(const PipelineFrame& a_other) const
	{
		return std::memcmp(ssrParams, a_other.ssrParams, sizeof(ssrParams)) == 0 && std::memcmp(worldMapVS, a_other.worldMapVS, sizeof(worldMapVS)) == 0 &&
		       std::memcmp(worldMapPS, a_other.worldMapPS, sizeof(worldMapPS)) == 0;
	}

	PipelineFrame SamplePipelineFrame()
	{
		EngineReadWindow::Touch("SamplePipelineFrame");
		PipelineFrame frame;

		// The sun: ShadowSceneNode[0]->sunLight, the template pass's sceneLights[0]; SetupGeometry reads it through BSLight+0x48.
		auto* sceneNode = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
		const RE::BSLight* sun = sceneNode ? sceneNode->GetRuntimeData().sunLight : nullptr;
		const auto* light = sun ? At<const std::byte* const>(sun, kBSLightNiLight) : nullptr;
		const auto* imageSpace = Global<const std::byte* const>(kImageSpaceManager);
		if (light && imageSpace) {
			frame.sun = true;
			// DirLightColor (0x1414dd5d5): the fade times the HDR multiplier, then each channel.
			const float scale = At<const float>(light, kNiLightFade) * At<const float>(imageSpace, kImageSpaceHdr);
			for (std::uint32_t c = 0; c < 3; ++c)
				frame.dirLightColor[c] = scale * At<const float>(light, kNiLightDiffuse + c * sizeof(float));
			// DirLightDirection (0x1414dd622): the direction negated (a sign flip, xorps 0x141ad3130), through the engine's own
			// D3DXVec3Normalize import, so the result is the same bits on this machine.
			const float negated[3] = { -At<const float>(light, kNiLightDirection), -At<const float>(light, kNiLightDirection + 4),
				-At<const float>(light, kNiLightDirection + 8) };
			using Vec3Normalize = float* (*)(float*, const float*);
			const auto normalize = Global<const Vec3Normalize>(kVec3NormalizeImport);
			normalize(frame.dirLightDirection, negated);
		}

		// DirectionalAmbient (0x1414dd957): row r is the 3x3's row r with the column's entry r (floats 9-11) as w.
		const float* ambient = &Global<const float>(kDirectionalAmbient);
		for (std::uint32_t r = 0; r < 3; ++r) {
			frame.directionalAmbient[r * 4 + 0] = ambient[r * 3 + 0];
			frame.directionalAmbient[r * 4 + 1] = ambient[r * 3 + 1];
			frame.directionalAmbient[r * 4 + 2] = ambient[r * 3 + 2];
			frame.directionalAmbient[r * 4 + 3] = ambient[9 + r];
		}

		// EyePosition (0x1414de12c): the current accumulator's eye less posAdjust, one subtraction a component.
		if (const auto* accumulator = Global<const std::byte* const>(kCurrentAccumulator)) {
			frame.eye = true;
			const float* posAdjust = &Global<const float>(kPosAdjust);
			for (std::uint32_t c = 0; c < 3; ++c)
				frame.eyePosition[c] = At<const float>(accumulator, kAccumulatorEye + c * sizeof(float)) - posAdjust[c];
		}

		// SSRParams.xyz (0x1414de283): x, then y + x (addss: the same bits either way round), then the auxiliary-target switch.
		frame.ssrParams[0] = Global<const float>(kSsrX);
		frame.ssrParams[1] = Global<const float>(kSsrY) + Global<const float>(kSsrX);
		frame.ssrParams[2] = Global<const float>(kSsrAuxiliary);

		// The world map rows (FUN_140e53b10: a float4 copy each).
		std::memcpy(frame.worldMapVS, &Global<const float>(kWorldMapVS), sizeof(frame.worldMapVS));
		std::memcpy(frame.worldMapPS, &Global<const float>(kWorldMapPS), sizeof(frame.worldMapPS));

		// AmbientSpecularTintAndFresnelPower: Engine Fixes' patch copies the 16 bytes as they are (movups).
		frame.ambientSpecularFix = AmbientSpecularFixInstalled();
		std::memcpy(frame.ambientSpecular, &Global<const float>(kAmbientSpecularGlobal), sizeof(frame.ambientSpecular));
		return frame;
	}

	bool PipelineGeometryConstants(std::uint32_t a_passDescriptor, [[maybe_unused]] std::uint32_t a_renderFlags, const PipelineFrame& a_frame, GeometryConstants& a_out)
	{
		a_out.vs.Reset();
		a_out.ps.Reset();
		if (!a_frame.sun)
			return false;
		const auto& vsLayout = LightingVSLayout();
		const auto& psLayout = LightingPSLayout();
		const std::uint32_t raw = RawTechniqueOf(a_passDescriptor);

		// The sun and the ambient, every pass (xyz of the two float3s; the w the engine leaves).
		std::memcpy(VariableAt(a_out.ps, psLayout, kPSDirLightColor), a_frame.dirLightColor, sizeof(a_frame.dirLightColor));
		std::memcpy(VariableAt(a_out.ps, psLayout, kPSDirLightDirection), a_frame.dirLightDirection, sizeof(a_frame.dirLightDirection));
		std::memcpy(VariableAt(a_out.ps, psLayout, kPSDirectionalAmbient), a_frame.directionalAmbient, sizeof(a_frame.directionalAmbient));

		// NumLightNumShadowLight.xy (0x1414dda7c): the raw technique's light counts, bits 3-5 (0: True PBR's hook) and 6-8 (the
		// descriptor's shadow lights). Then both point light arrays zeroed (0x70 bytes each: the layout's shared scratch). The point
		// light routine (FUN_1414df650, Light Limit Fix's replacement) runs only for a light count above 0, so ShadowLightMaskSelect
		// stays unwritten.
		float* counts = VariableAt(a_out.ps, psLayout, kPSNumLights);
		counts[0] = static_cast<float>(static_cast<std::int32_t>((raw >> 3) & 7));
		counts[1] = static_cast<float>(static_cast<std::int32_t>((raw >> 6) & 7));
		for (const std::uint32_t v : { kPSPointLightPosition, kPSPointLightColor })
			std::fill_n(VariableAt(a_out.ps, psLayout, v), psLayout.size[v], 0.0f);

		// EyePosition, xyz, for the passes that write it (with True PBR's AmbientSpecular: WritesEyePosition does not see that).
		if (WritesEye(raw) && a_frame.eye)
			std::memcpy(VariableAt(a_out.vs, vsLayout, kVSEyePosition), a_frame.eyePosition, sizeof(a_frame.eyePosition));

		// The world map rows (0x1414de0b1), WorldMap passes only.
		if (raw & kWorldMap) {
			std::memcpy(VariableAt(a_out.vs, vsLayout, kVSWorldMapOverlay), a_frame.worldMapVS, sizeof(a_frame.worldMapVS));
			std::memcpy(VariableAt(a_out.ps, psLayout, kPSWorldMapOverlay), a_frame.worldMapPS, sizeof(a_frame.worldMapPS));
		}

		// AmbientSpecularTintAndFresnelPower, all four, for AmbientSpecular passes (the raw technique's: True PBR's hook adds it), where
		// Engine Fixes' patch writes it.
		if ((raw & kAmbientSpecular) && a_frame.ambientSpecularFix)
			std::memcpy(VariableAt(a_out.ps, psLayout, kPSAmbientSpecular), a_frame.ambientSpecular, sizeof(a_frame.ambientSpecular));

		// SSRParams.xyz, every pass; w (the specular LOD fade) is the object's.
		std::memcpy(VariableAt(a_out.ps, psLayout, kPSSSRParams), a_frame.ssrParams, sizeof(a_frame.ssrParams));
		return true;
	}

	void MergeFrameLighting(const PipelineFrame& a_frame, std::array<float, 24>& a_out, std::uint32_t& a_written)
	{
		if (!a_frame.sun)
			return;
		auto put = [&](std::uint32_t a_index, float a_value) {
			if ((a_written >> a_index) & 1)
				return;
			a_out[a_index] = a_value;
			a_written |= 1u << a_index;
		};
		// FrameLighting's rows: DirLightDirection (0), DirLightColor (1), DirectionalAmbient (2-4), AmbientSpecularTintAndFresnelPower (5).
		for (std::uint32_t c = 0; c < 3; ++c) {
			put(c, a_frame.dirLightDirection[c]);
			put(4 + c, a_frame.dirLightColor[c]);
		}
		for (std::uint32_t i = 0; i < 12; ++i)
			put(8 + i, a_frame.directionalAmbient[i]);
		if (a_frame.ambientSpecularFix)
			for (std::uint32_t c = 0; c < 4; ++c)
				put(20 + c, a_frame.ambientSpecular[c]);
	}

	std::string Differences(const GeometryConstants& a_port, const GeometryConstants& a_evaluated, bool a_ignoreLightCount)
	{
		std::string out;
		for (std::uint32_t stage = 0; stage < 2; ++stage) {
			const auto& layout = stage ? LightingPSLayout() : LightingVSLayout();
			const std::uint64_t domain = stage ? kPSGroups[kPerGeometry] & ~kObjectPS : kVSGroups[kPerGeometry] & ~kObjectVS;
			const auto& a = stage ? a_port.ps : a_port.vs;
			const auto& b = stage ? a_evaluated.ps : a_evaluated.vs;
			for (std::uint32_t v = 0; v < layout.count; ++v) {
				if (!((domain >> v) & 1))
					continue;
				std::string values;
				bool differ = false;
				for (std::uint32_t c = 0; c < layout.size[v]; ++c) {
					if (stage && v == kPSSSRParams && c == 3)
						continue;  // the object's
					if (stage && v == kPSNumLights && c == 0 && a_ignoreLightCount)
						continue;
					const std::uint32_t at = layout.offset[v] + c;
					if (a.SameBits(b, at))
						continue;
					differ = true;
					if (values.size() < 96)
						values += fmt::format("{}[{}] {:#x}/{:#x}", values.empty() ? "" : " ", c, std::bit_cast<std::uint32_t>(a.floats[at]),
							std::bit_cast<std::uint32_t>(b.floats[at]));
				}
				if (differ)
					out += fmt::format("{}{}{} ({})", out.empty() ? "" : ", ", stage ? "PS" : "VS", v, values);
			}
		}
		return out;
	}
}
