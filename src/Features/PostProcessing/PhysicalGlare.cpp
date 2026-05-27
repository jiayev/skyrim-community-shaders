#include "PhysicalGlare.h"

#include "Features/LinearLighting.h"
#include "Globals.h"
#include "State.h"
#include "Util.h"

namespace
{
	constexpr auto kLensMode = "Lens (N-polygon)";
	constexpr auto kPupilMode = "Pupil (Circle)";
	constexpr auto kEyelashes = "Eyelashes";
	constexpr auto kPSFShaping = "PSF Shaping";
	constexpr auto kThreshold = "Threshold";
	constexpr auto kIntensity = "Intensity";
	constexpr auto kApertureMode = "Aperture Mode";
	constexpr auto kApertureBlades = "Aperture Blades";
	constexpr auto kFStop = "F-Stop";
	constexpr auto kSphericalAberration = "Spherical Aberration";
	constexpr auto kDustCount = "Dust Count";
	constexpr auto kDustSize = "Dust Size";
	constexpr auto kBladeRoughness = "Blade Roughness";
	constexpr auto kRoughnessFrequency = "Roughness Frequency";
	constexpr auto kScratchCount = "Scratch Count";
	constexpr auto kScratchOpacity = "Scratch Opacity";
	constexpr auto kScratchLength = "Scratch Length";
	constexpr auto kScratchWidth = "Scratch Width";
	constexpr auto kApertureRotation = "Aperture Rotation";
	constexpr auto kScatterStrength = "Scatter Strength";
	constexpr auto kParticleCount = "Particle Count";
	constexpr auto kParticleSize = "Particle Size";
	constexpr auto kGratingCount = "Grating Count";
	constexpr auto kGratingStrength = "Grating Strength";
	constexpr auto kTearFilmStrength = "Tear Film Strength";
	constexpr auto kTearFilmSpeed = "Tear Film Speed";
	constexpr auto kTearFilmComplexity = "Tear Film Complexity";
	constexpr auto kSutureBranches = "Suture Branches";
	constexpr auto kSutureStrength = "Suture Strength";
	constexpr auto kSutureWidth = "Suture Width";
	constexpr auto kStarburstSpikes = "Starburst Spikes";
	constexpr auto kStarburstStrength = "Starburst Strength";
	constexpr auto kStarburstIrregularity = "Starburst Irregularity";
	constexpr auto kEnableEyelashes = "Enable Eyelashes";
	constexpr auto kEyelashCount = "Eyelash Count";
	constexpr auto kEyelashLength = "Eyelash Length";
	constexpr auto kEyelashCurvature = "Eyelash Curvature";
	constexpr auto kAdaptSpeed = "Adapt Speed";
	constexpr auto kFFTResolution = "FFT Resolution";
	constexpr auto kPaddingRatio = "Padding Ratio";
	constexpr auto kKernelScale = "Kernel Scale";
	constexpr auto kFresnelExponent = "Fresnel Exponent";
	constexpr auto kChromaticSpread = "Chromatic Spread";
	constexpr auto kPSFSharpness = "PSF Sharpness";
	constexpr auto kPSFNoiseFloor = "PSF Noise Floor";

	template <class T>
	void ReadField(const json& j, const char* key, T& value)
	{
		value = j.value(key, value);
	}

	const json& GroupOrEmpty(const json& j, const char* key)
	{
		static const json empty = json::object();
		if (auto it = j.find(key); it != j.end() && it->is_object())
			return *it;
		return empty;
	}
}

void to_json(json& j, const PhysicalGlare::Settings& settings)
{
	j = {
		{ kThreshold, settings.ThresholdEV },
		{ kIntensity, settings.Intensity },
		{ kApertureMode, settings.ApertureMode },
		{ kApertureRotation, settings.ApertureRotation },
		{ kAdaptSpeed, settings.AdaptSpeed },
		{ kFFTResolution, settings.FFTResolution },
		{ kPaddingRatio, settings.PaddingRatio },
		{ kKernelScale, settings.KernelScale },
		{ kFresnelExponent, settings.FresnelExponent },
		{ kChromaticSpread, settings.ChromaticSpread },
		{ kLensMode, {
			{ kApertureBlades, settings.ApertureBlades },
			{ kFStop, settings.FStop },
			{ kSphericalAberration, settings.SphericalAberration },
			{ kDustCount, settings.DustCount },
			{ kDustSize, settings.DustSize },
			{ kBladeRoughness, settings.BladeRoughnessAmp },
			{ kRoughnessFrequency, settings.BladeRoughnessFreq },
			{ kScratchCount, settings.ScratchCount },
			{ kScratchOpacity, settings.ScratchOpacity },
			{ kScratchLength, settings.ScratchLength },
			{ kScratchWidth, settings.ScratchWidth } } },
		{ kPupilMode, {
			{ kScatterStrength, settings.ScatterStrength },
			{ kParticleCount, settings.ParticleCount },
			{ kParticleSize, settings.ParticleSize },
			{ kGratingCount, settings.GratingCount },
			{ kGratingStrength, settings.GratingStrength },
			{ kTearFilmStrength, settings.TearFilmStrength },
			{ kTearFilmSpeed, settings.TearFilmSpeed },
			{ kTearFilmComplexity, settings.TearFilmComplexity },
			{ kSutureBranches, settings.SutureBranches },
			{ kSutureStrength, settings.SutureStrength },
			{ kSutureWidth, settings.SutureWidth },
			{ kStarburstSpikes, settings.StarburstCount },
			{ kStarburstStrength, settings.StarburstStrength },
			{ kStarburstIrregularity, settings.StarburstIrregularity },
			{ kEyelashes, {
				{ kEnableEyelashes, settings.EnableEyelashes },
				{ kEyelashCount, settings.EyelashCount },
				{ kEyelashLength, settings.EyelashLength },
				{ kEyelashCurvature, settings.EyelashCurvature } } } } },
		{ kPSFShaping, {
			{ kPSFSharpness, settings.PSFSharpness },
			{ kPSFNoiseFloor, settings.PSFNoiseFloor } } }
	};
}

