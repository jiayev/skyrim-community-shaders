#include "ConstantEvaluator.h"

#include "Switches.h"

#include <array>
#include <cstring>
#include <string>

#include <ankerl/unordered_dense.h>

#include "FrameAnnotations.h"
#include "Features/Skin.h"
#include "State.h"

namespace DCLF
{
	namespace
	{
		constexpr std::size_t kSetupMaterialSlot = 4;  // BSShader vtable slots (engine notes)
		constexpr std::size_t kSetupGeometrySlot = 6;
		constexpr std::uint32_t kPerMaterial = 1;
		constexpr std::uint32_t kPerGeometry = 2;

		using ShadowStateData = std::remove_reference_t<decltype(RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData())>;

		StageLayout MakeLayout(std::uint32_t a_count, std::initializer_list<std::pair<std::uint32_t, std::uint32_t>> a_sizes,
			std::initializer_list<std::uint32_t> a_scratch)
		{
			StageLayout layout{};
			layout.count = a_count;
			layout.size.fill(4);
			for (auto [variable, size] : a_sizes)
				layout.size[variable] = static_cast<std::uint8_t>(size);
			// Variables nothing reads (the per-geometry point lights under Light Limit Fix) share one region at the end.
			constexpr std::uint32_t kScratchRegion = 228;
			std::uint32_t next = 0;
			for (std::uint32_t i = 0; i < a_count; ++i) {
				if (std::find(a_scratch.begin(), a_scratch.end(), i) != a_scratch.end()) {
					layout.offset[i] = static_cast<std::uint8_t>(kScratchRegion);
					continue;
				}
				layout.offset[i] = static_cast<std::uint8_t>(next);
				next += layout.size[i];
			}
			return layout;
		}

		template <class Shader>
		void PointAt(Shader* a_shader, const StageLayout& a_layout, ConstantBlock* a_blocks)
		{
			for (std::uint32_t level = 0; level < 3; ++level)
				a_shader->constantBuffers[level].data = a_blocks[level].floats.data();
			for (std::uint32_t i = 0; i < a_layout.count && i < a_shader->constantTable.size(); ++i)
				a_shader->constantTable[i] = a_layout.offset[i];
		}

		ID3D11ShaderResourceView* TextureSentinel(std::uint32_t a_slot)
		{
			return reinterpret_cast<ID3D11ShaderResourceView*>(static_cast<std::uintptr_t>(0xdc1f0000u + a_slot));
		}

		template <class Fn, class... Args>
		void CallVirtual(RE::BSShader* a_shader, std::size_t a_slot, Args... a_args)
		{
			const auto* vtable = *reinterpret_cast<const std::uintptr_t* const*>(a_shader);
			reinterpret_cast<Fn>(vtable[a_slot])(a_shader, a_args...);
		}
	}

	const StageLayout& LightingVSLayout()
	{
		// World, PreviousWorld and TextureProj are float3x4; Bones (skinned only) never needs real storage.
		static const StageLayout layout = MakeLayout(kLightingVSVariables, { { 0, 12 }, { 1, 12 }, { 6, 12 } }, { 16 });
		return layout;
	}

	const StageLayout& LightingPSLayout()
	{
		// DirectionalAmbient is float3x4; PointLightPosition/Color[7] (1, 2) are unused with Light Limit Fix.
		static const StageLayout layout = MakeLayout(kLightingPSVariables, { { 5, 12 }, { 1, 28 }, { 2, 28 } }, { 1, 2 });
		return layout;
	}

	void ConstantBlock::Reset()
	{
		floats.fill(std::bit_cast<float>(kUnwrittenBits));
	}

	ConstantEvaluator& ConstantEvaluator::Get()
	{
		static ConstantEvaluator evaluator;
		return evaluator;
	}

	namespace
	{
		/** @brief Constant buffer slots saved and restored around a stand-in call (D3D11 allows 14). */
		constexpr std::uint32_t kConstantBufferSlots = 14;

