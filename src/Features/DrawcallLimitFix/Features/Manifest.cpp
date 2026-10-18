// The features' (and integrations') shader manifest: the compile-time constants their shaders share with Core's, checked here not to
// overlap Core's or each other's - the frame bindings', frame textures' and graph buffers' texture registers, the frame constant rows, the
// object flag bits their technique contributions set, the record words their table families' references and ancestor values are carried
// in, and the views' set phases and view bits. A feature with a frame binding (Api::FrameBinding), a frame texture (Api::FrameTexture) or
// graph buffers (Api::FrameGraphBuffers) lists its registers, one with a frame constant row (Api::FrameConstantRow) its row; the install
// checks the registrations the same way (InstallContext::RegisterFrameBinding, RegisterFrameTexture, RegisterFrameGraphBuffers,
// RegisterFrameConstantRow). One with a technique contribution (Api::TechniqueContribution) lists its flags, one whose family the record
// carries (Api::TableFamily::recordWord) or with an ancestor value (Api::AncestorValueContribution::recordWord) its word, and one with a
// view contribution (Api::ViewContribution,
// Api::OcclusionMapContribution) its phase and view bit (the install checks those too: Extensions::ResolveViewBits). One with a main range
// (Api::MainRangeContribution, or a held group's: Api::HeldGroupContribution) checks its slot against the GPU layout its objects' flags
// select (BuildDrawsCS's group bits and count words: GpuLayouts.h's MainRangeCountWord). One whose view root contribution
// (Api::ViewRootContribution) or draw family's faces (Api::DrawFamilyContribution::faces.root) owns an engine root lists it: one owner a
// root (the install checks it too: InstallContext::RegisterViewRoot, RegisterDrawFamily). The contributions' keys (a technique, a geometry
// type, a Core class's value: Api/ObjectContributors.h) are not listed here: the install fails a key two features claim. A material
// contributor (Api::MaterialContributor) lists its feature texture registers, checked against the frame registers and against the
// shaders' decode of MaterialRecord's feature slots; a Lighting object flag (Api::LightingObjectFlag) its object flag and record bit; a
// per-draw buffer (Api::PerDrawBuffer) its register (the install checks those too: InstallContext::RegisterMaterialContributor,
// RegisterLightingObjectFlag, DeclarePerDrawBuffer).

// ViewEpoch.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ViewEpoch.h"
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWindScene.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Materials.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ViewCommon.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/SceneSet.h"
#include "Features/DrawcallLimitFix/Core/Shading/ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"
#include "Features/DrawcallLimitFix/Features/Decals/Decals.h"
#include "Features/DrawcallLimitFix/Features/Effects/EffectConstants.h"
#include "Features/DrawcallLimitFix/Features/FirstPerson/FirstPerson.h"
#include "Features/DrawcallLimitFix/Features/Lod/Lod.h"
#include "Features/DrawcallLimitFix/Features/OcclusionMaps/OcclusionMaps.h"
#include "Features/DrawcallLimitFix/Features/Reflection/Reflection.h"
#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLod.h"
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWind.h"
#include "Features/DrawcallLimitFix/Integrations/AdvancedSkin/AdvancedSkin.h"
#include "Features/DrawcallLimitFix/Integrations/CloudShadows/CloudShadows.h"
#include "Features/DrawcallLimitFix/Integrations/DynamicCubemaps/DynamicCubemaps.h"
#include "Features/DrawcallLimitFix/Integrations/ExponentialHeightFog/ExponentialHeightFog.h"
#include "Features/DrawcallLimitFix/Integrations/IBL/IBL.h"
#include "Features/DrawcallLimitFix/Integrations/LightLimitFix/LightLimitFix.h"
#include "Features/DrawcallLimitFix/Integrations/LinearLighting/LinearLighting.h"
#include "Features/DrawcallLimitFix/Integrations/Skylighting/Skylighting.h"
#include "Features/DrawcallLimitFix/Integrations/SubsurfaceScattering/SubsurfaceScattering.h"
#include "Features/DrawcallLimitFix/Integrations/TerrainBlending/TerrainBlending.h"
#include "Features/DrawcallLimitFix/Integrations/TerrainShadows/TerrainShadows.h"
#include "Features/DrawcallLimitFix/Integrations/TruePBR/TruePBRIntegration.h"
#include "Features/DrawcallLimitFix/Integrations/VolumetricShadows/VolumetricShadows.h"
#include "Features/DrawcallLimitFix/Integrations/WaterEffects/WaterEffects.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>

