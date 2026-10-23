// The hidden events' source: SceneWriterFramework's kHidden stores (SWF_EVENT_HIDDEN: every engine store that can change
// NiAVObject::flags' bit 0, patched by the elected SWF provider), each pushing the node's mirror update on the storing thread when the
// bit changed (Core's channel: Scene::PushHiddenUpdate). The LOD show stores take the show veto (Api::ShowVeto, through Core's cells)
// as SWF's show filter (SWF_FILTER_SHOW). Subscribed on the render thread, at the feature's early hooks.

#include "Features/DrawcallLimitFix/Features/HiddenNodes/HiddenNodes.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneEvents.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneWriter.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"

namespace DCLF::Features::HiddenNodes
{
	namespace
	{
		// The show veto's cells (Extensions::RegisteredShowVeto: Core's, at a fixed address for the session), set at InstallStores before
		// any event arrives.
		const Api::ShowVeto* showVeto = nullptr;
		// The hidden site of the plugins' writes through SWF (SWF_WRITE_HIDDEN): the first after SWF's stores.
		std::uint32_t writeSite = 0;

		void OnHidden(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			const auto& event = *reinterpret_cast<const SWF_HiddenEvent*>(a_event);
			// The show veto's engine hide observer (the LOD gates' coverage observer: the engine's own LOD hides, object update and terrain
			// block nodes, after a flip).
			if (event.hidden && showVeto->engineHid && std::ranges::find(showVeto->hideSites, event.siteOffset) != showVeto->hideSites.end())
				showVeto->engineHid(event.object, event.siteOffset);
			Scene::PushHiddenUpdate(event.object, event.site);
		}

		// A plugin's hidden write through SWF's Write (CS's features': DrawcallLimitFix::WriteHidden), after its store.
		void OnSceneWrite(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			const auto& write = *reinterpret_cast<const SWF_SceneWriteEvent*>(a_event);
			if (write.kind == SWF_WRITE_HIDDEN)
				Scene::PushHiddenUpdate(write.object, writeSite);
		}

		// SWF's show stubs call the filter while one is registered; the veto's open word keeps a closed gate to one load. A vetoed store
		// is left unrun, without an event, so the object stays hidden until the flip.
		std::int32_t VetoShow(const SWF_EventHeader* a_event, void*)
		{
			const auto& event = *reinterpret_cast<const SWF_ShowFilterEvent*>(a_event);
			return showVeto->open->load(std::memory_order_relaxed) != 0 && showVeto->vetoes(event.object) ? 1 : 0;
		}

		bool Subscribe()
		{
			const auto* api = SceneWriter::Api();
			if (!api || !(api->GetCaps() & SWF_CAP_HIDDEN)) {
				logger::warn("[DCLF] hidden events not installed: SceneWriterFramework has no hidden stores (see the [SWF] log lines)");
				return false;
			}
			SWF_Subscription filter = 0;
			if (const auto result = api->AddFilter(SWF_FILTER_SHOW, 0, &VetoShow, nullptr, &filter); result != SWF_OK) {
				logger::warn("[DCLF] hidden events not installed: the show filter was refused (SWF result {})", result);
				return false;
			}
			if (!SceneWriter::Subscribe(SWF_EVENT_HIDDEN, &OnHidden)) {
				api->RemoveFilter(filter);
				return false;
			}
			if (api->abiMinor >= 2 && !SceneWriter::Subscribe(SWF_EVENT_SCENE_WRITE, &OnSceneWrite))
				stl::report_and_fail("Drawcall Limit Fix: the scene writer framework refused its scene write events at ABI minor 2");
			logger::info("[DCLF] hidden events: {} SceneWriterFramework stores", api->SiteCount(SWF_EVENT_HIDDEN));
			return true;
		}
	}

	void InstallStores()
	{
		// Core's channel: the reserved sites (a feature's own stores, InstallContext::ReserveHiddenSite) follow SWF's; the walk gates its
		// verdicts on the events once installed. The show veto's cells before the first event (the filter loads them).
		showVeto = &Extensions::RegisteredShowVeto();
		const auto* api = SceneWriter::Api();
		// SWF's stores, then the plugins' writes through SWF as one more site.
		writeSite = api ? api->SiteCount(SWF_EVENT_HIDDEN) : 0;
		Extensions::NoteHiddenSourceSites(writeSite + 1);
		Scene::hiddenEventsInstalled = Subscribe();
	}

	std::uintptr_t StoreSiteOf(std::uintptr_t a_address)
	{
		const auto* api = SceneWriter::Api();
		std::uint32_t event = 0, offset = 0;
		if (!api || api->ResolveSite(a_address, &event, nullptr, &offset) != SWF_OK || event != SWF_EVENT_HIDDEN)
			return 0;
		return 0x140000000 + offset;
	}
}
