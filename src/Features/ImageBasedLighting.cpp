#include "ImageBasedLighting.h"

#include "State.h"
#include "Util.h"

void ImageBasedLighting::DrawSettings()
{
	if (ImGui::BeginTabItem("Image-Based Lighting")) {

		if (ImGui::TreeNodeEx("Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::InputInt("Metal Only", (int*)&settings.IBLMetalOnly);
		}

		ImGui::EndTabItem();
	}
}

enum class GrassShaderTechniques
{
	RenderDepth = 8,
};

void ImageBasedLighting::ModifyGrass(const RE::BSShader*, const uint32_t descriptor)
{
	const auto technique = descriptor & 0b1111;
	if (technique != static_cast<uint32_t>(GrassShaderTechniques::RenderDepth)) {
		ModifyLighting(nullptr, 0);
	}
}


constexpr auto MIPLEVELS = 10; // This is the max number of mipmaps possible

enum class LightingShaderTechniques
{
	None = 0,
	Envmap = 1,
	Glowmap = 2,
	Parallax = 3,
	Facegen = 4,
	FacegenRGBTint = 5,
	Hair = 6,
	ParallaxOcc = 7,
	MTLand = 8,
	LODLand = 9,
	Snow = 10,  // unused
	MultilayerParallax = 11,
	TreeAnim = 12,
	LODObjects = 13,
	MultiIndexSparkle = 14,
	LODObjectHD = 15,
	Eye = 16,
	Cloud = 17,  // unused
	LODLandNoise = 18,
	MTLandLODBlend = 19,
	Outline = 20,
};

uint32_t GetTechnique(uint32_t descriptor)
{
	return 0x3F & (descriptor >> 24);
}

void ImageBasedLighting::UpdateCubemap()
{
	auto renderer = BSGraphics::Renderer::QInstance();
	auto deviceContext = RE::BSRenderManager::GetSingleton()->GetRuntimeData().context;
	auto cubemap = renderer->pCubemapRenderTargets[0];

	for (UINT face = 0; face < 6; face++) {
		D3D11_BOX srcBox = { 0 };
		srcBox.left = 0;
		srcBox.right = (cubemapIBL->desc.Width >> 0);
		srcBox.top = 0;
		srcBox.bottom = (cubemapIBL->desc.Height >> 0);
		srcBox.front = 0;
		srcBox.back = 1;

		// Calculate the subresource index for the current face and mip level
		UINT srcSubresourceIndex = D3D11CalcSubresource(0, face, 1);

		// Copy the subresource from the source to the destination texture
		deviceContext->CopySubresourceRegion(cubemapIBL->resource.get(), D3D11CalcSubresource(0, face, MIPLEVELS), 0, 0, 0, cubemap.Texture, srcSubresourceIndex, &srcBox);
	}

	// Generate mipmaps for the destination texture
	deviceContext->GenerateMips(cubemapIBL->srv.get());
}

void ImageBasedLighting::ModifyLighting(const RE::BSShader*, const uint32_t)
{
	auto renderer = BSGraphics::Renderer::QInstance();
	auto cubemap = renderer->pCubemapRenderTargets[0];

	if (!cubemapIBL) {
		D3D11_TEXTURE2D_DESC texDesc{};
		cubemap.Texture->GetDesc(&texDesc);
		texDesc.MipLevels = MIPLEVELS;
		texDesc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;

		cubemapIBL = new Texture2D(texDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
		cubemap.SRV->GetDesc(&viewDesc);
		viewDesc.TextureCube.MipLevels = MIPLEVELS;

		cubemapIBL->CreateSRV(viewDesc);
	}

	auto deviceContext = RE::BSRenderManager::GetSingleton()->GetRuntimeData().context;

	// During world cubemap generation we render the cubemap into itself for performance.
	// We generate mipmaps once after the LOD cubemap has been fully rendered.
	auto shadowState = BSGraphics::RendererShadowState::QInstance();
	if (shadowState->m_CubeMapRenderTarget == RENDER_TARGET_CUBEMAP_REFLECTIONS)
	{
		enableIBL = true;

		ID3D11ShaderResourceView* views[1]{};
		views[0] = cubemap.SRV;
		deviceContext->PSSetShaderResources(20, ARRAYSIZE(views), views);

		PerPass perPassData{};
		perPassData.GeneratingIBL = true;
		perPassData.EnableIBL = enableIBL;
		perPassData.Settings = settings;
		perPass->Update(perPassData);

	} else if (!renderedScreenCamera) {
		UpdateCubemap();
		renderedScreenCamera = true;

		ID3D11ShaderResourceView* views[1]{};
		views[0] = cubemapIBL->srv.get();
		deviceContext->PSSetShaderResources(20, ARRAYSIZE(views), views);

		PerPass perPassData{};
		perPassData.GeneratingIBL = false;
		perPassData.EnableIBL = enableIBL;
		perPassData.Settings = settings;
		perPass->Update(perPassData);
	}

	ID3D11Buffer* buffers[1]{};
	buffers[0] = perPass->CB();
	deviceContext->PSSetConstantBuffers(4, ARRAYSIZE(buffers), buffers);
}

void ImageBasedLighting::Draw(const RE::BSShader* shader, const uint32_t descriptor)
{
	switch (shader->shaderType.get()) {
	case RE::BSShader::Type::Grass:
		ModifyGrass(shader, descriptor);
		break;
	case RE::BSShader::Type::Lighting:
		ModifyLighting(shader, descriptor);
		break;
	}
}

void ImageBasedLighting::Load(json&)
{

}

void ImageBasedLighting::Save(json&)
{
}

void ImageBasedLighting::SetupResources()
{
	perPass = new ConstantBuffer(ConstantBufferDesc<PerPass>());
}

bool ImageBasedLighting::ValidateCache(CSimpleIniA&)
{
	return true;
}

void ImageBasedLighting::WriteDiskCacheInfo(CSimpleIniA&)
{
}


void ImageBasedLighting::Reset()
{
	enableIBL = false;
	renderedScreenCamera = false;
}