#include "Utils/DxcShaderStore.h"

#include "Utils/SharedShaderStore.h"
#include "Utils/SpirvReflect.h"

#include <ORGModuleServices/ShaderCompiler.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <regex>

namespace Util
{
	namespace
	{
		// Must match extern/dxvk/src/d3d11/d3d11_shader_spirv.h.
		constexpr uint32_t kIngestVersion = 1;
		constexpr uint32_t kShiftCbv = 0;
		constexpr uint32_t kShiftSrv = 1000;
		constexpr uint32_t kShiftSampler = 2000;
		constexpr uint32_t kShiftUav = 3000;

		constexpr uint32_t Tag(const char (&a_tag)[5])
		{
			return static_cast<uint32_t>(a_tag[0]) | (static_cast<uint32_t>(a_tag[1]) << 8) | (static_cast<uint32_t>(a_tag[2]) << 16) | (static_cast<uint32_t>(a_tag[3]) << 24);
		}

		using PFN_IngestVersion = uint32_t(__stdcall*)();
		using PFN_ContainerHash = void(__stdcall*)(const void*, size_t, uint32_t*);

		HMODULE DxvkModule()
		{
			return GetModuleHandleW(L"dxvk_d3d11.dll");
		}

		PFN_ContainerHash ContainerHash()
		{
			static const auto hash = [] {
				const auto module = DxvkModule();
				return module ? reinterpret_cast<PFN_ContainerHash>(reinterpret_cast<void*>(GetProcAddress(module, "dxvkSpirvContainerHash"))) : nullptr;
			}();
			return hash;
		}

		bool IngestAvailable()
		{
			static const bool available = [] {
				const auto module = DxvkModule();
				if (!module)
					return false;
				const auto version = reinterpret_cast<PFN_IngestVersion>(reinterpret_cast<void*>(GetProcAddress(module, "dxvkSpirvIngestVersion")));
				if (!version || version() != kIngestVersion || !ContainerHash()) {
					logger::warn("DXVK does not take this version of DXC shaders, so they stay on FXC");
					return false;
				}
				return true;
			}();
			return available;
		}

		// Runs on the calling thread, which is already one of the shader cache's workers. Beside the DXVK DLLs, as the render graph's.
		org::services::ShaderCompiler& Compiler()
		{
			static auto* compiler = [] {
				std::filesystem::path directory;
				wchar_t path[MAX_PATH];
				if (const auto module = DxvkModule(); module && GetModuleFileNameW(module, path, MAX_PATH))
					directory = std::filesystem::path(path).parent_path();
				auto* created = new org::services::ShaderCompiler({}, directory, SharedShaderStore());
				created->Configure([](org::services::ServiceTask a_task) {
					a_task();
					return true;
				});
				return created;
			}();
			return *compiler;
		}

		std::wstring Widen(std::string_view a_text)
		{
			return std::wstring(a_text.begin(), a_text.end());
		}

		std::vector<std::byte> ReadFile(const std::filesystem::path& a_path)
		{
			std::ifstream file(a_path, std::ios::binary | std::ios::ate);
			if (!file)
				return {};
			std::vector<std::byte> bytes(static_cast<size_t>(file.tellg()));
			file.seekg(0);
			file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			return bytes;
		}

		std::span<const uint32_t> Words(const std::vector<std::byte>& a_bytes)
		{
			return { reinterpret_cast<const uint32_t*>(a_bytes.data()), a_bytes.size() / sizeof(uint32_t) };
		}

		// D3D_NAME's values, for a system value's semantic. SV_Target and SV_Depth are 0 in a signature.
		uint32_t SystemValueOf(std::string_view a_semantic)
		{
			static constexpr std::pair<std::string_view, uint32_t> kValues[] = {
				{ "SV_POSITION", 1 }, { "SV_CLIPDISTANCE", 2 }, { "SV_CULLDISTANCE", 3 }, { "SV_RENDERTARGETARRAYINDEX", 4 }, { "SV_VIEWPORTARRAYINDEX", 5 },
				{ "SV_VERTEXID", 6 }, { "SV_PRIMITIVEID", 7 }, { "SV_INSTANCEID", 8 }, { "SV_ISFRONTFACE", 9 }, { "SV_SAMPLEINDEX", 10 }
			};
			for (const auto& [name, value] : kValues) {
				if (std::ranges::equal(name, a_semantic, [](char a, char b) { return std::toupper(static_cast<unsigned char>(a)) == std::toupper(static_cast<unsigned char>(b)); }))
					return value;
			}
			return 0;
		}

