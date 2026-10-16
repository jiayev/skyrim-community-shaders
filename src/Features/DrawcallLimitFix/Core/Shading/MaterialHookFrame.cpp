// MaterialPort's HookFrame sample (render thread, the frame's start): the engine globals Community Shaders' SetupMaterial hooks read.

#include "Features/DrawcallLimitFix/Core/Shading/MaterialHookFrame.h"

#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"

#include <algorithm>
#include <cstddef>
#include <iterator>

namespace DCLF::MaterialPort
{
	static_assert(static_cast<std::uint32_t>(RE::RENDER_TARGETS::kTOTAL) <= kHookRenderTargets);

	ID3D11ShaderResourceView* ViewOfTexture(const RE::NiSourceTexture* a_texture)
	{
		const auto* texture = a_texture ? a_texture->rendererTexture : nullptr;
		return texture ? reinterpret_cast<ID3D11ShaderResourceView*>(texture->resourceView) : nullptr;
	}

	HookFrame SampleHookFrame()
	{
		HookFrame frame;

		const auto& frameGlobals = FrameGlobals::Current();
		frame.techniqueByte12 = frameGlobals.techniqueByte12;
		frame.techniqueByte7 = frameGlobals.techniqueByte7;

		if (const auto* smState = globals::game::smState) {
			frame.textureTransformBuffer = smState->textureTransformCurrentBuffer;
			frame.characterLightEnabled = smState->characterLightEnabled;
			std::copy_n(smState->characterLightParams, 4, frame.characterLightParams.data());
		}
		static const REL::Relocation<RE::ImageSpaceTexture*> characterLightTexture{ RELOCATION_ID(513464, 391302) };
		frame.characterLightTarget = static_cast<std::int32_t>(characterLightTexture->renderTarget);

		if (const auto* renderer = globals::game::renderer) {
			const auto& targets = renderer->GetRuntimeData().renderTargets;
			const std::size_t count = std::min<std::size_t>(std::size(targets), kHookRenderTargets);
			for (std::size_t i = 0; i < count; ++i)
				frame.renderTargetViews[i] = reinterpret_cast<ID3D11ShaderResourceView*>(targets[i].SRV);
		}

		if (const auto* graphicsState = globals::game::graphicsState) {
			const auto& state = graphicsState->GetRuntimeData();
			frame.defaultBlack = state.defaultTextureBlack.get();
			frame.defaultWhite = state.defaultTextureWhite.get();
			frame.defaultNormalMap = state.defaultTextureNormalMap.get();
			frame.defaultBlackView = ViewOfTexture(state.defaultTextureBlack.get());
			frame.defaultWhiteView = ViewOfTexture(state.defaultTextureWhite.get());
			frame.defaultNormalMapView = ViewOfTexture(state.defaultTextureNormalMap.get());
		}
		return frame;
	}
}
