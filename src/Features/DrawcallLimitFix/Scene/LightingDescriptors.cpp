#include "LightingDescriptors.h"

#include "ShaderCache.h"
#include "Features/DrawcallLimitFix/Common/Switches.h"
#include "Features/DrawcallLimitFix/Common/Toggles.h"
#include "State.h"
#include "TruePBR/BSLightingShaderMaterialPBR.h"
#include "Features/TerrainBlending.h"
#include "Features/DrawcallLimitFix/Draws/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Engine/ShadowViews.h"

namespace DCLF
{
	namespace
	{
		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
		using Technique = SIE::ShaderCache::LightingShaderTechniques;
		using LightingFlag = SIE::ShaderCache::LightingShaderFlags;

		constexpr std::uint64_t Bit(Flag a_flag) { return static_cast<std::uint64_t>(a_flag); }
		constexpr std::uint32_t Bit(LightingFlag a_flag) { return static_cast<std::uint32_t>(a_flag); }

		// Techniques DCLF supports. The rest stay native.
		bool IsSupportedTechnique(std::uint32_t a_technique)
		{
			switch (static_cast<Technique>(a_technique)) {
			case Technique::None:
			case Technique::Envmap:
			case Technique::Glowmap:
			case Technique::Parallax:
			// Parallax occlusion and multilayer parallax: SetupMaterial writes their constants and textures (ParallaxOccData,
			// MultiLayerParallaxData, the layer and envmap maps), which the material records take from the engine's own
			// evaluation; SetupTechnique's remap and samplers are SetupTechniqueDescriptor's and EvaluateTechnique's.
			case Technique::ParallaxOcc:
			case Technique::MultilayerParallax:
				return true;
			// A multi-index shape's ice or snow layer (kMultiIndexSnow with kProjectedUV): SparkleParams is SetupMaterial's, the
			// projected values the shape's own (RefreshObjectExtras), and the slot-10 texture SetupTechnique binds is replaced by
			// SetupGeometry's projected textures.
			case Technique::MultiIndexSparkle:
				return ActiveToggles().layers;
			case Technique::TreeAnim:
				return ActiveToggles().trees;
			// Actor skin (bodies, hands): the Lighting shader's SKIN path with TintColor, a PerMaterial constant.
			// NPC faces (Facegen: the tint and detail maps at t3/t4), hair (Hair: TintColor) and eyes (Eye: the eye
			// centres, material VS constants): everything they add is written by SetupMaterial, which the material
			// records take from the engine's own evaluation. A face part's positions are FaceSnapshots'.
			case Technique::FacegenRGBTint:
			case Technique::Facegen:
			case Technique::Hair:
			case Technique::Eye:
				return ActiveToggles().actors;
			case Technique::MTLand:
			case Technique::MTLandLODBlend:
				return MtLandEnabled();
			// Object LOD (dclf-lod.md): the LODOBJECTS / LODOBJECTSHD permutations, drawn by their visible segment ranges.
			// Reached only for a BSSubIndexTriShape with the LOD object flags (DeriveLightingDescriptors).
			case Technique::LODObjects:
			case Technique::LODObjectHD:
				return ActiveToggles().lodObjects;
			// Terrain LOD (dclf-lod.md): the LODLAND / LODLANDNOISE permutations. Reached only for a land block's shape under the
			// LOD root (IsLodLand).
			case Technique::LODLand:
			case Technique::LODLandNoise:
				return ActiveToggles().lodTerrain;
			default:
				return false;
			}
		}

		// [LightingShader] thresholds GetRenderPasses compares the fade node's LOD metric against
		// (engine notes: LOD fades). Defaults are the engine's.
		struct LodFadeSettings
		{
			float specularStart = 0.09f;
			float specularEnd = 0.10f;
			float envmapStart = 0.09f;
			float envmapEnd = 0.10f;
		};
		LodFadeSettings lodFade;

		float ReadSetting(const char* a_name, float a_default)
		{
			auto* setting = RE::GetINISetting(a_name);
			return setting ? setting->GetFloat() : a_default;
		}

		// FUN_14147c470: false when the feature has faded out; otherwise the fade factor in a_fade.
		// a_metric is the fade node's current LOD metric (+0x144); the previous one (+0x148) only
		// feeds the engine's render-pass cache invalidation, which DCLF does not need.
		bool LodFadeVisible(float a_metric, float a_start, float a_end, float& a_fade)
		{
			if (a_end < a_metric)
				return false;
			if (a_metric <= a_start) {
				a_fade = 1.0f;
			} else {
				a_fade = std::clamp((a_metric - a_end) / (a_start - a_end), 0.0f, 1.0f);
			}
			return true;
		}


		// BSLightingShader::SetupTechnique splits the pass descriptor (engine notes, SetupTechnique).
		constexpr std::uint32_t VertexDescriptorFromPass(std::uint32_t d)
		{
			return (d & 0x48007u) | ((d & 0x20a00u) ? 0x200u : 0u) | (d & 0x3f000000u);
		}

