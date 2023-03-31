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

constexpr auto MIPLEVELS = 10;  // This is the max number of mipmaps possible

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
	auto context = RE::BSRenderManager::GetSingleton()->GetRuntimeData().context;
	//auto device = RE::BSRenderManager::GetSingleton()->GetRuntimeData().forwarder;

	auto cubemap = renderer->pCubemapRenderTargets[0];

	for (UINT face = 0; face < 6; face++) {
		D3D11_BOX srcBox = { 0 };
		srcBox.left = 0;
		srcBox.right = (envTexture->desc.Width >> 0);
		srcBox.top = 0;
		srcBox.bottom = (envTexture->desc.Height >> 0);
		srcBox.front = 0;
		srcBox.back = 1;

		// Calculate the subresource index for the current face and mip level
		UINT srcSubresourceIndex = D3D11CalcSubresource(0, face, 1);

		// Copy the subresource from the source to the destination texture
		context->CopySubresourceRegion(unfilteredEnvTexture->resource.get(), D3D11CalcSubresource(0, face, MIPLEVELS), 0, 0, 0, cubemap.Texture, srcSubresourceIndex, &srcBox);
	}

	//{
	//	// Copy 0th mipmap level into destination environment map.
	//	for (int arraySlice = 0; arraySlice < 6; ++arraySlice) {
	//		const UINT subresourceIndex = D3D11CalcSubresource(0, arraySlice, MIPLEVELS);
	//		context->CopySubresourceRegion(envTexture->resource.get(), subresourceIndex, 0, 0, 0, cubemap.Texture, subresourceIndex, nullptr);
	//	}

	//	context->GenerateMips(envTexture->srv.get());
	//}


	//// Generate mipmaps for the destination texture
	//context->GenerateMips(unfilteredEnvTexture->srv.get());


