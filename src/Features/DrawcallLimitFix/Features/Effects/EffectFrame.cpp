// The Effect port's frame inputs: its three frame constant rows (Api::FrameConstantRow: the camera's planes, the Lighting variant's light
// colour, Blood's globals; PS b13 c17-c19) and the soft effects' depth (Api::FrameTexture, t118). Sampled on the render thread by each
// main commit (the rows) and each colour commit (the depth).

#include "Features/DrawcallLimitFix/Features/Effects/Effects.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Features/Effects/EffectConstants.h"
#include "Globals.h"

#include <array>
#include <cstddef>
#include <cstring>

namespace DCLF::Features::Effects
{
	namespace
	{
		/**
		 * @brief The camera planes BSEffectShader::SetupTechnique (0x1414ea2c0) takes for a Soft pass (camera at 0x14332a328: near +0x160,
		 * far +0x164): far, near, far - near (CameraDataEffect.yz), 0. PS b13 c17 (DCLFEffectCamera); zero without a camera.
		 */
		std::array<float, 4> SampleEffectCamera()
		{
			static const REL::Relocation<const std::byte* const*> camera{ REL::Offset(Address::kEffectCamera) };
			const std::byte* current = *camera.get();
			if (!current)
				return {};
			float nearPlane = 0.0f, farPlane = 0.0f;
			std::memcpy(&nearPlane, current + 0x160, sizeof(nearPlane));
			std::memcpy(&farPlane, current + 0x164, sizeof(farPlane));
			return { farPlane, nearPlane, farPlane - nearPlane, 0.0f };
		}

		/**
		 * @brief Blood (kWeaponBlood): PS AlphaTestRef.yz as SetupGeometry (0x1414eab10) writes them: globals 0x1433dccf0 and 0x1433dccf4,
		 * written only by FUN_1414ec2a0 from BSTempEffectWeaponBlood's Update and start (game settings DAT_14200fbe8, DAT_14200fc00); x 0
		 * (the record's threshold), w 0. PS b13 c19 (DCLFEffectBlood).
		 */
		std::array<float, 4> SampleEffectBlood()
		{
			static const REL::Relocation<const float*> globals{ REL::Offset(Address::kEffectBlood) };
			const float* blood = globals.get();
			return { 0.0f, blood[0], blood[1], 0.0f };
		}

		/**
		 * @brief The Lighting variant: PS DLightColor as BSEffectShader::SetupGeometry (0x1414eab10) writes it: light 0 (FUN_1414fcf80: the
		 * sun, ShadowSceneNode[0] +0x200, without kCloudLOD), its NiLight fade (+0x134) times the HDR multiplier (ImageSpaceManager +0xe0)
		 * times each channel of NiDirectionalLight +0x14c (the weather's effect lighting); w 0. PS b13 c18 (DCLFEffectDLightColor); zero
		 * without a sun light.
		 */
		std::array<float, 4> SampleEffectLight()
		{
			// SetupGeometry (0x1414eab10), the Lighting bit: PS DLightColor = (ImageSpaceManager +0xe0) x (NiLight +0x134), then that times each
			// of the light's +0x14c, +0x150, +0x154 (the multiplier first, as the engine's mulss order). Light 0 is BSLight +0x48's NiLight.
			static const REL::Relocation<const std::byte* const*> imageSpace{ REL::Offset(Engine::Address::kImageSpaceManager) };
			auto* sceneNode = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
			const RE::BSLight* sun = sceneNode ? sceneNode->GetRuntimeData().sunLight : nullptr;
			const std::byte* light = nullptr;
			if (sun)
				std::memcpy(&light, reinterpret_cast<const std::byte*>(sun) + 0x48, sizeof(light));
			const std::byte* manager = *imageSpace.get();
			if (!light || !manager)
				return {};
			auto read = [](const std::byte* a_base, std::size_t a_offset) {
				float value = 0.0f;
				std::memcpy(&value, a_base + a_offset, sizeof(value));
				return value;
			};
			const float scale = read(manager, 0xe0) * read(light, 0x134);
			return { scale * read(light, 0x14c), scale * read(light, 0x150), scale * read(light, 0x154), 0.0f };
		}

		template <std::array<float, 4> (*Sample)()>
		void SampleRow(float (&a_out)[4])
		{
			const auto value = Sample();
			static_assert(sizeof(value) == sizeof(a_out));
			std::memcpy(a_out, value.data(), sizeof(a_out));
		}

		// Soft effects' depth: the kPOST_ZPREPASS_COPY SetupTechnique binds for a Soft pass, which CS fills from the main depth after the
		// deferred pass, before the forward epoch.
		void* SoftDepthView()
		{
			return globals::game::renderer ? static_cast<void*>(globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV) :
			                                 nullptr;
		}
	}

	void RegisterFrame(Api::InstallContext& a_context)
	{
		// The camera planes soft effects read (DCLFEffectCamera), the sun's effect colour (DCLFEffectDLightColor), and Blood's
		// AlphaTestRef.yz: the weapon blood's two globals (DCLFEffectBlood). Latched in row order whatever the registration's.
		a_context.RegisterFrameConstantRow({ "effect camera", kEffectCameraFrameRow, &SampleRow<&SampleEffectCamera> });
		a_context.RegisterFrameConstantRow({ "effect light", kEffectLightFrameRow, &SampleRow<&SampleEffectLight> });
		a_context.RegisterFrameConstantRow({ "effect blood", kEffectBloodFrameRow, &SampleRow<&SampleEffectBlood> });
		a_context.RegisterFrameTexture({ "effect soft depth", kSoftDepthRegister, &SoftDepthView });
	}
}
