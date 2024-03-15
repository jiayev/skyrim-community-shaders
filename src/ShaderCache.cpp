#include "ShaderCache.h"

#include <RE/V/VertexDesc.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <fmt/std.h>
#include <wrl/client.h>

#include "Feature.h"
#include "State.h"

namespace SIE
{
	namespace SShaderCache
	{
		static void GetShaderDefines(RE::BSShader::Type, uint32_t, D3D_SHADER_MACRO*);
		static std::string GetShaderString(ShaderClass, const RE::BSShader&, uint32_t, bool = false);
		/**
		@brief Get the BSShader::Type from the ShaderString
		@param a_key The key generated from GetShaderString
		@return A string with a valid BSShader::Type
		*/
		static std::string GetTypeFromShaderString(std::string);
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

		static void GetLightingShaderDefines(uint32_t descriptor,
			D3D_SHADER_MACRO* defines)
		{
			static REL::Relocation<void(uint32_t, D3D_SHADER_MACRO*)> VanillaGetLightingShaderDefines(
				RELOCATION_ID(101631, 108698));

			const auto technique =
				static_cast<ShaderCache::LightingShaderTechniques>(GetTechnique(descriptor));

			int lastIndex = 0;

			if (technique == ShaderCache::LightingShaderTechniques::Outline) {
				defines[lastIndex++] = { "OUTLINE", nullptr };
			}

			for (auto* feature : Feature::GetFeatureList()) {
				if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::Lighting)) {
					defines[lastIndex++] = { feature->GetShaderDefineName().data(), nullptr };
				}
			}