		/**
		 * @brief A snapshot of the pipeline state a stand-in evaluation could disturb.
		 *
		 * CS_DCLF_EVAL=audit captures one of these before the stand-in call and another after the restore,
		 * and reports every slot that differs. The point is to stop guessing which feature hook leaves
		 * something behind: whatever the engine's SetupMaterial or a hook on it publishes directly to the
		 * device, rather than through the shadow state, shows up here by slot number.
		 *
		 * Deliberately wider than what RunStandIn restores today, which is only the constant buffer at the
		 * evaluated level.
		 */
		struct PipelineSnapshot
		{
			static constexpr std::uint32_t kConstantBuffers = kConstantBufferSlots;
			static constexpr std::uint32_t kPixelResources = 128;
			static constexpr std::uint32_t kSamplers = 16;
			static constexpr std::uint32_t kVertexResources = 16;

			std::array<ID3D11Buffer*, kConstantBuffers> vsConstants{};
			std::array<ID3D11Buffer*, kConstantBuffers> psConstants{};
			std::array<ID3D11ShaderResourceView*, kPixelResources> psResources{};
			std::array<ID3D11ShaderResourceView*, kVertexResources> vsResources{};
			std::array<ID3D11SamplerState*, kSamplers> psSamplers{};

			// CPU-side memory a hook can leave behind. The bindings above only catch state published to
			// the device; a field of the real shader object leaks just as effectively and shows up in
			// neither.
			std::array<std::byte, 256> shaderObject{};

			// Raw pointers only: the values are compared, never dereferenced, and every reference the
			// getters hand out is released immediately so the snapshot cannot keep anything alive.
			void Capture(ID3D11DeviceContext* a_context, const void* a_shader)
			{
				auto release = [](auto& a_array) {
					for (auto*& entry : a_array) {
						if (entry) {
							entry->Release();
						}
					}
				};
				std::memcpy(shaderObject.data(), a_shader, shaderObject.size());

				a_context->VSGetConstantBuffers(0, kConstantBuffers, vsConstants.data());
				a_context->PSGetConstantBuffers(0, kConstantBuffers, psConstants.data());
				a_context->PSGetShaderResources(0, kPixelResources, psResources.data());
				a_context->VSGetShaderResources(0, kVertexResources, vsResources.data());
				a_context->PSGetSamplers(0, kSamplers, psSamplers.data());
				release(vsConstants);
				release(psConstants);
				release(psResources);
				release(vsResources);
				release(psSamplers);
			}
		};

		/** @brief Reports each distinct leak once; a per-slot report every frame would be unreadable. */
		void ReportLeaks(const char* a_what, const PipelineSnapshot& a_before, const PipelineSnapshot& a_after)
		{
			static ankerl::unordered_dense::set<std::uint64_t> reported;
			auto compare = [&](const char* a_kind, const auto& a_lhs, const auto& a_rhs, std::uint32_t a_tag) {
				for (std::uint32_t slot = 0; slot < a_lhs.size(); ++slot) {
					if (a_lhs[slot] == a_rhs[slot]) {
						continue;
					}
					const std::uint64_t key = (static_cast<std::uint64_t>(a_tag) << 32) | slot;
					if (!reported.insert(key).second) {
						continue;
					}
					logger::warn("[DCLF] stand-in leak ({}): {} slot {} was {} and is now {}", a_what, a_kind, slot,
						static_cast<const void*>(a_lhs[slot]), static_cast<const void*>(a_rhs[slot]));
				}
			};
			compare("VS constant buffer", a_before.vsConstants, a_after.vsConstants, 0);
			compare("PS constant buffer", a_before.psConstants, a_after.psConstants, 1);
			compare("PS resource", a_before.psResources, a_after.psResources, 2);
			compare("VS resource", a_before.vsResources, a_after.vsResources, 3);
			compare("PS sampler", a_before.psSamplers, a_after.psSamplers, 4);

			// Byte ranges: report the first differing offset of each, once.
			auto compareBytes = [&](const char* a_kind, const auto& a_lhs, const auto& a_rhs, std::uint32_t a_tag) {
				for (std::size_t offset = 0; offset < a_lhs.size(); ++offset) {
					if (a_lhs[offset] == a_rhs[offset]) {
						continue;
					}
					const std::uint64_t key = (static_cast<std::uint64_t>(a_tag) << 32) | offset;
					if (!reported.insert(key).second) {
						return;
					}
					logger::warn("[DCLF] stand-in leak ({}): {} changed at offset 0x{:x} ({:02x} -> {:02x})", a_what, a_kind,
						offset, static_cast<unsigned>(a_lhs[offset]), static_cast<unsigned>(a_rhs[offset]));
					return;
				}
			};
			compareBytes("BSLightingShader object", a_before.shaderObject, a_after.shaderObject, 5);
		}
	}