namespace DCLF::Features
{
	namespace
	{
		// Every feature's frame-binding register (its HLSL declares the same: TreeWind's DCLFTreeWind, Common/DCLFObjects.hlsli), and Core's
		// fade roots' (their states and roots, Features/Fades/FadeRender.cpp).
		constexpr std::uint32_t kFrameBindingRegisters[] = {
			TreeWind::kWindRegister,
			kFadeStatesRegister,
			kFadeRootsRegister,
		};

		constexpr bool FrameBindingRegistersValid()
		{
			for (std::size_t i = 0; i < std::size(kFrameBindingRegisters); ++i) {
				const std::uint32_t r = kFrameBindingRegisters[i];
				if (r >= kTextureRegisters || Extensions::CoreFrameRegister(r))
					return false;
				for (std::size_t j = 0; j < i; ++j)
					if (kFrameBindingRegisters[j] == r)
						return false;
			}
			return true;
		}

		// Every frame texture's register (Api::FrameTexture; its HLSL declares the same: Effects' soft depth, Effect.hlsl's
		// DCLFEffectSoftDepth; Integrations/TerrainBlending's mask, TerrainBlending.hlsli's TerrainBlendingMaskTexture; the CS features'
		// views, their own HLSL's: Volumetric Shadows' SharedShadowMap, Cloud Shadows' CloudShadowsTexture, Exponential Height Fog's
		// ExponentialHeightFogIntegratedLightScattering, Dynamic Cubemaps' EnvReflectionsTexture and EnvTexture, Skylighting's SkylightingProbeArray and ShadowVisibilityProbeArray, Terrain Shadows' ShadowHeightTexture, Water Effects'
		// WaterCaustics, IBL's EnvIBLTexture, SkyIBLTexture, StaticDiffuseIBLTexture and StaticSpecularIBLTexture): none Core's, a frame
		// binding's or another's.
		constexpr std::uint32_t kFrameTextureRegisters[] = {
			Effects::kSoftDepthRegister,
			Integrations::TerrainBlending::kMaskRegister,
			Integrations::VolumetricShadows::kSharedShadowMapRegister,
			Integrations::CloudShadows::kCloudOcclusionRegister,
			Integrations::ExponentialHeightFog::kLightScatteringRegister,
			Integrations::DynamicCubemaps::kReflectionsRegister,
			Integrations::DynamicCubemaps::kEnvironmentRegister,
			Integrations::Skylighting::kProbeArrayRegister,
			Integrations::Skylighting::kShadowVisibilityRegister,
			Integrations::TerrainShadows::kShadowHeightRegister,
			Integrations::WaterEffects::kCausticsRegister,
			Integrations::IBL::kEnvironmentRegister,
			Integrations::IBL::kSkyRegister,
			Integrations::IBL::kStaticDiffuseRegister,
			Integrations::IBL::kStaticSpecularRegister,
		};

		constexpr bool FrameTextureRegistersValid()
		{
			for (std::size_t i = 0; i < std::size(kFrameTextureRegisters); ++i) {
				const std::uint32_t r = kFrameTextureRegisters[i];
				if (r < kPixelTextureSlots || r >= kTextureRegisters || Extensions::CoreFrameRegister(r))
					return false;
				for (const std::uint32_t binding : kFrameBindingRegisters)
					if (binding == r)
						return false;
				for (std::size_t j = 0; j < i; ++j)
					if (kFrameTextureRegisters[j] == r)
						return false;
			}
			return true;
		}

		// Every graph buffers' registers (Api::FrameGraphBuffers: firstRegister and the next, one a resource; its HLSL declares the same:
		// Integrations/LightLimitFix's lights, light index list and light grid, LightLimitFix.hlsli's t35-t37): none Core's, a frame binding's,
		// a frame texture's or another's.
		struct GraphBufferRegisters
		{
			std::uint32_t first = 0;
			std::uint32_t count = 0;
		};
		constexpr GraphBufferRegisters kGraphBufferRegisters[] = {
			{ Integrations::LightLimitFix::kLightsRegister, Integrations::LightLimitFix::kGraphBuffers },
		};

