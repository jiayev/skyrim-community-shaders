#include "DoF.h"

#include "Features/PostProcessing.h"
#include "Menu.h"
#include "State.h"
#include "Util.h"

#include "I18n/I18n.h"
#include <DDSTextureLoader.h>
#include <DirectXTex.h>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	DoF::Settings,
	AutoFocus,
	TransitionSpeed,
	FocusCoord,
	ManualFocusPlane,
	FocalLength,
	FNumber,
	FarPlaneMaxBlur,
	NearPlaneMaxBlur,
	BlurQuality,
	NearFarDistanceCompensation,
	HighlightBoost,
	BokehBusyFactor,
	PostBlurSmoothing,
	PetzvalStrength,
	HighlightShape,
	HighlightShapeRotationAngle,
	MaxNearCoCRadius,
	MaxFarCoCRadius,
	targetFocus,
	targetFocusFocalLength,
	consoleSelection)

void DoF::DrawSettings()
{
	ImGui::Checkbox(T("feature.post_processing.do_f.auto_focus", "Auto Focus"), &settings.AutoFocus);

	if (settings.AutoFocus) {
		ImGui::SliderFloat2(T("feature.post_processing.do_f.focus_point", "Focus Point"), &settings.FocusCoord.x, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	}
	ImGui::SliderFloat(T("feature.post_processing.do_f.transition_speed", "Transition Speed"), &settings.TransitionSpeed, 0.1f, 1.0f, "%.2f");
	ImGui::SliderFloat(T("feature.post_processing.do_f.manual_focus", "Manual Focus"), &settings.ManualFocusPlane, 0.1f, 150.0f, "%.2f m");
	ImGui::SliderFloat(T("feature.post_processing.do_f.focal_length", "Focal Length"), &settings.FocalLength, 1.0f, 300.0f, "%.1f mm");
	ImGui::SliderFloat(T("feature.post_processing.do_f.f_number", "F-Number"), &settings.FNumber, 1.0f, 22.0f, "f/%.1f");
	ImGui::SliderFloat(T("feature.post_processing.do_f.far_plane_max_blur", "Far Plane Max Blur"), &settings.FarPlaneMaxBlur, 0.0f, 8.0f, "%.2f");
	ImGui::SliderFloat(T("feature.post_processing.do_f.near_plane_max_blur", "Near Plane Max Blur"), &settings.NearPlaneMaxBlur, 0.0f, 4.0f, "%.2f");
	ImGui::SliderFloat(T("feature.post_processing.do_f.max_far_coc_radius", "Max Far Blur Radius"), &settings.MaxFarCoCRadius, 0.001f, 0.1f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(T("feature.post_processing.do_f.max_far_coc_radius_desc", "Upper bound of the far field blur disc radius, as a fraction of the screen width. Caps how expensive/undersampled the gather can get."));
	ImGui::SliderFloat(T("feature.post_processing.do_f.max_near_coc_radius", "Max Near Blur Radius"), &settings.MaxNearCoCRadius, 0.001f, 0.1f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(T("feature.post_processing.do_f.max_near_coc_radius_desc", "Upper bound of the near field blur disc radius, as a fraction of the screen width."));
	ImGui::SliderFloat(T("feature.post_processing.do_f.blur_quality", "Blur Quality"), &settings.BlurQuality, 2.0f, 30.0f, "%.1f");
	ImGui::SliderFloat(T("feature.post_processing.do_f.near_far_plane_distance_compenation", "Near-Far Plane Distance Compenation"), &settings.NearFarDistanceCompensation, 1.0f, 5.0f, "%.2f");
	ImGui::SliderFloat(T("feature.post_processing.do_f.bokeh_busy_factor", "Bokeh Busy Factor"), &settings.BokehBusyFactor, 0.0f, 1.0f, "%.2f");
	ImGui::SliderFloat(T("feature.post_processing.do_f.petzval_strength", "Petzval Strength"), &settings.PetzvalStrength, 0.0f, 2.0f, "%.2f");
	ImGui::SliderFloat(T("feature.post_processing.do_f.highlight_boost", "Highlight Boost"), &settings.HighlightBoost, 0.0f, 1.0f, "%.2f");
	ImGui::SliderFloat(T("feature.post_processing.do_f.post_blur_smoothing", "Post Blur Smoothing"), &settings.PostBlurSmoothing, 0.0f, 2.0f, "%.2f");
	ImGui::Combo(T("feature.post_processing.do_f.highlight_custom_shape", "Highlight Custom Shape"), &settings.HighlightShape, "Circle (No custom shape)\0Heart\0Hexagon\0Circle with fringe\0Hexagon with fringe\0Star\0Square\0");
	ImGui::SliderFloat(T("feature.post_processing.do_f.highlight_shape_rotation", "Highlight Shape Rotation"), &settings.HighlightShapeRotationAngle, 0.0f, 1.0f, "%.2f");
	ImGui::Checkbox(T("feature.post_processing.do_f.target_focus", "Target Focus"), &settings.targetFocus);
	ImGui::SliderFloat(T("feature.post_processing.do_f.target_focus_focal_length", "Target Focus Focal Length"), &settings.targetFocusFocalLength, 1.0f, 300.0f, "%.1f mm");
	ImGui::Checkbox(T("feature.post_processing.do_f.console_selection", "Console Selection"), &settings.consoleSelection);
	if (settings.consoleSelection && currentRef != 0) {
		ImGui::Text(T("feature.post_processing.do_f.selected_reference", "Selected Reference: %08X"), currentRef);
	}

	if (ImGui::CollapsingHeader(T("feature.post_processing.do_f.debug", "Debug"))) {
		static float debugRescale = .3f;
		ImGui::Text(T("feature.post_processing.do_f.debug_distance", "Debug Distance: %f"), debugDistance);
		ImGui::Text(T("feature.post_processing.do_f.debug_focus_plane", "Debug Focus Plane: %f"), debugFocusPlane);
		ImGui::SliderFloat(T("feature.post_processing.do_f.view_resize", "View Resize"), &debugRescale, 0.f, 1.f);

		BUFFER_VIEWER_NODE(texFocus, 64.0f)
		BUFFER_VIEWER_NODE(texPreFocus, 64.0f)

		BUFFER_VIEWER_NODE(texCoC, debugRescale)
		BUFFER_VIEWER_NODE(texCoCHalf, debugRescale)
		BUFFER_VIEWER_NODE(texCoCTile, debugRescale)
		BUFFER_VIEWER_NODE(texCoCTileTmp, debugRescale)
		BUFFER_VIEWER_NODE(texCoCTileDilated, debugRescale)
		BUFFER_VIEWER_NODE(texCoCBlur1, debugRescale)
		BUFFER_VIEWER_NODE(texCoCBlur2, debugRescale)

		BUFFER_VIEWER_NODE(texPreBlurred, debugRescale)
		BUFFER_VIEWER_NODE(texFarBlurred, debugRescale)
		BUFFER_VIEWER_NODE(texNearBlurred, debugRescale)

		BUFFER_VIEWER_NODE(texBlurredFiltered, debugRescale)
		BUFFER_VIEWER_NODE(texPostSmooth, debugRescale)
		BUFFER_VIEWER_NODE(texPostSmooth2, debugRescale)
	}
}

void DoF::RestoreDefaultSettings()
{
	settings = {};
}

void DoF::LoadSettings(json& o_json)
{
	settings = o_json;
}

void DoF::SaveSettings(json& o_json)
{
	o_json = settings;
}

void DoF::SetupResources()
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	logger::debug("Creating buffers...");
	{
		dofCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<DoFCB>());
	}

	logger::debug("Creating 2D textures...");
	{
		auto gameTexMainCopy = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_COPY];

		D3D11_TEXTURE2D_DESC texDesc;
		gameTexMainCopy.texture->GetDesc(&texDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 1;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		texDesc.MiscFlags = 0;

		texOutput = eastl::make_unique<Texture2D>(texDesc);
		texOutput->CreateSRV(srvDesc);
		texOutput->CreateUAV(uavDesc);

		texPostSmooth = eastl::make_unique<Texture2D>(texDesc);
		texPostSmooth->CreateSRV(srvDesc);
		texPostSmooth->CreateUAV(uavDesc);

		texPostSmooth2 = eastl::make_unique<Texture2D>(texDesc);
		texPostSmooth2->CreateSRV(srvDesc);
		texPostSmooth2->CreateUAV(uavDesc);

		D3D11_TEXTURE2D_DESC texDescHalf = texDesc;
		texDescHalf.Width /= 2;
		texDescHalf.Height /= 2;

		texPreBlurred = eastl::make_unique<Texture2D>(texDescHalf);
		texPreBlurred->CreateSRV(srvDesc);
		texPreBlurred->CreateUAV(uavDesc);

		texFarBlurred = eastl::make_unique<Texture2D>(texDescHalf);
		texFarBlurred->CreateSRV(srvDesc);
		texFarBlurred->CreateUAV(uavDesc);

		texNearBlurred = eastl::make_unique<Texture2D>(texDescHalf);
		texNearBlurred->CreateSRV(srvDesc);
		texNearBlurred->CreateUAV(uavDesc);

		texBlurredFiltered = eastl::make_unique<Texture2D>(texDescHalf);
		texBlurredFiltered->CreateSRV(srvDesc);
		texBlurredFiltered->CreateUAV(uavDesc);

		// CoC buffers. R16_FLOAT is plenty: the CoC is a screen width fraction clamped to ~0.025, so
		// half float resolves it to better than 1/40th of a pixel.
		texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R16_FLOAT;
		texDescHalf.Format = DXGI_FORMAT_R16_FLOAT;

		texCoC = eastl::make_unique<Texture2D>(texDesc);
		texCoC->CreateSRV(srvDesc);
		texCoC->CreateUAV(uavDesc);

		texCoCHalf = eastl::make_unique<Texture2D>(texDescHalf);
		texCoCHalf->CreateSRV(srvDesc);
		texCoCHalf->CreateUAV(uavDesc);

		texCoCBlur1 = eastl::make_unique<Texture2D>(texDescHalf);
		texCoCBlur1->CreateSRV(srvDesc);
		texCoCBlur1->CreateUAV(uavDesc);

		texCoCBlur2 = eastl::make_unique<Texture2D>(texDescHalf);
		texCoCBlur2->CreateSRV(srvDesc);
		texCoCBlur2->CreateUAV(uavDesc);

		// CoC tile buffers: one texel per 16x16 full res pixels, holding (min, max) signed CoC.
		// RGBA16F rather than RG16F because only the RGBA/R/RG32 families are guaranteed to support
		// typed UAV stores on D3D11 feature level 11_0 hardware.
		D3D11_TEXTURE2D_DESC texDescTile = texDesc;
		texDescTile.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		texDescTile.Width = std::max(1u, (texDesc.Width / 2 + 7) / 8);
		texDescTile.Height = std::max(1u, (texDesc.Height / 2 + 7) / 8);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDescTile = srvDesc;
		srvDescTile.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDescTile = uavDesc;
		uavDescTile.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

		texCoCTile = eastl::make_unique<Texture2D>(texDescTile);
		texCoCTile->CreateSRV(srvDescTile);
		texCoCTile->CreateUAV(uavDescTile);

		texCoCTileTmp = eastl::make_unique<Texture2D>(texDescTile);
		texCoCTileTmp->CreateSRV(srvDescTile);
		texCoCTileTmp->CreateUAV(uavDescTile);

		texCoCTileDilated = eastl::make_unique<Texture2D>(texDescTile);
		texCoCTileDilated->CreateSRV(srvDescTile);
		texCoCTileDilated->CreateUAV(uavDescTile);

		// The 1x1 focus texture stores a distance in km and wants the full float range.
		texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
		texDesc.Width = 1;
		texDesc.Height = 1;

		texFocus = eastl::make_unique<Texture2D>(texDesc);
		texFocus->CreateSRV(srvDesc);
		texFocus->CreateUAV(uavDesc);

		texPreFocus = eastl::make_unique<Texture2D>(texDesc);
		texPreFocus->CreateSRV(srvDesc);
		texPreFocus->CreateUAV(uavDesc);

		g_TDM = reinterpret_cast<TDM_API::IVTDM2*>(TDM_API::RequestPluginAPI(TDM_API::InterfaceVersion::V2));
	}

	// Bokeh shapes are loaded by PostProcessing::bokehResources (shared with LensFlare)

	logger::debug("Creating samplers...");
	{
		D3D11_SAMPLER_DESC samplerDesc = {
			.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
			.AddressU = D3D11_TEXTURE_ADDRESS_MIRROR,
			.AddressV = D3D11_TEXTURE_ADDRESS_MIRROR,
			.AddressW = D3D11_TEXTURE_ADDRESS_MIRROR,
			.MaxAnisotropy = 1,
			.MinLOD = 0,
			.MaxLOD = D3D11_FLOAT32_MAX
		};
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, linearSampler.put()));
	}

	CompileComputeShaders();
}

