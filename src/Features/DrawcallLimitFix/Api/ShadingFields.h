#pragma once

// The shading field contract (Api::InstallContext::RegisterShadingField): a field of the shading rows (BindlessShading) whose value is an
// owner's, not the object's property's - captured once per owner at the frame's start (the render thread's, where the CS feature keeps its
// state) and fanned out to the owner's members, the slots of a published entry class (EntryClassContribution::published). Every other slot's
// field is zero. FrameValues (Core/Gpu) samples the rest of the row and applies the captured values after it.

#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace DCLF::Api
{
	// Defined by Core (Core/Extensions/ShadingFields.h), which the integrations inside DCLF include for them.
	class ShadingFieldCapture;

	/** @brief A field of the shading row an owner's value fills (its bytes are Core's: BindlessShading's layout). */
	enum class ShadingRowField : std::uint8_t
	{
		SkinPerGeometry,  // Advanced Skin's SkinPerGeometry (PS b7): float4
		Count,
	};

	/** @brief A registered shading field (RegisterShadingField's result). */
	struct ShadingFieldId
	{
		std::uint32_t index = ~0u;
		explicit operator bool() const { return index != ~0u; }
	};

	/**
	 * @brief One membership write of a published entry class's slot, in the coordinator's order: the slot's owner group (0: not a member)
	 * and the entry's identity (incarnations included), or the class's members cleared (clear: the slot ignored). resetValue: a member
	 * whose value is handed out again even when its owner's did not change.
	 */
	struct ClassMemberChange
	{
		std::uint32_t slot = 0;
		std::uint64_t group = 0, identity = 0;
		bool resetValue = false;
		bool clear = false;
	};

	/** @brief What a field's membership is handed: one publication's writes of its members' class, in order. */
	struct ShadingFieldMembership
	{
		std::span<const ClassMemberChange> changes;
	};

	/** @brief The floats a captured value holds (a field's bytes are at most these). */
	inline constexpr std::size_t kShadingFieldValueFloats = 4;

	/** @brief One member slot's value of a field, as the frame's start captured it. */
	struct ShadingFieldValue
	{
		std::uint32_t slot = ~0u;
		ShadingRowField field = ShadingRowField::Count;
		std::array<float, kShadingFieldValueFloats> value{};
	};

	/**
	 * @brief A shading field contribution: the field, and the entry class whose members carry it (published; its registration may be
	 * another feature's).
	 *
	 * - name: what the registration's failures call it.
	 * - field: the row's field (one contribution's).
	 * - members: the entry class (not None) whose members are the field's slots (Core zeroes a slot's value when it leaves the class).
	 * - membership: render thread, at the frame's start as it takes each publication's deltas (FrameView::HandOverAtFrameStart), in the
	 *   publications' order: the class's membership writes, for the feature's own index of owners (none: a publication without writes).
	 * - capture: render thread, at the frame's start (FrameDriver, before FrameValues::Kick), in registration order: the members' values
	 *   (ShadingFieldCapture::Put), what changed since the last capture and every member that joined. A value put is the slot's until the
	 *   next put, or until the slot leaves the class (Core puts zero then, before the capture's values, in membership order).
	 */
	struct ShadingFieldContribution
	{
		std::string_view name;
		ShadingRowField field = ShadingRowField::Count;
		EntryClass members = EntryClass::None;
		void (*membership)(const ShadingFieldMembership& a_membership) = nullptr;
		void (*capture)(ShadingFieldCapture& a_capture) = nullptr;
	};
}
