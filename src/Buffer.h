#pragma once

#include <DirectXMath.h>
#include <d3d11.h>

#include <Windows.Foundation.h>
#include <stdio.h>
#include <winrt/base.h>
#include <wrl\client.h>
#include <wrl\wrappers\corewrappers.h>

template <typename T>
D3D11_BUFFER_DESC StructuredBufferDesc(uint64_t count, bool uav = true, bool dynamic = false)
{
	D3D11_BUFFER_DESC desc{};
	desc.Usage = (uav || !dynamic) ? D3D11_USAGE_DEFAULT : D3D11_USAGE_DYNAMIC;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	if (uav)
		desc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
	desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	desc.CPUAccessFlags = !dynamic ? 0 : D3D11_CPU_ACCESS_WRITE;
	desc.StructureByteStride = sizeof(T);
	desc.ByteWidth = (UINT)(sizeof(T) * count);
	return desc;
}

static constexpr std::uint32_t GetCBufferSize(std::uint32_t buffer_size)
{
	return (buffer_size + (64 - 1)) & ~(64 - 1);
}

template <typename T>
D3D11_BUFFER_DESC ConstantBufferDesc(bool dynamic = false)
{
	D3D11_BUFFER_DESC desc{};
	ZeroMemory(&desc, sizeof(desc));
	desc.Usage = (!dynamic) ? D3D11_USAGE_DEFAULT : D3D11_USAGE_DYNAMIC;
	desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	desc.CPUAccessFlags = !dynamic ? 0 : D3D11_CPU_ACCESS_WRITE;
	desc.ByteWidth = GetCBufferSize(sizeof(T));
	return desc;
}

class ConstantBuffer
{
public:
	ConstantBuffer(D3D11_BUFFER_DESC const& a_desc)
	{
		desc = a_desc;
		auto device = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().forwarder;
		DX::ThrowIfFailed(device->CreateBuffer(&desc, nullptr, resource.ReleaseAndGetAddressOf()));
	}

	ID3D11Buffer* CB() const { return resource.Get(); }

	void Update(void const* src_data, size_t data_size)
	{
		ID3D11DeviceContext* ctx = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
		if (desc.Usage & D3D11_USAGE_DYNAMIC) {
			D3D11_MAPPED_SUBRESOURCE mapped_buffer{};
			ZeroMemory(&mapped_buffer, sizeof(D3D11_MAPPED_SUBRESOURCE));
			DX::ThrowIfFailed(ctx->Map(resource.Get(), 0u, D3D11_MAP_WRITE_DISCARD, 0u, &mapped_buffer));
			memcpy(mapped_buffer.pData, src_data, data_size);
			ctx->Unmap(resource.Get(), 0);
		} else
			ctx->UpdateSubresource(resource.Get(), 0, nullptr, src_data, 0, 0);
	}

	template <typename T>
	void Update(T const& src_data)
	{
		Update(&src_data, sizeof(T));
	}

private:
	Microsoft::WRL::ComPtr<ID3D11Buffer> resource;
	D3D11_BUFFER_DESC desc;
};

class StructuredBuffer
{
public:
	StructuredBuffer(D3D11_BUFFER_DESC const& a_desc, UINT a_count)
	{
		desc = a_desc;
		count = a_count;
		auto device = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().forwarder;
		DX::ThrowIfFailed(device->CreateBuffer(&desc, nullptr, resource.ReleaseAndGetAddressOf()));
	}

	ID3D11ShaderResourceView* SRV(size_t i = 0) const { return srvs[i].Get(); }
	ID3D11UnorderedAccessView* UAV(size_t i = 0) const { return uavs[i].Get(); }

	virtual void CreateSRV()
	{
		ID3D11Device* device = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().forwarder;
		D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
		srv_desc.Format = DXGI_FORMAT_UNKNOWN;
		srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
		srv_desc.Buffer.FirstElement = 0;
		srv_desc.Buffer.NumElements = count;
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
		DX::ThrowIfFailed(device->CreateShaderResourceView(resource.Get(), &srv_desc, &srv));
		srvs.push_back(srv);
	}