	template <class Call>
	bool ConstantEvaluator::RunStandIn(std::uint32_t a_level, std::uint32_t a_passDescriptor, ConstantBlock& a_vs, ConstantBlock& a_ps, Call&& a_call)
	{
		if (!lightingShader)
			return false;

		// CS_DCLF_EVAL is a diagnostic, not a mode. Suppressing an evaluation leaves the tables with
		// unwritten constants, so DCLF's own objects render wrong; what it answers is whether the
		// *natively* drawn content (actors, anything outside DCLF's coverage) is still corrupted, which
		// separates state the stand-in leaks into the engine from anything DCLF's own draws do.
		//
		//   off       neither SetupMaterial nor SetupGeometry is ever called
		//   material  only SetupMaterial runs (the per-material evaluation, ~636 calls a frame)
		//   geometry  only SetupGeometry runs (the per-pipeline evaluation, ~37 calls a frame)
		static const std::string eval = SwitchValue("CS_DCLF_EVAL");
		if (eval == "off")
			return false;
		if (eval == "material" && a_level != kPerMaterial)
			return false;
		if (eval == "geometry" && a_level != kPerGeometry)
			return false;

		auto& state = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
		auto* context = globals::d3d::context;
		auto* shader = static_cast<RE::BSLightingShader*>(lightingShader);

		// Stand-in shaders: constant tables point into ConstantBlocks, constant buffers are null, so the
		// engine writes straight into the blocks instead of mapping anything.
		alignas(16) static std::uint8_t vsStorage[sizeof(RE::BSGraphics::VertexShader)];
		alignas(16) static std::uint8_t psStorage[sizeof(RE::BSGraphics::PixelShader)];
		static ConstantBlock vsBlocks[3];
		static ConstantBlock psBlocks[3];
		std::memset(vsStorage, 0, sizeof(vsStorage));
		std::memset(psStorage, 0, sizeof(psStorage));
		auto* vs = reinterpret_cast<RE::BSGraphics::VertexShader*>(vsStorage);
		auto* ps = reinterpret_cast<RE::BSGraphics::PixelShader*>(psStorage);
		for (std::uint32_t level = 0; level < 3; ++level) {
			vsBlocks[level].Reset();
			psBlocks[level].Reset();
		}
		PointAt(vs, LightingVSLayout(), vsBlocks);
		PointAt(ps, LightingPSLayout(), psBlocks);

		// Everything the shader functions may touch, to restore afterwards.
		auto savedState = std::make_unique<std::uint8_t[]>(sizeof(state));
		std::memcpy(savedState.get(), &state, sizeof(state));
		const std::uint32_t savedTechnique = shader->currentRawTechnique;
		// Community Shaders' SetupGeometry hooks write its permutation data (external emittance, Extended
		// Translucency); it only reaches the GPU in State::Draw, so restoring it undoes the evaluation's
		// effect. (Light Limit Fix's hook uploads its StrictLightData and caches what it uploaded; that stays
		// consistent and is left alone.)
		const auto savedPermutation = globals::state->permutationData;
		// Advanced Skin's SetupMaterial hook leaves its t71/t74 binding pending for the next State::Draw: restored
		// likewise, or the next native draw that skips SetupMaterial (the same material as the draw before it)
		// would bind the evaluated material's textures.
		auto& skin = globals::features::skin;
		const auto savedSkinTextures = skin.loaded ? skin.GetPendingTextures() : Skin::PendingTextures{};
		// Every constant buffer slot, not just the evaluated level. Restoring only a_level was an
		// assumption about which slots the shader functions touch, and the audit disproved it: a stand-in
		// SetupGeometry leaves a buffer bound at PS slot 7 that was not bound before (a feature hook binds
		// its own per-geometry buffer there). Saving the whole range costs one Get and one Set per stage
		// and removes the class of problem rather than the instance.
		std::array<ID3D11Buffer*, kConstantBufferSlots> savedVS{};
		std::array<ID3D11Buffer*, kConstantBufferSlots> savedPS{};
		context->VSGetConstantBuffers(0, kConstantBufferSlots, savedVS.data());
		context->PSGetConstantBuffers(0, kConstantBufferSlots, savedPS.data());

		// The audit is expensive (about 200 device calls a snapshot) and the frame rate collapses under it,
		// which is acceptable for a diagnostic. It covers *every* evaluation rather than a sample: the
		// first version audited only the first 8 of roughly 640 material evaluations a frame and reported
		// nothing, which says nothing at all when the leaking call might be any one of the other 630.
		const bool audit = eval == "audit" && auditsThisFrame < 4096;
		PipelineSnapshot before;
		if (audit) {
			++auditsThisFrame;
			before.Capture(context, shader);
		}

		state.currentVertexShader = vs;
		state.currentPixelShader = ps;
		for (std::uint32_t slot = 0; slot < kPixelTextureSlots; ++slot) {
			state.PSTexture[slot] = reinterpret_cast<std::remove_reference_t<decltype(state.PSTexture[slot])>>(TextureSentinel(slot));
			state.PSTextureFilterMode[slot] = static_cast<RE::BSGraphics::TextureFilterMode>(kUnwrittenFilterMode);
		}
		shader->currentRawTechnique = a_passDescriptor;

		evaluating = true;
		a_call(state);
		evaluating = false;

		a_vs = vsBlocks[a_level];
		a_ps = psBlocks[a_level];
		a_call.Collect(state);

		std::memcpy(&state, savedState.get(), sizeof(state));
		shader->currentRawTechnique = savedTechnique;
		globals::state->permutationData = savedPermutation;
		if (skin.loaded)
			skin.SetPendingTextures(savedSkinTextures);
		context->VSSetConstantBuffers(0, kConstantBufferSlots, savedVS.data());
		context->PSSetConstantBuffers(0, kConstantBufferSlots, savedPS.data());
		for (auto* buffer : savedVS) {
			if (buffer) {
				buffer->Release();
			}
		}
		for (auto* buffer : savedPS) {
			if (buffer) {
				buffer->Release();
			}
		}

		if (audit) {
			PipelineSnapshot after;
			after.Capture(context, shader);
			ReportLeaks(a_level == kPerMaterial ? "material" : "geometry", before, after);
		}
		return true;
	}