		void Append(std::vector<std::byte>& a_out, uint32_t a_value)
		{
			const auto* bytes = reinterpret_cast<const std::byte*>(&a_value);
			a_out.insert(a_out.end(), bytes, bytes + sizeof(a_value));
		}

		// An ISGN or OSGN chunk, ordered by register as FXC's are. An array or matrix takes one row per register.
		std::vector<std::byte> SignatureChunk(std::span<const Spirv::IoVariable> a_variables, std::span<const Spirv::IoPlacement> a_placements)
		{
			struct Row
			{
				const Spirv::IoVariable* variable;
				uint32_t semanticIndex;
				uint32_t reg;
				uint32_t mask;
			};
			std::vector<Row> rows;
			for (size_t i = 0; i < a_variables.size(); ++i) {
				const auto& variable = a_variables[i];
				const auto& placement = a_placements[i];
				const uint32_t mask = ((1u << variable.components) - 1) << placement.component;
				for (uint32_t r = 0; r < variable.registers; ++r)
					rows.push_back({ &variable, variable.semanticIndex + r, placement.reg == Spirv::kNoLocation ? Spirv::kNoLocation : placement.reg + r, mask });
			}
			std::ranges::stable_sort(rows, [](const Row& a, const Row& b) { return a.reg != b.reg ? a.reg < b.reg : a.mask < b.mask; });

			std::vector<std::byte> chunk;
			Append(chunk, static_cast<uint32_t>(rows.size()));
			Append(chunk, 8);
			uint32_t nameOffset = 8 + 24 * static_cast<uint32_t>(rows.size());
			std::vector<std::byte> names;
			for (const auto& row : rows) {
				Append(chunk, nameOffset + static_cast<uint32_t>(names.size()));
				Append(chunk, row.semanticIndex);
				Append(chunk, SystemValueOf(row.variable->semantic));
				Append(chunk, static_cast<uint32_t>(row.variable->type));
				Append(chunk, row.reg);
				Append(chunk, row.mask | (row.mask << 8));
				const auto* text = reinterpret_cast<const std::byte*>(row.variable->semantic.c_str());
				names.insert(names.end(), text, text + row.variable->semantic.size() + 1);
			}
			chunk.insert(chunk.end(), names.begin(), names.end());
			chunk.resize((chunk.size() + 3) & ~size_t(3));
			return chunk;
		}

		// A DXBC container, so the engine and DXVK's CreateInputLayout read it like any other. The hash is DXVK's, which checks it.
		winrt::com_ptr<ID3DBlob> MakeContainer(std::vector<std::byte> a_inputs, std::vector<std::byte> a_outputs, std::span<const uint32_t> a_module)
		{
			const std::array<std::pair<uint32_t, std::span<const std::byte>>, 4> chunks{ {
				{ Tag("ISGN"), a_inputs },
				{ Tag("OSGN"), a_outputs },
				{ Tag("SPRM"), std::as_bytes(std::span(&kIngestVersion, 1)) },
				{ Tag("SPRV"), std::as_bytes(a_module) },
			} };
			size_t size = 32 + 4 * chunks.size();
			for (const auto& chunk : chunks)
				size += 8 + chunk.second.size();

			winrt::com_ptr<ID3DBlob> blob;
			if (FAILED(D3DCreateBlob(size, blob.put())))
				return nullptr;
			auto* out = static_cast<std::byte*>(blob->GetBufferPointer());
			std::memset(out, 0, size);
			const auto write = [&](size_t a_offset, uint32_t a_value) { std::memcpy(out + a_offset, &a_value, sizeof(a_value)); };
			std::memcpy(out, "DXBC", 4);
			write(20, 1);
			write(24, static_cast<uint32_t>(size));
			write(28, static_cast<uint32_t>(chunks.size()));
			size_t offset = 32 + 4 * chunks.size();
			for (size_t i = 0; i < chunks.size(); ++i) {
				write(32 + 4 * i, static_cast<uint32_t>(offset));
				write(offset, chunks[i].first);
				write(offset + 4, static_cast<uint32_t>(chunks[i].second.size()));
				std::memcpy(out + offset + 8, chunks[i].second.data(), chunks[i].second.size());
				offset += 8 + chunks[i].second.size();
			}
			uint32_t hash[4];
			ContainerHash()(out, size, hash);
			std::memcpy(out + 4, hash, sizeof(hash));
			return blob;
		}

