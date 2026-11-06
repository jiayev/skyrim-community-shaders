#pragma once

#include <ORGModuleServices/ShaderStore.h>

#include <d3dcompiler.h>
#include <winrt/base.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

/// Builds game shaders with DXC, for DXVK to take as SPIR-V. See docs/development/dxc-game-shaders.md.
namespace Util
{
	/** @brief One DXC build of what would otherwise be an FXC request. */
	struct DxcRequest
	{
		std::filesystem::path root;                 ///< The source file.
		const D3D_SHADER_MACRO* defines = nullptr;  ///< Ends with a null entry.
		const char* target = nullptr;               ///< vs_6_6, ps_6_6 or cs_6_6.
	};

	struct DxcResult
	{
		winrt::com_ptr<ID3DBlob> blob;  ///< A DXBC container with the SPIR-V. Null when the build failed.
		bool fromStore = false;         ///< Nothing compiled.
		std::string diagnostics;
		org::services::ShaderStoreHash variantKey;  ///< The variant holding its source files, for FxcDependencyPaths.
	};

	/**
	 * @brief Whether a shader builds with DXC: "<path>:<target>:<descriptor in hex>" matches CS_SHADER_DXC, a regular expression, and the
	 * loaded DXVK takes SPIR-V. Off under native D3D11, whose runtime has no such export.
	 */
	bool UseDxc(const std::filesystem::path& a_root, std::string_view a_target, uint32_t a_descriptor);

	/** @brief Returns the shader from the store if it is still valid, else builds and saves it. */
	DxcResult ResolveDxcShader(const DxcRequest& a_request);

	/** @brief What ReflectConstantBuffers reads, from D3DReflect or from a DXC container's SPIR-V. */
	struct ShaderReflection
	{
		struct Variable
		{
			std::string name;
			uint32_t offset = 0;
			uint32_t size = 0;
			uint32_t elements = 0;
		};
		struct Buffer
		{
			std::string name;
			uint32_t size = 0;
			std::vector<Variable> variables;
		};
		struct Input
		{
			std::string semantic;
			uint32_t index = 0;
		};
		std::vector<Input> inputs;
		std::vector<Buffer> buffers;

		const Buffer* FindBuffer(std::string_view a_name) const;
	};

	/** @brief Reflects a shader blob of either compiler. False if it could not be read. */
	bool ReflectShader(ID3DBlob& a_blob, ShaderReflection& a_reflection);
}