	bool ConstantEvaluator::EvaluateMaterial(const RE::BSShaderMaterial* a_material, std::uint32_t a_passDescriptor, MaterialRecord& a_out)
	{
		if (!a_material)
			return false;

		struct Call
		{
			RE::BSShader* shader;
			const RE::BSShaderMaterial* material;
			MaterialRecord* out;

			void operator()(ShadowStateData&)
			{
				CallVirtual<void (*)(RE::BSShader*, const RE::BSShaderMaterial*)>(shader, kSetupMaterialSlot, material);
			}

			void Collect(ShadowStateData& a_state)
			{
				out->textureWritten = 0;
				for (std::uint32_t slot = 0; slot < kPixelTextureSlots; ++slot) {
					auto* texture = reinterpret_cast<ID3D11ShaderResourceView*>(a_state.PSTexture[slot]);
					if (texture != TextureSentinel(slot)) {
						out->textures[slot] = texture;
						out->addressModes[slot] = static_cast<std::uint32_t>(a_state.PSTextureAddressMode[slot].underlying());
						out->textureWritten |= 1u << slot;
					} else {
						out->textures[slot] = nullptr;
						out->addressModes[slot] = 0;
					}
					out->filterModes[slot] = static_cast<std::uint32_t>(a_state.PSTextureFilterMode[slot].underlying());
				}
				// Advanced Skin's, from the rule its hook binds by rather than from what the hook left pending.
				out->featureTextures = {};
				if (auto& skin = globals::features::skin; skin.loaded) {
					if (const auto textures = skin.MaterialTexturesOf(static_cast<const RE::BSLightingShaderMaterialBase*>(material)))
						out->featureTextures = *textures;
				}
			}
		};
		return RunStandIn(kPerMaterial, a_passDescriptor, a_out.vs, a_out.ps, Call{ lightingShader, a_material, &a_out });
	}