void from_json(const json& j, PhysicalGlare::Settings& settings)
{
	settings = {};
	ReadField(j, kThreshold, settings.ThresholdEV);
	ReadField(j, kIntensity, settings.Intensity);
	ReadField(j, kApertureMode, settings.ApertureMode);
	ReadField(j, kApertureRotation, settings.ApertureRotation);
	ReadField(j, kAdaptSpeed, settings.AdaptSpeed);
	ReadField(j, kFFTResolution, settings.FFTResolution);
	ReadField(j, kPaddingRatio, settings.PaddingRatio);
	ReadField(j, kKernelScale, settings.KernelScale);
	ReadField(j, kFresnelExponent, settings.FresnelExponent);
	ReadField(j, kChromaticSpread, settings.ChromaticSpread);

	const auto& lens = GroupOrEmpty(j, kLensMode);
	ReadField(lens, kApertureBlades, settings.ApertureBlades);
	ReadField(lens, kFStop, settings.FStop);
	ReadField(lens, kSphericalAberration, settings.SphericalAberration);
	ReadField(lens, kDustCount, settings.DustCount);
	ReadField(lens, kDustSize, settings.DustSize);
	ReadField(lens, kBladeRoughness, settings.BladeRoughnessAmp);
	ReadField(lens, kRoughnessFrequency, settings.BladeRoughnessFreq);
	ReadField(lens, kScratchCount, settings.ScratchCount);
	ReadField(lens, kScratchOpacity, settings.ScratchOpacity);
	ReadField(lens, kScratchLength, settings.ScratchLength);
	ReadField(lens, kScratchWidth, settings.ScratchWidth);

	const auto& pupil = GroupOrEmpty(j, kPupilMode);
	ReadField(pupil, kScatterStrength, settings.ScatterStrength);
	ReadField(pupil, kParticleCount, settings.ParticleCount);
	ReadField(pupil, kParticleSize, settings.ParticleSize);
	ReadField(pupil, kGratingCount, settings.GratingCount);
	ReadField(pupil, kGratingStrength, settings.GratingStrength);
	ReadField(pupil, kTearFilmStrength, settings.TearFilmStrength);
	ReadField(pupil, kTearFilmSpeed, settings.TearFilmSpeed);
	ReadField(pupil, kTearFilmComplexity, settings.TearFilmComplexity);
	ReadField(pupil, kSutureBranches, settings.SutureBranches);
	ReadField(pupil, kSutureStrength, settings.SutureStrength);
	ReadField(pupil, kSutureWidth, settings.SutureWidth);
	ReadField(pupil, kStarburstSpikes, settings.StarburstCount);
	ReadField(pupil, kStarburstStrength, settings.StarburstStrength);
	ReadField(pupil, kStarburstIrregularity, settings.StarburstIrregularity);

	const auto& eyelashes = GroupOrEmpty(pupil, kEyelashes);
	ReadField(eyelashes, kEnableEyelashes, settings.EnableEyelashes);
	ReadField(eyelashes, kEyelashCount, settings.EyelashCount);
	ReadField(eyelashes, kEyelashLength, settings.EyelashLength);
	ReadField(eyelashes, kEyelashCurvature, settings.EyelashCurvature);

	const auto& psf = GroupOrEmpty(j, kPSFShaping);
	ReadField(psf, kPSFSharpness, settings.PSFSharpness);
	ReadField(psf, kPSFNoiseFloor, settings.PSFNoiseFloor);
}

