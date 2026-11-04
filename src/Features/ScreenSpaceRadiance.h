#pragma once

#include "Buffer.h"

namespace ScreenSpaceRadiance
{
	/** @brief Compiles spatial expansion for the consumer's trace layout. */
	winrt::com_ptr<ID3D11ComputeShader> CompileResolve(bool a_quarter, bool a_specular, bool a_sh, bool a_fixedGrid = false);
	/** @brief Expands sparse radiance into full-resolution consumer inputs without temporal denoising. */
	void Resolve(ID3D11ComputeShader* a_shader, Texture2D& a_input, Texture2D& a_output,
		Texture2D* a_inputSH1 = nullptr, Texture2D* a_outputSH1 = nullptr);
}