		// Community Shaders replaces the pixel split (TruePBR.cpp, BSLightingShader_GetPixelTechnique) so that
		// bits 3-5 (its TruePbr/Deferred flags) survive; only the shadow light count (bits 6-8) is dropped.
		constexpr std::uint32_t PixelDescriptorFromPass(std::uint32_t d)
		{
			std::uint32_t p = d & ~0x1c0u;
			if ((p & 0x4u) == 0)  // Skinned is kept only with ModelSpaceNormals
				p &= ~0x2u;
			return p | 1u;  // VC is always set
		}
	}

	std::uint8_t FadeStateOf(const RE::BSShaderProperty* a_property)
	{
		if (!a_property)
			return 0;
		const std::uint64_t f = a_property->flags.underlying();
		if (!(f & (Bit(Flag::kSpecular) | Bit(Flag::kMultiIndexSnow) | Bit(Flag::kEnvMap))))
			return 0;
		return a_property->fadeNode ? 0 : kFadeNoNode;
	}

	RE::BSShaderProperty* LayerPropertyOf(const RE::BSGeometry& a_geometry)
	{
		if (const_cast<RE::BSGeometry&>(a_geometry).GetType().get() != RE::BSGeometry::Type::kMultiIndexTriShape)
			return nullptr;
		const auto& data = static_cast<const RE::BSMultiIndexTriShape&>(a_geometry).GetMultiIndexTrishapeRuntimeData();
		if (!data.altIndexBuffer || !*reinterpret_cast<ID3D11Buffer* const*>(data.altIndexBuffer) || data.altPrimCount == 0)
			return nullptr;
		return data.additionalShaderProperty.get();
	}

	std::uint32_t StaticShadowBits(const RE::BSGeometry& a_geometry, bool a_settled, const RE::BSLightingShaderProperty* a_property)
	{
		using Engine::Global;
		constexpr std::uintptr_t kMainAccumulator = 0x338c830;     // BSShaderAccumulator*, render mode 0
		constexpr std::size_t kAccumulatorDeferredShadow = 0x178;  // byte: the accumulator draws the deferred shadow mask
		constexpr std::uintptr_t kNoSunShadowDir = 0x20330a4;      // byte: GetRenderPasses gives no pass ShadowDir
		constexpr std::uintptr_t kScreenDoorFades = 0x2033468;     // byte: screen-door fades are on
		const auto* lighting = a_property ? a_property : netimmerse_cast<const RE::BSLightingShaderProperty*>(a_geometry.GetGeometryRuntimeData().shaderProperty.get());
		const auto* accumulator = Global<std::uint8_t*>(kMainAccumulator);
		if (!lighting || !accumulator)
			return 0;
		const std::uint64_t flags = lighting->flags.underlying();
		const auto* fadeNode = lighting->fadeNode;
		// a_settled: fully faded in, whatever the node holds (a member's pass, whose fade is the GPU's).
		const float fade = fadeNode && !a_settled ? fadeNode->GetRuntimeData().currentFade : 1.0f;
		const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(lighting->material);
		const float alpha = (material ? material->materialAlpha : 1.0f) * fade;
		const auto* alphaProperty = a_geometry.GetGeometryRuntimeData().alphaProperty.get();
		const bool blended = alphaProperty && (alphaProperty->alphaFlags & 1);
		// GetRenderPasses: local_164, local_167, local_165/local_168 and local_158.
		const bool translucent = alpha < 1.0f || blended;
		const bool screenDoor = Global<std::uint8_t>(kScreenDoorFades) && fadeNode && fadeNode->GetRuntimeData().unk154 && fade < 1.0f &&
		                        !(flags & (1ull << 19));
		constexpr std::uint64_t kOpaqueClasses = 0x800c000000ull;
		const bool eligible = !translucent || (screenDoor && !blended) || (flags & kOpaqueClasses);
		bool deferred = accumulator[kAccumulatorDeferredShadow] != 0;
		if (!screenDoor || blended)
			deferred = deferred && !((alpha < 1.0f || blended || (flags & (1ull << 33))) && !(flags & kOpaqueClasses));
		bool shadowDir = deferred && eligible && !Global<std::uint8_t>(kNoSunShadowDir);
		bool defShadow = deferred;
		if (!(flags & 0x800c000100ull) && !lighting->shadowMapOrMaskPasses.head)
			shadowDir = defShadow = false;
		return (shadowDir ? 0x2000u : 0u) | (defShadow ? 0x4000u : 0u) | (defShadow && !shadowDir ? 0x40u : 0u);
	}

