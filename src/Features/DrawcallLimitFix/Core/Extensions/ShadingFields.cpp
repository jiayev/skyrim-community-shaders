// The shading fields' entries, the published entry classes, and the frame start's calls into the fields (see the header).

#include "Features/DrawcallLimitFix/Core/Extensions/ShadingFields.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"

#include <array>
#include <cstdint>
#include <vector>

namespace DCLF::Extensions
{
	namespace
	{
		// Written only by ResolveShadingFields (InstallFeatures, before DCLF's threads exist), read-only after.
		struct Resolved
		{
			std::vector<ShadingFieldEntry> fields;
			std::vector<Api::EntryClass> published;
		};

		Resolved& State()
		{
			static Resolved resolved;
			return resolved;
		}

		// The render thread's, by published class: which slots are members as the membership writes left them (a byte by slot), and
		// the slots that left since the last capture, whose fields it zeroes before the fields' own values (CaptureShadingFields).
		struct Members
		{
			std::vector<std::uint8_t> member;
			std::vector<std::uint32_t> left;
		};
		std::array<Members, kPublishedEntryClasses> members;
	}

	void ResolveShadingFields(std::span<const Api::ShadingFieldContribution> a_registered)
	{
		auto& resolved = State();
		resolved.fields.reserve(a_registered.size());
		for (const auto& field : a_registered) {
			auto& entry = resolved.fields.emplace_back();
			entry.name = std::string(field.name);
			entry.contribution = field;
			entry.contribution.name = entry.name;
		}
		std::array<bool, kPublishedEntryClasses> published{};
		for (const auto& owner : RegisteredOwnerClasses())
			if (owner.published && owner.ownerClass != Api::EntryClass::None && owner.ownerClass < Api::EntryClass::Count)
				published[static_cast<std::size_t>(owner.ownerClass)] = true;
		for (std::size_t c = 0; c < published.size(); ++c)
			if (published[c])
				resolved.published.push_back(static_cast<Api::EntryClass>(c));
	}

	std::span<const ShadingFieldEntry> ShadingFields() { return State().fields; }

	std::span<const Api::EntryClass> PublishedEntryClasses() { return State().published; }

	void ForwardClassMembers(Api::EntryClass a_class, std::span<const Api::ClassMemberChange> a_changes)
	{
		auto& mine = members[static_cast<std::size_t>(a_class)];
		for (const auto& change : a_changes) {
			if (change.clear) {
				for (std::uint32_t slot = 0; slot < mine.member.size(); ++slot)
					if (mine.member[slot])
						mine.left.push_back(slot);
				mine.member.clear();
				continue;
			}
			if (mine.member.size() <= change.slot)
				mine.member.resize(std::size_t(change.slot) + 1, 0);
			const bool now = change.group != 0;
			if (mine.member[change.slot] && !now)
				mine.left.push_back(change.slot);
			mine.member[change.slot] = now;
		}
		const Api::ShadingFieldMembership membership{ a_changes };
		for (const auto& entry : State().fields)
			if (entry.contribution.members == a_class)
				entry.contribution.membership(membership);
	}

	std::vector<Api::ShadingFieldValue> CaptureShadingFields(Scene::FrameView& a_view)
	{
		std::vector<Api::ShadingFieldValue> values;
		// Their geometries the newest publication's (held by it), never the coordinator's tables.
		const auto geometry = a_view.NewestObjectGeometry();
		for (const auto& entry : State().fields) {
			Api::ShadingFieldCapture capture{ entry.contribution.field, geometry, values };
			// A slot that left the field's class first: its value zero (a member's own value follows: a slot that left and joined again
			// takes its new owner's, which its capture puts as a joined member's).
			for (const auto slot : members[static_cast<std::size_t>(entry.contribution.members)].left)
				capture.Put(slot, {});
			entry.contribution.capture(capture);
		}
		for (auto& mine : members)
			mine.left.clear();
		return values;
	}
}