		constexpr bool GraphBufferRegistersValid()
		{
			for (std::size_t i = 0; i < std::size(kGraphBufferRegisters); ++i) {
				const auto& range = kGraphBufferRegisters[i];
				if (!range.count || range.count > 4)
					return false;
				for (std::uint32_t r = range.first; r < range.first + range.count; ++r) {
					if (r < kPixelTextureSlots || r >= kTextureRegisters || Extensions::CoreFrameRegister(r))
						return false;
					for (const std::uint32_t binding : kFrameBindingRegisters)
						if (binding == r)
							return false;
					for (const std::uint32_t texture : kFrameTextureRegisters)
						if (texture == r)
							return false;
					for (std::size_t j = 0; j < i; ++j)
						if (r >= kGraphBufferRegisters[j].first && r < kGraphBufferRegisters[j].first + kGraphBufferRegisters[j].count)
							return false;
				}
			}
			return true;
		}

		// Every material contributor's feature texture registers (Api::MaterialContributor::textureRegisters, each list ascending; its HLSL
		// declares the same: Integrations/AdvancedSkin's t71 and t74, Integrations/TruePBR's t80-t91, Lighting.hlsl): none Core's, a frame
		// binding's, a frame texture's, a graph buffer's or another contributor's range, and no more than MaterialRecord's feature slots.
		// Core's frame registers no longer cover them (Extensions::CoreFrameRegister): they are the contributors'.
		constexpr std::span<const std::uint32_t> kFeatureTextureRegisters[] = {
			Integrations::AdvancedSkin::kMaterialTextureRegisters,
			Integrations::TruePBR::kTextureRegisters,
		};

		constexpr bool FrameRegisterListed(std::uint32_t a_register)
		{
			for (const std::uint32_t binding : kFrameBindingRegisters)
				if (binding == a_register)
					return true;
			for (const std::uint32_t texture : kFrameTextureRegisters)
				if (texture == a_register)
					return true;
			for (const auto& range : kGraphBufferRegisters)
				if (a_register >= range.first && a_register < range.first + range.count)
					return true;
			return false;
		}

		constexpr bool FeatureTextureRegistersValid()
		{
			std::size_t count = 0;
			for (std::size_t i = 0; i < std::size(kFeatureTextureRegisters); ++i) {
				const auto registers = kFeatureTextureRegisters[i];
				if (registers.empty())
					return false;
				for (std::size_t k = 0; k < registers.size(); ++k) {
					const std::uint32_t r = registers[k];
					if (r < kPixelTextureSlots || r >= kTextureRegisters || Extensions::CoreFrameRegister(r) || FrameRegisterListed(r) ||
						(k && registers[k - 1] >= r))
						return false;
				}
				for (std::size_t j = 0; j < i; ++j) {
					const auto other = kFeatureTextureRegisters[j];
					if (!other.empty() && registers.front() <= other.back() && other.front() <= registers.back())
						return false;
				}
				count += registers.size();
			}
			return count <= Extensions::kFeatureTextureSlots;
		}

		// DCLFMaterialTextureIndex (DCLFObjects.hlsli): the row header's feature slot a register reads, t71 the first, t74 the second, t80
		// and up the third on; none for another register.
		constexpr std::uint32_t DecodedFeatureSlot(std::uint32_t a_register)
		{
			return a_register == 71 ? 0u : a_register == 74 ? 1u : a_register >= 80 ? 2u + (a_register - 80) : ~0u;
		}

		// Core assigns every contributor's registers, ascending, to the feature slots from 0 (Extensions::ResolveMaterialContributors): each
		// register's slot must be the one the shaders decode.
		constexpr bool FeatureTextureSlotsDecoded()
		{
			std::array<std::uint32_t, Extensions::kFeatureTextureSlots> sorted{};
			std::size_t count = 0;
			for (const auto registers : kFeatureTextureRegisters) {
				for (const std::uint32_t r : registers) {
					if (count == sorted.size())
						return false;
					std::size_t at = count++;
					for (; at && sorted[at - 1] > r; --at)
						sorted[at] = sorted[at - 1];
					sorted[at] = r;
				}
			}
			for (std::size_t slot = 0; slot < count; ++slot)
				if (DecodedFeatureSlot(sorted[slot]) != slot)
					return false;
			return true;
		}

