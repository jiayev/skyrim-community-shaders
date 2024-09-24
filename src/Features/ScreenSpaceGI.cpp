#include "ScreenSpaceGI.h"
#include "Menu.h"

#include "Deferred.h"
#include "State.h"
#include "Util.h"

#include "DirectXTex.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenSpaceGI::Settings,
	Enabled,
	EnableGI,
	EnableSpecularGI,
	HalfRate,
	HalfRes,
	EnableTemporalDenoiser,
	NumSlices,
	NumSteps,
	DepthMIPSamplingOffset,
	AORadius,
	GIRadius,
	Thickness,
	DepthFadeRange,
	BackfaceStrength,
	EnableGIBounce,
	GIBounceFade,
	GIDistanceCompensation,
	AOPower,
	GIStrength,
	DepthDisocclusion,
	NormalDisocclusion,
	MaxAccumFrames,
	BlurRadius,
	BlurPasses,
	DistanceNormalisation)

class DisableGuard
{
private:
	bool disable;

public:
	DisableGuard(bool disable) :
		disable(disable)
	{
		if (disable)
			ImGui::BeginDisabled();
	}
	~DisableGuard()
	{
		if (disable)
			ImGui::EndDisabled();
	}
};

bool percentageSlider(const char* label, float* data, float lb = 0.f, float ub = 100.f, const char* format = "%.1f %%")
{
	float percentageData = (*data) * 1e2f;
	bool retval = ImGui::SliderFloat(label, &percentageData, lb, ub, format);
	(*data) = percentageData * 1e-2f;
	return retval;
}

////////////////////////////////////////////////////////////////////////////////////

void ScreenSpaceGI::RestoreDefaultSettings()
{
	settings = {};
}

