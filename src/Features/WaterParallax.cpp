#include "WaterParallax.h"
#include "State.h"
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	WaterParallax::Settings,
	EnableWaterParallax)

void WaterParallax::DrawSettings()
{
	if (ImGui::TreeNodeEx("Water Parallax", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Enable Water Parallax", (bool*)&settings.EnableWaterParallax);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Enables water parallax. "
				"Parallax adds depth to the water surface to simulate greater detail.");
		}

		ImGui::TreePop();
	}
}

void WaterParallax::Draw(const RE::BSShader*, const uint32_t)
{
	auto& context = State::GetSingleton()->context;
	PerPass data{};
	data.settings = settings;

	D3D11_MAPPED_SUBRESOURCE mapped;
	DX::ThrowIfFailed(context->Map(perPass->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
	size_t bytes = sizeof(PerPass);
	memcpy_s(mapped.pData, bytes, &data, bytes);
	context->Unmap(perPass->resource.get(), 0);

	ID3D11ShaderResourceView* views[1]{};
	views[0] = perPass->srv.get();
	context->PSSetShaderResources(72, ARRAYSIZE(views), views);
}

void WaterParallax::SetupResources()
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

void WaterParallax::Load(json& o_json)
{
	if (o_json[GetName()].is_object())
		settings = o_json[GetName()];
	Feature::Load(o_json);
}

void WaterParallax::Save(json& o_json)
{
	o_json[GetName()] = settings;
}

void WaterParallax::RestoreDefaultSettings()
{
	settings = {};
}

bool WaterParallax::HasShaderDefine(RE::BSShader::Type)
{
	return true;
}