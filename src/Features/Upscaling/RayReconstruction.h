#pragma once

#include "Buffer.h"

/** @brief Owns DLSS RR guides captured from the opaque deferred frame. */
class RayReconstruction
{
public:
	/** @brief Prepares resources before choosing the frame's denoiser. */
	bool BeginFrame();
	/** @brief Captures the albedo-applied diffuse color before screen-space SSS. */
	void CaptureBeforeSSS(ID3D11Resource* a_color);
	/** @brief Generates linear material guides and the signed SSS luminance difference. */
	void PrepareGuides(ID3D11ShaderResourceView* a_colorAfterSSS);
	/** @brief Retains the final opaque color before forward transparencies. */
	void CaptureBeforeTransparency(ID3D11Resource* a_color);
	/** @brief Converts RR colors between the scene encoding and linear HDR when needed. */
	void ConvertColor(ID3D11UnorderedAccessView* a_color, bool a_toScene);
	/** @brief Releases shaders and invalidates this frame's guides. */
	void ClearShaderCache();
	/** @brief Returns whether guides belong to the current rendered frame. */
	bool GuidesReady() const;

	eastl::unique_ptr<Texture2D> diffuseAlbedo, specularAlbedo, normalRoughness, specularHitDistance, sssGuide;
	eastl::unique_ptr<Texture2D> colorBeforeTransparency;
	// SSR writes original, unnormalized distances in the same layout as its trace.
	eastl::unique_ptr<Texture2D> tracedSpecularHitDistance;

private:
	eastl::unique_ptr<Texture2D> beforeSSS;
	winrt::com_ptr<ID3D11ComputeShader> prepareGuides;
	winrt::com_ptr<ID3D11ComputeShader> decodeColor, encodeColor;
	eastl::unique_ptr<ConstantBuffer> constants;
	uint32_t readyFrame = UINT32_MAX;
};
