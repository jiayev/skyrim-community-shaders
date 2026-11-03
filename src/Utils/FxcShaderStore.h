#pragma once

#include <ORGModuleServices/ShaderStore.h>

#include <d3dcompiler.h>
#include <winrt/base.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

/// Builds HLSL into DXBC with FXC, through the shared shader store.
namespace Util
{
	/// One FXC build. Every field that can change the output is part of its key.
	struct FxcRequest
	{
		std::filesystem::path root;         ///< The source file, read through the store's snapshot.
		std::filesystem::path includeBase;  ///< Includes are looked up here, and nowhere else, like the D3D11 shader paths always did.
		const D3D_SHADER_MACRO* defines = nullptr;  ///< Ends with a null entry. The order is part of the key.
		const char* entryPoint = "main";
		const char* profile = nullptr;
		uint32_t flags = 0;
		uint32_t stripFlags = 0;  ///< Passed to D3DStripShader after a compile. 0 leaves the output as compiled.
		bool debug = false;       ///< Debug info embeds the source, so any source change rebuilds it.
	};

	struct FxcResult
	{
		winrt::com_ptr<ID3DBlob> blob;  ///< Null when the shader failed.
		org::services::ShaderStoreResult store;
		org::services::ShaderStoreHash variantKey;
	};

	/** @brief The compile flags every FXC build uses. Debug builds keep validation, so bad source gives an error rather than bad code. */
	uint32_t FxcCompileFlags(bool a_debug);

	/** @brief Returns the shader from the store if it is still valid, else builds and saves it. */
	FxcResult ResolveFxcShader(const FxcRequest& a_request);

	/**
	 * @brief The files a built variant read, in the form the shader file watcher matches: absolute, normalized and lower case.
	 * The watcher needs this after a store hit too, when no compile ran to report them.
	 */
	std::vector<std::string> FxcDependencyPaths(const org::services::ShaderStoreHash& a_variantKey);
}