	bool ConstantEvaluator::EvaluateGeometry(const RE::BSRenderPass& a_templatePass, std::uint32_t a_passDescriptor, std::uint32_t a_renderFlags, GeometryConstants& a_out)
	{
		struct Call
		{
			RE::BSShader* shader;
			RE::BSRenderPass pass;
			std::uint32_t renderFlags;

			void operator()(ShadowStateData&)
			{
				CallVirtual<void (*)(RE::BSShader*, RE::BSRenderPass*, std::uint32_t)>(shader, kSetupGeometrySlot, &pass, renderFlags);
				// Frame Annotations' SetupGeometry hook opened a debugger event for this pass, which its
				// RestoreGeometry hook would close; the stand-in never restores, so close it here or every
				// evaluation leaves one open (about 37 a frame, nesting the rest of the frame under them).
				if (FrameAnnotations::GeometryEventsEnabled())
					globals::state->EndPerfEvent();
			}

			void Collect(ShadowStateData&) {}
		};
		Call call{ lightingShader, a_templatePass, a_renderFlags };
		call.pass.next = nullptr;
		call.pass.passGroupNext = nullptr;
		call.pass.passEnum = a_passDescriptor + 0x4800002Du;
		return RunStandIn(kPerGeometry, a_passDescriptor, a_out.vs, a_out.ps, call);
	}

	void EvaluateTechnique(std::uint32_t a_passDescriptor, TechniqueConstants& a_out)
	{
		// Lighting variable indices (ShaderConstants::LightingVS / LightingPS).
		constexpr std::uint32_t kVSFogParam = 13;
		constexpr std::uint32_t kVSFogNearColor = 14;
		constexpr std::uint32_t kVSFogFarColor = 15;
		constexpr std::uint32_t kPSFogColor = 19;
		constexpr std::uint32_t kPSColourOutputClamp = 20;
		constexpr auto kAnisotropic = static_cast<std::uint32_t>(RE::BSGraphics::TextureFilterMode::kAnisotropic);

		a_out.vs.Reset();
		a_out.ps.Reset();
		a_out.filterModes.fill(kUnwrittenFilterMode);

		// Sampler filter modes: diffuse and normal always, then per technique.
		const std::uint32_t technique = (a_passDescriptor >> 24) & 0x3f;
		a_out.filterModes[0] = a_out.filterModes[1] = kAnisotropic;
		switch (technique) {
		case 1:  // Envmap (and 0x10, not drawn): cube map and its mask
			a_out.filterModes[4] = a_out.filterModes[5] = kAnisotropic;
			break;
		case 2:  // Glowmap
			a_out.filterModes[6] = kAnisotropic;
			break;
		case 3:  // Parallax
			a_out.filterModes[3] = kAnisotropic;
			break;
		default:
			break;
		}

		// Shadowed directional or shadow-casting point lights with DefShadow: the shadow mask in t14
		// (clamped; point-filtered unless iShadowMaskQuarter is 4) and its inverse size in VPOSOffset.
		const bool shadowLights = (a_passDescriptor & (1u << 13)) || (a_passDescriptor & 0x1c0u);
		a_out.shadowMask = shadowLights && (a_passDescriptor & (1u << 14));
		a_out.shadowMaskTexture = nullptr;
		if (a_out.shadowMask) {
			constexpr std::uint32_t kPSVPOSOffset = 11;
			const auto& target = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kSHADOW_MASK];
			a_out.shadowMaskTexture = reinterpret_cast<ID3D11ShaderResourceView*>(target.SRV);
			static RE::Setting* quarter = RE::GetINISetting("iShadowMaskQuarter:Display");
			a_out.filterModes[kShadowMaskSlot] = (quarter && quarter->GetInteger() != 4) ? 1u : 0u;
			if (target.texture) {
				D3D11_TEXTURE2D_DESC desc{};
				reinterpret_cast<ID3D11Texture2D*>(target.texture)->GetDesc(&desc);
				float* offset = &a_out.ps.floats[LightingPSLayout().offset[kPSVPOSOffset]];
				offset[0] = 1.0f / static_cast<float>(desc.Width);
				offset[1] = 1.0f / static_cast<float>(desc.Height);
				offset[2] = 0.0f;
				offset[3] = 0.0f;
			}
		}