			VanillaGetLightingShaderDefines(descriptor, defines + lastIndex);
		}

		enum class BloodSplatterShaderTechniques
		{
			Splatter = 0,
			Flare = 1,
		};

		static void GetBloodSplaterShaderDefines(uint32_t descriptor, D3D_SHADER_MACRO* defines)
		{
			int lastIndex = 0;
			if (descriptor == static_cast<uint32_t>(BloodSplatterShaderTechniques::Splatter)) {
				defines[lastIndex++] = { "SPLATTER", nullptr };
			} else if (descriptor == static_cast<uint32_t>(BloodSplatterShaderTechniques::Flare)) {
				defines[lastIndex++] = { "FLARE", nullptr };
			}

			for (auto* feature : Feature::GetFeatureList()) {
				if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::BloodSplatter)) {
					defines[lastIndex++] = { feature->GetShaderDefineName().data(), nullptr };
				}
			}

			defines[lastIndex] = { nullptr, nullptr };
		}

		enum class DistantTreeShaderTechniques
		{
			DistantTreeBlock = 0,
			Depth = 1,
		};

		enum class DistantTreeShaderFlags
		{
			AlphaTest = 0x10000,
		};

		static void GetDistantTreeShaderDefines(uint32_t descriptor, D3D_SHADER_MACRO* defines)
		{
			const auto technique = descriptor & 1;
			int lastIndex = 0;
			if (technique == static_cast<uint32_t>(DistantTreeShaderTechniques::Depth)) {
				defines[lastIndex++] = { "RENDER_DEPTH", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(DistantTreeShaderFlags::AlphaTest)) {
				defines[lastIndex++] = { "DO_ALPHA_TEST", nullptr };
			}

			for (auto* feature : Feature::GetFeatureList()) {
				if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::DistantTree)) {
					defines[lastIndex++] = { feature->GetShaderDefineName().data(), nullptr };
				}
			}

			defines[lastIndex] = { nullptr, nullptr };
		}

		enum class SkyShaderTechniques
		{
			SunOcclude = 0,
			SunGlare = 1,
			MoonAndStarsMask = 2,
			Stars = 3,
			Clouds = 4,
			CloudsLerp = 5,
			CloudsFade = 6,
			Texture = 7,
			Sky = 8,
		};

		static void GetSkyShaderDefines(uint32_t descriptor, D3D_SHADER_MACRO* defines)
		{
			const auto technique = static_cast<SkyShaderTechniques>(descriptor);
			int lastIndex = 0;
			switch (technique) {
			case SkyShaderTechniques::SunOcclude:
				{
					defines[lastIndex++] = { "OCCLUSION", nullptr };
					break;
				}
			case SkyShaderTechniques::SunGlare:
				{
					defines[lastIndex++] = { "TEX", nullptr };
					defines[lastIndex++] = { "DITHER", nullptr };
					break;
				}
			case SkyShaderTechniques::MoonAndStarsMask:
				{
					defines[lastIndex++] = { "TEX", nullptr };
					defines[lastIndex++] = { "MOONMASK", nullptr };
					break;
				}
			case SkyShaderTechniques::Stars:
				{
					defines[lastIndex++] = { "HORIZFADE", nullptr };
					break;
				}
			case SkyShaderTechniques::Clouds:
				{
					defines[lastIndex++] = { "TEX", nullptr };
					defines[lastIndex++] = { "CLOUDS", nullptr };
					break;
				}
			case SkyShaderTechniques::CloudsLerp:
				{
					defines[lastIndex++] = { "TEX", nullptr };
					defines[lastIndex++] = { "CLOUDS", nullptr };
					defines[lastIndex++] = { "TEXLERP", nullptr };
					break;
				}
			case SkyShaderTechniques::CloudsFade:
				{
					defines[lastIndex++] = { "TEX", nullptr };
					defines[lastIndex++] = { "CLOUDS", nullptr };
					defines[lastIndex++] = { "TEXFADE", nullptr };
					break;
				}
			case SkyShaderTechniques::Texture:
				{
					defines[lastIndex++] = { "TEX", nullptr };
					break;
				}
			case SkyShaderTechniques::Sky:
				{
					defines[lastIndex++] = { "DITHER", nullptr };
					break;
				}
			}

			for (auto* feature : Feature::GetFeatureList()) {
				if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::Sky)) {
					defines[lastIndex++] = { feature->GetShaderDefineName().data(), nullptr };
				}
			}

			defines[lastIndex] = { nullptr, nullptr };
		}

		enum class GrassShaderTechniques
		{
			RenderDepth = 8,
		};

		enum class GrassShaderFlags
		{
			AlphaTest = 0x10000,
		};

		static void GetGrassShaderDefines(uint32_t descriptor, D3D_SHADER_MACRO* defines)
		{
			const auto technique = descriptor & 0b1111;
			int lastIndex = 0;
			if (technique == static_cast<uint32_t>(GrassShaderTechniques::RenderDepth)) {
				defines[lastIndex++] = { "RENDER_DEPTH", nullptr };
			}
			if (descriptor & static_cast<uint32_t>(GrassShaderFlags::AlphaTest)) {
				defines[lastIndex++] = { "DO_ALPHA_TEST", nullptr };
			}

			for (auto* feature : Feature::GetFeatureList()) {
				if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::Grass)) {
					defines[lastIndex++] = { feature->GetShaderDefineName().data(), nullptr };
				}
			}

			defines[lastIndex] = { nullptr, nullptr };
		}

		enum class ParticleShaderTechniques
		{
			Particles = 0,
			ParticlesGryColor = 1,
			ParticlesGryAlpha = 2,
			ParticlesGryColorAlpha = 3,
			EnvCubeSnow = 4,
			EnvCubeRain = 5,
		};

		static void GetParticleShaderDefines(uint32_t descriptor, D3D_SHADER_MACRO* defines)
		{
			const auto technique = static_cast<ParticleShaderTechniques>(descriptor);
			int lastIndex = 0;
			switch (technique) {
			case ParticleShaderTechniques::ParticlesGryColor:
				{
					defines[lastIndex++] = { "GRAYSCALE_TO_COLOR", nullptr };
					break;
				}
			case ParticleShaderTechniques::ParticlesGryAlpha:
				{
					defines[lastIndex++] = { "GRAYSCALE_TO_ALPHA", nullptr };
					break;
				}
			case ParticleShaderTechniques::ParticlesGryColorAlpha:
				{
					defines[lastIndex++] = { "GRAYSCALE_TO_COLOR", nullptr };
					defines[lastIndex++] = { "GRAYSCALE_TO_ALPHA", nullptr };
					break;
				}
			case ParticleShaderTechniques::EnvCubeSnow:
				{
					defines[lastIndex++] = { "ENVCUBE", nullptr };
					defines[lastIndex++] = { "SNOW", nullptr };
					break;
				}
			case ParticleShaderTechniques::EnvCubeRain:
				{
					defines[lastIndex++] = { "ENVCUBE", nullptr };
					defines[lastIndex++] = { "RAIN", nullptr };
					break;
				}
			}

			for (auto* feature : Feature::GetFeatureList()) {
				if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::Particle)) {
					defines[lastIndex++] = { feature->GetShaderDefineName().data(), nullptr };
				}
			}

			defines[lastIndex] = { nullptr, nullptr };
		}

		static void GetEffectShaderDefines(uint32_t descriptor, D3D_SHADER_MACRO* defines)
		{
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Vc)) {
				defines[0] = { "VC", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::TexCoord)) {
				defines[0] = { "TEXCOORD", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::TexCoordIndex)) {
				defines[0] = { "TEXCOORD_INDEX", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Skinned)) {
				defines[0] = { "SKINNED", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Normals)) {
				defines[0] = { "NORMALS", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::BinormalTangent)) {
				defines[0] = { "BINORMAL_TANGENT", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Texture)) {
				defines[0] = { "TEXTURE", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::IndexedTexture)) {
				defines[0] = { "INDEXED_TEXTURE", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Falloff)) {
				defines[0] = { "FALLOFF", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::AddBlend)) {
				defines[0] = { "ADDBLEND", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::MultBlend)) {
				defines[0] = { "MULTBLEND", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Particles)) {
				defines[0] = { "PARTICLES", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::StripParticles)) {
				defines[0] = { "STRIP_PARTICLES", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Blood)) {
				defines[0] = { "BLOOD", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Membrane)) {
				defines[0] = { "MEMBRANE", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Lighting)) {
				defines[0] = { "LIGHTING", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::ProjectedUv)) {
				defines[0] = { "PROJECTED_UV", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Soft)) {
				defines[0] = { "SOFT", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::GrayscaleToColor)) {
				defines[0] = { "GRAYSCALE_TO_COLOR", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::GrayscaleToAlpha)) {
				defines[0] = { "GRAYSCALE_TO_ALPHA", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::IgnoreTexAlpha)) {
				defines[0] = { "IGNORE_TEX_ALPHA", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::MultBlendDecal)) {
				defines[0] = { "MULTBLEND_DECAL", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::AlphaTest)) {
				defines[0] = { "ALPHA_TEST", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::SkyObject)) {
				defines[0] = { "SKY_OBJECT", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::MsnSpuSkinned)) {
				defines[0] = { "MSN_SPU_SKINNED", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::MotionVectorsNormals)) {
				defines[0] = { "MOTIONVECTORS_NORMALS", nullptr };
				++defines;
			}

			for (auto* feature : Feature::GetFeatureList()) {
				if (feature->loaded && feature->HasShaderDefine(RE::BSShader::Type::Effect)) {
					defines[0] = { feature->GetShaderDefineName().data(), nullptr };
					++defines;
				}
			}

			defines[0] = { nullptr, nullptr };
		}

		static void GetWaterShaderDefines(uint32_t descriptor, D3D_SHADER_MACRO* defines)
		{
			int lastIndex = 0;
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
				}
			}

			defines[lastIndex] = { nullptr, nullptr };
		}

		static void GetShaderDefines(RE::BSShader::Type type, uint32_t descriptor,
			D3D_SHADER_MACRO* defines)
		{
			switch (type) {
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
				{ "HighDetailRange", 12 },
				{ "FogParam", 13 },
				{ "FogNearColor", 14 },
				{ "FogFarColor", 15 },
				{ "LeftEyeCenter", 9 },
				{ "RightEyeCenter", 10 },
				{ "TexcoordOffset", 11 },
				{ "World", 0 },
				{ "PreviousWorld", 1 },
				{ "EyePosition", 2 },
				{ "LandBlendParams", 3 },
				{ "TreeParams", 4 },
				{ "WindTimers", 5 },
				{ "TextureProj", 6 },
				{ "IndexScale", 7 },
				{ "WorldMapOverlayParameters", 8 },
				{ "Bones", 16 },
			};

			auto& lightingPS = result[static_cast<size_t>(RE::BSShader::Type::Lighting)]
									 [static_cast<size_t>(ShaderClass::Pixel)];
			lightingPS = {
				{ "VPOSOffset", 11 },
				{ "FogColor", 19 },
				{ "ColourOutputClamp", 20 },
				{ "EnvmapData", 21 },
				{ "ParallaxOccData", 22 },
				{ "TintColor", 23 },
				{ "LODTexParams", 24 },
				{ "SpecularColor", 25 },
				{ "SparkleParams", 26 },
				{ "MultiLayerParallaxData", 27 },
				{ "LightingEffectParams", 28 },
				{ "IBLParams", 29 },
				{ "LandscapeTexture1to4IsSnow", 30 },
				{ "LandscapeTexture5to6IsSnow", 31 },
				{ "LandscapeTexture1to4IsSpecPower", 32 },
				{ "LandscapeTexture5to6IsSpecPower", 33 },
				{ "SnowRimLightParameters", 34 },
				{ "CharacterLightParams", 35 },
				{ "NumLightNumShadowLight", 0 },
				{ "PointLightPosition", 1 },
				{ "PointLightColor", 2 },
				{ "DirLightDirection", 3 },
				{ "DirLightColor", 4 },
				{ "DirectionalAmbient", 5 },
				{ "AmbientSpecularTintAndFresnelPower", 6 },
				{ "MaterialData", 7 },
				{ "EmitColor", 8 },
				{ "AlphaTestRef", 9 },
				{ "ShadowLightMaskSelect", 10 },
				{ "ProjectedUVParams", 12 },
				{ "ProjectedUVParams2", 13 },
				{ "ProjectedUVParams3", 14 },
				{ "SplitDistance", 15 },
				{ "SSRParams", 16 },
				{ "WorldMapOverlayParametersPS", 17 },
				{ "AmbientColor", 18 },
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
				{ "WorldViewProj", 1 },
				{ "World", 2 },
				{ "PreviousWorld", 3 },
				{ "FogParam", 4 },
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
				{ "ShadowClampValue", 14 },
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

			auto& effectPS = result[static_cast<size_t>(RE::BSShader::Type::Effect)]
								   [static_cast<size_t>(ShaderClass::Pixel)];
			effectPS = {
				{ "PropertyColor", 0 },
				{ "AlphaTestRef", 1 },
				{ "MembraneRimColor", 2 },
				{ "MembraneVars", 3 },
				{ "PLightPositionX", 4 },
				{ "PLightPositionY", 5 },
				{ "PLightPositionZ", 6 },
				{ "PLightingRadiusInverseSquared", 7 },
				{ "PLightColorR", 8 },
				{ "PLightColorG", 9 },
				{ "PLightColorB", 10 },
				{ "DLightColor", 11 },
				{ "VPOSOffset", 12 },
				{ "CameraData", 13 },
				{ "FilteringParam", 14 },
				{ "BaseColor", 15 },
				{ "BaseColorScale", 16 },
				{ "LightingInfluence", 17 },
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
				{ "SubTexOffset", 12 },
				{ "PosAdjust", 13 },
				{ "MatProj", 14 },
			};

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
				{ "CameraData", 8 },
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

			return result;
		}

		static int32_t GetVariableIndex(ShaderClass shaderClass, RE::BSShader::Type shaderType, const char* name)
		{
			static auto variableNames = GetVariableIndices();

			const auto& names =
				variableNames[static_cast<size_t>(shaderType)][static_cast<size_t>(shaderClass)];
			auto it = names.find(name);
			if (it == names.cend()) {
				return -1;
			}

			return it->second;
		}

		static std::string MergeDefinesString(std::array<D3D_SHADER_MACRO, 64>& defines, bool a_sort = false)
		{
			std::string result;
			if (a_sort)
				std::sort(std::begin(defines), std::end(defines), [](const D3D_SHADER_MACRO& a, const D3D_SHADER_MACRO& b) {
					return a.Name > b.Name;
				});
			for (const auto& def : defines) {
				if (def.Name != nullptr) {
					result += def.Name;
					if (def.Definition != nullptr && !std::string(def.Definition).empty()) {
						result += "=";
						result += def.Definition;
					}
					result += ' ';
				} else {
					if (a_sort)  // sometimes the sort messes up so null entries get interspersed
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

		template <size_t MaxOffsetsSize>
		static void ReflectConstantBuffers(ID3D11ShaderReflection& reflector,
			std::array<size_t, 3>& bufferSizes,
			std::array<int8_t, MaxOffsetsSize>& constantOffsets,
			uint64_t& vertexDesc,
			ShaderClass shaderClass, RE::BSShader::Type shaderType, uint32_t descriptor)
		{
			D3D11_SHADER_DESC desc;
			if (FAILED(reflector.GetDesc(&desc))) {
				logger::error("Failed to get shader descriptor for {} shader {}::{}",
					magic_enum::enum_name(shaderClass), magic_enum::enum_name(shaderType),
					descriptor);
				return;
			}

			if (desc.ConstantBuffers <= 0) {
				return;
			}

			if (shaderClass == ShaderClass::Vertex) {
				vertexDesc = 0b1111;
				bool hasTexcoord2 = false;
				bool hasTexcoord3 = false;
				for (uint32_t inputIndex = 0; inputIndex < desc.InputParameters; ++inputIndex) {
					D3D11_SIGNATURE_PARAMETER_DESC inputDesc;
					if (FAILED(reflector.GetInputParameterDesc(inputIndex, &inputDesc))) {
						logger::error(
							"Failed to get input parameter {} descriptor for {} shader {}::{}",
							inputIndex, magic_enum::enum_name(shaderClass),
							magic_enum::enum_name(shaderType),
							descriptor);
					} else {
						std::string_view semanticName = inputDesc.SemanticName;
						if (semanticName == "POSITION" && inputDesc.SemanticIndex == 0) {
							AddAttribute(vertexDesc, RE::BSGraphics::Vertex::VA_POSITION);
						} else if (semanticName == "TEXCOORD" &&
								   inputDesc.SemanticIndex == 0) {
							AddAttribute(vertexDesc, RE::BSGraphics::Vertex::VA_TEXCOORD0);
						} else if (semanticName == "TEXCOORD" && inputDesc.SemanticIndex == 1) {
							AddAttribute(vertexDesc, RE::BSGraphics::Vertex::VA_TEXCOORD1);
						} else if (semanticName == "NORMAL" &&
								   inputDesc.SemanticIndex == 0) {
							AddAttribute(vertexDesc, RE::BSGraphics::Vertex::VA_NORMAL);
						} else if (semanticName == "BINORMAL" && inputDesc.SemanticIndex == 0) {
							AddAttribute(vertexDesc, RE::BSGraphics::Vertex::VA_BINORMAL);
						} else if (semanticName == "COLOR" &&
								   inputDesc.SemanticIndex == 0) {
							AddAttribute(vertexDesc, RE::BSGraphics::Vertex::VA_COLOR);
						} else if (semanticName == "BLENDWEIGHT" && inputDesc.SemanticIndex == 0) {
							AddAttribute(vertexDesc, RE::BSGraphics::Vertex::VA_SKINNING);
						} else if (semanticName == "TEXCOORD" && inputDesc.SemanticIndex >= 4 &&
								   inputDesc.SemanticIndex <= 7) {
							AddAttribute(vertexDesc, RE::BSGraphics::Vertex::VA_INSTANCEDATA);
						} else if (semanticName == "TEXCOORD" &&
								   inputDesc.SemanticIndex == 2) {
							hasTexcoord2 = true;
						} else if (semanticName == "TEXCOORD" && inputDesc.SemanticIndex == 3) {
							hasTexcoord3 = true;
						}
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
					auto bufferReflector = reflector.GetConstantBufferByName(bufferName);
					if (bufferReflector == nullptr) {
						logger::trace("Buffer {} not found for {} shader {}::{}",
							bufferName, magic_enum::enum_name(shaderClass),
							magic_enum::enum_name(shaderType),
							descriptor);
						return;
					}

					D3D11_SHADER_BUFFER_DESC bufferDesc;
					if (FAILED(bufferReflector->GetDesc(&bufferDesc))) {
						logger::trace("Failed to get buffer {} descriptor for {} shader {}::{}",
							bufferName, magic_enum::enum_name(shaderClass),
							magic_enum::enum_name(shaderType),
							descriptor);
						return;
					}

					for (uint32_t i = 0; i < bufferDesc.Variables; i++) {
						ID3D11ShaderReflectionVariable* var = bufferReflector->GetVariableByIndex(i);

						D3D11_SHADER_VARIABLE_DESC varDesc;
						if (FAILED(var->GetDesc(&varDesc))) {
							logger::trace("Failed to get variable descriptor for {} shader {}::{}",
								magic_enum::enum_name(shaderClass), magic_enum::enum_name(shaderType),
								descriptor);
							continue;
						}

						const auto variableIndex =
							GetVariableIndex(shaderClass, shaderType, varDesc.Name);
						if (variableIndex != -1) {
							constantOffsets[variableIndex] = (int8_t)(varDesc.StartOffset / 4);
						} else {
							logger::trace("Unknown variable name {} in {} shader {}::{}",
								varDesc.Name, magic_enum::enum_name(shaderClass),
								magic_enum::enum_name(shaderType),
								descriptor);
						}
					}

					bufferSize = ((bufferDesc.Size + 15) & ~15) / 16;
				};

			mapBufferConsts("PerTechnique", bufferSizes[0]);
			mapBufferConsts("PerMaterial", bufferSizes[1]);
			mapBufferConsts("PerGeometry", bufferSizes[2]);
		}

		std::wstring GetDiskPath(const std::string_view& name, uint32_t descriptor, ShaderClass shaderClass)
		{
			switch (shaderClass) {
			case ShaderClass::Pixel:
				return std::format(L"Data/ShaderCache/{}/{:X}.pso", std::wstring(name.begin(), name.end()), descriptor);
			case ShaderClass::Vertex:
				return std::format(L"Data/ShaderCache/{}/{:X}.vso", std::wstring(name.begin(), name.end()), descriptor);
			}
			return std::format(L"Data/ShaderCache/{}/{:X}.cso", std::wstring(name.begin(), name.end()), descriptor);
		}

		static std::string GetShaderString(ShaderClass shaderClass, const RE::BSShader& shader, uint32_t descriptor, bool hashkey)
		{
			auto sourceShaderFile = shader.fxpFilename;
			std::array<D3D_SHADER_MACRO, 64> defines{};
			SIE::SShaderCache::GetShaderDefines(shader.shaderType.get(), descriptor, &defines[0]);
			std::string result;
			if (hashkey)  // generate hashkey so don't include descriptor
				result = fmt::format("{}:{}:{}", sourceShaderFile, magic_enum::enum_name(shaderClass), SIE::SShaderCache::MergeDefinesString(defines, true));
			else
				result = fmt::format("{}:{}:{:X}:{}", sourceShaderFile, magic_enum::enum_name(shaderClass), descriptor, SIE::SShaderCache::MergeDefinesString(defines, true));
			return result;
		}

		std::string GetTypeFromShaderString(std::string a_key)
		{
			std::string type = "";
			std::string::size_type pos = a_key.find(':');
			if (pos != std::string::npos)
				type = a_key.substr(0, pos);
			return type;
		}

		static ID3DBlob* CompileShader(ShaderClass shaderClass, const RE::BSShader& shader, uint32_t descriptor, bool useDiskCache)
		{
			ID3DBlob* shaderBlob = nullptr;

			// check hashmap
			auto& cache = ShaderCache::Instance();
			if (shaderBlob = cache.GetCompletedShader(shaderClass, shader, descriptor); shaderBlob) {
				// already compiled before
				logger::debug("Shader already compiled; using cache: {}", SShaderCache::GetShaderString(shaderClass, shader, descriptor));
				cache.IncCacheHitTasks();
				return shaderBlob;
			}
			const auto type = shader.shaderType.get();

			// check diskcache
			auto diskPath = GetDiskPath(shader.fxpFilename, descriptor, shaderClass);

			if (!shaderBlob && useDiskCache && std::filesystem::exists(diskPath)) {
				shaderBlob = nullptr;
				// check build time of cache
				auto diskCacheTime = std::chrono::clock_cast<std::chrono::system_clock>(std::filesystem::last_write_time(diskPath));
				if (cache.ShaderModifiedSince(shader.fxpFilename, diskCacheTime)) {
					logger::debug("Diskcached shader {} older than {}", SIE::SShaderCache::GetShaderString(shaderClass, shader, descriptor, true), std::format("{:%Y%m%d%H%M}", diskCacheTime));
				} else if (FAILED(D3DReadFileToBlob(diskPath.c_str(), &shaderBlob))) {
					logger::error("Failed to load {} shader {}::{}", magic_enum::enum_name(shaderClass), magic_enum::enum_name(type), descriptor);

					if (shaderBlob != nullptr) {
						shaderBlob->Release();
					}
				} else {
					std::string str;
					std::transform(diskPath.begin(), diskPath.end(), std::back_inserter(str), [](wchar_t c) {
						return (char)c;
					});
					logger::debug("Loaded shader from {}", str);
					cache.AddCompletedShader(shaderClass, shader, descriptor, shaderBlob);
					return shaderBlob;
				}
			}

			// prepare preprocessor defines
			std::array<D3D_SHADER_MACRO, 64> defines{};
			auto lastIndex = 0;
			if (shaderClass == ShaderClass::Vertex) {
				defines[lastIndex++] = { "VSHADER", nullptr };
			} else if (shaderClass == ShaderClass::Pixel) {
				defines[lastIndex++] = { "PSHADER", nullptr };
			}
			if (State::GetSingleton()->IsDeveloperMode()) {
				defines[lastIndex++] = { "D3DCOMPILE_SKIP_OPTIMIZATION", nullptr };
				defines[lastIndex++] = { "D3DCOMPILE_DEBUG", nullptr };
			}
			if (REL::Module::IsVR())
				defines[lastIndex++] = { "VR", nullptr };
			auto shaderDefines = State::GetSingleton()->GetDefines();
			if (!shaderDefines->empty()) {
				for (unsigned int i = 0; i < shaderDefines->size(); i++)
					defines[lastIndex++] = { shaderDefines->at(i).first.c_str(), shaderDefines->at(i).second.c_str() };
			}
			defines[lastIndex] = { nullptr, nullptr };  // do final entry
			GetShaderDefines(type, descriptor, &defines[lastIndex]);

			const std::wstring path = GetShaderPath(shader.fxpFilename);

			std::string strPath;
			std::transform(path.begin(), path.end(), std::back_inserter(strPath), [](wchar_t c) {
				return (char)c;
			});
			logger::debug("Compiling {} {}:{}:{:X} to {}", strPath, magic_enum::enum_name(type), magic_enum::enum_name(shaderClass), descriptor, MergeDefinesString(defines));

			// compile shaders
			ID3DBlob* errorBlob = nullptr;
			const uint32_t flags = !State::GetSingleton()->IsDeveloperMode() ? D3DCOMPILE_OPTIMIZATION_LEVEL3 : D3DCOMPILE_DEBUG;
			const HRESULT compileResult = D3DCompileFromFile(path.c_str(), defines.data(), D3D_COMPILE_STANDARD_FILE_INCLUDE, "main",
				GetShaderProfile(shaderClass), flags, 0, &shaderBlob, &errorBlob);

			if (FAILED(compileResult)) {
				if (errorBlob != nullptr) {
					logger::error("Failed to compile {} shader {}::{}: {}",
						magic_enum::enum_name(shaderClass), magic_enum::enum_name(type), descriptor,
						static_cast<char*>(errorBlob->GetBufferPointer()));
					errorBlob->Release();
				} else {
					logger::error("Failed to compile {} shader {}::{}",
						magic_enum::enum_name(shaderClass), magic_enum::enum_name(type), descriptor);
				}
				if (shaderBlob != nullptr) {
					shaderBlob->Release();
				}

				cache.AddCompletedShader(shaderClass, shader, descriptor, nullptr);
				return nullptr;
			}
			logger::debug("Compiled shader {}:{}:{:X}", magic_enum::enum_name(type), magic_enum::enum_name(shaderClass), descriptor);

			// strip debug info
			if (!State::GetSingleton()->IsDeveloperMode()) {
				ID3DBlob* strippedShaderBlob = nullptr;

				const uint32_t stripFlags = D3DCOMPILER_STRIP_DEBUG_INFO |
				                            D3DCOMPILER_STRIP_REFLECTION_DATA |
				                            D3DCOMPILER_STRIP_TEST_BLOBS |
				                            D3DCOMPILER_STRIP_PRIVATE_DATA;

				D3DStripShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), stripFlags, &strippedShaderBlob);
				std::swap(shaderBlob, strippedShaderBlob);
				strippedShaderBlob->Release();
			}

			// save shader to disk
			if (useDiskCache) {
				auto directoryPath = std::format("Data/ShaderCache/{}", shader.fxpFilename);
				if (!std::filesystem::is_directory(directoryPath)) {
					try {
						std::filesystem::create_directories(directoryPath);
					} catch (std::filesystem::filesystem_error const& ex) {
						logger::error("Failed to create folder: {}", ex.what());
					}
				}

				const HRESULT saveResult = D3DWriteBlobToFile(shaderBlob, diskPath.c_str(), true);
				if (FAILED(saveResult)) {
					std::string str;
					std::transform(diskPath.begin(), diskPath.end(), std::back_inserter(str), [](wchar_t c) {
						return (char)c;
					});
					logger::error("Failed to save shader to {}", str);
				} else {
					std::string str;
					std::transform(diskPath.begin(), diskPath.end(), std::back_inserter(str), [](wchar_t c) {
						return (char)c;
					});
					logger::debug("Saved shader to {}", str);
				}
			}
			cache.AddCompletedShader(shaderClass, shader, descriptor, shaderBlob);
			return shaderBlob;
		}

		std::unique_ptr<RE::BSGraphics::VertexShader> CreateVertexShader(ID3DBlob& shaderData,
			RE::BSShader::Type type, uint32_t descriptor)
		{
			static const auto device = REL::Relocation<ID3D11Device**>(RE::Offset::D3D11Device);
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
			auto newShader = std::unique_ptr<RE::BSGraphics::VertexShader>(shaderPtr);
			newShader->byteCodeSize = (uint32_t)shaderData.GetBufferSize();
			newShader->id = descriptor;
			newShader->shaderDesc = 0;

			Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflector;
			const auto reflectionResult = D3DReflect(shaderData.GetBufferPointer(), shaderData.GetBufferSize(),
				IID_PPV_ARGS(&reflector));
			if (FAILED(reflectionResult)) {
				logger::error("Failed to reflect vertex shader {}::{}", magic_enum::enum_name(type),
					descriptor);
			} else {
				std::array<size_t, 3> bufferSizes = { 0, 0, 0 };
#pragma warning(push)
#pragma warning(disable: 4244)
				std::fill(newShader->constantTable.begin(), newShader->constantTable.end(), 0);
#pragma warning(pop)
				ReflectConstantBuffers(*reflector.Get(), bufferSizes, newShader->constantTable, newShader->shaderDesc,
					ShaderClass::Vertex, type, descriptor);
				if (bufferSizes[0] != 0) {
					newShader->constantBuffers[0].buffer =
						(RE::ID3D11Buffer*)perTechniqueBuffersArray.get()[bufferSizes[0]];
				} else {
					newShader->constantBuffers[0].buffer = nullptr;
					newShader->constantBuffers[0].data = bufferData.get();
				}
				if (bufferSizes[1] != 0) {
					newShader->constantBuffers[1].buffer =
						(RE::ID3D11Buffer*)perMaterialBuffersArray.get()[bufferSizes[1]];
				} else {
					newShader->constantBuffers[1].buffer = nullptr;
					newShader->constantBuffers[1].data = bufferData.get();
				}
				if (bufferSizes[2] != 0) {
					newShader->constantBuffers[2].buffer =
						(RE::ID3D11Buffer*)perGeometryBuffersArray.get()[bufferSizes[2]];
				} else {
					newShader->constantBuffers[2].buffer = nullptr;
					newShader->constantBuffers[2].data = bufferData.get();
				}
			}

			return newShader;
		}

		std::unique_ptr<RE::BSGraphics::PixelShader> CreatePixelShader(ID3DBlob& shaderData,
			RE::BSShader::Type type, uint32_t descriptor)
		{
			static const auto device = REL::Relocation<ID3D11Device**>(RE::Offset::D3D11Device);
			static const auto perTechniqueBuffersArray =
				REL::Relocation<ID3D11Buffer**>(RELOCATION_ID(524761, 411377));
			static const auto perMaterialBuffersArray =
				REL::Relocation<ID3D11Buffer**>(RELOCATION_ID(524763, 411379));
			static const auto perGeometryBuffersArray =
				REL::Relocation<ID3D11Buffer**>(RELOCATION_ID(524765, 411381));
			static const auto bufferData = REL::Relocation<void*>(RELOCATION_ID(524967, 411448));

			auto newShader = std::make_unique<RE::BSGraphics::PixelShader>();
			newShader->id = descriptor;

			Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflector;
			const auto reflectionResult = D3DReflect(shaderData.GetBufferPointer(),
				shaderData.GetBufferSize(), IID_PPV_ARGS(&reflector));
			if (FAILED(reflectionResult)) {
				logger::error("Failed to reflect vertex shader {}::{}", magic_enum::enum_name(type),
					descriptor);
			} else {
				std::array<size_t, 3> bufferSizes = { 0, 0, 0 };
#pragma warning(push)
#pragma warning(disable: 4244)
				std::fill(newShader->constantTable.begin(), newShader->constantTable.end(), 0);
#pragma warning(pop)
				uint64_t dummy;
				ReflectConstantBuffers(*reflector.Get(), bufferSizes, newShader->constantTable,
					dummy,
					ShaderClass::Pixel, type, descriptor);
				if (bufferSizes[0] != 0) {
					newShader->constantBuffers[0].buffer =
						(RE::ID3D11Buffer*)perTechniqueBuffersArray.get()[bufferSizes[0]];
				} else {
					newShader->constantBuffers[0].buffer = nullptr;
					newShader->constantBuffers[0].data = bufferData.get();
				}
				if (bufferSizes[1] != 0) {
					newShader->constantBuffers[1].buffer =
						(RE::ID3D11Buffer*)perMaterialBuffersArray.get()[bufferSizes[1]];
				} else {
					newShader->constantBuffers[1].buffer = nullptr;
					newShader->constantBuffers[1].data = bufferData.get();
				}
				if (bufferSizes[2] != 0) {
					newShader->constantBuffers[2].buffer =
						(RE::ID3D11Buffer*)perGeometryBuffersArray.get()[bufferSizes[2]];
				} else {
					newShader->constantBuffers[2].buffer = nullptr;
					newShader->constantBuffers[2].data = bufferData.get();
				}
			}

			return newShader;
		}
	}

	RE::BSGraphics::VertexShader* ShaderCache::GetVertexShader(const RE::BSShader& shader,
		uint32_t descriptor)
	{
		if (shader.shaderType.get() == RE::BSShader::Type::Effect) {
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Lighting)) {
			} else {
				return nullptr;
			}
		}

		auto state = State::GetSingleton();
		if (!((ShaderCache::IsSupportedShader(shader) || state->IsDeveloperMode() && state->IsShaderEnabled(shader)))) {
			return nullptr;
		}

		auto key = SIE::SShaderCache::GetShaderString(ShaderClass::Vertex, shader, descriptor, true);
		if (blockedKeyIndex != -1 && !blockedKey.empty() && key == blockedKey) {
			if (std::find(blockedIDs.begin(), blockedIDs.end(), descriptor) == blockedIDs.end()) {
				blockedIDs.push_back(descriptor);
				logger::debug("Skipping blocked shader {:X}:{} total: {}", descriptor, blockedKey, blockedIDs.size());
			}
			return nullptr;
		}
		{
			std::lock_guard lockGuard(vertexShadersMutex);
			auto& typeCache = vertexShaders[static_cast<size_t>(shader.shaderType.underlying())];
			auto it = typeCache.find(descriptor);
			if (it != typeCache.end()) {
				return it->second.get();
			}
		}

		if (IsAsync()) {
			compilationSet.Add({ ShaderClass::Vertex, shader, descriptor });
		} else {
			return MakeAndAddVertexShader(shader, descriptor);
		}

		return nullptr;
	}

	RE::BSGraphics::PixelShader* ShaderCache::GetPixelShader(const RE::BSShader& shader,
		uint32_t descriptor)
	{
		if (shader.shaderType.get() == RE::BSShader::Type::Effect) {
			if (descriptor & static_cast<uint32_t>(ShaderCache::EffectShaderFlags::Lighting)) {
			} else {
				return nullptr;
			}
		}

		auto state = State::GetSingleton();
		if (!(ShaderCache::IsSupportedShader(shader) || state->IsDeveloperMode() &&
															state->IsShaderEnabled(shader))) {
			return nullptr;
		}

		auto key = SIE::SShaderCache::GetShaderString(ShaderClass::Pixel, shader, descriptor, true);
		if (blockedKeyIndex != -1 && !blockedKey.empty() && key == blockedKey) {
			if (std::find(blockedIDs.begin(), blockedIDs.end(), descriptor) == blockedIDs.end()) {
				blockedIDs.push_back(descriptor);
				logger::debug("Skipping blocked shader {:X}:{} total: {}", descriptor, blockedKey, blockedIDs.size());
			}
			return nullptr;
		}
		{
			std::lock_guard lockGuard(pixelShadersMutex);
			auto& typeCache = pixelShaders[static_cast<size_t>(shader.shaderType.underlying())];
			auto it = typeCache.find(descriptor);
			if (it != typeCache.end()) {
				return it->second.get();
			}
		}

		if (IsAsync()) {
			compilationSet.Add({ ShaderClass::Pixel, shader, descriptor });
		} else {
			return MakeAndAddPixelShader(shader, descriptor);
		}

		return nullptr;
	}

	ShaderCache::~ShaderCache()
	{
		Clear();
		fileWatcher->removeWatch(watchID);
	}

	void ShaderCache::Clear()
	{
		for (auto& shaders : vertexShaders) {
			for (auto& [id, shader] : shaders) {
				shader->shader->Release();
			}
			shaders.clear();
		}
		for (auto& shaders : pixelShaders) {
			for (auto& [id, shader] : shaders) {
				shader->shader->Release();
			}
			shaders.clear();
		}

		compilationSet.Clear();
		std::unique_lock lock{ mapMutex };
		shaderMap.clear();
	}

	void ShaderCache::Clear(RE::BSShader::Type a_type)
	{
		logger::debug("Clearing cache for {}", magic_enum::enum_name(a_type));
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
		compilationSet.Clear();
	}

	bool ShaderCache::AddCompletedShader(ShaderClass shaderClass, const RE::BSShader& shader, uint32_t descriptor, ID3DBlob* a_blob)
	{
		auto key = SIE::SShaderCache::GetShaderString(shaderClass, shader, descriptor, true);
		auto status = a_blob ? ShaderCompilationTask::Status::Completed : ShaderCompilationTask::Status::Failed;
		std::unique_lock lock{ mapMutex };
		logger::debug("Adding {} shader to map: {}", magic_enum ::enum_name(status), key);
		shaderMap.insert_or_assign(key, ShaderCacheResult{ a_blob, status, system_clock::now() });
		return (bool)a_blob;
	}

	ID3DBlob* ShaderCache::GetCompletedShader(const std::string a_key)
	{
		std::string type = SIE::SShaderCache::GetTypeFromShaderString(a_key);
		std::scoped_lock lock{ mapMutex };
		if (!shaderMap.empty() && shaderMap.contains(a_key)) {
			if (ShaderModifiedSince(type, shaderMap.at(a_key).compileTime)) {
				logger::debug("Shader {} compiled {} before changes at {}", a_key, std::format("{:%Y%m%d%H%M}", shaderMap.at(a_key).compileTime), std::format("{:%Y%m%d%H%M}", GetModifiedShaderMapTime(type)));
				return nullptr;
			}
			auto status = shaderMap.at(a_key).status;
			if (status != ShaderCompilationTask::Status::Pending)
				return shaderMap.at(a_key).blob;
		}
		return nullptr;
	}

	ID3DBlob* ShaderCache::GetCompletedShader(ShaderClass shaderClass, const RE::BSShader& shader,
		uint32_t descriptor)
	{
		auto key = SIE::SShaderCache::GetShaderString(shaderClass, shader, descriptor, true);
		return GetCompletedShader(key);
	}

	ID3DBlob* ShaderCache::GetCompletedShader(const ShaderCompilationTask& a_task)
	{
		auto key = a_task.GetString();
		return GetCompletedShader(key);
	}

	ShaderCompilationTask::Status ShaderCache::GetShaderStatus(const std::string a_key)
	{
		std::scoped_lock lock{ mapMutex };
		if (!shaderMap.empty() && shaderMap.contains(a_key)) {
			return shaderMap.at(a_key).status;
		}
		return ShaderCompilationTask::Status::Pending;
	}

	std::string ShaderCache::GetShaderStatsString(bool a_timeOnly)
	{
		return compilationSet.GetStatsString(a_timeOnly);
	}

	bool ShaderCache::IsCompiling()
	{
		return compilationSet.totalTasks && compilationSet.completedTasks + compilationSet.failedTasks < compilationSet.totalTasks;
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

	bool ShaderCache::IsDiskCache() const
	{
		return isDiskCache;
	}

	void ShaderCache::SetDiskCache(bool value)
	{
		isDiskCache = value;
	}

	void ShaderCache::DeleteDiskCache()
	{
		std::scoped_lock lock{ compilationSet.compilationMutex };
		try {
			std::filesystem::remove_all(L"Data/ShaderCache");
			logger::info("Deleted disk cache");
		} catch (std::filesystem::filesystem_error const& ex) {
			logger::error("Failed to delete disk cache: {}", ex.what());
		}
	}

	void ShaderCache::ValidateDiskCache()
	{
		CSimpleIniA ini;
		ini.SetUnicode();
		ini.LoadFile(L"Data\\ShaderCache\\Info.ini");
		bool valid = true;

		if (auto version = ini.GetValue("Cache", "Version")) {
			if (strcmp(SHADER_CACHE_VERSION.string().c_str(), version) != 0 || !(State::GetSingleton()->ValidateCache(ini))) {
				logger::info("Disk cache outdated or invalid");
				valid = false;
			}
		} else {
			logger::info("Disk cache outdated or invalid");
			valid = false;
		}

		if (valid) {
			logger::info("Using disk cache");
		} else {
			DeleteDiskCache();
		}
	}

	void ShaderCache::WriteDiskCacheInfo()
	{
		CSimpleIniA ini;
		ini.SetUnicode();
		ini.SetValue("Cache", "Version", SHADER_CACHE_VERSION.string().c_str());
		State::GetSingleton()->WriteDiskCacheInfo(ini);
		ini.SaveFile(L"Data\\ShaderCache\\Info.ini");
		logger::info("Saved disk cache info");
	}

	ShaderCache::ShaderCache()
	{
		logger::debug("ShaderCache initialized with {} compiler threads", (int)compilationThreadCount);
		compilationPool.push_task(&ShaderCache::ManageCompilationSet, this, ssource.get_token());
	}

	void ShaderCache::StartFileWatcher()
	{
		fileWatcher = new efsw::FileWatcher();
		listener = new UpdateListener();
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
	}

	bool ShaderCache::ShaderModifiedSince(std::string a_type, system_clock::time_point a_current)
	{
		if (a_type.empty() || magic_enum::enum_cast<RE::BSShader::Type>(a_type, magic_enum::case_insensitive).has_value())  // type is invalid
			return false;
		std::filesystem::path filePath{ SIE::SShaderCache::GetShaderPath(a_type) };
		std::lock_guard lockGuard(modifiedMapMutex);
		if (std::filesystem::exists(filePath) &&
			(modifiedShaderMap.empty() || !modifiedShaderMap.contains(a_type)))  // insert timestamp when first seen; rely on filewatcher for subsequent changes
			modifiedShaderMap.insert_or_assign(a_type, std::chrono::clock_cast<std::chrono::system_clock>(std::filesystem::last_write_time(filePath)));
		return !modifiedShaderMap.empty() && modifiedShaderMap.contains(a_type)  // map has Type
		       && modifiedShaderMap.at(a_type) > a_current;                      //modification time is older than a_current
	}

	RE::BSGraphics::VertexShader* ShaderCache::MakeAndAddVertexShader(const RE::BSShader& shader,
		uint32_t descriptor)
	{
		if (const auto shaderBlob =
				SShaderCache::CompileShader(ShaderClass::Vertex, shader, descriptor, isDiskCache)) {
			static const auto device = REL::Relocation<ID3D11Device**>(RE::Offset::D3D11Device);

			auto newShader = SShaderCache::CreateVertexShader(*shaderBlob, shader.shaderType.get(),
				descriptor);

			std::lock_guard lockGuard(vertexShadersMutex);

			const auto result = (*device)->CreateVertexShader(shaderBlob->GetBufferPointer(),
				newShader->byteCodeSize, nullptr, &newShader->shader);
			if (FAILED(result)) {
				logger::error("Failed to create vertex shader {}::{}",
					magic_enum::enum_name(shader.shaderType.get()), descriptor);
				if (newShader->shader != nullptr) {
					newShader->shader->Release();
				}
			} else {
				return vertexShaders[static_cast<size_t>(shader.shaderType.get())]
				    .insert_or_assign(descriptor, std::move(newShader))
				    .first->second.get();
			}
		}
		return nullptr;
	}

	RE::BSGraphics::PixelShader* ShaderCache::MakeAndAddPixelShader(const RE::BSShader& shader,
		uint32_t descriptor)
	{
		if (const auto shaderBlob =
				SShaderCache::CompileShader(ShaderClass::Pixel, shader, descriptor, isDiskCache)) {
			static const auto device = REL::Relocation<ID3D11Device**>(RE::Offset::D3D11Device);

			auto newShader = SShaderCache::CreatePixelShader(*shaderBlob, shader.shaderType.get(),
				descriptor);

			std::lock_guard lockGuard(pixelShadersMutex);
			const auto result = (*device)->CreatePixelShader(shaderBlob->GetBufferPointer(),
				shaderBlob->GetBufferSize(), nullptr, &newShader->shader);
			if (FAILED(result)) {
				logger::error("Failed to create pixel shader {}::{}",
					magic_enum::enum_name(shader.shaderType.get()),
					descriptor);
				if (newShader->shader != nullptr) {
					newShader->shader->Release();
				}
			} else {
				return pixelShaders[static_cast<size_t>(shader.shaderType.get())]
				    .insert_or_assign(descriptor, std::move(newShader))
				    .first->second.get();
			}
		}
		return nullptr;
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

	uint64_t ShaderCache::GetTotalTasks()
	{
		return compilationSet.totalTasks;
	}
	void ShaderCache::IncCacheHitTasks()
	{
		compilationSet.cacheHitTasks++;
	}

	bool ShaderCache::IsHideErrors()
	{
		return hideError;
	}

	void ShaderCache::InsertModifiedShaderMap(std::string a_shader, std::chrono::time_point<std::chrono::system_clock> a_time)
	{
		std::lock_guard lockGuard(modifiedMapMutex);
		modifiedShaderMap.insert_or_assign(a_shader, a_time);
	}

	std::chrono::time_point<std::chrono::system_clock> ShaderCache::GetModifiedShaderMapTime(std::string a_shader)
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
		std::scoped_lock lock{ mapMutex };
		auto targetIndex = a_forward ? 0 : shaderMap.size() - 1;           // default start or last element
		if (blockedKeyIndex >= 0 && shaderMap.size() > blockedKeyIndex) {  // grab next element
			targetIndex = (blockedKeyIndex + (a_forward ? 1 : -1)) % shaderMap.size();
		}
		auto index = 0;
		for (auto& [key, value] : shaderMap) {
			if (index++ == targetIndex) {
				blockedKey = key;
				blockedKeyIndex = (uint)targetIndex;
				blockedIDs.clear();
				logger::debug("Blocking shader ({}/{}) {}", blockedKeyIndex + 1, shaderMap.size(), blockedKey);
				return;
			}
		}
	}

	void ShaderCache::DisableShaderBlocking()
	{
		blockedKey = "";
		blockedKeyIndex = (uint)-1;
		blockedIDs.clear();
		logger::debug("Stopped blocking shaders");
	}

	void ShaderCache::ManageCompilationSet(std::stop_token stoken)
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		while (!stoken.stop_requested()) {
			const auto& task = compilationSet.WaitTake(stoken);
			if (!task.has_value())
				break;  // exit because thread told to end
			compilationPool.push_task(&ShaderCache::ProcessCompilationSet, this, stoken, task.value());
		}
	}

	void ShaderCache::ProcessCompilationSet(std::stop_token stoken, SIE::ShaderCompilationTask task)
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		task.Perform();
		compilationSet.Complete(task);
	}

	ShaderCompilationTask::ShaderCompilationTask(ShaderClass aShaderClass,
		const RE::BSShader& aShader,
		uint32_t aDescriptor) :
		shaderClass(aShaderClass),
		shader(aShader), descriptor(aDescriptor)
	{}

	void ShaderCompilationTask::Perform() const
	{
		if (shaderClass == ShaderClass::Vertex) {
			ShaderCache::Instance().MakeAndAddVertexShader(shader, descriptor);
		} else if (shaderClass == ShaderClass::Pixel) {
			ShaderCache::Instance().MakeAndAddPixelShader(shader, descriptor);
		}
	}

	size_t ShaderCompilationTask::GetId() const
	{
		return descriptor + (static_cast<size_t>(shader.shaderType.underlying()) << 32) +
		       (static_cast<size_t>(shaderClass) << 60);
	}

	std::string ShaderCompilationTask::GetString() const
	{
		return SIE::SShaderCache::GetShaderString(shaderClass, shader, descriptor, true);
	}

	bool ShaderCompilationTask::operator==(const ShaderCompilationTask& other) const
	{
		return GetId() == other.GetId();
	}

	std::optional<ShaderCompilationTask> CompilationSet::WaitTake(std::stop_token stoken)
	{
		std::unique_lock lock(compilationMutex);
		auto& shaderCache = ShaderCache::Instance();
		if (!conditionVariable.wait(
				lock, stoken,
				[this, &shaderCache]() { return !availableTasks.empty() &&
			                                    // check against all tasks in queue to trickle the work. It cannot be the active tasks count because the thread pool itself is maximum.
			                                    (int)shaderCache.compilationPool.get_tasks_total() <=
			                                        (!shaderCache.backgroundCompilation ? shaderCache.compilationThreadCount : shaderCache.backgroundCompilationThreadCount); })) {
			/*Woke up because of a stop request. */
			return std::nullopt;
		}
		if (!ShaderCache::Instance().IsCompiling()) {  // we just got woken up because there's a task, start clock
			lastCalculation = lastReset = high_resolution_clock::now();
		}
		auto node = availableTasks.extract(availableTasks.begin());
		auto& task = node.value();
		tasksInProgress.insert(std::move(node));
		return task;
	}

	void CompilationSet::Add(const ShaderCompilationTask& task)
	{
		std::unique_lock lock(compilationMutex);
		auto inProgressIt = tasksInProgress.find(task);
		auto processedIt = processedTasks.find(task);
		if (inProgressIt == tasksInProgress.end() && processedIt == processedTasks.end() && !ShaderCache::Instance().GetCompletedShader(task)) {
			auto [availableIt, wasAdded] = availableTasks.insert(task);
			lock.unlock();
			if (wasAdded) {
				conditionVariable.notify_one();
				totalTasks++;
			}
		}
	}

	void CompilationSet::Complete(const ShaderCompilationTask& task)
	{
		auto& cache = ShaderCache::Instance();
		auto key = task.GetString();
		auto shaderBlob = cache.GetCompletedShader(task);
		if (shaderBlob) {
			logger::debug("Compiling Task succeeded: {}", key);
			completedTasks++;
		} else {
			logger::debug("Compiling Task failed: {}", key);
			failedTasks++;
		}
		auto now = high_resolution_clock::now();
		totalMs += duration_cast<milliseconds>(now - lastCalculation).count();
		lastCalculation = now;
		std::scoped_lock lock(compilationMutex);
		processedTasks.insert(task);
		tasksInProgress.erase(task);
		conditionVariable.notify_one();
	}

	void CompilationSet::Clear()
	{
		std::scoped_lock lock(compilationMutex);
		availableTasks.clear();
		tasksInProgress.clear();
		processedTasks.clear();
		totalTasks = 0;
		completedTasks = 0;
		failedTasks = 0;
		cacheHitTasks = 0;
		lastReset = high_resolution_clock::now();
		lastCalculation = high_resolution_clock::now();
		totalMs = (double)duration_cast<std::chrono::milliseconds>(lastReset - lastReset).count();
	}

	std::string CompilationSet::GetHumanTime(double a_totalms)
	{
		int milliseconds = (int)a_totalms;
		int seconds = milliseconds / 1000;
		milliseconds %= 1000;
		int minutes = seconds / 60;
		seconds %= 60;
		int hours = minutes / 60;
		minutes %= 60;

		return fmt::format("{:02}:{:02}:{:02}", hours, minutes, seconds);
	}

	double CompilationSet::GetEta()
	{
		auto rate = completedTasks / totalMs;
		auto remaining = totalTasks - completedTasks - failedTasks;
		return std::max(remaining / rate, 0.0);
	}

	std::string CompilationSet::GetStatsString(bool a_timeOnly)
	{
		if (a_timeOnly)
			return fmt::format("{}/{}",
				GetHumanTime(totalMs),
				GetHumanTime(GetEta() + totalMs));
		return fmt::format("{}/{} (successful/total)\tfailed: {}\tcachehits: {}\nElapsed/Estimated Time: {}/{}",
			(std::uint64_t)completedTasks,
			(std::uint64_t)totalTasks,
			(std::uint64_t)failedTasks,
			(std::uint64_t)cacheHitTasks,
			GetHumanTime(totalMs),
			GetHumanTime(GetEta() + totalMs));
	}

	void UpdateListener::handleFileAction(efsw::WatchID, const std::string& dir, const std::string& filename, efsw::Action action, std::string)
	{
		auto& cache = SIE::ShaderCache::Instance();
		const std::filesystem::path filePath = std::filesystem::path(std::format("{}\\{}", dir, filename));
		std::chrono::time_point<std::chrono::system_clock> modifiedTime{};
		std::string extension = filePath.extension().string();
		std::string parentDir = filePath.parent_path().string();
		std::string shaderTypeString = filePath.stem().string();
		auto shaderType = magic_enum::enum_cast<RE::BSShader::Type>(shaderTypeString, magic_enum::case_insensitive);
		switch (action) {
		case efsw::Actions::Add:
			break;
		case efsw::Actions::Delete:
			break;
		case efsw::Actions::Modified:
			logger::debug("Detected changed path {}", filePath.string());
			if (std::filesystem::exists(filePath))
				modifiedTime = std::chrono::clock_cast<std::chrono::system_clock>(std::filesystem::last_write_time(filePath));
			else  // if file doesn't exist, don't do anything
				return;
			if (!std::filesystem::is_directory(filePath) && extension.starts_with(".hlsl") && parentDir.ends_with("Shaders") && shaderType.has_value()) {  // TODO: Case insensitive checks
				// Shader types, so only invalidate specific shader type (e.g,. Lighting)
				cache.InsertModifiedShaderMap(shaderTypeString, modifiedTime);
				cache.Clear(shaderType.value());
			} else if (!std::filesystem::is_directory(filePath) && extension.starts_with(".hlsl")) {  // TODO: Case insensitive checks
				// all other shaders, since we don't know what is using it, clear everything
				cache.DeleteDiskCache();
				cache.Clear();
			}
			break;
		case efsw::Actions::Moved:
			break;
		default:
			logger::error("Filewatcher received invalid action {}", magic_enum::enum_name(action));
		}
	}
}