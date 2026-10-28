// Standalone GPU regression of the production visibility sampler. Run from the
// repository root after prepare_shaders. Link d3d11.lib and d3dcompiler.lib.
#define NOMINMAX
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

static void Check(HRESULT hr)
{
	if (FAILED(hr))
		throw std::runtime_error("D3D failure " + std::to_string(hr));
}

struct Includes : ID3DInclude
{
	HRESULT Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* bytes) override
	{
		std::ifstream file(std::string("build/ALL/aio/Shaders/") + name, std::ios::binary);
		if (!file)
			return E_FAIL;
		std::string text(std::istreambuf_iterator<char>(file), {});
		auto* copy = new char[text.size()];
		std::memcpy(copy, text.data(), text.size());
		*data = copy;
		*bytes = UINT(text.size());
		return S_OK;
	}
	HRESULT Close(LPCVOID data) override
	{
		delete[] static_cast<const char*>(data);
		return S_OK;
	}
};
struct Case
{
	unsigned mask, rays;
	float angle, normalLength;
};
struct Result
{
	std::array<float, 4> integral, moment;
};

int main()
try {
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
	Includes includes;
	ComPtr<ID3DBlob> code, errors;
	auto hr = D3DCompileFromFile(L"tests/SSGIProbeSampling.hlsl", nullptr, &includes, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &code, &errors);
	if (errors)
		std::fputs(static_cast<const char*>(errors->GetBufferPointer()), stderr);
	Check(hr);
	ComPtr<ID3D11ComputeShader> shader;
	Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));
	std::vector<Case> cases;
	unsigned seed = 91218;
	for (unsigned i = 0; i < 64; ++i) {
		seed = seed * 1664525u + 1013904223u;
		unsigned mask = i == 0 ? 0 : i == 1 ? ~0u :
		                         i == 2     ? 0x81u :
		                         i == 3     ? 0x80000001u :
		                                      seed;
		float angle = (float(i) / 63 - 0.5f) * 3.0f;
		for (unsigned rays = 1; rays <= 4; ++rays)
			cases.push_back({ mask, rays, angle, 0.2f + 0.8f * float(i % 7) / 6 });
	}
	D3D11_BUFFER_DESC desc{};
	desc.ByteWidth = UINT(cases.size() * sizeof(Case));
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	desc.StructureByteStride = sizeof(Case);
	D3D11_SUBRESOURCE_DATA initial{ cases.data(), 0, 0 };
	ComPtr<ID3D11Buffer> input;
	Check(device->CreateBuffer(&desc, &initial, &input));
	ComPtr<ID3D11ShaderResourceView> srv;
	Check(device->CreateShaderResourceView(input.Get(), nullptr, &srv));
	desc.ByteWidth = UINT(cases.size() * 1024 * sizeof(Result));
	desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	desc.StructureByteStride = sizeof(Result);
	ComPtr<ID3D11Buffer> output;
	Check(device->CreateBuffer(&desc, nullptr, &output));
	ComPtr<ID3D11UnorderedAccessView> uav;
	Check(device->CreateUnorderedAccessView(output.Get(), nullptr, &uav));
	context->CSSetShader(shader.Get(), nullptr, 0);
	context->CSSetShaderResources(0, 1, srv.GetAddressOf());
	context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
	context->Dispatch(UINT(cases.size() * 16), 1, 1);
	ID3D11UnorderedAccessView* none = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &none, nullptr);
	desc.Usage = D3D11_USAGE_STAGING;
	desc.BindFlags = desc.MiscFlags = desc.StructureByteStride = 0;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ComPtr<ID3D11Buffer> staging;
	Check(device->CreateBuffer(&desc, nullptr, &staging));
	context->CopyResource(staging.Get(), output.Get());
	D3D11_MAPPED_SUBRESOURCE mapped{};
	Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
	const auto* results = static_cast<const Result*>(mapped.pData);
	constexpr double pi = 3.14159265358979323846;
	double worstError = 0;
	for (size_t c = 0; c < cases.size(); ++c) {
		const auto& data = cases[c];
		std::array<double, 7> expected{}, mean{};
		for (unsigned bit = 0; bit < 32; ++bit) {
			if (!(data.mask & (1u << bit)))
				continue;
			double angle = pi / 2 - (bit + 0.5) * pi / 32;
			double x = std::sin(data.angle + angle), z = std::cos(data.angle + angle);
			double w = data.normalLength * std::cos(angle) * std::abs(x) * std::sin(pi / 32);
			double green = 0.5 + 0.3 * x + 0.2 * z;
			expected[0] += w;
			expected[1] += w * green;
			expected[2] += w * (0.25 + 0.5 * x * x);
			expected[3] += w;
			expected[4] += w * green * x;
			expected[6] += w * green * z;
		}
		for (unsigned i = 0; i < 1024; ++i) {
			const auto& result = results[c * 1024 + i];
			if (result.moment[3] != 0)
				throw std::runtime_error("Ray escaped open mask in case " + std::to_string(c));
			if (std::abs(result.integral[0] - expected[0]) > 2e-6 || std::abs(result.integral[3] - expected[3]) > 2e-6)
				throw std::runtime_error("White furnace / probability compensation failed");
			for (unsigned k = 0; k < 4; ++k) mean[k] += result.integral[k] / 1024.0;
			for (unsigned k = 0; k < 3; ++k) mean[k + 4] += result.moment[k] / 1024.0;
		}
		for (unsigned k = 0; k < 7; ++k) {
			double error = std::abs(mean[k] - expected[k]);
			worstError = std::max(error, worstError);
			if (error > 0.002)
				throw std::runtime_error("Directional integral is biased in case " + std::to_string(c));
		}
	}
	context->Unmap(staging.Get(), 0);
	std::printf("PASS: %zu GPU estimates, disjoint masks, 1-4 rays, white-furnace energy and directional moments; max integral error %.8f\n", cases.size() * 1024, worstError);
	return 0;
} catch (const std::exception& error) {
	std::fprintf(stderr, "FAIL: %s\n", error.what());
	return 1;
}