void DoF::ClearShaderCache()
{
	const auto shaderPtrs = std::array{
		&UpdateFocusCS,
		&CalculateCoCCS,
		&CoCTileFlattenCS,
		&CoCTileDilateHCS,
		&CoCTileDilateVCS,
		&CoCGaussian1CS,
		&CoCGaussian2CS,
		&DownsampleCS,
		&FarBlurCS,
		&NearBlurCS,
		&TentFilterCS,
		&CombinerCS,
		&PostSmoothing1CS,
		&PostSmoothing2AndFocusingCS
	};

	for (auto shader : shaderPtrs)
		if ((*shader)) {
			(*shader)->Release();
			shader->detach();
		}

	CompileComputeShaders();
}

void DoF::CompileComputeShaders()
{
	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* programPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines;
		std::string entry = "main";
	};

	std::vector<ShaderCompileInfo>
		shaderInfos = {
			{ &UpdateFocusCS, "dof.cs.hlsl", {}, "CS_UpdateFocus" },
			{ &CalculateCoCCS, "dof.cs.hlsl", {}, "CS_CalculateCoC" },
			{ &CoCTileFlattenCS, "dof.cs.hlsl", {}, "CS_CoCTileFlatten" },
			{ &CoCTileDilateHCS, "dof.cs.hlsl", {}, "CS_CoCTileDilateH" },
			{ &CoCTileDilateVCS, "dof.cs.hlsl", {}, "CS_CoCTileDilateV" },
			{ &CoCGaussian1CS, "dof.cs.hlsl", {}, "CS_CoCGaussian1" },
			{ &CoCGaussian2CS, "dof.cs.hlsl", {}, "CS_CoCGaussian2" },
			{ &DownsampleCS, "dof.cs.hlsl", {}, "CS_Downsample" },
			{ &FarBlurCS, "dof.cs.hlsl", {}, "CS_FarBlur" },
			{ &NearBlurCS, "dof.cs.hlsl", {}, "CS_NearBlur" },
			{ &TentFilterCS, "dof.cs.hlsl", {}, "CS_TentFilter" },
			{ &CombinerCS, "dof.cs.hlsl", {}, "CS_Combiner" },
			{ &PostSmoothing1CS, "dof.cs.hlsl", {}, "CS_PostSmoothing1" },
			{ &PostSmoothing2AndFocusingCS, "dof.cs.hlsl", {}, "CS_PostSmoothing2AndFocusing" }
		};

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\PostProcessing\\DoF") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0", info.entry.c_str())))
			info.programPtr->attach(rawPtr);
	}
}

