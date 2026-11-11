#include "ShaderCache.h"
#include "Globals.h"
#include "ShaderFileWatcher.h"
#include "Util.h"

#ifdef DEVBENCH_BRIDGE_ENABLED
#	include <DevBenchAPI.h>
#endif

#include <d3dcompiler.h>

#include "Deferred.h"
#include "Rendering/EnvironmentProbe/Capture.h"
#include "State.h"
#include "Utils/D3D.h"
#include "Utils/FileRename.h"
#include "Utils/DxcShaderStore.h"
#include "Utils/FxcShaderStore.h"
#include "Utils/Logging.h"
#include "Utils/SharedShaderStore.h"

#include <ORGModuleServices/ShaderStore.h>

#include "Features/DrawcallLimitFix.h"
#include "Features/DynamicCubemaps.h"
#include "Features/RenderDoc.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include "Plugin.h"

namespace SIE
{
	class SharedBlob final : public ID3DBlob
	{
	public:
		explicit SharedBlob(ID3DBlob& a_source) :
			bytes(static_cast<const uint8_t*>(a_source.GetBufferPointer()),
				static_cast<const uint8_t*>(a_source.GetBufferPointer()) + a_source.GetBufferSize())
		{}

		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID a_riid, void** a_object) override
		{
			if (!a_object)
				return E_POINTER;
			if (a_riid == __uuidof(IUnknown) || a_riid == __uuidof(ID3DBlob)) {
				*a_object = static_cast<ID3DBlob*>(this);
				AddRef();
				return S_OK;
			}
			*a_object = nullptr;
			return E_NOINTERFACE;
		}

		ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount; }

		ULONG STDMETHODCALLTYPE Release() override
		{
			const ULONG count = --refCount;
			if (count == 0)
				delete this;
			return count;
		}

		LPVOID STDMETHODCALLTYPE GetBufferPointer() override { return bytes.data(); }
		SIZE_T STDMETHODCALLTYPE GetBufferSize() override { return bytes.size(); }

	private:
		std::atomic<ULONG> refCount{ 1 };
		std::vector<uint8_t> bytes;
	};

	namespace SShaderCache
	{
		static void GetShaderDefines(const RE::BSShader&, uint32_t, D3D_SHADER_MACRO*);
		static std::string GetShaderString(ShaderClass, const RE::BSShader&, uint32_t, bool = false);
		/**
		 * @brief Resolve image-space shader descriptor when applicable.
		 *
		 * If @p shader is an image-space shader, attempts to map it to a
		 * runtime image-space descriptor via GetImagespaceShaderDescriptor and
		 * returns true on success. If the shader is not image-space the
		 * function returns true and leaves @p descriptor unchanged. Returns
		 * false only when the shader is image-space and no valid descriptor
		 * could be resolved.
		 *
		 * This helper is used by the shader loading and caching code paths to
		 * determine whether an image-space shader can be loaded or cached. If
		 * this function returns false the caller should skip loading/compiling
		 * and caching that shader.
		 *
		 * @param shader The shader to resolve (may be an image-space shader).
		 * @param[out] descriptor Resolved descriptor for image-space shaders.
		 * @return True if descriptor is valid or not applicable, false on failure.
		 */
		static bool ResolveImageSpaceDescriptor(const RE::BSShader& shader, uint32_t& descriptor);
		/**
		@brief Get the BSShader::Type from the ShaderString
		@param a_key The key generated from GetShaderString
		@return A string with a valid BSShader::Type
		*/
		static std::string GetTypeFromShaderString(const std::string&);
		constexpr const char* VertexShaderProfile = "vs_5_0";
		constexpr const char* PixelShaderProfile = "ps_5_0";
		constexpr const char* ComputeShaderProfile = "cs_5_0";

		static std::wstring GetShaderPath(const std::string_view& name)
		{
			return std::format(L"Data/Shaders/{}.hlsl", std::wstring(name.begin(), name.end()));
		}

		static const char* GetShaderProfile(ShaderClass shaderClass)
		{
			switch (shaderClass) {
			case ShaderClass::Vertex:
				return VertexShaderProfile;
			case ShaderClass::Pixel:
				return PixelShaderProfile;
			case ShaderClass::Compute:
				return ComputeShaderProfile;
			}
			return nullptr;
		}

		uint32_t GetTechnique(uint32_t descriptor)
		{
			return 0x3F & (descriptor >> 24);
		}

		static void GetLightingShaderDefines(uint32_t descriptor, std::span<D3D_SHADER_MACRO> defines)
		{
			static REL::Relocation<void(uint32_t, D3D_SHADER_MACRO*)> VanillaGetLightingShaderDefines(RELOCATION_ID(101631, 108698));
			VanillaGetLightingShaderDefines(descriptor, defines.data());

			size_t lastIndex = std::ranges::find_if(defines, [](const D3D_SHADER_MACRO& macro) { return macro.Name == nullptr; }) - defines.begin();

			if (descriptor & static_cast<uint32_t>(ShaderCache::LightingShaderFlags::Deferred)) {
				defines[lastIndex++] = { "DEFERRED", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::LightingShaderFlags::EnvironmentProbe)) {
				defines[lastIndex++] = { "ENVIRONMENT_PROBE", nullptr };
			}
			if ((descriptor & static_cast<uint32_t>(ShaderCache::LightingShaderFlags::TruePbr)) != 0) {
				defines[lastIndex++] = { "TRUE_PBR", nullptr };
				if ((descriptor & static_cast<uint32_t>(ShaderCache::LightingShaderFlags::AnisoLighting)) != 0) {
					defines[lastIndex++] = { "GLINT", nullptr };
				}
			}
			if ((descriptor & static_cast<uint32_t>(ShaderCache::LightingShaderFlags::LandscapeSeams)) != 0) {
				defines[lastIndex++] = { "LANDSCAPE_SEAMS", nullptr };
			}

			for (auto* feature : Feature::GetFeatureList()) {
				if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::Lighting)) {
					defines[lastIndex++] = { feature->GetShaderDefineName().data(), nullptr };
					auto options = feature->GetShaderDefineOptions();
					if (!options.empty()) {
						for (auto& option : options) {
							const char* definition = option.second.empty() ? nullptr : option.second.data();
							defines[lastIndex++] = { option.first.data(), definition };
						}
					}
				}
			}

			defines[lastIndex] = { nullptr, nullptr };
		}

		static void GetBloodSplaterShaderDefines(uint32_t descriptor, std::span<D3D_SHADER_MACRO> defines)
		{
			size_t lastIndex = 0;
			if (descriptor == static_cast<uint32_t>(ShaderCache::BloodSplatterShaderTechniques::Splatter)) {
				defines[lastIndex++] = { "SPLATTER", nullptr };
			} else if (descriptor == static_cast<uint32_t>(ShaderCache::BloodSplatterShaderTechniques::Flare)) {
				defines[lastIndex++] = { "FLARE", nullptr };
			}

			for (auto* feature : Feature::GetFeatureList()) {
				if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::BloodSplatter)) {
					defines[lastIndex++] = { feature->GetShaderDefineName().data(), nullptr };
					auto options = feature->GetShaderDefineOptions();
					if (!options.empty()) {
						for (auto& option : options) {
							const char* definition = option.second.empty() ? nullptr : option.second.data();
							defines[lastIndex++] = { option.first.data(), definition };
						}
					}
				}
			}

			defines[lastIndex] = { nullptr, nullptr };
		}

		static void GetDistantTreeShaderDefines(uint32_t descriptor, std::span<D3D_SHADER_MACRO> defines)
		{
			const auto technique = descriptor & 1;
			size_t lastIndex = 0;
			if (technique == static_cast<uint32_t>(ShaderCache::DistantTreeShaderTechniques::Depth)) {
				defines[lastIndex++] = { "RENDER_DEPTH", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::DistantTreeShaderFlags::AlphaTest)) {
				defines[lastIndex++] = { "DO_ALPHA_TEST", nullptr };
			}

			if (descriptor & static_cast<uint32_t>(ShaderCache::DistantTreeShaderFlags::Deferred)) {
				defines[lastIndex++] = { "DEFERRED", nullptr };
			}

			for (auto* feature : Feature::GetFeatureList()) {
				if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::DistantTree)) {
					defines[lastIndex++] = { feature->GetShaderDefineName().data(), nullptr };
					auto options = feature->GetShaderDefineOptions();
					if (!options.empty()) {
						for (auto& option : options) {
							const char* definition = option.second.empty() ? nullptr : option.second.data();
							defines[lastIndex++] = { option.first.data(), definition };
						}
					}
				}
			}

			defines[lastIndex] = { nullptr, nullptr };
		}

		static void GetSkyShaderDefines(uint32_t descriptor, std::span<D3D_SHADER_MACRO> defines)
		{
			using enum ShaderCache::SkyShaderTechniques;

			const auto technique = static_cast<ShaderCache::SkyShaderTechniques>(descriptor & 255);
			size_t lastIndex = 0;
			switch (technique) {
			case SunOcclude:
				{
					defines[lastIndex++] = { "OCCLUSION", nullptr };
					break;
				}
			case SunGlare:
				{
					defines[lastIndex++] = { "TEX", nullptr };
					defines[lastIndex++] = { "DITHER", nullptr };
					break;
				}
			case MoonAndStarsMask:
				{
					defines[lastIndex++] = { "TEX", nullptr };
					defines[lastIndex++] = { "MOONMASK", nullptr };
					break;
				}
			case Stars:
				{
					defines[lastIndex++] = { "HORIZFADE", nullptr };
					break;
				}
			case Clouds:
				{
					defines[lastIndex++] = { "TEX", nullptr };
					defines[lastIndex++] = { "CLOUDS", nullptr };
					break;
				}
			case CloudsLerp:
				{
					defines[lastIndex++] = { "TEX", nullptr };
					defines[lastIndex++] = { "CLOUDS", nullptr };
					defines[lastIndex++] = { "TEXLERP", nullptr };
					break;
				}
			case CloudsFade:
				{
					defines[lastIndex++] = { "TEX", nullptr };
					defines[lastIndex++] = { "CLOUDS", nullptr };
					defines[lastIndex++] = { "TEXFADE", nullptr };
					break;
				}
			case Texture:
				{
					defines[lastIndex++] = { "TEX", nullptr };
					break;
				}
			case Sky:
				{
					defines[lastIndex++] = { "DITHER", nullptr };
					break;
				}
			}

			uint32_t flags = descriptor >> 8;

			if (flags) {
				defines[lastIndex++] = { "DEFERRED", nullptr };
			}

			for (auto* feature : Feature::GetFeatureList()) {
				if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::Sky)) {
					defines[lastIndex++] = { feature->GetShaderDefineName().data(), nullptr };
					auto options = feature->GetShaderDefineOptions();
					if (!options.empty()) {
						for (auto& option : options) {
							const char* definition = option.second.empty() ? nullptr : option.second.data();
							defines[lastIndex++] = { option.first.data(), definition };
						}
					}
				}
			}

			defines[lastIndex] = { nullptr, nullptr };
		}

		static void GetGrassShaderDefines(uint32_t descriptor, std::span<D3D_SHADER_MACRO> defines)
		{
			const auto technique = descriptor & 0b1111;
			size_t lastIndex = 0;
			if (technique == static_cast<uint32_t>(ShaderCache::GrassShaderTechniques::RenderDepthStencil) ||
				technique == static_cast<uint32_t>(ShaderCache::GrassShaderTechniques::RenderDepth)) {
				defines[lastIndex++] = { "RENDER_DEPTH", nullptr };
			} else if (technique == static_cast<uint32_t>(ShaderCache::GrassShaderTechniques::TruePbr)) {
				defines[lastIndex++] = { "TRUE_PBR", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::GrassShaderFlags::AlphaTest)) {
				defines[lastIndex++] = { "DO_ALPHA_TEST", nullptr };
			}

			for (auto* feature : Feature::GetFeatureList()) {
				if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::Grass)) {
					defines[lastIndex++] = { feature->GetShaderDefineName().data(), nullptr };
					auto options = feature->GetShaderDefineOptions();
					if (!options.empty()) {
						for (auto& option : options) {
							const char* definition = option.second.empty() ? nullptr : option.second.data();
							defines[lastIndex++] = { option.first.data(), definition };
						}
					}
				}
			}

			defines[lastIndex] = { nullptr, nullptr };
		}

		static void GetParticleShaderDefines(uint32_t descriptor, std::span<D3D_SHADER_MACRO> defines)
		{
			using enum ShaderCache::ParticleShaderTechniques;

			const auto technique = static_cast<ShaderCache::ParticleShaderTechniques>(descriptor);
			size_t lastIndex = 0;
			switch (technique) {
			case ParticlesGryColor:
				{
					defines[lastIndex++] = { "GRAYSCALE_TO_COLOR", nullptr };
					break;
				}
			case ParticlesGryAlpha:
				{
					defines[lastIndex++] = { "GRAYSCALE_TO_ALPHA", nullptr };
					break;
				}
			case ParticlesGryColorAlpha:
				{
					defines[lastIndex++] = { "GRAYSCALE_TO_COLOR", nullptr };
					defines[lastIndex++] = { "GRAYSCALE_TO_ALPHA", nullptr };
					break;
				}
			case EnvCubeSnow:
				{
					defines[lastIndex++] = { "ENVCUBE", nullptr };
					defines[lastIndex++] = { "SNOW", nullptr };
					break;
				}
			case EnvCubeRain:
				{
					defines[lastIndex++] = { "ENVCUBE", nullptr };
					defines[lastIndex++] = { "RAIN", nullptr };
					break;
				}
			}

			for (auto* feature : Feature::GetFeatureList()) {
				if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::Particle)) {
					defines[lastIndex++] = { feature->GetShaderDefineName().data(), nullptr };
					auto options = feature->GetShaderDefineOptions();
					if (!options.empty()) {
						for (auto& option : options) {
							const char* definition = option.second.empty() ? nullptr : option.second.data();
							defines[lastIndex++] = { option.first.data(), definition };
						}
					}
				}
			}

			defines[lastIndex] = { nullptr, nullptr };
		}

		static void GetEffectShaderDefines(uint32_t descriptor, std::span<D3D_SHADER_MACRO> defines)
		{
			size_t lastIndex = 0;

			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Vc)) {
				defines[lastIndex++] = { "VC", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::TexCoord)) {
				defines[lastIndex++] = { "TEXCOORD", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::TexCoordIndex)) {
				defines[lastIndex++] = { "TEXCOORD_INDEX", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Skinned)) {
				defines[lastIndex++] = { "SKINNED", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Normals)) {
				defines[lastIndex++] = { "NORMALS", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::BinormalTangent)) {
				defines[lastIndex++] = { "BINORMAL_TANGENT", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Texture)) {
				defines[lastIndex++] = { "TEXTURE", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::IndexedTexture)) {
				defines[lastIndex++] = { "INDEXED_TEXTURE", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Falloff)) {
				defines[lastIndex++] = { "FALLOFF", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::AddBlend)) {
				defines[lastIndex++] = { "ADDBLEND", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::MultBlend)) {
				defines[lastIndex++] = { "MULTBLEND", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Particles)) {
				defines[lastIndex++] = { "PARTICLES", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::StripParticles)) {
				defines[lastIndex++] = { "STRIP_PARTICLES", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Blood)) {
				defines[lastIndex++] = { "BLOOD", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Membrane)) {
				defines[lastIndex++] = { "MEMBRANE", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Lighting)) {
				defines[lastIndex++] = { "LIGHTING", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::ProjectedUv)) {
				defines[lastIndex++] = { "PROJECTED_UV", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Soft)) {
				defines[lastIndex++] = { "SOFT", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::GrayscaleToColor)) {
				defines[lastIndex++] = { "GRAYSCALE_TO_COLOR", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::GrayscaleToAlpha)) {
				defines[lastIndex++] = { "GRAYSCALE_TO_ALPHA", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::IgnoreTexAlpha)) {
				defines[lastIndex++] = { "IGNORE_TEX_ALPHA", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::MultBlendDecal)) {
				defines[lastIndex++] = { "MULTBLEND_DECAL", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::AlphaTest)) {
				defines[lastIndex++] = { "ALPHA_TEST", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::SkyObject)) {
				defines[lastIndex++] = { "SKY_OBJECT", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::MsnSpuSkinned)) {
				defines[lastIndex++] = { "MSN_SPU_SKINNED", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::MotionVectorsNormals)) {
				defines[lastIndex++] = { "MOTIONVECTORS_NORMALS", nullptr };
			}

			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Deferred)) {
				defines[lastIndex++] = { "DEFERRED", nullptr };
			}

			for (auto* feature : Feature::GetFeatureList()) {
				if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::Effect)) {
					defines[lastIndex++] = { feature->GetShaderDefineName().data(), nullptr };
					auto options = feature->GetShaderDefineOptions();
					if (!options.empty()) {
						for (auto& option : options) {
							const char* definition = option.second.empty() ? nullptr : option.second.data();
							defines[lastIndex++] = { option.first.data(), definition };
						}
					}
				}
			}

			defines[lastIndex] = { nullptr, nullptr };
		}

		static void GetWaterShaderDefines(uint32_t descriptor, std::span<D3D_SHADER_MACRO> defines)
		{
			size_t lastIndex = 0;
			defines[lastIndex++] = { "WATER", nullptr };
			defines[lastIndex++] = { "FOG", nullptr };

			if (descriptor & static_cast<uint32_t>(ShaderCache::WaterShaderFlags::Vc)) {
				defines[lastIndex++] = { "VC", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::WaterShaderFlags::NormalTexCoord)) {
				defines[lastIndex++] = { "NORMAL_TEXCOORD", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::WaterShaderFlags::Reflections)) {
				defines[lastIndex++] = { "REFLECTIONS", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::WaterShaderFlags::Refractions)) {
				defines[lastIndex++] = { "REFRACTIONS", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::WaterShaderFlags::Depth)) {
				defines[lastIndex++] = { "DEPTH", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::WaterShaderFlags::Interior)) {
				defines[lastIndex++] = { "INTERIOR", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::WaterShaderFlags::Wading)) {
				defines[lastIndex++] = { "WADING", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::WaterShaderFlags::VertexAlphaDepth)) {
				defines[lastIndex++] = { "VERTEX_ALPHA_DEPTH", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::WaterShaderFlags::Cubemap)) {
				defines[lastIndex++] = { "CUBEMAP", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::WaterShaderFlags::Flowmap)) {
				defines[lastIndex++] = { "FLOWMAP", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::WaterShaderFlags::BlendNormals)) {
				defines[lastIndex++] = { "BLEND_NORMALS", nullptr };
			}

			const auto technique = (descriptor >> 11) & 0xF;
			if (technique == static_cast<uint32_t>(ShaderCache::WaterShaderTechniques::Underwater)) {
				defines[lastIndex++] = { "UNDERWATER", nullptr };
			} else if (technique == static_cast<uint32_t>(ShaderCache::WaterShaderTechniques::Lod)) {
				defines[lastIndex++] = { "LOD", nullptr };
			} else if (technique == static_cast<uint32_t>(ShaderCache::WaterShaderTechniques::Stencil)) {
				defines[lastIndex++] = { "STENCIL", nullptr };
			} else if (technique == static_cast<uint32_t>(ShaderCache::WaterShaderTechniques::Simple)) {
				defines[lastIndex++] = { "SIMPLE", nullptr };
			} else if (technique < 8) {
				static constexpr std::array<const char*, 8> numLightDefines = { { "0", "1", "2", "3", "4",
					"5", "6", "7" } };
				defines[lastIndex++] = { "SPECULAR", nullptr };
				defines[lastIndex++] = { "NUM_SPECULAR_LIGHTS", numLightDefines[technique] };
			}

			for (auto* feature : Feature::GetFeatureList()) {
				if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::Water)) {
					defines[lastIndex++] = { feature->GetShaderDefineName().data(), nullptr };
					auto options = feature->GetShaderDefineOptions();
					if (!options.empty()) {
						for (auto& option : options) {
							const char* definition = option.second.empty() ? nullptr : option.second.data();
							defines[lastIndex++] = { option.first.data(), definition };
						}
					}
				}
			}

			defines[lastIndex] = { nullptr, nullptr };
		}

		static void GetUtilityShaderDefines(uint32_t descriptor, std::span<D3D_SHADER_MACRO> defines)
		{
			using enum ShaderCache::UtilityShaderFlags;

			size_t lastIndex = 0;
			const auto shadowMaskFlags = static_cast<uint32_t>(RenderShadowmask) |
			                             static_cast<uint32_t>(RenderShadowmaskDpb) |
			                             static_cast<uint32_t>(RenderShadowmaskPb) |
			                             static_cast<uint32_t>(RenderShadowmaskSpot);
			const bool hasShadowMask = (descriptor & shadowMaskFlags) != 0;

			if (descriptor & static_cast<uint32_t>(Vc)) {
				defines[lastIndex++] = { "VC", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(Texture)) {
				defines[lastIndex++] = { "TEXTURE", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(Skinned)) {
				defines[lastIndex++] = { "SKINNED", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(Normals)) {
				defines[lastIndex++] = { "NORMALS", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(AlphaTest)) {
				defines[lastIndex++] = { "ALPHA_TEST", nullptr };
			}

			if (descriptor & static_cast<uint32_t>(LodLandscape)) {
				if (descriptor &
					(static_cast<uint32_t>(RenderShadowmask) |
						static_cast<uint32_t>(RenderShadowmaskSpot))) {
					defines[lastIndex++] = { "FOCUS_SHADOW", nullptr };
				} else {
					defines[lastIndex++] = { "LOD_LANDSCAPE", nullptr };
				}
			}

			if ((descriptor & static_cast<uint32_t>(RenderNormal)) &&
				!(descriptor & static_cast<uint32_t>(RenderNormalClear))) {
				defines[lastIndex++] = { "RENDER_NORMAL", nullptr };

			} else if (!(descriptor & static_cast<uint32_t>(RenderNormal)) &&
					   (descriptor & static_cast<uint32_t>(RenderNormalClear))) {
				defines[lastIndex++] = { "RENDER_NORMAL_CLEAR", nullptr };

			} else if ((descriptor & static_cast<uint32_t>(RenderNormal)) &&
					   (descriptor & static_cast<uint32_t>(RenderNormalClear))) {
				defines[lastIndex++] = { "STENCIL_ABOVE_WATER", nullptr };
			}

			if (descriptor & static_cast<uint32_t>(RenderNormalFalloff)) {
				defines[lastIndex++] = { "RENDER_NORMAL_FALLOFF", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(RenderNormalClamp)) {
				defines[lastIndex++] = { "RENDER_NORMAL_CLAMP", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(RenderDepth)) {
				defines[lastIndex++] = { "RENDER_DEPTH", nullptr };
			}

			if (descriptor & static_cast<uint32_t>(OpaqueEffect)) {
				defines[lastIndex++] = { "OPAQUE_EFFECT", nullptr };

				if (!(descriptor & static_cast<uint32_t>(RenderShadowmap)) &&
					(descriptor & static_cast<uint32_t>(AdditionalAlphaMask))) {
					defines[lastIndex++] = { "ADDITIONAL_ALPHA_MASK", nullptr };
				}
				if (descriptor & static_cast<uint32_t>(GrayscaleToAlpha)) {
					defines[lastIndex++] = { "GRAYSCALE_TO_ALPHA", nullptr };
				}
			} else {
				if (descriptor & static_cast<uint32_t>(RenderShadowmap)) {
					defines[lastIndex++] = { "RENDER_SHADOWMAP", nullptr };

					if (descriptor & static_cast<uint32_t>(RenderShadowmapPb)) {
						defines[lastIndex++] = { "RENDER_SHADOWMAP_PB", nullptr };
					}
				} else if (descriptor &
						   static_cast<uint32_t>(AdditionalAlphaMask)) {
					defines[lastIndex++] = { "ADDITIONAL_ALPHA_MASK", nullptr };
				}
				if (descriptor & static_cast<uint32_t>(RenderShadowmapClamped)) {
					defines[lastIndex++] = { "RENDER_SHADOWMAP_CLAMPED", nullptr };
				}
			}

			if ((descriptor & static_cast<uint32_t>(GrayscaleMask)) &&
				!hasShadowMask) {
				defines[lastIndex++] = { "GRAYSCALE_MASK", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(RenderShadowmask)) {
				defines[lastIndex++] = { "RENDER_SHADOWMASK", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(RenderShadowmaskSpot)) {
				defines[lastIndex++] = { "RENDER_SHADOWMASKSPOT", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(RenderShadowmaskPb)) {
				defines[lastIndex++] = { "RENDER_SHADOWMASKPB", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(RenderShadowmaskDpb)) {
				defines[lastIndex++] = { "RENDER_SHADOWMASKDPB", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(RenderBaseTexture)) {
				defines[lastIndex++] = { "RENDER_BASE_TEXTURE", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(TreeAnim)) {
				defines[lastIndex++] = { "TREE_ANIM", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(LodObject)) {
				defines[lastIndex++] = { "LOD_OBJECT", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(LocalMapFogOfWar)) {
				defines[lastIndex++] = { "LOCALMAP_FOGOFWAR", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(TruePbr)) {
				defines[lastIndex++] = { "TRUE_PBR", nullptr };
			}

			if (hasShadowMask) {
				auto shaderFilter = (descriptor >> 17) & 0xFu;
				switch (shaderFilter) {
				case 0:
				case 1:
				case 2:
				case 4:
				case 8:
					break;
				default:
					logger::error("Unsupported Utility shadow-filter selector {:#x} in descriptor {:#010x}", shaderFilter, descriptor);
					shaderFilter = 0;
					break;
				}

				static constexpr std::array<const char*, 9> shadowFilters = {
					{ "0", "1", "2", nullptr, "4", nullptr, nullptr, nullptr, "8" }
				};
				defines[lastIndex++] = { "SHADOWFILTER", shadowFilters[shaderFilter] };
			} else if ((!(descriptor & static_cast<uint32_t>(OpaqueEffect)) &&
						   (descriptor &
							   static_cast<uint32_t>(RenderShadowmap))) ||
					   (descriptor & static_cast<uint32_t>(RenderDepth))) {
				if (descriptor & static_cast<uint32_t>(DepthWriteDecals)) {
					defines[lastIndex++] = { "DEPTH_WRITE_DECALS", nullptr };
				}
			} else {
				if (descriptor & (static_cast<uint32_t>(DepthWriteDecals) |
									 static_cast<uint32_t>(DebugColor))) {
					defines[lastIndex++] = { "DEBUG_COLOR", nullptr };
				}
				if (descriptor & static_cast<uint32_t>(DebugShadowSplit)) {
					defines[lastIndex++] = { "DEBUG_SHADOWSPLIT", nullptr };
				}
			}

			defines[lastIndex++] = { "SHADOWSPLITCOUNT", "3" };

			if ((descriptor & 0x14000) != 0x14000 &&
				((descriptor & 0x20004000) == 0x4000 || (descriptor & 0x1E02000) == 0x2000) &&
				!(descriptor & 0x80) && (descriptor & 0x14000) != 0x10000) {
				defines[lastIndex++] = { "NO_PIXEL_SHADER", nullptr };
			}

			defines[lastIndex++] = { nullptr, nullptr };
		}

		static void GetImagespaceShaderDefines(const RE::BSShader& shader, std::span<D3D_SHADER_MACRO> defines)
		{
			auto& isShader = const_cast<RE::BSImagespaceShader&>(static_cast<const RE::BSImagespaceShader&>(shader));
			auto* macros = reinterpret_cast<RE::BSImagespaceShader::ShaderMacro*>(defines.data());
			isShader.GetShaderMacros(macros);
			size_t lastIndex = std::ranges::find_if(defines, [](const D3D_SHADER_MACRO& macro) { return macro.Name == nullptr; }) - defines.begin();
			for (auto* feature : Feature::GetFeatureList()) {
				if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::ImageSpace)) {
					defines[lastIndex++] = { feature->GetShaderDefineName().data(), nullptr };
					auto options = feature->GetShaderDefineOptions();
					if (!options.empty()) {
						for (auto& option : options) {
							const char* definition = option.second.empty() ? nullptr : option.second.data();
							defines[lastIndex++] = { option.first.data(), definition };
						}
					}
				}
			}
			defines[lastIndex] = { nullptr, nullptr };
			return;
		}

		static void GetShaderDefines(const RE::BSShader& shader, uint32_t descriptor, std::span<D3D_SHADER_MACRO> defines)
		{
			switch (shader.shaderType.get()) {
			case RE::BSShader::Type::Grass:
				GetGrassShaderDefines(descriptor, defines);
				break;
			case RE::BSShader::Type::Sky:
				GetSkyShaderDefines(descriptor, defines);
				break;
			case RE::BSShader::Type::Water:
				GetWaterShaderDefines(descriptor, defines);
				break;
			case RE::BSShader::Type::BloodSplatter:
				GetBloodSplaterShaderDefines(descriptor, defines);
				break;
			case RE::BSShader::Type::ImageSpace:
				GetImagespaceShaderDefines(shader, defines);
				break;
			case RE::BSShader::Type::Lighting:
				GetLightingShaderDefines(descriptor, defines);
				break;
			case RE::BSShader::Type::DistantTree:
				GetDistantTreeShaderDefines(descriptor, defines);
				break;
			case RE::BSShader::Type::Particle:
				GetParticleShaderDefines(descriptor, defines);
				break;
			case RE::BSShader::Type::Effect:
				GetEffectShaderDefines(descriptor, defines);
				break;
			case RE::BSShader::Type::Utility:
				GetUtilityShaderDefines(descriptor, defines);
				break;
			}
		}

		static std::array<std::array<std::unordered_map<std::string, int32_t>,
							  static_cast<size_t>(ShaderClass::Total)>,
			static_cast<size_t>(RE::BSShader::Type::Total)>
		GetVariableIndices()
		{
			std::array<std::array<std::unordered_map<std::string, int32_t>,
						   static_cast<size_t>(ShaderClass::Total)>,
				static_cast<size_t>(RE::BSShader::Type::Total)>
				result;

			auto& lightingVS =
				result[static_cast<size_t>(RE::BSShader::Type::Lighting)][static_cast<size_t>(ShaderClass::Vertex)];
			lightingVS = {
				{ "World", 0 },
				{ "PreviousWorld", 1 },
				{ "EyePosition", 2 },
				{ "LandBlendParams", 3 },
				{ "TreeParams", 4 },
				{ "WindTimers", 5 },
				{ "TextureProj", 6 },
				{ "IndexScale", 7 },
				{ "WorldMapOverlayParameters", 8 },
				{ "LeftEyeCenter", 9 },
				{ "RightEyeCenter", 10 },
				{ "TexcoordOffset", 11 },
				{ "HighDetailRange", 12 },
				{ "FogParam", 13 },
				{ "FogNearColor", 14 },
				{ "FogFarColor", 15 },
				{ "Bones", 16 },
			};

			const auto& lightingPSConstants = ShaderConstants::LightingPS::Get();

			auto& lightingPS = result[static_cast<size_t>(RE::BSShader::Type::Lighting)]
									 [static_cast<size_t>(ShaderClass::Pixel)];

			lightingPS = {
				{ "NumLightNumShadowLight", lightingPSConstants.NumLightNumShadowLight },
				{ "PointLightPosition", lightingPSConstants.PointLightPosition },
				{ "PointLightColor", lightingPSConstants.PointLightColor },
				{ "DirLightDirection", lightingPSConstants.DirLightDirection },
				{ "DirLightColor", lightingPSConstants.DirLightColor },
				{ "DirectionalAmbient", lightingPSConstants.DirectionalAmbient },
				{ "AmbientSpecularTintAndFresnelPower", lightingPSConstants.AmbientSpecularTintAndFresnelPower },
				{ "MaterialData", lightingPSConstants.MaterialData },
				{ "EmitColor", lightingPSConstants.EmitColor },
				{ "AlphaTestRef", lightingPSConstants.AlphaTestRef },
				{ "ShadowLightMaskSelect", lightingPSConstants.ShadowLightMaskSelect },
				{ "VPOSOffset", lightingPSConstants.VPOSOffset },
				{ "ProjectedUVParams", lightingPSConstants.ProjectedUVParams },
				{ "ProjectedUVParams2", lightingPSConstants.ProjectedUVParams2 },
				{ "ProjectedUVParams3", lightingPSConstants.ProjectedUVParams3 },
				{ "SplitDistance", lightingPSConstants.SplitDistance },
				{ "SSRParams", lightingPSConstants.SSRParams },
				{ "WorldMapOverlayParametersPS", lightingPSConstants.WorldMapOverlayParametersPS },
				{ "ShadowSampleParam", lightingPSConstants.ShadowSampleParam },
				{ "EndSplitDistances", lightingPSConstants.EndSplitDistances },
				{ "StartSplitDistances", lightingPSConstants.StartSplitDistances },
				{ "DephBiasParam", lightingPSConstants.DephBiasParam },
				{ "ShadowLightParam", lightingPSConstants.ShadowLightParam },
				{ "ShadowMapProj", lightingPSConstants.ShadowMapProj },
				{ "AmbientColor", lightingPSConstants.AmbientColor },
				{ "FogColor", lightingPSConstants.FogColor },
				{ "ColourOutputClamp", lightingPSConstants.ColourOutputClamp },
				{ "EnvmapData", lightingPSConstants.EnvmapData },
				{ "ParallaxOccData", lightingPSConstants.ParallaxOccData },
				{ "TintColor", lightingPSConstants.TintColor },
				{ "LODTexParams", lightingPSConstants.LODTexParams },
				{ "SpecularColor", lightingPSConstants.SpecularColor },
				{ "SparkleParams", lightingPSConstants.SparkleParams },
				{ "MultiLayerParallaxData", lightingPSConstants.MultiLayerParallaxData },
				{ "LightingEffectParams", lightingPSConstants.LightingEffectParams },
				{ "IBLParams", lightingPSConstants.IBLParams },
				{ "LandscapeTexture1to4IsSnow", lightingPSConstants.LandscapeTexture1to4IsSnow },
				{ "LandscapeTexture5to6IsSnow", lightingPSConstants.LandscapeTexture5to6IsSnow },
				{ "LandscapeTexture1to4IsSpecPower", lightingPSConstants.LandscapeTexture1to4IsSpecPower },
				{ "LandscapeTexture5to6IsSpecPower", lightingPSConstants.LandscapeTexture5to6IsSpecPower },
				{ "SnowRimLightParameters", lightingPSConstants.SnowRimLightParameters },
				{ "CharacterLightParams", lightingPSConstants.CharacterLightParams },
				{ "InvWorldMat", lightingPSConstants.InvWorldMat },
				{ "PreviousWorldMat", lightingPSConstants.PreviousWorldMat },

				{ "PBRFlags", lightingPSConstants.PBRFlags },
				{ "PBRParams1", lightingPSConstants.PBRParams1 },
				{ "LandscapeTexture2PBRParams", lightingPSConstants.LandscapeTexture2PBRParams },
				{ "LandscapeTexture3PBRParams", lightingPSConstants.LandscapeTexture3PBRParams },
				{ "LandscapeTexture4PBRParams", lightingPSConstants.LandscapeTexture4PBRParams },
				{ "LandscapeTexture5PBRParams", lightingPSConstants.LandscapeTexture5PBRParams },
				{ "LandscapeTexture6PBRParams", lightingPSConstants.LandscapeTexture6PBRParams },
				{ "PBRParams2", lightingPSConstants.PBRParams2 },
				{ "LandscapeTexture1GlintParameters", lightingPSConstants.LandscapeTexture1GlintParameters },
				{ "LandscapeTexture2GlintParameters", lightingPSConstants.LandscapeTexture2GlintParameters },
				{ "LandscapeTexture3GlintParameters", lightingPSConstants.LandscapeTexture3GlintParameters },
				{ "LandscapeTexture4GlintParameters", lightingPSConstants.LandscapeTexture4GlintParameters },
				{ "LandscapeTexture5GlintParameters", lightingPSConstants.LandscapeTexture5GlintParameters },
				{ "LandscapeTexture6GlintParameters", lightingPSConstants.LandscapeTexture6GlintParameters },
				{ "MaterialObjectRGBScale", lightingPSConstants.MaterialObjectRGBScale },
			};

			auto& bloodSplatterVS = result[static_cast<size_t>(RE::BSShader::Type::BloodSplatter)]
										  [static_cast<size_t>(ShaderClass::Vertex)];
			bloodSplatterVS = {
				{ "WorldViewProj", 0 },
				{ "LightLoc", 1 },
				{ "Ctrl", 2 },
			};

			auto& bloodSplatterPS = result[static_cast<size_t>(RE::BSShader::Type::BloodSplatter)]
										  [static_cast<size_t>(ShaderClass::Pixel)];
			bloodSplatterPS = {
				{ "Alpha", 0 },
			};

			auto& distantTreeVS = result[static_cast<size_t>(RE::BSShader::Type::DistantTree)]
										[static_cast<size_t>(ShaderClass::Vertex)];

			distantTreeVS = {
				{ "InstanceData", 0 },
				{ "WorldViewProj", 1 },
				{ "World", 2 },
				{ "PreviousWorld", 3 },
				{ "FogParam", 4 },
				{ "FogNearColor", 5 },
				{ "FogFarColor", 6 },
				{ "DiffuseDir", 7 },
				{ "IndexScale", 8 },
			};

			auto& distantTreePS = result[static_cast<size_t>(RE::BSShader::Type::DistantTree)]
										[static_cast<size_t>(ShaderClass::Pixel)];
			distantTreePS = {
				{ "DiffuseColor", 0 },
				{ "AmbientColor", 1 },
			};

			auto& skyVS = result[static_cast<size_t>(RE::BSShader::Type::Sky)]
								[static_cast<size_t>(ShaderClass::Vertex)];
			skyVS = {
				{ "WorldViewProj", 0 },
				{ "World", 1 },
				{ "PreviousWorld", 2 },
				{ "BlendColor", 3 },
				{ "EyePosition", 4 },
				{ "TexCoordOff", 5 },
				{ "VParams", 6 },
			};

			auto& skyPS = result[static_cast<size_t>(RE::BSShader::Type::Sky)]
								[static_cast<size_t>(ShaderClass::Pixel)];
			skyPS = {
				{ "PParams", 0 },
			};

			auto& grassVS = result[static_cast<size_t>(RE::BSShader::Type::Grass)]
								  [static_cast<size_t>(ShaderClass::Vertex)];
			grassVS = {
				{ "WorldViewProj", 0 },
				{ "WorldView", 1 },
				{ "World", 2 },
				{ "PreviousWorld", 3 },
				{ "FogNearColor", 4 },
				{ "WindVector", 5 },
				{ "WindTimer", 6 },
				{ "DirLightDirection", 7 },
				{ "PreviousWindTimer", 8 },
				{ "DirLightColor", 9 },
				{ "AlphaParam1", 10 },
				{ "AmbientColor", 11 },
				{ "AlphaParam2", 12 },
				{ "ScaleMask", 13 },
			};

			const auto& grassPSConstants = ShaderConstants::GrassPS::Get();

			auto& grassPS = result[static_cast<size_t>(RE::BSShader::Type::Grass)]
								  [static_cast<size_t>(ShaderClass::Pixel)];
			grassPS = {
				{ "WorldViewProj", grassPSConstants.WorldViewProj },
				{ "WorldView", grassPSConstants.WorldView },
				{ "World", grassPSConstants.World },
				{ "PreviousWorld", grassPSConstants.PreviousWorld },
				{ "FogNearColor", grassPSConstants.FogNearColor },
				{ "WindVector", grassPSConstants.WindVector },
				{ "WindTimer", grassPSConstants.WindTimer },
				{ "DirLightDirection", grassPSConstants.DirLightDirection },
				{ "PreviousWindTimer", grassPSConstants.PreviousWindTimer },
				{ "DirLightColor", grassPSConstants.DirLightColor },
				{ "AlphaParam1", grassPSConstants.AlphaParam1 },
				{ "AmbientColor", grassPSConstants.AmbientColor },
				{ "AlphaParam2", grassPSConstants.AlphaParam2 },
				{ "ScaleMask", grassPSConstants.ScaleMask },
				{ "PBRFlags", grassPSConstants.PBRFlags },
				{ "PBRParams1", grassPSConstants.PBRParams1 },
				{ "PBRParams2", grassPSConstants.PBRParams2 },
				{ "PBRParams3", grassPSConstants.PBRParams3 },
				{ "PBRParams4", grassPSConstants.PBRParams4 },
				{ "PBRParams5", grassPSConstants.PBRParams5 },
			};

			auto& particleVS = result[static_cast<size_t>(RE::BSShader::Type::Particle)]
									 [static_cast<size_t>(ShaderClass::Vertex)];
			particleVS = {
				{ "WorldViewProj", 0 },
				{ "PrevWorldViewProj", 1 },
				{ "PrecipitationOcclusionWorldViewProj", 2 },
				{ "fVars0", 3 },
				{ "fVars1", 4 },
				{ "fVars2", 5 },
				{ "fVars3", 6 },
				{ "fVars4", 7 },
				{ "Color1", 8 },
				{ "Color2", 9 },
				{ "Color3", 10 },
				{ "Velocity", 11 },
				{ "Acceleration", 12 },
				{ "ScaleAdjust", 13 },
				{ "Wind", 14 },
			};

			auto& particlePS = result[static_cast<size_t>(RE::BSShader::Type::Particle)]
									 [static_cast<size_t>(ShaderClass::Pixel)];
			particlePS = {
				{ "ColorScale", 0 },
				{ "TextureSize", 1 },
			};

			auto& effectVS = result[static_cast<size_t>(RE::BSShader::Type::Effect)]
								   [static_cast<size_t>(ShaderClass::Vertex)];
			effectVS = {
				{ "World", 0 },
				{ "PreviousWorld", 1 },
				{ "Bones", 2 },
				{ "EyePosition", 3 },
				{ "FogParam", 4 },
				{ "FogNearColor", 5 },
				{ "FogFarColor", 6 },
				{ "FalloffData", 7 },
				{ "SoftMateralVSParams", 8 },
				{ "TexcoordOffset", 9 },
				{ "TexcoordOffsetMembrane", 10 },
				{ "SubTexOffset", 11 },
				{ "PosAdjust", 12 },
				{ "MatProj", 13 },
			};

			const auto& effectPSConstants = ShaderConstants::EffectPS::Get();

			auto& effectPS = result[static_cast<size_t>(RE::BSShader::Type::Effect)]
								   [static_cast<size_t>(ShaderClass::Pixel)];
			effectPS = {
				{ "PropertyColor", effectPSConstants.PropertyColor },
				{ "AlphaTestRef", effectPSConstants.AlphaTestRef },
				{ "MembraneRimColor", effectPSConstants.MembraneRimColor },
				{ "MembraneVars", effectPSConstants.MembraneVars },
				{ "PLightPositionX", effectPSConstants.PLightPositionX },
				{ "PLightPositionY", effectPSConstants.PLightPositionY },
				{ "PLightPositionZ", effectPSConstants.PLightPositionZ },
				{ "PLightingRadiusInverseSquared", effectPSConstants.PLightingRadiusInverseSquared },
				{ "PLightColorR", effectPSConstants.PLightColorR },
				{ "PLightColorG", effectPSConstants.PLightColorG },
				{ "PLightColorB", effectPSConstants.PLightColorB },
				{ "DLightColor", effectPSConstants.DLightColor },
				{ "VPOSOffset", effectPSConstants.VPOSOffset },
				{ "CameraDataEffect", effectPSConstants.CameraData },
				{ "FilteringParam", effectPSConstants.FilteringParam },
				{ "BaseColor", effectPSConstants.BaseColor },
				{ "BaseColorScale", effectPSConstants.BaseColorScale },
				{ "LightingInfluence", effectPSConstants.LightingInfluence },

				{ "ExtendedFlags", effectPSConstants.ExtendedFlags },
			};

			auto& waterVS = result[static_cast<size_t>(RE::BSShader::Type::Water)]
								  [static_cast<size_t>(ShaderClass::Vertex)];
			waterVS = {
				{ "WorldViewProj", 0 },
				{ "World", 1 },
				{ "PreviousWorld", 2 },
				{ "QPosAdjust", 3 },
				{ "ObjectUV", 4 },
				{ "NormalsScroll0", 5 },
				{ "NormalsScroll1", 6 },
				{ "NormalsScale", 7 },
				{ "VSFogParam", 8 },
				{ "VSFogNearColor", 9 },
				{ "VSFogFarColor", 10 },
				{ "CellTexCoordOffset", 11 },
			};

			waterVS.insert(
				{
					{ "SubTexOffset", 12 },
					{ "PosAdjust", 13 },
					{ "MatProj", 14 },
				});

			auto& waterPS = result[static_cast<size_t>(RE::BSShader::Type::Water)]
								  [static_cast<size_t>(ShaderClass::Pixel)];
			waterPS = {
				{ "TextureProj", 0 },
				{ "ShallowColor", 1 },
				{ "DeepColor", 2 },
				{ "ReflectionColor", 3 },
				{ "FresnelRI", 4 },
				{ "BlendRadius", 5 },
				{ "PosAdjust", 6 },
				{ "ReflectPlane", 7 },
				{ "CameraDataWater", 8 },
				{ "ProjData", 9 },
				{ "VarAmounts", 10 },
				{ "FogParam", 11 },
				{ "FogNearColor", 12 },
				{ "FogFarColor", 13 },
				{ "SunDir", 14 },
				{ "SunColor", 15 },
				{ "NumLights", 16 },
				{ "LightPos", 17 },
				{ "LightColor", 18 },
				{ "WaterParams", 19 },
				{ "DepthControl", 20 },
				{ "SSRParams", 21 },
				{ "SSRParams2", 22 },
				{ "NormalsAmplitude", 23 },
				{ "VPOSOffset", 24 },
			};

			auto& utilityVS = result[static_cast<size_t>(RE::BSShader::Type::Utility)]
									[static_cast<size_t>(ShaderClass::Vertex)];
			utilityVS = {
				{ "World", 0 },
				{ "TexcoordOffset", 1 },
				{ "EyePos", 2 },
				{ "HighDetailRange", 3 },
				{ "ParabolaParam", 4 },
				{ "ShadowFadeParam", 5 },
				{ "TreeParams", 6 },
				{ "WaterParams", 7 },
				{ "Bones", 8 },
			};

			auto& utilityPS = result[static_cast<size_t>(RE::BSShader::Type::Utility)]
									[static_cast<size_t>(ShaderClass::Pixel)];
			utilityPS = {
				{ "AlphaTestRef", 0 },
				{ "RefractionPower", 1 },
				{ "DebugColor", 2 },
				{ "BaseColor", 3 },
				{ "PropertyColor", 4 },
				{ "FocusShadowMapProj", 5 },
				{ "ShadowMapProj", 6 },
				{ "ShadowSampleParam", 7 },
				{ "ShadowLightParam", 8 },
			};

			utilityPS.insert(
				{
					{ "ShadowFadeParam", 9 },
					{ "VPOSOffset", 10 },
					{ "EndSplitDistances", 11 },
					{ "StartSplitDistances", 12 },
					{ "FocusShadowFadeParam", 13 },
				});

			return result;
		}

		static int32_t GetVariableIndex(ShaderClass shaderClass, const RE::BSShader& shader, const char* name)
		{
			if (shader.shaderType == RE::BSShader::Type::ImageSpace) {
				const auto& imagespaceShader = static_cast<const RE::BSImagespaceShader&>(shader);

				if (shaderClass == ShaderClass::Vertex) {
					for (size_t nameIndex = 0; nameIndex < imagespaceShader.vsConstantNames.size();
						++nameIndex) {
						if (std::string_view(imagespaceShader.vsConstantNames[static_cast<uint32_t>(nameIndex)].c_str()) ==
							name) {
							return static_cast<int32_t>(nameIndex);
						}
					}
				} else if (shaderClass == ShaderClass::Pixel || shaderClass == ShaderClass::Compute) {
					for (size_t nameIndex = 0; nameIndex < imagespaceShader.psConstantNames.size(); ++nameIndex) {
						if (std::string_view(imagespaceShader.psConstantNames[static_cast<uint32_t>(nameIndex)].c_str()) == name) {
							return static_cast<int32_t>(nameIndex);
						}
					}
				}
			} else {
				static auto variableNames = GetVariableIndices();

				const auto& names = variableNames[static_cast<size_t>(shader.shaderType.get())]
												 [static_cast<size_t>(shaderClass)];
				auto it = names.find(name);
				if (it != names.cend()) {
					return it->second;
				}
			}
			return -1;
		}

		static std::string MergeDefinesString(std::array<D3D_SHADER_MACRO, 64>& defines, bool a_sort = false)
		{
			std::string result;
			if (a_sort)
				std::sort(std::begin(defines), std::end(defines), [](const D3D_SHADER_MACRO& a, const D3D_SHADER_MACRO& b) {
					return std::pair{ std::string_view(a.Name ? a.Name : ""), std::string_view(a.Definition ? a.Definition : "") } <
					       std::pair{ std::string_view(b.Name ? b.Name : ""), std::string_view(b.Definition ? b.Definition : "") };
				});
			for (const auto& def : defines) {
				if (def.Name != nullptr) {
					result += def.Name;
					if (def.Definition != nullptr && !std::string_view(def.Definition).empty()) {
						result += "=";
						result += def.Definition;
					}
					result += ' ';
				} else {
					if (a_sort)
						continue;
					break;
				}
			}
			return result;
		}

		static void AddAttribute(uint64_t& desc, RE::BSGraphics::Vertex::Attribute attribute)
		{
			desc |= ((1ull << (44 + attribute)) | (1ull << (54 + attribute)) |
					 (0b1111ull << (4 * attribute + 4)));
		}

		// A constant table entry: the variable's offset in its buffer, in floats. The engine reads entries unsigned,
		// so a variable has to start within the first 256 floats of its buffer.
		static uint8_t ConstantTableEntry(uint32_t startOffsetBytes, const char* name, ShaderClass shaderClass, const RE::BSShader& shader, uint32_t descriptor)
		{
			const uint32_t offset = startOffsetBytes / 4;
			if (offset > std::numeric_limits<uint8_t>::max()) {
				logger::error("Variable {} in {} shader {}::{:X} starts at float {}, beyond what a constant table entry holds",
					name, magic_enum::enum_name(shaderClass), magic_enum::enum_name(shader.shaderType.get()), descriptor, offset);
			}
			return static_cast<uint8_t>(offset);
		}

		template <typename TableByte, size_t MaxOffsetsSize>
		static void ReflectConstantBuffers(const Util::ShaderReflection& reflection,
			std::array<size_t, 3>& bufferSizes,
			std::array<TableByte, MaxOffsetsSize>& constantOffsets,
			uint64_t& vertexDesc,
			ShaderClass shaderClass, uint32_t descriptor, const RE::BSShader& shader)
		{
			static_assert(std::is_same_v<TableByte, std::int8_t> || std::is_same_v<TableByte, std::uint8_t>);

			if (shaderClass == ShaderClass::Vertex) {
				vertexDesc = 0b1111;
				bool hasTexcoord2 = false;
				bool hasTexcoord3 = false;
				for (const auto& input : reflection.inputs) {
					std::string_view semanticName = input.semantic;
					if (semanticName == "POSITION" && input.index == 0) {
						AddAttribute(vertexDesc, RE::BSGraphics::Vertex::VA_POSITION);
					} else if (semanticName == "TEXCOORD" && input.index == 0) {
						AddAttribute(vertexDesc, RE::BSGraphics::Vertex::VA_TEXCOORD0);
					} else if (semanticName == "TEXCOORD" && input.index == 1) {
						AddAttribute(vertexDesc, RE::BSGraphics::Vertex::VA_TEXCOORD1);
					} else if (semanticName == "NORMAL" && input.index == 0) {
						AddAttribute(vertexDesc, RE::BSGraphics::Vertex::VA_NORMAL);
					} else if (semanticName == "BINORMAL" && input.index == 0) {
						AddAttribute(vertexDesc, RE::BSGraphics::Vertex::VA_BINORMAL);
					} else if (semanticName == "COLOR" && input.index == 0) {
						AddAttribute(vertexDesc, RE::BSGraphics::Vertex::VA_COLOR);
					} else if (semanticName == "BLENDWEIGHT" && input.index == 0) {
						AddAttribute(vertexDesc, RE::BSGraphics::Vertex::VA_SKINNING);
					} else if (semanticName == "TEXCOORD" && input.index >= 4 && input.index <= 7) {
						AddAttribute(vertexDesc, RE::BSGraphics::Vertex::VA_INSTANCEDATA);
					} else if (semanticName == "TEXCOORD" && input.index == 2) {
						hasTexcoord2 = true;
					} else if (semanticName == "TEXCOORD" && input.index == 3) {
						hasTexcoord3 = true;
					}
				}
				if (hasTexcoord2) {
					if (hasTexcoord3) {
						AddAttribute(vertexDesc, RE::BSGraphics::Vertex::VA_LANDDATA);
					} else {
						AddAttribute(vertexDesc, RE::BSGraphics::Vertex::VA_EYEDATA);
					}
				}
			}

			auto mapBufferConsts =
				[&](const char* bufferName, size_t& bufferSize) {
					const auto* buffer = reflection.FindBuffer(bufferName);
					if (buffer == nullptr) {
						logger::trace("Buffer {} not found for {} shader {}::{:X}",
							bufferName, magic_enum::enum_name(shaderClass),
							magic_enum::enum_name(shader.shaderType.get()),
							descriptor);
						return;
					}

					for (const auto& variable : buffer->variables) {
						const char* variableName = variable.name.c_str();
						const auto variableIndex =
							GetVariableIndex(shaderClass, shader, variableName);
						const bool variableFound = variableIndex != -1;
						if (variableFound) {
							constantOffsets[variableIndex] = ConstantTableEntry(variable.offset, variableName, shaderClass, shader, descriptor);
						} else {
							logger::trace("Unknown variable name {} in {} shader {}::{:X}",
								variableName, magic_enum::enum_name(shaderClass),
								magic_enum::enum_name(shader.shaderType.get()),
								descriptor);
						}

						if (shader.shaderType == RE::BSShader::Type::ImageSpace) {
							if (variable.elements > 0) {
								if (!variableFound) {
									const std::string arrayName =
										std::format("{}[{}]", variableName, variable.elements);
									const auto variableArrayIndex =
										GetVariableIndex(shaderClass, shader, arrayName.c_str());
									if (variableArrayIndex != -1) {
										constantOffsets[variableArrayIndex] = ConstantTableEntry(variable.offset, arrayName.c_str(), shaderClass, shader, descriptor);
									} else {
										logger::debug("Unknown variable name {} in {} shader {}::{:X}",
											arrayName, magic_enum::enum_name(shaderClass),
											magic_enum::enum_name(shader.shaderType.get()), descriptor);
									}
								} else {
									const auto elementSize = variable.size / variable.elements;
									for (uint32_t arrayIndex = 1; arrayIndex < variable.elements;
										++arrayIndex) {
										const std::string varName =
											std::format("{}[{}]", variableName, arrayIndex);
										const auto variableArrayElementIndex =
											GetVariableIndex(shaderClass, shader, varName.c_str());
										if (variableArrayElementIndex != -1) {
											constantOffsets[variableArrayElementIndex] = ConstantTableEntry(
												variable.offset + elementSize * arrayIndex, varName.c_str(), shaderClass, shader, descriptor);
										} else {
											logger::debug(
												"Unknown variable name {} in {} shader {}::{:X}", varName,
												magic_enum::enum_name(shaderClass),
												magic_enum::enum_name(shader.shaderType.get()),
												descriptor);
										}
									}
								}
							}
						}
					}

					bufferSize = ((buffer->size + 15) & ~15) / 16;
				};

			mapBufferConsts("PerTechnique", bufferSizes[0]);
			mapBufferConsts("PerMaterial", bufferSizes[1]);
			mapBufferConsts("PerGeometry", bufferSizes[2]);
		}

		static std::string GetShaderString(ShaderClass shaderClass, const RE::BSShader& shader, uint32_t descriptor, bool hashkey)
		{
			auto sourceShaderFile = shader.fxpFilename;
			std::array<D3D_SHADER_MACRO, 64> defines{};
			SIE::SShaderCache::GetShaderDefines(shader, descriptor, std::span{ defines });
			std::string result;
			if (hashkey)  // generate hashkey so don't include descriptor
				result = fmt::format("{}:{}:{}", sourceShaderFile, magic_enum::enum_name(shaderClass), SIE::SShaderCache::MergeDefinesString(defines, true));
			else
				result = fmt::format("{}:{}:{:X}:{}", sourceShaderFile, magic_enum::enum_name(shaderClass), descriptor, SIE::SShaderCache::MergeDefinesString(defines, true));
			return result;
		}

		std::string GetTypeFromShaderString(const std::string& a_key)
		{
			std::string type = "";
			std::string::size_type pos = a_key.find(':');
			if (pos != std::string::npos)
				type = a_key.substr(0, pos);
			if (type.starts_with("IS") || type == "ReflectionsRayTracing" || type == "LensFlareVisibility")
				type = "ImageSpace";  // fix type for image space shaders
			return type;
		}

		/**
		 * @brief Compiles or retrieves a cached shader.
		 *
		 * Checks the in-memory shader cache, then the shader store, which compiles from
		 * HLSL source if necessary. Records include dependencies for hot-reload invalidation.
		 *
		 * @param dependencyTracker Optional tracker for include dependency registration;
		 *                          enables cache invalidation when dependencies change.
		 *
		 * @return Compiled shader blob, or nullptr if compilation failed or source file not found.
		 */
		static winrt::com_ptr<ID3DBlob> CompileShader(ShaderClass shaderClass, const RE::BSShader& shader, uint32_t descriptor, ShaderFileDependencyTracker* dependencyTracker)
		{
			if (!SShaderCache::ResolveImageSpaceDescriptor(shader, descriptor)) {
				return nullptr;
			}

			auto& cache = ShaderCache::Instance();
			auto key = SShaderCache::GetShaderString(shaderClass, shader, descriptor, true);

			// Atomically check the shaderMap and either:
			//  - return the blob if already Completed (cache hit),
			//  - return nullptr if a previous attempt Failed,
			//  - wait if another thread is compiling (Pending),
			//  - claim the slot with Pending if nobody started yet.
			auto [claimResult, cachedBlob] = cache.ClaimCompilation(key);
			if (claimResult == ShaderCache::ClaimResult::CacheHit) {
				cache.IncCacheHitTasks();
				return cachedBlob;
			}
			if (claimResult == ShaderCache::ClaimResult::Failed) {
				return nullptr;
			}

			const auto type = shader.shaderType.get();

			// Read once: Developer Mode can toggle mid-compile
			const bool debugCompile = ShaderCache::IsDebugCompile();

			// prepare preprocessor defines
			std::array<D3D_SHADER_MACRO, 64> defines{};
			auto lastIndex = 0;
			if (shaderClass == ShaderClass::Vertex) {
				defines[lastIndex++] = { "VSHADER", nullptr };
			} else if (shaderClass == ShaderClass::Pixel) {
				defines[lastIndex++] = { "PSHADER", nullptr };
			} else if (shaderClass == ShaderClass::Compute) {
				defines[lastIndex++] = { "CSHADER", nullptr };
			}
			if (debugCompile) {
				defines[lastIndex++] = { "D3DCOMPILE_SKIP_OPTIMIZATION", nullptr };
				defines[lastIndex++] = { "D3DCOMPILE_DEBUG", nullptr };
			}
			auto shaderDefines = globals::state->GetDefines();
			if (!shaderDefines->empty()) {
				for (unsigned int i = 0; i < shaderDefines->size(); i++)
					defines[lastIndex++] = { shaderDefines->at(i).first.c_str(), shaderDefines->at(i).second.c_str() };
			}
			defines[lastIndex] = { nullptr, nullptr };  // do final entry
			GetShaderDefines(shader, descriptor, std::span{ defines }.subspan(lastIndex));

			const std::wstring path = GetShaderPath(
				shader.shaderType == RE::BSShader::Type::ImageSpace ?
					static_cast<const RE::BSImagespaceShader&>(shader).originalShaderName :
					shader.fxpFilename);
			logger::debug("Building {} {}:{}:{:X} from {}", Util::WStringToString(path), magic_enum::enum_name(type), magic_enum::enum_name(shaderClass), descriptor, MergeDefinesString(defines));

			// Debug builds stay on FXC, whose debug flags the DXC path does not have.
			const char* dxcTarget = shaderClass == ShaderClass::Vertex ? "vs_6_6" : shaderClass == ShaderClass::Pixel ? "ps_6_6" :
			                                                                                                             "cs_6_6";
			if (!debugCompile && Util::UseDxc(path, dxcTarget, descriptor)) {
				auto dxc = Util::ResolveDxcShader({ path, defines.data(), dxcTarget });
				if (dxc.blob) {
					if (dependencyTracker && cache.UseFileWatcher()) {
						if (const auto includes = Util::FxcDependencyPaths(dxc.variantKey); !includes.empty())
							dependencyTracker->RegisterDependencies(Util::WStringToString(path), includes);
					}
					if (!dxc.diagnostics.empty())
						logger::debug("Shader logs:\n{}", dxc.diagnostics);
					cache.AddCompletedShader(shaderClass, shader, descriptor, dxc.blob, /*fromDisk=*/dxc.fromStore);
					return dxc.blob;
				}
				logger::warn("DXC could not build {} shader {}::{:X}, so it uses FXC:\n{}", magic_enum::enum_name(shaderClass), magic_enum::enum_name(type), descriptor, dxc.diagnostics);
			}

			Util::FxcRequest request;
			request.root = path;
			request.includeBase = std::filesystem::path(path).parent_path();
			request.defines = defines.data();
			request.profile = GetShaderProfile(shaderClass);
			request.flags = Util::FxcCompileFlags(debugCompile);
			request.stripFlags = debugCompile ? 0 : (D3DCOMPILER_STRIP_DEBUG_INFO | D3DCOMPILER_STRIP_TEST_BLOBS | D3DCOMPILER_STRIP_PRIVATE_DATA);
			request.debug = debugCompile;
			auto built = Util::ResolveFxcShader(request);

			// The watcher needs each shader's includes, also when nothing compiled. This is development only, so a normal start skips the work.
			if (dependencyTracker && cache.UseFileWatcher()) {
				if (const auto includes = Util::FxcDependencyPaths(built.variantKey); !includes.empty())
					dependencyTracker->RegisterDependencies(Util::WStringToString(path), includes);
			}

			if (!built.blob) {
				logger::error("Failed to compile {} shader {}::{:X}{}:\n{}", magic_enum::enum_name(shaderClass), magic_enum::enum_name(type), descriptor,
					built.store.failureFromCache ? " (same inputs as the last failure, so it was not retried)" : "", built.store.diagnostics);

#ifdef TRACY_ENABLE
				{
					// Timeline annotation: a (re)compile failed. Pairs with the
					// MCP shadercache status (failedTasks) for build-agnostic
					// detection; this gives the exact frame for perf correlation.
					const auto tracyMsg = std::format("Shader compile FAILED: {} {} {:X}",
						magic_enum::enum_name(type), magic_enum::enum_name(shaderClass), descriptor);
					TracyMessageC(tracyMsg.c_str(), tracyMsg.size(), 0xFF4444);
				}
#endif

				cache.AddCompletedShader(shaderClass, shader, descriptor, nullptr);
				return nullptr;
			}
			if (!built.store.diagnostics.empty())
				logger::debug("Shader logs:\n{}", built.store.diagnostics);

			using org::services::ShaderStoreOutcome;

#ifdef TRACY_ENABLE
			if (built.store.outcome == ShaderStoreOutcome::Compiled) {
				// Timeline annotation: a shader (re)compiled successfully. During
				// a hot-reload this marks the exact frame the new shader went
				// live, so A/B perf windows split precisely on it.
				const auto tracyMsg = std::format("Shader compiled: {} {} {:X}",
					magic_enum::enum_name(type), magic_enum::enum_name(shaderClass), descriptor);
				TracyMessage(tracyMsg.c_str(), tracyMsg.size());
			}
#endif

			cache.AddCompletedShader(shaderClass, shader, descriptor, built.blob, /*fromDisk=*/built.store.outcome == ShaderStoreOutcome::Hit);
			return built.blob;
		}

		// Native constant groups are pooled by size across shader types. A probe can
		// overwrite a Utility/Lighting group that was not bound when capture began;
		// rebinding the saved pipeline cannot restore that group's contents.
		static ID3D11Buffer* IsolateProbeConstants(const RE::BSShader& shader, uint32_t descriptor,
			uint32_t stage, uint32_t group, ID3D11Buffer* original)
		{
			if (shader.shaderType.get() != RE::BSShader::Type::Lighting ||
				!(descriptor & uint32_t(ShaderCache::LightingShaderFlags::EnvironmentProbe)))
				return original;
			static std::mutex mutex;
			static std::array<std::array<std::unordered_map<UINT, winrt::com_ptr<ID3D11Buffer>>, 3>, 2> pools;
			D3D11_BUFFER_DESC desc{};
			original->GetDesc(&desc);
			std::scoped_lock lock(mutex);
			auto& buffer = pools[stage][group][desc.ByteWidth];
			if (!buffer) {
				DX::ThrowIfFailed(globals::d3d::device->CreateBuffer(&desc, nullptr, buffer.put()));
				Util::SetResourceName(buffer.get(), "EnvironmentProbe::%s Constants group%u size%u", stage == 0 ? "VS" : "PS", group, desc.ByteWidth);
			}
			return buffer.get();
		}

		VertexShaderPtr CreateVertexShader(ID3DBlob& shaderData,
			const RE::BSShader& shader, uint32_t descriptor)
		{
			static const auto perTechniqueBuffersArray =
				REL::Relocation<ID3D11Buffer**>(RELOCATION_ID(524755, 411371));
			static const auto perMaterialBuffersArray =
				REL::Relocation<ID3D11Buffer**>(RELOCATION_ID(524757, 411373));
			static const auto perGeometryBuffersArray =
				REL::Relocation<ID3D11Buffer**>(RELOCATION_ID(524759, 411375));
			static const auto bufferData = REL::Relocation<void*>(RELOCATION_ID(524965, 411446));

			auto rawPtr =
				new uint8_t[sizeof(RE::BSGraphics::VertexShader) + shaderData.GetBufferSize()];
			auto shaderPtr = new (rawPtr) RE::BSGraphics::VertexShader;
			memcpy(rawPtr + sizeof(RE::BSGraphics::VertexShader), shaderData.GetBufferPointer(),
				shaderData.GetBufferSize());
			VertexShaderPtr newShader{ shaderPtr };
			newShader->byteCodeSize = (uint32_t)shaderData.GetBufferSize();
			newShader->id = descriptor;
			newShader->vertexDesc = 0;

			Util::ShaderReflection reflection;
			if (!Util::ReflectShader(shaderData, reflection)) {
				logger::error("Failed to reflect vertex shader {}::{:X}", magic_enum::enum_name(shader.shaderType.get()),
					descriptor);
			} else {
				std::array<size_t, 3> bufferSizes = { 0, 0, 0 };
				std::fill(newShader->constantTable.begin(), newShader->constantTable.end(), static_cast<uint8_t>(0));
				ReflectConstantBuffers(reflection, bufferSizes, newShader->constantTable, newShader->vertexDesc,
					ShaderClass::Vertex, descriptor, shader);
				if (bufferSizes[0] != 0) {
					newShader->constantBuffers[0].buffer =
						(REX::W32::ID3D11Buffer*)IsolateProbeConstants(shader, descriptor, 0, 0, perTechniqueBuffersArray.get()[bufferSizes[0]]);
				} else {
					newShader->constantBuffers[0].buffer = nullptr;
					newShader->constantBuffers[0].data = bufferData.get();
				}
				if (bufferSizes[1] != 0) {
					newShader->constantBuffers[1].buffer =
						(REX::W32::ID3D11Buffer*)IsolateProbeConstants(shader, descriptor, 0, 1, perMaterialBuffersArray.get()[bufferSizes[1]]);
				} else {
					newShader->constantBuffers[1].buffer = nullptr;
					newShader->constantBuffers[1].data = bufferData.get();
				}
				if (bufferSizes[2] != 0) {
					newShader->constantBuffers[2].buffer =
						(REX::W32::ID3D11Buffer*)IsolateProbeConstants(shader, descriptor, 0, 2, perGeometryBuffersArray.get()[bufferSizes[2]]);
				} else {
					newShader->constantBuffers[2].buffer = nullptr;
					newShader->constantBuffers[2].data = bufferData.get();
				}
			}

			return newShader;
		}

		std::unique_ptr<RE::BSGraphics::PixelShader> CreatePixelShader(ID3DBlob& shaderData,
			const RE::BSShader& shader, uint32_t descriptor)
		{
			static const auto perTechniqueBuffersArray =
				REL::Relocation<ID3D11Buffer**>(RELOCATION_ID(524761, 411377));
			static const auto perMaterialBuffersArray =
				REL::Relocation<ID3D11Buffer**>(RELOCATION_ID(524763, 411379));
			static const auto perGeometryBuffersArray =
				REL::Relocation<ID3D11Buffer**>(RELOCATION_ID(524765, 411381));
			static const auto bufferData = REL::Relocation<void*>(RELOCATION_ID(524967, 411448));

			auto newShader = std::make_unique<RE::BSGraphics::PixelShader>();
			newShader->id = descriptor;

			Util::ShaderReflection reflection;
			if (!Util::ReflectShader(shaderData, reflection)) {
				logger::error("Failed to reflect vertex shader {}::{:X}", magic_enum::enum_name(shader.shaderType.get()),
					descriptor);
			} else {
				std::array<size_t, 3> bufferSizes = { 0, 0, 0 };
				std::ranges::fill(newShader->constantTable, static_cast<uint8_t>(0));
				uint64_t dummy;
				ReflectConstantBuffers(reflection, bufferSizes, newShader->constantTable,
					dummy,
					ShaderClass::Pixel, descriptor, shader);
				if (bufferSizes[0] != 0) {
					newShader->constantBuffers[0].buffer =
						(REX::W32::ID3D11Buffer*)IsolateProbeConstants(shader, descriptor, 1, 0, perTechniqueBuffersArray.get()[bufferSizes[0]]);
				} else {
					newShader->constantBuffers[0].buffer = nullptr;
					newShader->constantBuffers[0].data = bufferData.get();
				}
				if (bufferSizes[1] != 0) {
					newShader->constantBuffers[1].buffer =
						(REX::W32::ID3D11Buffer*)IsolateProbeConstants(shader, descriptor, 1, 1, perMaterialBuffersArray.get()[bufferSizes[1]]);
				} else {
					newShader->constantBuffers[1].buffer = nullptr;
					newShader->constantBuffers[1].data = bufferData.get();
				}
				if (bufferSizes[2] != 0) {
					newShader->constantBuffers[2].buffer =
						(REX::W32::ID3D11Buffer*)IsolateProbeConstants(shader, descriptor, 1, 2, perGeometryBuffersArray.get()[bufferSizes[2]]);
				} else {
					newShader->constantBuffers[2].buffer = nullptr;
					newShader->constantBuffers[2].data = bufferData.get();
				}
			}

			return newShader;
		}

		std::unique_ptr<RE::BSGraphics::ComputeShader> CreateComputeShader([[maybe_unused]] ID3DBlob& shaderData,
			[[maybe_unused]] const RE::BSShader& shader, uint32_t descriptor)
		{
			auto newShader = std::make_unique<RE::BSGraphics::ComputeShader>();
			newShader->id = descriptor;
			return newShader;
		}

		static bool GetImagespaceShaderDescriptor(const RE::BSImagespaceShader& imagespaceShader, uint32_t& descriptor)
		{
			using enum RE::ImageSpaceManager::ImageSpaceEffectEnum;

			static const ankerl::unordered_dense::map<std::string_view, uint32_t> descriptors{
				// { "BSImagespaceShaderISBlur", RE::ImageSpaceManager::GetCurrentIndex(ISBlur) },
				// { "BSImagespaceShaderBlur3", RE::ImageSpaceManager::GetCurrentIndex(ISBlur3) },
				// { "BSImagespaceShaderBlur5", RE::ImageSpaceManager::GetCurrentIndex(ISBlur5) },
				// { "BSImagespaceShaderBlur7", RE::ImageSpaceManager::GetCurrentIndex(ISBlur7) },
				// { "BSImagespaceShaderBlur9", RE::ImageSpaceManager::GetCurrentIndex(ISBlur9) },
				// { "BSImagespaceShaderBlur11", RE::ImageSpaceManager::GetCurrentIndex(ISBlur11) },
				// { "BSImagespaceShaderBlur13", RE::ImageSpaceManager::GetCurrentIndex(ISBlur13) },
				// { "BSImagespaceShaderBlur15", RE::ImageSpaceManager::GetCurrentIndex(ISBlur15) },
				// { "BSImagespaceShaderBrightPassBlur3", RE::ImageSpaceManager::GetCurrentIndex(ISBrightPassBlur3) },
				// { "BSImagespaceShaderBrightPassBlur5", RE::ImageSpaceManager::GetCurrentIndex(ISBrightPassBlur5) },
				// { "BSImagespaceShaderBrightPassBlur7", RE::ImageSpaceManager::GetCurrentIndex(ISBrightPassBlur7) },
				// { "BSImagespaceShaderBrightPassBlur9", RE::ImageSpaceManager::GetCurrentIndex(ISBrightPassBlur9) },
				// { "BSImagespaceShaderBrightPassBlur11", RE::ImageSpaceManager::GetCurrentIndex(ISBrightPassBlur11) },
				// { "BSImagespaceShaderBrightPassBlur13", RE::ImageSpaceManager::GetCurrentIndex(ISBrightPassBlur13) },
				// { "BSImagespaceShaderBrightPassBlur15", RE::ImageSpaceManager::GetCurrentIndex(ISBrightPassBlur15) },
				// { "BSImagespaceShaderNonHDRBlur3", RE::ImageSpaceManager::GetCurrentIndex(ISNonHDRBlur3) },
				// { "BSImagespaceShaderNonHDRBlur5", RE::ImageSpaceManager::GetCurrentIndex(ISNonHDRBlur5) },
				// { "BSImagespaceShaderNonHDRBlur7", RE::ImageSpaceManager::GetCurrentIndex(ISNonHDRBlur7) },
				// { "BSImagespaceShaderNonHDRBlur9", RE::ImageSpaceManager::GetCurrentIndex(ISNonHDRBlur9) },
				// { "BSImagespaceShaderNonHDRBlur11", RE::ImageSpaceManager::GetCurrentIndex(ISNonHDRBlur11) },
				// { "BSImagespaceShaderNonHDRBlur13", RE::ImageSpaceManager::GetCurrentIndex(ISNonHDRBlur13) },
				// { "BSImagespaceShaderNonHDRBlur15", RE::ImageSpaceManager::GetCurrentIndex(ISNonHDRBlur15) },
				// { "BSImagespaceShaderISBasicCopy", RE::ImageSpaceManager::GetCurrentIndex(ISBasicCopy) },
				// { "BSImagespaceShaderISSimpleColor", RE::ImageSpaceManager::GetCurrentIndex(ISSimpleColor) },
				// { "BSImagespaceShaderApplyReflections", RE::ImageSpaceManager::GetCurrentIndex(ISApplyReflections) },
				// { "BSImagespaceShaderISExp", RE::ImageSpaceManager::GetCurrentIndex(ISExp) },
				// { "BSImagespaceShaderISDisplayDepth", RE::ImageSpaceManager::GetCurrentIndex(ISDisplayDepth) },
				// { "BSImagespaceShaderAlphaBlend", RE::ImageSpaceManager::GetCurrentIndex(ISAlphaBlend) },
				// { "BSImagespaceShaderWaterFlow", RE::ImageSpaceManager::GetCurrentIndex(ISWaterFlow) },
				{ "BSImagespaceShaderISWaterBlend", RE::ImageSpaceManager::GetCurrentIndex(ISWaterBlend) },
				// { "BSImagespaceShaderGreyScale", RE::ImageSpaceManager::GetCurrentIndex(ISCopyGrayScale) },
				// { "BSImagespaceShaderCopy", RE::ImageSpaceManager::GetCurrentIndex(ISCopy) },
				// { "BSImagespaceShaderCopyScaleBias", RE::ImageSpaceManager::GetCurrentIndex(ISCopyScaleBias) },
				// { "BSImagespaceShaderCopyCustomViewport",
				//  RE::ImageSpaceManager::GetCurrentIndex(ISCopyCustomViewport) },
				// { "BSImagespaceShaderCopyTextureMask", RE::ImageSpaceManager::GetCurrentIndex(ISCopyTextureMask) },
				// { "BSImagespaceShaderCopyDynamicFetchDisabled",
				//  RE::ImageSpaceManager::GetCurrentIndex(ISCopyDynamicFetchDisabled) },
				{ "BSImagespaceShaderISCompositeVolumetricLighting",
					RE::ImageSpaceManager::GetCurrentIndex(ISCompositeVolumetricLighting) },
				{ "BSImagespaceShaderISCompositeLensFlare",
					RE::ImageSpaceManager::GetCurrentIndex(ISCompositeLensFlare) },
				{ "BSImagespaceShaderISCompositeLensFlareVolumetricLighting",
					RE::ImageSpaceManager::GetCurrentIndex(ISCompositeLensFlareVolumetricLighting) },
				{ "BGSLensFlareVisibilityPass", RE::ImageSpaceManager::GetCurrentIndex(ISLensFlareVisibility) },
				// { "BSImagespaceShaderISDebugSnow", RE::ImageSpaceManager::GetCurrentIndex(ISDebugSnow) },
				{ "BSImagespaceShaderDepthOfField", RE::ImageSpaceManager::GetCurrentIndex(ISDepthOfField) },
				{ "BSImagespaceShaderDepthOfFieldFogged",
					RE::ImageSpaceManager::GetCurrentIndex(ISDepthOfFieldFogged) },
				{ "BSImagespaceShaderDepthOfFieldMaskedFogged",
					RE::ImageSpaceManager::GetCurrentIndex(ISDepthOfFieldMaskedFogged) },
				{ "BSImagespaceShaderDistantBlur", RE::ImageSpaceManager::GetCurrentIndex(ISDistantBlur) },
				{ "BSImagespaceShaderDistantBlurFogged",
					RE::ImageSpaceManager::GetCurrentIndex(ISDistantBlurFogged) },
				{ "BSImagespaceShaderDistantBlurMaskedFogged",
					RE::ImageSpaceManager::GetCurrentIndex(ISDistantBlurMaskedFogged) },
				{ "BSImagespaceShaderDoubleVision", RE::ImageSpaceManager::GetCurrentIndex(ISDoubleVision) },
				{ "BSImagespaceShaderISDownsample", RE::ImageSpaceManager::GetCurrentIndex(ISDownsample) },
				{ "BSImagespaceShaderISDownsampleIgnoreBrightest",
					RE::ImageSpaceManager::GetCurrentIndex(ISDownsampleIgnoreBrightest) },
				// { "BSImagespaceShaderISUpsampleDynamicResolution",
				// 	RE::ImageSpaceManager::GetCurrentIndex(ISUpsampleDynamicResolution) },
				{ "BSImageSpaceShaderVolumetricLighting",
					RE::ImageSpaceManager::GetCurrentIndex(ISVolumetricLighting) },
				{ "BSImagespaceShaderHDRDownSample4", RE::ImageSpaceManager::GetCurrentIndex(ISHDRDownSample4) },
				{ "BSImagespaceShaderHDRDownSample4LightAdapt",
					RE::ImageSpaceManager::GetCurrentIndex(ISHDRDownSample4LightAdapt) },
				{ "BSImagespaceShaderHDRDownSample4LumClamp",
					RE::ImageSpaceManager::GetCurrentIndex(ISHDRDownSample4LumClamp) },
				{ "BSImagespaceShaderHDRDownSample4RGB2Lum",
					RE::ImageSpaceManager::GetCurrentIndex(ISHDRDownSample4RGB2Lum) },
				{ "BSImagespaceShaderHDRDownSample16", RE::ImageSpaceManager::GetCurrentIndex(ISHDRDownSample16) },
				{ "BSImagespaceShaderHDRDownSample16LightAdapt",
					RE::ImageSpaceManager::GetCurrentIndex(ISHDRDownSample16LightAdapt) },
				{ "BSImagespaceShaderHDRDownSample16Lum",
					RE::ImageSpaceManager::GetCurrentIndex(ISHDRDownSample16Lum) },
				{ "BSImagespaceShaderHDRDownSample16LumClamp",
					RE::ImageSpaceManager::GetCurrentIndex(ISHDRDownSample16LumClamp) },
				{ "BSImagespaceShaderHDRTonemapBlendCinematic",
					RE::ImageSpaceManager::GetCurrentIndex(ISHDRTonemapBlendCinematic) },
				{ "BSImagespaceShaderHDRTonemapBlendCinematicFade",
					RE::ImageSpaceManager::GetCurrentIndex(ISHDRTonemapBlendCinematicFade) },
				// { "BSImagespaceShaderISIBLensFlares", RE::ImageSpaceManager::GetCurrentIndex(ISIBLensFlares) },

				// Those cause issue because of typo in shader name in vanilla code but at the same time they are not used by vanilla game.
				// { "BSImagespaceShaderISLightingComposite",
				//  RE::ImageSpaceManager::GetCurrentIndex(ISLightingComposite) },
				// { "BSImagespaceShaderISLightingCompositeMenu",
				//  RE::ImageSpaceManager::GetCurrentIndex(ISLightingCompositeMenu) },
				// { "BSImagespaceShaderISLightingCompositeNoDirectionalLight",
				//  RE::ImageSpaceManager::GetCurrentIndex(ISLightingCompositeNoDirectionalLight) },

				// { "BSImagespaceShaderLocalMap", RE::ImageSpaceManager::GetCurrentIndex(ISLocalMap) },
				// { "BSISWaterBlendHeightmaps", RE::ImageSpaceManager::GetCurrentIndex(ISWaterBlendHeightmaps) },
				// { "BSISWaterDisplacementClearSimulation",
				// 	RE::ImageSpaceManager::GetCurrentIndex(ISWaterDisplacementClearSimulation) },
				// { "BSISWaterDisplacementNormals",
				// 	RE::ImageSpaceManager::GetCurrentIndex(ISWaterDisplacementNormals) },
				// { "BSISWaterDisplacementRainRipple",
				// 	RE::ImageSpaceManager::GetCurrentIndex(ISWaterDisplacementRainRipple) },
				// { "BSISWaterDisplacementTexOffset",
				// 	RE::ImageSpaceManager::GetCurrentIndex(ISWaterDisplacementTexOffset) },
				// { "BSISWaterWadingHeightmap", RE::ImageSpaceManager::GetCurrentIndex(ISWaterWadingHeightmap) },
				// { "BSISWaterRainHeightmap", RE::ImageSpaceManager::GetCurrentIndex(ISWaterRainHeightmap) },
				// { "BSISWaterSmoothHeightmap", RE::ImageSpaceManager::GetCurrentIndex(ISWaterSmoothHeightmap) },
				// { "BSISWaterWadingHeightmap", RE::ImageSpaceManager::GetCurrentIndex(ISWaterWadingHeightmap) },
				// { "BSImagespaceShaderMap", RE::ImageSpaceManager::GetCurrentIndex(ISMap) },
				// { "BSImagespaceShaderMap", RE::ImageSpaceManager::GetCurrentIndex(ISMap) },
				// { "BSImagespaceShaderWorldMap", RE::ImageSpaceManager::GetCurrentIndex(ISWorldMap) },
				// { "BSImagespaceShaderWorldMapNoSkyBlur",
				// 	RE::ImageSpaceManager::GetCurrentIndex(ISWorldMapNoSkyBlur) },
				{ "BSImagespaceShaderISMinify", RE::ImageSpaceManager::GetCurrentIndex(ISMinify) },
				{ "BSImagespaceShaderISMinifyContrast", RE::ImageSpaceManager::GetCurrentIndex(ISMinifyContrast) },
				// { "BSImagespaceShaderNoiseNormalmap", RE::ImageSpaceManager::GetCurrentIndex(ISNoiseNormalmap) },
				// { "BSImagespaceShaderNoiseScrollAndBlend",
				// 	RE::ImageSpaceManager::GetCurrentIndex(ISNoiseScrollAndBlend) },
				{ "BSImagespaceShaderRadialBlur", RE::ImageSpaceManager::GetCurrentIndex(ISRadialBlur) },
				{ "BSImagespaceShaderRadialBlurHigh", RE::ImageSpaceManager::GetCurrentIndex(ISRadialBlurHigh) },
				{ "BSImagespaceShaderRadialBlurMedium", RE::ImageSpaceManager::GetCurrentIndex(ISRadialBlurMedium) },
				{ "BSImagespaceShaderRefraction", RE::ImageSpaceManager::GetCurrentIndex(ISRefraction) },
				{ "BSImagespaceShaderISSAOCompositeSAO", RE::ImageSpaceManager::GetCurrentIndex(ISSAOCompositeSAO) },
				{ "BSImagespaceShaderISSAOCompositeFog", RE::ImageSpaceManager::GetCurrentIndex(ISSAOCompositeFog) },
				{ "BSImagespaceShaderISSAOCompositeSAOFog", RE::ImageSpaceManager::GetCurrentIndex(ISSAOCompositeSAOFog) },
				// { "BSImagespaceShaderISSAOCameraZ", RE::ImageSpaceManager::GetCurrentIndex(ISSAOCameraZ) },
				// { "BSImagespaceShaderISSILComposite", RE::ImageSpaceManager::GetCurrentIndex(ISSILComposite) },
				// { "BSImagespaceShaderISSnowSSS", RE::ImageSpaceManager::GetCurrentIndex(ISSnowSSS) },
				// { "BSImagespaceShaderISSAOBlurH", RE::ImageSpaceManager::GetCurrentIndex(ISSAOBlurH) },
				// { "BSImagespaceShaderISSAOBlurV", RE::ImageSpaceManager::GetCurrentIndex(ISSAOBlurV) },
				// { "BSImagespaceShaderISUnderwaterMask", RE::ImageSpaceManager::GetCurrentIndex(ISUnderwaterMask) },
				{ "BSImagespaceShaderISApplyVolumetricLighting", RE::ImageSpaceManager::GetCurrentIndex(ISApplyVolumetricLighting) },
				{ "BSImagespaceShaderReflectionsRayTracing", RE::ImageSpaceManager::GetCurrentIndex(ISReflectionsRayTracing) },
				//{ "BSImagespaceShaderReflectionsDebugSpecMask", RE::ImageSpaceManager::GetCurrentIndex(ISReflectionsDebugSpecMask) },
				{ "BSImagespaceShaderISTemporalAA", RE::ImageSpaceManager::GetCurrentIndex(ISTemporalAA) },
				{ "BSImagespaceShaderVolumetricLightingRaymarchCS", 256 },
				{ "BSImagespaceShaderVolumetricLightingGenerateCS", 257 },
				{ "BSImagespaceShaderVolumetricLightingBlurHCS", RE::ImageSpaceManager::GetCurrentIndex(ISVolumetricLightingBlurHCS) },
				{ "BSImagespaceShaderVolumetricLightingBlurVCS", RE::ImageSpaceManager::GetCurrentIndex(ISVolumetricLightingBlurVCS) },
				{ "BSImagespaceShaderReflectionBlurHCS", RE::ImageSpaceManager::GetCurrentIndex(ISReflectionBlurHCS) },
				{ "BSImagespaceShaderReflectionBlurVCS", RE::ImageSpaceManager::GetCurrentIndex(ISReflectionBlurVCS) },

				{ "BSImagespaceShaderISDownsampleHierarchicalDepthBufferCS", RE::ImageSpaceManager::GetCurrentIndex(ISDownsampleHierarchicalDepthBufferCS) },
				{ "BSImagespaceShaderISDiffScaleDownsampleDepthBufferCS", RE::ImageSpaceManager::GetCurrentIndex(ISDiffScaleDownsampleDepthBufferCS) },
				{ "BSImagespaceShaderISTransformLvl7PreTest", RE::ImageSpaceManager::GetCurrentIndex(ISTransformLvl7PreTest) },
				{ "BSImagespaceShaderISLvl6PreTest", RE::ImageSpaceManager::GetCurrentIndex(ISLvl6PreTest) },
				{ "BSImagespaceShaderISLvl5PreTest", RE::ImageSpaceManager::GetCurrentIndex(ISLvl5PreTest) },
				{ "BSImagespaceShaderISLvl4PreTest", RE::ImageSpaceManager::GetCurrentIndex(ISLvl4PreTest) },
				{ "BSImagespaceShaderISLvl3PreTest", RE::ImageSpaceManager::GetCurrentIndex(ISLvl3PreTest) },
				{ "BSImagespaceShaderISLvl2PreTest", RE::ImageSpaceManager::GetCurrentIndex(ISLvl2PreTest) },
				{ "BSImagespaceShaderISLvl1PreTest", RE::ImageSpaceManager::GetCurrentIndex(ISLvl1PreTest) },
				{ "BSImagespaceShaderISLvl0PreTest", RE::ImageSpaceManager::GetCurrentIndex(ISLvl0PreTest) },
				{ "BSImagespaceShaderISSetupPreTest", RE::ImageSpaceManager::GetCurrentIndex(ISSetupPreTest) },
			};

			auto it = descriptors.find(imagespaceShader.name);
			if (it == descriptors.cend()) {
				return false;
			}
			descriptor = it->second;
			return true;
		}

		static bool ResolveImageSpaceDescriptor(const RE::BSShader& shader, uint32_t& descriptor)
		{
			if (shader.shaderType == RE::BSShader::Type::ImageSpace) {
				const auto& isShader = static_cast<const RE::BSImagespaceShader&>(shader);
				return GetImagespaceShaderDescriptor(isShader, descriptor);
			}
			return true;
		}
	}

	RE::BSGraphics::VertexShader* ShaderCache::GetVertexShader(const RE::BSShader& shader,
		uint32_t descriptor, bool a_precompile, bool forceAsync)
	{
		if (a_precompile && backgroundCompilation)
			return nullptr;
		if (!SShaderCache::ResolveImageSpaceDescriptor(shader, descriptor)) {
			return nullptr;
		}

		auto state = globals::state;

		if (!((ShaderCache::IsSupportedShader(shader) || state->IsDeveloperMode() && state->IsShaderEnabled(shader)) && state->enableVShaders)) {
			return nullptr;
		}

		if (state->IsDeveloperMode()) {
			// Track this shader as active
			TrackActiveShader(ShaderClass::Vertex, shader, descriptor);

			// Only build the key when a shader is blocked; it is a full defines string, too costly for every draw.
			if (blockedKeyIndex != -1 && !blockedKey.empty()) {
				auto key = SIE::SShaderCache::GetShaderString(ShaderClass::Vertex, shader, descriptor, true);
				if (key == blockedKey) {
					if (std::find(blockedIDs.begin(), blockedIDs.end(), descriptor) == blockedIDs.end()) {
						blockedIDs.push_back(descriptor);
						logger::debug("Skipping blocked shader {:X}:{} total: {}", descriptor, blockedKey, blockedIDs.size());
					}
					return nullptr;
				}
			}
		}

		{
			std::lock_guard lockGuard(vertexShadersMutex);
			auto& typeCache = vertexShaders[static_cast<size_t>(shader.shaderType.underlying())];
			auto it = typeCache.find(descriptor);
			if (it != typeCache.end()) {
				return it->second.get();
			}
		}

		if (forceAsync || (IsAsync() && !(shader.shaderType.get() == RE::BSShader::Type::Lighting &&
											(descriptor & static_cast<uint32_t>(LightingShaderFlags::EnvironmentProbe))))) {
			compilationSet.Add({ ShaderClass::Vertex, shader, descriptor, a_precompile });
		} else {
			return MakeAndAddVertexShader(shader, descriptor);
		}

		return nullptr;
	}

	RE::BSGraphics::PixelShader* ShaderCache::GetPixelShader(const RE::BSShader& shader,
		uint32_t descriptor, bool a_precompile, bool forceAsync)
	{
		if (a_precompile && backgroundCompilation)
			return nullptr;
		auto state = globals::state;

		if (!((ShaderCache::IsSupportedShader(shader) || state->IsDeveloperMode() && state->IsShaderEnabled(shader)) && state->enablePShaders)) {
			return nullptr;
		}

		if (!SShaderCache::ResolveImageSpaceDescriptor(shader, descriptor)) {
			return nullptr;
		}

		if (state->IsDeveloperMode()) {
			// Track this shader as active
			TrackActiveShader(ShaderClass::Pixel, shader, descriptor);

			// Only build the key when a shader is blocked; it is a full defines string, too costly for every draw.
			if (blockedKeyIndex != -1 && !blockedKey.empty()) {
				auto key = SIE::SShaderCache::GetShaderString(ShaderClass::Pixel, shader, descriptor, true);
				if (key == blockedKey) {
					if (std::find(blockedIDs.begin(), blockedIDs.end(), descriptor) == blockedIDs.end()) {
						blockedIDs.push_back(descriptor);
						logger::debug("Skipping blocked shader {:X}:{} total: {}", descriptor, blockedKey, blockedIDs.size());
					}
					return nullptr;
				}
			}
		}

		{
			std::lock_guard lockGuard(pixelShadersMutex);
			auto& typeCache = pixelShaders[static_cast<size_t>(shader.shaderType.underlying())];
			auto it = typeCache.find(descriptor);
			if (it != typeCache.end()) {
				return it->second.get();
			}
		}

		if (forceAsync || (IsAsync() && !(shader.shaderType.get() == RE::BSShader::Type::Lighting &&
											(descriptor & static_cast<uint32_t>(LightingShaderFlags::EnvironmentProbe))))) {
			compilationSet.Add({ ShaderClass::Pixel, shader, descriptor, a_precompile });
		} else {
			return MakeAndAddPixelShader(shader, descriptor);
		}

		return nullptr;
	}

	bool ShaderCache::HasPixelShaders(RE::BSShader::Type type, std::span<const uint32_t> descriptors)
	{
		if (type <= RE::BSShader::Type::None || type >= RE::BSShader::Type::Total)
			return false;
		std::lock_guard lockGuard(pixelShadersMutex);
		const auto& typeCache = pixelShaders[static_cast<size_t>(type)];
		return std::ranges::all_of(descriptors, [&](uint32_t descriptor) {
			const auto it = typeCache.find(descriptor);
			return it != typeCache.end() && it->second && it->second->shader;
		});
	}

	RE::BSGraphics::ComputeShader* ShaderCache::GetComputeShader(const RE::BSShader& shader,
		uint32_t descriptor)
	{
		auto state = globals::state;
		if (!((ShaderCache::IsSupportedShader(shader) || state->IsDeveloperMode() && state->IsShaderEnabled(shader)) && state->enableCShaders)) {
			return nullptr;
		}

		if (!SShaderCache::ResolveImageSpaceDescriptor(shader, descriptor)) {
			return nullptr;
		}

		if (state->IsDeveloperMode()) {
			// Track this shader as active
			TrackActiveShader(ShaderClass::Compute, shader, descriptor);

			// Only build the key when a shader is blocked; it is a full defines string, too costly for every draw.
			if (blockedKeyIndex != -1 && !blockedKey.empty()) {
				auto key = SIE::SShaderCache::GetShaderString(ShaderClass::Compute, shader, descriptor, true);
				if (key == blockedKey) {
					if (std::find(blockedIDs.begin(), blockedIDs.end(), descriptor) == blockedIDs.end()) {
						blockedIDs.push_back(descriptor);
						logger::debug("Skipping blocked shader {:X}:{} total: {}", descriptor, blockedKey, blockedIDs.size());
					}
					return nullptr;
				}
			}
		}

		{
			std::lock_guard lockGuard(computeShadersMutex);
			auto& typeCache = computeShaders[static_cast<size_t>(shader.shaderType.underlying())];
			auto it = typeCache.find(descriptor);
			if (it != typeCache.end()) {
				return it->second.get();
			}
		}

		if (IsAsync()) {
			compilationSet.Add({ ShaderClass::Compute, shader, descriptor });
		} else {
			return MakeAndAddComputeShader(shader, descriptor);
		}

		return nullptr;
	}

	ShaderCache::~ShaderCache()
	{
		Clear();
		StopFileWatcher();
		// Signal management thread to stop dispatching; pool workers observe the same
		// stop token and will not pick up new tasks after current compilations finish.
		HANDLE managementHandle = managementJthread.native_handle();
		managementJthread.request_stop();
		// Purge unstarted tasks so we only wait for compilations already in flight.
		compilationPool.purge();
		if (!compilationPool.wait_for(std::chrono::milliseconds(1000))) {
			logger::info("Tasks still running despite request to stop; killing management thread {}!", GetThreadId(managementHandle));
			WaitForSingleObject(managementHandle, 1000);
			TerminateThread(managementHandle, 0);
		}
	}

	void ShaderCache::Reload(const std::function<void()>& activate)
	{
		const bool watch = UseFileWatcher();
		if (watch)
			StopFileWatcher();
		managementJthread.request_stop();
		if (managementJthread.joinable())
			managementJthread.join();
		compilationPool.wait();
		activate();
		Clear();
		managementJthread = std::jthread([this](std::stop_token token) { ManageCompilationSet(token); });
		if (watch) {
			useFileWatcher = true;
			StartFileWatcher();
		}
	}

	void ShaderCache::Clear()
	{
		EnvironmentProbe::ClearShaderCache();
		// A clear asks for shaders to be built from what is on disk now, so the store must read the files again.
		Util::BeginShaderStoreGeneration();
		auto* state = globals::state;
		if (globals::game::currentVertexShader && *globals::game::currentVertexShader == state->customVertexShader)
			*globals::game::currentVertexShader = nullptr;
		if (globals::game::currentPixelShader && *globals::game::currentPixelShader == state->customPixelShader)
			*globals::game::currentPixelShader = nullptr;
		state->customVertexShader = nullptr;
		state->customPixelShader = nullptr;
		{
			std::lock_guard lockGuardV(vertexShadersMutex);
			for (auto& shaders : vertexShaders) {
				for (auto& [id, shader] : shaders) {
					shader->shader->Release();
				}
				shaders.clear();
			}
		}
		{
			std::lock_guard lockGuardP(pixelShadersMutex);
			for (auto& shaders : pixelShaders) {
				for (auto& [id, shader] : shaders) {
					shader->shader->Release();
				}
				shaders.clear();
			}
		}
		{
			std::lock_guard lockGuardC(computeShadersMutex);
			for (auto& shaders : computeShaders) {
				for (auto& [id, shader] : shaders) {
					shader->shader->Release();
				}
				shaders.clear();
			}
		}
		{
			std::unique_lock lockM{ mapMutex };
			shaderMap.clear();
		}
		{
			std::unique_lock lockH{ hlslMapMutex };
			hlslToShaderMap.clear();
		}
		ResetActiveShaderTracking();
		compilationSet.Clear();
		Util::ClearShaderCompileFailures();
		globals::deferred->ClearShaderCache();
		for (auto* feature : Feature::GetFeatureList()) {
			if (feature->loaded) {
				feature->ClearShaderCache();
			}
		}
	}

	template <typename ShaderType, typename MutexType>
	void ReleaseShader(ShaderType& shaders,
		MutexType& mutex, RE::BSShader::Type type, uint32_t descriptor)
	{
		std::lock_guard<MutexType> lockGuard(mutex);

		if (static_cast<size_t>(type) < shaders.size()) {
			auto& shaderMap = shaders[static_cast<size_t>(type)];
			auto shaderIt = shaderMap.find(descriptor);
			if (shaderIt != shaderMap.end()) {
				auto& shaderPtr = shaderIt->second;
				if (shaderPtr && shaderPtr->shader) {
					shaderPtr->shader->Release();
				}
				shaderMap.erase(shaderIt);
			}
		}
	}
	bool ShaderCache::Clear(const std::string& a_path)
	{
		EnvironmentProbe::ClearShaderCache();
		ResetActiveShaderTracking();
		std::string lowerFilePath = Util::FixFilePath(a_path);
		if (lowerFilePath.ends_with("environmentprobegeometry.hlsli"))
			return true;

		// Step 1: Lock hlslMapMutex to find and copy the relevant entries
		std::set<hlslRecord> entries;
		{
			std::unique_lock lockH{ hlslMapMutex };
			auto it = hlslToShaderMap.find(lowerFilePath);

			if (it == hlslToShaderMap.end()) {
				return false;
			}

			entries = it->second;  // Copy the entries
			hlslToShaderMap.erase(it);
		}

		// Step 2: Process the copied entries without holding hlslMapMutex
		for (auto& entry : entries) {
			// Remove shader key from shaderMap
			{
				std::unique_lock lockM{ mapMutex };
				shaderMap.erase(entry.key);
			}

			// Handle vertex, pixel, and compute shaders (each will lock)
			switch (entry.shaderClass) {
			case SIE::ShaderClass::Vertex:
				ReleaseShader(vertexShaders, vertexShadersMutex, entry.type, entry.descriptor);
				break;
			case SIE::ShaderClass::Pixel:
				ReleaseShader(pixelShaders, pixelShadersMutex, entry.type, entry.descriptor);
				break;
			case SIE::ShaderClass::Compute:
				ReleaseShader(computeShaders, computeShadersMutex, entry.type, entry.descriptor);
				break;
			default:
				logger::warn("Unexpected shader class: {}", static_cast<int>(entry.shaderClass));
				break;
			}

			logger::debug("Marking recompile for shader: {}", entry.key);
		}

		if (!entries.empty()) {
			logger::debug("Marked {} entries for recompile due to change to {}", entries.size(), a_path);
			compilationSet.Clear();
		}

		return true;
	}

	void ShaderCache::Clear(RE::BSShader::Type a_type)
	{
		if (a_type == RE::BSShader::Type::Lighting)
			EnvironmentProbe::ClearShaderCache();
		logger::debug("Clearing cache for {}", magic_enum::enum_name(a_type));
		ResetActiveShaderTracking();
		std::lock_guard lockGuardV(vertexShadersMutex);
		{
			for (auto& [id, shader] : vertexShaders[static_cast<size_t>(a_type)]) {
				shader->shader->Release();
			}
			vertexShaders[static_cast<size_t>(a_type)].clear();
		}
		std::lock_guard lockGuardP(pixelShadersMutex);
		{
			for (auto& [id, shader] : pixelShaders[static_cast<size_t>(a_type)]) {
				shader->shader->Release();
			}
			pixelShaders[static_cast<size_t>(a_type)].clear();
		}
		std::lock_guard lockGuardC(computeShadersMutex);
		{
			for (auto& [id, shader] : computeShaders[static_cast<size_t>(a_type)]) {
				shader->shader->Release();
			}
			computeShaders[static_cast<size_t>(a_type)].clear();
		}
		ClearShaderMap(a_type);
		compilationSet.Clear();
	}

	bool ShaderCache::AddCompletedShader(ShaderClass shaderClass, const RE::BSShader& shader, uint32_t descriptor, winrt::com_ptr<ID3DBlob> a_blob, bool fromDisk)
	{
		auto key = SIE::SShaderCache::GetShaderString(shaderClass, shader, descriptor, true);
		auto status = a_blob ? ShaderCompilationTask::Status::Completed : ShaderCompilationTask::Status::Failed;
		// Building the descriptor string costs more than the log call, so only do it when debug logging is on.
		if (spdlog::should_log(spdlog::level::debug))
			logger::debug("Adding {} shader to map: {}", magic_enum::enum_name(status), SIE::SShaderCache::GetShaderString(shaderClass, shader, descriptor, false));
		// The map's blob is shared across threads; the caller's d3dcompiler blob stays with the caller.
		winrt::com_ptr<ID3DBlob> shared;
		if (a_blob)
			shared.attach(new SharedBlob(*a_blob));
		{
			std::unique_lock lockM{ mapMutex };
			shaderMap.insert_or_assign(key, ShaderCacheResult{ std::move(shared), status, system_clock::now(), fromDisk });
		}
		mapCV.notify_all();  // wake threads waiting on a Pending→Completed/Failed transition
		const std::wstring path = SIE::SShaderCache::GetShaderPath(
			shader.shaderType == RE::BSShader::Type::ImageSpace ?
				static_cast<const RE::BSImagespaceShader&>(shader).originalShaderName :
				shader.fxpFilename);
		auto pathString = Util::WStringToString(path);
		// Always create or update an hlsl->shader record so failing compiles are
		// trackable and can be invalidated by the file watcher. This allows
		// Clear(path) to find failed shaders and mark them for recompilation.
		std::string lowerFilePath = Util::FixFilePath(pathString);
		{
			std::unique_lock lockH{ hlslMapMutex };
			auto it = hlslToShaderMap.find(lowerFilePath);
			hlslRecord newRecord{ key, shader.shaderType.get(), descriptor, shaderClass };

			if (it != hlslToShaderMap.end()) {
				auto& entries = it->second;

				// Find and remove existing record with the same key
				auto existingRecord = std::find_if(entries.begin(), entries.end(),
					[&](const hlslRecord& r) { return r.key == key; });

				if (existingRecord != entries.end()) {
					entries.erase(existingRecord);  // Remove the old record
				}

				// Insert the new or updated record
				entries.insert(newRecord);
			} else {
				// Create a new entry in hlslToShaderMap for this file path
				hlslToShaderMap.emplace(lowerFilePath, std::set<hlslRecord>{ newRecord });
			}
		}

		return a_blob != nullptr;
	}

	std::pair<ShaderCache::ClaimResult, winrt::com_ptr<ID3DBlob>> ShaderCache::ClaimCompilation(const std::string& key)
	{
		std::unique_lock lockM{ mapMutex };

		for (;;) {
			auto it = shaderMap.find(key);
			if (it != shaderMap.end()) {
				auto& entry = it->second;
				if (entry.status == ShaderCompilationTask::Status::Completed) {
					if (entry.blob) {
						logger::debug("Shader already compiled; using cache: {}", key);
						return { ClaimResult::CacheHit, entry.blob };
					}
					break;  // Completed with nullptr blob — re-compile
				}
				if (entry.status == ShaderCompilationTask::Status::Failed) {
					return { ClaimResult::Failed, nullptr };
				}
				// Status is Pending — another thread is compiling this shader.
				logger::debug("Shader compilation in progress, waiting: {}", key);
				mapCV.wait(lockM);
				continue;  // re-check after wakeup
			}
			break;  // not in map at all
		}

		// Claim the slot as Pending before releasing the lock
		shaderMap.insert_or_assign(key, ShaderCacheResult{ nullptr, ShaderCompilationTask::Status::Pending, system_clock::now() });
		return { ClaimResult::Claimed, nullptr };
	}

	void ShaderCache::ResolvePendingFailure(const std::string& key)
	{
		bool changed = false;
		{
			std::unique_lock lockM{ mapMutex };
			auto it = shaderMap.find(key);
			if (it != shaderMap.end() && it->second.status == ShaderCompilationTask::Status::Pending) {
				it->second = ShaderCacheResult{ nullptr, ShaderCompilationTask::Status::Failed, system_clock::now() };
				changed = true;
			}
		}
		if (changed) {
			mapCV.notify_all();
		}
	}

	winrt::com_ptr<ID3DBlob> ShaderCache::GetCompletedShader(const std::string& a_key)
	{
		std::string type = SIE::SShaderCache::GetTypeFromShaderString(a_key);
		UpdateShaderModifiedTime(type);
		std::scoped_lock lockM{ mapMutex };
		if (!shaderMap.empty() && shaderMap.contains(a_key)) {
			if (ShaderModifiedSince(type, shaderMap.at(a_key).compileTime)) {
				logger::debug("Shader {} compiled {} before changes at {}",
					a_key,
					std::format("{:%H:%M:%S}", shaderMap.at(a_key).compileTime),
					std::format("{:%H:%M:%S}", GetModifiedShaderMapTime(type)));
				return nullptr;
			}
			auto status = shaderMap.at(a_key).status;
			if (status != ShaderCompilationTask::Status::Pending)
				return shaderMap.at(a_key).blob;
		}
		return nullptr;
	}

	winrt::com_ptr<ID3DBlob> ShaderCache::GetCompletedShader(ShaderClass shaderClass, const RE::BSShader& shader,
		uint32_t descriptor)
	{
		auto key = SIE::SShaderCache::GetShaderString(shaderClass, shader, descriptor, true);
		return GetCompletedShader(key);
	}

	winrt::com_ptr<ID3DBlob> ShaderCache::GetCompletedShader(const ShaderCompilationTask& a_task)
	{
		auto key = a_task.GetString();
		return GetCompletedShader(key);
	}

	bool ShaderCache::IsShaderLoadedFromDisk(const std::string& a_key)
	{
		std::scoped_lock lockM{ mapMutex };
		auto it = shaderMap.find(a_key);
		if (it != shaderMap.end())
			return it->second.loadedFromDisk;
		return false;
	}

	ShaderCompilationTask::Status ShaderCache::GetShaderStatus(const std::string& a_key)
	{
		std::scoped_lock lockM{ mapMutex };
		if (!shaderMap.empty() && shaderMap.contains(a_key)) {
			return shaderMap.at(a_key).status;
		}
		return ShaderCompilationTask::Status::Pending;
	}

	std::string ShaderCache::GetShaderStatsString(bool a_timeOnly, bool a_elapsedOnly)
	{
		return compilationSet.GetStatsString(a_timeOnly, a_elapsedOnly);
	}

	inline bool ShaderCache::IsShaderSourceAvailable(const RE::BSShader& shader)
	{
		const std::wstring path = SIE::SShaderCache::GetShaderPath(shader.fxpFilename);

		std::string strPath;
		std::transform(path.begin(), path.end(), std::back_inserter(strPath), [](wchar_t c) {
			return (char)c;
		});
		try {
			return std::filesystem::exists(path);
		} catch (const std::filesystem::filesystem_error& e) {
			logger::warn("Error accessing {} : {}", strPath, e.what());
			return false;
		}
	}

	bool ShaderCache::IsCompiling()
	{
		// ORG requests retained before renderer initialization also run in this pool.
		// Keep the initial compilation gate up until those stage futures have completed.
		return (compilationSet.totalTasks && compilationSet.completedTasks + compilationSet.failedTasks < compilationSet.totalTasks) ||
		       compilationPool.get_tasks_total() != 0;
	}

	void ShaderCache::StopCompilation()
	{
		if (IsCompiling()) {
			logger::info("Stopping {} remaining shader compilation tasks", compilationSet.totalTasks - compilationSet.completedTasks - compilationSet.failedTasks);
		}
		ssource.request_stop();            // signals any legacy stop_token users
		managementJthread.request_stop();  // stops management thread + in-flight compilations
		compilationSet.Clear();
	}

	bool ShaderCache::IsEnabled() const
	{
		return isEnabled;
	}

	void ShaderCache::SetEnabled(bool value)
	{
		isEnabled = value;
	}

	bool ShaderCache::IsAsync() const
	{
		return isAsync;
	}

	void ShaderCache::SetAsync(bool value)
	{
		isAsync = value;
	}

	bool ShaderCache::IsDump() const
	{
		return isDump;
	}

	void ShaderCache::SetDump(bool value)
	{
		isDump = value;
	}

	void ShaderCache::EnqueueStandaloneShaderCompile(
		std::wstring sourcePath,
		std::string entryPoint,
		std::vector<std::pair<const char*, const char*>> defines,
		StandaloneShaderClass shaderClass,
		StandaloneShaderReadyCallback onReady)
	{
		for (auto* feature : Feature::GetFeatureList()) {
			if (!feature->loaded)
				continue;
			for (const auto& [name, value] : feature->GetCommonShaderDefines()) {
				if (std::ranges::none_of(defines, [&](const auto& macro) { return macro.first && name == macro.first; }))
					defines.emplace_back(name.data(), value.empty() ? nullptr : value.data());
			}
		}
		compilationSet.EnqueueAux(
			[this, sourcePath = std::move(sourcePath), entryPoint = std::move(entryPoint),
				defines = std::move(defines), shaderClass, onReady = std::move(onReady)]() mutable {
				auto device = globals::d3d::device;
				auto* state = globals::state;
				if (!device || !state) {
					onReady(nullptr);
					return;
				}

				const char* stageName;
				const char* profile;
				switch (shaderClass) {
				case StandaloneShaderClass::Vertex:
					stageName = "vertex";
					profile = "vs_5_0";
					break;
				case StandaloneShaderClass::Pixel:
					stageName = "pixel";
					profile = "ps_5_0";
					break;
				default:
					stageName = "compute";
					profile = "cs_5_0";
					break;
				}

				const std::filesystem::path srcPath{ sourcePath };
				const std::string srcPathStr = Util::WStringToString(sourcePath);

				ID3D11DeviceChild* shader = nullptr;

				auto createShader = [&](ID3DBlob* blob) -> HRESULT {
					switch (shaderClass) {
					case StandaloneShaderClass::Vertex:
						{
							ID3D11VertexShader* created = nullptr;
							HRESULT hr = device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &created);
							shader = created;
							return hr;
						}
					case StandaloneShaderClass::Pixel:
						{
							ID3D11PixelShader* created = nullptr;
							HRESULT hr = device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &created);
							shader = created;
							return hr;
						}
					default:
						{
							ID3D11ComputeShader* created = nullptr;
							HRESULT hr = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &created);
							shader = created;
							return hr;
						}
					}
				};

				const bool debugCompile = IsDebugCompile();
				std::vector<D3D_SHADER_MACRO> macros;
				for (const auto& d : defines) {
					if (d.first && d.first[0])
						macros.push_back({ d.first, d.second });
				}
				if (debugCompile) {
					macros.push_back({ "D3DCOMPILE_SKIP_OPTIMIZATION", "" });
					macros.push_back({ "D3DCOMPILE_DEBUG", "" });
				}
				auto shaderDefines = state->GetDefines();
				if (!shaderDefines->empty()) {
					for (unsigned int i = 0; i < shaderDefines->size(); i++)
						macros.push_back({ shaderDefines->at(i).first.c_str(), shaderDefines->at(i).second.c_str() });
				}
				if (shaderClass == StandaloneShaderClass::Compute)
					macros.push_back({ "COMPUTESHADER", "" });
				else if (shaderClass == StandaloneShaderClass::Pixel)
					macros.push_back({ "PSHADER", "" });
				else
					macros.push_back({ "VSHADER", "" });
				macros.push_back({ "WINPC", "" });
				macros.push_back({ "DX11", "" });
				macros.push_back({ nullptr, nullptr });

				Util::FxcRequest request;
				request.root = srcPath;
				request.includeBase = L"Data\\Shaders";
				request.defines = macros.data();
				request.entryPoint = entryPoint.c_str();
				request.profile = profile;
				request.flags = Util::FxcCompileFlags(debugCompile);
				request.debug = debugCompile;

				auto built = Util::ResolveFxcShader(request);
				if (built.blob && FAILED(createShader(built.blob.get())) && built.store.outcome != org::services::ShaderStoreOutcome::Compiled) {
					// A whole artifact can still hold bytecode the device rejects. Drop it and build from source once.
					logger::warn("Failed to create {} shader from the stored copy of {}:{}", stageName, srcPathStr, entryPoint);
					Util::SharedShaderStore()->RemoveArtifact(built.store.compileKey);
					built = Util::ResolveFxcShader(request);
					if (built.blob)
						createShader(built.blob.get());
				}
				if (!built.blob) {
					logger::warn("Standalone {} shader compilation failed for {}:{}:\n{}", stageName, srcPathStr, entryPoint, built.store.diagnostics);
					onReady(nullptr);
					return;
				}
				if (!built.store.diagnostics.empty())
					logger::debug("Shader logs:\n{}", built.store.diagnostics);
				if (!shader) {
					logger::warn("Failed to create {} shader for {}:{}", stageName, srcPathStr, entryPoint);
					onReady(nullptr);
					return;
				}
				Util::SetResourceName(shader, "%s:%s", srcPathStr.c_str(), entryPoint.c_str());

				onReady(shader);
			});
	}

	void ShaderCache::EnqueueComputeShaderCompile(
		std::wstring sourcePath,
		std::string entryPoint,
		std::vector<std::pair<const char*, const char*>> defines,
		ComputeShaderReadyCallback onReady)
	{
		EnqueueStandaloneShaderCompile(std::move(sourcePath), std::move(entryPoint), std::move(defines),
			StandaloneShaderClass::Compute,
			[onReady = std::move(onReady)](ID3D11DeviceChild* shader) {
				onReady(static_cast<ID3D11ComputeShader*>(shader));
			});
	}

	void ShaderCache::ReloadShaderSources()
	{
		Util::BeginShaderStoreGeneration();
	}

	bool ShaderCache::IsDiskCache() const
	{
		return isDiskCache;
	}

	void ShaderCache::SetDiskCache(bool value)
	{
		isDiskCache = value;
	}

	bool ShaderCache::IsDebugCompile()
	{
		return globals::state->IsDeveloperMode() || RenderDoc::IsLoaded();
	}

	void ShaderCache::SetBackgroundCompilation(bool value)
	{
		{
			// Serialize with WaitTake's predicate check and transition into wait.
			std::scoped_lock lock{ compilationSet.compilationMutex };
			backgroundCompilation = value;
			if (value) {
				// Background mode compiles on demand: drop startup precompiles no draw has asked for yet.
				// Demanded tasks and work already started are kept.
				for (auto it = compilationSet.availableTasks.begin(); it != compilationSet.availableTasks.end();) {
					if (!it->IsPrecompile()) {
						++it;
						continue;
					}
					--compilationSet.totalTasks;
					compilationSet.totalPriorityWeight -= static_cast<uint64_t>(it->GetPriority()) + 1;
					it = compilationSet.availableTasks.erase(it);
				}
				if (compilationSet.totalTasks != 0 &&
					compilationSet.completedTasks + compilationSet.failedTasks >= compilationSet.totalTasks &&
					compilationSet.completionTime.load(std::memory_order_relaxed) == 0) {
					LARGE_INTEGER now;
					QueryPerformanceCounter(&now);
					compilationSet.completionTime.store(now.QuadPart, std::memory_order_relaxed);
				}
			}
		}
		compilationSet.conditionVariable.notify_one();
	}

	void ShaderCache::DeleteDiskCache()
	{
		std::scoped_lock lock{ compilationSet.compilationMutex };
		// Through the store: it holds the index in memory too, and would save it back.
		Util::SharedShaderStore()->Clear();
		std::error_code ec;
		for (const auto& entry : std::filesystem::directory_iterator(L"Data/ShaderCache", ec)) {
			if (entry.path().filename().wstring() == Util::kShaderStoreDirName)
				continue;
			std::error_code removeError;
			std::filesystem::remove_all(entry.path(), removeError);
			if (removeError)
				logger::error("Failed to delete disk cache entry {}: {}", Util::WStringToString(entry.path().wstring()), removeError.message());
		}
		logger::info("Deleted disk cache");
	}

	ShaderCache::ShaderCache()
	{
		dependencyTracker = std::make_unique<ShaderFileDependencyTracker>();
		logger::debug("ShaderCache initialized: {} startup threads, {} background threads, {} pool threads",
			(int)compilationThreadCount, (int)backgroundCompilationThreadCount, (int)compilationPool.get_thread_count());
		// Management thread runs on a dedicated jthread, not in the compilation pool,
		// so it doesn't consume a pool slot that could be used for shader compilation.
		managementJthread = std::jthread([this](std::stop_token stoken) {
			ManageCompilationSet(stoken);
		});
	}

	bool ShaderCache::UseFileWatcher() const
	{
		return useFileWatcher;
	}

	void ShaderCache::SetFileWatcher(bool value)
	{
		auto oldValue = useFileWatcher;
		useFileWatcher = value;
		if (useFileWatcher && !oldValue)
			StartFileWatcher();
		else if (!useFileWatcher && oldValue)
			StopFileWatcher();
	}

	void ShaderCache::StartFileWatcher()
	{
		logger::info("Starting FileWatcher");
		if (!fileWatcher) {
			fileWatcher = new efsw::FileWatcher();
			listener = new UpdateListener(dependencyTracker.get());
			// Add a folder to watch, and get the efsw::WatchID
			// Reporting the files and directories changes to the instance of the listener
			watchID = fileWatcher->addWatch("Data\\Shaders", listener, true);
			// Start watching asynchronously the directories
			fileWatcher->watch();
			std::string pathStr = "";
			for (auto path : fileWatcher->directories()) {
				pathStr += std::format("{}; ", path);
			}
			logger::debug("ShaderCache watching for changes in {}", pathStr);
			// Capture listener by value so the thread does not race with StopFileWatcher()
			// nulling this->listener before the thread has had a chance to start.
			auto* capturedListener = listener;
			capturedListener->fileWatcherThread = std::jthread([capturedListener]() {
				capturedListener->processQueue();
			});
		} else {
			logger::debug("ShaderCache already enabled");
		}
	}

	void ShaderCache::StopFileWatcher()
	{
		logger::info("Stopping FileWatcher");
		// Set flag first so processQueue()'s loop condition becomes false before we join.
		useFileWatcher = false;
		if (fileWatcher) {
			fileWatcher->removeWatch(watchID);
			delete fileWatcher;
			fileWatcher = nullptr;
		}
		if (listener) {
			delete listener;
			listener = nullptr;
		}
	}

	bool ShaderCache::UpdateShaderModifiedTime(const std::string& a_type, boolean a_forceUpdate)
	{
		if (!UseFileWatcher())
			return false;
		// Validate the shader type
		if (a_type.empty() || !magic_enum::enum_cast<RE::BSShader::Type>(a_type, magic_enum::case_insensitive).has_value()) {
			return false;  // Invalid type
		}

		std::lock_guard lockGuard(modifiedMapMutex);

		// Check for force update
		if (a_forceUpdate) {
			// Set an artificial timestamp far in the future (100 years)
			auto futureTime = std::chrono::system_clock::now() + std::chrono::hours(24 * 365 * 100);
			modifiedShaderMap.insert_or_assign(a_type, futureTime);
			return true;
		}

		// Otherwise, update with the actual file time
		std::filesystem::path filePath{ SIE::SShaderCache::GetShaderPath(a_type) };
		if (std::filesystem::exists(filePath)) {
			auto fileTime = std::chrono::clock_cast<std::chrono::system_clock>(std::filesystem::last_write_time(filePath));

			// Update only if timestamp has changed
			if (!modifiedShaderMap.contains(a_type) || modifiedShaderMap.at(a_type) != fileTime) {
				modifiedShaderMap.insert_or_assign(a_type, fileTime);
				return true;
			}
		}
		return false;
	}

	bool ShaderCache::ShaderModifiedSince(const std::string& a_type, std::chrono::system_clock::time_point a_current)
	{
		if (!UseFileWatcher())
			return false;
		// Validate the shader type
		if (a_type.empty() || !magic_enum::enum_cast<RE::BSShader::Type>(a_type, magic_enum::case_insensitive).has_value()) {
			return false;  // Invalid type
		}

		std::lock_guard lockGuard(modifiedMapMutex);

		// Check if the shader type exists in the map and if its modification time is newer than a_current
		return !modifiedShaderMap.empty() && modifiedShaderMap.contains(a_type) && modifiedShaderMap.at(a_type) > a_current;
	}

	namespace
	{
		// Names the next D3D11 shader this thread creates for capture tools (DXVK's dxvkSetNextShaderName, which Nsight Graphics then
		// shows as the stage's entry point): type, stage, descriptor and the descriptor's own defines, without the features' global ones.
		// Null clears it.
		void SetNextDxvkShaderName(ShaderClass a_class, const RE::BSShader* a_shader, uint32_t a_descriptor)
		{
			using SetNextShaderName = void(__stdcall*)(const char*);
			static const auto setName = []() -> SetNextShaderName {
				const auto module = ::GetModuleHandleW(L"dxvk_d3d11.dll");
				return module ? reinterpret_cast<SetNextShaderName>(::GetProcAddress(module, "dxvkSetNextShaderName")) : nullptr;
			}();
			if (!setName)
				return;
			if (!a_shader) {
				setName(nullptr);
				return;
			}

			const auto type = a_shader->shaderType.get();
			std::array<D3D_SHADER_MACRO, 64> defines{};
			SShaderCache::GetShaderDefines(*a_shader, a_descriptor, defines);
			std::string name = std::format("{} {} {:08X}", magic_enum::enum_name(type),
				a_class == ShaderClass::Vertex ? "vs" : a_class == ShaderClass::Pixel ? "ps" :
																						"cs",
				a_descriptor);
			for (const auto& define : defines) {
				if (!define.Name)
					break;
				const bool global = std::ranges::any_of(Feature::GetFeatureList(), [&](Feature* feature) {
					return feature->loaded && feature->HasShaderDefine(type) && feature->GetShaderDefineName() == define.Name;
				});
				if (!global)
					name += std::format(" {}", define.Name);
			}
			setName(name.c_str());
		}
	}

	RE::BSGraphics::VertexShader* ShaderCache::MakeAndAddVertexShader(const RE::BSShader& shader,
		uint32_t descriptor)
	{
		if (const auto shaderBlob =
				SShaderCache::CompileShader(ShaderClass::Vertex, shader, descriptor, dependencyTracker.get())) {
			auto device = globals::d3d::device;

			auto newShader = SShaderCache::CreateVertexShader(*shaderBlob, shader,
				descriptor);

			SetNextDxvkShaderName(ShaderClass::Vertex, &shader, descriptor);
			const auto result = device->CreateVertexShader(shaderBlob->GetBufferPointer(),
				newShader->byteCodeSize, nullptr, reinterpret_cast<ID3D11VertexShader**>(&newShader->shader));
			SetNextDxvkShaderName(ShaderClass::Vertex, nullptr, 0);
			if (FAILED(result)) {
				logger::error("Failed to create vertex shader {}::{:X}",
					magic_enum::enum_name(shader.shaderType.get()), descriptor);
				if (newShader->shader != nullptr) {
					newShader->shader->Release();
				}
			} else {
				std::lock_guard lockGuard(vertexShadersMutex);
				auto& entry = vertexShaders[static_cast<size_t>(shader.shaderType.get())][descriptor];
				if (entry && entry->shader)
					entry->shader->Release();
				entry = std::move(newShader);
				return entry.get();
			}
		}
		return nullptr;
	}

	RE::BSGraphics::PixelShader* ShaderCache::MakeAndAddPixelShader(const RE::BSShader& shader,
		uint32_t descriptor)
	{
		if (const auto shaderBlob =
				SShaderCache::CompileShader(ShaderClass::Pixel, shader, descriptor, dependencyTracker.get())) {
			auto device = globals::d3d::device;

			auto newShader = SShaderCache::CreatePixelShader(*shaderBlob, shader,
				descriptor);

			SetNextDxvkShaderName(ShaderClass::Pixel, &shader, descriptor);
			const auto result = device->CreatePixelShader(shaderBlob->GetBufferPointer(),
				shaderBlob->GetBufferSize(), nullptr, reinterpret_cast<ID3D11PixelShader**>(&newShader->shader));
			SetNextDxvkShaderName(ShaderClass::Pixel, nullptr, 0);
			if (FAILED(result)) {
				logger::error("Failed to create pixel shader {}::{:X}",
					magic_enum::enum_name(shader.shaderType.get()),
					descriptor);
				if (newShader->shader != nullptr) {
					newShader->shader->Release();
				}
			} else {
				if (shader.shaderType.get() == RE::BSShader::Type::ImageSpace)
					Util::SetResourceName(reinterpret_cast<ID3D11PixelShader*>(newShader->shader), "%s", static_cast<const RE::BSImagespaceShader&>(shader).name);
				else
					Util::SetResourceName(reinterpret_cast<ID3D11PixelShader*>(newShader->shader), "%s:%X", std::string(magic_enum::enum_name(shader.shaderType.get())).c_str(), descriptor);
				std::lock_guard lockGuard(pixelShadersMutex);
				auto& entry = pixelShaders[static_cast<size_t>(shader.shaderType.get())][descriptor];
				if (entry && entry->shader)
					entry->shader->Release();
				entry = std::move(newShader);
				return entry.get();
			}
		}
		return nullptr;
	}

	RE::BSGraphics::ComputeShader* ShaderCache::MakeAndAddComputeShader(const RE::BSShader& shader,
		uint32_t descriptor)
	{
		if (const auto shaderBlob =
				SShaderCache::CompileShader(ShaderClass::Compute, shader, descriptor, dependencyTracker.get())) {
			auto device = globals::d3d::device;

			auto newShader = SShaderCache::CreateComputeShader(*shaderBlob, shader,
				descriptor);

			SetNextDxvkShaderName(ShaderClass::Compute, &shader, descriptor);
			const auto result = device->CreateComputeShader(shaderBlob->GetBufferPointer(),
				shaderBlob->GetBufferSize(), nullptr, reinterpret_cast<ID3D11ComputeShader**>(&newShader->shader));
			SetNextDxvkShaderName(ShaderClass::Compute, nullptr, 0);
			if (FAILED(result)) {
				logger::error("Failed to create pixel shader {}::{:X}",
					magic_enum::enum_name(shader.shaderType.get()),
					descriptor);
				if (newShader->shader != nullptr) {
					newShader->shader->Release();
				}
			} else {
				if (shader.shaderType.get() == RE::BSShader::Type::ImageSpace)
					Util::SetResourceName(reinterpret_cast<ID3D11ComputeShader*>(newShader->shader), "%s", static_cast<const RE::BSImagespaceShader&>(shader).name);
				else
					Util::SetResourceName(reinterpret_cast<ID3D11ComputeShader*>(newShader->shader), "%s:%X", std::string(magic_enum::enum_name(shader.shaderType.get())).c_str(), descriptor);
				std::lock_guard lockGuard(computeShadersMutex);
				auto& entry = computeShaders[static_cast<size_t>(shader.shaderType.get())][descriptor];
				if (entry && entry->shader)
					entry->shader->Release();
				entry = std::move(newShader);
				return entry.get();
			}
		}
		return nullptr;
	}

	std::string ShaderCache::GetDefinesString(const RE::BSShader& shader, uint32_t descriptor)
	{
		std::array<D3D_SHADER_MACRO, 64> defines{};
		SIE::SShaderCache::GetShaderDefines(shader, descriptor, std::span{ defines });

		return SIE::SShaderCache::MergeDefinesString(defines, true);
	}

	std::vector<std::pair<std::string, std::string>> ShaderCache::GetCompileDefines(const RE::BSShader& shader, ShaderClass shaderClass, uint32_t descriptor)
	{
		// Same composition as the D3DCompile path in CompileShader.
		std::vector<std::pair<std::string, std::string>> result;
		if (shaderClass == ShaderClass::Vertex)
			result.emplace_back("VSHADER", "");
		else if (shaderClass == ShaderClass::Pixel)
			result.emplace_back("PSHADER", "");
		else if (shaderClass == ShaderClass::Compute)
			result.emplace_back("CSHADER", "");
		if (const auto stateDefines = globals::state->GetDefines()) {
			for (const auto& define : *stateDefines)
				result.push_back(define);
		}
		std::array<D3D_SHADER_MACRO, 64> defines{};
		SIE::SShaderCache::GetShaderDefines(shader, descriptor, std::span{ defines });
		for (const auto& define : defines) {
			if (!define.Name)
				break;
			result.emplace_back(define.Name, define.Definition ? define.Definition : "");
		}
		return result;
	}

	uint64_t ShaderCache::GetCachedHitTasks()
	{
		return compilationSet.cacheHitTasks;
	}
	uint64_t ShaderCache::GetCompletedTasks()
	{
		return compilationSet.completedTasks;
	}
	uint64_t ShaderCache::GetFailedTasks()
	{
		return compilationSet.failedTasks;
	}

	uint64_t ShaderCache::GetCurrentFailedCount()
	{
		std::scoped_lock lock(mapMutex);
		uint64_t count = 0;
		for (const auto& [key, result] : shaderMap) {
			if (result.status == ShaderCompilationTask::Status::Failed) {
				++count;
			}
		}
		return count;
	}

	uint64_t ShaderCache::GetTotalTasks()
	{
		return compilationSet.totalTasks;
	}
	uint64_t ShaderCache::GetDiskHitTasks()
	{
		return compilationSet.diskHitTasks;
	}
	void ShaderCache::IncCacheHitTasks()
	{
		compilationSet.cacheHitTasks++;
	}
	ShaderCache::ShaderStoreUsage ShaderCache::GetShaderStoreUsage()
	{
		const auto usage = Util::SharedShaderStore()->MeasureArtifacts();
		std::error_code ec;
		const std::filesystem::path relative = std::filesystem::path(L"Data/ShaderCache") / Util::kShaderStoreDirName;
		const auto absolute = std::filesystem::absolute(relative, ec);
		return { Util::WStringToString((ec ? relative : absolute).wstring()), usage.count, usage.bytes,
			static_cast<uint64_t>(globals::state->shaderStoreMaxMB.load(std::memory_order_relaxed)) << 20 };
	}

	bool ShaderCache::IsHideErrors()
	{
		return hideError;
	}

	int ShaderCache::GetHeavyTasksInFlight()
	{
		return static_cast<int>(compilationSet.heavyTasksInFlight.load(std::memory_order_relaxed));
	}

	uint64_t ShaderCache::GetSlowTasks()
	{
		return compilationSet.slowTasks.load(std::memory_order_relaxed);
	}

	uint64_t ShaderCache::GetVerySlowTasks()
	{
		return compilationSet.verySlowTasks.load(std::memory_order_relaxed);
	}

	std::vector<CompilationSet::SlowTaskRecord> CompilationSet::GetTopSlowTasks(size_t n) const
	{
		std::lock_guard lock(slowTasksMutex);
		// Partial sort to get the N highest without fully sorting the whole vector.
		std::vector<SlowTaskRecord> result = slowTaskRecords;
		if (result.size() > n) {
			std::partial_sort(result.begin(), result.begin() + n, result.end(),
				[](const SlowTaskRecord& a, const SlowTaskRecord& b) { return a.elapsedMs > b.elapsedMs; });
			result.resize(n);
		} else {
			std::sort(result.begin(), result.end(),
				[](const SlowTaskRecord& a, const SlowTaskRecord& b) { return a.elapsedMs > b.elapsedMs; });
		}
		return result;
	}

	std::vector<CompilationSet::SlowTaskRecord> ShaderCache::GetTopSlowTasks(size_t n)
	{
		return compilationSet.GetTopSlowTasks(n);
	}

	std::optional<CompilationSet::ParallelismStats> CompilationSet::GetParallelismStats() const
	{
		std::vector<SlowTaskRecord> records;
		{
			std::lock_guard lock(slowTasksMutex);
			if (slowTaskRecords.empty()) {
				return std::nullopt;
			}
			records = slowTaskRecords;
		}

		ParallelismStats stats;
		stats.sampleCount = records.size();
		for (const auto& rec : records) {
			stats.workMs += rec.elapsedMs;
			stats.spanMs = std::max(stats.spanMs, rec.elapsedMs);
			stats.avgQueueWaitMs += rec.queueWaitMs;
			stats.maxQueueWaitMs = std::max(stats.maxQueueWaitMs, rec.queueWaitMs);
		}
		stats.avgQueueWaitMs /= static_cast<double>(stats.sampleCount);

		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		int64_t endTime = completionTime.load(std::memory_order_relaxed);
		if (endTime == 0) {
			endTime = now.QuadPart;
		}
		stats.makespanMs = static_cast<double>(endTime - lastReset.QuadPart) * 1000.0 / frequency.QuadPart;

		if (stats.spanMs > 0.0) {
			stats.avgParallelism = stats.workMs / stats.spanMs;
		}
		if (stats.makespanMs > 0.0) {
			stats.infiniteCoreEfficiency = stats.spanMs / stats.makespanMs;
			stats.infiniteCoreGapPercent = std::max(0.0, 100.0 * (1.0 - stats.infiniteCoreEfficiency));
		}

		return stats;
	}

	std::optional<CompilationSet::ParallelismStats> ShaderCache::GetParallelismStats()
	{
		return compilationSet.GetParallelismStats();
	}

	void ShaderCache::ClearShaderMap(RE::BSShader::Type a_type)
	{
		std::string_view shaderTypeStr = magic_enum::enum_name(a_type);

		std::unique_lock lockM{ SIE::ShaderCache::mapMutex };
		logger::debug("Clearing shaderMap of {}", shaderTypeStr);
		for (auto it = shaderMap.begin(); it != shaderMap.end();) {
			auto typeInKey = SIE::SShaderCache::GetTypeFromShaderString(it->first);
			if (typeInKey == shaderTypeStr) {
				it = shaderMap.erase(it);
			} else {
				++it;
			}
		}
	}

	void ShaderCache::InsertModifiedShaderMap(const std::string& a_shader, std::chrono::time_point<std::chrono::system_clock> a_time)
	{
		std::lock_guard lockGuard(modifiedMapMutex);
		modifiedShaderMap.insert_or_assign(a_shader, a_time);
	}

	std::chrono::time_point<std::chrono::system_clock> ShaderCache::GetModifiedShaderMapTime(const std::string& a_shader)
	{
		std::lock_guard lockGuard(modifiedMapMutex);
		return modifiedShaderMap.at(a_shader);
	}

	void ShaderCache::ToggleErrorMessages()
	{
		hideError = !hideError;
	}

	void ShaderCache::IterateShaderBlock(bool a_forward)
	{
		// Try to use active shaders list if available in developer mode
		if (globals::state->IsDeveloperMode()) {
			std::lock_guard lockActive(activeShadersMutex);
			if (!activeShaders.empty()) {
				// Build sorted list of active shader keys
				std::vector<std::string> keys;
				keys.reserve(activeShaders.size());
				for (const auto& [key, _] : activeShaders) {
					keys.push_back(key);
				}
				std::sort(keys.begin(), keys.end());

				// Find current position or start
				int currentIdx = -1;
				if (!blockedKey.empty()) {
					auto it = std::find(keys.begin(), keys.end(), blockedKey);
					if (it != keys.end()) {
						currentIdx = static_cast<int>(std::distance(keys.begin(), it));
					}
				}

				// Calculate next index
				int targetIdx = 0;
				if (currentIdx >= 0) {
					targetIdx = a_forward ? (currentIdx + 1) % static_cast<int>(keys.size()) : (currentIdx - 1 + static_cast<int>(keys.size())) % static_cast<int>(keys.size());
				} else {
					targetIdx = a_forward ? 0 : static_cast<int>(keys.size()) - 1;
				}

				blockedKey = keys[targetIdx];
				blockedKeyIndex = -2;  // Set to -2 for dev selections to distinguish from shaderMap indices
				blockedIDs.clear();
				logger::debug("Blocking active shader ({}/{}) {}", targetIdx + 1, keys.size(), blockedKey);
				return;
			}
		}

		// Fallback to original behavior with full shader map
		std::scoped_lock lockM{ mapMutex };
		auto targetIndex = a_forward ? 0 : shaderMap.size() - 1;           // default start or last element
		if (blockedKeyIndex >= 0 && shaderMap.size() > blockedKeyIndex) {  // grab next element
			targetIndex = (blockedKeyIndex + (a_forward ? 1 : -1)) % shaderMap.size();
		}
		auto index = 0;
		for (auto& [key, value] : shaderMap) {
			if (index++ == targetIndex) {
				blockedKey = key;
				blockedKeyIndex = -1;
				blockedIDs.clear();
				logger::debug("Blocking shader ({}/{}) {}", blockedKeyIndex + 1, shaderMap.size(), blockedKey);
				return;
			}
		}
	}

	void ShaderCache::DisableShaderBlocking()
	{
		blockedKey = "";
		blockedKeyIndex = -1;
		blockedIDs.clear();
		logger::debug("Stopped blocking shaders");
	}

	void ShaderCache::TrackActiveShader(ShaderClass shaderClass, const RE::BSShader& shader, uint32_t descriptor)
	{
		if (!globals::state->IsDeveloperMode())
			return;

		std::lock_guard lock(activeShadersMutex);

		// Building the key formats and sorts the full defines list, so do it once per shader instead of every draw.
		const auto taskId = ShaderCompilationTask::MakeId(shaderClass, shader.shaderType.get(), descriptor);
		auto keyIt = activeShaderKeys.find(taskId);
		if (keyIt == activeShaderKeys.end())
			keyIt = activeShaderKeys.emplace(taskId, SIE::SShaderCache::GetShaderString(shaderClass, shader, descriptor, true)).first;
		const auto& key = keyIt->second;

		auto& info = activeShaders[key];
		if (info.key.empty()) {
			// First time seeing this shader
			info.key = key;
			info.shaderType = shader.shaderType.get();
			info.shaderClass = shaderClass;
			info.descriptor = descriptor;
		}

		info.isActive = true;
		info.drawCalls++;
		info.lastUsed = std::chrono::steady_clock::now();
	}

	void ShaderCache::ResetActiveShaderTracking()
	{
		std::lock_guard lock(activeShadersMutex);
		activeShaderKeys.clear();
		activeShaders.clear();
	}

	void ShaderCache::ResetFrameShaderTracking()
	{
		if (!globals::state->IsDeveloperMode())
			return;

		std::lock_guard lock(activeShadersMutex);

		// Mark all shaders as inactive for this frame
		// Keep shaders that were used recently (within last 60 frames / ~1 second at 60fps)
		auto now = std::chrono::steady_clock::now();
		auto timeout = std::chrono::seconds(1);

		for (auto it = activeShaders.begin(); it != activeShaders.end();) {
			auto& info = it->second;
			info.isActive = false;
			info.drawCalls = 0;

			// Remove shaders that haven't been used recently
			if (now - info.lastUsed > timeout) {
				it = activeShaders.erase(it);
			} else {
				++it;
			}
		}
	}

	std::vector<ShaderCache::ActiveShaderInfo> ShaderCache::GetActiveShaders() const
	{
		std::lock_guard lock(activeShadersMutex);
		std::vector<ActiveShaderInfo> result;
		result.reserve(activeShaders.size());

		for (const auto& [key, info] : activeShaders) {
			result.push_back(info);
		}

		return result;
	}

	void ShaderCache::ManageCompilationSet(std::stop_token stoken)
	{
		managementThread = GetCurrentThread();
		SetThreadPriority(managementThread, THREAD_PRIORITY_BELOW_NORMAL);
		while (!stoken.stop_requested()) {
			auto next = compilationSet.TryTakeNext(stoken);
			if (!next.has_value()) {
				if (stoken.stop_requested())
					break;  // exit because thread told to end
				continue;   // spurious wake or lost a race; re-check
			}
			std::visit([this, stoken](auto&& work) {
				using T = std::decay_t<decltype(work)>;
				if constexpr (std::is_same_v<T, ShaderCompilationTask>) {
					compilationPool.detach_task([this, stoken, t = work] { ProcessCompilationSet(stoken, t); });
				} else {
					compilationPool.detach_task([this, work = std::move(work)]() mutable {
						const SKSE::stl::scope_exit releaseSlot([this]() noexcept { compilationSet.ReleaseDispatchSlot(); });
						work();
					});
				}
			},
				std::move(*next));
		}
	}

	void ShaderCache::ProcessCompilationSet(std::stop_token stoken, SIE::ShaderCompilationTask task)
	{
		const SKSE::stl::scope_exit releaseSlot([this]() noexcept { compilationSet.ReleaseDispatchSlot(); });

		if (stoken.stop_requested()) {
			return;
		}

		const auto taskKey = task.GetString();

		// Run all shader compilation work at below-normal priority.
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

		LARGE_INTEGER start, end, freq;
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&start);
		const double queueWaitMs = task.GetEnqueuedQpc() > 0 ?
		                               static_cast<double>(start.QuadPart - task.GetEnqueuedQpc()) * 1000.0 / freq.QuadPart :
		                               0.0;

		try {
			task.Perform();
		} catch (const std::exception& e) {
			logger::error("Unhandled exception compiling shader task {}: {}", taskKey, e.what());
			ResolvePendingFailure(taskKey);
		} catch (...) {
			logger::error("Unhandled non-standard exception compiling shader task {}", taskKey);
			ResolvePendingFailure(taskKey);
		}

		QueryPerformanceCounter(&end);
		const double elapsedMs = static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / freq.QuadPart;
		// Use saturating math: without a lock, Clear() can zero totalTasks while completedTasks
		// still reads high briefly, which would otherwise underflow uint64_t (logs as ~2^64-1).
		const uint64_t total = compilationSet.totalTasks.load(std::memory_order_relaxed);
		const uint64_t done = compilationSet.completedTasks.load(std::memory_order_relaxed) +
		                      compilationSet.failedTasks.load(std::memory_order_relaxed);
		// This task has already finished running, but Complete(task) has not yet updated the counters.
		// Include the current task in the local progress snapshot so the logged remaining count is accurate.
		const uint64_t doneIncludingCurrent = (done < total) ? (done + 1) : total;
		const uint64_t remaining = (total > doneIncludingCurrent) ? (total - doneIncludingCurrent) : 0;

		// Proxy for permutation complexity: descriptor low 32 bits from GetId(); popcount = active defines.
		// Shader file size provides a secondary signal for source complexity.
		const auto descriptorComplexity = std::popcount(static_cast<uint32_t>(task.GetId()));
		uintmax_t sourceBytes = 0;
		{
			// GetString() format: "fxpFilename:ShaderClass:defines" — filename is before the first colon.
			const auto& taskStr = taskKey;
			const auto sep = taskStr.find(':');
			if (sep != std::string::npos) {
				const auto shaderName = taskStr.substr(0, sep);
				if (auto path = SIE::SShaderCache::GetShaderPath(shaderName); !path.empty()) {
					std::error_code ec;
					sourceBytes = std::filesystem::file_size(path, ec);
				}
			}
		}

		// Debug: full per-task record for post-mortem straggler analysis.
		logger::debug("[ShaderTiming] {:.0f}ms | queue_wait={:.0f}ms | remaining={} | defines={} | src={}B | prio={} | tid={} | {}",
			elapsedMs, queueWaitMs, remaining, descriptorComplexity, sourceBytes,
			task.GetPriority(), GetCurrentThreadId(), taskKey);

		constexpr double kSlowMs = 2000.0;
		constexpr double kVerySlowMs = 8000.0;

		// Record every task for post-mortem analysis and developer UI (top-N display).
		{
			std::lock_guard lock(compilationSet.slowTasksMutex);
			compilationSet.slowTaskRecords.push_back({ taskKey, elapsedMs, queueWaitMs, task.GetPriority(),
				static_cast<int>(descriptorComplexity), sourceBytes });
		}

		if (elapsedMs >= kVerySlowMs) {
			compilationSet.verySlowTasks++;
			compilationSet.slowTasks++;
			logger::info("[ShaderTiming] Very slow {:.0f}ms | queue_wait={:.0f}ms | remaining={} | defines={} | src={}B | prio={} | {}",
				elapsedMs, queueWaitMs, remaining, descriptorComplexity, sourceBytes, task.GetPriority(), taskKey);
		} else if (elapsedMs >= kSlowMs) {
			compilationSet.slowTasks++;
			logger::debug("[ShaderTiming] Slow {:.0f}ms | queue_wait={:.0f}ms | remaining={} | defines={} | src={}B | prio={} | {}",
				elapsedMs, queueWaitMs, remaining, descriptorComplexity, sourceBytes, task.GetPriority(), taskKey);
		}

		if (stoken.stop_requested()) {
			return;
		}

		compilationSet.Complete(task);
	}

	ShaderCompilationTask::ShaderCompilationTask(ShaderClass aShaderClass,
		const RE::BSShader& aShader,
		uint32_t aDescriptor, bool a_precompile) :
		shaderClass(aShaderClass),
		shader(aShader), descriptor(aDescriptor),
		cachedPriority(ComputePriority(aShaderClass, aShader, aDescriptor)),
		precompile(a_precompile)
	{}

	void ShaderCompilationTask::Perform() const
	{
		ZoneScoped;
#ifdef TRACY_ENABLE
		if (TracyIsConnected) {
			const auto taskString = GetString();
			ZoneText(taskString.c_str(), taskString.size());
		}
#endif

		if (shaderClass == ShaderClass::Vertex) {
			ShaderCache::Instance().MakeAndAddVertexShader(shader, descriptor);
		} else if (shaderClass == ShaderClass::Pixel) {
			ShaderCache::Instance().MakeAndAddPixelShader(shader, descriptor);
		} else if (shaderClass == ShaderClass::Compute) {
			ShaderCache::Instance().MakeAndAddComputeShader(shader, descriptor);
		}
		if (shaderClass == ShaderClass::Vertex || shaderClass == ShaderClass::Pixel) {
			try {
				DrawcallLimitFix::PrecompileShader(shader, shaderClass == ShaderClass::Pixel, descriptor);
			} catch (const std::exception& e) {
				// An optional ORG artifact must not turn a successful native compile into a failed task.
				logger::warn("[DCLF] ORG shader precompile failed: {}", e.what());
			}
		}
	}

	size_t ShaderCompilationTask::MakeId(ShaderClass a_shaderClass, RE::BSShader::Type a_type, uint32_t a_descriptor)
	{
		return a_descriptor + (static_cast<size_t>(a_type) << 32) +
		       (static_cast<size_t>(a_shaderClass) << 60);
	}

	size_t ShaderCompilationTask::GetId() const
	{
		return MakeId(shaderClass, shader.shaderType.get(), descriptor);
	}

	std::string ShaderCompilationTask::GetString() const
	{
		return SIE::SShaderCache::GetShaderString(shaderClass, shader, descriptor, true);
	}

	bool ShaderCompilationTask::operator==(const ShaderCompilationTask& other) const
	{
		return GetId() == other.GetId();
	}

	int ShaderCompilationTask::ComputePriority(ShaderClass shaderClass, const RE::BSShader& shader, uint32_t descriptor)
	{
		int priority = 0;
		const auto type = shader.shaderType.get();

		// Base priority by shader type — Lighting is consistently the slowest
		// (123KB source, 12s+ compile), followed by Effect (~31KB, up to 12s).
		switch (type) {
		case RE::BSShader::Type::Lighting:
			priority += 1000;
			break;
		case RE::BSShader::Type::Effect:
			priority += 500;
			break;
		case RE::BSShader::Type::Water:
			priority += 300;
			break;
		default:
			break;
		}

		// Pixel shaders compile significantly slower than vertex shaders
		if (shaderClass == ShaderClass::Pixel)
			priority += 200;

		// More active descriptor bits → more #defines → more code paths for the compiler
		priority += std::popcount(descriptor) * 30;

		// Known heavy Lighting techniques and flags from straggler analysis
		if (type == RE::BSShader::Type::Lighting) {
			const auto technique = static_cast<ShaderCache::LightingShaderTechniques>(0x3F & (descriptor >> 24));

			// LANDSCAPE techniques (MTLand, MTLandLODBlend) are among the heaviest
			// due to multi-texture blending codegen — regularly 60-130s compile times
			if (technique == ShaderCache::LightingShaderTechniques::MTLand ||
				technique == ShaderCache::LightingShaderTechniques::MTLandLODBlend)
				priority += 500;
			if (technique == ShaderCache::LightingShaderTechniques::Parallax ||
				technique == ShaderCache::LightingShaderTechniques::ParallaxOcc)
				priority += 300;
			if (technique == ShaderCache::LightingShaderTechniques::Eye)
				priority += 200;
			if (technique == ShaderCache::LightingShaderTechniques::MultilayerParallax)
				priority += 200;

			// TRUE_PBR and ANISO_LIGHTING are the dominant cost drivers,
			// especially in combination with LANDSCAPE (115-130s observed)
			if (descriptor & static_cast<uint32_t>(ShaderCache::LightingShaderFlags::TruePbr))
				priority += 500;
			if (descriptor & static_cast<uint32_t>(ShaderCache::LightingShaderFlags::AnisoLighting))
				priority += 300;
			// Deferred adds extra codegen overhead
			if (descriptor & static_cast<uint32_t>(ShaderCache::LightingShaderFlags::Deferred))
				priority += 200;

			// LANDSCAPE + TRUE_PBR combination triggers extreme register pressure
			// (6x unrolled texture layers * PBR params = 30+ textures, 180s+ compile)
			if ((technique == ShaderCache::LightingShaderTechniques::MTLand ||
					technique == ShaderCache::LightingShaderTechniques::MTLandLODBlend) &&
				(descriptor & static_cast<uint32_t>(ShaderCache::LightingShaderFlags::TruePbr)))
				priority += 500;
		}

		return priority;
	}

	std::optional<std::variant<ShaderCompilationTask, std::function<void()>>> CompilationSet::TryTakeNext(std::stop_token stoken)
	{
		std::unique_lock lock(compilationMutex);
		auto shaderCache = globals::shaderCache;
		if (!conditionVariable.wait(
				lock, stoken,
				[this, &shaderCache]() { return (!availableTasks.empty() || !demandedTasks.empty() || !pendingAuxTasks.empty()) &&
			                                    // Count matrix and auxiliary tasks; ReleaseDispatchSlot()
			                                    // decrements under this mutex before notifying the waiter.
			                                    static_cast<int32_t>(dispatchedTasksInFlight.load(std::memory_order_relaxed)) <
			                                        (!shaderCache->backgroundCompilation ? shaderCache->compilationThreadCount : shaderCache->backgroundCompilationThreadCount); })) {
			/*Woke up because of a stop request. */
			return std::nullopt;
		}
		// Session clock is now managed by CompilationSet::Add(); this branch is kept
		// as a safety net but will not trigger because totalTasks is incremented
		// before the conditionVariable notification.
		if (!shaderCache->IsCompiling()) {
			QueryPerformanceCounter(&lastReset);
			lastCalculation = lastReset;
		}

		// Matrix tasks are checked before aux so queued shader work always wins
		// admission over standalone compute-shader work when both are ready.
		if (!availableTasks.empty() || !demandedTasks.empty()) {
			// Startup policy: keep dispatching the hardest queued work first.
			// This preserves the existing priority score while preventing light tasks
			// from bypassing queued heavy shaders and stretching the tail.
			// Shaders the game is already drawing with go first, or they stay black behind the heavy tail.
			auto& queue = demandedTasks.empty() ? availableTasks : demandedTasks;
			auto bestIt = std::prev(queue.end());
			ShaderCompilationTask task = *bestIt;
			queue.erase(bestIt);

			if (task.GetPriority() >= kHeavyPriorityThreshold) {
				heavyTasksInFlight.fetch_add(1, std::memory_order_relaxed);
			}

			tasksInProgress.insert(task);
			dispatchedTasksInFlight.fetch_add(1, std::memory_order_relaxed);
			return std::variant<ShaderCompilationTask, std::function<void()>>(std::in_place_index<0>, task);
		}

		if (!pendingAuxTasks.empty()) {
			auto work = std::move(pendingAuxTasks.front());
			pendingAuxTasks.pop_front();
			dispatchedTasksInFlight.fetch_add(1, std::memory_order_relaxed);
			return std::variant<ShaderCompilationTask, std::function<void()>>(std::in_place_index<1>, std::move(work));
		}

		return std::nullopt;
	}

	void CompilationSet::ReleaseDispatchSlot()
	{
		{
			// Unlocked, this could race TryTakeNext()'s predicate check and lose the notify.
			std::scoped_lock lock(compilationMutex);
			dispatchedTasksInFlight.fetch_sub(1, std::memory_order_relaxed);
		}
		conditionVariable.notify_one();
	}

	void CompilationSet::EnqueueAux(std::function<void()> work)
	{
		{
			std::scoped_lock lock(compilationMutex);
			pendingAuxTasks.push_back(std::move(work));
		}
		conditionVariable.notify_one();
	}

	void CompilationSet::Add(const ShaderCompilationTask& task)
	{
		std::unique_lock lock(compilationMutex);
		if (task.IsPrecompile() && globals::shaderCache->backgroundCompilation)
			return;
		auto inProgressIt = tasksInProgress.find(task);
		auto processedIt = processedTasks.find(task);
		if (inProgressIt == tasksInProgress.end() && processedIt == processedTasks.end() && !demandedTasks.contains(task) && !globals::shaderCache->GetCompletedShader(task)) {
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			auto queuedTask = task;
			queuedTask.SetEnqueuedQpc(now.QuadPart);
			bool wasAdded;
			if (globals::shaderCache->backgroundCompilation) {
				// Only the game's own draw requests reach Add, so anything queued now is demanded.
				if (auto queued = availableTasks.extract(queuedTask)) {
					demandedTasks.insert(std::move(queued));
					wasAdded = false;
				} else {
					wasAdded = demandedTasks.insert(queuedTask).second;
				}
			} else {
				auto [queuedIt, inserted] = availableTasks.insert(queuedTask);
				wasAdded = inserted;
				// A draw asked for a queued startup precompile, so keep it if background mode later drops precompiles
				if (!inserted && !task.IsPrecompile() && queuedIt->IsPrecompile()) {
					auto node = availableTasks.extract(queuedIt);
					node.value().RequireForRendering();
					availableTasks.insert(std::move(node));
				}
			}
			bool batchStarted = false;
			if (wasAdded) {
				// Increment counters inside the lock so that TryTakeNext, which reads
				// IsCompiling() after waking up, sees the updated totalTasks and
				// does NOT incorrectly treat the new work as a "fresh start" and
				// reset the session clock via its !IsCompiling() branch.
				const uint64_t doneTasks = completedTasks.load(std::memory_order_relaxed) +
				                           failedTasks.load(std::memory_order_relaxed);
				const uint64_t prevTotal = totalTasks.load(std::memory_order_relaxed);

				// If every previously-known task is done (either a fresh session after
				// Clear(), or a burst of disk-cache hits drained the queue before all
				// shader requests had been submitted), restart the session clock so that
				// elapsed-time and ETA figures are accurate for the new batch of work.
				if (doneTasks >= prevTotal) {
					QueryPerformanceCounter(&lastReset);
					lastCalculation = lastReset;
					batchStarted = true;
				}

				// If compilation was previously marked complete (prematurely, because a
				// disk-cache burst completed all known tasks before further shaders were
				// submitted), clear the completion timestamp.  This lets the timer keep
				// running and allows the true final completion to be recorded later.
				if (completionTime.load(std::memory_order_relaxed) != 0) {
					completionTime.store(0, std::memory_order_relaxed);
					compilationPhaseStarted.store(false, std::memory_order_relaxed);
					compilationPhaseStart = { 0 };
				}

				totalTasks++;
				totalPriorityWeight += static_cast<uint64_t>(task.GetPriority()) + 1;
			}
			lock.unlock();
			// Info, with the completion below: a run can tell from the log whether compilation is still going.
			if (batchStarted)
				logger::info("Compilation started");
			if (wasAdded) {
				conditionVariable.notify_one();
			}
		}
	}

	/**
	 * @brief Marks a shader compilation task as complete and updates compilation state.
	 *
	 * Updates task completion counters, tracks compilation timing and cache behavior,
	 * detects batch-level completion, and emits notifications and events when the
	 * entire compilation session finishes.
	 *
	 * @param task The compilation task that has completed.
	 */
	void CompilationSet::Complete(const ShaderCompilationTask& task)
	{
		auto& cache = ShaderCache::Instance();
		auto key = task.GetString();
		auto shaderBlob = cache.GetCompletedShader(key);

		bool shouldLogCompletion = false;
		double completionTimeMs = 0.0;
#ifdef DEVBENCH_BRIDGE_ENABLED
		// Snapshot of the counters latched under the lock at the moment completion is
		// detected, so the emitted event reflects that exact state — not whatever a
		// concurrent Complete()/Clear() may have changed it to after we release the lock.
		uint64_t completedSnapshot = 0;
		uint64_t failedSnapshot = 0;
		uint64_t totalSnapshot = 0;
#endif

		// Determine whether this task was resolved from the disk cache or actually compiled.
		bool wasDiskHit = cache.IsShaderLoadedFromDisk(key);

		// Perform all completion operations under one mutex acquisition
		{
			std::scoped_lock lock(compilationMutex);

			// Update task counters
			if (shaderBlob) {
				logger::debug("Compiling Task succeeded: {}", key);
				completedTasks++;
			} else {
				logger::debug("Compiling Task failed: {}", key);
				failedTasks++;
			}
			completedPriorityWeight += static_cast<uint64_t>(task.GetPriority()) + 1;

			// Track disk-cache hits separately so ETA can use compilation-only timing.
			if (wasDiskHit) {
				diskHitTasks++;
				diskHitPriorityWeight += static_cast<uint64_t>(task.GetPriority()) + 1;
			} else if (!compilationPhaseStarted.load(std::memory_order_relaxed)) {
				// First actual compilation: start the compilation-phase clock.
				// Write the start time before the release-store so readers see it.
				QueryPerformanceCounter(&compilationPhaseStart);
				compilationPhaseStarted.store(true, std::memory_order_release);
			}

			// Track heavy task completion for P-core concurrency limiting
			if (task.GetPriority() >= kHeavyPriorityThreshold) {
				auto current = heavyTasksInFlight.load(std::memory_order_relaxed);
				while (current > 0 &&
					   !heavyTasksInFlight.compare_exchange_weak(current, current - 1,
						   std::memory_order_relaxed,
						   std::memory_order_relaxed)) {
				}
			}

			// Update timing
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			totalTime.QuadPart += now.QuadPart - lastCalculation.QuadPart;
			lastCalculation = now;

			// Check if compilation is complete and set completion time if needed
			if (completionTime.load(std::memory_order_relaxed) == 0 && completedTasks + failedTasks >= totalTasks) {
				completionTime.store(now.QuadPart, std::memory_order_relaxed);
				completionTimeMs = static_cast<double>(now.QuadPart - lastReset.QuadPart) * 1000.0 / frequency.QuadPart;
				shouldLogCompletion = true;
#ifdef DEVBENCH_BRIDGE_ENABLED
				completedSnapshot = completedTasks.load(std::memory_order_relaxed);
				failedSnapshot = failedTasks.load(std::memory_order_relaxed);
				totalSnapshot = totalTasks.load(std::memory_order_relaxed);
#endif
			}

			// Update task tracking
			processedTasks.insert(task);
			tasksInProgress.erase(task);
		}

		// Log completion outside the lock
		if (shouldLogCompletion) {
			logger::info("Compilation completed in {} ms", GetHumanTime(completionTimeMs));
			Util::FinishShaderStoreBatch("shader compilation");

#ifdef DEVBENCH_BRIDGE_ENABLED
			// A compilation batch finished (initial build OR a hot-reload recompile).
			// Emit one summary event so a benchmark scenario can split its A/B window
			// precisely on the moment a recompiled shader went live, and detect failures
			// without polling. Guarded on the devbench host being present.
			if (auto* dvb = DevBenchAPI::GetDevBenchInterface001()) {
				const nlohmann::json payload{
					{ "completedTasks", completedSnapshot },
					{ "failedTasks", failedSnapshot },
					{ "totalTasks", totalSnapshot },
					{ "durationMs", completionTimeMs },
				};
				const std::string dumped = payload.dump();
				dvb->EmitEvent("communityshaders.shaderRecompiled", dumped.c_str());
			}
#endif
		}

		conditionVariable.notify_one();
	}

	void CompilationSet::Clear()
	{
		std::scoped_lock lock(compilationMutex);
		availableTasks.clear();
		demandedTasks.clear();
		pendingAuxTasks.clear();
		tasksInProgress.clear();
		processedTasks.clear();
		totalTasks = 0;
		completedTasks = 0;
		failedTasks = 0;
		cacheHitTasks = 0;
		diskHitTasks = 0;
		diskHitPriorityWeight = 0;
		compilationPhaseStarted = false;
		compilationPhaseStart = { 0 };
		slowTasks = 0;
		verySlowTasks = 0;
		totalPriorityWeight = 0;
		completedPriorityWeight = 0;
		heavyTasksInFlight = 0;
		QueryPerformanceCounter(&lastReset);
		QueryPerformanceCounter(&lastCalculation);
		completionTime = { 0 };  // Reset completion time
		totalTime = { 0 };
		{
			std::lock_guard slowLock(slowTasksMutex);
			slowTaskRecords.clear();
		}
	}

	std::string CompilationSet::GetHumanTime(double a_totalMs)
	{
		return Util::FormatDuration(a_totalMs);
	}

	double CompilationSet::GetEta()
	{
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		const int64_t endQpc = (completionTime.load(std::memory_order_relaxed) != 0) ? completionTime.load(std::memory_order_relaxed) : now.QuadPart;

		// Helper: given elapsed time and done/total priority weights, return remaining ms (or 0).
		auto weightedEta = [](double elapsedMs, double doneW, double totalW) -> double {
			if (elapsedMs <= 0.0 || doneW <= 0.0 || totalW <= 0.0)
				return 0.0;
			double fraction = doneW / totalW;
			return std::max(elapsedMs / fraction - elapsedMs, 0.0);
		};

		const uint64_t diskWeight = diskHitPriorityWeight.load(std::memory_order_relaxed);
		const uint64_t totalWeight = totalPriorityWeight.load(std::memory_order_relaxed);
		const uint64_t doneWeight = completedPriorityWeight.load(std::memory_order_relaxed);

		if (diskWeight > 0) {
			// There are disk-cache hits in this session.
			if (!compilationPhaseStarted.load(std::memory_order_acquire)) {
				// Compilations haven't started yet (still loading from disk cache).
				// We have no compilation rate to extrapolate from, so return 0 to
				// avoid a wildly wrong ETA based purely on the fast disk-hit rate.
				return 0.0;
			}

			// At least one actual compilation has completed.  Use compilation-phase
			// timing so that fast disk loads at the start of the session don't inflate
			// the apparent progress rate and produce an underestimated ETA.
			const int64_t phaseStart = compilationPhaseStart.QuadPart;  // visible due to acquire above
			double compilationElapsedMs = static_cast<double>(endQpc - phaseStart) * 1000.0 / frequency.QuadPart;

			// Exclude disk-hit weight from both numerator and denominator so the
			// rate reflects only the actual compilation speed.
			double compiledDone = static_cast<double>(doneWeight > diskWeight ? doneWeight - diskWeight : 0);
			double compiledTotal = static_cast<double>(totalWeight > diskWeight ? totalWeight - diskWeight : 0);
			return weightedEta(compilationElapsedMs, compiledDone, compiledTotal);
		}

		// No disk hits: fall back to the original whole-session ETA.
		// Priority-weighted so heavy tasks completing early don't inflate the estimate.
		double elapsedMs = static_cast<double>(endQpc - lastReset.QuadPart) * 1000.0 / frequency.QuadPart;
		return weightedEta(elapsedMs, static_cast<double>(doneWeight), static_cast<double>(totalWeight));
	}

	std::string CompilationSet::GetStatsString(bool a_timeOnly, bool a_elapsedOnly)
	{
		// Calculate elapsed time since compilation started
		LARGE_INTEGER currentTime;
		QueryPerformanceCounter(&currentTime);

		// Use completion time if compilation is finished, otherwise current time
		int64_t endTime = (completionTime.load(std::memory_order_relaxed) != 0) ? completionTime.load(std::memory_order_relaxed) : currentTime.QuadPart;
		double totalMs = static_cast<double>(endTime - lastReset.QuadPart) * 1000.0 / frequency.QuadPart;

		if (a_timeOnly) {
			if (a_elapsedOnly) {
				// Only elapsed
				return GetHumanTime(totalMs);
			} else {
				// Elapsed + estimated
				return fmt::format("{}/{}",
					GetHumanTime(totalMs),
					GetHumanTime(GetEta() + totalMs));
			}
		}

		return fmt::format("{}/{} (successful/total)\tfailed: {}\tdeduplicated: {}\tdisk cache: {}\nElapsed/Estimated Time: {}/{}",
			(std::uint64_t)completedTasks,
			(std::uint64_t)totalTasks,
			(std::uint64_t)failedTasks,
			(std::uint64_t)cacheHitTasks,
			(std::uint64_t)diskHitTasks,
			GetHumanTime(totalMs),
			GetHumanTime(GetEta() + totalMs));
	}

	UpdateListener::UpdateListener(ShaderFileDependencyTracker* deps_) :
		deps(deps_) {}

	void UpdateListener::UpdateCache(const std::filesystem::path& filePath, SIE::ShaderCache* cache, bool& clearCache, bool& fileDone)
	{
		fileDone = true;
		// Skip directories
		if (std::filesystem::is_directory(filePath)) {
			return;
		}
		// Extract file components
		const std::string extension = filePath.extension().string();
		const std::string shaderTypeString = filePath.stem().string();
		std::chrono::time_point<std::chrono::system_clock> modifiedTime{};
		auto shaderType = magic_enum::enum_cast<RE::BSShader::Type>(shaderTypeString, magic_enum::case_insensitive);
		// Check if the file exists and get its modified time
		if (std::filesystem::exists(filePath)) {
			modifiedTime = std::chrono::clock_cast<std::chrono::system_clock>(std::filesystem::last_write_time(filePath));
		} else {
			return;
		}

		// Ensure the file is not a directory and is a valid shader file (.hlsl)
		std::string lowerExtension = extension;
		std::transform(lowerExtension.begin(), lowerExtension.end(), lowerExtension.begin(),
			[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		// The render graph's live pipelines (its compute programs, Drawcall Limit Fix's graphics pipelines) build from the same tree
		// (docs/development/dclf-hot-reload.md): an edited shader rebuilds those built from it, an include every one, at the next present.
		// DCLF's own shaders (Data/Shaders/DrawcallLimitFix/) are no D3D11 shader's: an .hlsl there leaves this cache alone (an include there
		// still invalidates the D3D11 shaders that include it, below).
		if (lowerExtension == ".hlsl" || lowerExtension == ".hlsli" || lowerExtension == ".h") {
			// Before anything asks for a rebuild: the store must see this file's new contents.
			Util::BeginShaderStoreGeneration();
			RenderGraphRuntime::RequestShaderReload(filePath.generic_string());
			std::string lowerPath = filePath.generic_string();
			std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (lowerExtension == ".hlsl" && lowerPath.find("/drawcalllimitfix/") != std::string::npos) {
				logger::info("Shader watcher: {} is Drawcall Limit Fix's; its pipelines are rebuilt, the D3D11 cache kept", filePath.string());
				fileDone = false;
				return;
			}
		}
		if (!std::filesystem::is_directory(filePath) && lowerExtension == ".hlsl") {
			// Update cache with the modified shader
			cache->InsertModifiedShaderMap(shaderTypeString, modifiedTime);

			// Attempt to mark the shader for recompilation
			bool foundPath = cache->Clear(filePath.string());

			if (!foundPath) {
				// File was not found in the the map so check its shader type
				std::string parentDirName = filePath.parent_path().filename().string();
				std::transform(parentDirName.begin(), parentDirName.end(), parentDirName.begin(),
					[](unsigned char c) { return static_cast<char>(std::tolower(c)); });

				// Check if the parent directory name matches "shaders" in a case-insensitive way
				if (lowerExtension == ".hlsl" && parentDirName == "shaders" && shaderType.has_value()) {
					cache->Clear(shaderType.value());
				} else {
					// If it's not specifically handled, clear all cache
					clearCache = true;
				}
			}
		}
		// Handle include file changes (.hlsli, .h) by invalidating dependents
		else if (!std::filesystem::is_directory(filePath) && (lowerExtension == ".hlsli" || lowerExtension == ".h")) {
			// Normalize to absolute canonical path to match how dependencies are tracked
			std::error_code ec;
			auto canonicalPath = std::filesystem::weakly_canonical(filePath, ec);
			std::string pathStr = (ec ? filePath.string() : canonicalPath.string());
			// On Windows, normalize to lowercase to match Util::FxcDependencyPaths
#ifdef _WIN32
			std::transform(pathStr.begin(), pathStr.end(), pathStr.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
#endif
			// Invalidate all .hlsl files that depend on this .hlsli
			auto dependents = deps->GetDependents(pathStr);
			for (const auto& hlsl : dependents) {
				cache->Clear(hlsl);
			}
		}
		// Indicate that file processing is not yet complete
		fileDone = false;
	}

	void UpdateListener::processQueue()
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		std::unique_lock lock(actionMutex, std::defer_lock);
		auto cache = globals::shaderCache;
		while (cache->UseFileWatcher()) {
			lock.lock();
			if (!queue.empty()) {
				bool clearCache = false;
				for (fileAction fAction : queue) {
					const std::filesystem::path filePath = std::filesystem::path(std::format("{}\\{}", fAction.dir, fAction.filename));
					bool fileDone = false;
					switch (fAction.action) {
					case efsw::Actions::Add:
						logger::debug("Detected Added path {}", filePath.string());
						UpdateCache(filePath, cache, clearCache, fileDone);
						break;
					case efsw::Actions::Delete:
						logger::debug("Detected Deleted path {}", filePath.string());
						break;
					case efsw::Actions::Modified:
						if (!std::filesystem::is_directory(filePath)) {
							logger::debug("Detected Changed path {}", filePath.string());
						}
						UpdateCache(filePath, cache, clearCache, fileDone);
						break;
					case efsw::Actions::Moved:
						logger::debug("Detected Moved path {}", filePath.string());
						break;
					default:
						logger::error("Filewatcher received invalid action {}", magic_enum::enum_name(fAction.action));
					}
					if (fileDone)
						continue;
				}
				// Feature shaders are not dependency-tracked, so any edit may fix a failed compile.
				Util::ClearShaderCompileFailures();
				if (clearCache) {
					cache->Clear();
				}
				queue.clear();
			}
			lock.unlock();
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		queue.clear();
	}

	void UpdateListener::handleFileAction(efsw::WatchID watchid, const std::string& dir, const std::string& filename, efsw::Action action, std::string oldFilename)
	{
		std::lock_guard lock(actionMutex);
		if (queue.empty() || queue.back().action != action ||
			queue.back().filename != filename || queue.back().dir != dir ||
			queue.back().oldFilename != oldFilename || queue.back().watchID != watchid) {
			// only add if not a duplicate; esfw is very spammy
			queue.push_back({ watchid, dir, filename, action, oldFilename });
		}
	}
}