		// Every frame constant row (Api::FrameConstantRow; its HLSL declares the same: Effects' camera planes, light colour and blood,
		// Effect.hlsl's DCLFEffectCamera, DCLFEffectDLightColor and DCLFEffectBlood): within Core's feature rows of PS b13, none another's.
		constexpr std::uint32_t kFrameConstantRows[] = {
			Effects::kEffectCameraFrameRow,
			Effects::kEffectLightFrameRow,
			Effects::kEffectBloodFrameRow,
		};

		constexpr bool FrameConstantRowsValid()
		{
			for (std::size_t i = 0; i < std::size(kFrameConstantRows); ++i) {
				const std::uint32_t row = kFrameConstantRows[i];
				if (row < Extensions::kFrameConstantRowFirst || row >= Extensions::kFrameConstantRowsEnd)
					return false;
				for (std::size_t j = 0; j < i; ++j)
					if (kFrameConstantRows[j] == row)
						return false;
			}
			return true;
		}

		// Every feature's object flags (its technique contributions' objectFlags, a root states contribution's objectFlags): within the bits
		// Core's enum leaves the features (Records.h, kFeatureObjectFlags), none another's. The root test's two (the fade roots': the
		// fade-out distance and the tree height test, Api::RootStatesContribution::objectFlags) are named by Core's enum until
		// Features/Fades declares them. The Lighting object flags' (Api::LightingObjectFlag::objectFlag): Integrations/SubsurfaceScattering's.
		constexpr std::uint32_t kObjectFlags[] = {
			Integrations::SubsurfaceScattering::kObjectBeastRace,
			TreeWind::kObjectTreeAnim,
			kObjectFadeTest,
			kObjectHeightTest,
		};

		// Every Lighting object flag's record bit (Api::LightingObjectFlag::recordFlag: BindlessObject::recordFlags, DCLFObjects.hlsli's
		// DCLFRecordFlags): one bit, none Core's (LightingConstants.h: the alpha-blended pair's, refraction's and the fade root slot's) or
		// another's.
		constexpr std::uint32_t kRecordFlags[] = {
			Integrations::SubsurfaceScattering::kRecordBeastRace,
		};
		constexpr std::uint32_t kCoreRecordFlags = kRecordAlphaBlended | kRecordRefraction | kRecordRefractionFalloff | kRecordRefractionClamp |
		                                           kRecordRefractionNoNormals | kRecordRefractionVertexColors | kRecordRefractionTwoSided | kRecordDecalNudge |
		                                           (~0u << kRecordFadeRootShift);

		// Every per-draw buffer's register (Api::PerDrawBuffer, all of the pixel stage; its HLSL declares the same: Integrations/LightLimitFix's
		// StrictLightData b3, Integrations/AdvancedSkin's SkinPerGeometry b7, Integrations/LinearLighting's LLPerGeometry b8, Color.hlsli):
		// below b14, none Core's (Extensions::kCorePerDrawPS) or another's.
		constexpr std::uint32_t kPerDrawPSRegisters[] = {
			3,  // Integrations/LightLimitFix's StrictLightData (LightLimitFixIntegration.cpp's kStrictLightDataRegister)
			Integrations::AdvancedSkin::kSkinPerGeometryRegister,
			Integrations::LinearLighting::kLLPerGeometryRegister,
		};

		constexpr bool PerDrawRegistersValid(const std::uint32_t* a_registers, std::size_t a_count, std::uint32_t a_core)
		{
			std::uint32_t taken = a_core;
			for (std::size_t i = 0; i < a_count; ++i) {
				const std::uint32_t r = a_registers[i];
				if (r >= kConstantBufferRegisters || (taken & (1u << r)))
					return false;
				taken |= 1u << r;
			}
			return true;
		}

		constexpr bool ObjectFlagsValid()
		{
			std::uint32_t taken = 0;
			for (const std::uint32_t flags : kObjectFlags) {
				if (!flags || (flags & ~kFeatureObjectFlags) || (flags & taken))
					return false;
				taken |= flags;
			}
			return true;
		}