void ScreenSpaceGI::DrawSettings()
{
	static bool showAdvanced;

	if (!ShadersOK())
		ImGui::TextColored({ 1, 0, 0, 1 }, "Compute shaders failed to compile!");

	///////////////////////////////
	ImGui::SeparatorText("Toggles");

	ImGui::Checkbox("Show Advanced Options", &showAdvanced);

	if (ImGui::BeginTable("Toggles", 3)) {
		ImGui::TableNextColumn();
		ImGui::Checkbox("Enabled", &settings.Enabled);
		ImGui::TableNextColumn();
		recompileFlag |= ImGui::Checkbox("Diffuse IL", &settings.EnableGI);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Simulates indirect diffuse lighting.");
		ImGui::TableNextColumn();
		{
			auto _ = DisableGuard(!settings.EnableGI);
			recompileFlag |= ImGui::Checkbox("Specular IL", &settings.EnableSpecularGI);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text(
					"Reuses diffuse samples to simulate indirect specular lighting.\n"
					"Doubles the cost of denoisers.\n"
					"Only for Complex Material or TruePBR materials.");
		}

		ImGui::EndTable();
	}

	///////////////////////////////
	ImGui::SeparatorText("Quality/Performance");

	if (ImGui::BeginTable("Presets", 5)) {
		ImGui::TableNextColumn();
		if (ImGui::Button("AO only", { -1, 0 })) {
			settings.NumSlices = 1;
			settings.NumSteps = 6;
			settings.EnableBlur = false;
			settings.EnableGI = false;
			recompileFlag = true;
		}
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"1 Slice, 6 Steps, no blur, no GI\n"
				"Try smaller effect radius :)");

		ImGui::TableNextColumn();
		if (ImGui::Button("Low", { -1, 0 })) {
			settings.NumSlices = 2;
			settings.NumSteps = 4;
			settings.EnableBlur = true;
			settings.EnableGI = true;
			recompileFlag = true;
		}
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("2 Slices, 4 Steps");

		ImGui::TableNextColumn();
		if (ImGui::Button("Medium", { -1, 0 })) {
			settings.NumSlices = 3;
			settings.NumSteps = 6;
			settings.EnableBlur = true;
			settings.EnableGI = true;
			recompileFlag = true;
		}
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("3 Slices, 6 Steps");

		ImGui::TableNextColumn();
		if (ImGui::Button("High", { -1, 0 })) {
			settings.NumSlices = 4;
			settings.NumSteps = 8;
			settings.EnableBlur = true;
			settings.EnableGI = true;
			recompileFlag = true;
		}
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("4 Slices, 8 Steps");

		ImGui::TableNextColumn();
		if (ImGui::Button("Ultra", { -1, 0 })) {
			settings.NumSlices = 6;
			settings.NumSteps = 10;
			settings.EnableBlur = true;
			settings.EnableGI = true;
			recompileFlag = true;
		}
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("6 Slices, 10 Steps");

		ImGui::EndTable();
	}

	ImGui::SliderInt("Slices", (int*)&settings.NumSlices, 1, 10);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"How many directions do the samples take.\n"
			"Controls noise.");

	ImGui::SliderInt("Steps Per Slice", (int*)&settings.NumSteps, 1, 20);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"How many samples does it take in one direction.\n"
			"Controls accuracy of lighting, and noise when effect radius is large.");

	if (showAdvanced) {
		ImGui::SliderFloat("MIP Sampling Offset", &settings.DepthMIPSamplingOffset, 2.f, 6.f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Mainly performance (texture memory bandwidth) setting but as a side-effect reduces overshadowing by thin objects and increases temporal instability.");
	}

	if (ImGui::BeginTable("Less Work", 2)) {
		ImGui::TableNextColumn();
		recompileFlag |= ImGui::Checkbox("Half Rate", &settings.HalfRate);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Shading only half the pixels per frame. Cheaper for higher settings but has more ghosting, and takes twice as long to converge.");

		ImGui::TableNextColumn();
		recompileFlag |= ImGui::Checkbox("Half Resolution", &settings.HalfRes);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Rendering internally with half resolution. Vastly cheaper but quite more noise.");

		ImGui::EndTable();
	}

	///////////////////////////////
	ImGui::SeparatorText("Visual");

	ImGui::SliderFloat("AO Power", &settings.AOPower, 0.f, 6.f, "%.2f");

	{
		auto _ = DisableGuard(!settings.EnableGI);
		ImGui::SliderFloat("IL Source Brightness", &settings.GIStrength, 0.f, 10.f, "%.2f");
	}

	ImGui::Separator();

	ImGui::SliderFloat("AO radius", &settings.AORadius, 10.f, 800.0f, "%.1f game units");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("A smaller radius produces tighter AO.");

	{
		auto _ = DisableGuard(!settings.EnableGI);

		ImGui::SliderFloat("IL radius", &settings.GIRadius, 10.f, 800.0f, "%.1f game units");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("A larger radius produces wider IL.");
	}

	ImGui::SliderFloat2("Depth Fade Range", &settings.DepthFadeRange.x, 1e4, 5e4, "%.0f game units");

	if (showAdvanced) {
		ImGui::Separator();

		ImGui::SliderFloat("Thickness", &settings.Thickness, 0.f, 500.0f, "%.1f game units");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("How thick the occluders are. Only affects AO.");
	}

	///////////////////////////////
	ImGui::SeparatorText("Visual - IL");

	{
		auto _ = DisableGuard(!settings.EnableGI);

		if (showAdvanced) {
			ImGui::SliderFloat("IL Distance Compensation", &settings.GIDistanceCompensation, -5.0f, 5.0f, "%.1f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Brighten/Dimming further radiance samples.");

			ImGui::Separator();
		}

		recompileFlag |= ImGui::Checkbox("Ambient Bounce", &settings.EnableGIBounce);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Simulates multiple light bounces. Better with denoiser on.\n"
				"Mandatory if you want ambient as part of the light source for IL calculation.");

		{
			auto __ = DisableGuard(!settings.EnableGIBounce);
			ImGui::Indent();
			percentageSlider("Ambient Bounce Strength", &settings.GIBounceFade);
			ImGui::Unindent();
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("How much of this frame's ambient+IL get carried to the next frame as source.");
		}

		if (showAdvanced) {
			ImGui::Separator();

			percentageSlider("Backface Lighting Mix", &settings.BackfaceStrength);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("How bright at the back of objects is compared to the front. A small value to make up for foliage translucency.");
		}
	}

	///////////////////////////////
	ImGui::SeparatorText("Denoising");

	if (ImGui::BeginTable("denoisers", 2)) {
		ImGui::TableNextColumn();
		recompileFlag |= ImGui::Checkbox("Temporal Denoiser", &settings.EnableTemporalDenoiser);

		ImGui::TableNextColumn();
		ImGui::Checkbox("Blur", &settings.EnableBlur);

		ImGui::EndTable();
	}

	if (showAdvanced) {
		ImGui::Separator();

		{
			auto _ = DisableGuard(!settings.EnableTemporalDenoiser);
			ImGui::SliderInt("Max Frame Accumulation", (int*)&settings.MaxAccumFrames, 1, 64, "%d", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("How many past frames to accumulate results with. Higher values are less noisy but potentially cause ghosting.");
		}

		ImGui::Separator();

		{
			auto _ = DisableGuard(!settings.EnableTemporalDenoiser && !(settings.EnableGI || settings.EnableGIBounce));

			percentageSlider("Movement Disocclusion", &settings.DepthDisocclusion, 0.f, 10.f);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text(
					"If a pixel has moved too far from the last frame, its radiance will not be carried to this frame.\n"
					"Lower values are stricter.");

			ImGui::Separator();
		}

		{
			auto _ = DisableGuard(!settings.EnableBlur);
			ImGui::SliderFloat("Blur Radius", &settings.BlurRadius, 0.f, 30.f, "%.1f px");

			ImGui::SliderInt("Blur Passes", (int*)&settings.BlurPasses, 1, 3, "%d", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Blurring repeatedly for x times.");

			if (showAdvanced) {
				ImGui::SliderFloat("Geometry Weight", &settings.DistanceNormalisation, 0.f, 5.f, "%.2f");
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text(
						"Higher value makes the blur more sensitive to differences in geometry.");
			}
		}
	}

	///////////////////////////////
	ImGui::SeparatorText("Debug");

	if (ImGui::TreeNode("Buffer Viewer")) {
		auto deferred = Deferred::GetSingleton();

		static float debugRescale = .3f;
		ImGui::SliderFloat("View Resize", &debugRescale, 0.f, 1.f);

		BUFFER_VIEWER_NODE(texNoise, debugRescale)
		BUFFER_VIEWER_NODE(texWorkingDepth, debugRescale)
		BUFFER_VIEWER_NODE(texPrevGeo, debugRescale)
		BUFFER_VIEWER_NODE(texRadiance, debugRescale)
		BUFFER_VIEWER_NODE(texGI[0], debugRescale)
		BUFFER_VIEWER_NODE(texGI[1], debugRescale)
		BUFFER_VIEWER_NODE(texGISpecular[0], debugRescale)
		BUFFER_VIEWER_NODE(texGISpecular[1], debugRescale)

		BUFFER_VIEWER_NODE(deferred->prevDiffuseAmbientTexture, debugRescale)

		ImGui::TreePop();
	}
}

void ScreenSpaceGI::LoadSettings(json& o_json)
{
	settings = o_json;
}

void ScreenSpaceGI::SaveSettings(json& o_json)
{
	o_json = settings;
}

void ScreenSpaceGI::SetupResources()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& device = State::GetSingleton()->device;

	logger::debug("Creating buffers...");
	{
		ssgiCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<SSGICB>());
	}

	logger::debug("Creating textures...");
	{
		D3D11_TEXTURE2D_DESC texDesc{
			.Width = 64,
			.Height = 64,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R32_UINT,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = texDesc.MipLevels }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		auto mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		mainTex.texture->GetDesc(&texDesc);
		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
		texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 5;
		texDesc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;

		{
			texRadiance = eastl::make_unique<Texture2D>(texDesc);
			texRadiance->CreateSRV(srvDesc);
			texRadiance->CreateUAV(uavDesc);
		}

		texDesc.BindFlags &= ~D3D11_BIND_RENDER_TARGET;
		texDesc.MiscFlags &= ~D3D11_RESOURCE_MISC_GENERATE_MIPS;
		texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R16_FLOAT;

		{
			texWorkingDepth = eastl::make_unique<Texture2D>(texDesc);
			texWorkingDepth->CreateSRV(srvDesc);
			for (int i = 0; i < 5; ++i) {
				uavDesc.Texture2D.MipSlice = i;
				DX::ThrowIfFailed(device->CreateUnorderedAccessView(texWorkingDepth->resource.get(), &uavDesc, uavWorkingDepth[i].put()));
			}
		}

		uavDesc.Texture2D.MipSlice = 0;
		texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 1;
		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		{
			texGI[0] = eastl::make_unique<Texture2D>(texDesc);
			texGI[0]->CreateSRV(srvDesc);
			texGI[0]->CreateUAV(uavDesc);

			texGI[1] = eastl::make_unique<Texture2D>(texDesc);
			texGI[1]->CreateSRV(srvDesc);
			texGI[1]->CreateUAV(uavDesc);

			texGISpecular[0] = eastl::make_unique<Texture2D>(texDesc);
			texGISpecular[0]->CreateSRV(srvDesc);
			texGISpecular[0]->CreateUAV(uavDesc);

			texGISpecular[1] = eastl::make_unique<Texture2D>(texDesc);
			texGISpecular[1]->CreateSRV(srvDesc);
			texGISpecular[1]->CreateUAV(uavDesc);
		}

		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R8_UNORM;
		{
			texAccumFrames[0] = eastl::make_unique<Texture2D>(texDesc);
			texAccumFrames[0]->CreateSRV(srvDesc);
			texAccumFrames[0]->CreateUAV(uavDesc);

			texAccumFrames[1] = eastl::make_unique<Texture2D>(texDesc);
			texAccumFrames[1]->CreateSRV(srvDesc);
			texAccumFrames[1]->CreateUAV(uavDesc);
		}

		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
		{
			texPrevGeo = eastl::make_unique<Texture2D>(texDesc);
			texPrevGeo->CreateSRV(srvDesc);
			texPrevGeo->CreateUAV(uavDesc);
		}
	}

	logger::debug("Loading noise texture...");
	{
		DirectX::ScratchImage image;
		try {
			std::filesystem::path path{ "Data\\Shaders\\ScreenSpaceGI\\fast_2uges.dds" };

			DX::ThrowIfFailed(LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		ID3D11Resource* pResource = nullptr;
		try {
			DX::ThrowIfFailed(CreateTexture(device,
				image.GetImages(), image.GetImageCount(),
				image.GetMetadata(), &pResource));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			return;
		}

		texNoise = eastl::make_unique<Texture2D>(reinterpret_cast<ID3D11Texture2D*>(pResource));

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texNoise->desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = 1 }
		};
		texNoise->CreateSRV(srvDesc);
	}

	logger::debug("Creating samplers...");
	{
		D3D11_SAMPLER_DESC samplerDesc = {
			.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
			.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
			.MaxAnisotropy = 1,
			.MinLOD = 0,
			.MaxLOD = D3D11_FLOAT32_MAX
		};
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, linearClampSampler.put()));

		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, pointClampSampler.put()));
	}

	CompileComputeShaders();
}