		// The chunk's bytes in a container, empty if it has none.
		std::span<const std::byte> FindChunk(ID3DBlob& a_blob, uint32_t a_tag)
		{
			const auto* bytes = static_cast<const std::byte*>(a_blob.GetBufferPointer());
			const size_t size = a_blob.GetBufferSize();
			if (size < 32)
				return {};
			uint32_t count = 0;
			std::memcpy(&count, bytes + 28, sizeof(count));
			for (uint32_t i = 0; i < count && 32 + 4 * size_t(i) + 4 <= size; ++i) {
				uint32_t offset = 0, tag = 0, chunkSize = 0;
				std::memcpy(&offset, bytes + 32 + 4 * i, sizeof(offset));
				if (size_t(offset) + 8 > size)
					break;
				std::memcpy(&tag, bytes + offset, sizeof(tag));
				std::memcpy(&chunkSize, bytes + offset + 4, sizeof(chunkSize));
				if (tag == a_tag && size_t(offset) + 8 + chunkSize <= size)
					return { bytes + offset + 8, chunkSize };
			}
			return {};
		}

		bool ReflectDxbc(ID3DBlob& a_blob, ShaderReflection& a_reflection)
		{
			winrt::com_ptr<ID3D11ShaderReflection> reflector;
			if (FAILED(D3DReflect(a_blob.GetBufferPointer(), a_blob.GetBufferSize(), IID_PPV_ARGS(reflector.put()))))
				return false;
			D3D11_SHADER_DESC desc;
			if (FAILED(reflector->GetDesc(&desc)))
				return false;
			for (UINT i = 0; i < desc.InputParameters; ++i) {
				D3D11_SIGNATURE_PARAMETER_DESC input;
				if (SUCCEEDED(reflector->GetInputParameterDesc(i, &input)))
					a_reflection.inputs.push_back({ input.SemanticName, input.SemanticIndex });
			}
			for (UINT b = 0; b < desc.ConstantBuffers; ++b) {
				auto* buffer = reflector->GetConstantBufferByIndex(b);
				D3D11_SHADER_BUFFER_DESC bufferDesc;
				if (!buffer || FAILED(buffer->GetDesc(&bufferDesc)))
					continue;
				auto& reflected = a_reflection.buffers.emplace_back();
				reflected.name = bufferDesc.Name;
				reflected.size = bufferDesc.Size;
				for (UINT v = 0; v < bufferDesc.Variables; ++v) {
					auto* variable = buffer->GetVariableByIndex(v);
					D3D11_SHADER_VARIABLE_DESC variableDesc;
					if (FAILED(variable->GetDesc(&variableDesc)))
						continue;
					D3D11_SHADER_TYPE_DESC typeDesc{};
					variable->GetType()->GetDesc(&typeDesc);
					reflected.variables.push_back({ variableDesc.Name, variableDesc.StartOffset, variableDesc.Size, typeDesc.Elements });
				}
			}
			return true;
		}
	}

	bool UseDxc(const std::filesystem::path& a_root, std::string_view a_target, uint32_t a_descriptor)
	{
		static const std::optional<std::regex> pattern = []() -> std::optional<std::regex> {
			char value[1024];
			const DWORD length = GetEnvironmentVariableA("CS_SHADER_DXC", value, sizeof(value));
			if (!length || length >= sizeof(value))
				return std::nullopt;
			try {
				return std::regex(value, std::regex::icase | std::regex::optimize);
			} catch (const std::regex_error& error) {
				logger::warn("CS_SHADER_DXC is not a valid regular expression: {}", error.what());
				return std::nullopt;
			}
		}();
		return pattern && IngestAvailable() && Compiler().Available() && std::regex_search(std::format("{}:{}:{:08X}", a_root.generic_string(), a_target, a_descriptor), *pattern);
	}

