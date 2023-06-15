#include "ExtendedMaterials.h"

#include "State.h"
#include "Util.h"

#include <srell.hpp>

using RE::RENDER_TARGETS;

void ExtendedMaterials::DrawSettings()
{
	if (ImGui::BeginTabItem("Extended Materials")) {
		if (ImGui::TreeNodeEx("General", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Checkbox("Enable Parallax", (bool*)&settings.EnableParallax);
			ImGui::Checkbox("Enable Terrain Parallax", (bool*)&settings.EnableTerrainParallax);
			ImGui::Checkbox("Enable Extended Parallax", (bool*)&settings.EnableExtendedParallax);
			ImGui::Checkbox("Enable Complex Materials", (bool*)&settings.EnableComplexMaterial);

			ImGui::SliderInt("LOD Level", (int*)&settings.LODLevel, 1, 16);
			ImGui::SliderInt("Min Samples", (int*)&settings.MinSamples, 1, 128);
			ImGui::SliderInt("Max Samples", (int*)&settings.MaxSamples, 1, 128);

			ImGui::SliderFloat("Intensity", &settings.Intensity, 0, 3);

			ImGui::TreePop();
		}

		ImGui::EndTabItem();
	}
}

void ExtendedMaterials::ModifyLighting(const RE::BSShader*, const uint32_t)
{
	auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;

	{
		PerPass data{};
		data.settings = settings;

		std::vector<PerPass> vec;
		vec.push_back(data);

		D3D11_MAPPED_SUBRESOURCE mapped;
		DX::ThrowIfFailed(context->Map(perPass->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
		size_t bytes = sizeof(PerPass);
		memcpy_s(mapped.pData, bytes, vec.data(), bytes);
		context->Unmap(perPass->resource.get(), 0);
	}

	ID3D11ShaderResourceView* views[1]{};
	views[0] = perPass->srv.get();
	context->PSSetShaderResources(30, 1, views);
}

void ExtendedMaterials::Draw(const RE::BSShader* shader, const uint32_t descriptor)
{
	switch (shader->shaderType.get()) {
	case RE::BSShader::Type::Lighting:
		ModifyLighting(shader, descriptor);
		break;
	}
}

void ExtendedMaterials::Load(json&)
{
}

void ExtendedMaterials::Save(json& )
{
}


void ExtendedMaterials::SetupResources()
{
	D3D11_BUFFER_DESC sbDesc{};
	sbDesc.Usage = D3D11_USAGE_DYNAMIC;
	sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	sbDesc.StructureByteStride = sizeof(PerPass);
	sbDesc.ByteWidth = sizeof(PerPass);
	perPass = std::make_unique<Buffer>(sbDesc);

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	srvDesc.Buffer.FirstElement = 0;
	srvDesc.Buffer.NumElements = 1;
	perPass->CreateSRV(srvDesc);
}

bool ExtendedMaterials::ValidateCache(CSimpleIniA& )
{
	return true;
}

void ExtendedMaterials::WriteDiskCacheInfo(CSimpleIniA& )
{
}

void ExtendedMaterials::Reset()
{
}


