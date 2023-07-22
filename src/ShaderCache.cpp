#include "ShaderCache.h"

#include <RE/V/VertexDesc.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "Features/ExtendedMaterials.h"
#include "Features/GrassCollision.h"
#include "Features/ScreenSpaceShadows.h"
#include "Features/WaterBlending.h"
#include "State.h"

namespace SIE
{
	namespace SShaderCache
	{
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

		enum class LightingShaderFlags
		{
			VC = 1 << 0,
			Skinned = 1 << 1,
			ModelSpaceNormals = 1 << 2,
			// flags 3 to 8 are unused
			Specular = 1 << 9,
			SoftLighting = 1 << 10,
			RimLighting = 1 << 11,
			BackLighting = 1 << 12,
			ShadowDir = 1 << 13,
			DefShadow = 1 << 14,
			ProjectedUV = 1 << 15,
			AnisoLighting = 1 << 16,
			AmbientSpecular = 1 << 17,
			WorldMap = 1 << 18,
			BaseObjectIsSnow = 1 << 19,
			DoAlphaTest = 1 << 20,
			Snow = 1 << 21,
			CharacterLight = 1 << 22,
			AdditionalAlphaMask = 1 << 23,
		};

		static void GetLightingShaderDefines(uint32_t descriptor,
			D3D_SHADER_MACRO* defines)
		{
			static REL::Relocation<void(uint32_t, D3D_SHADER_MACRO*)> VanillaGetLightingShaderDefines(
				RELOCATION_ID(101631, 108698));

			const auto technique =
				static_cast<LightingShaderTechniques>(GetTechnique(descriptor));

			if (technique == LightingShaderTechniques::Outline) {
				defines[0] = { "OUTLINE", nullptr };
				++defines;
			}

			if (ScreenSpaceShadows::GetSingleton()->loaded) {
				defines[0] = { "SCREEN_SPACE_SHADOWS", nullptr };
				++defines;
			}

			if (ExtendedMaterials::GetSingleton()->loaded) {
				defines[0] = { "COMPLEX_PARALLAX_MATERIALS", nullptr };
				++defines;
			}

			if (REL::Module::IsVR()) {
				defines[0] = { "VR", nullptr };
				++defines;
			}
			VanillaGetLightingShaderDefines(descriptor, defines);
		}

		enum class BloodSplatterShaderTechniques
		{
			Splatter = 0,
			Flare = 1,
		};

