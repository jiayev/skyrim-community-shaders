#include "Utils/FxcShaderStore.h"

#include "Globals.h"
#include "State.h"
#include "Utils/SharedShaderStore.h"

#include <fstream>
#include <mutex>

#include <nlohmann/json.hpp>

namespace Util
{
	namespace
	{
		// The format tag of a stored DXBC container. ORG's formats are DXIL 0 and SPIR-V 1.
		constexpr uint8_t kFormatDxbc = 2;

		/**
		 * @brief The link time, image size and checksum of the loaded d3dcompiler, read from its PE header in memory.
		 * A compiler update must not reuse bytecode the old one built. Empty if the module cannot be identified.
		 */
		const std::string& CompilerIdentity()
		{
			static const std::string identity = []() -> std::string {
				const HMODULE module = GetModuleHandleW(L"d3dcompiler_47.dll");
				if (!module)
					return {};
				const auto* base = reinterpret_cast<const std::byte*>(module);
				const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
				if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
					return {};
				const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dosHeader->e_lfanew);
				if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
					return {};
				return std::format("{:08x}:{:08x}:{:08x}", ntHeaders->FileHeader.TimeDateStamp, ntHeaders->OptionalHeader.SizeOfImage, ntHeaders->OptionalHeader.CheckSum);
			}();
			return identity;
		}

		/** @brief Serves includes through the store, which records each file a variant reads. FXC keeps the data only until Close. */
		class StoreInclude final : public ID3DInclude
		{
		public:
			StoreInclude(const std::filesystem::path& a_base, org::services::ShaderIncludeTracker& a_includes) :
				base(a_base), includes(a_includes) {}

			HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE, LPCSTR a_name, LPCVOID, LPCVOID* a_data, UINT* a_bytes) override
			{
				const auto* bytes = includes.Open(base / a_name);
				if (!bytes) {
					*a_data = nullptr;
					*a_bytes = 0;
					return E_FAIL;
				}
				*a_data = bytes->data();
				*a_bytes = static_cast<UINT>(bytes->size());
				return S_OK;
			}

			HRESULT STDMETHODCALLTYPE Close(LPCVOID) override { return S_OK; }

