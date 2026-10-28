// Standalone D3D11 regression test. Run from the repository root.
// cl /std:c++20 /EHsc tests/EnvironmentProbeRasterization.cpp /Febuild/ProbeRasterTest.exe /link d3d11.lib d3dcompiler.lib
// build/ProbeRasterTest.exe
// build/ProbeRasterTest.exe --hardware (also exercises instanced GS and compares against analytic rays)
#define NOMINMAX
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
using Vec = std::array<float, 3>;

static void Check(HRESULT hr)
{
	if (FAILED(hr))
		throw std::runtime_error("D3D operation failed: " + std::to_string(hr));
}

static ComPtr<ID3DBlob> Compile(const wchar_t* path, const char* entry, const char* target, const D3D_SHADER_MACRO* defines = nullptr)
{
	ComPtr<ID3DBlob> code, errors;
	const auto hr = D3DCompileFromFile(path, defines, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target, D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, &code, &errors);
	if (errors)
		std::fputs(static_cast<const char*>(errors->GetBufferPointer()), stderr);
	Check(hr);
	return code;
}

struct Capture
{
	Vec signs{};
	unsigned mask{};
	float nearDistance{}, farDistance{};
	unsigned lightCount{};
	unsigned textureSize{};
	Vec worldDelta{};
	unsigned cullBackface{};
	Vec lightDelta{};
	float padding{};
};
static_assert(sizeof(Capture) == 64);

static std::vector<Vec> Box(Vec lo, Vec hi)
{
	std::vector<Vec> vertices;
	for (unsigned axis = 0; axis < 3; ++axis) {
		for (unsigned side = 0; side < 2; ++side) {
			std::array<Vec, 4> corners;
			for (unsigned corner = 0; corner < 4; ++corner) {
				corners[corner][axis] = side ? hi[axis] : lo[axis];
				corners[corner][(axis + 1) % 3] = corner & 1 ? hi[(axis + 1) % 3] : lo[(axis + 1) % 3];
				corners[corner][(axis + 2) % 3] = corner & 2 ? hi[(axis + 2) % 3] : lo[(axis + 2) % 3];
			}
			for (unsigned index : { 0, 1, 2, 2, 1, 3 })
				vertices.push_back(corners[side ? index : index ^ 1]);
		}
	}
	return vertices;
}

static Vec Direction(unsigned x, unsigned y, unsigned size)
{
	Vec n{ (float(x) + .5f) * 2 / float(size) - 1, (float(y) + .5f) * 2 / float(size) - 1, 0 };
	n[2] = 1 - std::abs(n[0]) - std::abs(n[1]);
	if (n[2] < 0) {
		const auto px = n[0];
		n[0] = std::copysign(1 - std::abs(n[1]), px);
		n[1] = std::copysign(1 - std::abs(px), n[1]);
	}
	float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	for (auto& component : n) component /= length;
	return n;
}

static float RayBox(Vec direction, Vec lo, Vec hi)
{
	float distance = INFINITY;
	for (unsigned axis = 0; axis < 3; ++axis)
		if (direction[axis] != 0)
			distance = std::min(distance, (direction[axis] > 0 ? hi[axis] : lo[axis]) / direction[axis]);
	return distance;
}