	LodFadeFrame SampleLodFadeFrame()
	{
		using Engine::Global;
		LodFadeFrame frame;
		if (const auto* camera = RE::Main::WorldRootCamera()) {
			frame.eye[0] = camera->world.translate.x;
			frame.eye[1] = camera->world.translate.y;
			frame.eye[2] = camera->world.translate.z;
			frame.lodAdjust = camera->GetRuntimeData2().lodAdjust;
		}
		frame.specularStart = lodFade.specularStart;
		frame.specularEnd = lodFade.specularEnd;
		frame.envmapStart = lodFade.envmapStart;
		frame.envmapEnd = lodFade.envmapEnd;
		frame.metricScale = Global<float>(0x1aa6300);
		frame.defaultScale = Global<float>(0x1ad2840);
		frame.metricOverride = Global<float>(0x332a254);
		frame.overridden = Global<float>(0x332a254) != Global<float>(0x1769578) ? 1.0f : 0.0f;
		for (std::uint32_t type = 0; type < 16; ++type)
			frame.divisors[type] = Global<float>(0x2032e00 + type * 4);
		frame.fadesOn = Global<std::uint8_t>(0x2032dfd) ? 1.0f : 0.0f;
		return frame;
	}

	std::array<float, 4> LodFadeNodeOf(const RE::BSShaderProperty* a_property)
	{
		if (!a_property || !a_property->fadeNode)
			return { 0.0f, 0.0f, 0.0f, -1.0f };
		const auto* node = a_property->fadeNode;
		const bool held = !(a_property->flags.underlying() & (Bit(Flag::kSpecular) | Bit(Flag::kMultiIndexSnow) | Bit(Flag::kEnvMap))) ||
		                  node->GetFlags().any(RE::NiAVObject::Flag::kIgnoreFade);
		const auto& center = node->worldBound.center;
		return { center.x, center.y, center.z, static_cast<float>(Engine::At<std::uint8_t>(node, 0x153) & 0xF) + (held ? kLodFadeHeld : 0.0f) };
	}

	float LodMetricOf(const LodFadeFrame& a_frame, const std::array<float, 4>& a_node)
	{
		// FUN_14147b110: the distance from the camera to the node's bound centre, scaled by lodAdjust over the LOD type's
		// divisor (the default scale when that is not positive), times the metric scale; or the override.
		if (a_frame.overridden != 0.0f)
			return a_frame.metricOverride;
		const float divisor = a_frame.divisors[static_cast<std::uint32_t>(a_node[3]) & 0xF];
		const float scale = divisor > 0.0f ? a_frame.lodAdjust / divisor : a_frame.defaultScale;
		const float dx = a_node[0] - a_frame.eye[0], dy = a_node[1] - a_frame.eye[1], dz = a_node[2] - a_frame.eye[2];
		return std::sqrt(dx * dx + dy * dy + dz * dz) * scale * a_frame.metricScale;
	}

	float LodFadeAt(float a_metric, float a_start, float a_end)
	{
		float fade = 0.0f;
		return LodFadeVisible(a_metric, a_start, a_end, fade) ? fade : 0.0f;
	}

	void RefreshLodFadeSettings()
	{
		lodFade.specularStart = ReadSetting("fSpecularLODFadeStart:LightingShader", 0.09f);
		lodFade.specularEnd = ReadSetting("fSpecularLODFadeEnd:LightingShader", 0.10f);
		lodFade.envmapStart = ReadSetting("fEnvmapLODFadeStart:LightingShader", 0.09f);
		lodFade.envmapEnd = ReadSetting("fEnvmapLODFadeEnd:LightingShader", 0.10f);
	}

	static_assert(kLightingPixelDeferred == static_cast<std::uint32_t>(SIE::ShaderCache::LightingShaderFlags::Deferred));

	void LightingShaderDescriptors(std::uint32_t a_pass, bool a_deferred, std::uint32_t& a_vertex, std::uint32_t& a_pixel)
	{
		uint vertex = VertexDescriptorFromPass(SetupTechniqueDescriptor(a_pass));
		uint pixel = PixelDescriptorFromPass(SetupTechniqueDescriptor(a_pass));
		// Forced, so the result does not depend on whether the deferred pass happens to be running; ModifyShaderLookup only adds the
		// bit, so a forward view's descriptor is the same without it.
		globals::state->ModifyShaderLookup(RE::BSShader::Type::Lighting, vertex, pixel, true);
		if (!a_deferred)
			pixel &= ~kLightingPixelDeferred;
		a_vertex = vertex;
		a_pixel = pixel;
	}

	std::uint32_t SetupTechniqueDescriptor(std::uint32_t a_pass)
	{
		const std::uint32_t technique = a_pass & 0x3f000000u;
		if (technique == 0x12000000u && !*reinterpret_cast<const std::uint8_t*>(REL::Offset(0x2032fdb).address()))
			return (a_pass & 0xc9ffffffu) | 0x9000000u;
		if (technique == 0x7000000u && !*reinterpret_cast<const std::uint8_t*>(REL::Offset(0x2035500).address()))
			return a_pass & 0xc0ffffffu;
		return a_pass;
	}

