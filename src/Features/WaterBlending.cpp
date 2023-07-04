#include "WaterBlending.h"

#include "Util.h"


void WaterBlending::DrawSettings()
{
	if (ImGui::BeginTabItem("Water Blending")) {
		if (ImGui::TreeNodeEx("General", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Checkbox("Enable Water Blending", (bool*)&settings.EnableWaterBlending);

			ImGui::SliderFloat("Water Blend Range", &settings.WaterBlendRange, 0, 2);

			ImGui::Checkbox("Enable Water Blending SSR", (bool*)&settings.EnableWaterBlendingSSR);

			ImGui::SliderFloat("SSR Blend Range", &settings.SSRBlendRange, 0, 2);

			ImGui::TreePop();
		}

		ImGui::EndTabItem();
	}
}

void WaterBlending::Draw(const RE::BSShader* shader, const uint32_t)
{
	if (shader->shaderType.any(RE::BSShader::Type::Water, RE::BSShader::Type::Lighting)) 
	{
		auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
		
		PerPass data{};
		data.settings = settings;

		auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();

		data.waterHeight = -FLT_MAX;

		if (auto player = RE::PlayerCharacter::GetSingleton()) {
			if (auto cell = player->GetParentCell()) {
				if (!cell->IsInteriorCell()) {
					auto height = cell->GetExteriorWaterHeight();
					data.waterHeight = height - shadowState->GetRuntimeData().posAdjust.getEye().z;
				}
			}
		}

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
			context->PSSetShaderResources(31, ARRAYSIZE(views), views);
		} else {
			ID3D11ShaderResourceView* views[1]{};
			views[0] = perPass->srv.get();
			context->PSSetShaderResources(32, ARRAYSIZE(views), views);
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