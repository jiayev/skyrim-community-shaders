#include "ScreenSpaceGI.h"

#include "Deferred.h"
#include "State.h"
#include "Util.h"

#include "DirectXTex.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenSpaceGI::Settings,
	Enabled,
	UseBitmask,
	EnableGI,
	EnableTemporalDenoiser,
	NumSlices,
	NumSteps,
	DepthMIPSamplingOffset,
	EffectRadius,
	EffectFalloffRange,
	ThinOccluderCompensation,
	Thickness,
	DepthFadeRange,
	CheckBackface,
	BackfaceStrength,
	EnableGIBounce,
	GIBounceFade,
	GIDistanceCompensation,
	GICompensationMaxDist,
	AOPower,
	GIStrength,
	DepthDisocclusion,
	MaxAccumFrames)

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

bool percentageSlider(const char* label, float* data, const char* format = "%.1f %%")
{
	float percentageData = (*data) * 1e2f;
	bool retval = ImGui::SliderFloat(label, &percentageData, 0.f, 100.f, format);
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
	///////////////////////////////
	ImGui::SeparatorText("Toggles");

	if (ImGui::BeginTable("Toggles", 3)) {
		ImGui::TableNextColumn();
		ImGui::Checkbox("Enabled", &settings.Enabled);
		ImGui::TableNextColumn();
		recompileFlag |= ImGui::Checkbox("GI", &settings.EnableGI);
		ImGui::TableNextColumn();
		recompileFlag |= ImGui::Checkbox("Bitmask", &settings.UseBitmask);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("An alternative way to calculate AO/GI");

		ImGui::EndTable();
	}

	///////////////////////////////
	ImGui::SeparatorText("Quality/Performance");

	ImGui::SliderInt("Slices", (int*)&settings.NumSlices, 1, 10);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("How many directions do the samples take. A greater value reduces noise but is more expensive.");

	ImGui::SliderInt("Steps Per Slice", (int*)&settings.NumSteps, 1, 20);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("How many samples does it take in one direction. A greater value enhances the effects but is more expensive.");

	ImGui::SliderFloat("MIP Sampling Offset", &settings.DepthMIPSamplingOffset, 2.f, 6.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Mainly performance (texture memory bandwidth) setting but as a side-effect reduces overshadowing by thin objects and increases temporal instability.");

	if (ImGui::BeginTable("Quality Toggles", 2)) {
		ImGui::TableNextColumn();
		recompileFlag |= ImGui::Checkbox("Half Resolution", &settings.HalfRes);

		ImGui::EndTable();
	}

	///////////////////////////////
	ImGui::SeparatorText("Visual");

	ImGui::SliderFloat("AO Power", &settings.AOPower, 0.f, 3.f, "%.2f");

	{
		auto _ = DisableGuard(!settings.EnableGI);
		ImGui::SliderFloat("GI Strength", &settings.GIStrength, 0.f, 20.f, "%.2f");
		// percentageSlider("GI Saturation", &settings.GISaturation);
	}

	ImGui::Separator();

	ImGui::SliderFloat("Effect radius", &settings.EffectRadius, 10.f, 300.0f, "%.1f game units");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("World (viewspace) effect radius. Depends on the scene & requirements");

	ImGui::SliderFloat2("Depth Fade Range", &settings.DepthFadeRange.x, 1e4, 5e4, "%.0f game units");

	ImGui::Separator();

	{
		auto _ = DisableGuard(settings.UseBitmask);

		ImGui::SliderFloat("Falloff Range", &settings.EffectFalloffRange, 0.05f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Gently reduce sample impact as it gets out of 'Effect radius' bounds");

		ImGui::SliderFloat("Thin Occluder Compensation", &settings.ThinOccluderCompensation, 0.f, 0.7f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Slightly reduce impact of samples further back to counter the bias from depth-based (incomplete) input scene geometry data");
	}
	{
		auto _ = DisableGuard(!settings.UseBitmask);

		ImGui::SliderFloat("Thickness", &settings.Thickness, 0.f, 500.0f, "%.1f game units");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("How thick the occluders are. 20 to 30 percent of effect radius is recommended.");
	}

	///////////////////////////////
	ImGui::SeparatorText("Visual - GI");

	{
		auto _ = DisableGuard(!settings.EnableGI);

		ImGui::SliderFloat("GI Distance Compensation", &settings.GIDistanceCompensation, 0.0f, 9.0f, "%.1f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Brighten up further radiance samples that are otherwise too weak. Creates a wider GI look.\n"
				"If using bitmask, this value should be roughly inverse to thickness.");

		ImGui::SliderFloat("GI Compensation Distance", &settings.GICompensationMaxDist, 10.0f, 500.0f, "%.1f game units");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("The distance of maximal compensation/brightening.");

		ImGui::Separator();

		recompileFlag |= ImGui::Checkbox("GI Bounce", &settings.EnableGIBounce);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Simulates multiple light bounces. Better with denoiser on.");

		{
			auto __ = DisableGuard(!settings.EnableGIBounce);
			ImGui::Indent();
			percentageSlider("GI Bounce Strength", &settings.GIBounceFade);
			ImGui::Unindent();
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("How much of this frame's GI gets carried to the next frame.");
		}

		ImGui::Separator();

		recompileFlag |= ImGui::Checkbox("Backface Checks", &settings.CheckBackface);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Disable to get some frames, IF you don't care about light emitting from the back of objects.");
		{
			auto __ = DisableGuard(!settings.CheckBackface);
			ImGui::Indent();
			percentageSlider("Backface Lighting Mix", &settings.BackfaceStrength);
			ImGui::Unindent();
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("How bright at the back of objects is compared to the front. A small value to make up for foliage translucency.");
		}
	}

	///////////////////////////////
	ImGui::SeparatorText("Denoising");

	ImGui::TextWrapped("At full resolution, you can try disabling denoisers and let TAA handle the noise.");

	recompileFlag |= ImGui::Checkbox("Temporal Denoiser", &settings.EnableTemporalDenoiser);

	{
		auto _ = DisableGuard(!settings.EnableTemporalDenoiser);
		ImGui::Indent();
		ImGui::SliderInt("Max Frame Accumulation", (int*)&settings.MaxAccumFrames, 1, 64, "%d", ImGuiSliderFlags_AlwaysClamp);
		ImGui::Unindent();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("How many past frames to accumulate results with. Higher values are less noisy but potentially cause ghosting.");
	}

	// ImGui::SliderInt("Passes", (int*)&settings.DenoisePasses, 0, 10);
	// if (auto _tt = Util::HoverTooltipWrapper())
	// 	ImGui::Text("How many denoising passes to go through. The more the blurrier.");

	{
		auto _ = DisableGuard(!settings.EnableTemporalDenoiser && !(settings.EnableGI || settings.EnableGIBounce));

		ImGui::SliderFloat("Movement Disocclusion", &settings.DepthDisocclusion, 0.f, 100.f, "%.1f game units");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"If a pixel has moved this far from the last frame, its radiance will not be carried to this frame.\n"
				"Lower values are stricter.");
	}

	///////////////////////////////
	ImGui::SeparatorText("Debug");

	if (ImGui::TreeNode("Buffer Viewer")) {
		static float debugRescale = .3f;
		ImGui::SliderFloat("View Resize", &debugRescale, 0.f, 1.f);

		// ImGui doesn't support U32
		// if (ImGui::TreeNode("texHilbertLUT")) {
		// 	ImGui::Image(texHilbertLUT->srv.get(), { (float)texHilbertLUT->desc.Width, (float)texHilbertLUT->desc.Height });
		// 	ImGui::TreePop();
		// }
		if (ImGui::TreeNode("texWorkingDepth")) {
			ImGui::Image(texWorkingDepth->srv.get(), { texWorkingDepth->desc.Width * debugRescale, texWorkingDepth->desc.Height * debugRescale });
			ImGui::TreePop();
		}
		if (ImGui::TreeNode("texPrevDepth")) {
			ImGui::Image(texPrevDepth->srv.get(), { texPrevDepth->desc.Width * debugRescale, texPrevDepth->desc.Height * debugRescale });
			ImGui::TreePop();
		}
		if (ImGui::TreeNode("texRadiance")) {
			ImGui::Image(texRadiance->srv.get(), { texRadiance->desc.Width * debugRescale, texRadiance->desc.Height * debugRescale });
			ImGui::TreePop();
		}
		if (ImGui::TreeNode("texGI0")) {
			ImGui::Image(texGI0->srv.get(), { texGI0->desc.Width * debugRescale, texGI0->desc.Height * debugRescale });
			ImGui::TreePop();
		}
		if (ImGui::TreeNode("texGI1")) {
			ImGui::Image(texGI1->srv.get(), { texGI1->desc.Width * debugRescale, texGI1->desc.Height * debugRescale });
			ImGui::TreePop();
		}
		if (ImGui::TreeNode("texPrevGIAlbedo")) {
			ImGui::Image(texPrevGIAlbedo->srv.get(), { texPrevGIAlbedo->desc.Width * debugRescale, texPrevGIAlbedo->desc.Height * debugRescale });
			ImGui::TreePop();
		}

		ImGui::TreePop();
	}
}

void ScreenSpaceGI::Load(json& o_json)
{
	if (o_json[GetName()].is_object())
		settings = o_json[GetName()];

	Feature::Load(o_json);
}

void ScreenSpaceGI::Save([[maybe_unused]] json& o_json)
{
	o_json[GetName()] = settings;
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

		{
			texHilbertLUT = eastl::make_unique<Texture2D>(texDesc);
			texHilbertLUT->CreateSRV(srvDesc);
			texHilbertLUT->CreateUAV(uavDesc);
		}

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
			texGI0 = eastl::make_unique<Texture2D>(texDesc);
			texGI0->CreateSRV(srvDesc);
			texGI0->CreateUAV(uavDesc);

			texGI1 = eastl::make_unique<Texture2D>(texDesc);
			texGI1->CreateSRV(srvDesc);
			texGI1->CreateUAV(uavDesc);
		}

		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
		{
			texPrevGIAlbedo = eastl::make_unique<Texture2D>(texDesc);
			texPrevGIAlbedo->CreateSRV(srvDesc);
			texPrevGIAlbedo->CreateUAV(uavDesc);
		}

		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R8_UINT;
		{
			texAccumFrames = eastl::make_unique<Texture2D>(texDesc);
			texAccumFrames->CreateSRV(srvDesc);
			texAccumFrames->CreateUAV(uavDesc);
		}

		srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R16_FLOAT;
		{
			texPrevDepth = eastl::make_unique<Texture2D>(texDesc);
			texPrevDepth->CreateSRV(srvDesc);
			texPrevDepth->CreateUAV(uavDesc);
		}
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
		&hilbertLutCompute, &prefilterDepthsCompute, &radianceDisoccCompute, &giCompute, &upsampleCompute, &outputCompute
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
			{ &hilbertLutCompute, "hilbert.cs.hlsl", {} },
			{ &prefilterDepthsCompute, "prefilterDepths.cs.hlsl", {} },
			{ &radianceDisoccCompute, "radianceDisocc.cs.hlsl", {} },
			{ &giCompute, "gi.cs.hlsl", {} },
			{ &upsampleCompute, "upsample.cs.hlsl", {} },
			{ &outputCompute, "output.cs.hlsl", {} }
		};
	for (auto& info : shaderInfos) {
		if (REL::Module::IsVR())
			info.defines.push_back({ "VR", "" });
		if (settings.HalfRes)
			info.defines.push_back({ "HALF_RES", "" });
		if (settings.EnableTemporalDenoiser)
			info.defines.push_back({ "TEMPORAL_DENOISER", "" });
		if (settings.UseBitmask)
			info.defines.push_back({ "BITMASK", "" });
		if (settings.EnableGI)
			info.defines.push_back({ "GI", "" });
		if (settings.EnableGIBounce)
			info.defines.push_back({ "GI_BOUNCE", "" });
		if (settings.CheckBackface)
			info.defines.push_back({ "BACKFACE", "" });
	}

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\ScreenSpaceGI") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0")))
			info.programPtr->attach(rawPtr);
	}

	hilbertLutGenFlag = true;
	recompileFlag = false;
}

