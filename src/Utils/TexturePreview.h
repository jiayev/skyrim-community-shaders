#pragma once

#include "Buffer.h"

class TexturePreview
{
public:
	/** @brief Displays a 2D texture or volume slice with mip, channel and range controls. */
	void Draw(ID3D11ShaderResourceView* srv, const char* id, const char* label, float scale, bool scalar = false, float2 defaultRange = { 0.f, 1.f });
	/** @brief Releases the preview shader so it can be requested again. */
	void ClearShaderCache();

private:
	struct View
	{
		eastl::unique_ptr<Texture2D> texture;
		int mip = 0;
		int slice = 0;
		int channel = 0;
		float2 range = { 0.f, 1.f };
	};
	struct Parameters
	{
		uint32_t mip, slice, channel, volume;
		float2 range;
		float2 padding = {};
	};
	static_assert(sizeof(Parameters) == 32);
	ankerl::unordered_dense::map<std::string, View> views;
	eastl::unique_ptr<ConstantBuffer> constants;
	winrt::com_ptr<ID3D11ComputeShader> shader;
	bool requested = false;
};