int main(int argc, char** argv)
try {
	const bool hardware = argc > 1 && std::string(argv[1]) == "--hardware";
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	Check(D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
	auto vertexCode = Compile(L"tests/EnvironmentProbeRasterization.hlsl", "VS", "vs_5_0");
	auto geometryCode = Compile(L"tests/EnvironmentProbeRasterization.hlsl", "main", "gs_5_0");
	auto pixelCode = Compile(L"tests/EnvironmentProbeRasterization.hlsl", "PS", "ps_5_0");
	const D3D_SHADER_MACRO vsDefines[] = { { "VSHADER", "1" }, {} };
	auto clearVertexCode = Compile(L"package/Shaders/EnvironmentProbeClear.hlsl", "main", "vs_5_0", vsDefines);
	auto clearPixelCode = Compile(L"package/Shaders/EnvironmentProbeClear.hlsl", "main", "ps_5_0");
	ComPtr<ID3D11VertexShader> vs, clearVS;
	ComPtr<ID3D11PixelShader> ps, clearPS;
	ComPtr<ID3D11GeometryShader> gs;
	Check(device->CreateGeometryShader(geometryCode->GetBufferPointer(), geometryCode->GetBufferSize(), nullptr, &gs));
	Check(device->CreateVertexShader(vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr, &vs));
	Check(device->CreatePixelShader(pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(), nullptr, &ps));
	Check(device->CreateVertexShader(clearVertexCode->GetBufferPointer(), clearVertexCode->GetBufferSize(), nullptr, &clearVS));
	Check(device->CreatePixelShader(clearPixelCode->GetBufferPointer(), clearPixelCode->GetBufferSize(), nullptr, &clearPS));
	const D3D11_INPUT_ELEMENT_DESC input{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
	ComPtr<ID3D11InputLayout> layout;
	Check(device->CreateInputLayout(&input, 1, vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), &layout));
	auto vertices = Box({ -2, -3, -4 }, { 5, 6, 7 });
	const auto outer = Box({ -8, -8, -8 }, { 8, 8, 8 });
	vertices.insert(vertices.end(), outer.begin(), outer.end());
	D3D11_BUFFER_DESC bufferDesc{};
	bufferDesc.ByteWidth = UINT(vertices.size() * sizeof(Vec));
	bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	D3D11_SUBRESOURCE_DATA initial{ vertices.data(), 0, 0 };
	ComPtr<ID3D11Buffer> vertexBuffer, flippedBuffer, captureBuffer, dimensionBuffer;
	Check(device->CreateBuffer(&bufferDesc, &initial, &vertexBuffer));
	auto flipped = vertices;
	for (size_t i = 0; i < flipped.size(); i += 3) std::swap(flipped[i], flipped[i + 1]);
	initial.pSysMem = flipped.data();
	Check(device->CreateBuffer(&bufferDesc, &initial, &flippedBuffer));
	bufferDesc.ByteWidth = sizeof(Capture);
	bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	Check(device->CreateBuffer(&bufferDesc, nullptr, &captureBuffer));
	bufferDesc.ByteWidth = 16;
	Check(device->CreateBuffer(&bufferDesc, nullptr, &dimensionBuffer));
	context->GSSetConstantBuffers(8, 1, captureBuffer.GetAddressOf());
	context->VSSetConstantBuffers(8, 1, captureBuffer.GetAddressOf());
	context->PSSetConstantBuffers(8, 1, captureBuffer.GetAddressOf());
	context->PSSetConstantBuffers(0, 1, dimensionBuffer.GetAddressOf());
	D3D11_RASTERIZER_DESC rasterDesc{};
	rasterDesc.FillMode = D3D11_FILL_SOLID;
	rasterDesc.CullMode = D3D11_CULL_NONE;
	rasterDesc.DepthClipEnable = true;
	ComPtr<ID3D11RasterizerState> raster;
	Check(device->CreateRasterizerState(&rasterDesc, &raster));
	ComPtr<ID3D11RasterizerState> culled, culledCCW;
	rasterDesc.CullMode = D3D11_CULL_BACK;
	Check(device->CreateRasterizerState(&rasterDesc, &culled));
	rasterDesc.FrontCounterClockwise = true;
	Check(device->CreateRasterizerState(&rasterDesc, &culledCCW));
	context->RSSetState(raster.Get());
	CD3D11_DEPTH_STENCIL_DESC depthDesc(D3D11_DEFAULT);
	depthDesc.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
	ComPtr<ID3D11DepthStencilState> depthState;
	Check(device->CreateDepthStencilState(&depthDesc, &depthState));
	context->OMSetDepthStencilState(depthState.Get(), 0);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	unsigned cases = 0;
	float maxError = 0;
	unsigned clipBoundaryPixels = 0;
	for (unsigned useGS = 0; useGS <= unsigned(hardware); ++useGS)
		for (auto [size, farFirst, cullBack, ccw] : { std::array{ 64u, 0u, 0u, 0u }, std::array{ 65u, 0u, 0u, 0u }, std::array{ 64u, 1u, 0u, 0u }, std::array{ 65u, 1u, 0u, 0u }, std::array{ 64u, 0u, 1u, 0u }, std::array{ 65u, 1u, 1u, 0u }, std::array{ 64u, 0u, 1u, 1u }, std::array{ 65u, 1u, 1u, 1u } }) {
			D3D11_TEXTURE2D_DESC tex{};
			tex.Width = tex.Height = size;
			tex.MipLevels = tex.ArraySize = tex.SampleDesc.Count = 1;
			tex.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
			tex.BindFlags = D3D11_BIND_RENDER_TARGET;
			ComPtr<ID3D11Texture2D> color, distance, depth, colorRead, distanceRead;
			ComPtr<ID3D11RenderTargetView> colorRTV, distanceRTV;
			ComPtr<ID3D11DepthStencilView> dsv;
			Check(device->CreateTexture2D(&tex, nullptr, &color));
			Check(device->CreateRenderTargetView(color.Get(), nullptr, &colorRTV));
			tex.Format = DXGI_FORMAT_R32_FLOAT;
			Check(device->CreateTexture2D(&tex, nullptr, &distance));
			Check(device->CreateRenderTargetView(distance.Get(), nullptr, &distanceRTV));
			tex.BindFlags = 0;
			tex.Usage = D3D11_USAGE_STAGING;
			tex.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			Check(device->CreateTexture2D(&tex, nullptr, &distanceRead));
			tex.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
			Check(device->CreateTexture2D(&tex, nullptr, &colorRead));
			tex.Format = DXGI_FORMAT_D32_FLOAT;
			tex.BindFlags = D3D11_BIND_DEPTH_STENCIL;
			tex.Usage = D3D11_USAGE_DEFAULT;
			tex.CPUAccessFlags = 0;
			Check(device->CreateTexture2D(&tex, nullptr, &depth));
			Check(device->CreateDepthStencilView(depth.Get(), nullptr, &dsv));
			ID3D11RenderTargetView* targets[] = { colorRTV.Get(), distanceRTV.Get() };
			D3D11_VIEWPORT viewport{ 0, 0, float(size), float(size), 0, 1 };
			context->RSSetViewports(1, &viewport);
			float dimensions[] = { 1.f / float(size), 1.f / float(size), 0, 0 };
			context->UpdateSubresource(dimensionBuffer.Get(), 0, nullptr, dimensions, 0, 0);
			for (unsigned mask : { 1u, 2u, 4u, 8u, 16u, 32u, 64u, 128u, 0x69u, 0xFFu }) {
				for (auto range : { std::array{ .1f, 20.f }, std::array{ 12.137f, 20.f }, std::array{ .1f, 3.137f } }) {
					Capture data{};
					data.mask = mask;
					data.cullBackface = cullBack ? (ccw ? 2u : 1u) : 0;
					data.textureSize = size;
					data.nearDistance = range[0];
					data.farDistance = range[1];
					context->UpdateSubresource(captureBuffer.Get(), 0, nullptr, &data, 0, 0);
					const float sentinel[] = { 97, 97, 97, 97 };
					context->ClearRenderTargetView(colorRTV.Get(), sentinel);
					context->ClearRenderTargetView(distanceRTV.Get(), sentinel);
					context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0, 0);
					context->OMSetRenderTargets(2, targets, nullptr);
					context->IASetInputLayout(nullptr);
					context->GSSetShader(nullptr, nullptr, 0);
					context->VSSetShader(clearVS.Get(), nullptr, 0);
					context->PSSetShader(clearPS.Get(), nullptr, 0);
					context->RSSetState(raster.Get());
					context->Draw(3, 0);
					context->OMSetRenderTargets(2, targets, dsv.Get());
					context->IASetInputLayout(layout.Get());
					UINT stride = sizeof(Vec), offset = 0;
					context->IASetVertexBuffers(0, 1, (ccw ? flippedBuffer : vertexBuffer).GetAddressOf(), &stride, &offset);
					context->VSSetShader(vs.Get(), nullptr, 0);
					context->PSSetShader(ps.Get(), nullptr, 0);
					context->GSSetShader(useGS ? gs.Get() : nullptr, nullptr, 0);
					for (unsigned face = 0; face < (useGS ? 1u : 8u); ++face) {
						if (!useGS && !(mask & (1u << face)))
							continue;
						data.signs = { face & 1 ? -1.f : 1.f, face & 2 ? -1.f : 1.f, face & 4 ? -1.f : 1.f };
						context->RSSetState(cullBack && !useGS ? (ccw ? culledCCW : culled).Get() : raster.Get());
						context->UpdateSubresource(captureBuffer.Get(), 0, nullptr, &data, 0, 0);
						if (farFirst) {
							context->Draw(UINT(outer.size()), UINT(vertices.size() - outer.size()));
							context->Draw(UINT(vertices.size() - outer.size()), 0);
						} else {
							context->Draw(UINT(vertices.size()), 0);
						}
					}
					context->OMSetRenderTargets(0, nullptr, nullptr);
					context->CopyResource(colorRead.Get(), color.Get());
					context->CopyResource(distanceRead.Get(), distance.Get());
					D3D11_MAPPED_SUBRESOURCE colors{}, distances{};
					Check(context->Map(colorRead.Get(), 0, D3D11_MAP_READ, 0, &colors));
					Check(context->Map(distanceRead.Get(), 0, D3D11_MAP_READ, 0, &distances));
					for (unsigned y = 0; y < size; ++y)
						for (unsigned x = 0; x < size; ++x) {
							const Vec ray = Direction(x, y, size);
							const int px = int(x * 2 + 1) - int(size), py = int(y * 2 + 1) - int(size);
							const unsigned face = (px < 0 ? 1u : 0u) | (py < 0 ? 2u : 0u) | (std::abs(px) + std::abs(py) > int(size) ? 4u : 0u);
							const bool selected = (mask & (1u << face)) != 0;
							const float innerDistance = RayBox(ray, { -2, -3, -4 }, { 5, 6, 7 });
							const float outerDistance = RayBox(ray, { -8, -8, -8 }, { 8, 8, 8 });
							// A surface within subpixel precision of a radial clip sphere can lie on either
							// side after clipping/interpolation. Do not assert its discontinuous hit color.
							if (selected && (std::abs(innerDistance - range[0]) < .02f || std::abs(innerDistance - range[1]) < .02f ||
												std::abs(outerDistance - range[0]) < .02f || std::abs(outerDistance - range[1]) < .02f)) {
								++clipBoundaryPixels;
								continue;
							}
							float expected = 97;
							bool hit = false;
							if (selected) {
								expected = range[1];
								for (float radius : { innerDistance, outerDistance })
									if (radius >= range[0] && radius < expected) {
										expected = radius;
										hit = true;
									}
							}
							const float actual = reinterpret_cast<float*>(static_cast<char*>(distances.pData) + y * distances.RowPitch)[x];
							const float* colorPixel = reinterpret_cast<float*>(static_cast<char*>(colors.pData) + y * colors.RowPitch) + x * 4;
							maxError = std::max(maxError, std::abs(actual - expected));
							// Hardware clipping quantizes new vertices to the rasterizer's subpixel grid.
							bool valid = std::abs(actual - expected) < .02f;
							for (unsigned c = 0; c < 4; ++c) {
								const float value = !selected ? 97.f : !hit ? 0.f :
								                                   c == 3   ? 1.f :
								                                              ray[c] * expected;
								maxError = std::max(maxError, std::abs(colorPixel[c] - value));
								valid &= std::abs(colorPixel[c] - value) < .02f;
							}
							if (!valid) {
								std::fprintf(stderr, "Mismatch GS=%u size=%u mask=%02X near=%g far=%g xy=%u,%u face=%u depth=%g expected=%g\n", useGS, size, mask, range[0], range[1], x, y, face, actual, expected);
								return 1;
							}
						}
					context->Unmap(colorRead.Get(), 0);
					context->Unmap(distanceRead.Get(), 0);
					++cases;
				}
			}
		}
	std::printf("Passed %u %s cases (max error %.6f): direct octahedral rasterization, folds/axes, perspective interpolation, radial occlusion, near/far clip, and selected-face preservation.\n", cases, hardware ? "hardware (reference + GS)" : "WARP (reference)", maxError);
	std::printf("Excluded %u samples within rasterization tolerance of radial clip boundaries.\n", clipBoundaryPixels);
	return 0;
} catch (const std::exception& error) {
	std::fprintf(stderr, "%s\n", error.what());
	return 1;
}