void PhysicalGlare::DrawSettings()
{
	auto tooltip = [](const char* text) {
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(text);
	};

	ImGui::SliderFloat("Threshold", &settings.ThresholdEV, -10.f, 20.f, "%+.2f EV");
	tooltip("Per-channel brightness threshold for glare extraction in EV (0 EV = 1.0 linear).");

	ImGui::SliderFloat("Intensity", &settings.Intensity, 0.f, 2.f, "%.2f");
	tooltip("Overall glare intensity.");

	{
		const char* modeNames[] = { kLensMode, kPupilMode };
		ImGui::Combo("Aperture Mode", &settings.ApertureMode, modeNames, IM_ARRAYSIZE(modeNames));
		tooltip("Lens: camera lens polygon starburst. Pupil: circular human eye aperture.");
	}

	ImGui::SliderFloat("Aperture Rotation", &settings.ApertureRotation, -180.f, 180.f, "%.1f deg");
	tooltip("Rotation angle of the aperture.");

	ImGui::SliderFloat("Adapt Speed", &settings.AdaptSpeed, 0.5f, 10.f, "%.1f");
	tooltip("How fast the glare adapts to brightness changes.");

	Util::FFTResolutionCombo("FFT Resolution", settings.FFTResolution);
	tooltip("Resolution of the FFT convolution. Higher = sharper starburst but more expensive.");

	ImGui::SliderFloat("Padding Ratio", &settings.PaddingRatio, 0.f, 0.25f, "%.3f");
	tooltip(
		"Zero-padding per side to prevent FFT wrap-around.\n"
		"0.25 = paper default (50% effective resolution).\n"
		"0.1  = 80% effective (recommended for high-res).\n"
		"0.0  = 100% (maximum sharpness, may wrap at edges).\n"
		"Lower = sharper glare on high-res screens.");

	ImGui::SliderFloat("Kernel Scale", &settings.KernelScale, 0.01f, 1.f, "%.2f");
	tooltip(
		"Scale of the glare kernel size on screen.\n"
		"1.0 = default. Smaller = more concentrated glare.\n"
		"Does not affect aperture physics.");

	ImGui::SliderFloat("Fresnel Exponent", &settings.FresnelExponent, 0.f, 80.f, "%.1f");
	tooltip("Fresnel phase at aperture edge (radians). Paper eq 2.12: e^(i*pi/(lambda*z) * r^2).\nHigher = more Fresnel rings. 0 = pure Fraunhofer (no rings).");

	ImGui::SliderFloat("Chromatic Spread", &settings.ChromaticSpread, 0.f, 3.f, "%.2f");
	tooltip("Multiplier on wavelength-dependent UV scaling (paper section 2.3: lambda/575nm).\n1.0 = physically correct. Higher = more rainbow spread. 0 = monochrome.");

	if (settings.ApertureMode == 0) {
		ImGui::SeparatorText(kLensMode);

		ImGui::SliderInt("Aperture Blades", &settings.ApertureBlades, 3, 10);
		tooltip("Number of aperture blades. Controls starburst pattern.");

		ImGui::SliderFloat("F-Stop", &settings.FStop, 1.0f, 22.0f, "F%.1f");
		tooltip("Aperture f-number (e.g. F2.8). Smaller = larger aperture = wider diffraction spikes.\nPhysically: aperture radius = 1 / f-number.");

		ImGui::SliderFloat("Spherical Aberration", &settings.SphericalAberration, 0.f, 100.f, "%.1f");
		tooltip(
			"Seidel spherical aberration (r^4 wavefront error).\n"
			"Models lens curvature: outer rays focus at a different point\n"
			"than central rays, producing concentric ring structure in the\n"
			"PSF and softer glare edges. Physical range: 0-50.");

		ImGui::SliderInt("Dust Count", &settings.DustCount, 0, 500);
		tooltip("Dust particles on lens element surfaces.\nProduces scattered haze via Babinet's principle.");

		if (settings.DustCount > 0) {
			ImGui::SliderFloat("Dust Size", &settings.DustSize, 0.5f, 5.f, "%.1f");
			tooltip("Radius of each dust particle in pixels.");
		}

		ImGui::SliderFloat("Blade Roughness", &settings.BladeRoughnessAmp, 0.f, 2.f, "%.2f");
		tooltip("Micro-serrations on aperture blade edges (manufacturing imperfections).\nMakes star spikes slightly fuzzy/irregular. 0 = perfect edges.");

		if (settings.BladeRoughnessAmp > 0.f) {
			ImGui::SliderInt("Roughness Frequency", &settings.BladeRoughnessFreq, 5, 100);
			tooltip("Number of bumps per blade edge. Higher = finer serrations.");
		}

		ImGui::SliderInt("Scratch Count", &settings.ScratchCount, 0, 20);
		tooltip("Linear scratches on lens element surfaces.\nEach scratch produces a perpendicular streak in the glare.");

		if (settings.ScratchCount > 0) {
			ImGui::SliderFloat("Scratch Opacity", &settings.ScratchOpacity, 0.f, 1.f, "%.2f");
			tooltip("How opaque each scratch is. Higher = more visible streaks.");

			ImGui::SliderFloat("Scratch Length", &settings.ScratchLength, 0.2f, 1.5f, "%.2f");
			tooltip("Length of scratches relative to aperture size.");

			ImGui::SliderFloat("Scratch Width", &settings.ScratchWidth, 0.5f, 4.f, "%.1f");
			tooltip("Pixel width of each scratch.");
		}
	}

	if (settings.ApertureMode == 1) {
		ImGui::SeparatorText(kPupilMode);

		ImGui::SliderFloat("Scatter Strength", &settings.ScatterStrength, 0.f, 1.f, "%.2f");
		tooltip("Opacity of scatter particles in pupil mode (paper section 2.4).\n0 = transparent (no scatter), 1 = fully opaque.");

		ImGui::SliderInt("Particle Count", &settings.ParticleCount, 0, 1000);
		tooltip("Number of scatter particles in lens/vitreous (Ritschel: 750).\nProduces ciliary corona needle pattern via Babinet's principle.");

		ImGui::SliderFloat("Particle Size", &settings.ParticleSize, 0.5f, 5.f, "%.1f");
		tooltip("Radius of each particle in pixels.");

		ImGui::SliderInt("Grating Count", &settings.GratingCount, 0, 400);
		tooltip("Number of radial lens gratings (paper section 2.4: Ritschel uses 200).\nProduces lenticular halo via edge diffraction.");

		if (settings.GratingCount > 0) {
			ImGui::SliderFloat("Grating Strength", &settings.GratingStrength, 0.f, 1.f, "%.2f");
			tooltip("Opacity of lens gratings. Higher = stronger lenticular halo.");
		}

		ImGui::SliderFloat("Tear Film Strength", &settings.TearFilmStrength, 0.f, 1.f, "%.2f");
		tooltip("Simulates tear film irregularities on the cornea surface.\nProduces flickering, sharp, irregular star spikes.\n0 = disabled (static PSF).");

		if (settings.TearFilmStrength > 0.f) {
			ImGui::SliderFloat("Tear Film Speed", &settings.TearFilmSpeed, 0.1f, 8.f, "%.1f");
			tooltip("How fast the tear film fluctuates (blink refresh rate ~0.3Hz, breakup ~2-5Hz).");

			ImGui::SliderInt("Tear Film Complexity", &settings.TearFilmComplexity, 3, 16);
			tooltip("Number of angular harmonics. More = more spikes, finer detail.");
		}

		ImGui::SliderInt("Suture Branches", &settings.SutureBranches, 0, 8);
		tooltip(
			"Lens suture lines: Y-shaped junctions where lens fiber cells meet.\n"
			"3 = young eye (anterior Y + posterior inverted Y = 6 spikes).\n"
			"More branches = older/more complex lens. 0 = disabled.");

		if (settings.SutureBranches > 0) {
			ImGui::SliderFloat("Suture Strength", &settings.SutureStrength, 0.f, 1.f, "%.2f");
			tooltip("Opacity of suture lines. Higher = stronger star spikes.");

			ImGui::SliderFloat("Suture Width", &settings.SutureWidth, 0.5f, 5.f, "%.1f");
			tooltip("Pixel width of each suture line. Thinner = sharper spikes.");
		}

		ImGui::SliderInt("Starburst Spikes", &settings.StarburstCount, 0, 128);
		tooltip(
			"Lens fiber radial phase grating.\nCreates many thin, sharp radial star spikes.\n"
			"Higher count = more spikes (typical human eye: 20-80). 0 = disabled.");

		if (settings.StarburstCount > 0) {
			ImGui::SliderFloat("Starburst Strength", &settings.StarburstStrength, 0.f, 2.f, "%.2f");
			tooltip("Phase shift strength per fiber. Higher = brighter spikes.");

			ImGui::SliderFloat("Starburst Irregularity", &settings.StarburstIrregularity, 0.f, 1.f, "%.2f");
			tooltip(
				"Random variation in fiber spacing and strength.\n"
				"0 = perfectly regular (even spikes).\n"
				"1 = maximally irregular (natural look).");
		}

		if (ImGui::CollapsingHeader(kEyelashes)) {
			ImGui::Checkbox("Enable Eyelashes", &settings.EnableEyelashes);
			tooltip("Simulate eyelash occlusion for streak effects (paper section 3.1).");

			if (settings.EnableEyelashes) {
				ImGui::SliderInt("Eyelash Count", &settings.EyelashCount, 5, 80);
				tooltip("Total number of eyelash hairs (upper + lower).");

				ImGui::SliderFloat("Eyelash Length", &settings.EyelashLength, 0.1f, 0.8f, "%.2f");
				tooltip("Length of eyelashes relative to aperture radius.");

				ImGui::SliderFloat("Eyelash Curvature", &settings.EyelashCurvature, 0.f, 1.f, "%.2f");
				tooltip("Streak curvature via UV bending (paper fig 3.7: sin(x) vertical offset).");
			}
		}
	}

	ImGui::SeparatorText(kPSFShaping);

	ImGui::SliderFloat("PSF Sharpness", &settings.PSFSharpness, 0.2f, 1.f, "%.2f");
	tooltip(
		"Dynamic range compression exponent (paper Table 3.9: 0.45).\n"
		"Lower = wider/softer glare, higher = concentrated near light source.\n"
		"Increase if glare looks too blurry/spreads too far.");

	ImGui::SliderFloat("PSF Noise Floor", &settings.PSFNoiseFloor, 0.f, 0.01f, "%.4f");
	tooltip(
		"Threshold to remove low-level FFT noise from the PSF.\n"
		"Paper default: 0.001. Higher = cleaner glare wings.");

	if (ImGui::CollapsingHeader("Debug")) {
		if (texGlareResult) {
			constexpr float kDebugPreviewSize = 256.f;
			const float previewSize = kDebugPreviewSize * Util::GetUIScale();
			ImGui::Image(texGlareResult->srv.get(), { previewSize, previewSize });
		}
	}
}