//	for (UINT face = 0; face < 6; face++) {
//		D3D11_BOX srcBox = { 0 };
//		srcBox.left = 0;
//		srcBox.right = (envTexture->desc.Width >> 0);
//		srcBox.top = 0;
//		srcBox.bottom = (envTexture->desc.Height >> 0);
//		srcBox.front = 0;
//		srcBox.back = 1;
//
//		// Calculate the subresource index for the current face and mip level
//		UINT srcSubresourceIndex = D3D11CalcSubresource(0, face, 1);
//
//		// Copy the subresource from the source to the destination texture
//		context->CopySubresourceRegion(envTexture->resource.get(), D3D11CalcSubresource(0, face, MIPLEVELS), 0, 0, 0, cubemap.Texture, srcSubresourceIndex, &srcBox);
//	}
//
////	{
//	// Copy 0th mipmap level into destination environment map.
//	//for (int arraySlice = 0; arraySlice < 6; ++arraySlice) {
//	//	const UINT subresourceIndex = D3D11CalcSubresource(0, arraySlice, MIPLEVELS);
//	//	context->CopySubresourceRegion(envTexture->resource.get(), subresourceIndex, 0, 0, 0, cubemap.Texture, subresourceIndex, nullptr);
//	//}
//
//	context->GenerateMips(envTexture->srv.get());
//
//	context->CSSetShaderResources(0, 1, &cubemap.SRV);
//	context->CSSetSamplers(0, 1, &computeSampler);
//	context->CSSetShader(spmapProgram, nullptr, 0);
//
//	// Pre-filter rest of the mip chain.
//	const float deltaRoughness = 1.0f / max(float(MIPLEVELS - 1), 1.0f);
//	for (UINT level = 1, size = 512; level < MIPLEVELS; ++level, size /= 2) {
//		const UINT numGroups = (UINT)max(1, size / 32);
//		static std::unordered_map<UINT, ID3D11UnorderedAccessView*> uavs;
//
//		if (!uavs.contains(level)) {
//
//			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
//			uavDesc.Format = envTexture->desc.Format;
//			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
//			uavDesc.Texture2DArray.MipSlice = level;
//			uavDesc.Texture2DArray.FirstArraySlice = 0;
//			uavDesc.Texture2DArray.ArraySize = envTexture->desc.ArraySize;
//
//			ID3D11UnorderedAccessView* uav;
//			DX::ThrowIfFailed(device->CreateUnorderedAccessView(envTexture->resource.get(), &uavDesc, &uav));
//			uavs.insert({ level, uav });
//		}
//
//		auto& uav = uavs[level];
//
//		const SpecularMapFilterSettingsCB spmapConstants = { level * deltaRoughness };
//		spmapCB->Update(spmapConstants);
//		
//		//context->UpdateSubresource(spmapCB->CB(), 0, nullptr, &spmapConstants, 0, 0);
//
//		ID3D11Buffer* buffers[1];
//		buffers[0] = spmapCB->CB();
//
//		context->CSSetConstantBuffers(0, ARRAYSIZE(buffers), buffers);
//		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
//		context->Dispatch(numGroups, numGroups, 6);
//	}


	//	context->CSSetConstantBuffers(0, 1, &nullBuffer);
	//	context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
	//}


	if (!settings.IBLMetalOnly) {
		// Compute diffuse irradiance cubemap.
		{

			struct OldState
			{
				ID3D11ShaderResourceView* srv;
				ID3D11SamplerState* sampler;
				ID3D11UnorderedAccessView* uav;
				ID3D11ComputeShader* shader;
			};

			OldState oldState{};
			context->CSGetShaderResources(0, 1, &oldState.srv);
			context->CSGetSamplers(0, 1, &oldState.sampler);
			context->CSGetUnorderedAccessViews(0, 1, &oldState.uav);
			context->CSGetShader(&oldState.shader, nullptr, 0);


			auto srv = unfilteredEnvTexture->srv.get();
			context->CSSetShaderResources(0, 1, &srv);
			context->CSSetSamplers(0, 1, &computeSampler);
			auto uav = irmapTexture->uav.get();
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
			context->CSSetShader(irmapProgram, nullptr, 0);
			context->Dispatch(irmapTexture->desc.Width / 32, irmapTexture->desc.Height / 32, 6);

			context->CSSetShaderResources(0, 1, &oldState.srv);
			context->CSSetSamplers(0, 1, &oldState.sampler);
			context->CSSetUnorderedAccessViews(0, 1, &oldState.uav, nullptr);
			context->CSSetShader(oldState.shader, nullptr, 0);
		}
	}

}



