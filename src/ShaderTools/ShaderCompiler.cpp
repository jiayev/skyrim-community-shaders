#include "d3d11.h"
#include "d3dcompiler.h"

namespace ShaderCompiler
{
	ID3D11PixelShader* RegisterPixelShader(const std::wstring a_filePath)
	{
		REL::Relocation<ID3D11Device**> g_ID3D11Device{ RELOCATION_ID(524729, 411348) };

		ID3DBlob* shaderBlob = nullptr;

		if (FAILED(D3DReadFileToBlob(a_filePath.c_str(), &shaderBlob))) {
			logger::error("Pixel shader load failed:\n{}", "File does not exist or is invalid");

			if (shaderBlob)
				shaderBlob->Release();

			return nullptr;
		}

		logger::debug("shader load succeeded");

		logger::debug("registering shader");

		ID3D11PixelShader* regShader;

		if (FAILED((*g_ID3D11Device)->CreatePixelShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, &regShader))) {
			logger::error("pixel shader registration failed");

			if (shaderBlob)
				shaderBlob->Release();

			return nullptr;
		}

		logger::debug("shader registration succeeded");

		return regShader;
	}

	ID3D11PixelShader* CompileAndRegisterPixelShader(const std::wstring a_filePath)
	{
		REL::Relocation<ID3D11Device**> g_ID3D11Device{ RELOCATION_ID(524729, 411348) };

		D3D_SHADER_MACRO macros[3] = {};

		macros[0].Name = "WINPC";
		macros[0].Definition = "";
		macros[1].Name = "DX11";
		macros[1].Definition = "";

		UINT compilerFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
		ID3DBlob* shaderBlob = nullptr;
		ID3DBlob* shaderErrors = nullptr;

		if (FAILED(D3DCompileFromFile(a_filePath.c_str(), macros, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "ps_5_0", compilerFlags, 0, &shaderBlob, &shaderErrors))) {
			logger::error("Pixel shader compilation failed:\n{}", shaderErrors ? (const char*)shaderErrors->GetBufferPointer() : "Unknown error");

			if (shaderBlob)
				shaderBlob->Release();

			if (shaderErrors)
				shaderErrors->Release();

			return nullptr;
		}

		if (shaderErrors)
			shaderErrors->Release();

		logger::debug("shader compilation succeeded");

		logger::debug("registering shader");

		ID3D11PixelShader* regShader;

		if (FAILED((*g_ID3D11Device)->CreatePixelShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, &regShader))) {
			logger::error("pixel shader registration failed");

			if (shaderBlob)
				shaderBlob->Release();

			return nullptr;
		}

		logger::debug("shader registration succeeded");

		return regShader;
	}
}