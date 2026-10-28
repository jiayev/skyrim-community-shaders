// Run after prepare_shaders, from the repository root.
// cl /std:c++20 /O2 /EHsc tests/EnvironmentProbeShadows.cpp /link d3d11.lib d3dcompiler.lib
#define NOMINMAX
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <fstream>
#include <stdexcept>
#include <string>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
static void Check(HRESULT hr)
{
	if (FAILED(hr))
		throw std::runtime_error(std::to_string(hr));
}

struct Includes : ID3DInclude
{
	HRESULT Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* size) override
	{
		std::ifstream file(std::string("build/ALL/aio/Shaders/") + name, std::ios::binary | std::ios::ate);
		if (!file)
			return E_FAIL;
		*size = static_cast<UINT>(file.tellg());
		auto* bytes = new char[*size];
		file.seekg(0);
		file.read(bytes, *size);
		*data = bytes;
		return S_OK;
	}
	HRESULT Close(LPCVOID data) override
	{
		delete[] static_cast<const char*>(data);
		return S_OK;
	}
};

int main()
try {
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
	Includes includes;
	constexpr char source[] = R"(
#include "Common/EnvironmentProbe.hlsli"
#include "Common/EnvironmentProbeShadows.hlsli"
RWStructuredBuffer<float> Results : register(u0);
[numthreads(1, 1, 1)] void main() {
    // Native CSM still compares receivers behind the far plane.
    Results[0] = EnvironmentProbe::GetSunShadow(float3(.5, .5, 1.002));
    // Receiver bias prevents acne, including a receiver just beyond the map's far plane.
    Results[1] = EnvironmentProbe::GetSunShadow(float3(.5, .5, .902));
    Results[2] = EnvironmentProbe::GetSunShadow(float3(.5, .5, -.1));
    // A coarser cascade covers receivers outside the near footprint.
    Results[3] = EnvironmentProbe::GetSunShadow(float3(1.5, .5, 1.002));
    Results[4] = EnvironmentProbe::GetSunShadow(float3(3, .5, 1.002));
    Results[5] = EnvironmentProbe::GetSunShadow(float3(1.5, .5, .998));
    Results[6] = EnvironmentProbe::GetSunShadow(float3(.75, .75, 1.002));
    Results[7] = EnvironmentProbe::GetSunShadow(float3(.5, .75, 1.002));
})";
	ComPtr<ID3DBlob> code, errors;
	auto hr = D3DCompile(source, sizeof(source) - 1, nullptr, nullptr, &includes, "main", "cs_5_0",
		D3DCOMPILE_WARNINGS_ARE_ERRORS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
	if (errors)
		std::fputs(static_cast<const char*>(errors->GetBufferPointer()), stderr);
	Check(hr);
	ComPtr<ID3D11ComputeShader> shader;
	Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));
	context->CSSetShader(shader.Get(), nullptr, 0);

	std::array<float, 4> depth{ .9f, .9f, .9f, 1.f }, farDepth{ .99f, .99f, .99f, .99f };
	D3D11_SUBRESOURCE_DATA pixels[] = { { depth.data(), 8, 0 }, { farDepth.data(), 8, 0 } };
	D3D11_TEXTURE2D_DESC textureDesc{};
	textureDesc.Width = textureDesc.Height = 2;
	textureDesc.MipLevels = 1;
	textureDesc.ArraySize = 2;
	textureDesc.Format = DXGI_FORMAT_R32_FLOAT;
	textureDesc.SampleDesc.Count = 1;
	textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	ComPtr<ID3D11Texture2D> texture;
	ComPtr<ID3D11ShaderResourceView> srv;
	Check(device->CreateTexture2D(&textureDesc, pixels, &texture));
	Check(device->CreateShaderResourceView(texture.Get(), nullptr, &srv));
	context->CSSetShaderResources(104, 1, srv.GetAddressOf());

	std::array<float, 100> capture{};
	for (unsigned base : { 16u, 32u, 48u })
		for (unsigned diagonal : { 0u, 5u, 10u, 15u }) capture[base + diagonal] = 1;
	capture[32] = .5f;
	capture[72] = 100;
	capture[73] = 200;
	const unsigned two = 2;
	std::memcpy(&capture[75], &two, sizeof(two));
	const unsigned one = 1;
	std::memcpy(&capture[15], &one, sizeof(one));
	D3D11_BUFFER_DESC desc{};
	desc.ByteWidth = sizeof(capture);
	desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	D3D11_SUBRESOURCE_DATA initial{ capture.data() };
	ComPtr<ID3D11Buffer> captureCB, shadowCB;
	Check(device->CreateBuffer(&desc, &initial, &captureCB));
	context->CSSetConstantBuffers(8, 1, captureCB.GetAddressOf());
	std::array<float, 100> parameters{};
	parameters[9] = .0045f;
	parameters[10] = .01f;
	parameters[14] = 1.e8f;
	desc.ByteWidth = sizeof(parameters);
	initial.pSysMem = parameters.data();
	ComPtr<ID3D11Buffer> nativeCB;
	Check(device->CreateBuffer(&desc, &initial, &nativeCB));
	Check(device->CreateBuffer(&desc, nullptr, &shadowCB));
	context->CopyResource(shadowCB.Get(), nativeCB.Get());
	parameters.fill(0);
	context->UpdateSubresource(nativeCB.Get(), 0, nullptr, parameters.data(), 0, 0);
	context->CSSetConstantBuffers(13, 1, shadowCB.GetAddressOf());

	desc.ByteWidth = 8 * sizeof(float);
	desc.StructureByteStride = sizeof(float);
	desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	ComPtr<ID3D11Buffer> output, readback;
	ComPtr<ID3D11UnorderedAccessView> uav;
	Check(device->CreateBuffer(&desc, nullptr, &output));
	Check(device->CreateUnorderedAccessView(output.Get(), nullptr, &uav));
	context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
	desc.BindFlags = desc.MiscFlags = desc.StructureByteStride = 0;
	desc.Usage = D3D11_USAGE_STAGING;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	Check(device->CreateBuffer(&desc, nullptr, &readback));
	context->Dispatch(1, 1, 1);
	context->CopyResource(readback.Get(), output.Get());
	D3D11_MAPPED_SUBRESOURCE mapped{};
	Check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
	const auto* results = static_cast<const float*>(mapped.pData);
	const std::array expected{ .25f, 1.f, 1.f, 0.f, 1.f, 1.f, 1.f, .5f };
	bool passed = true;
	for (unsigned i = 0; i < expected.size(); ++i) {
		std::printf("case %u: %.6f (expected %.6f)\n", i, results[i], expected[i]);
		passed &= std::isfinite(results[i]) && std::abs(results[i] - expected[i]) < 1.e-5f;
	}
	context->Unmap(readback.Get(), 0);
	// Preferred coarse split misses XY, but the fine split still covers the point.
	capture[71] = 150;
	capture[44] = 2;
	context->UpdateSubresource(captureCB.Get(), 0, nullptr, capture.data(), 0, 0);
	context->Dispatch(1, 1, 1);
	context->CopyResource(readback.Get(), output.Get());
	Check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
	float fallback = *static_cast<const float*>(mapped.pData);
	context->Unmap(readback.Get(), 0);
	std::printf("fine fallback: %.6f (expected 0.250000)\n", fallback);
	passed &= std::isfinite(fallback) && std::abs(fallback - .25f) < 1.e-5f;
	// A coarse map fills missing CSM coverage, but never replaces covered CSM samples.
	std::array<float, 4> coarseDepth{ .2f, .2f, .2f, .2f };
	textureDesc.ArraySize = 1;
	D3D11_SUBRESOURCE_DATA coarsePixels{ coarseDepth.data(), 8, 0 };
	ComPtr<ID3D11Texture2D> coarse;
	ComPtr<ID3D11ShaderResourceView> coarseSRV;
	Check(device->CreateTexture2D(&textureDesc, &coarsePixels, &coarse));
	Check(device->CreateShaderResourceView(coarse.Get(), nullptr, &coarseSRV));
	context->CSSetShaderResources(105, 1, coarseSRV.GetAddressOf());
	capture[71] = 0;
	capture[44] = 0;
	capture[80] = .1f;
	capture[85] = 1;
	capture[90] = .5f;
	capture[95] = 1;
	std::memcpy(&capture[97], &one, sizeof(one));
	context->UpdateSubresource(captureCB.Get(), 0, nullptr, capture.data(), 0, 0);
	context->Dispatch(1, 1, 1);
	context->CopyResource(readback.Get(), output.Get());
	Check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
	const auto* hybrid = static_cast<const float*>(mapped.pData);
	std::printf("coarse fallback: %.6f, CSM priority: %.6f\n", hybrid[4], hybrid[1]);
	passed &= hybrid[4] == 0 && hybrid[1] == 1;
	context->Unmap(readback.Get(), 0);
	if (!passed)
		throw std::runtime_error("Probe shadow comparison regression");
	std::puts("PASS: native bias snapshot, all-cascade coverage, bilinear comparison");
	return 0;
} catch (const std::exception& error) {
	std::fprintf(stderr, "%s\n", error.what());
	return 1;
}