void ScreenSpaceGI::ClearShaderCache()
{
	static const std::vector<winrt::com_ptr<ID3D11ComputeShader>*> shaderPtrs = {
		&prefilterDepthsCompute, &radianceDisoccCompute, &giCompute, &blurCompute, &blurSpecularCompute, &upsampleCompute
	};

	for (auto shader : shaderPtrs)
		if ((*shader)) {
			(*shader)->Release();
			shader->detach();
		}

	CompileComputeShaders();
}

void ScreenSpaceGI::CompileComputeShaders()
{
	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* programPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines;
	};

	std::vector<ShaderCompileInfo>
		shaderInfos = {
			{ &prefilterDepthsCompute, "prefilterDepths.cs.hlsl", {} },
			{ &radianceDisoccCompute, "radianceDisocc.cs.hlsl", {} },
			{ &giCompute, "gi.cs.hlsl", {} },
			{ &blurCompute, "blur.cs.hlsl", {} },
			{ &blurSpecularCompute, "blur.cs.hlsl", { { "SPECULAR_BLUR", "" } } },
			{ &upsampleCompute, "upsample.cs.hlsl", {} },
		};
	for (auto& info : shaderInfos) {
		if (REL::Module::IsVR())
			info.defines.push_back({ "VR", "" });
		if (settings.HalfRes)
			info.defines.push_back({ "HALF_RES", "" });
		if (settings.HalfRate)
			info.defines.push_back({ "HALF_RATE", "" });
		if (settings.EnableTemporalDenoiser)
			info.defines.push_back({ "TEMPORAL_DENOISER", "" });
		if (settings.EnableGI)
			info.defines.push_back({ "GI", "" });
		if (settings.EnableSpecularGI)
			info.defines.push_back({ "GI_SPECULAR", "" });
		if (settings.EnableGIBounce)
			info.defines.push_back({ "GI_BOUNCE", "" });
	}

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\ScreenSpaceGI") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0")))
			info.programPtr->attach(rawPtr);
	}

	recompileFlag = false;
}