	std::uint32_t SelectLightingTechnique(std::uint64_t f)
	{
		// Same order as BSLightingShaderProperty::GetRenderPasses; later tests win.
		std::uint32_t t = (f & Bit(Flag::kEnvMap)) ? 1u : 0u;
		if (f & Bit(Flag::kGlowMap))
			t = 2;
		if ((f & (Bit(Flag::kParallax) | Bit(Flag::kParallaxOcclusion))) == Bit(Flag::kParallax))
			t = 3;
		if (f & Bit(Flag::kFace))
			t = 4;
		if (f & Bit(Flag::kFaceGenRGBTint))
			t = 5;
		if (f & Bit(Flag::kHairTint))
			t = 6;
		if (f & Bit(Flag::kParallaxOcclusion))
			t = 7;
		if (f & Bit(Flag::kMultiTextureLandscape))
			t = 8;
		if (f & Bit(Flag::kNoLODLandBlend))
			t = 19;
		if (f & Bit(Flag::kLODLandscape))
			t = 18;
		if (f & Bit(Flag::kLODObjects))
			t = 13;
		if (f & Bit(Flag::kHDLODObjects))
			t = 15;
		if (f & Bit(Flag::kMultiLayerParallax))
			t = 11;
		if (f & Bit(Flag::kTreeAnim))
			t = 12;
		if ((f & (Bit(Flag::kMultiIndexSnow) | Bit(Flag::kProjectedUV))) == (Bit(Flag::kMultiIndexSnow) | Bit(Flag::kProjectedUV)))
			t = 14;
		if (f & Bit(Flag::kEyeReflect))
			t = 16;
		return t;
	}

	namespace
	{
		// Globals BSLightingShader::SetupGeometry's TreeAnim case reads. Module-relative, like the other
		// engine addresses this feature depends on; a wrong one shows up at once as a capture-parity
		// mismatch on VS PerGeometry 4 or 5 rather than as a silent wrong value.
		// Resolved as addresses and read through, rather than as typed Relocations: these are plain data
		// globals, not the function or vtable pointers Relocation's accessors are shaped for.
		REL::Relocation<std::uintptr_t> treeWindFadeStart{ REL::Offset(0x2033100) };
		REL::Relocation<std::uintptr_t> treeWindFadeEnd{ REL::Offset(0x2033104) };
		REL::Relocation<std::uintptr_t> treeWindTimerScale{ REL::Offset(0x1ad28bc) };
		// A pointer to the object whose +0x304 holds the global wind magnitude.
		REL::Relocation<std::uintptr_t> treeWindSource{ REL::Offset(0x2033060) };
		// The tree manager (BSTreeManager*), whose +0x78 is the gust's speed and +0x80 the squared range of its update, and
		// the frame's seconds the manager advances the clocks by (FUN_140437d40's argument; GetSecondsPassed reads it).
		REL::Relocation<std::uintptr_t> treeManager{ REL::Offset(0x20f6a18) };
		REL::Relocation<std::uintptr_t> frameSeconds{ REL::Offset(0x31cc288) };
		// The fade node the engine treats as "no node" as well as null.
		REL::Relocation<std::uintptr_t> emptyFadeNode{ REL::Offset(0x332a2a0) };

		float GlobalFloat(const REL::Relocation<std::uintptr_t>& a_at)
		{
			return *reinterpret_cast<const float*>(a_at.address());
		}

		// BSFadeNode vtable slot 0x1f8/8: the downcast to BSTreeNode, null for anything that is not one.
		constexpr std::size_t kAsTreeNodeSlot = 0x1f8 / 8;

		const void* AsTreeNode(const RE::BSFadeNode* a_fadeNode)
		{
			if (!a_fadeNode || reinterpret_cast<std::uintptr_t>(a_fadeNode) == emptyFadeNode.address())
				return nullptr;
			const auto* vtable = *reinterpret_cast<const std::uintptr_t* const*>(a_fadeNode);
			using Fn = const void* (*)(const RE::BSFadeNode*);
			return reinterpret_cast<Fn>(vtable[kAsTreeNodeSlot])(a_fadeNode);
		}

		// The engine's own square root, reproduced bit for bit (engine notes: Func6, case 0xc).
		float FastSqrt(float a_value)
		{
			// The engine shifts the bit pattern as a SIGNED integer (`sar`). For every real distance the
			// two shifts agree; they differ on a negative pattern, which is what a fern's uninitialised
			// distance field holds, and where the engine's result then clamps to the maximum amplitude.
			const auto bits = std::bit_cast<std::int32_t>(a_value);
			const float estimate = std::bit_cast<float>(static_cast<std::uint32_t>(0x5f3759df - (bits >> 1)));
			return (1.5f - a_value * 0.5f * estimate * estimate) * estimate * a_value;
		}

