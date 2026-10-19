// Portals: the walk's thunk (early hooks), the main cull input and its programs buffer (PortalPrograms.cpp), RoomVisibility's detours
// and live flag, and the parity (Diagnostics/).

#include "Features/DrawcallLimitFix/Features/Portals/Diagnostics/PortalParity.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalPrograms.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalViews.h"
#include "Features/DrawcallLimitFix/Features/Portals/Portals.h"
#include "Features/DrawcallLimitFix/Features/Portals/RoomVisibility.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CullInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Features::Portals
{
	namespace
	{
		// AE 1.6.1170 (module offsets).
		constexpr std::uintptr_t kPortalWalkCallSite = 0x64685f;  // Main::Update -> FUN_1401a43c0, the main camera's portal walk
		constexpr std::uintptr_t kPortalWalk = 0x1a43c0;

		// The main cull input's programWords (parity only): the walk's programs as encoded now, which a fade log window saves.
		std::span<const std::uint32_t> ProgramWords()
		{
			return PortalViews::Get().Encoded();
		}

		/** @brief Main::Update's portal walk (FUN_1401a43c0): DCLF's walk follows, same camera, before the list build. */
		struct PortalWalk
		{
			static void thunk(void* a_tes, void* a_process)
			{
				func(a_tes, a_process);
				// Only with the primary cull installed (written once at PostPostLoad), as when its install wrote this thunk.
				if (Extensions::StandInInstalled())
					PortalViews::Get().Build();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief The walk's thunk, before the primary cull's install, which requires it (Extensions::MainWalkInstalled): its call site
		 * checked, else nothing is written and the primary cull stays off with its own warning.
		 */
		void EarlyHooks()
		{
			const auto base = REL::Module::get().base();
			if (!Engine::CallsTo(base + kPortalWalkCallSite, base + kPortalWalk))
				return;
			stl::write_thunk_call<PortalWalk>(base + kPortalWalkCallSite);
			Extensions::NoteMainWalkInstalled();
		}

		bool UnboundVisible()
		{
			return PortalViews::Get().VisibleUnbound();
		}

		void Structure(const void* a_parent)
		{
			PortalViews::Get().NoteStructure(static_cast<const RE::NiNode*>(a_parent));
		}

		std::uint64_t ProgramsVersion()
		{
			return PortalViews::Get().ProgramsVersion();
		}

		/** @brief Each asked root's program: its room's (ProgramOf), in Core's order. */
		void Judge(Api::RootProgramsJudge& a_roots)
		{
			const auto& portals = PortalViews::Get();
			for (std::size_t i = 0, count = a_roots.Count(); i < count; ++i)
				a_roots.Put(i, portals.ProgramOf(static_cast<const RE::NiAVObject*>(a_roots.Root(i))));
		}

		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterSwitch({ .name = "CS_DCLF_PORTAL_UNIFORM_SCAN", .kind = Api::SwitchKind::Diagnostic, .description = "Evaluate portal batches with the diagnostic uniform instruction scan" });
			a_context.RegisterMainCullInput({ .name = "portals",
				.noProgram = kPortalNoProgram,
				.unboundVisible = &UnboundVisible,
				.structure = &Structure,
				.programsVersion = &ProgramsVersion,
				.judge = &Judge,
				.test = &PortalTest,
				.programWords = &ProgramWords });
			// The programs buffer: its scene resources, the depth commit's upload and the Programs cull binding (PortalPrograms.cpp).
			RegisterPrograms(a_context);
			// The frame's Main::IsRoomVisible calls answered by DCLF only while it runs (Main::Update's entry, where the shell set it).
			a_context.RegisterStandInObserver({ .updateEntry = &RoomVisibility::SetLive });
			// Main::IsRoomVisible answered from PortalViews' VisibleSet while DCLF runs (its install log right after the primary cull's,
			// the feature listed first).
			RoomVisibility::Install();
			// CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY: the portal views against the engine's entry.
			if (Diagnostics::Observe().claimsOrScene)
				Diagnostics::PortalParity::Install();
		}
	}

	Api::Feature Feature()
	{
		return { .name = "portals", .install = &Install, .earlyHooks = &EarlyHooks };
	}
}