bool ScreenSpaceGI::ShadersOK()
{
	return texNoise && prefilterDepthsCompute && radianceDisoccCompute && giCompute && blurCompute && blurSpecularCompute && upsampleCompute;
}

void ScreenSpaceGI::UpdateSB()
{
	auto viewport = RE::BSGraphics::State::GetSingleton();

	float2 res = { (float)texRadiance->desc.Width, (float)texRadiance->desc.Height };
	float2 dynres = Util::ConvertToDynamic(res);
	dynres = { floor(dynres.x), floor(dynres.y) };

	static float4x4 prevInvView[2] = {};

	SSGICB data;
	{
		for (int eyeIndex = 0; eyeIndex < (1 + REL::Module::IsVR()); ++eyeIndex) {
			auto eye = Util::GetCameraData(eyeIndex);

			data.PrevInvViewMat[eyeIndex] = prevInvView[eyeIndex];
			data.NDCToViewMul[eyeIndex] = { 2.0f / eye.projMat(0, 0), -2.0f / eye.projMat(1, 1) };
			data.NDCToViewAdd[eyeIndex] = { -1.0f / eye.projMat(0, 0), 1.0f / eye.projMat(1, 1) };
			if (REL::Module::IsVR())
				data.NDCToViewMul[eyeIndex].x *= 2;

			prevInvView[eyeIndex] = eye.viewMat.Invert();
		}

		data.TexDim = res;
		data.RcpTexDim = float2(1.0f) / res;
		data.FrameDim = dynres;
		data.RcpFrameDim = float2(1.0f) / dynres;
		data.FrameIndex = viewport->frameCount;

		data.NumSlices = settings.NumSlices;
		data.NumSteps = settings.NumSteps;
		data.DepthMIPSamplingOffset = settings.DepthMIPSamplingOffset;

		data.EffectRadius = std::max(settings.AORadius, settings.GIRadius);
		data.AORadius = settings.AORadius / data.EffectRadius;
		data.GIRadius = settings.GIRadius / data.EffectRadius;
		data.Thickness = settings.Thickness;
		data.DepthFadeRange = settings.DepthFadeRange;
		data.DepthFadeScaleConst = 1 / (settings.DepthFadeRange.y - settings.DepthFadeRange.x);

		data.BackfaceStrength = settings.BackfaceStrength;
		data.GIBounceFade = settings.GIBounceFade;
		data.GIDistanceCompensation = settings.GIDistanceCompensation;
		data.GICompensationMaxDist = settings.AORadius;

		data.AOPower = settings.AOPower;
		data.GIStrength = settings.GIStrength;

		data.DepthDisocclusion = settings.DepthDisocclusion;
		data.NormalDisocclusion = settings.NormalDisocclusion;
		data.MaxAccumFrames = settings.MaxAccumFrames;
		data.BlurRadius = settings.BlurRadius;
		data.DistanceNormalisation = settings.DistanceNormalisation;
	}

	ssgiCB->Update(data);
}

