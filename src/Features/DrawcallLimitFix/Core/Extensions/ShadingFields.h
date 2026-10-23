#pragma once

// The shading field contract's Core side (Api/ShadingFields.h): the fields' entries and the published entry classes, resolved once at
// InstallFeatures; what a field's capture is handed; and the calls Core makes where a publication's deltas are taken
// (FrameView::HandOverAtFrameStart) and at the frame's start (FrameDriver). The tables keep the published classes' members
// (SceneTables::classMembers); Core zeroes a slot's fields when it leaves their class and FrameValues applies the values, the fields'
// only writer (a shading sample leaves them).

#include "Features/DrawcallLimitFix/Api/ShadingFields.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace RE
{
	class BSGeometry;
}

namespace DCLF::Scene
{
	class FrameView;
}

namespace DCLF::Api
{
	/** @brief A field's capture (render thread, the frame's start): the newest publication's geometry by object slot, and the values out. */
	class ShadingFieldCapture
	{
	public:
		ShadingRowField field = ShadingRowField::Count;
		std::span<RE::BSGeometry* const> objectGeometry;  // empty without a publication
		std::vector<ShadingFieldValue>& values;

		/** @brief a_slot's value of the field (applied after the frame's shading sample). */
		void Put(std::uint32_t a_slot, const std::array<float, kShadingFieldValueFloats>& a_value) const { values.push_back({ a_slot, field, a_value }); }
	};
}

namespace DCLF::Extensions
{
	/** @brief The entry classes the tables keep members of (SceneTables::classMembers), by class (Api::EntryClass). */
	inline constexpr std::size_t kPublishedEntryClasses = static_cast<std::size_t>(Api::EntryClass::Count);
	/** @brief The shading row's fields (Api::ShadingRowField). */
	inline constexpr std::size_t kShadingRowFields = static_cast<std::size_t>(Api::ShadingRowField::Count);

	/** @brief A field's entry: its registration (copied, its name owned). */
	struct ShadingFieldEntry
	{
		std::string name;
		Api::ShadingFieldContribution contribution;
	};

	/**
	 * @brief Main thread, the end of InstallFeatures: the fields' entries from their registrations (in registration order), and the published
	 * entry classes from the owner classes' registrations (Extensions::RegisteredOwnerClasses).
	 */
	void ResolveShadingFields(std::span<const Api::ShadingFieldContribution> a_registered);
	/** @brief Any thread, after InstallFeatures (none before): the fields, in registration order. */
	std::span<const ShadingFieldEntry> ShadingFields();
	/** @brief Any thread, after InstallFeatures (none before): the entry classes registered published, in class order. */
	std::span<const Api::EntryClass> PublishedEntryClasses();
	/**
	 * @brief Render thread, as FrameView takes a publication's deltas: a_class's membership writes (one publication's, in order) to every
	 * field whose members it is, in registration order; a slot that left the class (or every member, at a clear) noted for its zero.
	 */
	void ForwardClassMembers(Api::EntryClass a_class, std::span<const Api::ClassMemberChange> a_changes);
	/**
	 * @brief Render thread, the frame's start (FrameDriver, for FrameValues::Kick): every field's capture, in registration order, each
	 * after a zero for every slot that left its class since the last.
	 */
	std::vector<Api::ShadingFieldValue> CaptureShadingFields(Scene::FrameView& a_view);
}