		// Every record word a family's reference is carried in, its row's and the next, its tag's (Api::TableFamily::recordWord): within
		// the record, none another's.
		constexpr std::uint32_t kRecordWords[] = {
			TreeWind::kTreeRecordWord,
		};
		// Every ancestor value's record word (Api::AncestorValueContribution::recordWord, one word): within the record, none a family's or
		// another's.
		constexpr std::uint32_t kAncestorRecordWords[] = {
			Integrations::LightLimitFix::kRoomRecordWord,
		};

		// Every view's set phase (SceneSet.h's SetPhase bits: one bit of the eight, none Core's: kCoreSetPhases) and view bit at the GPU cull
		// (GpuLayouts.h's kView*: one bit, none Core's: kCoreViewBits), none another view's, of either family (the occlusion maps too).
		constexpr std::uint32_t kViewPhases[] = {
			OcclusionMaps::kSetOccluderSky,
			OcclusionMaps::kSetOccluderPrecipitation,
			Reflection::kSetReflection,
			FirstPerson::kSetFirstPerson,
		};
		constexpr std::uint32_t kViewBits[] = {
			OcclusionMaps::kViewSkyOccluder,
			OcclusionMaps::kViewPrecipOccluder,
			Reflection::kViewReflection,
			FirstPerson::kViewFirstPerson,
		};

		constexpr bool ViewBitsValid(const std::uint32_t* a_bits, std::size_t a_count, std::uint32_t a_core, std::uint32_t a_space)
		{
			std::uint32_t taken = a_core;
			for (std::size_t i = 0; i < a_count; ++i) {
				const std::uint32_t bit = a_bits[i];
				if (!bit || (bit & (bit - 1)) || (bit & ~a_space) || (bit & taken))
					return false;
				taken |= bit;
			}
			return true;
		}

		// Every engine root a view root contribution or a draw family's faces own (Api::ViewRoot): a root, none another's.
		constexpr Api::ViewRoot kViewRoots[] = {
			Lod::kOwnedRoot,
			TreeLod::kFacesRoot,
		};

		constexpr bool ViewRootsValid()
		{
			std::uint32_t taken = 0;
			for (const Api::ViewRoot root : kViewRoots) {
				if (root >= Api::ViewRoot::Count || (Api::ViewRootBit(root) & taken))
					return false;
				taken |= Api::ViewRootBit(root);
			}
			return true;
		}

		constexpr bool RecordWordsValid()
		{
			for (std::size_t i = 0; i < std::size(kRecordWords); ++i) {
				if (std::size_t(kRecordWords[i]) + 2 > sizeof(BindlessObject) / 4)
					return false;
				for (std::size_t j = 0; j < i; ++j)
					if (kRecordWords[j] + 1 >= kRecordWords[i] && kRecordWords[i] + 1 >= kRecordWords[j])
						return false;
			}
			return true;
		}

		constexpr bool AncestorRecordWordsValid()
		{
			for (std::size_t i = 0; i < std::size(kAncestorRecordWords); ++i) {
				const std::uint32_t word = kAncestorRecordWords[i];
				if (std::size_t(word) + 1 > sizeof(BindlessObject) / 4)
					return false;
				for (const std::uint32_t family : kRecordWords)
					if (word == family || word == family + 1)
						return false;
				for (std::size_t j = 0; j < i; ++j)
					if (kAncestorRecordWords[j] == word)
						return false;
			}
			return true;
		}
	}