// Thanks Ershin!
RE::NiPoint3 DoF::GetCameraPos()
{
	auto player = RE::PlayerCharacter::GetSingleton();
	auto playerCamera = RE::PlayerCamera::GetSingleton();
	RE::NiPoint3 ret;

	if (playerCamera->currentState == playerCamera->GetRuntimeData().cameraStates[RE::CameraStates::kFirstPerson] ||
		playerCamera->currentState == playerCamera->GetRuntimeData().cameraStates[RE::CameraStates::kThirdPerson] ||
		playerCamera->currentState == playerCamera->GetRuntimeData().cameraStates[RE::CameraStates::kMount]) {
		RE::NiNode* root = playerCamera->cameraRoot.get();
		if (root) {
			ret.x = root->world.translate.x;
			ret.y = root->world.translate.y;
			ret.z = root->world.translate.z;
		}
	} else if (playerCamera->IsInFreeCameraMode()) {
		auto freeCameraState = static_cast<RE::FreeCameraState*>(playerCamera->currentState.get());
		ret = freeCameraState->translation;
	} else {
		RE::NiPoint3 playerPos = player->GetLookingAtLocation();

		ret.z = playerPos.z;
		ret.x = player->GetPositionX();
		ret.y = player->GetPositionY();
	}

	return ret;
}

