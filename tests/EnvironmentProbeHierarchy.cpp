// Production D3D11 kernels versus analytic geometry, plus conservative pyramid
// bounds and incremental face preservation. Run after prepare_shaders, from the
// repository root. Link d3d11.lib and d3dcompiler.lib. Optional --hardware.
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
struct F
{
	float x{}, y{}, z{}, w{};
};
static F Add(F a, F b) { return { a.x + b.x, a.y + b.y, a.z + b.z, 0 }; }
static F Mul(F a, float b) { return { a.x * b, a.y * b, a.z * b, 0 }; }
static float Dot(F a, F b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static F Normalize(F a) { return Mul(a, 1 / std::sqrt(Dot(a, a))); }
static unsigned Face(F a) { return (a.x < 0 ? 1u : 0u) | (a.y < 0 ? 2u : 0u) | (a.z < 0 ? 4u : 0u); }
static F Decode(float u, float v)
{
	F a{ u * 2 - 1, v * 2 - 1, 0, 0 };
	a.z = 1 - std::abs(a.x) - std::abs(a.y);
	float t = std::max(0.f, -a.z);
	a.x += a.x >= 0 ? -t : t;
	a.y += a.y >= 0 ? -t : t;
	return Normalize(a);
}
static void Check(HRESULT hr)
{
	if (FAILED(hr))
		throw std::runtime_error("D3D failure " + std::to_string(hr));
}
struct Includes : ID3DInclude
{
	HRESULT Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* size) override
	{
		std::ifstream file(std::string("build/ALL/aio/Shaders/") + name, std::ios::binary);
		if (!file)
			return E_FAIL;
		std::string s(std::istreambuf_iterator<char>(file), {});
		char* p = new char[s.size()];
		std::memcpy(p, s.data(), s.size());
		*data = p;
		*size = UINT(s.size());
		return S_OK;
	}
	HRESULT Close(LPCVOID data) override
	{
		delete[] static_cast<const char*>(data);
		return S_OK;
	}
};
static ComPtr<ID3D11ComputeShader> Compile(ID3D11Device* device, const wchar_t* path, const char* define = nullptr)
{
	D3D_SHADER_MACRO macros[] = { { define, "" }, { nullptr, nullptr } };
	Includes includes;
	ComPtr<ID3DBlob> code, errors;
	HRESULT hr = D3DCompileFromFile(path, define ? macros : nullptr, &includes, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &code, &errors);
	if (errors)
		std::fputs(static_cast<const char*>(errors->GetBufferPointer()), stderr);
	Check(hr);
	ComPtr<ID3D11ComputeShader> shader;
	Check(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader));
	return shader;
}
struct Texture
{
	ComPtr<ID3D11Texture2D> resource;
	ComPtr<ID3D11ShaderResourceView> srv;
	ComPtr<ID3D11UnorderedAccessView> uav;
};
static Texture MakeTexture(ID3D11Device* device, DXGI_FORMAT format, unsigned sizeX, unsigned sizeY, unsigned mips = 1, unsigned layers = 1)
{
	Texture t;
	D3D11_TEXTURE2D_DESC d{};
	d.Width = sizeX;
	d.Height = sizeY;
	d.MipLevels = mips;
	d.ArraySize = layers;
	d.Format = format;
	d.SampleDesc.Count = 1;
	d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	Check(device->CreateTexture2D(&d, nullptr, &t.resource));
	Check(device->CreateShaderResourceView(t.resource.Get(), nullptr, &t.srv));
	if (mips == 1)
		Check(device->CreateUnorderedAccessView(t.resource.Get(), nullptr, &t.uav));
	return t;
}
static std::vector<float> Read(ID3D11Device* device, ID3D11DeviceContext* context, const Texture& t, unsigned mip, unsigned slice, unsigned components)
{
	D3D11_TEXTURE2D_DESC d{};
	t.resource->GetDesc(&d);
	unsigned subresource = mip + slice * d.MipLevels;
	d.Width = std::max(1u, d.Width >> mip);
	d.Height = std::max(1u, d.Height >> mip);
	d.MipLevels = d.ArraySize = 1;
	d.Usage = D3D11_USAGE_STAGING;
	d.BindFlags = 0;
	d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ComPtr<ID3D11Texture2D> staging;
	Check(device->CreateTexture2D(&d, nullptr, &staging));
	context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, t.resource.Get(), subresource, nullptr);
	D3D11_MAPPED_SUBRESOURCE mapped{};
	Check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
	std::vector<float> result(size_t(d.Width) * d.Height * components);
	for (unsigned y = 0; y < d.Height; ++y) std::memcpy(result.data() + size_t(y) * d.Width * components, static_cast<const char*>(mapped.pData) + y * mapped.RowPitch, d.Width * components * 4);
	context->Unmap(staging.Get(), 0);
	return result;
}
struct Ray
{
	F origin, direction;
};
struct Constants
{
	F prefix[5]{};
	unsigned useProbe = 1, valid = 255, steps = 128;
	float farDistance = 16384;
	F origins[8]{};
	float bounds = 17000;
	unsigned reflectionValid{};
	float pad[2]{};
};
static_assert(sizeof(Constants) == 240);
struct BuildData
{
	unsigned faces[8]{};
	unsigned size = 256;
	float farDistance = 16384;
	unsigned sharedSeams = 255;
	unsigned padding{};
};
static_assert(sizeof(BuildData) == 48);