		float TreeNodeFloat(const void* a_node, std::size_t a_offset)
		{
			return *reinterpret_cast<const float*>(static_cast<const std::byte*>(a_node) + a_offset);
		}

		// FUN_14147d640(node): the node's object (+0x118, through its vfunc 0x30), which FUN_140437e50 selects a tree for the near
		// list by: its +0x128, that one's +0x18, and an int at +0x10 of that equal to 1.
		REL::Relocation<std::uintptr_t (*)(const void*)> treeObjectOf{ REL::Offset(0x147d640) };

		bool NearListSelectable(const void* a_node)
		{
			const std::uintptr_t object = treeObjectOf(a_node);
			if (!object)
				return false;
			const auto first = *reinterpret_cast<const std::uintptr_t*>(object + 0x128);
			if (!first)
				return false;
			const auto second = *reinterpret_cast<const std::uintptr_t*>(first + 0x18);
			return second && *reinterpret_cast<const std::int32_t*>(second + 0x10) == 1;
		}
	}

	const void* TreeStaticOf(const RE::BSShaderProperty& a_property, TreeStatic& a_out)
	{
		a_out = {};
		const void* node = AsTreeNode(a_property.fadeNode);
		if (!node)
			return nullptr;
		const auto* bytes = static_cast<const std::byte*>(node);
		std::memcpy(a_out.position, bytes + 0xA0, sizeof(a_out.position));
		a_out.leafFrequency = TreeNodeFloat(node, 0x160);
		a_out.timer = TreeNodeFloat(node, 0x164);
		a_out.previousTimer = TreeNodeFloat(node, 0x168);
		a_out.amplitude = TreeNodeFloat(node, 0x15c);
		// The manager advances a tree node only in its near loop (FUN_1404381e0's loop 2, over FUN_140437e50's near list): a node
		// the near list selects (NearListSelectable), whose model (+0xF8, its +0x40) has bones (+0xB8). It scales the gust by that
		// model's +0xB0. Any other node keeps its own clock and amplitude: its model's +0xB0 is no amplitude the engine ever reads
		// (2.3e36 on trees whose leaves, gusted by it, covered the sun's cascades). Not replicated: the near list's cap on its
		// count (the nearest first).
		const auto* holder = *reinterpret_cast<const std::byte* const*>(bytes + 0xF8);
		const auto* model = holder ? *reinterpret_cast<const std::byte* const*>(holder + 0x40) : nullptr;
		const bool bones = model && *reinterpret_cast<const void* const*>(model + 0xB8);
		a_out.animated = bones && NearListSelectable(node) ? 1u : 0u;
		a_out.modelAmplitude = a_out.animated ? *reinterpret_cast<const float*>(model + 0xB0) : 0.0f;
		return node;
	}

	TreeWindFrame SampleTreeWindFrame()
	{
		TreeWindFrame frame;
		frame.deltaTime = GlobalFloat(frameSeconds);
		if (auto* camera = RE::Main::WorldRootCamera()) {
			frame.camera[0] = camera->world.translate.x;
			frame.camera[1] = camera->world.translate.y;
			frame.camera[2] = camera->world.translate.z;
		}
		if (const auto manager = *reinterpret_cast<const std::uintptr_t*>(treeManager.address())) {
			frame.windSpeed = *reinterpret_cast<const float*>(manager + 0x78);
			frame.maxDistance2 = *reinterpret_cast<const float*>(manager + 0x80);
		}
		frame.windMagnitude = *reinterpret_cast<const float*>(*reinterpret_cast<const std::uintptr_t*>(treeWindSource.address()) + 0x304);
		frame.fadeStart = GlobalFloat(treeWindFadeStart);
		frame.fadeEnd = GlobalFloat(treeWindFadeEnd);
		frame.timerScale = GlobalFloat(treeWindTimerScale);
		return frame;
	}

