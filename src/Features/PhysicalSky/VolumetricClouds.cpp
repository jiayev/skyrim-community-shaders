#include "Features/PhysicalSky.h"

#include "Deferred.h"
#include "Features/CloudShadows.h"
#include "Features/PostProcessing/RasterPass.h"
#include "Features/TerrainShadows.h"
#include "Features/ViewAtmosphere.h"
#include "State.h"
#include "Util.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <imgui.h>
#include <limits>
#include <numbers>
#include <vector>

namespace
{
	constexpr float KilometersToGameUnits(float value) { return value / Util::Units::GAME_UNIT_TO_KM; }

	struct CloudMsQuadrature
	{
		std::array<double, 32> nodes, weights;

		CloudMsQuadrature()
		{
			for (uint32_t i = 0; i < nodes.size(); ++i) {
				double x = std::cos(std::numbers::pi * (i + 0.75) / (nodes.size() + 0.5));
				double derivative = 0.0;
				for (uint32_t iteration = 0; iteration < 16; ++iteration) {
					double previous = 1.0, current = x;
					for (uint32_t degree = 2; degree <= nodes.size(); ++degree) {
						const double next = ((2.0 * degree - 1.0) * x * current - (degree - 1.0) * previous) / degree;
						previous = current;
						current = next;
					}
					derivative = nodes.size() * (x * current - previous) / (x * x - 1.0);
					const double step = current / derivative;
					x -= step;
					if (std::abs(step) < 1e-14)
						break;
				}
				nodes[i] = 0.5 * (x + 1.0);
				weights[i] = 1.0 / ((1.0 - x * x) * derivative * derivative);
			}
		}
	};

	struct CloudMiePhase
	{
		float4 coefficients;

		explicit CloudMiePhase(float diameter)
		{
			const float d = std::isfinite(diameter) ? std::clamp(diameter, 5.f, 50.f) : 20.f;
			// Jendersie and d'Eon (2023), mean water-droplet diameter in micrometers.
			coefficients = {
				std::exp(-0.0990567f / (d - 1.67154f)),
				std::exp(-2.20679f / (d + 3.91029f) - 0.428934f),
				std::exp(3.62489f - 8.29288f / (d + 5.52825f)),
				std::exp(-0.599085f / (d - 0.641583f) - 0.665888f)
			};
		}

		double MeanCosine() const
		{
			const double g = coefficients.y;
			const double alpha = coefficients.z;
			const double meanDraine = g * (1.0 + alpha * (3.0 + 2.0 * g * g) / 5.0) /
			                          (1.0 + alpha * (1.0 + 2.0 * g * g) / 3.0);
			return std::lerp(static_cast<double>(coefficients.x), meanDraine, static_cast<double>(coefficients.w));
		}

		float4 AmbientPhase(float albedo) const
		{
			const double g = coefficients.y;
			const double g2 = g * g;
			const double alpha = coefficients.z;
			const double secondDraine = (g2 + alpha * (2.0 / 15.0 + 11.0 * g2 / 21.0 + 12.0 * g2 * g2 / 35.0)) /
			                            (1.0 + alpha * (1.0 + 2.0 * g2) / 3.0);
			const double first = MeanCosine();
			const double second = std::lerp(double(coefficients.x) * coefficients.x, secondDraine, double(coefficients.w));
			double fraction = first * first;
			double a = (first - fraction) / (1.0 - fraction);
			double b = (second - fraction) / (1.0 - fraction);
			const double vertex = b > 0.0 ? std::clamp(-a / (5.0 * b), -1.0, 1.0) : -1.0;
			if (b < 0.0 || 1.0 + 3.0 * a * vertex + 2.5 * b * (3.0 * vertex * vertex - 1.0) < 0.0) {
				fraction = std::max(0.0, (3.0 * first - 1.0) / 2.0);
				a = (first - fraction) / (1.0 - fraction);
				b = 0.0;
			}
			const double extinction = 1.0 - albedo * fraction;
			return { float(extinction), float(albedo * (1.0 - fraction) / extinction), float(std::sqrt(a)), float(std::sqrt(b)) };
		}

		double Evaluate4Pi(double cosine) const
		{
			const auto hg = [cosine](double g) {
				const double complement = 1.0 - g;
				const double denominator = complement * complement + 2.0 * g * (1.0 - cosine);
				return complement * (1.0 + g) / (denominator * std::sqrt(denominator));
			};
			const double g = coefficients.y;
			const double alpha = coefficients.z;
			const double draine = hg(g) * (1.0 + alpha * cosine * cosine) /
			                      (1.0 + alpha * (1.0 + 2.0 * g * g) / 3.0);
			return std::lerp(hg(coefficients.x), draine, static_cast<double>(coefficients.w));
		}
	};

	const std::array<float4, PhysicalSky::kCloudMsCalibrationSize>& CloudMsCoefficients(float albedo, const CloudMiePhase& phase)
	{
		static const CloudMsQuadrature quadrature;
		struct Integral
		{
			double depth, densityScale;
			std::array<double, 32> powers, weights, depths;
		};
		// Integrate a power-law boundary profile with finite-slab escape.
		static const auto integrals = [] {
			std::array<std::array<Integral, 4>, PhysicalSky::kCloudMsCalibrationSize> result{};
			for (uint32_t i = 0; i < result.size(); ++i) {
				const double referenceDepth = std::exp2(-12.0 + 0.5 * i);
				for (uint32_t j = 0; j < 4; ++j) {
					const double exponent = 0.35 + 0.1 * j;
					auto& integral = result[i][j];
					integral.depth = referenceDepth * std::tgamma(0.5) * std::tgamma(exponent + 1.0) / std::tgamma(exponent + 1.5);
					integral.densityScale = std::pow(referenceDepth / (exponent + 1.0), exponent / (exponent + 1.0));
					const double range = std::min(integral.depth, 40.0);
					for (uint32_t n = 0; n < quadrature.nodes.size(); ++n) {
						const double u = quadrature.nodes[n];
						const double depth = range * u * u;
						double escape = 0.0;
						for (uint32_t m = 0; m < quadrature.nodes.size(); ++m)
							escape += quadrature.weights[m] * std::exp(-depth / quadrature.nodes[m]);
						integral.depths[n] = depth;
						integral.powers[n] = std::pow(depth, exponent / (exponent + 1.0));
						integral.weights[n] = quadrature.weights[n] * range * u * escape;
					}
				}
			}
			return result;
		}();
		static std::array<float, 5> previous = { -1.f, 0.f, 0.f, 0.f, 0.f };
		static std::array<float4, PhysicalSky::kCloudMsCalibrationSize> coefficients{};
		const auto& fit = phase.coefficients;
		const std::array<float, 5> parameters = { albedo, fit.x, fit.y, fit.z, fit.w };
		if (parameters == previous)
			return coefficients;
		previous = parameters;
		const double omega = albedo;
		const double transport = 1.0 - omega * phase.MeanCosine();
		const double ratio = std::sqrt((1.0 - omega) / transport);
		const double reflectance = (1.0 - ratio) / (1.0 + ratio);
		const double attenuation = std::sqrt((1.0 - omega) * transport);
		std::array<double, 32> singleWeights{};
		for (uint32_t n = 0; n < quadrature.nodes.size(); ++n) {
			const double mu = quadrature.nodes[n];
			singleWeights[n] = quadrature.weights[n] * 0.5 * omega * phase.Evaluate4Pi(-mu) * mu / (1.0 + mu);
		}
		for (uint32_t i = 0; i < coefficients.size(); ++i) {
			std::array<float, 4> row{};
			for (uint32_t j = 0; j < row.size(); ++j) {
				const auto& integral = integrals[i][j];
				std::array<double, 32> transportWeights{};
				for (uint32_t n = 0; n < quadrature.nodes.size(); ++n)
					transportWeights[n] = integral.weights[n] * std::exp(-transport * integral.depths[n]);
				const double loss = -std::expm1(-2.0 * attenuation * integral.depth);
				const double total = reflectance * loss / (1.0 - reflectance * reflectance * (1.0 - loss));
				double single = 0.0;
				for (uint32_t n = 0; n < quadrature.nodes.size(); ++n) {
					const double mu = quadrature.nodes[n];
					single += singleWeights[n] * -std::expm1(-(1.0 + 1.0 / mu) * integral.depth);
				}
				if (total <= single)
					continue;
				double lower = 0.0, upper = 20.0;
				double maximum = 0.0;
				for (uint32_t n = 0; n < quadrature.nodes.size(); ++n) {
					const double transmission = std::exp(-upper * integral.powers[n]);
					maximum += omega * transportWeights[n] * omega * (1.0 - transmission) / (1.0 - omega + omega * transmission);
				}
				// Strong absorption can put the two-flux target outside the local closure's range.
				if (total - single >= maximum) {
					row[j] = static_cast<float>(upper * integral.densityScale);
					continue;
				}
				double amplitude = 1.0;
				for (uint32_t iteration = 0; iteration < 20; ++iteration) {
					double multiple = 0.0, derivative = 0.0;
					for (uint32_t n = 0; n < quadrature.nodes.size(); ++n) {
						const double transmission = std::exp(-amplitude * integral.powers[n]);
						const double denominator = 1.0 - omega + omega * transmission;
						const double gain = omega * (1.0 - transmission) / denominator;
						multiple += omega * transportWeights[n] * gain;
						derivative += omega * transportWeights[n] * omega * transmission * integral.powers[n] / (denominator * denominator);
					}
					const double error = multiple - (total - single);
					if (std::abs(error) < 1e-9)
						break;
					if (error < 0.0)
						lower = amplitude;
					else
						upper = amplitude;
					const double candidate = amplitude - error / std::max(derivative, 1e-20);
					amplitude = candidate > lower && candidate < upper ? candidate : 0.5 * (lower + upper);
				}
				row[j] = static_cast<float>(amplitude * integral.densityScale);
			}
			coefficients[i] = { row[0], row[1], row[2], row[3] };
		}
		return coefficients;
	}

	float2 CloudVelocity(float2 velocity)
	{
		const float speed = std::hypot(velocity.x, velocity.y);
		if (!std::isfinite(speed))
			return {};
		return velocity * (speed > 80.f ? 80.f / speed : 1.f);
	}

	float CloudUnitParameter(float value)
	{
		return std::isfinite(value) ? std::clamp(value, 0.f, 1.f) : 0.f;
	}

	bool IsVolumeTexture(ID3D11ShaderResourceView* srv)
	{
		if (!srv)
			return false;
		D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
		srv->GetDesc(&desc);
		return desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE3D && CloudWeatherManager::IsLinearFormat(desc.Format, 4);
	}

	constexpr DXGI_FORMAT kHistoryRadianceFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
	constexpr UINT kWeatherShapeSrvSlot = 107;
	constexpr UINT kWeatherSunSrvSlot = 97;
	constexpr UINT kWeatherSunWidth = 224;
	constexpr UINT kWeatherSeedStride = 2;
	static_assert(kWeatherSunWidth % (8 * kWeatherSeedStride) == 0);
	constexpr UINT kWeatherCurlSrvSlot = 104;
	constexpr UINT kFiniteCloudWeatherSrvSlot = 117;
	constexpr UINT kWeatherCloudBoundsSrvSlot = 5;
	constexpr UINT kWeatherCloudBoundsSize = CloudWeatherManager::kMapSize / 8u;
	static_assert(kWeatherCloudBoundsSize == 64);

