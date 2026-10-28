#include "Rendering/EnvironmentProbe/Geometry.h"

#include "Globals.h"
#include "Util.h"
#include <bit>
#include <d3dcompiler.h>
#include <fstream>

namespace EnvironmentProbe
{
	ID3D11GeometryShader* GeometryShaders::Get(const RE::BSGraphics::VertexShader& vertex)
	{
		auto* key = reinterpret_cast<ID3D11VertexShader*>(vertex.shader);
		if (const auto found = vertices.find(key); found != vertices.end())
			return found->second.geometry;
		auto remember = [&](ID3D11GeometryShader* shader) {
			VertexEntry entry;
			entry.owner.copy_from(key);
			entry.geometry = shader;
			vertices.emplace(key, std::move(entry));
			return shader;
		};
		winrt::com_ptr<ID3D11ShaderReflection> reflection;
		DX::ThrowIfFailed(D3DReflect(vertex.rawBytecode, vertex.byteCodeSize, __uuidof(ID3D11ShaderReflection), reflection.put_void()));
		D3D11_SHADER_DESC desc{};
		DX::ThrowIfFailed(reflection->GetDesc(&desc));
		std::string signature = "struct ProbeVertex {\n";
		std::string aliases;
		uint32_t required = 0;
		for (uint32_t i = 0; i < desc.OutputParameters; ++i) {
			D3D11_SIGNATURE_PARAMETER_DESC parameter{};
			DX::ThrowIfFailed(reflection->GetOutputParameterDesc(i, &parameter));
			const auto components = std::popcount(uint32_t(parameter.Mask));
			const char* type = parameter.ComponentType == D3D_REGISTER_COMPONENT_UINT32 ? "uint" : parameter.ComponentType == D3D_REGISTER_COMPONENT_SINT32 ? "int" :
			                                                                                                                                                  "float";
			signature += std::format("{}{}{} v{} : {}{};\n", parameter.ComponentType == D3D_REGISTER_COMPONENT_FLOAT32 ? "" : "nointerpolation ", type, components, i, parameter.SemanticName, parameter.SemanticIndex);
			const char* alias = nullptr;
			if (parameter.SystemValueType == D3D_NAME_POSITION) {
				alias = "PROBE_POSITION";
				required |= 1;
			} else if (!_stricmp(parameter.SemanticName, "POSITION") && parameter.SemanticIndex == 1) {
				alias = "PROBE_WORLD";
				required |= 2;
			} else if (parameter.SystemValueType == D3D_NAME_CLIP_DISTANCE) {
				alias = "PROBE_CLIP";
				required |= 4;
			} else if (!_stricmp(parameter.SemanticName, "TEXCOORD") && parameter.SemanticIndex == 13) {
				alias = "PROBE_FACE";
				required |= 8;
			}
			if (alias)
				aliases += std::format("#define {} v{}\n", alias, i);
		}
		if (required != 15)
			throw std::runtime_error("Environment probe VS is missing projection varyings");
		signature += "};\n" + aliases;
		if (const auto found = signatures.find(signature); found != signatures.end())
			return remember(found->second.get());
		const std::filesystem::path path = "Data/Shaders/Common/EnvironmentProbeGeometry.hlsli";
		std::ifstream file(path, std::ios::binary);
		if (!file)
			throw std::runtime_error("Cannot read EnvironmentProbeGeometry.hlsli");
		const std::string source = signature + std::string(std::istreambuf_iterator<char>(file), {});
		winrt::com_ptr<ID3DBlob> bytecode, errors;
		const auto hr = D3DCompile(source.data(), source.size(), path.string().c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
			"main", "gs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, bytecode.put(), errors.put());
		if (FAILED(hr))
			throw std::runtime_error(errors ? static_cast<const char*>(errors->GetBufferPointer()) : "Environment probe GS compilation failed");
		winrt::com_ptr<ID3D11GeometryShader> shader;
		DX::ThrowIfFailed(globals::d3d::device->CreateGeometryShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, shader.put()));
		Util::SetResourceName(shader.get(), "EnvironmentProbe::Octant Geometry Shader");
		return remember(signatures.emplace(std::move(signature), std::move(shader)).first->second.get());
	}

	void GeometryShaders::Clear()
	{
		vertices.clear();
		signatures.clear();
	}
}
