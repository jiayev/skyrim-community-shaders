#include "WaterBlending.h"
#include <Util.h>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	WaterBlending::Settings,
	EnableWaterBlending,
	WaterBlendRange,
	EnableWaterBlendingSSR,
	SSRBlendRange)

void WaterBlending::DrawSettings()
{
	if (ImGui::TreeNodeEx("Water Blending", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Enable Water Blending", (bool*)&settings.EnableWaterBlending);
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
			ImGui::Text("Enables blending water into the terrain and objects on the water's surface.");
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}

		ImGui::SliderFloat("Water Blend Range", &settings.WaterBlendRange, 0, 3);
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
			ImGui::Text("Water Blend Range.");
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::Checkbox("Enable Water Blending SSR", (bool*)&settings.EnableWaterBlendingSSR);
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
			ImGui::Text("Enables blending screen-space reflections (SSR) as they are faded out near where the terrain touches large water sources.");
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}

		ImGui::SliderFloat("SSR Blend Range", &settings.SSRBlendRange, 0, 3);
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
			ImGui::Text("Screen-space Reflection (SSR) Blend Range.");
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}

		ImGui::TreePop();
	}

	ImGui::Spacing();
	ImGui::Spacing();

	if (ImGui::Button("Restore Defaults", { -1, 0 })) {
		WaterBlending::GetSingleton()->RestoreDefaultSettings();
	}
	if (ImGui::IsItemHovered()) {
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 35.0f);
		ImGui::Text(
			"Restores the feature's settings back to their default values. "
			"You will still need to Save Settings to make these changes permanent. ");
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
}

void WaterBlending::Draw(const RE::BSShader* shader, const uint32_t)
{
	if (shader->shaderType.any(RE::BSShader::Type::Water, RE::BSShader::Type::Lighting)) {
		auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;

		PerPass data{};
		data.settings = settings;

		D3D11_MAPPED_SUBRESOURCE mapped;
		DX::ThrowIfFailed(context->Map(perPass->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
		size_t bytes = sizeof(PerPass);
		memcpy_s(mapped.pData, bytes, &data, bytes);
		context->Unmap(perPass->resource.get(), 0);

		if (shader->shaderType.any(RE::BSShader::Type::Water)) {
			auto renderer = RE::BSGraphics::Renderer::GetSingleton();
			ID3D11ShaderResourceView* views[2]{};
			views[0] = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV;
			views[1] = perPass->srv.get();
			context->PSSetShaderResources(33, ARRAYSIZE(views), views);
		} else {
			ID3D11ShaderResourceView* views[1]{};
			views[0] = perPass->srv.get();
			context->PSSetShaderResources(34, ARRAYSIZE(views), views);
		}
	}
}

void WaterBlending::SetupResources()
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

void WaterBlending::Load(json& o_json)
{
	if (o_json[GetName()].is_object())
		settings = o_json[GetName()];

	Feature::Load(o_json);
}

void WaterBlending::Save(json& o_json)
{
	o_json[GetName()] = settings;
}

void WaterBlending::RestoreDefaultSettings()
{
	settings = {};
}

bool WaterBlending::HasShaderDefine(RE::BSShader::Type)
{
	return true;
}