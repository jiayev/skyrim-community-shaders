// The features' shader manifest: the compile-time constants their shaders share with Core's, checked here not to overlap Core's or each
// other's - the frame bindings' and frame textures' texture registers, the frame constant rows, the object flag bits their technique
// contributions set, the record words their table families' references are carried in, and the views' set phases and view bits. A feature
// with a frame binding (Api::FrameBinding) or a frame texture (Api::FrameTexture) lists its register, one with a frame constant row
// (Api::FrameConstantRow) its row; the install checks the registrations the same way (InstallContext::RegisterFrameBinding,
// RegisterFrameTexture, RegisterFrameConstantRow). One with a technique
// contribution (Api::TechniqueContribution) lists its flags, one whose family the record carries (Api::TableFamily::recordWord) its word,
// and one with a view contribution (Api::ViewContribution, Api::OcclusionMapContribution) its phase and view bit (the install checks those
// too: Extensions::ResolveViewBits). One with a main range (Api::MainRangeContribution) checks its slot against the GPU layout its objects'
// flags select (BuildDrawsCS's group bits and count words: GpuLayouts.h's MainRangeCountWord).
// The contributions' keys (a technique, a geometry type, a Core class's value: Api/ObjectContributors.h) are not listed here: the install
// fails a key two features claim.

// TreeWindScene.h first: volk must precede every Vulkan header of the translation unit (the record layout).
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWindScene.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"
#include "Features/DrawcallLimitFix/Features/Decals/Decals.h"
#include "Features/DrawcallLimitFix/Features/FirstPerson/FirstPerson.h"
#include "Features/DrawcallLimitFix/Features/OcclusionMaps/OcclusionMaps.h"
#include "Features/DrawcallLimitFix/Features/Reflection/Reflection.h"
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWind.h"

#include <cstddef>
#include <cstdint>
#include <iterator>

namespace DCLF::Features
{
	namespace
	{
		// Every feature's frame-binding register (its HLSL declares the same: TreeWind's DCLFTreeWind, Common/DCLFObjects.hlsli).
		constexpr std::uint32_t kFrameBindingRegisters[] = {
			TreeWind::kWindRegister,
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

		// Every frame texture's register (Api::FrameTexture; its HLSL declares the same: the soft effects' depth, Effect.hlsl's
		// DCLFEffectSoftDepth, Core's built-in until Features/Effects registers it): none Core's, a frame binding's or another's.
		constexpr std::uint32_t kFrameTextureRegisters[] = {
			kEffectSoftDepthRegister,
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

		// Every frame constant row (Api::FrameConstantRow; its HLSL declares the same: the Effect port's camera planes, light colour and
		// blood, Effect.hlsl's DCLFEffectCamera, DCLFEffectDLightColor and DCLFEffectBlood, Core's built-ins until Features/Effects
		// registers them): within Core's feature rows of PS b13, none another's.
		constexpr std::uint32_t kFrameConstantRows[] = {
			kEffectCameraFrameRow,
			kEffectLightFrameRow,
			kEffectBloodFrameRow,
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

		// Every feature's object flags (its technique contributions' objectFlags): within the bits Core's enum leaves the features
		// (Records.h, kFeatureObjectFlags), none another's.
		constexpr std::uint32_t kObjectFlags[] = {
			TreeWind::kObjectTreeAnim,
		};

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
	}

	static_assert(FrameBindingRegistersValid(), "a feature's frame-binding register is Core's, another feature's, or past t127");
	static_assert(std::size(kFrameBindingRegisters) <= Extensions::kFrameBindingSlots, "more frame bindings than ResourceAddresses embeds: grow kFrameBindingSlots");
	static_assert(FrameTextureRegistersValid(), "a frame texture's register is below t16, past t127, Core's, a frame binding's or another frame texture's");
	static_assert(std::size(kFrameTextureRegisters) <= Extensions::kFrameTextureSlots, "more frame textures than a colour commit samples: grow kFrameTextureSlots");
	static_assert(FrameConstantRowsValid(), "a frame constant row is outside Core's feature rows of PS b13 or another row's");
	static_assert(ObjectFlagsValid(), "a feature's object flags are Core's (outside kFeatureObjectFlags) or another feature's");
	static_assert(RecordWordsValid(), "a family's record words are past the object record or another family's");
	static_assert(ViewBitsValid(kViewPhases, std::size(kViewPhases), kCoreSetPhases, 0xFFu), "a view's set phase is not one bit of the eight, or is Core's or another view's");
	static_assert(ViewBitsValid(kViewBits, std::size(kViewBits), Draws::kCoreViewBits, 0xFFFFFFFFu), "a view's view bit is not one bit, or is Core's or another view's");
	// FirstPerson's view bit: BuildDrawsCS.hlsl's kViewFirstPerson (the first-person forward range's cull). Reflection's and the
	// occlusion maps' have no HLSL mirror (the faces' and the maps' culls read their latch's view bits as data).
	static_assert(FirstPerson::kViewFirstPerson == 1u << 11, "FirstPerson's view bit is not BuildDrawsCS's kViewFirstPerson");
	// The reflection's faces: Core's per-view capacity (ViewEpoch.h).
	static_assert(Reflection::kFaces <= Draws::kMaxViewFaces, "the reflection's faces are past Core's kMaxViewFaces");
	// TreeWind's: DCLFObjectRecord's DCLFWindTimers.zw (the record's TreeParams and WindTimers, xy the record's own).
	static_assert(offsetof(BindlessObject, tree) + offsetof(ObjectTreeAnim, windTimers) + 2 * sizeof(float) == TreeWind::kTreeRecordWord * 4,
		"TreeWind's record word is not DCLFWindTimers.z");
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
}
