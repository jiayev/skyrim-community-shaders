// The first-person skeleton's claims (CS_DCLF_SET_PARITY): at the view's walk of the skeleton's geometry (every 16 frames), its slots and
// claimed phases, logged when they change. Render thread.

#include "Features/DrawcallLimitFix/Features/FirstPerson/FirstPerson.h"

#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"

#include <string>

namespace DCLF::Features::FirstPerson
{
	namespace
	{
		// The skeleton's slots and claimed phases as last logged.
		std::string loggedSkeleton;

		void NoteFirstPersonSkeleton(const Diagnostics::FirstPersonSkeletonWalked& a_walked)
		{
			// Which first-person geometry the first-person view does not take, and what it takes instead.
			const auto& tables = a_walked.tables;
			const auto& skeleton = a_walked.skeleton;
			const auto& claims = a_walked.claims;
			std::string members;
			for (std::size_t o = 0; o < tables.objects.size() && !skeleton.empty(); ++o)
				if (o < tables.objectGeometry.size() && tables.objectGeometry[o] && skeleton.contains(tables.objectGeometry[o]) && !(tables.objects[o].flags & kObjectFree))
					members += fmt::format("{}'{}' {} slot {} flags {:#x} phases {:#x}", members.empty() ? "" : "; ", tables.objectGeometry[o]->name.c_str() ? tables.objectGeometry[o]->name.c_str() : "?", static_cast<const void*>(tables.objectGeometry[o]),
						o, tables.objects[o].flags, claims.PhasesOf(tables.objectGeometry[o]));
			auto& logged = loggedSkeleton;
			if (members != logged) {
				logged = members;
				logger::info("[DCLF] first-person skeleton's slots (T7e, CS_DCLF_SET_PARITY): {} geometries walked; {}", a_walked.walked, members);
			}
		}
	}

	void SubscribeSkeletonParity()
	{
		Diagnostics::Subscribe<Diagnostics::FirstPersonSkeletonWalked>(&NoteFirstPersonSkeleton);
	}
}