void ScreenSpaceGI::DrawSSGI(Texture2D* srcPrevAmbient)
{
	auto& context = State::GetSingleton()->context;

	if (!(settings.Enabled && ShadersOK())) {
		FLOAT clr[4] = { 0., 0., 0., 0. };
		context->ClearUnorderedAccessViewFloat(texGI[outputGIIdx]->uav.get(), clr);
		context->ClearUnorderedAccessViewFloat(texGISpecular[outputGIIdx]->uav.get(), clr);

		return;
	}

	ZoneScoped;
	TracyD3D11Zone(State::GetSingleton()->tracyCtx, "SSGI");

	static uint lastFrameGITexIdx = 0;
	static uint lastFrameAccumTexIdx = 0;
	uint inputGITexIdx = lastFrameGITexIdx;

	//////////////////////////////////////////////////////

	if (recompileFlag)
		ClearShaderCache();

	UpdateSB();

	bool doSpecular = settings.EnableGI && settings.EnableSpecularGI;

	//////////////////////////////////////////////////////

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto rts = renderer->GetRuntimeData().renderTargets;
	auto deferred = Deferred::GetSingleton();

	float2 size = Util::ConvertToDynamic(State::GetSingleton()->screenSize);
	uint resolution[2] = { (uint)size.x, (uint)size.y };
	uint halfRes[2] = { resolution[0] >> 1, resolution[1] >> 1 };
	auto internalRes = settings.HalfRes ? halfRes : resolution;

	std::array<ID3D11ShaderResourceView*, 9> srvs = { nullptr };
	std::array<ID3D11UnorderedAccessView*, 5> uavs = { nullptr };
	std::array<ID3D11SamplerState*, 2> samplers = { pointClampSampler.get(), linearClampSampler.get() };
	auto cb = ssgiCB->CB();

	auto resetViews = [&]() {
		srvs.fill(nullptr);
		uavs.fill(nullptr);

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	};

	//////////////////////////////////////////////////////

	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());

	// prefilter depths
	{
		TracyD3D11Zone(State::GetSingleton()->tracyCtx, "SSGI - Prefilter Depths");

		srvs.at(0) = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV;
		for (int i = 0; i < 5; ++i)
			uavs.at(i) = uavWorkingDepth[i].get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(prefilterDepthsCompute.get(), nullptr, 0);
		context->Dispatch((resolution[0] + 15) >> 4, (resolution[1] + 15) >> 4, 1);
	}

	// fetch radiance and disocclusion
	{
		TracyD3D11Zone(State::GetSingleton()->tracyCtx, "SSGI - Radiance Disocc");

		resetViews();
		srvs.at(0) = rts[deferred->forwardRenderTargets[0]].SRV;
		srvs.at(1) = texGI[inputGITexIdx]->srv.get();
		srvs.at(2) = doSpecular ? texGISpecular[inputGITexIdx]->srv.get() : nullptr;
		srvs.at(3) = texWorkingDepth->srv.get();
		srvs.at(4) = rts[NORMALROUGHNESS].SRV;
		srvs.at(5) = texPrevGeo->srv.get();
		srvs.at(6) = rts[RE::RENDER_TARGET::kMOTION_VECTOR].SRV;
		srvs.at(7) = srcPrevAmbient->srv.get();
		srvs.at(8) = texAccumFrames[lastFrameAccumTexIdx]->srv.get();

		uavs.at(0) = texRadiance->uav.get();
		uavs.at(1) = texAccumFrames[!lastFrameAccumTexIdx]->uav.get();
		uavs.at(2) = texGI[!inputGITexIdx]->uav.get();
		uavs.at(3) = doSpecular ? texGISpecular[!inputGITexIdx]->uav.get() : nullptr;

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(radianceDisoccCompute.get(), nullptr, 0);
		context->Dispatch((internalRes[0] + 7u) >> 3, (internalRes[1] + 7u) >> 3, 1);

		context->GenerateMips(texRadiance->srv.get());

		inputGITexIdx = !inputGITexIdx;
		lastFrameAccumTexIdx = !lastFrameAccumTexIdx;
	}

	// GI
	{
		TracyD3D11Zone(State::GetSingleton()->tracyCtx, "SSGI - GI");

		resetViews();
		srvs.at(0) = texWorkingDepth->srv.get();
		srvs.at(1) = rts[NORMALROUGHNESS].SRV;
		srvs.at(2) = texRadiance->srv.get();
		srvs.at(3) = texNoise->srv.get();
		srvs.at(4) = texAccumFrames[lastFrameAccumTexIdx]->srv.get();
		srvs.at(5) = texGI[inputGITexIdx]->srv.get();
		srvs.at(6) = texGISpecular[inputGITexIdx]->srv.get();

		uavs.at(0) = texGI[!inputGITexIdx]->uav.get();
		uavs.at(1) = texGISpecular[!inputGITexIdx]->uav.get();
		uavs.at(2) = nullptr;
		uavs.at(3) = texPrevGeo->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(giCompute.get(), nullptr, 0);
		context->Dispatch((internalRes[0] + 7u) >> 3, (internalRes[1] + 7u) >> 3, 1);

		inputGITexIdx = !inputGITexIdx;
		lastFrameGITexIdx = inputGITexIdx;
	}

	// blur
	if (settings.EnableBlur) {
		for (uint i = 0; i < settings.BlurPasses; i++) {
			if (doSpecular) {
				TracyD3D11Zone(State::GetSingleton()->tracyCtx, "SSGI - Specular Blur");
				resetViews();
				srvs.at(0) = texGISpecular[inputGITexIdx]->srv.get();
				srvs.at(1) = texAccumFrames[lastFrameAccumTexIdx]->srv.get();
				srvs.at(2) = texWorkingDepth->srv.get();
				srvs.at(3) = rts[NORMALROUGHNESS].SRV;

				uavs.at(0) = texGISpecular[!inputGITexIdx]->uav.get();
				uavs.at(1) = nullptr;

				context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
				context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
				context->CSSetShader(blurSpecularCompute.get(), nullptr, 0);
				context->Dispatch((internalRes[0] + 7u) >> 3, (internalRes[1] + 7u) >> 3, 1);
			}

			TracyD3D11Zone(State::GetSingleton()->tracyCtx, "SSGI - Diffuse Blur");

			resetViews();
			srvs.at(0) = texGI[inputGITexIdx]->srv.get();
			srvs.at(1) = texAccumFrames[lastFrameAccumTexIdx]->srv.get();
			srvs.at(2) = texWorkingDepth->srv.get();
			srvs.at(3) = rts[NORMALROUGHNESS].SRV;

			uavs.at(0) = texGI[!inputGITexIdx]->uav.get();
			uavs.at(1) = texAccumFrames[!lastFrameAccumTexIdx]->uav.get();

			context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
			context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
			context->CSSetShader(blurCompute.get(), nullptr, 0);
			context->Dispatch((internalRes[0] + 7u) >> 3, (internalRes[1] + 7u) >> 3, 1);

			inputGITexIdx = !inputGITexIdx;
			lastFrameGITexIdx = inputGITexIdx;
			lastFrameAccumTexIdx = !lastFrameAccumTexIdx;
		}
	}

	// upsasmple
	if (settings.HalfRes) {
		resetViews();
		srvs.at(0) = texWorkingDepth->srv.get();
		srvs.at(1) = texGI[inputGITexIdx]->srv.get();

		uavs.at(0) = texGI[!inputGITexIdx]->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(upsampleCompute.get(), nullptr, 0);
		context->Dispatch((resolution[0] + 7u) >> 3, (resolution[1] + 7u) >> 3, 1);

		if (doSpecular) {
			resetViews();
			srvs.at(0) = texWorkingDepth->srv.get();
			srvs.at(1) = texGISpecular[inputGITexIdx]->srv.get();

			uavs.at(0) = texGISpecular[!inputGITexIdx]->uav.get();

			context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
			context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
			context->CSSetShader(upsampleCompute.get(), nullptr, 0);
			context->Dispatch((resolution[0] + 7u) >> 3, (resolution[1] + 7u) >> 3, 1);
		}

		inputGITexIdx = !inputGITexIdx;
	}

	outputGIIdx = inputGITexIdx;

	// cleanup
	resetViews();

	samplers.fill(nullptr);
	cb = nullptr;

	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
	context->CSSetShader(nullptr, nullptr, 0);
}