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
// type, a Core class's value: Api/ObjectContributors.h) are not listed here: the install fails a key two features claim.

// TreeWindScene.h first: volk must precede every Vulkan header of the translation unit (the record layout).
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWindScene.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"
#include "Features/DrawcallLimitFix/Features/Decals/Decals.h"
#include "Features/DrawcallLimitFix/Features/Effects/EffectConstants.h"
#include "Features/DrawcallLimitFix/Features/FirstPerson/FirstPerson.h"
#include "Features/DrawcallLimitFix/Features/Lod/Lod.h"
#include "Features/DrawcallLimitFix/Features/OcclusionMaps/OcclusionMaps.h"
#include "Features/DrawcallLimitFix/Features/Reflection/Reflection.h"
#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLod.h"
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWind.h"
#include "Features/DrawcallLimitFix/Integrations/LightLimitFix/LightLimitFix.h"
#include "Features/DrawcallLimitFix/Integrations/TerrainBlending/TerrainBlending.h"

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

		// Every frame texture's register (Api::FrameTexture; its HLSL declares the same: Effects' soft depth, Effect.hlsl's
		// DCLFEffectSoftDepth; Integrations/TerrainBlending's mask, TerrainBlending.hlsli's TerrainBlendingMaskTexture): none Core's, a frame
		// binding's or another's.
		constexpr std::uint32_t kFrameTextureRegisters[] = {
			Effects::kSoftDepthRegister,
			Integrations::TerrainBlending::kMaskRegister,
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
	static_assert(FrameConstantRowsValid(), "a frame constant row is outside Core's feature rows of PS b13 or another row's");
	static_assert(ObjectFlagsValid(), "a feature's object flags are Core's (outside kFeatureObjectFlags) or another feature's");
	static_assert(RecordWordsValid(), "a family's record words are past the object record or another family's");
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