		private:
			const std::filesystem::path& base;
			org::services::ShaderIncludeTracker& includes;
		};

		std::string BlobText(ID3DBlob* a_blob)
		{
			if (!a_blob)
				return {};
			std::string text(static_cast<const char*>(a_blob->GetBufferPointer()), a_blob->GetBufferSize());
			if (!text.empty() && text.back() == '\0')
				text.pop_back();
			return text;
		}

		// A failed call with no message means FXC itself failed (out of memory and the like), which may not happen next time.
		// A failed call with a message is about the shader, and the same inputs will fail the same way.
		bool IsTransient(HRESULT a_result, const std::string& a_diagnostics)
		{
			return a_result == E_OUTOFMEMORY || a_diagnostics.empty();
		}

		// CS_FXC_REQUEST_DUMP=<folder>: one line per request in fxc-requests.jsonl, with what is needed to repeat the build outside the
		// game and its artifact's key in the store. tools/shader-compare/dxc_survey.py reads it.
		void DumpRequest(const FxcRequest& a_request, const org::services::ShaderStoreResult& a_store)
		{
			static const auto folder = []() -> std::filesystem::path {
				wchar_t value[MAX_PATH];
				const DWORD length = GetEnvironmentVariableW(L"CS_FXC_REQUEST_DUMP", value, MAX_PATH);
				return length && length < MAX_PATH ? std::filesystem::path(value) : std::filesystem::path{};
			}();
			if (folder.empty())
				return;
			static std::mutex mutex;
			static std::ofstream out = [] {
				std::error_code ec;
				std::filesystem::create_directories(folder, ec);
				return std::ofstream(folder / "fxc-requests.jsonl", std::ios::binary | std::ios::trunc);
			}();
			nlohmann::json line;
			line["root"] = Util::WStringToString(a_request.root.generic_wstring());
			line["entry"] = a_request.entryPoint;
			line["profile"] = a_request.profile;
			line["flags"] = a_request.flags;
			line["key"] = a_store.compileKey.ToHex();
			line["failed"] = a_store.outcome == org::services::ShaderStoreOutcome::Failed;
			auto& defines = line["defines"] = nlohmann::json::array();
			for (const auto* define = a_request.defines; define && define->Name; ++define)
				defines.push_back({ define->Name, define->Definition ? define->Definition : "" });
			std::lock_guard lock(mutex);
			out << line.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";
			out.flush();
		}

		class FxcBuild final : public org::services::ShaderVariantBuild
		{
		public:
			explicit FxcBuild(const FxcRequest& a_request) :
				request(a_request), rootName(Util::WStringToString(a_request.root.generic_wstring())) {}

			org::services::ShaderPreprocessOutput Preprocess(const std::string* a_root, org::services::ShaderIncludeTracker& a_includes) override
			{
				org::services::ShaderPreprocessOutput output;
				if (!a_root) {
					output.diagnostics = std::format("{} does not exist", rootName);
					return output;
				}
				StoreInclude include(request.includeBase, a_includes);
				winrt::com_ptr<ID3DBlob> preprocessed, errors;
				const HRESULT result = D3DPreprocess(a_root->data(), a_root->size(), rootName.c_str(), request.defines, &include, preprocessed.put(), errors.put());
				output.diagnostics = BlobText(errors.get());
				if (FAILED(result) || !preprocessed) {
					output.transientFailure = IsTransient(result, output.diagnostics);
					return output;
				}
				output.text = BlobText(preprocessed.get());
				output.succeeded = true;
				return output;
			}

			org::services::ShaderCompileOutput Compile(const std::string*, const org::services::ShaderPreprocessOutput& a_preprocessed, org::services::ShaderIncludeTracker&) override
			{
				org::services::ShaderCompileOutput output;
				// The preprocessed text is exactly what the key was built from, so the output always matches its key.
				winrt::com_ptr<ID3DBlob> compiled, errors;
				const HRESULT result = D3DCompile(a_preprocessed.text.data(), a_preprocessed.text.size(), rootName.c_str(), nullptr, nullptr, request.entryPoint, request.profile,
					request.flags, 0, compiled.put(), errors.put());
				output.diagnostics = BlobText(errors.get());
				if (FAILED(result) || !compiled) {
					output.transientFailure = IsTransient(result, output.diagnostics);
					return output;
				}
				if (request.stripFlags) {
					winrt::com_ptr<ID3DBlob> stripped;
					if (SUCCEEDED(D3DStripShader(compiled->GetBufferPointer(), compiled->GetBufferSize(), request.stripFlags, stripped.put())))
						compiled = std::move(stripped);
				}
				const auto* first = static_cast<const std::byte*>(compiled->GetBufferPointer());
				output.binary.assign(first, first + compiled->GetBufferSize());
				output.succeeded = true;
				return output;
			}

		private:
			const FxcRequest& request;
			std::string rootName;
		};
	}

	uint32_t FxcCompileFlags(bool a_debug)
	{
		uint32_t flags = a_debug ? D3DCOMPILE_DEBUG : (D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_SKIP_VALIDATION);
		if (globals::state->enablePartialPrecision.load(std::memory_order_relaxed))
			flags |= D3DCOMPILE_PARTIAL_PRECISION;
		if (globals::state->enableAvoidFlowControl.load(std::memory_order_relaxed))
			flags |= D3DCOMPILE_AVOID_FLOW_CONTROL;
		return flags;
	}

	FxcResult ResolveFxcShader(const FxcRequest& a_request)
	{
		using namespace org::services;

		ShaderKeyBuilder config;
		config.Add(std::string_view("fxc")).Add(std::string_view(CompilerIdentity())).Add(std::string_view(a_request.entryPoint)).Add(std::string_view(a_request.profile));
		config.Add(static_cast<uint64_t>(a_request.flags)).Add(static_cast<uint64_t>(a_request.stripFlags));

		ShaderKeyBuilder variant;
		const auto configKey = config.Finish();
		variant.Add(configKey).Add(std::wstring_view(a_request.root.generic_wstring())).Add(std::wstring_view(a_request.includeBase.generic_wstring()));
		for (const auto* define = a_request.defines; define && define->Name; ++define)
			variant.Add(std::string_view(define->Name)).Add(std::string_view(define->Definition ? define->Definition : ""));

		FxcBuild build(a_request);
		ShaderVariantRequest request;
		request.variantKey = variant.Finish();
		request.compileConfigKey = configKey;
		request.debug = a_request.debug;
		request.format = kFormatDxbc;
		request.rootPath = a_request.root;
		request.build = &build;

		FxcResult result;
		result.variantKey = request.variantKey;
		result.store = SharedShaderStore()->Resolve(request);
		DumpRequest(a_request, result.store);
		if (result.store.outcome != ShaderStoreOutcome::Failed && SUCCEEDED(D3DCreateBlob(result.store.binary.size(), result.blob.put()))) {
			std::memcpy(result.blob->GetBufferPointer(), result.store.binary.data(), result.store.binary.size());
			result.store.binary = {};
		}
		return result;
	}

	std::vector<std::string> FxcDependencyPaths(const org::services::ShaderStoreHash& a_variantKey)
	{
		std::vector<std::string> paths;
		SharedShaderStore()->ForEachDependency(a_variantKey, [&](const std::filesystem::path& a_path) {
			std::error_code ec;
			const auto canonical = std::filesystem::weakly_canonical(a_path, ec);
			auto text = Util::WStringToString((ec ? a_path : canonical).wstring());
			std::ranges::transform(text, text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			paths.push_back(std::move(text));
		});
		return paths;
	}
}