	/** @brief Creates a single-mip 2D texture with an SRV and a UAV. */
	eastl::unique_ptr<Texture2D> CreateCloudTexture(uint32_t a_width, uint32_t a_height, DXGI_FORMAT a_format, const char* a_name)
	{
		D3D11_TEXTURE2D_DESC tex_desc = {
			.Width = a_width,
			.Height = a_height,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = a_format,
			.SampleDesc = { .Count = 1, .Quality = 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {
			.Format = tex_desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {
			.Format = tex_desc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		auto texture = eastl::make_unique<Texture2D>(tex_desc, a_name);
		texture->CreateSRV(srv_desc);
		texture->CreateUAV(uav_desc);
		return texture;
	}
}

float2 CloudWindSettings::UpperFlowDirection() const
{
	const auto shear = CloudVelocity(highVelocity) - CloudVelocity(lowVelocity);
	const float length = std::hypot(shear.x, shear.y);
	// Fade out the preferred direction as the shear approaches zero.
	return shear * (1.f / std::max(length, 1.f));
}

CloudWindSettings CloudWindSettings::Interpolate(const CloudWindSettings& from, const CloudWindSettings& to, float weight)
{
	const float blend = CloudUnitParameter(weight);
	const float2 lowFrom = CloudVelocity(from.lowVelocity);
	const float2 highFrom = CloudVelocity(from.highVelocity);
	return {
		.lowVelocity = lowFrom + (CloudVelocity(to.lowVelocity) - lowFrom) * blend,
		.highVelocity = highFrom + (CloudVelocity(to.highVelocity) - highFrom) * blend,
		.development = std::lerp(CloudUnitParameter(from.development), CloudUnitParameter(to.development), blend),
		.disturbance = std::lerp(CloudUnitParameter(from.disturbance), CloudUnitParameter(to.disturbance), blend)
	};
}

float LowCloudSettings::GetWorldScale() const
{
	return std::isfinite(worldScale) ? std::clamp(worldScale, 0.15f, 1.f) : 0.4f;
}

float2 LowCloudSettings::GetAltitudeRangeKm(bool convection) const
{
	const float base = std::isfinite(baseAltitude) ? std::clamp(baseAltitude, 0.f, 20000.f) : 700.f;
	const float depth = std::isfinite(baseDepthScale) ? std::clamp(baseDepthScale, .15f, 1.65f) : 1.f;
	const float sourceThickness = convection ? 9700.f : 5700.f * depth;
	return { base * 0.001f, (base + sourceThickness * GetWorldScale()) * 0.001f };
}

float LowCloudSettings::GetCurvatureRadiusKm() const
{
	return std::isfinite(curvatureRadius) ? std::clamp(curvatureRadius, 25.f, 10000.f) : 74.946f;
}

float LowCloudSettings::GetShadowBottomKm(float bottomKm, float rangeKm, uint32_t resolution, float2 cameraXY) const
{
	const float cell = rangeKm / static_cast<float>(resolution);
	const float x = std::abs(std::floor((cameraXY.x * Util::Units::GAME_UNIT_TO_KM) / cell) * cell) + 0.5f * rangeKm;
	const float y = std::abs(std::floor((cameraXY.y * Util::Units::GAME_UNIT_TO_KM) / cell) * cell) + 0.5f * rangeKm;
	const float distanceSquared = x * x + y * y;
	const float curvature = GetCurvatureRadiusKm();
	const float shell = std::max(0.5f * curvature + bottomKm, 0.001f);
	const float shellDrop = std::min(distanceSquared, shell * shell) /
	                        (shell + std::sqrt(std::max(shell * shell - distanceSquared, 0.f)));
	return bottomKm - std::max(shellDrop, distanceSquared / curvature);
}

float CirrusSettings::GetAltitudeKm() const
{
	return (std::isfinite(altitude) ? std::clamp(altitude, 1.f, 24000.f) : 8000.f) * 0.001f;
}

float CirrusSettings::GetUvSize() const
{
	return std::isfinite(uvSize) ? std::clamp(uvSize, 0.1f, 32.f) : 1.f;
}

void PhysicalSky::LoadCloudTextures()
{
	volMainHistoryValid = false;
	auto loadDDS = [&](const wchar_t* path, winrt::com_ptr<ID3D11ShaderResourceView>& srv, bool required = false) {
		srv = nullptr;
		const auto key = TextureManager::Key(path);
		std::error_code error;
		if (!std::filesystem::exists(path, error) && !error && !required) {
			if (cloudTextures.texList.erase(key))
				++cloudTextures.revision;
			cloudTextures.loadResults.erase(key);
			return;
		}
		cloudTextures.EnsureLoaded(key);
		srv.copy_from(cloudTextures.Query(key));
	};

	loadDDS(L"Data\\Textures\\PhysicalSky\\NubisCloudShapeNoise.dds", importedShapeNoiseSrv, !settings.cloudNoise.procedural);
	if (importedShapeNoiseSrv && !IsVolumeTexture(importedShapeNoiseSrv.get())) {
		logger::warn("Ignoring Nubis noise composite because it is not a linear RGBA 3D texture.");
		importedShapeNoiseSrv = nullptr;
	}

	loadDDS(L"Data\\Textures\\PhysicalSky\\NubisVerticalProfile.dds", cloudProfileLutSrv, true);
	loadDDS(L"Data\\Textures\\PhysicalSky\\NubisVerticalAdjustment.dds", importedAdjustmentLutSrv, true);
	if (cloudProfileLutSrv && !CloudWeatherManager::IsLinearTexture(cloudProfileLutSrv.get(), 2)) {
		logger::warn("Cloud profile DDS must be a linear 2D texture with at least RG channels.");
		cloudProfileLutSrv = nullptr;
	}
	if (importedAdjustmentLutSrv && !CloudWeatherManager::IsLinearTexture(importedAdjustmentLutSrv.get(), 3)) {
		logger::warn("Cloud adjustment DDS must be a linear 2D texture with at least RGB channels.");
		importedAdjustmentLutSrv = nullptr;
	}
	loadDDS(L"Data\\Textures\\PhysicalSky\\CloudWorley.dds", weatherWorleySrv);
	loadDDS(L"Data\\Textures\\PhysicalSky\\CloudTypeProfiles.dds", weatherProfilesSrv);
	loadDDS(L"Data\\Textures\\PhysicalSky\\CloudCurl.dds", weatherCurlSrv);
	weatherProfileBoundsValid = false;
	cloudWeatherBoundsValid = false;
	if (weatherWorleySrv) {
		D3D11_SHADER_RESOURCE_VIEW_DESC noiseDesc;
		weatherWorleySrv->GetDesc(&noiseDesc);
		if (noiseDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE3D || noiseDesc.Format != DXGI_FORMAT_R8_UNORM) {
			logger::warn("Ignoring cloud noise: expected a 3D R8 texture.");
			weatherWorleySrv = nullptr;
		}
	}
	if (weatherProfilesSrv) {
		winrt::com_ptr<ID3D11Resource> resource;
		weatherProfilesSrv->GetResource(resource.put());
		D3D11_TEXTURE2D_DESC profilesDesc{};
		if (auto texture = resource.try_as<ID3D11Texture2D>())
			texture->GetDesc(&profilesDesc);
		if (weatherWorleySrv && profilesDesc.Width == 256 && profilesDesc.Height == 258 &&
			profilesDesc.ArraySize == 1 && profilesDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
			logger::info("Loaded cloud type profiles and Worley noise.");
		} else {
			logger::warn("Ignoring incomplete or invalid cloud profiles.");
			weatherProfilesSrv = nullptr;
		}
	}
	baseShapeNoiseSrv = importedShapeNoiseSrv;
	cloudAdjustmentLutSrv = importedAdjustmentLutSrv;
	cloudCrossSectionDirty = true;
}

void PhysicalSky::SetupVolumetricResources()
{
	auto device = globals::d3d::device;
	auto context = globals::d3d::context;

	logger::debug("Setting up volumetric cloud resources...");

	debugCubeFaceSrvs.clear();
	texturePreview.ClearShaderCache();
	// Tileable sampler (wrap all axes)
	{
		D3D11_SAMPLER_DESC desc = {};
		desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
		desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		desc.MaxAnisotropy = 1;
		desc.MinLOD = 0;
		desc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&desc, sampTileable.put()));
	}

	// Volumetric cloud constants
	{
		volCloudCb = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<VolumetricCloudCB>());
		cloudDispatchCb = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<std::array<uint32_t, 4>>(), "PhysicalSky::CloudDispatch");
	}