	void DeriveTreeAnim(const RE::BSShaderProperty& a_property, ObjectTreeAnim& a_out)
	{
		const void* node = AsTreeNode(a_property.fadeNode);

		a_out.treeParams[0] = 0.0f;
		a_out.treeParams[1] = *reinterpret_cast<const float*>(
			*reinterpret_cast<const std::uintptr_t*>(treeWindSource.address()) + 0x304);

		// Amplitude falls off with distance. The engine does NOT use sqrtf here: it takes the squared
		// distance at +0x158 through the 0x5f3759df fast inverse square root with one Newton step, and
		// multiplies back by x. Substituting std::sqrt agreed for every tree that clamps at one end of
		// the fade band or the other, and disagreed for the 600 draws a frame actually inside it - which
		// is exactly the population whose amplitude depends on the value rather than on the clamp.
		const float distance = node ? FastSqrt(TreeNodeFloat(node, 0x158)) : 0.0f;
		const float maxAmplitude = node ? TreeNodeFloat(node, 0x15c) : 1.0f;
		const float fadeStart = GlobalFloat(treeWindFadeStart);
		const float span = GlobalFloat(treeWindFadeEnd) - fadeStart;
		float amplitude = (1.0f - (distance - fadeStart) / span) * maxAmplitude;
		amplitude = std::max(amplitude, 0.0f);
		amplitude = std::min(amplitude, maxAmplitude);
		a_out.treeParams[2] = amplitude;
		a_out.treeParams[3] = node ? TreeNodeFloat(node, 0x160) : 1.0f;

		const float scale = GlobalFloat(treeWindTimerScale);
		a_out.windTimers[0] = node ? TreeNodeFloat(node, 0x164) * scale : 0.0f;
		a_out.windTimers[1] = node ? TreeNodeFloat(node, 0x168) * scale : 0.0f;
		// Diagnostics only (the shader reads xy): the raw inputs the amplitude came from, so a parity
		// mismatch on it can say whether the node or its distance was the thing that differed.
		a_out.windTimers[2] = node ? TreeNodeFloat(node, 0x158) : -1.0f;
		a_out.windTimers[3] = node ? TreeNodeFloat(node, 0x15c) : -1.0f;
	}

	bool TerrainBlendingDefersTerrain()
	{
		const auto& terrainBlending = globals::features::terrainBlending;
		return terrainBlending.loaded && terrainBlending.settings.Enabled;
	}

	bool MtLandEnabled()
	{
		return ActiveToggles().mtLand && !TerrainBlendingDefersTerrain();
	}

	namespace
	{
		/**
		 * @brief alphaBlendMode as BSShader's alpha setup (AE FUN_14150bc80) chooses it from a blending
		 * NiAlphaProperty's source and destination functions, or 0 when the pair is one it leaves alone.
		 *
		 * The function also picks mode 1 for a non-blending property when the shader property's alpha is
		 * below 1 (a fading object); that case is not eligible here, so it is not modelled.
		 */
		std::uint32_t AlphaBlendModeOf(const RE::NiAlphaProperty& a_alpha)
		{
			using Function = RE::NiAlphaProperty::AlphaFunction;
			const auto source = a_alpha.GetSrcBlendMode();
			const auto dest = a_alpha.GetDestBlendMode();
			if (source == Function::kSrcAlpha && dest == Function::kInvSrcAlpha)
				return 1;
			if ((source == Function::kSrcAlpha && dest == Function::kOne) || (source == Function::kOne && dest == Function::kOne) ||
				(source == Function::kSrcAlpha && dest == Function::kInvDestAlpha))
				return 2;
			if ((source == Function::kZero && dest == Function::kSrcColor) || (source == Function::kDestColor && dest == Function::kZero))
				return 4;
			if (source == Function::kDestColor && dest == Function::kInvSrcAlpha)
				return 3;
			return 0;
		}
	}