	static_assert(FrameBindingRegistersValid(), "a feature's frame-binding register is Core's, another feature's, or past t127");
	static_assert(std::size(kFrameBindingRegisters) <= Extensions::kFrameBindingSlots, "more frame bindings than ResourceAddresses embeds: grow kFrameBindingSlots");
	static_assert(FrameTextureRegistersValid(), "a frame texture's register is below t16, past t127, Core's, a frame binding's or another frame texture's");
	static_assert(std::size(kFrameTextureRegisters) <= Extensions::kFrameTextureSlots, "more frame textures than a colour commit samples: grow kFrameTextureSlots");
	static_assert(GraphBufferRegistersValid(), "a graph buffer's register is below t16, past t127, Core's, a frame binding's, a frame texture's or another graph buffer's");
	static_assert(FeatureTextureRegistersValid(),
		"a material contributor's feature texture registers are not ascending, below t16, past t127, Core's, a frame binding's, a frame "
		"texture's, a graph buffer's or another contributor's, or more than MaterialRecord's feature slots");
	static_assert(FeatureTextureSlotsDecoded(), "the feature texture registers, ascending, are not DCLFMaterialTextureIndex's slots (t71, t74, t80 on)");
	static_assert(FrameConstantRowsValid(),"a frame constant row is outside Core's feature rows of PS b13 or another row's");
	static_assert(ObjectFlagsValid(), "a feature's object flags are Core's (outside kFeatureObjectFlags) or another feature's");
	// Every bit kFeatureObjectFlags leaves the features is one a feature declares (none left over for Core to take back unnoticed).
	static_assert([] {
		std::uint32_t all = 0;
		for (const std::uint32_t flags : kObjectFlags)
			all |= flags;
		return all == kFeatureObjectFlags;
	}(), "kFeatureObjectFlags holds a bit no feature declares");
	// The root test's bits: DCLFObjects.hlsli's and BuildDrawsCS's fade test and height test (27, 28), which the shaders mirror.
	static_assert(kObjectFadeTest == 1u << 27 && kObjectHeightTest == 1u << 28, "the root test's object flags are not the shaders' bits 27 and 28");
	static_assert(ViewBitsValid(kRecordFlags, std::size(kRecordFlags), kCoreRecordFlags, 0xFFFFFFFFu),
		"a Lighting object flag's record bit is not one bit, or is Core's or another's");
	// Integrations/SubsurfaceScattering's: DCLFRecordFlags bit 0, which Lighting.hlsl reads as IsBeastRace (DCLFObjects.hlsli).
	static_assert(Integrations::SubsurfaceScattering::kRecordBeastRace == 1u << 0, "Subsurface Scattering's record bit is not Lighting.hlsl's DCLFRecordFlags bit 0");
	static_assert(PerDrawRegistersValid(kPerDrawPSRegisters, std::size(kPerDrawPSRegisters), Extensions::kCorePerDrawPS),
		"a per-draw buffer's register is past b13, Core's or another per-draw buffer's");
	// The per-draw buffers' HLSL: SkinPerGeometry (Advanced Skin's, b7) and LLPerGeometry (Color.hlsli, b8).
	static_assert(Integrations::AdvancedSkin::kSkinPerGeometryRegister == 7 && Integrations::LinearLighting::kLLPerGeometryRegister == 8,
		"a per-draw buffer's register is not its HLSL's");
	static_assert(RecordWordsValid(),"a family's record words are past the object record or another family's");
	static_assert(AncestorRecordWordsValid(), "an ancestor value's record word is past the object record, a family's or another ancestor value's");
	// Integrations/LightLimitFix's room index: DCLFObjectRecord's RoomIndex (DCLFObjects.hlsli), the record's first word.
	static_assert(offsetof(BindlessObject, ancestorValue) == Integrations::LightLimitFix::kRoomRecordWord * 4, "Light Limit Fix's record word is not DCLFObjectRecord's RoomIndex");
	static_assert(ViewRootsValid(), "a view root is not one, or another registration owns it");
	static_assert(ViewBitsValid(kViewPhases, std::size(kViewPhases), kCoreSetPhases, 0xFFu), "a view's set phase is not one bit of the eight, or is Core's or another view's");
	static_assert(ViewBitsValid(kViewBits, std::size(kViewBits), Draws::kCoreViewBits, 0xFFFFFFFFu), "a view's view bit is not one bit, or is Core's or another view's");
	// FirstPerson's view bit: BuildDrawsCS.hlsl's kViewFirstPerson (the first-person forward range's cull). Reflection's and the
	// occlusion maps' have no HLSL mirror (the faces' and the maps' culls read their latch's view bits as data).
	static_assert(FirstPerson::kViewFirstPerson == 1u << 11, "FirstPerson's view bit is not BuildDrawsCS's kViewFirstPerson");
	// The reflection's faces: Core's per-view capacity (ViewEpoch.h).
	static_assert(Reflection::kFaces <= Draws::kMaxViewFaces, "the reflection's faces are past Core's kMaxViewFaces");
	// The feature block: BindlessObject and BindlessShading are the C++ mirrors of the frozen DCLFObjects.hlsli (DCLFObjectRecord words 4-11,
	// DCLFShadingRow words 16-23), so neither size nor the block's place moves.
	static_assert(sizeof(BindlessObject) == 64 && sizeof(BindlessShading) == 96, "an object row is not DCLFObjects.hlsli's size");
	static_assert(sizeof(BindlessObject::featureBlock) == Api::kShadingBlockWords * 4 && sizeof(BindlessShading::featureBlock) == Api::kShadingBlockWords * 4,
		"the feature block is not Api::kShadingBlockWords");
	static_assert(offsetof(BindlessObject, featureBlock) == TreeWind::kRecordBlockWord * 4, "TreeWind's record block word is not DCLFTreeParams");
	static_assert(offsetof(BindlessShading, featureBlock) == TreeWind::kShadingBlockWord * 4, "TreeWind's shading block word is not DCLFShadingTreeParams");
	static_assert(TreeWind::kBlockWords <= Api::kShadingBlockWords && TreeWind::kTreeParamsVariable == kVSTreeParams && TreeWind::kWindTimersVariable == kVSWindTimers,
		"TreeWind's block is past the feature block, or its variables are not the Lighting VS TreeParams and WindTimers");
	// TreeWind's: DCLFObjectRecord's DCLFWindTimers.zw (the record's TreeParams and WindTimers, xy the record's own), inside the record block.
	static_assert(TreeWind::kTreeRecordWord >= TreeWind::kRecordBlockWord && TreeWind::kTreeRecordWord + 2 <= TreeWind::kRecordBlockWord + Api::kShadingBlockWords,
		"TreeWind's record word is outside the record's feature block");
	static_assert(TreeWind::kTreeRecordWord == TreeWind::kRecordBlockWord + TreeWind::kBlockWindTimers + 2, "TreeWind's record word is not DCLFWindTimers.z");
	// The main range slots (Api::MainRangeSlot) are BuildDrawsCS's ranges: the fixed ones the decal groups' (count words 19, 20 and 27),
	// the appended ones the terrain groups' (28, 29).
	static_assert(Draws::kMainRangeSlotCount == static_cast<std::uint32_t>(Api::MainRangeSlot::Count) && Draws::kDecalGroups == Api::kFixedMainRanges,
		"Api::MainRangeSlot is not GpuLayouts.h's range slots");
	static_assert(Draws::MainRangeCountWord(static_cast<std::uint32_t>(Api::MainRangeSlot::Fixed0)) == Draws::kCountDecalGroupWord &&
	                  Draws::MainRangeCountWord(static_cast<std::uint32_t>(Api::MainRangeSlot::Fixed1)) == Draws::kCountDecalGroupWord + 1 &&
	                  Draws::MainRangeCountWord(static_cast<std::uint32_t>(Api::MainRangeSlot::Fixed2)) == Draws::kCountDecalLayerWord &&
	                  Draws::MainRangeCountWord(static_cast<std::uint32_t>(Api::MainRangeSlot::Appended0)) == Draws::kCountTerrainGroupWord &&
	                  Draws::MainRangeCountWord(static_cast<std::uint32_t>(Api::MainRangeSlot::Appended1)) == Draws::kCountTerrainGroupWord + 1,
		"a main range slot does not read BuildDrawsCS's count word for it");
	// Decals' slots: group bits g (Records.h, kObjectDecalGroupShift) draw in slot Fixed(g - 1); opaque 1, blended 2, layers 3.
	static_assert(static_cast<std::uint32_t>(Decals::kOpaqueRange) == 1 - 1 && static_cast<std::uint32_t>(Decals::kBlendedRange) == 2 - 1 &&
	                  static_cast<std::uint32_t>(Decals::kLayerRange) == 3 - 1,
		"Decals' range slots are not its groups' (ObjectDecalGroup - 1)");
	// Integrations/TerrainBlending's held groups' slots: terrain group bits g (Records.h) draw in slot Appended(g - 1); terrain 1, no-blend 2.
	static_assert(static_cast<std::uint32_t>(Integrations::TerrainBlending::kTerrainRange) == static_cast<std::uint32_t>(Api::MainRangeSlot::Appended0) + 1 - 1 &&
	                  static_cast<std::uint32_t>(Integrations::TerrainBlending::kNoBlendRange) == static_cast<std::uint32_t>(Api::MainRangeSlot::Appended0) + 2 - 1,
		"Terrain Blending's range slots are not its terrain groups' (Appended0 + group - 1)");
}