void PhysicalGlare::RestoreDefaultSettings()
{
	settings = {};
}

void PhysicalGlare::LoadSettings(json& o_json)
{
	settings = o_json;
}

void PhysicalGlare::SaveSettings(json& o_json)
{
	o_json = settings;
}

void PhysicalGlare::CreateFFTTextures(uint resolution)
{
	currentFFTResolution = resolution;
	psfDirty = true;

	D3D11_TEXTURE2D_DESC texDesc = {
		.Width = resolution,
		.Height = resolution,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = DXGI_FORMAT_R32G32_FLOAT,
		.SampleDesc = { .Count = 1, .Quality = 0 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
	};

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

	// FFT ping-pong textures (RG32F) for 3 channels
	for (int ch = 0; ch < 3; ch++) {
		for (int pp = 0; pp < 2; pp++) {
			texFFT[ch][pp] = eastl::make_unique<Texture2D>(texDesc);
			texFFT[ch][pp]->CreateSRV(srvDesc);
			texFFT[ch][pp]->CreateUAV(uavDesc);
		}
	}

	// PSF FFT cache (RG32F) for 3 channels
	for (int ch = 0; ch < 3; ch++) {
		texPSF_FFT[ch] = eastl::make_unique<Texture2D>(texDesc);
		texPSF_FFT[ch]->CreateSRV(srvDesc);
		texPSF_FFT[ch]->CreateUAV(uavDesc);
	}

	// Glare result and history (RGBA16F, FFT resolution)
	D3D11_TEXTURE2D_DESC glareDesc = texDesc;
	glareDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	srvDesc.Format = glareDesc.Format;
	uavDesc.Format = glareDesc.Format;

	texGlareResult = eastl::make_unique<Texture2D>(glareDesc);
	texGlareResult->CreateSRV(srvDesc);
	texGlareResult->CreateUAV(uavDesc);

	texGlarePrev = eastl::make_unique<Texture2D>(glareDesc);
	texGlarePrev->CreateSRV(srvDesc);
	texGlarePrev->CreateUAV(uavDesc);

	// Clear glare history to zero — D3D11 USAGE_DEFAULT textures have undefined content
	// which may contain NaN/Inf, poisoning the temporal blend permanently
	auto context = globals::d3d::context;
	const FLOAT clearColor[4] = { 0.f, 0.f, 0.f, 0.f };
	context->ClearUnorderedAccessViewFloat(texGlareResult->uav.get(), clearColor);
	context->ClearUnorderedAccessViewFloat(texGlarePrev->uav.get(), clearColor);
}

void PhysicalGlare::SetupResources()
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	logger::debug("PhysicalGlare: Creating buffers...");
	{
		glareCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<GlareCB>());
	}

	logger::debug("PhysicalGlare: Creating FFT textures...");
	{
		currentFFTResolution = std::clamp((uint)settings.FFTResolution, FFT_MIN, FFT_MAX);
		CreateFFTTextures(currentFFTResolution);
	}

	logger::debug("PhysicalGlare: Creating output texture...");
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

		texDesc.MipLevels = 1;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		texDesc.MiscFlags = 0;

		texOutput = eastl::make_unique<Texture2D>(texDesc);
		texOutput->CreateSRV(srvDesc);
		texOutput->CreateUAV(uavDesc);
	}

	logger::debug("PhysicalGlare: Creating samplers...");
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
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, linearSampler.put()));

		D3D11_SAMPLER_DESC wrapSamplerDesc = {
			.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
			.AddressU = D3D11_TEXTURE_ADDRESS_WRAP,
			.AddressV = D3D11_TEXTURE_ADDRESS_WRAP,
			.AddressW = D3D11_TEXTURE_ADDRESS_WRAP,
			.MaxAnisotropy = 1,
			.MinLOD = 0,
			.MaxLOD = D3D11_FLOAT32_MAX
		};
		DX::ThrowIfFailed(device->CreateSamplerState(&wrapSamplerDesc, wrapSampler.put()));
	}

	CompileComputeShaders();
}