	DxcResult ResolveDxcShader(const DxcRequest& a_request)
	{
		DxcResult result;
		const auto source = ReadFile(a_request.root);
		if (source.empty()) {
			result.diagnostics = std::format("{} could not be read", a_request.root.generic_string());
			return result;
		}

		org::services::ShaderCompileRequest request{};
		request.sourceName = a_request.root.generic_string();
		request.source = source;
		request.entryPoint = L"main";
		request.target = Widen(a_request.target);
		request.format = org::services::ShaderBinaryFormat::Spirv;
		request.warningsAsErrors = false;
		request.languageVersion = L"2018";  // CS's shaders are written for FXC's semantics
		request.keepInMemory = false;       // Thousands of variants, and the shader cache keeps what it needs
		request.includeDirectories = { a_request.root.parent_path(), std::filesystem::path(L"Data/Shaders") };
		for (const auto* define = a_request.defines; define && define->Name; ++define)
			request.defines.push_back({ Widen(define->Name), Widen(define->Definition ? define->Definition : "") });
		const auto shift = [&](const wchar_t* a_flag, uint32_t a_value) {
			request.arguments.insert(request.arguments.end(), { a_flag, std::to_wstring(a_value), L"0" });
		};
		shift(L"-fvk-b-shift", kShiftCbv);
		shift(L"-fvk-t-shift", kShiftSrv);
		shift(L"-fvk-s-shift", kShiftSampler);
		shift(L"-fvk-u-shift", kShiftUav);
		request.arguments.emplace_back(L"-fvk-use-dx-position-w");
		request.arguments.emplace_back(L"-fspv-use-unknown-image-format");

		// The optimizer removes unread inputs and keeps its numbering, while FXC's registers count them. So the interface comes from a
		// build that stops before it, the only one with -fspv-reflect, which would otherwise add extensions to the module the driver sees.
		auto interfaceRequest = request;
		interfaceRequest.arguments.insert(interfaceRequest.arguments.end(), { L"-fcgl", L"-fspv-reflect" });

		auto& compiler = Compiler();
		const auto optimized = compiler.Compile(std::move(request));
		result.diagnostics = optimized.diagnostics;
		result.variantKey = optimized.variantKey;
		if (!optimized)
			return result;

		const bool hasInterface = !std::string_view(a_request.target).starts_with("cs");
		std::vector<uint32_t> placed;
		std::vector<std::byte> inputs, outputs;
		bool fromStore = optimized.fromCache;
		if (hasInterface) {
			const auto whole = compiler.Compile(std::move(interfaceRequest));
			if (!whole) {
				result.diagnostics = whole.diagnostics;
				return result;
			}
			fromStore = fromStore && whole.fromCache;
			const auto stageIo = Spirv::ReflectInterface(Words(whole.binary));
			const auto placedInputs = Spirv::PlaceLikeFxc(stageIo.inputs, !std::string_view(a_request.target).starts_with("vs"));
			const auto placedOutputs = Spirv::PlaceLikeFxc(stageIo.outputs, true);
			placed = Spirv::PlaceVariables(Words(optimized.binary), stageIo, placedInputs, placedOutputs);
			inputs = SignatureChunk(stageIo.inputs, placedInputs);
			outputs = SignatureChunk(stageIo.outputs, placedOutputs);
		} else {
			const auto module = Words(optimized.binary);
			placed.assign(module.begin(), module.end());
			inputs = SignatureChunk({}, {});
			outputs = SignatureChunk({}, {});
		}

		for (const auto& buffer : Spirv::ReflectConstantBuffers(placed)) {
			if (buffer.name.ends_with("Globals")) {
				result.diagnostics = "It declares constants outside a constant buffer, which DXC places differently from FXC";
				return result;
			}
		}

		result.blob = MakeContainer(std::move(inputs), std::move(outputs), placed);
		result.fromStore = fromStore;
		return result;
	}

	const ShaderReflection::Buffer* ShaderReflection::FindBuffer(std::string_view a_name) const
	{
		const auto found = std::ranges::find_if(buffers, [&](const Buffer& a_buffer) { return a_buffer.name == a_name; });
		return found != buffers.end() ? &*found : nullptr;
	}

	bool ReflectShader(ID3DBlob& a_blob, ShaderReflection& a_reflection)
	{
		const auto code = FindChunk(a_blob, Tag("SPRV"));
		if (code.empty())
			return ReflectDxbc(a_blob, a_reflection);

		// Copied: a chunk need not be aligned for its words.
		std::vector<uint32_t> words(code.size() / sizeof(uint32_t));
		std::memcpy(words.data(), code.data(), words.size() * sizeof(uint32_t));
		for (auto& buffer : Spirv::ReflectConstantBuffers(words)) {
			auto& reflected = a_reflection.buffers.emplace_back();
			reflected.name = std::move(buffer.name);
			reflected.size = buffer.size;
			for (auto& variable : buffer.variables)
				reflected.variables.push_back({ std::move(variable.name), variable.offset, variable.size, variable.elements });
		}

		const auto signature = FindChunk(a_blob, Tag("ISGN"));
		if (signature.size() < 8)
			return true;
		uint32_t count = 0;
		std::memcpy(&count, signature.data(), sizeof(count));
		for (uint32_t i = 0; i < count && 8 + 24 * size_t(i + 1) <= signature.size(); ++i) {
			uint32_t nameOffset = 0, index = 0;
			std::memcpy(&nameOffset, signature.data() + 8 + 24 * i, sizeof(nameOffset));
			std::memcpy(&index, signature.data() + 8 + 24 * i + 4, sizeof(index));
			if (nameOffset < signature.size())
				a_reflection.inputs.push_back({ reinterpret_cast<const char*>(signature.data() + nameOffset), index });
		}
		return true;
	}
}