bool ScreenSpaceGI::ShadersOK()
{
	return hilbertLutCompute && prefilterDepthsCompute && radianceDisoccCompute && giCompute && upsampleCompute && outputCompute;
}

void ScreenSpaceGI::GenerateHilbertLUT()
{
	auto& context = State::GetSingleton()->context;

	ID3D11UnorderedAccessView* uav = texHilbertLUT->uav.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShader(hilbertLutCompute.get(), nullptr, 0);

	context->Dispatch(2, 2, 1);

	uav = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);

	hilbertLutGenFlag = false;
}

void ScreenSpaceGI::UpdateSB()
{
	auto viewport = RE::BSGraphics::State::GetSingleton();
	auto& state = State::GetSingleton()->shadowState;

	uint resolution[2] = {
		(uint)(State::GetSingleton()->screenWidth * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale),
		(uint)(State::GetSingleton()->screenHeight * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale)
	};
	uint halfRes[2] = { (resolution[0] + 1) >> 1, (resolution[1] + 1) >> 1 };

	float2 res = settings.HalfRes ? float2{ (float)halfRes[0], (float)halfRes[1] } : float2{ (float)resolution[0], (float)resolution[1] };

	static float4x4 prevInvView[2] = {};

	SSGICB data;
	{
		for (int eyeIndex = 0; eyeIndex < (1 + REL::Module::IsVR()); ++eyeIndex) {
			auto eye = (!REL::Module::IsVR()) ? state->GetRuntimeData().cameraData.getEye(eyeIndex) : state->GetVRRuntimeData().cameraData.getEye(eyeIndex);

			data.PrevInvViewMat[eyeIndex] = prevInvView[eyeIndex];
			data.NDCToViewMul[eyeIndex] = { 2.0f / eye.projMat(0, 0), -2.0f / eye.projMat(1, 1) };
			data.NDCToViewAdd[eyeIndex] = { -1.0f / eye.projMat(0, 0), 1.0f / eye.projMat(1, 1) };
			data.NDCToViewMul_x_PixelSize[eyeIndex] = data.NDCToViewMul[eyeIndex] / res;
			if (REL::Module::IsVR())
				data.NDCToViewMul[eyeIndex].x *= 2;

			prevInvView[eyeIndex] = eye.viewMat.Invert();
		}

		data.FrameDim = res;
		data.RcpFrameDim = float2(1.0f) / res;
		data.FrameIndex = viewport->uiFrameCount;

		data.NumSlices = settings.NumSlices;
		data.NumSteps = settings.NumSteps;
		data.DepthMIPSamplingOffset = settings.DepthMIPSamplingOffset;

		data.EffectRadius = settings.EffectRadius;
		data.EffectFalloffRange = settings.EffectFalloffRange;
		data.ThinOccluderCompensation = settings.ThinOccluderCompensation;
		data.Thickness = settings.Thickness;
		data.DepthFadeRange = settings.DepthFadeRange;
		data.DepthFadeScaleConst = 1 / (settings.DepthFadeRange.y - settings.DepthFadeRange.x);

		data.BackfaceStrength = settings.BackfaceStrength;
		data.GIBounceFade = settings.GIBounceFade;
		data.GIDistanceCompensation = settings.GIDistanceCompensation;
		data.GICompensationMaxDist = settings.GICompensationMaxDist;

		data.AOPower = settings.AOPower;
		data.GIStrength = settings.GIStrength;

		data.DepthDisocclusion = settings.DepthDisocclusion;
		data.MaxAccumFrames = settings.MaxAccumFrames;
	}

	ssgiCB->Update(data);
}