void PhysicalGlare::ClearShaderCache()
{
	auto const shaderPtrs = std::array{
		&thresholdCS, &apertureCS, &psfCS, &fftRowCS, &fftColCS, &fftRowInvCS, &fftColInvCS, &multiplyCS, &compositeCS
	};

	Util::ResetComPtrs(shaderPtrs);

	CompileComputeShaders();
}

void PhysicalGlare::CompileComputeShaders()
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
			{ &thresholdCS, "threshold.cs.hlsl", {}, "CS_Threshold" },
			{ &apertureCS, "aperture.cs.hlsl", {}, "CS_Aperture" },
			{ &psfCS, "psf.cs.hlsl", {}, "CS_ChromaticBlur" },
			{ &fftRowCS, "fft.cs.hlsl", { { "ROW_PASS", "" }, { "FORWARD", "" } }, "CS_FFT" },
			{ &fftColCS, "fft.cs.hlsl", { { "COL_PASS", "" }, { "FORWARD", "" } }, "CS_FFT" },
			{ &fftRowInvCS, "fft.cs.hlsl", { { "ROW_PASS", "" }, { "INVERSE", "" } }, "CS_FFT" },
			{ &fftColInvCS, "fft.cs.hlsl", { { "COL_PASS", "" }, { "INVERSE", "" } }, "CS_FFT" },
			{ &multiplyCS, "multiply.cs.hlsl", {}, "CS_Multiply" },
			{ &compositeCS, "composite.cs.hlsl", {}, "CS_Composite" },
		};

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\PostProcessing\\PhysicalGlare") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0", info.entry.c_str())))
			info.programPtr->attach(rawPtr);
	}
}

bool PhysicalGlare::NeedsPSFRegeneration() const
{
	return psfDirty ||
	       cachedPSFParams.ApertureMode != settings.ApertureMode ||
	       cachedPSFParams.ApertureBlades != settings.ApertureBlades ||
	       cachedPSFParams.ApertureRotation != settings.ApertureRotation ||
	       cachedPSFParams.ScatterStrength != settings.ScatterStrength ||
	       cachedPSFParams.FFTResolution != settings.FFTResolution ||
	       cachedPSFParams.EnableEyelashes != settings.EnableEyelashes ||
	       cachedPSFParams.EyelashCount != settings.EyelashCount ||
	       cachedPSFParams.EyelashLength != settings.EyelashLength ||
	       cachedPSFParams.EyelashCurvature != settings.EyelashCurvature ||
	       cachedPSFParams.FresnelExponent != settings.FresnelExponent ||
	       cachedPSFParams.ChromaticSpread != settings.ChromaticSpread ||
	       cachedPSFParams.FStop != settings.FStop ||
	       cachedPSFParams.PSFSharpness != settings.PSFSharpness ||
	       cachedPSFParams.PSFNoiseFloor != settings.PSFNoiseFloor ||
	       cachedPSFParams.ParticleCount != settings.ParticleCount ||
	       cachedPSFParams.ParticleSize != settings.ParticleSize ||
	       cachedPSFParams.GratingCount != settings.GratingCount ||
	       cachedPSFParams.GratingStrength != settings.GratingStrength ||
	       cachedPSFParams.TearFilmStrength != settings.TearFilmStrength ||
	       cachedPSFParams.TearFilmSpeed != settings.TearFilmSpeed ||
	       cachedPSFParams.TearFilmComplexity != settings.TearFilmComplexity ||
	       cachedPSFParams.SutureBranches != settings.SutureBranches ||
	       cachedPSFParams.SutureStrength != settings.SutureStrength ||
	       cachedPSFParams.SutureWidth != settings.SutureWidth ||
	       cachedPSFParams.StarburstCount != settings.StarburstCount ||
	       cachedPSFParams.StarburstStrength != settings.StarburstStrength ||
	       cachedPSFParams.StarburstIrregularity != settings.StarburstIrregularity ||
	       cachedPSFParams.DustCount != settings.DustCount ||
	       cachedPSFParams.DustSize != settings.DustSize ||
	       cachedPSFParams.BladeRoughnessFreq != settings.BladeRoughnessFreq ||
	       cachedPSFParams.BladeRoughnessAmp != settings.BladeRoughnessAmp ||
	       cachedPSFParams.ScratchCount != settings.ScratchCount ||
	       cachedPSFParams.ScratchOpacity != settings.ScratchOpacity ||
	       cachedPSFParams.ScratchLength != settings.ScratchLength ||
	       cachedPSFParams.ScratchWidth != settings.ScratchWidth ||
	       cachedPSFParams.SphericalAberration != settings.SphericalAberration ||
	       cachedPSFParams.KernelScale != settings.KernelScale ||
	       cachedPSFParams.UseAP1 != (globals::features::linearLighting.settings.enableACEScg && globals::features::linearLighting.settings.enableLinearLighting) ||
	       settings.TearFilmStrength > 0.f;  // force per-frame regen when active
}

