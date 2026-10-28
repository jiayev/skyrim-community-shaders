// Run after prepare_shaders, from the repository root.
// cl /std:c++20 /O2 /EHsc tests/EnvironmentProbeLighting.cpp /link d3d11.lib d3dcompiler.lib
#define NOMINMAX
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
using Vec = std::array<float, 3>;
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

struct Capture
{
	Vec sign{};
	uint32_t mask = 255;
	float nearDistance = 1, farDistance = 16384;
	uint32_t lightCount{}, size = 512;
	float worldOrigin[4]{};
	Vec lightOrigin{};
	float pad{};
};
struct Light
{
	float material[8]{};
	Vec position{};
	float w{};
	uint32_t room[4]{}, flags{}, shadow{}, local{}, pad{};
};
static_assert(sizeof(Capture) == 64 && sizeof(Light) == 80);

static Vec Ray(float u, float v)
{
	Vec p{ u * 2 - 1, v * 2 - 1, 0 };
	p[2] = 1 - std::abs(p[0]) - std::abs(p[1]);
	const float t = std::max(-p[2], 0.f);
	p[0] += p[0] >= 0 ? -t : t;
	p[1] += p[1] >= 0 ? -t : t;
	const float length = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
	for (auto& c : p) c /= length;
	return p;
}

int main()
try {
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
	Includes includes;
	ComPtr<ID3DBlob> code, errors;
	const auto hr = D3DCompileFromFile(L"build/ALL/aio/Shaders/LightLimitFix/EnvironmentProbeGrid.cs.hlsl", nullptr, &includes,
		"main", "cs_5_0", D3DCOMPILE_WARNINGS_ARE_ERRORS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
	if (errors)
		std::fputs(static_cast<const char*>(errors->GetBufferPointer()), stderr);
	Check(hr);
	ComPtr<ID3D11ComputeShader> shader;
	Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));
	context->CSSetShader(shader.Get(), nullptr, 0);
	D3D11_BUFFER_DESC desc{};
	desc.ByteWidth = 1024 * sizeof(Light);
	desc.StructureByteStride = sizeof(Light);
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	ComPtr<ID3D11Buffer> lights, masks, readback, constants;
	Check(device->CreateBuffer(&desc, nullptr, &lights));
	ComPtr<ID3D11ShaderResourceView> srv;
	Check(device->CreateShaderResourceView(lights.Get(), nullptr, &srv));
	context->CSSetShaderResources(0, 1, srv.GetAddressOf());
	desc.ByteWidth = 16 * 16 * 24 * 33 * sizeof(uint32_t);
	desc.StructureByteStride = sizeof(uint32_t);
	desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	Check(device->CreateBuffer(&desc, nullptr, &masks));
	ComPtr<ID3D11UnorderedAccessView> uav;
	Check(device->CreateUnorderedAccessView(masks.Get(), nullptr, &uav));
	context->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
	desc.BindFlags = desc.MiscFlags = desc.StructureByteStride = 0;
	desc.Usage = D3D11_USAGE_STAGING;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	Check(device->CreateBuffer(&desc, nullptr, &readback));
	desc = {};
	desc.ByteWidth = sizeof(Capture);
	desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	Check(device->CreateBuffer(&desc, nullptr, &constants));
	context->CSSetConstantBuffers(8, 1, constants.GetAddressOf());
	std::mt19937 random(2781);
	std::uniform_real_distribution<float> uniform(0, 1);
	uint64_t verified = 0, candidates = 0;
	unsigned cases = 0;
	for (uint32_t count : { 0u, 1u, 31u, 32u, 33u, 1024u }) {
		for (bool dense : { false, true }) {
			Capture capture;
			capture.lightCount = count;
			capture.lightOrigin = { 120000, -230000, 512 };
			std::vector<Light> data(1024);
			for (uint32_t i = 0; i < count; ++i) {
				auto direction = Ray(uniform(random), uniform(random));
				float distance = std::exp2(uniform(random) * 14);
				data[i].material[4] = dense ? 40000.f : std::exp2(uniform(random) * 12 - 2);
				for (unsigned c = 0; c < 3; ++c) data[i].position[c] = capture.lightOrigin[c] + direction[c] * distance;
				if (i % 19 == 0)
					data[i].flags = 1u << 9;
			}
			context->UpdateSubresource(lights.Get(), 0, nullptr, data.data(), 0, 0);
			context->UpdateSubresource(constants.Get(), 0, nullptr, &capture, 0, 0);
			context->Dispatch(16, 16, 24);
			context->CopyResource(readback.Get(), masks.Get());
			D3D11_MAPPED_SUBRESOURCE mapped{};
			Check(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
			const auto* words = static_cast<const uint32_t*>(mapped.pData);
			for (uint32_t z = 0; z < 24; ++z)
				for (uint32_t y = 0; y < 16; ++y)
					for (uint32_t x = 0; x < 16; ++x) {
						const auto* cluster = words + (x + 16 * (y + 16 * z)) * 33;
						uint32_t active = 0;
						for (uint32_t word = 0; word < 32; ++word)
							if (cluster[1 + word])
								active |= 1u << word;
						if (active != cluster[0])
							throw std::runtime_error("Invalid active-word mask");
						for (uint32_t i = 0; i < 1024; ++i) {
							bool present = (cluster[1 + i / 32] & (1u << (i % 32))) != 0;
							candidates += present;
							bool enabled = i < count && !(data[i].flags & (1u << 9));
							if ((!enabled && present) || (dense && enabled && !present))
								throw std::runtime_error("Disabled, tail, or dense light mask mismatch");
							if (!enabled || dense || present)
								continue;
							// Independent point-in-sphere oracle, including tile/fold/axis corners
							// and both radial slice boundaries. No cone approximation in the oracle.
							for (auto uv : { std::array{ 0.f, 0.f }, { 1.f, 0.f }, { 1.f, 1.f }, { 0.f, 1.f }, { .5f, .5f } }) {
								auto ray = Ray((x + uv[0]) / 16, (y + uv[1]) / 16);
								for (float slice : { 0.f, .5f, 1.f }) {
									float radius = std::exp2((z + slice) * 14 / 24);
									float d2 = 0;
									for (unsigned c = 0; c < 3; ++c) {
										float d = data[i].position[c] - capture.lightOrigin[c] - ray[c] * radius;
										d2 += d * d;
									}
									if (d2 < data[i].material[4] * data[i].material[4])
										throw std::runtime_error("Grid culled a contributing light");
									++verified;
								}
							}
						}
					}
			context->Unmap(readback.Get(), 0);
			++cases;
		}
	}
	std::printf("Passed %u WARP grid cases; %llu point/light checks, %llu candidates. No missing lights, including folds, radial boundaries, 1024-light overflow and translated origins.\n", cases, verified, candidates);
	return 0;
} catch (const std::exception& error) {
	std::fprintf(stderr, "%s\n", error.what());
	return 1;
}