		static void GetBloodSplaterShaderDefines(uint32_t descriptor, D3D_SHADER_MACRO* defines)
		{
			if (descriptor == static_cast<uint32_t>(BloodSplatterShaderTechniques::Splatter)) {
				defines[0] = { "SPLATTER", nullptr };
				++defines;
			} else if (descriptor == static_cast<uint32_t>(BloodSplatterShaderTechniques::Flare)) {
				defines[0] = { "FLARE", nullptr };
				++defines;
			}
			defines[0] = { nullptr, nullptr };
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
			if (technique == static_cast<uint32_t>(DistantTreeShaderTechniques::Depth)) {
				defines[0] = { "RENDER_DEPTH", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(DistantTreeShaderFlags::AlphaTest)) {
				defines[0] = { "DO_ALPHA_TEST", nullptr };
				++defines;
			}

			if (ScreenSpaceShadows::GetSingleton()->loaded) {
				defines[0] = { "SCREEN_SPACE_SHADOWS", nullptr };
				++defines;
			}

			defines[0] = { nullptr, nullptr };
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
			switch (technique) {
			case SkyShaderTechniques::SunOcclude:
				{
					defines[0] = { "OCCLUSION", nullptr };
					++defines;
					break;
				}
			case SkyShaderTechniques::SunGlare:
				{
					defines[0] = { "TEX", nullptr };
					defines[1] = { "DITHER", nullptr };
					defines += 2;
					break;
				}
			case SkyShaderTechniques::MoonAndStarsMask:
				{
					defines[0] = { "TEX", nullptr };
					defines[1] = { "MOONMASK", nullptr };
					defines += 2;
					break;
				}
			case SkyShaderTechniques::Stars:
				{
					defines[0] = { "HORIZFADE", nullptr };
					++defines;
					break;
				}
			case SkyShaderTechniques::Clouds:
				{
					defines[0] = { "TEX", nullptr };
					defines[1] = { "CLOUDS", nullptr };
					defines += 2;
					break;
				}
			case SkyShaderTechniques::CloudsLerp:
				{
					defines[0] = { "TEX", nullptr };
					defines[1] = { "CLOUDS", nullptr };
					defines[2] = { "TEXLERP", nullptr };
					defines += 3;
					break;
				}
			case SkyShaderTechniques::CloudsFade:
				{
					defines[0] = { "TEX", nullptr };
					defines[1] = { "CLOUDS", nullptr };
					defines[2] = { "TEXFADE", nullptr };
					defines += 3;
					break;
				}
			case SkyShaderTechniques::Texture:
				{
					defines[0] = { "TEX", nullptr };
					++defines;
					break;
				}
			case SkyShaderTechniques::Sky:
				{
					defines[0] = { "DITHER", nullptr };
					++defines;
					break;
				}
			}

			defines[0] = { nullptr, nullptr };
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
			if (technique == static_cast<uint32_t>(GrassShaderTechniques::RenderDepth)) {
				defines[0] = { "RENDER_DEPTH", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(GrassShaderFlags::AlphaTest)) {
				defines[0] = { "DO_ALPHA_TEST", nullptr };
				++defines;
			}

			if (GrassCollision::GetSingleton()->loaded) {
				defines[0] = { "GRASS_COLLISION", nullptr };
				++defines;
			}

			if (ScreenSpaceShadows::GetSingleton()->loaded) {
				defines[0] = { "SCREEN_SPACE_SHADOWS", nullptr };
				++defines;
			}

			if (REL::Module::IsVR()) {
				defines[0] = { "VR", nullptr };
				++defines;
			}
			defines[0] = { nullptr, nullptr };
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
			switch (technique) {
			case ParticleShaderTechniques::ParticlesGryColor:
				{
					defines[0] = { "GRAYSCALE_TO_COLOR", nullptr };
					++defines;
					break;
				}
			case ParticleShaderTechniques::ParticlesGryAlpha:
				{
					defines[0] = { "GRAYSCALE_TO_ALPHA", nullptr };
					++defines;
					break;
				}
			case ParticleShaderTechniques::ParticlesGryColorAlpha:
				{
					defines[0] = { "GRAYSCALE_TO_COLOR", nullptr };
					defines[1] = { "GRAYSCALE_TO_ALPHA", nullptr };
					defines += 2;
					break;
				}
			case ParticleShaderTechniques::EnvCubeSnow:
				{
					defines[0] = { "ENVCUBE", nullptr };
					defines[1] = { "SNOW", nullptr };
					defines += 2;
					break;
				}
			case ParticleShaderTechniques::EnvCubeRain:
				{
					defines[0] = { "ENVCUBE", nullptr };
					defines[1] = { "RAIN", nullptr };
					defines += 2;
					break;
				}
			}

			defines[0] = { nullptr, nullptr };
		}

		enum class EffectShaderFlags
		{
			Vc = 1 << 0,
			TexCoord = 1 << 1,
			TexCoordIndex = 1 << 2,
			Skinned = 1 << 3,
			Normals = 1 << 4,
			BinormalTangent = 1 << 5,
			Texture = 1 << 6,
			IndexedTexture = 1 << 7,
			Falloff = 1 << 8,
			AddBlend = 1 << 10,
			MultBlend = 1 << 11,
			Particles = 1 << 12,
			StripParticles = 1 << 13,
			Blood = 1 << 14,
			Membrane = 1 << 15,
			Lighting = 1 << 16,
			ProjectedUv = 1 << 17,
			Soft = 1 << 18,
			GrayscaleToColor = 1 << 19,
			GrayscaleToAlpha = 1 << 20,
			IgnoreTexAlpha = 1 << 21,
			MultBlendDecal = 1 << 22,
			AlphaTest = 1 << 23,
			SkyObject = 1 << 24,
			MsnSpuSkinned = 1 << 25,
			MotionVectorsNormals = 1 << 26,
		};

		static void GetEffectShaderDefines(uint32_t descriptor, D3D_SHADER_MACRO* defines)
		{
			if (descriptor & static_cast<uint32_t>(EffectShaderFlags::Vc)) {
				defines[0] = { "VC", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(EffectShaderFlags::TexCoord)) {
				defines[0] = { "TEXCOORD", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(EffectShaderFlags::TexCoordIndex)) {
				defines[0] = { "TEXCOORD_INDEX", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(EffectShaderFlags::Skinned)) {
				defines[0] = { "SKINNED", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(EffectShaderFlags::Normals)) {
				defines[0] = { "NORMALS", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(EffectShaderFlags::BinormalTangent)) {
				defines[0] = { "BINORMAL_TANGENT", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(EffectShaderFlags::Texture)) {
				defines[0] = { "TEXTURE", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(EffectShaderFlags::IndexedTexture)) {
				defines[0] = { "INDEXED_TEXTURE", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(EffectShaderFlags::Falloff)) {
				defines[0] = { "FALLOFF", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(EffectShaderFlags::AddBlend)) {
				defines[0] = { "ADDBLEND", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(EffectShaderFlags::MultBlend)) {
				defines[0] = { "MULTBLEND", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(EffectShaderFlags::Particles)) {
				defines[0] = { "PARTICLES", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(EffectShaderFlags::StripParticles)) {
				defines[0] = { "STRIP_PARTICLES", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(EffectShaderFlags::Blood)) {
				defines[0] = { "BLOOD", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(EffectShaderFlags::Membrane)) {
				defines[0] = { "MEMBRANE", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(EffectShaderFlags::Lighting)) {
				defines[0] = { "LIGHTING", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(EffectShaderFlags::ProjectedUv)) {
				defines[0] = { "PROJECTED_UV", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(EffectShaderFlags::Soft)) {
				defines[0] = { "SOFT", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(EffectShaderFlags::GrayscaleToColor)) {
				defines[0] = { "GRAYSCALE_TO_COLOR", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(EffectShaderFlags::GrayscaleToAlpha)) {
				defines[0] = { "GRAYSCALE_TO_ALPHA", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(EffectShaderFlags::IgnoreTexAlpha)) {
				defines[0] = { "IGNORE_TEX_ALPHA", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(EffectShaderFlags::MultBlendDecal)) {
				defines[0] = { "MULTBLEND_DECAL", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(EffectShaderFlags::AlphaTest)) {
				defines[0] = { "ALPHA_TEST", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(EffectShaderFlags::SkyObject)) {
				defines[0] = { "SKY_OBJECT", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(EffectShaderFlags::MsnSpuSkinned)) {
				defines[0] = { "MSN_SPU_SKINNED", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(EffectShaderFlags::MotionVectorsNormals)) {
				defines[0] = { "MOTIONVECTORS_NORMALS", nullptr };
				++defines;
			}

			defines[0] = { nullptr, nullptr };
		}

		enum class WaterShaderTechniques
		{
			Underwater = 8,
			Lod = 9,
			Stencil = 10,
			Simple = 11,
		};

		enum class WaterShaderFlags
		{
			Vc = 1 << 0,
			NormalTexCoord = 1 << 1,
			Reflections = 1 << 2,
			Refractions = 1 << 3,
			Depth = 1 << 4,
			Interior = 1 << 5,
			Wading = 1 << 6,
			VertexAlphaDepth = 1 << 7,
			Cubemap = 1 << 8,
			Flowmap = 1 << 9,
			BlendNormals = 1 << 10,
		};

		static void GetWaterShaderDefines(uint32_t descriptor, D3D_SHADER_MACRO* defines)
		{
			defines[0] = { "WATER", nullptr };
			defines[1] = { "FOG", nullptr };
			defines += 2;

			if (descriptor & static_cast<uint32_t>(WaterShaderFlags::Vc)) {
				defines[0] = { "VC", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(WaterShaderFlags::NormalTexCoord)) {
				defines[0] = { "NORMAL_TEXCOORD", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(WaterShaderFlags::Reflections)) {
				defines[0] = { "REFLECTIONS", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(WaterShaderFlags::Refractions)) {
				defines[0] = { "REFRACTIONS", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(WaterShaderFlags::Depth)) {
				defines[0] = { "DEPTH", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(WaterShaderFlags::Interior)) {
				defines[0] = { "INTERIOR", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(WaterShaderFlags::Wading)) {
				defines[0] = { "WADING", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(WaterShaderFlags::VertexAlphaDepth)) {
				defines[0] = { "VERTEX_ALPHA_DEPTH", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(WaterShaderFlags::Cubemap)) {
				defines[0] = { "CUBEMAP", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(WaterShaderFlags::Flowmap)) {
				defines[0] = { "FLOWMAP", nullptr };
				++defines;
			}
			if (descriptor & static_cast<uint32_t>(WaterShaderFlags::BlendNormals)) {
				defines[0] = { "BLEND_NORMALS", nullptr };
				++defines;
			}

			const auto technique = (descriptor >> 11) & 0xF;
			if (technique == static_cast<uint32_t>(WaterShaderTechniques::Underwater)) {
				defines[0] = { "UNDERWATER", nullptr };
				++defines;
			} else if (technique == static_cast<uint32_t>(WaterShaderTechniques::Lod)) {
				defines[0] = { "LOD", nullptr };
				++defines;
			} else if (technique == static_cast<uint32_t>(WaterShaderTechniques::Stencil)) {
				defines[0] = { "STENCIL", nullptr };
				++defines;
			} else if (technique == static_cast<uint32_t>(WaterShaderTechniques::Simple)) {
				defines[0] = { "SIMPLE", nullptr };
				++defines;
			} else if (technique < 8) {
				static constexpr std::array<const char*, 8> numLightDefines = { { "0", "1", "2", "3", "4",
					"5", "6", "7" } };
				defines[0] = { "SPECULAR", nullptr };
				defines[1] = { "NUM_SPECULAR_LIGHTS", numLightDefines[technique] };
				defines += 2;
			}

			if (WaterBlending::GetSingleton()->loaded) {
				defines[0] = { "WATER_BLENDING", nullptr };
				++defines;
			}

			defines[0] = { nullptr, nullptr };
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

		static std::string MergeDefinesString(const std::array<D3D_SHADER_MACRO, 64>& defines)
		{
			std::string result;
			for (const auto& def : defines) {
				if (def.Name != nullptr) {
					result += def.Name;
					result += ' ';
				} else {
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

		static ID3DBlob* CompileShader(ShaderClass shaderClass, const RE::BSShader& shader,
			uint32_t descriptor, bool useDiskCache)
		{
			const auto type = shader.shaderType.get();
			const std::wstring path = GetShaderPath(shader.fxpFilename);

			std::array<D3D_SHADER_MACRO, 64> defines;
			if (shaderClass == ShaderClass::Vertex) {
				defines[0] = { "VSHADER", nullptr };
			} else if (shaderClass == ShaderClass::Pixel) {
				defines[0] = { "PSHADER", nullptr };
			}
			defines[1] = { nullptr, nullptr };
			GetShaderDefines(type, descriptor, &defines[1]);

			logger::debug("{}, {}", descriptor, MergeDefinesString(defines));

			auto diskPath = GetDiskPath(shader.fxpFilename, descriptor, shaderClass);

			if (useDiskCache && std::filesystem::exists(diskPath)) {
				ID3DBlob* shaderBlob = nullptr;
				if (FAILED(D3DReadFileToBlob(diskPath.c_str(), &shaderBlob))) {
					logger::error("Failed to load {} shader {}::{}", magic_enum::enum_name(shaderClass), magic_enum::enum_name(type), descriptor);

					if (shaderBlob != nullptr) {
						shaderBlob->Release();
					}
				} else {
					std::string str;
					;
					std::transform(diskPath.begin(), diskPath.end(), std::back_inserter(str), [](wchar_t c) {
						return (char)c;
					});
					logger::debug("Loaded shader from {}", str);
					return shaderBlob;
				}
			}

			ID3DBlob* shaderBlob = nullptr;
			ID3DBlob* errorBlob = nullptr;
			const uint32_t flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
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

				return nullptr;
			}
			logger::debug("Compiled {} shader {}::{}", magic_enum::enum_name(shaderClass),
				magic_enum::enum_name(type), descriptor);

			ID3DBlob* strippedShaderBlob = nullptr;

			const uint32_t stripFlags = D3DCOMPILER_STRIP_DEBUG_INFO |
			                            D3DCOMPILER_STRIP_REFLECTION_DATA |
			                            D3DCOMPILER_STRIP_TEST_BLOBS |
			                            D3DCOMPILER_STRIP_PRIVATE_DATA;

			D3DStripShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), stripFlags, &strippedShaderBlob);
			std::swap(shaderBlob, strippedShaderBlob);
			strippedShaderBlob->Release();

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
					;
					std::transform(diskPath.begin(), diskPath.end(), std::back_inserter(str), [](wchar_t c) {
						return (char)c;
					});
					logger::error("Failed to save shader to {}", str);
				} else {
					std::string str;
					;
					std::transform(diskPath.begin(), diskPath.end(), std::back_inserter(str), [](wchar_t c) {
						return (char)c;
					});
					logger::debug("Saved shader to {}", str);
				}
			}

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
		if (!ShaderCache::IsSupportedShader(shader)) {
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
		if (!ShaderCache::IsSupportedShader(shader)) {
			return nullptr;
		}

		if (shader.shaderType.get() == RE::BSShader::Type::Water) {
			const auto technique = (descriptor >> 11) & 0xF;
			if (technique == 9) {  // LOD
				return nullptr;
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
		std::lock_guard lock(compilationSet.mutex);
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
		static const auto compilationThreadCount = static_cast<int32_t>(std::thread::hardware_concurrency());
		for (size_t threadIndex = 0; threadIndex < compilationThreadCount; ++threadIndex) {
			compilationThreads.push_back(std::jthread(&ShaderCache::ProcessCompilationSet, this));
		}
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

	uint64_t ShaderCache::GetCompletedTasks()
	{
		return compilationSet.completedTasks;
	}

	uint64_t ShaderCache::GetTotalTasks()
	{
		return compilationSet.totalTasks;
	}

	void ShaderCache::ProcessCompilationSet()
	{
		while (true) {
			const auto& task = compilationSet.WaitTake();
			task.Perform();
			compilationSet.Complete(task);
		}
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

	bool ShaderCompilationTask::operator==(const ShaderCompilationTask& other) const
	{
		return GetId() == other.GetId();
	}

	ShaderCompilationTask CompilationSet::WaitTake()
	{
		std::unique_lock lock(mutex);
		conditionVariable.wait(lock, [this]() { return !availableTasks.empty(); });

		auto node = availableTasks.extract(availableTasks.begin());
		auto task = node.value();
		tasksInProgress.insert(std::move(node));
		return task;
	}

	void CompilationSet::Add(const ShaderCompilationTask& task)
	{
		std::unique_lock lock(mutex);
		auto inProgressIt = tasksInProgress.find(task);
		if (inProgressIt == tasksInProgress.end()) {
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
		std::unique_lock lock(mutex);
		tasksInProgress.erase(task);
		completedTasks++;
	}

	void CompilationSet::Clear()
	{
		std::lock_guard lock(mutex);
		availableTasks.clear();
		tasksInProgress.clear();
		totalTasks = 0;
		completedTasks = 0;
	}
}