int main(int argc, char** argv)
try {
	bool hardware = argc > 1 && std::string(argv[1]) == "--hardware";
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	Check(D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
	auto base = Compile(device.Get(), L"build/ALL/aio/Shaders/ScreenSpaceReflections/probeDepth.cs.hlsl");
	auto reduce = Compile(device.Get(), L"build/ALL/aio/Shaders/ScreenSpaceReflections/probeDepth.cs.hlsl", "REDUCE_PROBE_DEPTH");
	auto trace = Compile(device.Get(), L"tests/EnvironmentProbeHierarchy.hlsl", "PROBE_HIERARCHY");
	auto reference = Compile(device.Get(), L"tests/EnvironmentProbeHierarchy.hlsl");
	constexpr unsigned size = 512, side = size / 2, mips = 9, count = 1024;
	auto depth = MakeTexture(device.Get(), DXGI_FORMAT_R32_FLOAT, size, size);
	auto color = MakeTexture(device.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, size, size);
	auto pyramid = MakeTexture(device.Get(), DXGI_FORMAT_R32G32_FLOAT, side, side, mips, 8);
	auto output = MakeTexture(device.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, count, 256);
	std::array<ComPtr<ID3D11ShaderResourceView>, mips> views;
	std::array<ComPtr<ID3D11UnorderedAccessView>, mips> outputs;
	for (unsigned mip = 0; mip < mips; ++mip) {
		D3D11_SHADER_RESOURCE_VIEW_DESC s{};
		s.Format = DXGI_FORMAT_R32G32_FLOAT;
		s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		s.Texture2DArray = { mip, 1, 0, 8 };
		Check(device->CreateShaderResourceView(pyramid.resource.Get(), &s, &views[mip]));
		D3D11_UNORDERED_ACCESS_VIEW_DESC u{};
		u.Format = s.Format;
		u.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
		u.Texture2DArray = { mip, 0, 8 };
		Check(device->CreateUnorderedAccessView(pyramid.resource.Get(), &u, &outputs[mip]));
	}
	ComPtr<ID3D11Buffer> cb, buildCB, rayBuffer;
	D3D11_BUFFER_DESC bd{};
	bd.ByteWidth = sizeof(Constants);
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	Check(device->CreateBuffer(&bd, nullptr, &cb));
	bd.ByteWidth = sizeof(BuildData);
	Check(device->CreateBuffer(&bd, nullptr, &buildCB));
	bd.ByteWidth = count * sizeof(Ray);
	bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	bd.StructureByteStride = sizeof(Ray);
	Check(device->CreateBuffer(&bd, nullptr, &rayBuffer));
	ComPtr<ID3D11ShaderResourceView> rayView;
	Check(device->CreateShaderResourceView(rayBuffer.Get(), nullptr, &rayView));
	D3D11_SAMPLER_DESC sd{};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	ComPtr<ID3D11SamplerState> sampler;
	Check(device->CreateSamplerState(&sd, &sampler));
	context->CSSetSamplers(1, 1, sampler.GetAddressOf());
	std::vector<F> colors(size * size, F{ 1, 2, 4, 1 });
	std::vector<float> depths(size * size);
	std::vector<Ray> rays(count);
	context->UpdateSubresource(color.resource.Get(), 0, nullptr, colors.data(), size * 16, 0);
	unsigned seed = 7171;
	auto random = [&]() {seed=seed*1664525u+1013904223u;return float(seed>>8)/16777216.f; };
	for (auto& ray : rays) {
		ray.origin = { (random() - .5f) * 1000, (random() - .5f) * 1000, (random() - .5f) * 1000, 10 };
		ray.direction = Normalize({ random() - .5f, random() - .5f, random() - .5f, 0 });
		ray.direction.w = 5;
	}
	const auto originalRays = rays;
	auto unbind = [&]() {ID3D11ShaderResourceView* nil[11]{};ID3D11UnorderedAccessView* out[5]{};context->CSSetShaderResources(0,11,nil);context->CSSetUnorderedAccessViews(0,5,out,nullptr); };
	auto build = [&](unsigned mask, unsigned sharedSeams = 255) {
		BuildData data;
		data.sharedSeams = sharedSeams;
		unsigned n = 0;
		for (unsigned f = 0; f < 8; ++f)
			if (mask & (1u << f))
				data.faces[n++] = f;
		unbind();
		context->UpdateSubresource(buildCB.Get(), 0, nullptr, &data, 0, 0);
		context->CSSetConstantBuffers(0, 1, buildCB.GetAddressOf());
		context->CSSetShaderResources(0, 1, depth.srv.GetAddressOf());
		ID3D11UnorderedAccessView* out[5];
		for (unsigned i = 0; i < 5; ++i) out[i] = outputs[i].Get();
		context->CSSetUnorderedAccessViews(0, 5, out, nullptr);
		context->CSSetShader(base.Get(), nullptr, 0);
		context->Dispatch(16, 16, n);
		unbind();
		data.size = 16;
		context->UpdateSubresource(buildCB.Get(), 0, nullptr, &data, 0, 0);
		context->CSSetShaderResources(0, 1, views[4].GetAddressOf());
		out[0] = nullptr;
		for (unsigned i = 1; i < 5; ++i) out[i] = outputs[i + 4].Get();
		context->CSSetUnorderedAccessViews(0, 5, out, nullptr);
		context->CSSetShader(reduce.Get(), nullptr, 0);
		context->Dispatch(1, 1, n);
		unbind();
	};
	ComPtr<ID3D11Query> beginTime, endTime, disjoint;
	bool timing = false;
	double milliseconds = 0;
	if (hardware) {
		D3D11_QUERY_DESC q{ D3D11_QUERY_TIMESTAMP, 0 };
		Check(device->CreateQuery(&q, &beginTime));
		Check(device->CreateQuery(&q, &endTime));
		q.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
		Check(device->CreateQuery(&q, &disjoint));
	}
	auto dispatch = [&](ID3D11ComputeShader* shader, Constants constants) {
		unbind();
		context->UpdateSubresource(cb.Get(), 0, nullptr, &constants, 0, 0);
		context->UpdateSubresource(rayBuffer.Get(), 0, nullptr, rays.data(), 0, 0);
		context->CSSetConstantBuffers(1, 1, cb.GetAddressOf());
		context->CSSetShaderResources(0, 1, rayView.GetAddressOf());
		ID3D11ShaderResourceView* inputs[] = { color.srv.Get(), depth.srv.Get() };
		context->CSSetShaderResources(7, 2, inputs);
		context->CSSetShaderResources(10, 1, pyramid.srv.GetAddressOf());
		context->CSSetUnorderedAccessViews(0, 1, output.uav.GetAddressOf(), nullptr);
		context->CSSetShader(shader, nullptr, 0);
		if (timing) {
			context->Begin(disjoint.Get());
			context->End(beginTime.Get());
		}
		context->Dispatch(timing ? 4096 : count / 64, 1, 1);
		if (timing) {
			context->End(endTime.Get());
			context->End(disjoint.Get());
		}
		unbind();
		auto result = Read(device.Get(), context.Get(), output, 0, 0, 4);
		if (timing) {
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT clock{};
			UINT64 a = 0, b = 0;
			Check(context->GetData(disjoint.Get(), &clock, sizeof(clock), 0));
			Check(context->GetData(beginTime.Get(), &a, sizeof(a), 0));
			Check(context->GetData(endTime.Get(), &b, sizeof(b), 0));
			if (clock.Disjoint)
				throw std::runtime_error("unstable GPU timestamp");
			milliseconds = double(b - a) * 1000 / clock.Frequency;
		}
		return result;
	};
	Constants constants;
	unsigned checked = 0;
	for (unsigned scenario = 0; scenario < 9; ++scenario) {
		if (scenario == 8)
			for (auto& ray : rays) {
				ray.origin = { 2100, 0, 0, 10 };
				ray.direction = Normalize(Add(Mul(Normalize({ random() - .5f, random() - .5f, random() - .5f, 0 }), 900), Mul(ray.origin, -1)));
				ray.direction.w = 5;
			}
		constants.valid = scenario == 2 ? 0x55u : scenario == 6 ? 0u :
		                                                          255u;
		for (unsigned f = 0; f < 8; ++f) constants.origins[f] = scenario == 1 ? F{ float(int(f) - 4) * 3, float(f) * 2, -float(f), 0 } : F{};
		for (unsigned y = 0; y < size; ++y)
			for (unsigned x = 0; x < size; ++x) {
				F d = Decode((x + .5f) / size, (y + .5f) / size), o = constants.origins[Face(d)];
				float b = Dot(o, d);
				float distance = scenario == 7 ? 2000 / std::max({ std::abs(d.x), std::abs(d.y), std::abs(d.z) }) : -b + std::sqrt(b * b - Dot(o, o) + 2000 * 2000);
				depths[size_t(y) * size + x] = scenario == 3 ? 16384.f : scenario == 4 ? NAN :
				                                                                         distance;
			}
		context->UpdateSubresource(depth.resource.Get(), 0, nullptr, depths.data(), size * 4, 0);
		build(255, scenario == 1 || scenario == 2 || scenario == 6 ? 0u : 255u);
		for (auto& ray : rays) ray.origin.w = scenario == 5 ? 4000.f : 10.f;
		auto result = dispatch(trace.Get(), constants);
		unsigned errors = 0;
		for (unsigned i = 0; i < count; ++i) {
			auto ray = rays[i];
			if ((scenario == 4 || scenario == 5 || scenario == 6) && result[i * 4 + 2] != 0)
				throw std::runtime_error("unknown visibility incorrectly classified as open sky");
			float b = Dot(ray.origin, ray.direction);
			float expected = -b + std::sqrt(b * b - Dot(ray.origin, ray.origin) + 2000 * 2000);
			F point = Add(ray.origin, Mul(ray.direction, expected));
			if (scenario == 7) {
				expected = INFINITY;
				float p[] = { ray.origin.x, ray.origin.y, ray.origin.z }, d[] = { ray.direction.x, ray.direction.y, ray.direction.z };
				for (unsigned a = 0; a < 3; ++a)
					if (d[a] != 0)
						expected = std::min(expected, ((d[a] > 0 ? 2000.f : -2000.f) - p[a]) / d[a]);
			}
			if ((scenario == 1 || scenario == 2) && std::min({ std::abs(point.x), std::abs(point.y), std::abs(point.z) }) < 25)
				continue;
			bool hit = (scenario < 3 || scenario >= 7) && (constants.valid & (1u << Face(point)));
			float actual = result[i * 4 + 3];
			bool accurate = std::abs(actual - expected) <= 10;
			if (scenario == 7) {
				F p = Add(ray.origin, Mul(ray.direction, actual));
				float radius = std::sqrt(Dot(p, p));
				// Normalized octahedral directions vary by at most sqrt(18)/size
				// from a texel center. Add the explicitly allowed radial shell.
				float error = std::abs(std::max({ std::abs(p.x), std::abs(p.y), std::abs(p.z) }) - 2000);
				accurate = error <= ray.direction.w + 2 * std::sqrt(18.f) / size * radius;
			}
			if ((actual > 0) != hit || (hit && !accurate)) {
				if (errors++ < 8)
					std::printf("Mismatch scenario=%u ray=%u expected=%g actual=%g hit=%d\n", scenario, i, expected, actual, hit);
			} else
				++checked;
		}
		std::printf("Scenario %u: %u mismatches\n", scenario, errors);
		if (errors)
			throw std::runtime_error("analytic geometry mismatch");
	}
	// Visibility certificates must distinguish a genuinely empty captured cone
	// from an invalid/missing capture, without changing the intersection budget.
	std::fill(depths.begin(), depths.end(), 16384.f);
	context->UpdateSubresource(depth.resource.Get(), 0, nullptr, depths.data(), size * 4, 0);
	build(255);
	constants = {};
	for (auto& ray : rays) {
		ray.origin = { 0, 0, 0, 1 };
		ray.direction = Normalize({ .2f + random() * .6f, .5f, 1, 0 });
		ray.direction.w = 1;
	}
	for (auto shader : { reference.Get(), trace.Get() }) {
		for (unsigned mask : { 255u, 0u }) {
			constants.valid = mask;
			auto result = dispatch(shader, constants);
			for (unsigned i = 0; i < count; ++i)
				if (result[i * 4 + 3] != 0 || result[i * 4 + 2] != (mask ? 1.0f : 0.0f))
					throw std::runtime_error("empty/unknown visibility certificate mismatch");
		}
	}
	std::printf("PASS: 4096 fixed/hierarchical empty versus unknown visibility certificates\n");
	// One-texel radial patches: every selected ray has an analytic intersection at
	// the patch center. Empty coarse tiles must not skip these tiny occluders.
	std::fill(depths.begin(), depths.end(), 16384.f);
	constants = {};
	for (unsigned i = 0; i < count; ++i) {
		unsigned pixel = (i * 257) % (size * size), x = pixel % size, y = pixel / size;
		float d = 700 + random() * 2000;
		depths[y * size + x] = d;
		F target = Mul(Decode((x + .5f) / size, (y + .5f) / size), d);
		rays[i].origin = { (random() - .5f) * 200, (random() - .5f) * 200, (random() - .5f) * 200, 0.5 };
		rays[i].direction = Normalize(Add(target, Mul(rays[i].origin, -1)));
		rays[i].direction.w = .5f;
	}
	context->UpdateSubresource(depth.resource.Get(), 0, nullptr, depths.data(), size * 4, 0);
	build(255);
	auto thin = dispatch(trace.Get(), constants);
	for (unsigned i = 0; i < count; ++i)
		if (thin[i * 4 + 3] <= 0)
			throw std::runtime_error("skipped one-texel occluder " + std::to_string(i));
	checked += count;
	// Every level must bound every descendant's whole radial texel footprint.
	for (unsigned face = 0; face < 8; ++face) {
		auto leaf = Read(device.Get(), context.Get(), pyramid, 0, face, 2);
		for (unsigned y = 0; y < side; ++y)
			for (unsigned x = 0; x < side; ++x) {
				size_t i = (y * side + x) * 2;
				if (std::abs(leaf[i]) > leaf[i + 1])
					continue;
				unsigned px = x + ((face & 1) ? 0 : side), py = y + ((face & 2) ? 0 : side);
				float radius = depths[py * size + px];
				for (float dy : { 0.f, .25f, .5f, .75f, 1.f })
					for (float dx : { 0.f, .25f, .5f, .75f, 1.f }) {
						F d = Decode((px + dx) / size, (py + dy) / size);
						if (Face(d) != face)
							continue;
						float w = radius * (std::abs(d.x) + std::abs(d.y) + std::abs(d.z));
						if (w < leaf[i] || w > leaf[i + 1])
							throw std::runtime_error("texel footprint outside bounds");
					}
			}
		for (unsigned level = 1; level < mips; ++level) {
			auto coarse = Read(device.Get(), context.Get(), pyramid, level, face, 2);
			unsigned coarseSize = side >> level;
			for (unsigned y = 0; y < side; ++y)
				for (unsigned x = 0; x < side; ++x) {
					size_t i = (y * side + x) * 2, j = ((y >> level) * coarseSize + (x >> level)) * 2;
					if (std::abs(leaf[i]) <= leaf[i + 1] && (std::abs(coarse[j]) > std::abs(leaf[i]) || coarse[j + 1] < leaf[i + 1]))
						throw std::runtime_error("nonconservative pyramid");
				}
		}
	}
	// Rebuilding one face must preserve every mip of all other faces exactly.
	std::vector<std::vector<float>> previous;
	for (unsigned face = 0; face < 8; ++face)
		for (unsigned level = 0; level < mips; ++level) previous.push_back(Read(device.Get(), context.Get(), pyramid, level, face, 2));
	std::fill(depths.begin(), depths.end(), 3000.f);
	context->UpdateSubresource(depth.resource.Get(), 0, nullptr, depths.data(), size * 4, 0);
	build(1u << 3);
	for (unsigned face = 0; face < 8; ++face)
		for (unsigned level = 0; level < mips; ++level)
			if (face != 3 && previous[face * mips + level] != Read(device.Get(), context.Get(), pyramid, level, face, 2))
				throw std::runtime_error("incremental capture corrupted another face");
	if (hardware) {
		rays = originalRays;
		constants = {};
		constants.steps = 64;
		timing = true;
		for (bool empty : { false, true }) {
			std::fill(depths.begin(), depths.end(), empty ? 16384.f : 2000.f);
			context->UpdateSubresource(depth.resource.Get(), 0, nullptr, depths.data(), size * 4, 0);
			build(255);
			for (auto shader : { reference.Get(), trace.Get() }) {
				std::vector<double> times;
				for (unsigned i = 0; i < 8; ++i) {
					auto result = dispatch(shader, constants);
					for (unsigned r = 0; r < count; ++r)
						if ((result[r * 4 + 3] > 0) == empty)
							throw std::runtime_error("benchmark visibility differs");
					if (i >= 3)
						times.push_back(milliseconds);
				}
				std::sort(times.begin(), times.end());
				std::printf("BENCH: 262144 %s rays, 64-step budget, %s median %.4f ms\n", empty ? "empty" : "sphere", shader == trace.Get() ? "hierarchy" : "reference", times[times.size() / 2]);
			}
		}
		std::fill(depths.begin(), depths.end(), 2000.f);
		context->UpdateSubresource(depth.resource.Get(), 0, nullptr, depths.data(), size * 4, 0);
		for (unsigned mask : { 1u, 17u, 255u }) {
			std::vector<double> times;
			for (unsigned i = 0; i < 8; ++i) {
				context->Begin(disjoint.Get());
				context->End(beginTime.Get());
				build(mask);
				context->End(endTime.Get());
				context->End(disjoint.Get());
				context->Flush();
				D3D11_QUERY_DATA_TIMESTAMP_DISJOINT clock{};
				while (context->GetData(disjoint.Get(), &clock, sizeof(clock), 0) == S_FALSE) Sleep(0);
				UINT64 a{}, b{};
				Check(context->GetData(beginTime.Get(), &a, sizeof(a), 0));
				Check(context->GetData(endTime.Get(), &b, sizeof(b), 0));
				if (clock.Disjoint)
					throw std::runtime_error("unstable build timestamp");
				if (i >= 3)
					times.push_back(double(b - a) * 1000 / clock.Frequency);
			}
			std::sort(times.begin(), times.end());
			std::printf("BENCH: two-dispatch hierarchy build, face mask 0x%02X median %.4f ms\n", mask, times[times.size() / 2]);
		}
	}
	std::printf("PASS: %u %s analytic/sparse rays, conservative min/max levels, independent origins, selected octants, invalid/empty depth, behind-start rejection, incremental face preservation\n", checked, hardware ? "hardware" : "WARP");
	return 0;
} catch (const std::exception& e) {
	std::fprintf(stderr, "%s\n", e.what());
	return 1;
}