	Ineligible DeriveLightingDescriptors(const RE::BSLightingShaderProperty& a_property, const RE::BSGeometry& a_geometry,
		const AccumulatedPass* a_accumulated, LightingDescriptors& a_out, bool a_layer)
	{
		std::uint64_t f = a_property.flags.underlying();

		// Terrain LOD (a land block's BSTriShape) is DCLF's with its toggle; object LOD (a BSSubIndexTriShape: dclf-lod.md) with its
		// own, drawn by the ranges its hidden cells leave (LodSegments).
		if ((f & Bit(Flag::kLODLandscape)) && !(ActiveToggles().lodTerrain && IsLodLand(a_property, a_geometry)))
			return Ineligible::Lod;
		if ((f & (Bit(Flag::kLODObjects) | Bit(Flag::kHDLODObjects))) && !(ActiveToggles().lodObjects && IsLodObject(a_property, a_geometry)))
			return Ineligible::Lod;
		const auto* alpha = a_geometry.GetGeometryRuntimeData().alphaProperty.get();
		if (a_layer) {
			// A multi-index shape's layer (engine notes, "The main modes' registration"): every pass of its property goes into
			// geometry group 2 with hint 12, whatever its flags, and FUN_1414b3bb0 draws that group after the opaque decals
			// with the same state - depth test and write, bias mode 8 + b, blending off, write mode 10, render flags 0x41 (the
			// alpha property is not applied). DCLF's decal group 3, drawn between its opaque and blended groups.
			if (!ActiveToggles().decals)
				return Ineligible::Decal;
			a_out.decalGroup = 3;
			a_out.decalBlendMode = 0;
			a_out.decalWriteMode = 10;
		} else if (f & (Bit(Flag::kDecal) | Bit(Flag::kDynamicDecal))) {
			// Decals, from what the engine was measured doing with them (engine notes:
			// decals). A Lighting decal pass carries accumulation hint 2 or 3, and the hint is which
			// geometry group draws it and with what state:
			//   hint 2: depth test+write with the opaque decal bias, blending off, write mode 10, the main
			//           pass's render flags (0x41: the alpha property is NOT applied);
			//   hint 3: depth test only with the blended decal bias, render flags 0x45, so the alpha
			//           property IS applied - blend mode from its functions, alpha test from its flag -
			//           and write mode 1 when the property has kZBufferWrite (SetupGeometry forces it),
			//           11 otherwise (what the group function set).
			// Both test depth against everything opaque and neither occludes anything its host does not,
			// which is why DCLF draws them in a second pass without writing depth (IndirectDraws). Without
			// an accumulated pass (scene membership) the hint is GetRenderPasses' for the settled object:
			// 2 + (alpha < 1 or blending), with the material's alpha (the fade is the feedback's).
			if (!ActiveToggles().decals || !(f & Bit(Flag::kZBufferTest)))
				return Ineligible::Decal;
			const auto* decalMaterial = static_cast<const RE::BSLightingShaderMaterialBase*>(a_property.material);
			const std::uint32_t hint = a_accumulated ? a_accumulated->hint :
			                           ((decalMaterial && decalMaterial->materialAlpha < 1.0f) || (alpha && alpha->GetAlphaBlending())) ? 3u : 2u;
			if (hint == 2) {
				if (alpha && alpha->GetAlphaBlending())
					return Ineligible::Decal;  // not measured in this group; the state would be a guess
				a_out.decalGroup = 1;
				a_out.decalBlendMode = 0;
				a_out.decalWriteMode = 10;
			} else if (hint == 3) {
				// The fading case (alpha < 1 without blending) takes blend mode 1 in the engine and is
				// also what Ineligible::Fading covers; it is left to the native loop.
				if (!alpha || !alpha->GetAlphaBlending())
					return Ineligible::Decal;
				const std::uint32_t blendMode = AlphaBlendModeOf(*alpha);
				if (blendMode == 0)
					return Ineligible::Decal;  // a blend function pair the engine's setup leaves as it finds it
				a_out.decalGroup = 2;
				a_out.decalBlendMode = blendMode;
				a_out.decalWriteMode = (f & Bit(Flag::kZBufferWrite)) ? 1u : 11u;
			} else {
				return Ineligible::Decal;
			}
		}
		// kSkinned selects the SKINNED permutation, whose vertex shader wants a palette; with the switch on
		// the palette comes from the geometry's skin instance (SceneStore), so a skinned property without
		// one would draw from nothing and stays native.
		if ((f & Bit(Flag::kSkinned)) && !(ActiveToggles().skinned && a_geometry.GetGeometryRuntimeData().skinInstance))
			return Ineligible::Skinned;
		if ((f & Bit(Flag::kProjectedUV)) && !ActiveToggles().projectedUv)
			return Ineligible::ProjectedUV;
		// Terrain Blending holds these passes (a mesh opts out of the blend with this otherwise unused flag) and
		// redraws them after its terrain, testing EQUAL, so the terrain does not blend over them. DCLF draws
		// before that terrain (DrawcallLimitFix::AfterOpaquePass), so they stay native.
		if ((f & Bit(Flag::kNoTransparencyMultiSample)) && TerrainBlendingDefersTerrain())
			return Ineligible::TerrainNoBlend;
		// Refraction has its own LOD fade that removes the object entirely; leave it to the game.
		if (f & (Bit(Flag::kRefraction) | Bit(Flag::kTempRefraction))) {
			a_out.rejectedTechnique = kRefractionReject;
			return Ineligible::Technique;
		}

		// Blended geometry is drawn after the deferred composite, forward and sorted; the one exception is
		// the engine's blended decal group, which blends inside the G-buffer pass and is handled above.
		if (alpha && alpha->GetAlphaBlending() && a_out.decalGroup != 2 && !a_layer)
			return Ineligible::AlphaBlend;

		// The descriptor derived from the property, as GetRenderPasses builds it for an opaque object with its specular and
		// envmap LOD fades not run out: the draw fades those two by distance (LodFadeFrame), so the descriptor is the same
		// at every distance. Where the accumulator holds a pass, only its per-frame bits are taken from it (below).
		std::uint32_t derived = 0;
		const float specularFade = a_property.specularLODFade;
		const float envmapFade = a_property.envmapLODFade;
		const Ineligible derivedReason = [&] {
			// Without a fade node GetRenderPasses never computes the LOD fades, so the property fields the draw reads may be
			// stale. Leave such objects native rather than guess.
			if (!a_property.fadeNode && (f & (Bit(Flag::kSpecular) | Bit(Flag::kMultiIndexSnow) | Bit(Flag::kEnvMap))))
				return Ineligible::Fading;

			const std::uint32_t technique = SelectLightingTechnique(f);
			if (!IsSupportedTechnique(technique)) {
				a_out.rejectedTechnique = technique;
				return Ineligible::Technique;
			}

			std::uint32_t d = technique << 24;
			if (f & Bit(Flag::kVertexColors))
				d |= Bit(LightingFlag::VC);
			// GetRenderPasses copies the property's kSkinned straight into bit 1, whether or not the geometry has a
			// skin (static fish and buckets carry it).
			if (f & Bit(Flag::kSkinned))
				d |= Bit(LightingFlag::Skinned);
			// ProjectedUV is kProjectedUV itself; the snow conditions only add bits 19 and 21 on top (not derived).
			if (f & Bit(Flag::kProjectedUV))
				d |= Bit(LightingFlag::ProjectedUV);
			if (f & Bit(Flag::kModelSpaceNormals))
				d |= Bit(LightingFlag::ModelSpaceNormals);
			if (f & (Bit(Flag::kSpecular) | Bit(Flag::kMultiIndexSnow)))
				d |= Bit(LightingFlag::Specular);
			if (f & Bit(Flag::kSoftLighting))
				d |= Bit(LightingFlag::SoftLighting);
			if (f & Bit(Flag::kRimLighting))
				d |= Bit(LightingFlag::RimLighting);
			if (f & Bit(Flag::kBackLighting))
				d |= Bit(LightingFlag::BackLighting);
			if (f & Bit(Flag::kAnisotropicLighting))
				d |= Bit(LightingFlag::AnisoLighting);
			// GetRenderPasses (1414adfb0) gives every decal without kMultiIndexSnow DoAlphaTest, whatever its alpha property.
			if ((alpha && alpha->GetAlphaTesting()) ||
				((f & (Bit(Flag::kDecal) | Bit(Flag::kDynamicDecal))) && !(f & Bit(Flag::kMultiIndexSnow))))
				d |= Bit(LightingFlag::DoAlphaTest);
			if (f & Bit(Flag::kCharacterLighting))
				d |= Bit(LightingFlag::CharacterLight);

			// Community Shaders' GetRenderPasses hook (TruePBR.cpp): PBR materials swap Specular for TruePbr,
			// and glint turns on AnisoLighting.
			const auto* material = a_property.material;
			const bool isPbr = (f & Bit(Flag::kVertexLighting)) && material &&
			                   (material->GetFeature() == RE::BSShaderMaterial::Feature::kDefault ||
								   material->GetFeature() == RE::BSShaderMaterial::Feature::kMultiTexLandLODBlend);
			if (isPbr) {
				d |= Bit(LightingFlag::TruePbr);
				d &= ~Bit(LightingFlag::Specular);
				if (static_cast<const BSLightingShaderMaterialPBR*>(material)->glintParameters.enabled)
					d |= Bit(LightingFlag::AnisoLighting);
			}
			derived = d;
			return Ineligible::None;
		}();

		if (derivedReason != Ineligible::None)
			return derivedReason;
		a_out.derivedPass = derived;
		std::uint32_t d = derived;
		if (a_accumulated) {
			// The pass the accumulator registered gives only what the property does not: DoAlphaTest, the screen-door fade and the
			// snow bits (kRegisteredPassBits). The rest is the derivation's, so the technique does not change with the distance the
			// engine's LOD fades were taken at, and the shadow bits are StaticShadowBits', which the draw decides per frame.
			// A layer's pass has no shadow bits: ShadowDir is the property's light mask naming the sun, and the registrations write
			// masks on the main property alone (FUN_1414b2140), so DefShadow goes too (LayerShadowBits).
			d = (derived & ~kRegisteredPassBits) | (a_accumulated->technique & kRegisteredPassBits) | (a_layer ? 0u : StaticShadowBits(a_geometry, false, &a_property));
			// The screen-door fade: Lighting.hlsl discards against a 4x4 screen pattern and MaterialData.z, so
			// the object stays opaque and the Z-prepass (which keeps the alpha test) dithers identically.
			if ((d & Bit(LightingFlag::AdditionalAlphaMask)) && !ActiveToggles().fading)
				return Ineligible::Fading;
		}

		a_out.rawVertex = VertexDescriptorFromPass(SetupTechniqueDescriptor(d));
		a_out.rawPixel = PixelDescriptorFromPass(SetupTechniqueDescriptor(d));
		std::uint32_t vertex = 0, pixel = 0;
		LightingShaderDescriptors(d, true, vertex, pixel);

		a_out.technique = (d >> 24) & 0x3f;
		a_out.pass = d;
		a_out.projectedUV = (d & Bit(LightingFlag::ProjectedUV)) != 0;
		a_out.vertex = vertex;
		a_out.pixel = pixel;
		a_out.specularLODFade = specularFade;
		a_out.envmapLODFade = envmapFade;
		return Ineligible::None;
	}
}