bool DoF::GetTargetLockEnabled()
{
	return g_TDM && g_TDM->GetCurrentTarget();
}

bool DoF::GetInDialogue()
{
	return RE::MenuTopicManager::GetSingleton()->speaker || RE::MenuTopicManager::GetSingleton()->lastSpeaker;
}

float DoF::GetDistanceToReference(RE::TESObjectREFR* a_ref)
{
	RE::NiPoint3 cameraPosition = GetCameraPos();
	RE::NiPoint3 targetPosition = a_ref->GetPosition();
	if (a_ref->GetFormType() == RE::FormType::ActorCharacter && !a_ref->IsPlayer()) {
		auto head = a_ref->GetNodeByName("NPC Head [Head]");
		if (head) {
			targetPosition = head->world.translate;
		}
	}
	return cameraPosition.GetDistance(targetPosition);
}

void DoF::Draw(TextureInfo& inout_tex)
{
	auto state = globals::state;
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	auto* depthSRV = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].depthSRV;
	if (!depthSRV) {
		return;
	}

	float2 res = { (float)texOutput->desc.Width, (float)texOutput->desc.Height };

	float focusLen = settings.FocalLength;
	float nearBlur = settings.NearPlaneMaxBlur;
	float manualFocus = settings.ManualFocusPlane / 1000.0f;
	debugFocusPlane = manualFocus;
	bool autoFocus = settings.AutoFocus;

	if (settings.targetFocus) {
		focusLen = 1.0f;
		nearBlur = 0.0f;
		float targetFocusDistanceGame = 0;
		auto targetFocusEnabled = false;
		autoFocus = false;

		RE::TESObjectREFR* target = nullptr;
		const auto consoleRef = RE::Console::GetSelectedRef();
		if (settings.consoleSelection)
			if (consoleRef && !consoleRef->IsDisabled() && !consoleRef->IsDeleted() && consoleRef->Is3DLoaded()) {
				currentRef = consoleRef->formID;
				target = consoleRef.get();
				targetFocusEnabled = true;
			} else {
				currentRef = 0;
			}

		if (GetTargetLockEnabled()) {
			target = g_TDM->GetCurrentTarget().get().get();
			targetFocusEnabled = true;
		}

		if (GetInDialogue()) {
			if (RE::MenuTopicManager::GetSingleton()->speaker) {
				target = RE::MenuTopicManager::GetSingleton()->speaker.get().get();
			} else {
				target = RE::MenuTopicManager::GetSingleton()->lastSpeaker.get().get();
			}
			targetFocusEnabled = true;
		}
		if (target)
			targetFocusDistanceGame = GetDistanceToReference(target);
		debugDistance = targetFocusDistanceGame;
		if (targetFocusEnabled) {
			nearBlur = settings.NearPlaneMaxBlur;
			focusLen = settings.targetFocusFocalLength;
			manualFocus = Util::Units::GameUnitsToMeters(targetFocusDistanceGame) * 0.001f;  // in KM
		} else {
			return;
		}
	}
	debugFocusPlane = manualFocus;
	state->BeginPerfEvent("Depth of Field");

	const uint halfResX = std::max(1u, (uint)res.x / 2);
	const uint halfResY = std::max(1u, (uint)res.y / 2);
	const uint tileDimX = std::max(1u, (halfResX + 7) / 8);
	const uint tileDimY = std::max(1u, (halfResY + 7) / 8);

	// The near gather's kernel radius comes from the gaussian dilated near CoC, whose reach is
	// MaxNearCoCRadius * width * NearPlaneMaxBlur. The tile dilation has to cover at least that or
	// the group uniform early out would skip pixels the gaussian legitimately reaches, so the reach
	// is clamped back to whatever the (bounded) dilation radius can actually guarantee.
	const float wantNearRadiusPx = std::max(settings.MaxNearCoCRadius, 1e-4f) * res.x * std::max(nearBlur, 0.0f);
	const uint tileDilateRadius = std::min(48u, (uint)std::ceil(wantNearRadiusPx / 16.0f) + 1u);
	const float nearGaussianReachPx = std::min(wantNearRadiusPx, (float)(tileDilateRadius - 1u) * 16.0f);

	DoFCB dofData = {
		.TransitionSpeed = settings.TransitionSpeed,
		.FocusCoord = settings.FocusCoord,
		.ManualFocusPlane = manualFocus,
		.FocalLength = focusLen,
		.FNumber = settings.FNumber,
		.FarPlaneMaxBlur = settings.FarPlaneMaxBlur,
		.NearPlaneMaxBlur = nearBlur,
		.BlurQuality = settings.BlurQuality,
		.NearFarDistanceCompensation = settings.NearFarDistanceCompensation,
		.BokehBusyFactor = settings.BokehBusyFactor,
		.HighlightBoost = settings.HighlightBoost,
		.PostBlurSmoothing = settings.PostBlurSmoothing,
		.HighlightShape = (uint)settings.HighlightShape,
		.HighlightShapeRotationAngle = settings.HighlightShapeRotationAngle,
		.PetzvalStrength = settings.PetzvalStrength,
		.AutoFocus = autoFocus,
		.MaxNearCoCRadius = std::max(settings.MaxNearCoCRadius, 1e-4f),
		.MaxFarCoCRadius = std::max(settings.MaxFarCoCRadius, 1e-4f),
		.TileDilateRadius = tileDilateRadius,
		.CoCTileDimX = tileDimX,
		.CoCTileDimY = tileDimY,
		.HalfResDimX = halfResX,
		.HalfResDimY = halfResY,
		.NearGaussianReachPx = nearGaussianReachPx
	};
	dofCB->Update(dofData);

	std::array<ID3D11ShaderResourceView*, 12> srvs = {};
	std::array<ID3D11UnorderedAccessView*, 4> uavs = {};
	std::array<ID3D11SamplerState*, 1> samplers = { linearSampler.get() };
	auto cb = dofCB->CB();
	auto resetViews = [&]() {
		srvs.fill(nullptr);
		uavs.fill(nullptr);

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	};

	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
	uint dispatchWidth = ((uint)res.x + 7) >> 3;
	uint dispatchHeight = ((uint)res.y + 7) >> 3;
	uint dispatchWidthBlur = (halfResX + 7) >> 3;
	uint dispatchHeightBlur = (halfResY + 7) >> 3;
	uint dispatchWidthTile = (tileDimX + 7) >> 3;
	uint dispatchHeightTile = (tileDimY + 7) >> 3;

	// Update Focus
	{
		srvs.at(0) = inout_tex.srv;
		srvs.at(1) = texPreFocus->srv.get();
		srvs.at(2) = depthSRV;
		uavs.at(1) = texFocus->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

		context->CSSetShader(UpdateFocusCS.get(), nullptr, 0);
		context->Dispatch(1, 1, 1);
	}

	resetViews();
	context->CopyResource(texPreFocus->resource.get(), texFocus->resource.get());

	// Calculate CoC
	{
		globals::profiler->BeginPass("PostProcessing::DoF::CoC");
		state->BeginPerfEvent("Calculate CoC");
		srvs.at(0) = inout_tex.srv;
		srvs.at(1) = texPreFocus->srv.get();
		srvs.at(2) = depthSRV;
		uavs.at(2) = texCoC->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

		context->CSSetShader(CalculateCoCCS.get(), nullptr, 0);
		context->Dispatch(dispatchWidth, dispatchHeight, 1);
		state->EndPerfEvent();
		globals::profiler->EndPass();
	}

	resetViews();

	// Half res downsample of colour + CoC (bilateral). Everything the gather kernels read is half res
	// from here on.
	{
		globals::profiler->BeginPass("PostProcessing::DoF::Downsample");
		state->BeginPerfEvent("Downsample");
		srvs.at(0) = inout_tex.srv;
		srvs.at(3) = texCoC->srv.get();
		uavs.at(0) = texPreBlurred->uav.get();
		uavs.at(2) = texCoCHalf->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

		context->CSSetShader(DownsampleCS.get(), nullptr, 0);
		context->Dispatch(dispatchWidthBlur, dispatchHeightBlur, 1);

		resetViews();
		state->EndPerfEvent();
		globals::profiler->EndPass();
	}

	// CoC tile flatten + separable min/max dilation, at 1/16 of the full resolution.
	{
		globals::profiler->BeginPass("PostProcessing::DoF::CoCTile");
		state->BeginPerfEvent("CoC Tile");
		srvs.at(3) = texCoC->srv.get();
		uavs.at(3) = texCoCTile->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

		context->CSSetShader(CoCTileFlattenCS.get(), nullptr, 0);
		context->Dispatch(dispatchWidthTile, dispatchHeightTile, 1);

		resetViews();

		srvs.at(10) = texCoCTile->srv.get();
		uavs.at(3) = texCoCTileTmp->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

		context->CSSetShader(CoCTileDilateHCS.get(), nullptr, 0);
		context->Dispatch(dispatchWidthTile, dispatchHeightTile, 1);

		resetViews();

		srvs.at(10) = texCoCTileTmp->srv.get();
		uavs.at(3) = texCoCTileDilated->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

		context->CSSetShader(CoCTileDilateVCS.get(), nullptr, 0);
		context->Dispatch(dispatchWidthTile, dispatchHeightTile, 1);

		resetViews();
		state->EndPerfEvent();
		globals::profiler->EndPass();
	}

	// CoC Gaussian Blur: dilates the per tile near CoC into the smooth near field mask.
	{
		globals::profiler->BeginPass("PostProcessing::DoF::CoCBlur");
		state->BeginPerfEvent("CoC Gaussian Blur");
		srvs.at(10) = texCoCTile->srv.get();
		uavs.at(2) = texCoCBlur1->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

		context->CSSetShader(CoCGaussian1CS.get(), nullptr, 0);
		context->Dispatch(dispatchWidthBlur, dispatchHeightBlur, 1);

		resetViews();

		srvs.at(4) = texCoCBlur1->srv.get();
		uavs.at(2) = texCoCBlur2->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

		context->CSSetShader(CoCGaussian2CS.get(), nullptr, 0);
		context->Dispatch(dispatchWidthBlur, dispatchHeightBlur, 1);

		resetViews();
		state->EndPerfEvent();
		globals::profiler->EndPass();
	}

	// Gather
	{
		globals::profiler->BeginPass("PostProcessing::DoF::FarBlur");
		state->BeginPerfEvent("Far Blur");
		srvs.at(0) = texPreBlurred->srv.get();
		srvs.at(9) = texCoCHalf->srv.get();
		srvs.at(10) = texCoCTile->srv.get();
		if (owner)
			srvs.at(8) = owner->bokehResources.GetShapeSRV(std::clamp(settings.HighlightShape - 1, 0, BokehResources::NUM_BUILTIN_SHAPES - 1));
		uavs.at(0) = texFarBlurred->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

		context->CSSetShader(FarBlurCS.get(), nullptr, 0);
		context->Dispatch(dispatchWidthBlur, dispatchHeightBlur, 1);

		resetViews();
		state->EndPerfEvent();
		globals::profiler->EndPass();

		globals::profiler->BeginPass("PostProcessing::DoF::NearBlur");
		state->BeginPerfEvent("Near Blur");
		srvs.at(0) = texFarBlurred->srv.get();
		srvs.at(4) = texCoCBlur2->srv.get();
		srvs.at(9) = texCoCHalf->srv.get();
		srvs.at(11) = texCoCTileDilated->srv.get();
		if (owner)
			srvs.at(8) = owner->bokehResources.GetShapeSRV(std::clamp(settings.HighlightShape - 1, 0, BokehResources::NUM_BUILTIN_SHAPES - 1));
		uavs.at(0) = texNearBlurred->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

		context->CSSetShader(NearBlurCS.get(), nullptr, 0);
		context->Dispatch(dispatchWidthBlur, dispatchHeightBlur, 1);

		resetViews();
		state->EndPerfEvent();
		globals::profiler->EndPass();
	}

	// Tent Filter
	{
		globals::profiler->BeginPass("PostProcessing::DoF::TentFilter");
		state->BeginPerfEvent("Tent Filter");
		srvs.at(0) = texFarBlurred->srv.get();
		uavs.at(0) = texBlurredFiltered->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

		context->CSSetShader(TentFilterCS.get(), nullptr, 0);
		context->Dispatch(dispatchWidthBlur, dispatchHeightBlur, 1);

		resetViews();
		state->EndPerfEvent();
		globals::profiler->EndPass();
	}

	// Post Smoothing only touches out of focus highlights; when it's disabled the combiner can write
	// straight into the output and we save two full res passes.
	const bool doPostSmoothing = settings.PostBlurSmoothing >= 0.01f;

	// Combiner
	{
		globals::profiler->BeginPass("PostProcessing::DoF::Combiner");
		state->BeginPerfEvent("Combiner");
		srvs.at(0) = inout_tex.srv;
		srvs.at(3) = texCoC->srv.get();
		srvs.at(5) = texBlurredFiltered->srv.get();
		srvs.at(6) = texNearBlurred->srv.get();
		uavs.at(0) = doPostSmoothing ? texPostSmooth->uav.get() : texOutput->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

		context->CSSetShader(CombinerCS.get(), nullptr, 0);
		context->Dispatch(dispatchWidth, dispatchHeight, 1);

		resetViews();
		state->EndPerfEvent();
		globals::profiler->EndPass();
	}

	// Post Smooth
	if (doPostSmoothing) {
		globals::profiler->BeginPass("PostProcessing::DoF::PostSmooth");
		state->BeginPerfEvent("Post Smooth");
		srvs.at(0) = texPostSmooth->srv.get();
		srvs.at(3) = texCoC->srv.get();
		uavs.at(0) = texPostSmooth2->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

		context->CSSetShader(PostSmoothing1CS.get(), nullptr, 0);
		context->Dispatch(dispatchWidth, dispatchHeight, 1);

		resetViews();

		srvs.at(0) = texPostSmooth->srv.get();
		srvs.at(3) = texCoC->srv.get();
		srvs.at(7) = texPostSmooth2->srv.get();
		uavs.at(0) = texOutput->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

		context->CSSetShader(PostSmoothing2AndFocusingCS.get(), nullptr, 0);
		context->Dispatch(dispatchWidth, dispatchHeight, 1);

		resetViews();
		state->EndPerfEvent();
		globals::profiler->EndPass();
	}

	samplers.fill(nullptr);
	cb = nullptr;

	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
	context->CSSetShader(nullptr, nullptr, 0);

	inout_tex = { texOutput->resource.get(), texOutput->srv.get() };
	state->EndPerfEvent();
}