	virtual void CreateUAV()
	{
		ID3D11Device* device = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().forwarder;
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
		uav_desc.Format = DXGI_FORMAT_UNKNOWN;
		uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uav_desc.Buffer.Flags = 0;
		uav_desc.Buffer.FirstElement = 0;
		uav_desc.Buffer.NumElements = count;
		Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(resource.Get(), &uav_desc, &uav));
		uavs.push_back(uav);
	}

	void Update(void const* src_data, [[maybe_unused]] size_t data_size)
	{
		ID3D11DeviceContext* ctx = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
		D3D11_MAPPED_SUBRESOURCE mapped_buffer{};
		ZeroMemory(&mapped_buffer, sizeof(D3D11_MAPPED_SUBRESOURCE));
		DX::ThrowIfFailed(ctx->Map(resource.Get(), 0u, D3D11_MAP_WRITE_DISCARD, 0u, &mapped_buffer));
		memcpy(mapped_buffer.pData, src_data, desc.ByteWidth);
		ctx->Unmap(resource.Get(), 0);
	}
	template <typename T>
	void UpdateList(T const& src_data, std::int64_t count)
	{
		Update(&src_data, sizeof(T) * count);
	}
	std::vector<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>> srvs;
	std::vector<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>> uavs;

private:
	Microsoft::WRL::ComPtr<ID3D11Buffer> resource;
	D3D11_BUFFER_DESC desc;
	UINT count;
};

class Buffer
{
public:
	Buffer(D3D11_BUFFER_DESC const& a_desc, D3D11_SUBRESOURCE_DATA* a_init = nullptr)
	{
		desc = a_desc;
		auto device = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().forwarder;
		DX::ThrowIfFailed(device->CreateBuffer(&desc, a_init, resource.put()));
	}

	void CreateSRV(D3D11_SHADER_RESOURCE_VIEW_DESC const& a_desc)
	{
		ID3D11Device* device = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().forwarder;
		DX::ThrowIfFailed(device->CreateShaderResourceView(resource.get(), &a_desc, srv.put()));
	}
	void CreateUAV(D3D11_UNORDERED_ACCESS_VIEW_DESC const& a_desc)
	{
		ID3D11Device* device = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().forwarder;
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(resource.get(), &a_desc, uav.put()));
	}

	D3D11_BUFFER_DESC desc;
	winrt::com_ptr<ID3D11Buffer> resource;
	winrt::com_ptr<ID3D11ShaderResourceView> srv;
	winrt::com_ptr<ID3D11UnorderedAccessView> uav;
};

class Texture2D
{
public:
	Texture2D(D3D11_TEXTURE2D_DESC const& a_desc)
	{
		desc = a_desc;
		auto device = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().forwarder;
		DX::ThrowIfFailed(device->CreateTexture2D(&desc, nullptr, resource.put()));
	}

	void CreateSRV(D3D11_SHADER_RESOURCE_VIEW_DESC const& a_desc)
	{
		ID3D11Device* device = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().forwarder;
		DX::ThrowIfFailed(device->CreateShaderResourceView(resource.get(), &a_desc, srv.put()));
	}
	void CreateUAV(D3D11_UNORDERED_ACCESS_VIEW_DESC const& a_desc)
	{
		ID3D11Device* device = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().forwarder;
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(resource.get(), &a_desc, uav.put()));
	}

	D3D11_TEXTURE2D_DESC desc;
	winrt::com_ptr<ID3D11Texture2D> resource;
	winrt::com_ptr<ID3D11ShaderResourceView> srv;
	winrt::com_ptr<ID3D11UnorderedAccessView> uav;
};

class Texture3D
{
public:
	Texture3D(D3D11_TEXTURE3D_DESC const& a_desc)
	{
		desc = a_desc;
		auto device = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().forwarder;
		DX::ThrowIfFailed(device->CreateTexture3D(&desc, nullptr, resource.put()));
	}

	void CreateSRV(D3D11_SHADER_RESOURCE_VIEW_DESC const& a_desc)
	{
		ID3D11Device* device = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().forwarder;
		DX::ThrowIfFailed(device->CreateShaderResourceView(resource.get(), &a_desc, srv.put()));
	}
	void CreateUAV(D3D11_UNORDERED_ACCESS_VIEW_DESC const& a_desc)
	{
		ID3D11Device* device = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().forwarder;
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(resource.get(), &a_desc, uav.put()));
	}

	D3D11_TEXTURE3D_DESC desc;
	winrt::com_ptr<ID3D11Texture3D> resource;
	winrt::com_ptr<ID3D11ShaderResourceView> srv;
	winrt::com_ptr<ID3D11UnorderedAccessView> uav;
};