void PhysicalGlare::GeneratePSF()
{
	auto context = globals::d3d::context;

	// Build the CB data for PSF generation
	GlareCB cbData = {
		.Threshold = exp2(settings.ThresholdEV),
		.Intensity = settings.Intensity,
		.ScatterStrength = settings.ScatterStrength,
		.ApertureMode = (uint)settings.ApertureMode,
		.ApertureBlades = settings.ApertureBlades,
		.ApertureRotation = settings.ApertureRotation * 3.14159265f / 180.f,
		.AdaptSpeed = settings.AdaptSpeed,
		.DeltaTime = 0.f,
		.FFTResolution = currentFFTResolution,
		.PaddingRatio = settings.PaddingRatio,
		.ScreenWidth = texOutput ? (float)texOutput->desc.Width : 1920.f,
		.ScreenHeight = texOutput ? (float)texOutput->desc.Height : 1080.f,
		.ChannelIndex = 0,
		.FresnelExponent = settings.FresnelExponent,
		.ChromaticSpread = settings.ChromaticSpread,
		.ApertureSize = 1.0f / std::max(settings.FStop, 1.0f),
		.PSFSharpness = settings.PSFSharpness,
		.PSFNoiseFloor = settings.PSFNoiseFloor,
		.EnableEyelashes = settings.EnableEyelashes ? 1u : 0u,
		.EyelashCurvature = settings.EyelashCurvature,
		// Eye mode
		.EyelashCount = (uint)settings.EyelashCount,
		.EyelashLength = settings.EyelashLength,
		.ParticleCount = (uint)settings.ParticleCount,
		.ParticleSize = settings.ParticleSize,
		.GratingCount = (uint)settings.GratingCount,
		.GratingStrength = settings.GratingStrength,
		.TearFilmStrength = settings.TearFilmStrength,
		.TearFilmSpeed = settings.TearFilmSpeed,
		.TearFilmComplexity = (uint)settings.TearFilmComplexity,
		.TearFilmTime = tearFilmTimeAccum,
		.SutureBranches = (uint)settings.SutureBranches,
		.SutureStrength = settings.SutureStrength,
		.SutureWidth = settings.SutureWidth,
		.StarburstCount = (uint)settings.StarburstCount,
		.StarburstStrength = settings.StarburstStrength,
		.StarburstIrregularity = settings.StarburstIrregularity,
		// Lens mode
		.DustCount = (uint)settings.DustCount,
		.DustSize = settings.DustSize,
		.BladeRoughnessFreq = (uint)settings.BladeRoughnessFreq,
		.BladeRoughnessAmp = settings.BladeRoughnessAmp,
		.ScratchCount = (uint)settings.ScratchCount,
		.ScratchOpacity = settings.ScratchOpacity,
		.ScratchLength = settings.ScratchLength,
		.ScratchWidth = settings.ScratchWidth,
		.SphericalAberration = settings.SphericalAberration,
		.UseAP1 = (globals::features::linearLighting.settings.enableACEScg && globals::features::linearLighting.settings.enableLinearLighting) ? 1u : 0u,
		.KernelScale = settings.KernelScale,
	};

	glareCB->Update(cbData);
	ID3D11Buffer* cb = glareCB->CB();
	context->CSSetConstantBuffers(1, 1, &cb);

	// ===== Step 1: Render aperture polygon =====
	// Output: texFFT[0][0] (real = aperture value, imag = 0)
	{
		ID3D11UnorderedAccessView* uav = texFFT[0][0]->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(apertureCS.get(), nullptr, 0);
		context->Dispatch((currentFFTResolution + 7) >> 3, (currentFFTResolution + 7) >> 3, 1);

		uav = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	}

	// ===== Step 2: FFT aperture (Fraunhofer diffraction) =====
	// texFFT[0][0] -> row FFT -> texFFT[0][1] -> col FFT -> texFFT[0][0]
	// Now texFFT[0][0] holds the complex diffraction amplitude F(u,v)
	DispatchFFT(fftRowCS.get(), texFFT[0][0].get(), texFFT[0][1].get(), currentFFTResolution);
	DispatchFFT(fftColCS.get(), texFFT[0][1].get(), texFFT[0][0].get(), currentFFTResolution);

	// ===== Step 3: Chromatic blur per RGB channel =====
	// Reads texFFT[0][0] (diffraction amplitude, t0), writes texFFT[ch][1] (u0)
	// Computes |F|² at wavelength-dependent UV scales with CIE spectral weighting
	{
		ID3D11SamplerState* sampler = wrapSampler.get();
		context->CSSetSamplers(0, 1, &sampler);

		for (int ch = 0; ch < 3; ch++) {
			cbData.ChannelIndex = (uint)ch;
			glareCB->Update(cbData);
			cb = glareCB->CB();
			context->CSSetConstantBuffers(1, 1, &cb);

			ID3D11ShaderResourceView* srv = texFFT[0][0]->srv.get();
			ID3D11UnorderedAccessView* uav = texFFT[ch][1]->uav.get();

			context->CSSetShaderResources(0, 1, &srv);
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
			context->CSSetShader(psfCS.get(), nullptr, 0);
			context->Dispatch((currentFFTResolution + 7) >> 3, (currentFFTResolution + 7) >> 3, 1);

			srv = nullptr;
			uav = nullptr;
			context->CSSetShaderResources(0, 1, &srv);
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		}

		sampler = nullptr;
		context->CSSetSamplers(0, 1, &sampler);
	}

	// ===== Step 4: FFT each channel's PSF for frequency-domain storage =====
	// texFFT[ch][1] -> row FFT -> texFFT[ch][0] -> col FFT -> texPSF_FFT[ch]
	for (int ch = 0; ch < 3; ch++) {
		DispatchFFT(fftRowCS.get(), texFFT[ch][1].get(), texFFT[ch][0].get(), currentFFTResolution);
		DispatchFFT(fftColCS.get(), texFFT[ch][0].get(), texPSF_FFT[ch].get(), currentFFTResolution);
	}

	// Cache parameters
	cachedPSFParams.ApertureMode = settings.ApertureMode;
	cachedPSFParams.ApertureBlades = settings.ApertureBlades;
	cachedPSFParams.ApertureRotation = settings.ApertureRotation;
	cachedPSFParams.ScatterStrength = settings.ScatterStrength;
	cachedPSFParams.FFTResolution = settings.FFTResolution;
	cachedPSFParams.EnableEyelashes = settings.EnableEyelashes;
	cachedPSFParams.EyelashCount = settings.EyelashCount;
	cachedPSFParams.EyelashLength = settings.EyelashLength;
	cachedPSFParams.EyelashCurvature = settings.EyelashCurvature;
	cachedPSFParams.FresnelExponent = settings.FresnelExponent;
	cachedPSFParams.ChromaticSpread = settings.ChromaticSpread;
	cachedPSFParams.FStop = settings.FStop;
	cachedPSFParams.ParticleCount = settings.ParticleCount;
	cachedPSFParams.ParticleSize = settings.ParticleSize;
	cachedPSFParams.GratingCount = settings.GratingCount;
	cachedPSFParams.GratingStrength = settings.GratingStrength;
	cachedPSFParams.TearFilmStrength = settings.TearFilmStrength;
	cachedPSFParams.TearFilmSpeed = settings.TearFilmSpeed;
	cachedPSFParams.TearFilmComplexity = settings.TearFilmComplexity;
	cachedPSFParams.SutureBranches = settings.SutureBranches;
	cachedPSFParams.SutureStrength = settings.SutureStrength;
	cachedPSFParams.SutureWidth = settings.SutureWidth;
	cachedPSFParams.StarburstCount = settings.StarburstCount;
	cachedPSFParams.StarburstStrength = settings.StarburstStrength;
	cachedPSFParams.StarburstIrregularity = settings.StarburstIrregularity;
	cachedPSFParams.DustCount = settings.DustCount;
	cachedPSFParams.DustSize = settings.DustSize;
	cachedPSFParams.PSFSharpness = settings.PSFSharpness;
	cachedPSFParams.PSFNoiseFloor = settings.PSFNoiseFloor;
	cachedPSFParams.BladeRoughnessAmp = settings.BladeRoughnessAmp;
	cachedPSFParams.BladeRoughnessFreq = settings.BladeRoughnessFreq;
	cachedPSFParams.ScratchCount = settings.ScratchCount;
	cachedPSFParams.ScratchOpacity = settings.ScratchOpacity;
	cachedPSFParams.ScratchLength = settings.ScratchLength;
	cachedPSFParams.ScratchWidth = settings.ScratchWidth;
	cachedPSFParams.SphericalAberration = settings.SphericalAberration;
	cachedPSFParams.KernelScale = settings.KernelScale;
	cachedPSFParams.UseAP1 = globals::features::linearLighting.settings.enableACEScg && globals::features::linearLighting.settings.enableLinearLighting;
	psfDirty = false;
}