void ScreenSpaceGI::DrawSSGI(Texture2D* outGI)
{
	if (!(settings.Enabled && ShadersOK()))
		return;

	//////////////////////////////////////////////////////

	if (recompileFlag)
		ClearShaderCache();

	if (hilbertLutGenFlag)
		GenerateHilbertLUT();

	UpdateSB();

	//////////////////////////////////////////////////////

	auto& context = State::GetSingleton()->context;
	auto viewport = RE::BSGraphics::State::GetSingleton();
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto rts = renderer->GetRuntimeData().renderTargets;
	auto deferred = Deferred::GetSingleton();

	uint resolution[2] = {
		(uint)(State::GetSingleton()->screenWidth * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale),
		(uint)(State::GetSingleton()->screenHeight * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale)
	};
	uint halfRes[2] = { resolution[0] >> 1, resolution[1] >> 1 };
	auto targetRes = settings.HalfRes ? halfRes : resolution;

	std::array<ID3D11ShaderResourceView*, 7> srvs = { nullptr };
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
		srvs[0] = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV;
		for (int i = 0; i < 5; ++i)
			uavs[i] = uavWorkingDepth[i].get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(prefilterDepthsCompute.get(), nullptr, 0);
		context->Dispatch((resolution[0] + 15) >> 4, (resolution[1] + 15) >> 4, 1);
	}

	// fetch radiance and disocclusion
	{
		resetViews();
		srvs[0] = rts[deferred->forwardRenderTargets[0]].SRV;
		srvs[1] = texGI0->srv.get();
		srvs[2] = texWorkingDepth->srv.get();
		srvs[3] = rts[NORMALROUGHNESS].SRV;
		srvs[4] = texPrevDepth->srv.get();
		srvs[5] = rts[RE::RENDER_TARGET::kMOTION_VECTOR].SRV;
		srvs[6] = texPrevGIAlbedo->srv.get();

		uavs[0] = texRadiance->uav.get();
		uavs[1] = texAccumFrames->uav.get();
		uavs[2] = texGI1->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(radianceDisoccCompute.get(), nullptr, 0);
		context->Dispatch((targetRes[0] + 7u) >> 3, (targetRes[1] + 7u) >> 3, 1);

		context->GenerateMips(texRadiance->srv.get());
	}

	// GI
	{
		resetViews();
		srvs[0] = texWorkingDepth->srv.get();
		srvs[1] = rts[NORMALROUGHNESS].SRV;
		srvs[2] = texRadiance->srv.get();
		srvs[3] = texHilbertLUT->srv.get();
		srvs[4] = texAccumFrames->srv.get();
		srvs[5] = texGI1->srv.get();

		uavs[0] = texGI0->uav.get();
		uavs[1] = nullptr;
		uavs[2] = texPrevDepth->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(giCompute.get(), nullptr, 0);
		context->Dispatch((targetRes[0] + 7u) >> 3, (targetRes[1] + 7u) >> 3, 1);
	}

	// upsasmple
	if (settings.HalfRes) {
		resetViews();
		srvs[0] = texWorkingDepth->srv.get();
		srvs[1] = texGI0->srv.get();

		uavs[0] = texGI1->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(upsampleCompute.get(), nullptr, 0);
		context->Dispatch((resolution[0] + 7u) >> 3, (resolution[1] + 7u) >> 3, 1);
	}

	// output
	{
		resetViews();
		srvs[0] = settings.HalfRes ? texGI1->srv.get() : texGI0->srv.get();
		srvs[1] = rts[ALBEDO].SRV;

		uavs[0] = outGI->uav.get();
		uavs[1] = texPrevGIAlbedo->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(outputCompute.get(), nullptr, 0);
		context->Dispatch((resolution[0] + 7u) >> 3, (resolution[1] + 7u) >> 3, 1);
	}

	// cleanup
	resetViews();

	samplers.fill(nullptr);
	cb = nullptr;

	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
	context->CSSetShader(nullptr, nullptr, 0);
}