void ImageBasedLighting::ModifyLighting(const RE::BSShader*, const uint32_t)
{
	//auto context = RE::BSRenderManager::GetSingleton()->GetRuntimeData().context;

	auto renderer = BSGraphics::Renderer::QInstance();
	auto cubemap = renderer->pCubemapRenderTargets[0];

	if (!envTexture) {
		auto device = RE::BSRenderManager::GetSingleton()->GetRuntimeData().forwarder;

		{
			D3D11_TEXTURE2D_DESC texDesc{};
			cubemap.Texture->GetDesc(&texDesc);

			//if (texDesc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) {
				//texDesc.Width = 1024;
				//texDesc.Height = 1024;
				//texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
				////texDesc.MipLevels = MIPLEVELS;
				////texDesc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
				////texDesc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
				//cubemap.Texture->Release();

				//cubemapTexture = new Texture2D(texDesc);
				//cubemap.Texture = cubemapTexture->resource.get();

				//D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
				//cubemap.SRV->GetDesc(&srvDesc);
				//cubemap.SRV->Release();

				//srvDesc.Format = texDesc.Format;
				////srvDesc.TextureCube.MipLevels = MIPLEVELS;
				//cubemapTexture->CreateSRV(srvDesc);

				//cubemap.SRV = cubemapTexture->srv.get();
		//	}
		}

		{
			D3D11_SAMPLER_DESC samplerDesc = {};
			samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
			samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
			samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
			samplerDesc.MaxAnisotropy = 1;
			samplerDesc.MinLOD = 0;
			samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
			DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &computeSampler));
		}

		{
			spmapProgram = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\ImageBasedLighting\\spmap.hlsl", {}, "cs_5_0");
			spmapCB = new ConstantBuffer(ConstantBufferDesc<SpecularMapFilterSettingsCB>());

			D3D11_TEXTURE2D_DESC texDesc{};
			cubemap.Texture->GetDesc(&texDesc);
			texDesc.MipLevels = MIPLEVELS;
			texDesc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;
			texDesc.BindFlags |= D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			envTexture = new Texture2D(texDesc);

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			cubemap.SRV->GetDesc(&srvDesc);
			srvDesc.TextureCube.MipLevels = MIPLEVELS;
			envTexture->CreateSRV(srvDesc);

			unfilteredEnvTexture = new Texture2D(texDesc);
			unfilteredEnvTexture->CreateSRV(srvDesc);
		}

		{
			irmapProgram = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\ImageBasedLighting\\irmap.hlsl", {}, "cs_5_0");

			D3D11_TEXTURE2D_DESC texDesc{};
			texDesc.Width = 32;
			texDesc.Height = 32;
			texDesc.MipLevels = 1;
			texDesc.ArraySize = 6;
			texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			texDesc.SampleDesc.Count = 1;
			texDesc.Usage = D3D11_USAGE_DEFAULT;
			texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			texDesc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;

			irmapTexture = new Texture2D(texDesc);

			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = texDesc.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
			uavDesc.Texture2DArray.MipSlice = 0;
			uavDesc.Texture2DArray.FirstArraySlice = 0;
			uavDesc.Texture2DArray.ArraySize = texDesc.ArraySize;

			irmapTexture->CreateUAV(uavDesc);

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			cubemap.SRV->GetDesc(&srvDesc);
			srvDesc.TextureCube.MipLevels = 1;
			irmapTexture->CreateSRV(srvDesc);
		}

		{
			spBRDFProgram = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\ImageBasedLighting\\spbrdf.hlsl", {}, "cs_5_0");
			
			D3D11_TEXTURE2D_DESC texDesc{};
			texDesc.Width = 256;
			texDesc.Height = 256;
			texDesc.MipLevels = 1;
			texDesc.ArraySize = 1;
			texDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
			texDesc.SampleDesc.Count = 1;
			texDesc.Usage = D3D11_USAGE_DEFAULT;
			texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			spBRDFLUT = new Texture2D(texDesc);

			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = texDesc.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = 0;
			spBRDFLUT->CreateUAV(uavDesc);

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = texDesc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MostDetailedMip = 0;
			srvDesc.Texture2D.MipLevels = 1;
			spBRDFLUT->CreateSRV(srvDesc);


			D3D11_SAMPLER_DESC samplerDesc = {};
			samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDesc.MaxAnisotropy = 1;
			samplerDesc.MinLOD = 0;
			samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
			DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &spBRDFSampler));

			{
				auto context = RE::BSRenderManager::GetSingleton()->GetRuntimeData().context;

				// Compute Cook-Torrance BRDF 2D LUT for split-sum approximation.
				auto uav = spBRDFLUT->uav.get();
				context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
				context->CSSetShader(spBRDFProgram, nullptr, 0);
				context->Dispatch(spBRDFLUT->desc.Width / 32, spBRDFLUT->desc.Height / 32, 1);
				context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
			}
		}

		//D3D11_TEXTURE2D_DESC texDesc{};
		//cubemap.Texture->GetDesc(&texDesc);
		//texDesc.MipLevels = MIPLEVELS;
		//texDesc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;

		//cubemapIBL = new Texture2D(texDesc);

		//D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
		//cubemap.SRV->GetDesc(&viewDesc);
		//viewDesc.TextureCube.MipLevels = MIPLEVELS;

		//cubemapIBL->CreateSRV(viewDesc);
	}

	auto deviceContext = RE::BSRenderManager::GetSingleton()->GetRuntimeData().context;


	// During world cubemap generation we render the cubemap into itself for performance.
	// We generate mipmaps once after the LOD cubemap has been fully rendered.
	auto shadowState = BSGraphics::RendererShadowState::QInstance();
	if (shadowState->m_CubeMapRenderTarget == RENDER_TARGET_CUBEMAP_REFLECTIONS) {
		enableIBL = true;

		ID3D11ShaderResourceView* views[1]{};
		views[0] = cubemap.SRV;
		deviceContext->PSSetShaderResources(21, ARRAYSIZE(views), views);

		PerPass perPassData{};
		perPassData.GeneratingIBL = true;
		perPassData.EnableIBL = enableIBL;
		perPassData.Settings = settings;
		perPass->Update(perPassData);
	} else if (!renderedScreenCamera) {
		UpdateCubemap();
		renderedScreenCamera = true;

		PerPass perPassData{};
		perPassData.GeneratingIBL = false;
		perPassData.EnableIBL = enableIBL;
		perPassData.Settings = settings;
		perPass->Update(perPassData);
	}

	if (renderedScreenCamera) {
		ID3D11ShaderResourceView* views[3]{};
		views[0] = envTexture->srv.get();
		//views[0] = cubemap.SRV;
		views[1] = irmapTexture->srv.get();
		views[2] = spBRDFLUT->srv.get();
		deviceContext->PSSetShaderResources(20, ARRAYSIZE(views), views);
	}

	ID3D11Buffer* buffers[1]{};
	buffers[0] = perPass->CB();
	deviceContext->PSSetConstantBuffers(4, ARRAYSIZE(buffers), buffers);


	// Discover the availability of a sampler state
	// 
	//ID3D11SamplerState* samplers[16]{};
	//deviceContext->PSGetSamplers(0, 16, samplers);
	//for (int i = 0; i < 16; i++) {
	//	if (auto firstSampler = samplers[i]) {
	//		D3D11_SAMPLER_DESC firstSamplerDesc;
	//		firstSampler->GetDesc(&firstSamplerDesc);

	//		for (int k = 0; k < 16; k++) {
	//			if (i == k) {
	//				continue;
	//			}
	//			if (auto secondSampler = samplers[k]) {
	//				D3D11_SAMPLER_DESC secondSamplerDesc;
	//				secondSampler->GetDesc(&secondSamplerDesc);

	//				if (firstSamplerDesc.AddressU == secondSamplerDesc.AddressU ||
	//					firstSamplerDesc.AddressV == secondSamplerDesc.AddressV ||
	//					firstSamplerDesc.AddressW == secondSamplerDesc.AddressW ||
	//					firstSamplerDesc.BorderColor[0] == secondSamplerDesc.BorderColor[0] ||
	//					firstSamplerDesc.BorderColor[1] == secondSamplerDesc.BorderColor[1] ||
	//					firstSamplerDesc.BorderColor[2] == secondSamplerDesc.BorderColor[2] ||
	//					firstSamplerDesc.BorderColor[3] == secondSamplerDesc.BorderColor[3] ||
	//					firstSamplerDesc.ComparisonFunc == secondSamplerDesc.ComparisonFunc ||
	//					firstSamplerDesc.Filter == secondSamplerDesc.Filter ||
	//					firstSamplerDesc.MaxAnisotropy == secondSamplerDesc.MaxAnisotropy ||
	//					firstSamplerDesc.MaxLOD == secondSamplerDesc.MaxLOD ||
	//					firstSamplerDesc.MinLOD == secondSamplerDesc.MinLOD ||
	//					firstSamplerDesc.MipLODBias == secondSamplerDesc.MipLODBias) {
	//					logger::info("Sampler {} and {} were the same", i, k);
	//				} else if ((i == 1 && k == 0) || (i == 0 && k == 1)) {
	//					logger::info("Sampler {} and {} were not the same", i, k);
	//				}
	//			}
	//		}
	//	}
	//}
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