void PhysicalGlare::DispatchFFT(ID3D11ComputeShader* shader, Texture2D* input, Texture2D* output, uint resolution)
{
	auto context = globals::d3d::context;

	ID3D11ShaderResourceView* srv = input->srv.get();
	ID3D11UnorderedAccessView* uav = output->uav.get();

	context->CSSetShaderResources(0, 1, &srv);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShader(shader, nullptr, 0);
	context->Dispatch(resolution, 1, 1);

	srv = nullptr;
	uav = nullptr;
	context->CSSetShaderResources(0, 1, &srv);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
}

void PhysicalGlare::Draw(TextureInfo& inout_tex)
{
	auto state = globals::state;
	auto context = globals::d3d::context;

	state->BeginPerfEvent("Physical Glare");

	// Handle FFT resolution change
	uint targetRes = std::clamp((uint)settings.FFTResolution, FFT_MIN, FFT_MAX);
	if (targetRes != currentFFTResolution) {
		CreateFFTTextures(targetRes);
	}

	// Accumulate tear film time
	if (settings.TearFilmStrength > 0.f) {
		tearFilmTimeAccum += *globals::game::deltaTime;
	}

	// Update constant buffer
	GlareCB cbData = {
		.Threshold = exp2(settings.ThresholdEV),
		.Intensity = settings.Intensity,
		.ScatterStrength = settings.ScatterStrength,
		.ApertureMode = (uint)settings.ApertureMode,
		.ApertureBlades = settings.ApertureBlades,
		.ApertureRotation = settings.ApertureRotation * 3.14159265f / 180.f,
		.AdaptSpeed = settings.AdaptSpeed,
		.DeltaTime = *globals::game::deltaTime,
		.FFTResolution = currentFFTResolution,
		.PaddingRatio = settings.PaddingRatio,
		.ScreenWidth = (float)texOutput->desc.Width,
		.ScreenHeight = (float)texOutput->desc.Height,
		.ChannelIndex = 0,
		.FresnelExponent = settings.FresnelExponent,
		.ChromaticSpread = settings.ChromaticSpread,
		.ApertureSize = 1.0f / std::max(settings.FStop, 1.0f),
		.PSFSharpness = settings.PSFSharpness,
		.PSFNoiseFloor = settings.PSFNoiseFloor,
		.EnableEyelashes = settings.EnableEyelashes ? 1u : 0u,
		.EyelashCurvature = settings.EyelashCurvature,
		// Eye mode
		.EyelashCount = (uint)settings.EyelashCount,
		.EyelashLength = settings.EyelashLength,
		.ParticleCount = (uint)settings.ParticleCount,
		.ParticleSize = settings.ParticleSize,
		.GratingCount = (uint)settings.GratingCount,
		.GratingStrength = settings.GratingStrength,
		.TearFilmStrength = settings.TearFilmStrength,
		.TearFilmSpeed = settings.TearFilmSpeed,
		.TearFilmComplexity = (uint)settings.TearFilmComplexity,
		.TearFilmTime = tearFilmTimeAccum,
		.SutureBranches = (uint)settings.SutureBranches,
		.SutureStrength = settings.SutureStrength,
		.SutureWidth = settings.SutureWidth,
		.StarburstCount = (uint)settings.StarburstCount,
		.StarburstStrength = settings.StarburstStrength,
		.StarburstIrregularity = settings.StarburstIrregularity,
		// Lens mode
		.DustCount = (uint)settings.DustCount,
		.DustSize = settings.DustSize,
		.BladeRoughnessFreq = (uint)settings.BladeRoughnessFreq,
		.BladeRoughnessAmp = settings.BladeRoughnessAmp,
		.ScratchCount = (uint)settings.ScratchCount,
		.ScratchOpacity = settings.ScratchOpacity,
		.ScratchLength = settings.ScratchLength,
		.ScratchWidth = settings.ScratchWidth,
		.SphericalAberration = settings.SphericalAberration,
		.UseAP1 = (globals::features::linearLighting.settings.enableACEScg && globals::features::linearLighting.settings.enableLinearLighting) ? 1u : 0u,
		.KernelScale = settings.KernelScale,
	};
	glareCB->Update(cbData);

	ID3D11Buffer* cb = glareCB->CB();
	context->CSSetConstantBuffers(1, 1, &cb);

	// ========== Step 1: Regenerate PSF if parameters changed ==========
	if (NeedsPSFRegeneration()) {
		GeneratePSF();

		// Re-update CB because GeneratePSF() overwrites it with DeltaTime=0
		glareCB->Update(cbData);
		cb = glareCB->CB();
		context->CSSetConstantBuffers(1, 1, &cb);
	}

	// ========== Step 2: Threshold + downsample scene into FFT textures ==========
	{
		// We write R, G, B channels into texFFT[0..2][0]
		ID3D11ShaderResourceView* srv = inout_tex.srv;
		std::array<ID3D11UnorderedAccessView*, 3> uavs = {
			texFFT[0][0]->uav.get(),
			texFFT[1][0]->uav.get(),
			texFFT[2][0]->uav.get()
		};

		context->CSSetShaderResources(0, 1, &srv);
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(thresholdCS.get(), nullptr, 0);
		context->Dispatch((currentFFTResolution + 7) >> 3, (currentFFTResolution + 7) >> 3, 1);

		srv = nullptr;
		uavs.fill(nullptr);
		context->CSSetShaderResources(0, 1, &srv);
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	}

	// ========== Step 3: Forward FFT on scene (per channel) ==========
	for (int ch = 0; ch < 3; ch++) {
		// Row FFT: texFFT[ch][0] -> texFFT[ch][1]
		DispatchFFT(fftRowCS.get(), texFFT[ch][0].get(), texFFT[ch][1].get(), currentFFTResolution);
		// Col FFT: texFFT[ch][1] -> texFFT[ch][0]
		DispatchFFT(fftColCS.get(), texFFT[ch][1].get(), texFFT[ch][0].get(), currentFFTResolution);
	}

	// ========== Step 4: Frequency-domain multiply (scene * PSF) ==========
	{
		// Input: texFFT[ch][0] (scene FFT), texPSF_FFT[ch]
		// Output: texFFT[ch][1]
		std::array<ID3D11ShaderResourceView*, 2> srvs = { nullptr, nullptr };
		std::array<ID3D11UnorderedAccessView*, 1> uavs = { nullptr };

		for (int ch = 0; ch < 3; ch++) {
			srvs[0] = texFFT[ch][0]->srv.get();
			srvs[1] = texPSF_FFT[ch]->srv.get();
			uavs[0] = texFFT[ch][1]->uav.get();

			context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
			context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
			context->CSSetShader(multiplyCS.get(), nullptr, 0);
			context->Dispatch((currentFFTResolution + 7) >> 3, (currentFFTResolution + 7) >> 3, 1);

			srvs.fill(nullptr);
			uavs.fill(nullptr);
			context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
			context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		}
	}

	// ========== Step 5: Inverse FFT (per channel) ==========
	for (int ch = 0; ch < 3; ch++) {
		// Row IFFT: texFFT[ch][1] -> texFFT[ch][0]
		DispatchFFT(fftRowInvCS.get(), texFFT[ch][1].get(), texFFT[ch][0].get(), currentFFTResolution);
		// Col IFFT: texFFT[ch][0] -> texFFT[ch][1]
		DispatchFFT(fftColInvCS.get(), texFFT[ch][0].get(), texFFT[ch][1].get(), currentFFTResolution);
	}

	// ========== Step 6: Composite (upsample + add to scene) ==========
	{
		// t0 = scene, t1/t2/t3 = IFFT result R/G/B (texFFT[ch][1]),
		// u0 = output
		std::array<ID3D11ShaderResourceView*, 4> srvs = {
			inout_tex.srv,
			texFFT[0][1]->srv.get(),
			texFFT[1][1]->srv.get(),
			texFFT[2][1]->srv.get(),
		};
		std::array<ID3D11UnorderedAccessView*, 1> uavs = {
			texOutput->uav.get(),
		};
		ID3D11SamplerState* sampler = linearSampler.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetSamplers(0, 1, &sampler);
		context->CSSetShader(compositeCS.get(), nullptr, 0);

		context->Dispatch(((uint)texOutput->desc.Width + 7) >> 3, ((uint)texOutput->desc.Height + 7) >> 3, 1);

		srvs.fill(nullptr);
		uavs.fill(nullptr);
		sampler = nullptr;
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetSamplers(0, 1, &sampler);
	}

	// Cleanup
	cb = nullptr;
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetShader(nullptr, nullptr, 0);

	state->EndPerfEvent();
}