	// Static low-cloud cross-section shown in the Clouds page.
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = kCloudCrossSectionW;
		desc.Height = kCloudCrossSectionH;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		texCloudCrossSection = eastl::make_unique<Texture2D>(desc, "PhysicalSky::CloudCrossSection");
		D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = desc.Format;
		srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv.Texture2D.MipLevels = 1;
		texCloudCrossSection->CreateSRV(srv);
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
		uav.Format = desc.Format;
		uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		texCloudCrossSection->CreateUAV(uav);
		cloudCrossSectionCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<CloudCrossSectionCB>());
		cloudCrossSectionReady = false;
		cloudCrossSectionDirty = true;
	}

	{
		D3D11_TEXTURE2D_DESC texDesc{
			.Width = 6,
			.Height = 1,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
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

		texVolCloudAmbientSH = eastl::make_unique<Texture2D>(texDesc, "PhysicalSky::VolumetricCloudAmbientSH");
		texVolCloudAmbientSH->CreateSRV(srvDesc);
		texVolCloudAmbientSH->CreateUAV(uavDesc);
		texDesc.Width = 8;
		texDesc.Height = 8;
		texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		texCloudAmbientPartial = eastl::make_unique<Texture2D>(texDesc, "PhysicalSky::CloudAmbientPartial");
		texCloudAmbientPartial->CreateSRV(srvDesc);
		texCloudAmbientPartial->CreateUAV(uavDesc);
	}

	// Get main render target dimensions for TR/Lum textures
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

	D3D11_TEXTURE2D_DESC mainDesc;
	mainTex.texture->GetDesc(&mainDesc);

	cloudAmbientVolumeCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<CloudAmbientVolumeCB>(), "PhysicalSky::CloudAmbientVolumeConstants");

	const uint32_t lowW = std::max(1u, (mainDesc.Width + 3u) / 4u);
	const uint32_t lowH = std::max(1u, (mainDesc.Height + 3u) / 4u);
	texVolLowTr = CreateCloudTexture(lowW, lowH, DXGI_FORMAT_R16_FLOAT, "PhysicalSky::CloudTraceTr");
	texVolLowLum = CreateCloudTexture(lowW, lowH, kHistoryRadianceFormat, "PhysicalSky::CloudTraceLum");
	texVolLowAux = CreateCloudTexture(lowW, lowH, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::CloudTraceAux");
	texLightningTrace = CreateCloudTexture(lowW, lowH, DXGI_FORMAT_R16G16_FLOAT, "PhysicalSky::LightningTrace");
	texVolStormMotion = CreateCloudTexture(lowW, lowH, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::CloudStormMotion");
	texStormTiles = CreateCloudTexture((lowW + 7u) / 8u, (lowH + 7u) / 8u, DXGI_FORMAT_R32_UINT, "PhysicalSky::StormTiles");
	texUpperTiles = CreateCloudTexture((lowW + 7u) / 8u, (lowH + 7u) / 8u, DXGI_FORMAT_R32_UINT, "PhysicalSky::UpperCloudTiles");
	texUpperSunDepth = CreateCloudTexture(256, 256, DXGI_FORMAT_R16G16_FLOAT, "PhysicalSky::UpperSunDepth");
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = desc.Height = kMiddleBoundsSize;
		desc.MipLevels = kMiddleBoundsLevels;
		desc.ArraySize = desc.SampleDesc.Count = 1;
		desc.Format = DXGI_FORMAT_R32G32_FLOAT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		texMiddleBounds = eastl::make_unique<Texture2D>(desc, "PhysicalSky::MiddleCloudBounds");
		D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
		srv.Format = desc.Format;
		srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srv.Texture2D.MipLevels = kMiddleBoundsLevels;
		texMiddleBounds->CreateSRV(srv);
		for (uint32_t mip = 0; mip < kMiddleBoundsLevels; ++mip) {
			srv.Texture2D.MostDetailedMip = mip;
			srv.Texture2D.MipLevels = 1;
			middleBoundsSrvs[mip] = nullptr;
			DX::ThrowIfFailed(device->CreateShaderResourceView(texMiddleBounds->resource.get(), &srv, middleBoundsSrvs[mip].put()));
			Util::SetResourceName(middleBoundsSrvs[mip].get(), "PhysicalSky::MiddleCloudBounds Mip%u SRV", mip);
			D3D11_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = desc.Format;
			uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uav.Texture2D.MipSlice = mip;
			middleBoundsUavs[mip] = nullptr;
			DX::ThrowIfFailed(device->CreateUnorderedAccessView(texMiddleBounds->resource.get(), &uav, middleBoundsUavs[mip].put()));
			Util::SetResourceName(middleBoundsUavs[mip].get(), "PhysicalSky::MiddleCloudBounds Mip%u UAV", mip);
		}
		middleBoundsDirty = true;
	}
	texVoxelTraceTr = CreateCloudTexture(lowW, lowH, DXGI_FORMAT_R16_FLOAT, "PhysicalSky::VoxelTraceTr");
	texVoxelTraceLum = CreateCloudTexture(lowW, lowH, kHistoryRadianceFormat, "PhysicalSky::VoxelTraceLum");
	texVoxelTraceAux = CreateCloudTexture(lowW, lowH, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VoxelTraceAux");
	texVoxelTraceMotion = CreateCloudTexture(lowW, lowH, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VoxelTraceMotion");
	texVoxelTraceLightning = CreateCloudTexture(lowW, lowH, DXGI_FORMAT_R16G16_FLOAT, "PhysicalSky::VoxelTraceLightning");

	auto createCube = [](uint32_t size, DXGI_FORMAT format, const char* name) {
		D3D11_TEXTURE2D_DESC desc = {
			.Width = size,
			.Height = size,
			.MipLevels = 1,
			.ArraySize = 6,
			.Format = format,
			.SampleDesc = { .Count = 1, .Quality = 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srv = {
			.Format = desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE,
			.TextureCube = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav = {
			.Format = desc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY,
			.Texture2DArray = { .MipSlice = 0, .FirstArraySlice = 0, .ArraySize = 6 }
		};
		auto texture = eastl::make_unique<Texture2D>(desc, name);
		texture->CreateSRV(srv);
		texture->CreateUAV(uav);
		return texture;
	};
	texVolCubeTr = createCube(kVolCubeSize, DXGI_FORMAT_R16_FLOAT, "PhysicalSky::VolCubeTr");
	texVolCubeLum = createCube(kVolCubeSize, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolCubeLum");
	texVolCubeAux = createCube(kVolCubeSize, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolCubeAux");
	texVolCubeHistoryTr = createCube(kVolCubeSize, DXGI_FORMAT_R16_FLOAT, "PhysicalSky::VolCubeHistoryTr");
	texVolCubeHistoryLum = createCube(kVolCubeSize, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolCubeHistoryLum");
	texVolCubeHistoryAux = createCube(kVolCubeSize, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolCubeHistoryAux");
	texVolCubeTraceTr = createCube(kVolCubeSize / 4u, DXGI_FORMAT_R16_FLOAT, "PhysicalSky::VolCubeTraceTr");
	texVolCubeTraceLum = createCube(kVolCubeSize / 4u, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolCubeTraceLum");
	texVolCubeTraceAux = createCube(kVolCubeSize / 4u, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolCubeTraceAux");
	texLightningCubeTrace = createCube(kVolCubeSize / 4u, DXGI_FORMAT_R16G16_FLOAT, "PhysicalSky::LightningCubeTrace");
	texLightningCubeMask = createCube(kVolCubeSize, DXGI_FORMAT_R16G16_FLOAT, "PhysicalSky::LightningCubeMask");
	texLightningCubeHistory = createCube(kVolCubeSize, DXGI_FORMAT_R16G16_FLOAT, "PhysicalSky::LightningCubeHistory");
	texLightningCubeLum = createCube(kVolCubeSize, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::LightningCubeRadiance");
	texVolCubeStormMotion = createCube(kVolCubeSize / 4u, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::VolCubeStormMotion");
	for (auto* texture : { texLightningTrace.get(), texLightningCubeTrace.get(), texLightningCubeMask.get(), texLightningCubeHistory.get(), texLightningCubeLum.get() })
		context->ClearUnorderedAccessViewFloat(texture->uav.get(), kClearLuminance);
	lightning.SetupResources();
	context->ClearUnorderedAccessViewFloat(texVolCubeTr->uav.get(), kClearTransmittance);
	context->ClearUnorderedAccessViewFloat(texVolCubeLum->uav.get(), kClearLuminance);
	context->ClearUnorderedAccessViewFloat(texVolCubeAux->uav.get(), kClearLuminance);
	volMainHistoryValid = false;

	// Shadow volume 3D texture
	{
		D3D11_TEXTURE3D_DESC tex3d_desc = {
			.Width = kShadowVolW,
			.Height = kShadowVolH,
			.Depth = kShadowVolD,
			.MipLevels = 1,
			.Format = DXGI_FORMAT_R16_FLOAT,
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {
			.Format = tex3d_desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D,
			.Texture3D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {
			.Format = tex3d_desc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D,
			.Texture3D = { .MipSlice = 0, .FirstWSlice = 0, .WSize = kShadowVolD }
		};

		texShadowVolume = eastl::make_unique<Texture3D>(tex3d_desc, "PhysicalSky::VolumetricCloudShadowVolume");
		texShadowVolume->CreateSRV(srv_desc);
		texShadowVolume->CreateUAV(uav_desc);
		texShadowVolumeRaw = eastl::make_unique<Texture3D>(tex3d_desc, "PhysicalSky::CloudShadowIntegration");
		texShadowVolumeRaw->CreateSRV(srv_desc);
		texShadowVolumeRaw->CreateUAV(uav_desc);

		tex3d_desc.MipLevels = srv_desc.Texture3D.MipLevels = 2;
		tex3d_desc.Width = tex3d_desc.Height = kWeatherSunWidth;
		tex3d_desc.Depth = uav_desc.Texture3D.WSize = kWeatherSunSlices;
		tex3d_desc.Format = srv_desc.Format = uav_desc.Format = DXGI_FORMAT_R16G16_FLOAT;
		auto makeSunVolume = [&](const char* name, winrt::com_ptr<ID3D11UnorderedAccessView>& ambientUav) {
			auto volume = eastl::make_unique<Texture3D>(tex3d_desc, name);
			volume->CreateSRV(srv_desc);
			volume->CreateUAV(uav_desc);
			auto ambientDesc = uav_desc;
			ambientDesc.Texture3D.MipSlice = 1;
			ambientDesc.Texture3D.WSize = kWeatherSunSlices / 2;
			ambientUav = nullptr;
			DX::ThrowIfFailed(device->CreateUnorderedAccessView(volume->resource.get(), &ambientDesc, ambientUav.put()));
			Util::SetResourceName(ambientUav.get(), "%s Ambient Mip UAV", name);
			return volume;
		};
		weatherLightVolume = makeSunVolume("PhysicalSky::WeatherDirectLight", weatherAmbientUav);
		weatherLightHistory = makeSunVolume("PhysicalSky::WeatherDirectLightHistory", weatherAmbientHistoryUav);
		weatherLightValid = false;

		context->ClearUnorderedAccessViewFloat(texShadowVolume->uav.get(), kClearShadowVolume);
		context->ClearUnorderedAccessViewFloat(texShadowVolumeRaw->uav.get(), kClearShadowVolume);
		// Metadata row plus endpoint-inclusive vertical mean transmittance profile.
		texCloudShadowMean = CreateCloudTexture(kProjectedShadowWidth + 1u, kShadowMeanHeights + 1u, DXGI_FORMAT_R32G32B32A32_FLOAT, "PhysicalSky::CloudShadowMean");
		context->ClearUnorderedAccessViewFloat(texCloudShadowMean->uav.get(), kClearLuminance);

		tex3d_desc.Width = kProjectedShadowWidth;
		tex3d_desc.Height = kShadowMeanHeights;
		tex3d_desc.Depth = uav_desc.Texture3D.WSize = kProjectedAPShadowDepth;
		tex3d_desc.MipLevels = srv_desc.Texture3D.MipLevels = 1;
		tex3d_desc.Format = srv_desc.Format = uav_desc.Format = DXGI_FORMAT_R32_FLOAT;
		texProjectedAPShadow = eastl::make_unique<Texture3D>(tex3d_desc, "PhysicalSky::ProjectedAPShadow");
		texProjectedAPShadow->CreateSRV(srv_desc);
		texProjectedAPShadow->CreateUAV(uav_desc);
		projectedAPShadowFrame = UINT32_MAX;
	}

	// Load cloud textures and initialize weather generation.
	LoadCloudTextures();
	weatherMap.SetupResources();
	texWeatherProfileBounds = CreateCloudTexture(32, 64, DXGI_FORMAT_R16G16_FLOAT, "PhysicalSky::WeatherProfileBounds");
	texWeatherCloudBounds = CreateCloudTexture(kWeatherCloudBoundsSize, kWeatherCloudBoundsSize, DXGI_FORMAT_R32_UINT, "PhysicalSky::WeatherCloudBounds");
	cirrusMapManager.SetupResources();
	midCloudMapManager.SetupResources();
	cloudStormManager.SetupResources();
	cloudNoiseGenerator.SetupResources();
}

/** @brief Full-resolution cloud targets, created together on first use. */
static constexpr struct
{
	eastl::unique_ptr<Texture2D> PhysicalSky::* texture;
	DXGI_FORMAT format;
	const char* name;
} kScreenTargets[] = {
	{ &PhysicalSky::texLightningMask, DXGI_FORMAT_R16G16_FLOAT, "PhysicalSky::LightningMask" },
	{ &PhysicalSky::texLightningHistory, DXGI_FORMAT_R16G16_FLOAT, "PhysicalSky::LightningHistory" },
	{ &PhysicalSky::texLightningChannels, kHistoryRadianceFormat, "PhysicalSky::LightningChannels" },
	{ &PhysicalSky::texVolFilteredTr, DXGI_FORMAT_R16_FLOAT, "PhysicalSky::CloudFilteredTr" },
	{ &PhysicalSky::texVolFilteredLum, kHistoryRadianceFormat, "PhysicalSky::CloudFilteredLum" },
	{ &PhysicalSky::texVolFilteredAux, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::CloudFilteredAux" },
	{ &PhysicalSky::texVolTr, DXGI_FORMAT_R16_FLOAT, "PhysicalSky::CloudTr" },
	{ &PhysicalSky::texVolLum, kHistoryRadianceFormat, "PhysicalSky::CloudLum" },
	{ &PhysicalSky::texVolAux, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::CloudAux" },
	{ &PhysicalSky::texVolHistoryTr, DXGI_FORMAT_R16_FLOAT, "PhysicalSky::CloudHistoryTr" },
	{ &PhysicalSky::texVolHistoryLum, kHistoryRadianceFormat, "PhysicalSky::CloudHistoryLum" },
	{ &PhysicalSky::texVolHistoryAux, DXGI_FORMAT_R16G16B16A16_FLOAT, "PhysicalSky::CloudHistoryAux" },
};

bool PhysicalSky::EnsureVolumetricScreenResources()
{
	if (texVolTr) {
		assert(std::ranges::all_of(kScreenTargets, [this](const auto& target) { return this->*target.texture != nullptr; }));
		return true;
	}

	D3D11_TEXTURE2D_DESC mainDesc;
	globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].texture->GetDesc(&mainDesc);
	try {
		for (const auto& target : kScreenTargets)
			this->*target.texture = CreateCloudTexture(mainDesc.Width, mainDesc.Height, target.format, target.name);
	} catch (const std::exception& e) {
		logger::error("[PhysicalSky] Failed to create the volumetric cloud screen targets ({}), disabling volumetric clouds for this session", e.what());
		for (const auto& target : kScreenTargets)
			(this->*target.texture).reset();
		volumetricScreenResourcesFailed = true;
		return false;
	}

	// New targets are read before the first cloud pass writes them.
	ClearVolumetricToNeutral();
	volMainHistoryValid = false;
	return true;
}

void PhysicalSky::CompileVolumetricShaders()
{
	volMainHistoryValid = false;
	weatherProfileBoundsValid = false;
	cloudWeatherBoundsValid = false;
	logger::debug("Queueing volumetric cloud shaders...");
	cloudStormManager.CompileShaders(shaders);

	struct ShaderInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* csPtr;
		const char* filename;
		Util::AsyncShaderSet::Defines defines;
		std::string entry = "main";
	};

	std::array shaderInfos = {
		ShaderInfo{ &csVolAmbientSH, "Volumetrics.cs.hlsl", {}, "buildCloudAmbientSH" },
		ShaderInfo{ &csCloudAmbientReduce, "Volumetrics.cs.hlsl", {}, "reduceCloudAmbientSH" },
		ShaderInfo{ &csVolMainView, "Volumetrics.cs.hlsl", { { "CLOUD_AP_SHADOW", "" }, { "CLOUD_MAIN_DISPATCH", "" } } },
		ShaderInfo{ &csUpperSunDepth, "Volumetrics.cs.hlsl", { { "NO_CLOUD_STORMS", "" } }, "buildUpperSunDepth" },
		ShaderInfo{ &csUpperView, "Volumetrics.cs.hlsl", { { "NO_CLOUD_STORMS", "" } }, "renderUpperView" },
		ShaderInfo{ &csUpperTiles, "Volumetrics.cs.hlsl", { { "NO_CLOUD_STORMS", "" } }, "classifyUpperTiles" },
		ShaderInfo{ &csUpperViewTiles, "Volumetrics.cs.hlsl", { { "NO_CLOUD_STORMS", "" } }, "renderUpperTiles" },
		ShaderInfo{ &csMiddleBounds, "MidCloudBounds.cs.hlsl", {} },
		ShaderInfo{ &csMiddleBoundsReduce, "MidCloudBounds.cs.hlsl", {}, "reduceBounds" },
		ShaderInfo{ &csStormTiles, "Volumetrics.cs.hlsl", {}, "classifyStormTiles" },
		ShaderInfo{ &csWeatherProfileBounds, "Volumetrics.cs.hlsl", { { "NO_CLOUD_STORMS", "" }, { "BUILD_WEATHER_BOUNDS", "" } }, "buildWeatherProfileBounds" },
		ShaderInfo{ &csWeatherCloudBounds, "Volumetrics.cs.hlsl", { { "NO_CLOUD_STORMS", "" }, { "BUILD_WEATHER_BOUNDS", "" } }, "buildWeatherCloudBounds" },
		ShaderInfo{ &csVolMainVolcanic, "Volumetrics.cs.hlsl", { { "CLOUD_AP_SHADOW", "" }, { "CLOUD_MAIN_DISPATCH", "" }, { "VOLCANIC_STORMS_ONLY", "" } } },
		ShaderInfo{ &csVolMainSingleVolcanic, "Volumetrics.cs.hlsl", { { "CLOUD_AP_SHADOW", "" }, { "CLOUD_MAIN_DISPATCH", "" }, { "VOLCANIC_STORMS_ONLY", "" }, { "SINGLE_CLOUD_STORM", "" } } },
		ShaderInfo{ &csVolCubemapVolcanic, "Volumetrics.cs.hlsl", { { "CLOUD_REFLECTION", "" }, { "VOLCANIC_STORMS_ONLY", "" } }, "renderCubemap" },
		ShaderInfo{ &csVolShadowVolcanic, "Volumetrics.cs.hlsl", { { "VOLCANIC_STORMS_ONLY", "" } }, "renderShadowVolume" },
		ShaderInfo{ &csVoxelView, "Volumetrics.cs.hlsl", { { "CLOUD_AP_SHADOW", "" }, { "CLOUD_VOXELS", "" }, { "CLOUD_VOXEL_VIEW", "" }, { "NO_CLOUD_STORMS", "" } }, "renderVoxelView" },
		ShaderInfo{ &csVoxelViewSingle, "Volumetrics.cs.hlsl", { { "CLOUD_AP_SHADOW", "" }, { "CLOUD_VOXELS", "" }, { "CLOUD_VOXEL_SINGLE", "" }, { "CLOUD_VOXEL_VIEW", "" }, { "NO_CLOUD_STORMS", "" } }, "renderVoxelView" },
		ShaderInfo{ &csVoxelLighting, "Volumetrics.cs.hlsl", { { "CLOUD_VOXELS", "" } }, "buildVoxelLighting" },
		ShaderInfo{ &csVolCubemapVoxel, "Volumetrics.cs.hlsl", { { "CLOUD_REFLECTION", "" }, { "CLOUD_VOXELS", "" }, { "CLOUD_VOXEL_COARSE", "" } }, "renderCubemap" },
		ShaderInfo{ &csVolShadowVoxel, "Volumetrics.cs.hlsl", { { "CLOUD_VOXELS", "" } }, "renderShadowVolume" },
		ShaderInfo{ &csVolFilter, "Volumetrics.cs.hlsl", {}, "filterCloud" },
		ShaderInfo{ &csVolFilterWide, "Volumetrics.cs.hlsl", { { "CLOUD_BLUR_RADIUS", "2" } }, "filterCloud" },
		ShaderInfo{ &csVolFilterWideFast, "Volumetrics.cs.hlsl", {}, "filterCloudWideFast" },
		ShaderInfo{ &csVolReproject, "Volumetrics.cs.hlsl", {}, "reproject" },
		ShaderInfo{ &csVolReprojectFast, "Volumetrics.cs.hlsl", { { "CLOUD_FAST_RECONSTRUCTION", "" } }, "reproject" },
		ShaderInfo{ &csVolCubeReproject, "Volumetrics.cs.hlsl", {}, "reprojectCubemap" },
		ShaderInfo{ &csVolShadowVolume, "Volumetrics.cs.hlsl", {}, "renderShadowVolume" },
		ShaderInfo{ &csVolShadowWeather, "Volumetrics.cs.hlsl", { { "NO_CLOUD_STORMS", "" } }, "renderShadowVolume" },
		ShaderInfo{ &csWeatherLightSlices, "Volumetrics.cs.hlsl", { { "NO_CLOUD_STORMS", "" } }, "buildWeatherDirectLight" },
		ShaderInfo{ &csWeatherAmbientSlices, "Volumetrics.cs.hlsl", { { "NO_CLOUD_STORMS", "" } }, "buildWeatherAmbientLight" },
		ShaderInfo{ &csWeatherLightReproject, "Volumetrics.cs.hlsl", { { "NO_CLOUD_STORMS", "" } }, "reprojectWeatherDirectLight" },
		ShaderInfo{ &csVolShadowColumns, "Volumetrics.cs.hlsl", { { "NO_CLOUD_STORMS", "" } }, "renderShadowColumns" },
		ShaderInfo{ &csVolShadowResample, "Volumetrics.cs.hlsl", {}, "resampleShadowVolume" },
		ShaderInfo{ &csCloudShadowMean, "Volumetrics.cs.hlsl", {}, "buildCloudShadowMean" },
		ShaderInfo{ &csCloudShadowMeanWeather, "Volumetrics.cs.hlsl", { { "NO_CLOUD_STORMS", "" } }, "buildCloudShadowMean" },
		ShaderInfo{ &csCloudShadowMeanVolcanic, "Volumetrics.cs.hlsl", { { "VOLCANIC_STORMS_ONLY", "" } }, "buildCloudShadowMean" },
		ShaderInfo{ &csCloudShadowMeanVoxel, "Volumetrics.cs.hlsl", { { "CLOUD_VOXELS", "" } }, "buildCloudShadowMean" },
		ShaderInfo{ &csIntegrateProjectedAPShadow, "Volumetrics.cs.hlsl", {}, "integrateProjectedAPShadow" },
		ShaderInfo{ &csVolCubemap, "Volumetrics.cs.hlsl", { { "CLOUD_REFLECTION", "" } }, "renderCubemap" },
		ShaderInfo{ &csCloudCrossSection, "CloudCrossSection.cs.hlsl", {} },
		ShaderInfo{ &csLightningQuery, "Volumetrics.cs.hlsl", { { "CLOUD_VOXELS", "" } }, "findLightningSource" },
		ShaderInfo{ &csLightningComposite, "Volumetrics.cs.hlsl", { { "CLOUD_VOXELS", "" } }, "compositeLightning" },
		ShaderInfo{ &csLightningCubeComposite, "Volumetrics.cs.hlsl", { { "CLOUD_VOXELS", "" } }, "compositeLightningCube" }
	};

	for (auto& info : shaderInfos) {
		const auto path = (std::filesystem::path(kShaderDir) / info.filename).wstring();
		shaders.Add(*info.csPtr, path, std::move(info.defines), std::move(info.entry));
	}
	const auto weatherPath = (std::filesystem::path(kShaderDir) / "Volumetrics.cs.hlsl").wstring();
	Util::AsyncShaderSet::Defines weatherDefines{ { "CLOUD_MAIN_DISPATCH", "" }, { "CLOUD_AP_SHADOW", "" }, { "NO_CLOUD_STORMS", "" } };
	shaders.Add(weatherViewShaders.general, weatherPath, weatherDefines);
	auto baseOnly = weatherDefines;
	baseOnly.emplace_back("WEATHER_BASE_ONLY", "");
	shaders.Add(weatherViewShaders.baseOnly, weatherPath, baseOnly);
	baseOnly.emplace_back("CLOUD_QUALITY_VIEW_STEPS", "");
	shaders.Add(weatherViewShaders.baseOnlyQuality, weatherPath, std::move(baseOnly));
	weatherDefines.emplace_back("WEATHER_EMPTY_BOUNDS", "");
	shaders.Add(weatherViewShaders.bounds, weatherPath, weatherDefines);
	weatherDefines.emplace_back("CLOUD_QUALITY_VIEW_STEPS", "");
	shaders.Add(weatherViewShaders.boundsQuality, weatherPath, std::move(weatherDefines));
	weatherMap.CompileShaders(shaders);
	cirrusMapManager.CompileShaders(shaders);
	midCloudMapManager.CompileShaders(shaders);
	middleBoundsDirty = true;
	cloudNoiseGenerator.CompileShaders(shaders);
}

bool PhysicalSky::UpdateWeatherCloudBounds(const VolumetricCloudCB& a_cloudData)
{
	if (!csWeatherProfileBounds || !csWeatherCloudBounds || !texWeatherProfileBounds || !texWeatherCloudBounds)
		return false;
	const std::array boundsKey{ a_cloudData.lowCloudBaseAltitude, a_cloudData.lowCloudTopAltitude,
		a_cloudData.weatherBaseTopAltitude, a_cloudData.weatherStormTopAltitude, a_cloudData.baseHeightVariation, a_cloudData.weatherDetail.z };
	if (cloudWeatherBoundsValid && cloudWeatherBoundsKey == boundsKey)
		return true;

	auto* context = globals::d3d::context;
	ID3D11UnorderedAccessView* nullUav = nullptr;
	globals::profiler->BeginPass("PhysicalSky::WeatherCloudBounds");
	if (!weatherProfileBoundsValid) {
		auto* output = texWeatherProfileBounds->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
		context->CSSetShader(csWeatherProfileBounds.get(), nullptr, 0);
		context->Dispatch(4, 8, 1);
		context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
		weatherProfileBoundsValid = true;
	}
	auto* profiles = texWeatherProfileBounds->srv.get();
	context->CSSetShaderResources(kWeatherCloudBoundsSrvSlot, 1, &profiles);
	auto* output = texWeatherCloudBounds->uav.get();
	context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
	context->CSSetShader(csWeatherCloudBounds.get(), nullptr, 0);
	context->Dispatch(kWeatherCloudBoundsSize, kWeatherCloudBoundsSize, 1);
	context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	ID3D11ShaderResourceView* nullSrv = nullptr;
	context->CSSetShaderResources(kWeatherCloudBoundsSrvSlot, 1, &nullSrv);
	globals::profiler->EndPass();
	cloudWeatherBoundsKey = boundsKey;
	cloudWeatherBoundsValid = true;
	return true;
}

void PhysicalSky::RenderCloudCrossSection()
{
	auto* weather = weatherMap.Weather();
	auto* finiteWeather = weatherMap.FiniteWeather();
	if (!texCloudCrossSection || !csCloudCrossSection || !cloudCrossSectionCB || !weather || !weatherCurlSrv || !weatherWorleySrv || !weatherProfilesSrv)
		return;

	const auto& low = settings.cloudLayer.low;
	const float2 base = low.GetAltitudeRangeKm(false);
	const float2 storm = low.GetAltitudeRangeKm(true);
	const float spanX = std::clamp(low.weatherSize.x, 4.f, 64.f) * 1000.f;
	const float spanY = std::clamp(low.weatherSize.y, 4.f, 64.f) * 1000.f;
	const auto camera = globals::game::frameBufferCached.GetCameraPosAdjust();
	const float2 cameraMeters{ camera.x * Util::Units::GAME_UNIT_TO_M, camera.y * Util::Units::GAME_UNIT_TO_M };

	const auto cameraTop = weatherMap.HasConvection() ? storm.y : base.y;
	auto* detailNoise = low.nvdfDetail > 0.f ? voxelCloudManager.Noise() : nullptr;
	const CloudCrossSectionCB data{
		.field = { 1.f / spanX, 1.f / spanY, base.x * 1000.f, cameraTop * 1000.f },
		.cameraSpan = { cameraMeters.x, cameraMeters.y, spanX, spanY },
		.shape = { base.y * 1000.f, storm.y * 1000.f, low.GetWorldScale(), std::clamp(low.baseFadeMeters, 0.f, 450.f) },
		.extinction = { std::clamp(low.stratusExtinction, 0.f, .75f), std::clamp(low.cumulusExtinction, 0.f, .75f), std::clamp(low.stormExtinction, 0.f, .75f), volWind.disturbance },
		.wind = { static_cast<float>(volWind.lowOffset[0]), static_cast<float>(volWind.lowOffset[1]),
			detailNoise ? std::clamp(low.nvdfDetail, 0.f, 1.f) : 0.f, std::clamp(low.baseHeightVariation, 0.f, 450.f) },
		.proceduralWeight = weatherMap.GetProceduralWeight(),
		.finiteCloudWeather = finiteWeather ? 1u : 0u,
	};
	cloudCrossSectionCB->Update(data);

	auto* context = globals::d3d::context;
	auto* cb = cloudCrossSectionCB->CB();
	ID3D11ShaderResourceView* profiles[] = { weatherWorleySrv.get(), weatherProfilesSrv.get() };
	ID3D11SamplerState* samplers[] = { sampTileable.get(), sampTr.get() };
	auto* uav = texCloudCrossSection->uav.get();
	context->CSSetConstantBuffers(0, 1, &cb);
	context->CSSetSamplers(2, 2, samplers);
	context->CSSetShaderResources(6, 1, &weather);
	context->CSSetShaderResources(kFiniteCloudWeatherSrvSlot, 1, &finiteWeather);
	context->CSSetShaderResources(76, 1, &detailNoise);
	context->CSSetShaderResources(kWeatherShapeSrvSlot, 2, profiles);
	auto* curl = weatherCurlSrv.get();
	context->CSSetShaderResources(kWeatherCurlSrvSlot, 1, &curl);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShader(csCloudCrossSection.get(), nullptr, 0);
	globals::profiler->BeginPass("PhysicalSky::CloudCrossSection");
	context->Dispatch((kCloudCrossSectionW + 7u) >> 3, (kCloudCrossSectionH + 7u) >> 3, 1);
	globals::profiler->EndPass();

	ID3D11ShaderResourceView* nullSrvs[2] = {};
	ID3D11UnorderedAccessView* nullUav = nullptr;
	ID3D11Buffer* nullCb = nullptr;
	ID3D11SamplerState* nullSamplers[2] = {};
	context->CSSetShaderResources(6, 1, nullSrvs);
	context->CSSetShaderResources(kFiniteCloudWeatherSrvSlot, 1, nullSrvs);
	context->CSSetShaderResources(76, 1, nullSrvs);
	context->CSSetShaderResources(kWeatherShapeSrvSlot, 2, nullSrvs);
	context->CSSetShaderResources(kWeatherCurlSrvSlot, 1, nullSrvs);
	context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	context->CSSetConstantBuffers(0, 1, &nullCb);
	context->CSSetSamplers(2, 2, nullSamplers);
	context->CSSetShader(nullptr, nullptr, 0);

	cloudCrossSectionCamera = cameraMeters;
	cloudCrossSectionDirty = false;
	cloudCrossSectionReady = true;
}

void CloudStormManager::UpdateLighting(float dropletDiameter)
{
	if (lightingRevision == revision && lightingDiameter == dropletDiameter)
		return;
	const CloudMiePhase phase(dropletDiameter);
	for (auto& instance : active) {
		instance.ambientPhase = phase.AmbientPhase(instance.driftDensity.w);
		instance.msCalibration = CloudMsCoefficients(instance.driftDensity.w, phase);
	}
	lightingRevision = revision;
	lightingDiameter = dropletDiameter;
}

void PhysicalSky::UpdateCloudWind()
{
	auto* ui = globals::game::ui;
	if (!ui || ui->GameIsPaused())
		return;
	const double elapsed = RE::GetSecondsSinceLastFrame();
	if (!std::isfinite(elapsed) || elapsed <= 0.0)
		return;

	const auto& layer = settings.cloudLayer;
	const float2 lowVelocity = CloudVelocity(layer.wind.lowVelocity);
	const float2 highVelocity = CloudVelocity(layer.wind.highVelocity);
	volWind.lowOffset[0] += lowVelocity.x * elapsed;
	volWind.lowOffset[1] += lowVelocity.y * elapsed;
	volWind.highOffset[0] += highVelocity.x * elapsed;
	volWind.highOffset[1] += highVelocity.y * elapsed;
	volWind.phase += CloudUnitParameter(layer.wind.development) * 0.03 * elapsed;
	volWind.stormTime += elapsed;
	volWind.disturbance = std::lerp(volWind.disturbance, CloudUnitParameter(layer.wind.disturbance), static_cast<float>(-std::expm1(-elapsed / 2.0)));
}

void PhysicalSky::UpdateDiskPhaseLut()
{
	if (!csDiskPhaseLutGen || !texDiskPhaseLut || !diskPhaseCb)
		return;
	const auto& lighting = settings.cloudLayer.lighting;
	const auto fit = CloudMiePhase(lighting.dropletDiameter).coefficients;
	const std::array<float, 12> parameters = {
		fit.x, fit.y, fit.z, fit.w,
		cbData.diskAngularRadius.x, cbData.diskAngularRadius.y, cbData.diskAngularRadius.z, 0.f,
		std::clamp(lighting.phaseForwardG, 0.f, .95f), std::clamp(lighting.phaseBackwardG, -.95f, 0.f),
		std::clamp(settings.aerosolPhaseG, -.999f, .999f), std::lerp(.82f, .94f, std::clamp(1.f - std::abs(settings.silverLiningSpread), 0.f, 1.f))
	};
	auto* context = globals::d3d::context;
	if (!diskPhaseValid || parameters != diskPhaseParameters) {
		ID3D11ShaderResourceView* nullSrv = nullptr;
		context->PSSetShaderResources(106, 1, &nullSrv);
		context->CSSetShaderResources(106, 1, &nullSrv);
		diskPhaseCb->Update(parameters);
		auto* constants = diskPhaseCb->CB();
		winrt::com_ptr<ID3D11Buffer> previousConstants;
		context->CSGetConstantBuffers(0, 1, previousConstants.put());
		context->CSSetConstantBuffers(0, 1, &constants);
		auto* uav = texDiskPhaseLut->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(csDiskPhaseLutGen.get(), nullptr, 0);
		globals::profiler->BeginPass("PhysicalSky::DiskPhaseLut");
		context->Dispatch((kDiskPhaseLutSize + 63u) / 64u, 3, 1);
		globals::profiler->EndPass();
		uav = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(nullptr, nullptr, 0);
		constants = previousConstants.get();
		context->CSSetConstantBuffers(0, 1, &constants);
		diskPhaseParameters = parameters;
		diskPhaseValid = true;
	}
	auto* srv = texDiskPhaseLut->srv.get();
	context->CSSetShaderResources(106, 1, &srv);
}

void PhysicalSky::RenderVolumetricClouds(VolumetricCloudPass a_pass)
{
	if (a_pass == VolumetricCloudPass::kMainViewAndCubemap) {
		const bool unified = globals::features::viewAtmosphere.IsReady();
		if (unified != volHistoryUnifiedMedium) {
			volMainHistoryValid = false;
			volHistoryUnifiedMedium = unified;
		}
	}
	if (!ShadersOK())
		return;
	if (!weatherMap.Weather() || !weatherCurlSrv || !weatherWorleySrv || !weatherProfilesSrv || !baseShapeNoiseSrv || !cloudProfileLutSrv || !cloudAdjustmentLutSrv)
		return;
	volumetricNeutralCleared = false;

	auto state = globals::state;
	auto context = globals::d3d::context;
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	float3 cloudLightDir = cbData.sunDir;
	if (auto* shaderManager = globals::game::smState) {
		if (auto* shadowSceneNode = shaderManager->shadowSceneNode[0]) {
			auto shadowSunLight = shadowSceneNode->GetRuntimeData().sunLight;
			if (shadowSunLight && shadowSunLight->light) {
				if (auto* dirLight = skyrim_cast<RE::NiDirectionalLight*>(shadowSunLight->light.get())) {
					const auto& direction = dirLight->GetWorldDirection();
					cloudLightDir = { -direction.x, -direction.y, -direction.z };
					const float lengthSq = cloudLightDir.x * cloudLightDir.x + cloudLightDir.y * cloudLightDir.y + cloudLightDir.z * cloudLightDir.z;
					if (lengthSq > 1e-6f) {
						const float invLength = 1.0f / std::sqrt(lengthSq);
						cloudLightDir.x *= invLength;
						cloudLightDir.y *= invLength;
						cloudLightDir.z *= invLength;
					} else {
						cloudLightDir = cbData.sunDir;
					}
				}
			}
		}
	}
	// Ground shadow directions may be elevation-clamped or switched to a moon
	// while elevated clouds are still sunlit. Trace their actual solar column.
	if (cbData.volCloudUseSun != 0)
		cloudLightDir = cbData.sunDir;
	const auto& frameBuffer = globals::game::frameBufferCached;
	const auto& resolutionScale = frameBuffer.GetDynamicResolutionParams1();
	const float2 textureDim = { static_cast<float>(texVolTr->desc.Width), static_cast<float>(texVolTr->desc.Height) };
	const float2 frameDim = {
		std::clamp(textureDim.x * resolutionScale.x, 1.0f, textureDim.x),
		std::clamp(textureDim.y * resolutionScale.y, 1.0f, textureDim.y)
	};
	const uint32_t renderW = static_cast<uint32_t>(frameDim.x);
	const uint32_t renderH = static_cast<uint32_t>(frameDim.y);

	auto& low = settings.cloudLayer.low;
	auto& cirrus = settings.cloudLayer.cirrus;
	const auto& middle = settings.cloudLayer.middle;
	auto* middleWeather = middle.enabled ? midCloudMapManager.Weather() : nullptr;
	const auto middleAltitude = middle.GetAltitudeRange();
	const auto bandValue = [](float value, float lo, float hi, float fallback) {
		return std::isfinite(value) ? std::clamp(value, lo, hi) : fallback;
	};
	const float bandAngle = (bandValue(middle.bands.direction, -360.f, 360.f, 0.f) + 90.f) * (std::numbers::pi_v<float> / 180.f);
	const float bandWavelength = bandValue(middle.bands.wavelength, 500.f, 16000.f, 2000.f) / Util::Units::GAME_UNIT_TO_M;
	const float bandCoverage = bandValue(middle.bands.coverage, 0.f, 1.f, 0.f);
	const float bandThickness = bandValue(middle.bands.thickness, 0.f, 1.f, 0.f);
	const float bandLift = bandValue(middle.bands.displacement, 0.f, .5f, 0.f) * bandValue(middle.thickness, 100.f, 4000.f, 800.f) / Util::Units::GAME_UNIT_TO_M;
	auto& lighting = settings.cloudLayer.lighting;

	low.weatherSize.x = std::clamp(low.weatherSize.x, 4.0f, 64.0f);
	low.weatherSize.y = std::clamp(low.weatherSize.y, 4.0f, 64.0f);

	const auto& previousWind = volMainHistoryValid ? volHistoryWind : volWind;
	auto offset = [](const std::array<double, 2>& value) -> float2 {
		return { static_cast<float>(value[0] / Util::Units::GAME_UNIT_TO_M), static_cast<float>(value[1] / Util::Units::GAME_UNIT_TO_M) };
	};
	auto delta = [](const std::array<double, 2>& current, const std::array<double, 2>& previous) -> float2 {
		return { static_cast<float>((current[0] - previous[0]) / Util::Units::GAME_UNIT_TO_M), static_cast<float>((current[1] - previous[1]) / Util::Units::GAME_UNIT_TO_M) };
	};
	const float2 noiseWindOffset = offset(volWind.lowOffset);
	const float2 windDelta = delta(volWind.lowOffset, previousWind.lowOffset);
	const bool hasConvection = weatherMap.HasConvection();
	auto* finiteWeather = weatherMap.FiniteWeather();
	const float2 lowAltitudeRange = low.GetAltitudeRangeKm(hasConvection);
	const float lowCloudBaseKm = lowAltitudeRange.x;
	const float lowCloudTopKm = lowAltitudeRange.y;
	const float cloudCurvatureRadius = KilometersToGameUnits(low.GetCurvatureRadiusKm());
	const auto stormAltitude = cloudStormManager.Altitude();
	const bool stormActive = cloudStormManager.Count() > 0;
	float volumeBottomKm = stormActive ? std::min(lowCloudBaseKm, stormAltitude.x * 0.001f) : lowCloudBaseKm;
	float volumeTopKm = stormActive ? std::max(lowCloudTopKm, stormAltitude.y * 0.001f) : lowCloudTopKm;
	if (middle.enabled) {
		volumeBottomKm = std::min(volumeBottomKm, middleAltitude.x * .001f);
		volumeTopKm = std::max(volumeTopKm, middleAltitude.y * .001f);
	}
	const bool voxelActive = voxelCloudManager.Count() > 0;
	if (voxelActive) {
		const auto altitude = voxelCloudManager.Altitude();
		volumeBottomKm = std::min(volumeBottomKm, altitude.x * .001f);
		volumeTopKm = std::max(volumeTopKm, altitude.y * .001f);
	}

	const uint32_t traceScale = settings.cloudHalfResTrace ? 2u : 1u;
	const uint32_t traceStride = 4u * traceScale;
	const uint32_t lowW = (renderW + traceStride - 1u) / traceStride;
	const uint32_t lowH = (renderH + traceStride - 1u) / traceStride;
	const int blurRadius = std::clamp(settings.cloudBlurRadius, 0, 2);

	const auto cameraPosition = globals::game::frameBufferCached.GetCameraPosAdjust();
	const auto cirrusTextures = cirrusMapManager.GetTextures();
	const bool renderCirrus = cirrus.enabled && bool(cirrusTextures);
	const CloudMiePhase lowCloudPhase(lighting.dropletDiameter);
	auto* voxelNoise = settings.cloudNoise.useVoxelNoise || low.nvdfDetail > 0.f ? voxelCloudManager.Noise() : nullptr;
	auto* middleVoxelNoise = settings.cloudNoise.useVoxelNoise ? voxelNoise : nullptr;
	const float shadowBottomKm = low.GetShadowBottomKm(volumeBottomKm, settings.shadowVolumeRange, kShadowVolW, { cameraPosition.x, cameraPosition.y });

	VolumetricCloudCB cloudData = {
		.rayMarchRange = KilometersToGameUnits(std::clamp(settings.rayMarchRange, 1.f, 64.f)),
		.shadowVolumeRange = KilometersToGameUnits(settings.shadowVolumeRange),
		.marchStepScale = std::clamp(settings.marchStepScale, 0.0625f, 4.f),
		.cloudFrameIndex = volFrameIndex,

		.rcpFrameDim = float2(1.0f) / textureDim,
		.activeFrameDim = { static_cast<float>(renderW), static_cast<float>(renderH) },
		.lowFrameDim = { static_cast<float>(lowW), static_cast<float>(lowH) },
		.previousFrameDim = volHistoryFrameDim,

		.dirlightDir = cloudLightDir,
		.bottomZ = cbData.zBottom,
		.previousCamera = volHistoryCamera,
		.planetRadius = cbData.rPlanet,

		.lowestCloudAltitude = KilometersToGameUnits(volumeBottomKm),
		.highestCloudAltitude = KilometersToGameUnits(volumeTopKm),
		.lowCloudBaseAltitude = KilometersToGameUnits(lowCloudBaseKm),
		.lowCloudTopAltitude = KilometersToGameUnits(lowCloudTopKm),

		.weatherBaseTopAltitude = KilometersToGameUnits(low.GetAltitudeRangeKm(false).y),
		.weatherStormTopAltitude = KilometersToGameUnits(low.GetAltitudeRangeKm(true).y),
		.cloudCurvatureRadius = cloudCurvatureRadius,
		.weatherWorldScale = low.GetWorldScale(),

		.shadowVolumeBottom = KilometersToGameUnits(shadowBottomKm),
		.shadowVolumeTop = KilometersToGameUnits(volumeTopKm),
		.historyValid = volMainHistoryValid ? 1u : 0u,
		.cirrusEnabled = renderCirrus ? 1u : 0u,

		.weatherFrequency = { Util::Units::GAME_UNIT_TO_KM / low.weatherSize.x, Util::Units::GAME_UNIT_TO_KM / low.weatherSize.y },
		.noiseWindOffset = noiseWindOffset,
		.cloudWindDelta = windDelta,
		.cirrusWindOffset = offset(volWind.highOffset),

		.cirrusWindDelta = delta(volWind.highOffset, previousWind.highOffset),
		.cirrusAltitude = KilometersToGameUnits(cirrus.GetAltitudeKm()),
		.cirrusUvInvSize = 1.f / cirrus.GetUvSize(),

		.cirrusDensityScale = std::max(cirrus.densityScale, 0.f),
		.cirrusLightThickness = std::max(lighting.cirrusLightThickness, 1.f),
		.phaseForwardG = std::clamp(lighting.phaseForwardG, 0.f, 0.95f),
		.phaseBackwardG = std::clamp(lighting.phaseBackwardG, -0.95f, 0.f),

		.cumulusExtinction = std::clamp(low.cumulusExtinction, 0.f, .75f),
		.coverageBottomPower = 0.4375f,
		.coverageHeightRange = 0.3f,
		.bottomDensityPower = 4.6f,
		.bottomDensityWidth = 6.8f,
		.topExpansion = 0.f,
		.scatterDensityThreshold = std::max(lighting.scatterDensityThreshold, 0.001f),
		.scatterDensityPower = std::max(lighting.scatterDensityPower, 0.01f),

		.cloudLightingScale = std::max(lighting.brightness, 0.f),
		.sunExtinction = std::max(lighting.sunExtinction, 0.f),
		.viewExtinction = std::max(lighting.viewExtinction, 0.f),
		.ambientStrength = std::max(lighting.ambientStrength, 0.f),

		.msAlbedo = std::clamp(lighting.msAlbedo, 0.f, 0.9999f),
		.msStrength = std::max(lighting.msStrength, 0.f),

		.weatherDetail = { voxelNoise ? std::clamp(low.nvdfDetail, 0.f, 1.f) : 0.f, 0.f, std::clamp(low.baseFadeMeters, 0.f, 450.f), volWind.disturbance },
		.previousViewProj = volHistoryViewProj,

		.lowCloudPhase = lowCloudPhase.coefficients,
		.stormCount = cloudStormManager.Count(),
		.voxelCount = voxelCloudManager.Count(),
		.voxelAltitude = voxelCloudManager.Altitude(),
		.skipShapeNoise = settings.cloudNoise.skipShapeNoise ? 1u : 0u,
		.useVoxelNoise = middleVoxelNoise ? 1u : 0u,
		.noisePadding = { volMainHistoryValid ? volHistoryCurvatureRadius : cloudCurvatureRadius, cloudWeatherChanged ? 1.f / 32.f : 0.f },
		.middleLayer = { KilometersToGameUnits(middleAltitude.x * .001f), KilometersToGameUnits(middleAltitude.y * .001f),
			middleWeather && std::isfinite(middle.densityScale) ? std::clamp(middle.densityScale, 0.f, 1.f) : 0.f,
			Util::Units::GAME_UNIT_TO_M / (16384.f * (std::isfinite(middle.uvSize) ? std::clamp(middle.uvSize, .01f, 100.f) : 1.f)) },
		.middleShape = { std::isfinite(middle.edgeSoftness) ? std::clamp(middle.edgeSoftness, .01f, .49f) : .1f,
			0.f,
			Util::Units::GAME_UNIT_TO_M / (std::isfinite(middle.detailScale) ? std::clamp(middle.detailScale, 200.f, 4000.f) : 256.f),
			static_cast<float>(std::min(middle.detailMode, 1u)) },
		.middleCells = { std::isfinite(middle.cellFrequency) ? std::clamp(middle.cellFrequency, .25f, 16.f) : 2.f,
			std::isfinite(middle.cellStrength) ? std::clamp(middle.cellStrength, 0.f, 1.f) : .9f,
			std::isfinite(middle.stratusCellStrength) ? std::clamp(middle.stratusCellStrength, 0.f, 1.f) : .1f,
			std::isfinite(middle.warpStrength) ? std::clamp(middle.warpStrength, 0.f, .5f) : .1f },
		.middleBands = { std::cos(bandAngle) / bandWavelength, std::sin(bandAngle) / bandWavelength,
			bandValue(middle.bands.warp, 0.f, 1.f, .25f), bandCoverage },
		.middleBandShape = { bandThickness, bandLift, bandWavelength, bandCoverage + bandThickness + bandLift > 0.f ? 1.f : 0.f },
		.cloudTraceScale = traceScale,
		.updateAmbientMip = lighting.ambientStrength > 0.f ? 1u : 0u,
		.stormExtinction = std::clamp(low.stormExtinction, 0.f, .75f),
		.stratusExtinction = std::clamp(low.stratusExtinction, 0.f, .75f),
		.usePerformanceCloudSteps = settings.usePerformanceCloudSteps ? 1u : 0u,
		.weatherLightFirstSlice = (volFrameIndex * kWeatherSlicesPerFrame) % kWeatherSunSlices,
		.baseHeightVariation = std::clamp(low.baseHeightVariation, 0.f, 450.f),
		.weatherProceduralWeight = weatherMap.GetProceduralWeight(),
		.finiteCloudWeather = finiteWeather ? 1u : 0u,
		.cloudCascadeShadows = settings.cloudCascadeShadows ? 1u : 0u,
	};
	const bool updateAmbient = cloudData.updateAmbientMip != 0u;
	cloudStormManager.UpdateLighting(lighting.dropletDiameter);
	cloudStormManager.UpdateMotion(volWind.stormTime, previousWind.stormTime,
		CloudVelocity(settings.cloudLayer.wind.lowVelocity), CloudVelocity(settings.cloudLayer.wind.highVelocity),
		(lowAltitudeRange.x + lowAltitudeRange.y) * 500.f, settings.cloudLayer.cirrus.GetAltitudeKm() * 1000.f);
	cloudData.msTransportExtinction = 1.f - cloudData.msAlbedo * static_cast<float>(lowCloudPhase.MeanCosine());
	cloudData.msBoundaryWidth = std::clamp(lighting.msBoundaryWidth, 1.f, 200.f);
	cloudData.msCalibration = CloudMsCoefficients(cloudData.msAlbedo, lowCloudPhase);
	// The light-volume projection covers the horizon from at least 2500 source metres.
	// The weather sphere matches the authored tangent curvature.
	const bool mainView = a_pass == VolumetricCloudPass::kMainViewAndCubemap;
	const float radius = cloudCurvatureRadius * 0.5f;
	const float eyeHeight = std::max(cameraPosition.z - cloudData.bottomZ, 2500.f * low.GetWorldScale() / Util::Units::GAME_UNIT_TO_M);
	const float horizon = std::sqrt(cameraPosition.x * cameraPosition.x + cameraPosition.y * cameraPosition.y +
									eyeHeight * (2.f * radius + eyeHeight));
	const float farHorizon = horizon + std::sqrt(cloudData.lowCloudTopAltitude * (2.f * radius + cloudData.lowCloudTopAltitude));
	const float originZ = eyeHeight - horizon * farHorizon / (radius + eyeHeight);
	const float4 nextGrid{ cameraPosition.x, cameraPosition.y, originZ, farHorizon };
	bool rebuildSun = false;
	bool reprojectSun = false;
	if (mainView) {
		if (weatherLightValid) {
			weatherLightGrid.x += noiseWindOffset.x - weatherLightLastWind.x;
			weatherLightGrid.y += noiseWindOffset.y - weatherLightLastWind.y;
		}
		cloudData.weatherPreviousLightGrid = weatherLightGrid;
		const float dx = nextGrid.x - weatherLightGrid.x, dy = nextGrid.y - weatherLightGrid.y, dz = nextGrid.z - weatherLightGrid.z;
		const float threshold = 0.03f * std::min(nextGrid.w, weatherLightGrid.w);
		// Continuous weather blends change the settings key every frame. Refresh their
		// slices in rotation; rebuild on a cut or a layer shift larger than one slice.
		const float sliceHeight = (cloudData.lowCloudTopAltitude - cloudData.lowCloudBaseAltitude) / kWeatherSunSlices;
		// At each handoff the near probes point at the current disc, while
		// the far column can retain light from a whole update cycle ago.
		constexpr float kMaxSolarShiftCos = 0.9999619f;  // 0.5 degrees
		const bool solarShift = weatherLightValid && std::ranges::any_of(weatherLightSliceDirections, [&](const float3& direction) {
			return direction.x * cloudLightDir.x + direction.y * cloudLightDir.y + direction.z * cloudLightDir.z < kMaxSolarShiftCos;
		});
		rebuildSun = !weatherLightValid || (updateAmbient && !weatherAmbientValid) || !volMainHistoryValid || solarShift ||
		             dx * dx + dy * dy > nextGrid.w * nextGrid.w ||
		             std::abs(weatherLightAltitudes.x - cloudData.lowCloudBaseAltitude) > sliceHeight ||
		             std::abs(weatherLightAltitudes.y - cloudData.lowCloudTopAltitude) > sliceHeight;
		reprojectSun = !rebuildSun && (dx * dx + dy * dy + dz * dz > threshold * threshold || std::abs(nextGrid.w - weatherLightGrid.w) > threshold);
		if (rebuildSun || reprojectSun)
			weatherLightGrid = nextGrid;
		weatherLightLastWind = noiseWindOffset;
		weatherLightAltitudes = { cloudData.lowCloudBaseAltitude, cloudData.lowCloudTopAltitude };
	}
	cloudData.weatherLightGrid = weatherLightGrid;
	cloudData.weatherLightSeedStride = rebuildSun ? kWeatherSeedStride : 1u;
	if (mainView && rebuildSun)
		weatherAmbientUpdates = 0;
	cloudData.weatherAmbientUpdate = weatherAmbientUpdates;
	volCloudCb->Update(cloudData);

	const auto depthTarget = globals::features::viewAtmosphere.CompositeEnabled() && a_pass == VolumetricCloudPass::kMainViewAndCubemap ?
	                             RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN :
	                             RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY;

	auto* cloudConstants = volCloudCb->CB();
	context->CSSetConstantBuffers(1, 1, &cloudConstants);
	context->CSSetShaderResources(kFiniteCloudWeatherSrvSlot, 1, &finiteWeather);
	context->CSSetShaderResources(99, 1, &middleWeather);
	// Reuse the mipmapped weather noises; middle detail is evaluated independently
	// of the coarse weather-map resolution in every upper-cloud density query.
	const auto& middleNoise = weatherMap.GetNoiseInputs();
	context->CSSetShaderResources(101, 3, middleNoise.data() + 1);
	ID3D11ShaderResourceView* weatherSrvs[] = { weatherWorleySrv.get(), weatherProfilesSrv.get() };
	context->CSSetShaderResources(kWeatherShapeSrvSlot, 2, weatherSrvs);
	auto* curl = weatherCurlSrv.get();
	context->CSSetShaderResources(kWeatherCurlSrvSlot, 1, &curl);
	if (voxelActive)
		voxelCloudManager.Bind();
	if (voxelNoise)
		context->CSSetShaderResources(76, 1, &voxelNoise);
	std::array<ID3D11ShaderResourceView*, 12> srvs = {
		texTrLut->srv.get(),                                                  // t1
		texApLut->srv.get(),                                                  // t2
		renderer->GetDepthStencilData().depthStencils[depthTarget].depthSRV,  // t3
		baseShapeNoiseSrv.get(),                                              // t4 cloud shape noise
		nullptr,                                                              // t5
		weatherMap.Weather(),                                                 // t6 coverage and type pairs
		globals::deferred->GetDirectionalShadowMap(),                         // t7
		texMsLut->srv.get(),                                                  // t8
		renderCirrus ? cirrusTextures.weather : nullptr,                      // t9
		renderCirrus ? cirrusTextures.patterns : nullptr,                     // t10
		cloudProfileLutSrv.get(),                                             // t11 vertical profile
		cloudAdjustmentLutSrv.get(),                                          // t12 profile expansion and noise warp
	};
	ID3D11ShaderResourceView* ambientShSrv = texVolCloudAmbientSH ? texVolCloudAmbientSH->srv.get() : nullptr;
	std::array<ID3D11SamplerState*, 3> samplers = { sampTileable.get(), sampTr.get(), sampSv.get() };
	context->CSSetSamplers(2, (uint)samplers.size(), samplers.data());
	if (mainView) {
		ID3D11ShaderResourceView* nullInput = nullptr;
		ID3D11UnorderedAccessView* nullOutput = nullptr;
		context->CSSetShaderResources(1, (uint)srvs.size(), srvs.data());
		globals::profiler->BeginPass("PhysicalSky::WeatherDirectLight");
		const auto buildSlices = [&](UINT width, UINT slices) {
			auto* output = weatherLightVolume->uav.get();
			context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
			context->CSSetShader(csWeatherLightSlices.get(), nullptr, 0);
			context->Dispatch(width / 8, width / 8, slices);
			context->CSSetUnorderedAccessViews(0, 1, &nullOutput, nullptr);
		};
		constexpr UINT ambientWidth = kWeatherSunWidth / 2, ambientSlices = kWeatherSunSlices / 2;
		const auto buildAmbient = [&](ID3D11UnorderedAccessView* output, UINT width, UINT slices) {
			context->CSSetUnorderedAccessViews(1, 1, &output, nullptr);
			context->CSSetShader(csWeatherAmbientSlices.get(), nullptr, 0);
			context->Dispatch((width + 7u) / 8u, (width + 7u) / 8u, slices);
			context->CSSetUnorderedAccessViews(1, 1, &nullOutput, nullptr);
		};
		if (rebuildSun) {
			buildSlices(kWeatherSunWidth / kWeatherSeedStride, kWeatherSunSlices);
			weatherLightSliceDirections.fill(cloudLightDir);
			if (updateAmbient)
				buildAmbient(weatherAmbientUav.get(), ambientWidth / 4, ambientSlices);
		}
		if (rebuildSun || reprojectSun) {
			auto* oldVolume = weatherLightVolume->srv.get();
			ID3D11UnorderedAccessView* outputs[] = { weatherLightHistory->uav.get(), updateAmbient ? weatherAmbientHistoryUav.get() : nullptr };
			context->CSSetShaderResources(kWeatherSunSrvSlot, 1, &oldVolume);
			context->CSSetUnorderedAccessViews(0, 2, outputs, nullptr);
			context->CSSetShader(csWeatherLightReproject.get(), nullptr, 0);
			context->Dispatch(kWeatherSunWidth / 8, kWeatherSunWidth / 8, kWeatherSunSlices);
			ID3D11UnorderedAccessView* nullOutputs[2]{};
			context->CSSetUnorderedAccessViews(0, 2, nullOutputs, nullptr);
			context->CSSetShaderResources(kWeatherSunSrvSlot, 1, &nullInput);
			weatherLightVolume.swap(weatherLightHistory);
			std::swap(weatherAmbientUav, weatherAmbientHistoryUav);
		}
		if (!rebuildSun) {
			buildSlices(kWeatherSunWidth, kWeatherSlicesPerFrame);
			weatherLightSliceDirections[cloudData.weatherLightFirstSlice / kWeatherSlicesPerFrame] = cloudLightDir;
			if (updateAmbient) {
				auto* history = weatherLightVolume->srv.get();
				context->CSSetShaderResources(kWeatherSunSrvSlot, 1, &history);
				buildAmbient(weatherAmbientHistoryUav.get(), ambientWidth, 1);
				context->CSSetShaderResources(kWeatherSunSrvSlot, 1, &nullInput);
				const UINT slice = weatherAmbientUpdates % ambientSlices;
				const D3D11_BOX box{ 0, 0, slice, ambientWidth, ambientWidth, slice + 1 };
				context->CopySubresourceRegion(weatherLightVolume->resource.get(), 1, 0, 0, slice, weatherLightHistory->resource.get(), 1, &box);
				++weatherAmbientUpdates;
			}
		}
		weatherAmbientValid = updateAmbient;
		auto* sunVolume = weatherLightVolume->srv.get();
		context->CSSetShaderResources(kWeatherSunSrvSlot, 1, &sunVolume);
		globals::profiler->EndPass();
		weatherLightValid = true;
	}
	if (voxelActive && a_pass == VolumetricCloudPass::kMainViewAndCubemap)
		voxelCloudManager.UpdateLighting(csVoxelLighting.get(), cloudData.dirlightDir);
	if (a_pass == VolumetricCloudPass::kMainViewAndCubemap && texVolCloudAmbientSH) {
		state->BeginPerfEvent("Volumetric Clouds: Ambient SH");
		ID3D11ShaderResourceView* nullAmbientSrv = nullptr;
		context->CSSetShaderResources(13, 1, &nullAmbientSrv);
		auto* groundReflection = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS].SRV;
		context->CSSetShaderResources(39, 1, &groundReflection);
		auto* partialOutput = texCloudAmbientPartial->uav.get();
		context->CSSetShaderResources(1, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(1, 1, &partialOutput, nullptr);
		context->CSSetShader(csVolAmbientSH.get(), nullptr, 0);
		globals::profiler->BeginPass("PhysicalSky::VolumetricCloudAmbientSH");
		context->Dispatch(8, 1, 1);
		ID3D11UnorderedAccessView* nullUav = nullptr;
		context->CSSetUnorderedAccessViews(1, 1, &nullUav, nullptr);
		auto* partialInput = texCloudAmbientPartial->srv.get();
		context->CSSetShaderResources(90, 1, &partialInput);
		auto* ambientShOutput = texVolCloudAmbientSH->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &ambientShOutput, nullptr);
		context->CSSetShader(csCloudAmbientReduce.get(), nullptr, 0);
		context->Dispatch(1, 1, 1);
		globals::profiler->EndPass();
		context->CSSetShaderResources(90, 1, &nullAmbientSrv);
		context->CSSetShaderResources(39, 1, &nullAmbientSrv);

		context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
		context->CSSetShader(nullptr, nullptr, 0);
		state->EndPerfEvent();
	}

	std::array<ID3D11ShaderResourceView*, 3> outputSrvs = { texVolTr->srv.get(), texVolLum->srv.get(), texShadowVolume->srv.get() };

	context->CSSetSamplers(2, (uint)samplers.size(), samplers.data());
	ID3D11ShaderResourceView* stormSrvs[] = { cloudStormManager.Height(), cloudStormManager.Modeling(), cloudStormManager.Data() };
	context->CSSetShaderResources(55, 3, stormSrvs);

	if (a_pass == VolumetricCloudPass::kShadowVolume) {
		// Shadow path: accumulate the cloud extinction column into the 3D shadow
		// volume along the light direction.
		state->BeginPerfEvent("Volumetric Clouds: Shadow Volume");
		std::array<ID3D11UnorderedAccessView*, 1> uavs = { texShadowVolumeRaw->uav.get() };
		ID3D11ShaderResourceView* nullPsShadowSrv = nullptr;
		context->PSSetShaderResources(112, 1, &nullPsShadowSrv);

		context->CSSetShaderResources(1, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);

		// Dispatch based on dominant light direction component
		const float shadowRangeGu = KilometersToGameUnits(settings.shadowVolumeRange);
		const float shadowThicknessGu = std::max(cloudData.shadowVolumeTop - cloudData.shadowVolumeBottom, 1.f);
		float3 ray_px_dir = { -cloudLightDir.x, -cloudLightDir.y, -cloudLightDir.z };
		ray_px_dir.x *= kShadowVolW / shadowRangeGu;
		ray_px_dir.y *= kShadowVolH / shadowRangeGu;
		ray_px_dir.z *= kShadowVolD / shadowThicknessGu;
		const float dir_max_component = std::max({ std::abs(ray_px_dir.x), std::abs(ray_px_dir.y), std::abs(ray_px_dir.z) });
		const bool horizontal = std::abs(ray_px_dir.x) == dir_max_component || std::abs(ray_px_dir.y) == dir_max_component;
		// Long, nearly horizontal low/storm rays need the parallel scan's latency
		// hiding. Short columns and sparse multi-layer weather favour coherent rays.
		const bool serialColumns = !voxelActive && cloudData.stormCount == 0u && (!horizontal || cloudData.middleLayer.z > 0.f);
		auto* shadowShader = voxelActive                      ? csVolShadowVoxel.get() :
		                     cloudData.stormCount == 0u       ? (serialColumns ? csVolShadowColumns.get() : csVolShadowWeather.get()) :
		                     cloudStormManager.VolcanicOnly() ? csVolShadowVolcanic.get() :
		                                                        csVolShadowVolume.get();
		context->CSSetShader(shadowShader, nullptr, 0);
		globals::profiler->BeginPass("PhysicalSky::VolumetricShadowVolume");
		if (serialColumns) {
			const uint32_t columnsY = horizontal ? kShadowVolD : kShadowVolH;
			context->Dispatch((kShadowVolW + 7u) / 8u, (columnsY + 7u) / 8u, 1);
		} else if (horizontal) {
			context->Dispatch(kShadowVolW, kShadowVolD, 1);
		} else {
			constexpr uint32_t columnsPerGroup = kShadowVolW / kShadowVolD;
			static_assert(kShadowVolW % kShadowVolD == 0);
			context->Dispatch(kShadowVolW / columnsPerGroup, kShadowVolH, 1);
		}
		ID3D11UnorderedAccessView* nullUav = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
		auto* rawShadow = texShadowVolumeRaw->srv.get();
		context->CSSetShaderResources(14, 1, &rawShadow);
		auto* filteredShadow = texShadowVolume->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &filteredShadow, nullptr);
		context->CSSetShader(csVolShadowResample.get(), nullptr, 0);
		context->Dispatch((kShadowVolW + 7u) / 8u, (kShadowVolH + 7u) / 8u, (kShadowVolD + 3u) / 4u);
		context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
		context->CSSetShaderResources(14, 1, &nullPsShadowSrv);
		globals::profiler->EndPass();

		context->CSSetShaderResources(69, 1, &nullPsShadowSrv);
		context->PSSetShaderResources(69, 1, &nullPsShadowSrv);
		auto* shadowInput = texShadowVolume->srv.get();
		context->CSSetShaderResources(14, 1, &shadowInput);
		auto* meanOutput = texCloudShadowMean->uav.get();
		const bool buildAPShadow = globals::features::viewAtmosphere.IsPrepared() && !voxelActive && cloudData.stormCount == 0u &&
		                           csCloudShadowMeanWeather && csIntegrateProjectedAPShadow && texProjectedAPShadow;
		ID3D11UnorderedAccessView* projectedOutput = buildAPShadow ? texProjectedAPShadow->uav.get() : nullptr;
		ID3D11UnorderedAccessView* shadowOutputs[] = { meanOutput, projectedOutput };
		context->CSSetUnorderedAccessViews(0, 2, shadowOutputs, nullptr);
		context->CSSetShader(voxelActive ? csCloudShadowMeanVoxel.get() : (cloudData.stormCount == 0u ? csCloudShadowMeanWeather.get() : (cloudStormManager.VolcanicOnly() ? csCloudShadowMeanVolcanic.get() : csCloudShadowMean.get())), nullptr, 0);
		globals::profiler->BeginPass("PhysicalSky::CloudShadowMean");
		context->Dispatch(1u + kProjectedShadowWidth / 64u, kShadowMeanHeights, 1);
		globals::profiler->EndPass();
		ID3D11UnorderedAccessView* nullShadowOutputs[2]{};
		context->CSSetUnorderedAccessViews(0, 2, nullShadowOutputs, nullptr);
		if (buildAPShadow) {
			globals::profiler->BeginPass("PhysicalSky::IntegrateProjectedAPShadow");
			context->CSSetUnorderedAccessViews(1, 1, &projectedOutput, nullptr);
			context->CSSetShader(csIntegrateProjectedAPShadow.get(), nullptr, 0);
			context->Dispatch((kProjectedShadowWidth + 7u) / 8u, (kShadowMeanHeights + 7u) / 8u, 1);
			context->CSSetUnorderedAccessViews(1, 1, &nullUav, nullptr);
			globals::profiler->EndPass();
			projectedAPShadowFrame = state->frameCount;
		}
		context->CSSetShaderResources(14, 1, &nullPsShadowSrv);
		state->EndPerfEvent();
	}

	if (a_pass == VolumetricCloudPass::kMainViewAndCubemap) {
		context->CSSetShaderResources(1, (uint)srvs.size(), srvs.data());
		ID3D11UnorderedAccessView* nullBoundsUav = nullptr;
		ID3D11ShaderResourceView* noMiddleBounds = nullptr;
		context->CSSetShaderResources(105, 1, &noMiddleBounds);
		if (cloudData.middleLayer.z > 0.f && csMiddleBounds && csMiddleBoundsReduce) {
			if (middleBoundsDirty) {
				globals::profiler->BeginPass("PhysicalSky::MiddleCloudBounds");
				auto* output = middleBoundsUavs[0].get();
				context->CSSetUnorderedAccessViews(5, 1, &output, nullptr);
				context->CSSetShader(csMiddleBounds.get(), nullptr, 0);
				context->Dispatch(kMiddleBoundsSize, kMiddleBoundsSize, 1);
				ID3D11UnorderedAccessView* noOutput = nullptr;
				context->CSSetUnorderedAccessViews(5, 1, &noOutput, nullptr);
				context->CSSetShader(csMiddleBoundsReduce.get(), nullptr, 0);
				for (uint32_t mip = 1; mip < kMiddleBoundsLevels; ++mip) {
					auto* input = middleBoundsSrvs[mip - 1].get();
					output = middleBoundsUavs[mip].get();
					context->CSSetShaderResources(105, 1, &input);
					context->CSSetUnorderedAccessViews(5, 1, &output, nullptr);
					const auto groups = ((kMiddleBoundsSize >> mip) + 7u) / 8u;
					context->Dispatch(groups, groups, 1);
					context->CSSetUnorderedAccessViews(5, 1, &noOutput, nullptr);
					context->CSSetShaderResources(105, 1, &noMiddleBounds);
				}
				globals::profiler->EndPass();
				middleBoundsDirty = false;
			}
			auto* input = texMiddleBounds->srv.get();
			context->CSSetShaderResources(105, 1, &input);
		}
		if (cloudData.middleLayer.z > 0.f || renderCirrus) {
			ID3D11ShaderResourceView* noSunInput = nullptr;
			context->CSSetShaderResources(100, 1, &noSunInput);
			auto* output = texUpperSunDepth->uav.get();
			context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
			context->CSSetShader(csUpperSunDepth.get(), nullptr, 0);
			globals::profiler->BeginPass("PhysicalSky::UpperSunDepth");
			context->Dispatch(32, 32, 1);
			globals::profiler->EndPass();
			context->CSSetUnorderedAccessViews(0, 1, &nullBoundsUav, nullptr);
			auto* input = texUpperSunDepth->srv.get();
			context->CSSetShaderResources(100, 1, &input);
		}

		CloudAmbientVolumeCB volumeData{ .phase = lowCloudPhase.AmbientPhase(cloudData.msAlbedo) };
		cloudAmbientVolumeCB->Update(volumeData);
		auto* ambientConstants = cloudAmbientVolumeCB->CB();
		context->CSSetConstantBuffers(2, 1, &ambientConstants);

		globals::features::viewAtmosphere.Bind(true);
		ID3D11UnorderedAccessView* nullUavs[3] = {};
		ID3D11ShaderResourceView* nullTemporalSrvs[6] = {};
		ID3D11ShaderResourceView* nullOutputs[2] = {};
		context->PSSetShaderResources(110, 2, nullOutputs);
		context->PSSetShaderResources(114, 2, nullOutputs);
		context->PSSetShaderResources(116, 1, nullOutputs);
		context->PSSetShaderResources(113, 1, nullOutputs);
		context->PSSetShaderResources(109, 1, nullOutputs);
		texVolCubeTr.swap(texVolCubeHistoryTr);
		const auto voxelRect = voxelCloudManager.ScreenRect(cloudData.bottomZ, lowW, lowH);
		const bool voxelVisible = voxelRect[2] > voxelRect[0] && voxelRect[3] > voxelRect[1] && !state->isMapMenuOpen;
		texVolCubeLum.swap(texVolCubeHistoryLum);
		texVolCubeAux.swap(texVolCubeHistoryAux);
		texLightningCubeMask.swap(texLightningCubeHistory);
		context->CSSetShaderResources(1, (uint)srvs.size(), srvs.data());
		context->CSSetShaderResources(13, 1, &ambientShSrv);
		lightning.QuerySource(csLightningQuery.get());
		std::array<ID3D11UnorderedAccessView*, 3> uavs = {};
		if (!state->isMapMenuOpen) {
			const auto& weather = weatherViewShaders;
			const bool qualitySteps = !settings.usePerformanceCloudSteps;
			auto* boundsShader = qualitySteps && weather.boundsQuality ? weather.boundsQuality.get() : weather.bounds.get();
			const bool useWeatherBounds = boundsShader && weatherMap.HasProceduralConvection() && UpdateWeatherCloudBounds(cloudData);
			auto& terrainShadows = globals::features::terrainShadows;
			auto& cloudShadows = globals::features::cloudShadows;
			auto* cloudShadowLayer = cloudShadows.loaded ? cloudShadows.texCloudShadowLayers[CloudShadows::kMaxCloudLayers - 1] : nullptr;
			std::array<ID3D11ShaderResourceView*, 2> apShadowSrvs = {
				terrainShadows.IsShadowMapReady() ? terrainShadows.texShadowHeight->srv.get() : nullptr,
				cloudShadowLayer ? cloudShadowLayer->srv.get() : nullptr
			};
			ID3D11ShaderResourceView* directionalShadowLights = globals::deferred->directionalShadowLights ?
			                                                        globals::deferred->directionalShadowLights->srv.get() :
			                                                        nullptr;
			auto* volumeShadow = texShadowVolume->srv.get();
			context->CSSetShaderResources(14, 1, &volumeShadow);
			context->CSSetShaderResources(36, (uint)apShadowSrvs.size(), apShadowSrvs.data());
			context->CSSetShaderResources(98, 1, &directionalShadowLights);
			globals::features::viewAtmosphere.Bind(true);
			texVolTr.swap(texVolHistoryTr);
			texVolLum.swap(texVolHistoryLum);
			texVolAux.swap(texVolHistoryAux);
			texLightningMask.swap(texLightningHistory);
			uavs = { texVolLowTr->uav.get(), texVolLowLum->uav.get(), texVolLowAux->uav.get() };
			context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
			auto* lightningOutput = texLightningTrace->uav.get();
			context->CSSetUnorderedAccessViews(3, 1, &lightningOutput, nullptr);
			auto* stormMotionOutput = texVolStormMotion->uav.get();
			context->CSSetUnorderedAccessViews(6, 1, &stormMotionOutput, nullptr);
			auto stormRect = cloudStormManager.ScreenRect(cloudData.bottomZ, lowW, lowH,
				{ cloudData.lowCloudBaseAltitude, cloudData.lowCloudTopAltitude }, cloudData.cloudCurvatureRadius);
			const bool stormVisible = stormActive && stormRect[2] > stormRect[0] && stormRect[3] > stormRect[1];
			cloudDispatchCb->Update(stormRect);
			auto* dispatchConstants = cloudDispatchCb->CB();
			context->CSSetConstantBuffers(3, 1, &dispatchConstants);
			globals::profiler->BeginPass("PhysicalSky::VolumetricMainView");
			if (stormVisible) {
				auto* tilesOutput = texStormTiles->uav.get();
				context->CSSetUnorderedAccessViews(7, 1, &tilesOutput, nullptr);
				context->CSSetShader(csStormTiles.get(), nullptr, 0);
				context->Dispatch((lowW + 63u) / 64u, (lowH + 63u) / 64u, 1);
				context->CSSetUnorderedAccessViews(7, 1, nullUavs, nullptr);
				auto* tilesInput = texStormTiles->srv.get();
				context->CSSetShaderResources(88, 1, &tilesInput);
			}
			// Ordinary pixels retain the small, storm-free shader even while a storm
			// is visible. The two dispatches write disjoint pixels of the same outputs.
			auto* weatherShader = weather.general.get();
			// The generator guarantees zero anvil coverage when convection and painted maps are inactive.
			if (!hasConvection) {
				if (auto* baseOnly = qualitySteps && weather.baseOnlyQuality ? weather.baseOnlyQuality.get() : weather.baseOnly.get())
					weatherShader = baseOnly;
			} else if (useWeatherBounds) {
				weatherShader = boundsShader;
				auto* bounds = texWeatherCloudBounds->srv.get();
				context->CSSetShaderResources(kWeatherCloudBoundsSrvSlot, 1, &bounds);
			}
			context->CSSetShader(weatherShader, nullptr, 0);
			context->Dispatch((lowW + 7u) / 8u, (lowH + 3u) / 4u, 1);
			if (useWeatherBounds)
				context->CSSetShaderResources(kWeatherCloudBoundsSrvSlot, 1, nullOutputs);
			if (stormVisible) {
				const bool volcanic = cloudStormManager.VolcanicOnly() && csVolMainVolcanic;
				auto* singleConstants = volcanic && csVolMainSingleVolcanic ? cloudStormManager.SingleData() : nullptr;
				winrt::com_ptr<ID3D11Buffer> previousConstants;
				if (singleConstants) {
					context->CSGetConstantBuffers(0, 1, previousConstants.put());
					context->CSSetConstantBuffers(0, 1, &singleConstants);
				}
				context->CSSetShader(singleConstants ? csVolMainSingleVolcanic.get() : (volcanic ? csVolMainVolcanic.get() : csVolMainView.get()), nullptr, 0);
				const uint32_t groupWidth = volcanic ? 16u : 8u;
				const uint32_t groupHeight = volcanic && !singleConstants ? 4u : 8u;
				context->Dispatch((stormRect[2] - stormRect[0] + groupWidth - 1u) / groupWidth,
					(stormRect[3] - stormRect[1] + groupHeight - 1u) / groupHeight, 1);
				if (singleConstants) {
					auto* previous = previousConstants.get();
					context->CSSetConstantBuffers(0, 1, &previous);
				}
			}
			globals::profiler->EndPass();
			context->CSSetShaderResources(88, 1, nullOutputs);
			stormMotionOutput = nullptr;
			context->CSSetUnorderedAccessViews(6, 1, &stormMotionOutput, nullptr);
			dispatchConstants = nullptr;
			context->CSSetConstantBuffers(3, 1, &dispatchConstants);
			context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
			if (voxelVisible) {
				cloudDispatchCb->Update(voxelRect);
				dispatchConstants = cloudDispatchCb->CB();
				context->CSSetConstantBuffers(3, 1, &dispatchConstants);
				// Weather and voxel clouds sample the same checkerboard phase. Combine their
				// low-resolution integrals before the shared temporal reconstruction.
				ID3D11UnorderedAccessView* nullLightning = nullptr;
				context->CSSetUnorderedAccessViews(3, 1, &nullLightning, nullptr);
				std::array<ID3D11ShaderResourceView*, 3> weatherInputs = { texVolLowTr->srv.get(), texVolLowLum->srv.get(), texVolLowAux->srv.get() };
				context->CSSetShaderResources(20, (uint)weatherInputs.size(), weatherInputs.data());
				auto* motionInput = texVolStormMotion->srv.get();
				context->CSSetShaderResources(58, 1, &motionInput);
				auto* maskInput = texLightningTrace->srv.get();
				context->CSSetShaderResources(90, 1, &maskInput);
				std::array<ID3D11UnorderedAccessView*, 4> voxelOutputs = { texVoxelTraceTr->uav.get(), texVoxelTraceLum->uav.get(), texVoxelTraceAux->uav.get(), texVoxelTraceLightning->uav.get() };
				context->CSSetUnorderedAccessViews(0, (uint)voxelOutputs.size(), voxelOutputs.data(), nullptr);
				auto* motionOutput = texVoxelTraceMotion->uav.get();
				context->CSSetUnorderedAccessViews(6, 1, &motionOutput, nullptr);
				context->CSSetShader(voxelCloudManager.Count() == 1 && csVoxelViewSingle ? csVoxelViewSingle.get() : csVoxelView.get(), nullptr, 0);
				globals::profiler->BeginPass("PhysicalSky::VoxelMainView");
				// Outside the voxel rectangle the shader copies the weather sample.
				context->Dispatch((lowW + 7u) / 8u, (lowH + 7u) / 8u, 1);
				globals::profiler->EndPass();
				context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
				context->CSSetUnorderedAccessViews(3, 1, &nullLightning, nullptr);
				motionOutput = nullptr;
				context->CSSetUnorderedAccessViews(6, 1, &motionOutput, nullptr);
				context->CSSetShaderResources(20, 3, nullTemporalSrvs);
				context->CSSetShaderResources(58, 1, nullOutputs);
				context->CSSetShaderResources(90, 1, nullOutputs);
				texVolLowTr.swap(texVoxelTraceTr);
				texVolLowLum.swap(texVoxelTraceLum);
				texVolLowAux.swap(texVoxelTraceAux);
				texVolStormMotion.swap(texVoxelTraceMotion);
				texLightningTrace.swap(texVoxelTraceLightning);
				dispatchConstants = nullptr;
				context->CSSetConstantBuffers(3, 1, &dispatchConstants);
			}
			if (cloudData.middleLayer.z > 0.f || renderCirrus) {
				// Reuse the free composition targets after the voxel pass. Upper
				// layers retain their own march and share only temporal reconstruction.
				ID3D11UnorderedAccessView* nullLightning = nullptr;
				context->CSSetUnorderedAccessViews(3, 1, &nullLightning, nullptr);
				std::array<ID3D11ShaderResourceView*, 3> weatherInputs = { texVolLowTr->srv.get(), texVolLowLum->srv.get(), texVolLowAux->srv.get() };
				context->CSSetShaderResources(20, (uint)weatherInputs.size(), weatherInputs.data());
				auto* motionInput = texVolStormMotion->srv.get();
				context->CSSetShaderResources(58, 1, &motionInput);
				auto* maskInput = texLightningTrace->srv.get();
				context->CSSetShaderResources(90, 1, &maskInput);
				std::array<ID3D11UnorderedAccessView*, 4> voxelOutputs = { texVoxelTraceTr->uav.get(), texVoxelTraceLum->uav.get(), texVoxelTraceAux->uav.get(), texVoxelTraceLightning->uav.get() };
				context->CSSetUnorderedAccessViews(0, (uint)voxelOutputs.size(), voxelOutputs.data(), nullptr);
				auto* motionOutput = texVoxelTraceMotion->uav.get();
				context->CSSetUnorderedAccessViews(6, 1, &motionOutput, nullptr);
				globals::profiler->BeginPass("PhysicalSky::UpperCloudMainView");
				// Below the middle layer, opaque foreground can hide whole tiles.
				// Cirrus-only and inside/above-layer views keep the direct dispatch.
				const bool classify = cloudData.middleLayer.z > 0.f &&
				                      cameraPosition.z - cloudData.bottomZ < cloudData.middleLayer.x && csUpperTiles && csUpperViewTiles;
				if (classify) {
					auto* tileOutput = texUpperTiles->uav.get();
					context->CSSetUnorderedAccessViews(4, 1, &tileOutput, nullptr);
					context->CSSetShader(csUpperTiles.get(), nullptr, 0);
					context->Dispatch((lowW + 7u) / 8u, (lowH + 7u) / 8u, 1);
					tileOutput = nullptr;
					context->CSSetUnorderedAccessViews(4, 1, &tileOutput, nullptr);
					auto* tileInput = texUpperTiles->srv.get();
					context->CSSetShaderResources(87, 1, &tileInput);
				}
				context->CSSetShader(classify ? csUpperViewTiles.get() : csUpperView.get(), nullptr, 0);
				// All pixels use the same phase as the low-cloud trace.
				context->Dispatch((lowW + 7u) / 8u, (lowH + 7u) / 8u, 1);
				globals::profiler->EndPass();
				context->CSSetShaderResources(87, 1, nullOutputs);
				context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
				context->CSSetUnorderedAccessViews(3, 1, &nullLightning, nullptr);
				motionOutput = nullptr;
				context->CSSetUnorderedAccessViews(6, 1, &motionOutput, nullptr);
				context->CSSetShaderResources(20, 3, nullTemporalSrvs);
				context->CSSetShaderResources(58, 1, nullOutputs);
				context->CSSetShaderResources(90, 1, nullOutputs);
				texVolLowTr.swap(texVoxelTraceTr);
				texVolLowLum.swap(texVoxelTraceLum);
				texVolLowAux.swap(texVoxelTraceAux);
				texVolStormMotion.swap(texVoxelTraceMotion);
				texLightningTrace.swap(texVoxelTraceLightning);
				dispatchConstants = nullptr;
				context->CSSetConstantBuffers(3, 1, &dispatchConstants);
			}
			context->CSSetShaderResources(14, 1, nullOutputs);
			context->CSSetShaderResources(36, (uint)apShadowSrvs.size(), nullTemporalSrvs);
			context->CSSetShaderResources(98, 1, nullOutputs);

			std::array<ID3D11ShaderResourceView*, 3> historySrvs = {
				texVolHistoryTr->srv.get(), texVolHistoryLum->srv.get(), texVolHistoryAux->srv.get()
			};
			std::array<ID3D11ShaderResourceView*, 3> traceSrvs = {
				texVolLowTr->srv.get(), texVolLowLum->srv.get(), texVolLowAux->srv.get()
			};
			context->CSSetShaderResources(17, (uint)historySrvs.size(), historySrvs.data());
			ID3D11UnorderedAccessView* nullLightning = nullptr;
			context->CSSetUnorderedAccessViews(3, 1, &nullLightning, nullptr);
			ID3D11ShaderResourceView* lightningInputs[] = { texLightningTrace->srv.get(), texLightningHistory->srv.get() };
			context->CSSetShaderResources(90, 2, lightningInputs);
			lightningOutput = texLightningMask->uav.get();
			context->CSSetUnorderedAccessViews(3, 1, &lightningOutput, nullptr);
			context->CSSetShaderResources(20, (uint)traceSrvs.size(), traceSrvs.data());
			uavs = { texVolTr->uav.get(), texVolLum->uav.get(), texVolAux->uav.get() };
			context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
			auto* stormMotionInput = texVolStormMotion->srv.get();
			context->CSSetShaderResources(58, 1, &stormMotionInput);
			context->CSSetShader(settings.cloudFastReconstruction ? csVolReprojectFast.get() : csVolReproject.get(), nullptr, 0);
			globals::profiler->BeginPass("PhysicalSky::VolumetricReproject");
			context->Dispatch((renderW + 7u) >> 3, (renderH + 7u) >> 3, 1);
			globals::profiler->EndPass();
			context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
			context->CSSetShaderResources(17, (uint)historySrvs.size(), nullTemporalSrvs);
			context->CSSetShaderResources(20, (uint)traceSrvs.size(), nullTemporalSrvs);
			context->CSSetShaderResources(58, 1, nullTemporalSrvs);

			if (blurRadius == 0) {
				// Forward draws already hold these output identities. Keep them and
				// the unfiltered history separate without dispatching a blur shader.
				context->CopyResource(texVolFilteredTr->resource.get(), texVolTr->resource.get());
				context->CopyResource(texVolFilteredLum->resource.get(), texVolLum->resource.get());
				context->CopyResource(texVolFilteredAux->resource.get(), texVolAux->resource.get());
			} else {
				std::array<ID3D11ShaderResourceView*, 3> resultSrvs = {
					texVolTr->srv.get(), texVolLum->srv.get(), texVolAux->srv.get()
				};
				context->CSSetShaderResources(29, (uint)resultSrvs.size(), resultSrvs.data());
				std::array<ID3D11UnorderedAccessView*, 3> filteredUavs = {
					texVolFilteredTr->uav.get(), texVolFilteredLum->uav.get(), texVolFilteredAux->uav.get()
				};
				context->CSSetUnorderedAccessViews(3, (uint)filteredUavs.size(), filteredUavs.data(), nullptr);
				auto* filter = blurRadius == 1 ? csVolFilter.get() :
				                                 (settings.cloudFastReconstruction ? csVolFilterWideFast.get() : csVolFilterWide.get());
				context->CSSetShader(filter, nullptr, 0);
				globals::profiler->BeginPass("PhysicalSky::VolumetricFilter");
				context->Dispatch((renderW + 7u) >> 3, (renderH + 7u) >> 3, 1);
				globals::profiler->EndPass();
				context->CSSetUnorderedAccessViews(3, 3, nullUavs, nullptr);
				context->CSSetShaderResources(29, (uint)resultSrvs.size(), nullTemporalSrvs);
			}
		} else {
			context->ClearUnorderedAccessViewFloat(texVolTr->uav.get(), kClearTransmittance);
			context->ClearUnorderedAccessViewFloat(texVolLum->uav.get(), kClearLuminance);
			context->ClearUnorderedAccessViewFloat(texVolAux->uav.get(), kClearLuminance);
			context->ClearUnorderedAccessViewFloat(texVolFilteredTr->uav.get(), kClearTransmittance);
			context->ClearUnorderedAccessViewFloat(texVolFilteredLum->uav.get(), kClearLuminance);
			context->ClearUnorderedAccessViewFloat(texVolFilteredAux->uav.get(), kClearLuminance);
		}
		outputSrvs[0] = texVolFilteredTr->srv.get();
		outputSrvs[1] = texVolFilteredLum->srv.get();
		if (!state->isMapMenuOpen && cbData.lightningColorIntensity.w > 0.f) {
			ID3D11ShaderResourceView* inputs[] = { lightning.Segments(), texLightningMask->srv.get() };
			context->CSSetShaderResources(94, 2, inputs);
			auto* filteredAux = texVolFilteredAux->srv.get();
			context->CSSetShaderResources(31, 1, &filteredAux);
			ID3D11UnorderedAccessView* outputs[] = { texLightningChannels->uav.get(), texVolFilteredLum->uav.get() };
			context->CSSetUnorderedAccessViews(4, 2, outputs, nullptr);
			context->CSSetShader(csLightningComposite.get(), nullptr, 0);
			globals::profiler->BeginPass("PhysicalSky::LightningChannels");
			context->Dispatch((renderW + 7u) / 8u, (renderH + 7u) / 8u, 1);
			globals::profiler->EndPass();
			context->CSSetUnorderedAccessViews(4, 2, nullUavs, nullptr);
			context->CSSetShaderResources(94, 2, nullTemporalSrvs);
			context->CSSetShaderResources(31, 1, nullTemporalSrvs);
		}

		uavs = { texVolCubeTraceTr->uav.get(), texVolCubeTraceLum->uav.get(), texVolCubeTraceAux->uav.get() };
		auto* cubeLightningOutput = texLightningCubeTrace->uav.get();
		context->CSSetUnorderedAccessViews(3, 1, &cubeLightningOutput, nullptr);
		auto* stormMotionOutput = texVolCubeStormMotion->uav.get();
		context->CSSetUnorderedAccessViews(7, 1, &stormMotionOutput, nullptr);
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(voxelActive ? csVolCubemapVoxel.get() : (cloudStormManager.VolcanicOnly() && csVolCubemapVolcanic ? csVolCubemapVolcanic.get() : csVolCubemap.get()), nullptr, 0);
		globals::profiler->BeginPass("PhysicalSky::VolumetricCubemap");
		context->Dispatch(kVolCubeSize / 4u, kVolCubeSize / 4u, 6);
		globals::profiler->EndPass();
		stormMotionOutput = nullptr;
		context->CSSetUnorderedAccessViews(7, 1, &stormMotionOutput, nullptr);
		context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
		std::array<ID3D11ShaderResourceView*, 6> cubeSrvs = {
			texVolCubeHistoryTr->srv.get(), texVolCubeHistoryLum->srv.get(), texVolCubeHistoryAux->srv.get(),
			texVolCubeTraceTr->srv.get(), texVolCubeTraceLum->srv.get(), texVolCubeTraceAux->srv.get()
		};
		context->CSSetShaderResources(23, (uint)cubeSrvs.size(), cubeSrvs.data());
		uavs = { texVolCubeTr->uav.get(), texVolCubeLum->uav.get(), texVolCubeAux->uav.get() };
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		auto* stormMotionInput = texVolCubeStormMotion->srv.get();
		context->CSSetShaderResources(59, 1, &stormMotionInput);
		context->CSSetShader(csVolCubeReproject.get(), nullptr, 0);
		ID3D11UnorderedAccessView* nullLightning = nullptr;
		context->CSSetUnorderedAccessViews(3, 1, &nullLightning, nullptr);
		ID3D11ShaderResourceView* lightningCubeInputs[] = { texLightningCubeTrace->srv.get(), texLightningCubeHistory->srv.get() };
		context->CSSetShaderResources(92, 2, lightningCubeInputs);
		cubeLightningOutput = texLightningCubeMask->uav.get();
		context->CSSetUnorderedAccessViews(3, 1, &cubeLightningOutput, nullptr);
		globals::profiler->BeginPass("PhysicalSky::VolumetricCubeReproject");
		context->Dispatch((kVolCubeSize + 7u) >> 3, (kVolCubeSize + 7u) >> 3, 6);
		globals::profiler->EndPass();
		context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
		context->CSSetShaderResources(23, (uint)cubeSrvs.size(), nullTemporalSrvs);
		context->CSSetShaderResources(59, 1, nullTemporalSrvs);
		context->CSSetUnorderedAccessViews(3, 1, &nullLightning, nullptr);
		if (lightning.HasSource()) {
			auto* mask = texLightningCubeMask->srv.get();
			auto* channel = lightning.Segments();
			context->CSSetShaderResources(94, 1, &channel);
			context->CSSetShaderResources(95, 1, &mask);
			auto* output = texLightningCubeLum->uav.get();
			context->CSSetUnorderedAccessViews(5, 1, &output, nullptr);
			context->CSSetShader(csLightningCubeComposite.get(), nullptr, 0);
			globals::profiler->BeginPass("PhysicalSky::LightningReflections");
			context->Dispatch((kVolCubeSize + 7u) / 8u, (kVolCubeSize + 7u) / 8u, 6);
			globals::profiler->EndPass();
			context->CSSetUnorderedAccessViews(5, 1, &nullLightning, nullptr);
		}
		lightning.CloudFrameComplete(volMainHistoryValid && !state->isMapMenuOpen);
		volMainHistoryValid = true;
		volHistoryWind = volWind;
		cloudStormManager.CommitMotion();
		volHistoryViewProj = globals::game::frameBufferCached.GetCameraViewProj();
		volHistoryCamera = { cameraPosition.x, cameraPosition.y, cameraPosition.z };
		volHistoryCurvatureRadius = cloudCurvatureRadius;
		volHistoryFrameDim = state->isMapMenuOpen ? float2{} : frameDim;
		++volFrameIndex;
	}

	// Cleanup
	{
		ID3D11ShaderResourceView* nullSrvs[86] = {};
		ID3D11UnorderedAccessView* nullUavs[3] = {};
		ID3D11Buffer* nullCb[1] = {};
		ID3D11SamplerState* nullSamplers[3] = {};
		context->CSSetShaderResources(0, ARRAYSIZE(nullSrvs), nullSrvs);
		context->CSSetShaderResources(90, 7, nullSrvs);
		context->CSSetShaderResources(kWeatherSunSrvSlot, 1, nullSrvs);
		context->CSSetShaderResources(99, 1, nullSrvs);
		context->CSSetShaderResources(100, 1, nullSrvs);
		context->CSSetShaderResources(101, 3, nullSrvs);
		context->CSSetShaderResources(105, 1, nullSrvs);
		context->CSSetShaderResources(kWeatherShapeSrvSlot, 2, nullSrvs);
		context->CSSetShaderResources(kWeatherCurlSrvSlot, 1, nullSrvs);
		context->CSSetShaderResources(kFiniteCloudWeatherSrvSlot, 1, nullSrvs);
		context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
		context->CSSetUnorderedAccessViews(3, 3, nullUavs, nullptr);
		context->CSSetConstantBuffers(1, 1, nullCb);
		context->CSSetConstantBuffers(2, 1, nullCb);
		context->CSSetSamplers(2, 3, nullSamplers);
		context->CSSetShader(nullptr, nullptr, 0);

		// Make results available to any later pixel-shader passes that sample PhysicalSky/Common.hlsli.
		context->PSSetShaderResources(110, (uint)outputSrvs.size(), outputSrvs.data());
		auto* channels = texLightningChannels->srv.get();
		auto* reflection = texLightningCubeLum->srv.get();
		context->PSSetShaderResources(113, 1, &channels);
		context->PSSetShaderResources(109, 1, &reflection);
	}
}

ID3D11ShaderResourceView* PhysicalSky::GetDebugCubeFaceSrv(Texture2D* a_texture)
{
	if (!a_texture || !a_texture->resource)
		return nullptr;

	ID3D11Resource* resource = a_texture->resource.get();
	if (auto it = debugCubeFaceSrvs.find(resource); it != debugCubeFaceSrvs.end())
		return it->second.get();
	if (debugCubeFace < 0 || static_cast<uint32_t>(debugCubeFace) >= a_texture->desc.ArraySize)
		return nullptr;

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
		.Format = a_texture->desc.Format,
		.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY,
		.Texture2DArray = { .MostDetailedMip = 0, .MipLevels = 1, .FirstArraySlice = static_cast<UINT>(debugCubeFace), .ArraySize = 1 }
	};

	winrt::com_ptr<ID3D11ShaderResourceView> srv;
	if (FAILED(globals::d3d::device->CreateShaderResourceView(resource, &srvDesc, srv.put())))
		return nullptr;

	return debugCubeFaceSrvs.emplace(resource, std::move(srv)).first->second.get();
}

void PhysicalSky::DrawDebugCube(Texture2D* a_texture, const char* a_label, float a_scale)
{
	if (!a_texture || !a_texture->srv)
		return;

	if (auto srv = GetDebugCubeFaceSrv(a_texture)) {
		ImGui::BulletText("%s", a_label);
		ImGui::Image(srv, { a_texture->desc.Width * a_scale, a_texture->desc.Height * a_scale });
	}
}

ID3D11ShaderResourceView* PhysicalSky::GetDebugVolumeSliceSrv(ID3D11ShaderResourceView* a_srv, eastl::unique_ptr<Texture2D>& a_target)
{
	if (!a_srv)
		return nullptr;

	winrt::com_ptr<ID3D11Resource> resource;
	a_srv->GetResource(resource.put());
	winrt::com_ptr<ID3D11Texture3D> texture;
	if (!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(texture.put()))))
		return nullptr;

	D3D11_TEXTURE3D_DESC desc;
	texture->GetDesc(&desc);

	if (!a_target || a_target->desc.Width != desc.Width || a_target->desc.Height != desc.Height || a_target->desc.Format != desc.Format) {
		D3D11_TEXTURE2D_DESC sliceDesc = {
			.Width = desc.Width,
			.Height = desc.Height,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = desc.Format,
			.SampleDesc = { .Count = 1, .Quality = 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = sliceDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		a_target = eastl::make_unique<Texture2D>(sliceDesc, "PhysicalSky::DebugVolumeSlice");
		a_target->CreateSRV(srvDesc);
	}

	// A 3D subresource covers every depth slice, so the slice is selected by the box.
	const D3D11_BOX box = { 0, 0, 0, desc.Width, desc.Height, 1 };
	globals::d3d::context->CopySubresourceRegion(a_target->resource.get(), 0, 0, 0, 0, texture.get(), 0, &box);

	return a_target->srv.get();
}

void PhysicalSky::DrawDebugVolume(ID3D11ShaderResourceView* a_srv, const char* a_label, eastl::unique_ptr<Texture2D>& a_target, float a_scale)
{
	if (!a_srv)
		return;

	if (auto srv = GetDebugVolumeSliceSrv(a_srv, a_target)) {
		ImGui::BulletText("%s", a_label);
		ImGui::Image(srv, { a_target->desc.Width * a_scale, a_target->desc.Height * a_scale });
	}
}

#define I18N_KEY_PREFIX "feature.physical_sky."

void PhysicalSky::DrawDebugCloudTexture(ID3D11ShaderResourceView* srv, const char* id, const char* label, float scale, bool scalar)
{
	texturePreview.Draw(srv, id, label, scale, scalar);
}

#undef I18N_KEY_PREFIX
