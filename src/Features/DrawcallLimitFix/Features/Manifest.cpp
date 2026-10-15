// The features' shader manifest: the compile-time constants their shaders share with Core's, checked here not to overlap Core's or each
// other's - the frame bindings' texture registers, the object flag bits their technique contributions set, and the record words their
// table families' references are carried in. A feature with a frame binding (Api::FrameBinding) lists its register; the install checks
// the registrations the same way (InstallContext::RegisterFrameBinding). One with a technique contribution (Api::TechniqueContribution)
// lists its flags, and one whose family the record carries (Api::TableFamily::recordWord) its word.

// TreeWindScene.h first: volk must precede every Vulkan header of the translation unit (the record layout).
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWindScene.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"
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
	static_assert(ObjectFlagsValid(), "a feature's object flags are Core's (outside kFeatureObjectFlags) or another feature's");
	static_assert(RecordWordsValid(), "a family's record words are past the object record or another family's");
	// TreeWind's: DCLFObjectRecord's DCLFWindTimers.zw (the record's TreeParams and WindTimers, xy the record's own).
	static_assert(offsetof(BindlessObject, tree) + offsetof(ObjectTreeAnim, windTimers) + 2 * sizeof(float) == TreeWind::kTreeRecordWord * 4,
		"TreeWind's record word is not DCLFWindTimers.z");
}