		// Fog from the current scene graph's fog property (FUN_1414dfad0 in AE).
		const auto& shaderState = RE::BSShaderManager::State::GetSingleton();
		auto* sceneNode = shaderState.shadowSceneNode[shaderState.sceneGraph];
		const auto* fog = sceneNode ? sceneNode->GetRuntimeData().fogProperty.get() : nullptr;
		if (fog) {
			const auto& vsLayout = LightingVSLayout();
			const auto& psLayout = LightingPSLayout();
			float* param = &a_out.vs.floats[vsLayout.offset[kVSFogParam]];
			if (fog->farDistance != 0.0f || fog->nearDistance != 0.0f) {
				const float inverseRange = 1.0f / (fog->farDistance - fog->nearDistance);
				param[0] = inverseRange * fog->nearDistance;
				param[1] = inverseRange;
				param[2] = fog->power;
				param[3] = fog->clamp;
			} else {
				param[0] = 5000000.0f;
				param[1] = 0.1f;
				param[2] = 1.0f;
				param[3] = 0.0f;
			}
			const float nearColor[4] = { fog->nearColor.red, fog->nearColor.green, fog->nearColor.blue, shaderState.invFrameBufferRange };
			std::memcpy(&a_out.vs.floats[vsLayout.offset[kVSFogNearColor]], nearColor, sizeof(nearColor));
			std::memcpy(&a_out.ps.floats[psLayout.offset[kPSFogColor]], nearColor, sizeof(nearColor));
			// The engine leaves w of FogFarColor as whatever was on its stack.
			float* farColor = &a_out.vs.floats[vsLayout.offset[kVSFogFarColor]];
			farColor[0] = fog->farColor.red;
			farColor[1] = fog->farColor.green;
			farColor[2] = fog->farColor.blue;
		}

		// ColourOutputClamp: the fLightingOutputColourClamp* settings, copied when the shader is created.
		static RE::Setting* clampSettings[3] = {
			RE::GetINISetting("fLightingOutputColourClampPostLit:General"),
			RE::GetINISetting("fLightingOutputColourClampPostEnv:General"),
			RE::GetINISetting("fLightingOutputColourClampPostSpec:General"),
		};
		float* clamp = &a_out.ps.floats[LightingPSLayout().offset[kPSColourOutputClamp]];
		for (std::uint32_t i = 0; i < 3; ++i)
			clamp[i] = clampSettings[i] ? clampSettings[i]->GetFloat() : 1.0f;
		clamp[3] = 0.0f;
	}
}
