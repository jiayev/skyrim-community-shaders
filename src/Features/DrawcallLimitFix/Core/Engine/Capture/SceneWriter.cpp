#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneWriter.h"

#include <swf/swf_client.h>
#include <swf/swf_embed.h>

namespace DCLF::SceneWriter
{
	namespace
	{
		const SWF_Api* api = nullptr;

		void LogSink(std::int32_t a_level, const char* a_message)
		{
			switch (a_level) {
			case SWF_LOG_DEBUG:
				logger::debug("[SWF] {}", a_message);
				break;
			case SWF_LOG_WARN:
				logger::warn("[SWF] {}", a_message);
				break;
			case SWF_LOG_ERROR:
				logger::error("[SWF] {}", a_message);
				break;
			default:
				logger::info("[SWF] {}", a_message);
				break;
			}
		}
	}

	void Elect()
	{
		SWF_Embed_SetLogSink(&LogSink);
		api = SWF_Embed_OnPostLoad();
	}

	const SWF_Api* Api()
	{
		// Another host's election may have run without CS's (CS's ran only if DCLF will install).
		if (!api)
			api = swf::client::Connect();
		return api;
	}

	SWF_Caps Caps()
	{
		const auto* provider = Api();
		return provider ? provider->GetCaps() : 0;
	}

	std::string MissingReason()
	{
		const auto* provider = Api();
		if (!provider)
			return "no SceneWriterFramework provider (see the [SWF] log lines)";
		const SWF_Caps missing = kRequiredCaps & ~provider->GetCaps();
		if (!missing)
			return {};
		SWF_ProviderInfo info{};
		info.size = sizeof(info);
		provider->GetProviderInfo(&info);
		return fmt::format("SceneWriterFramework ({} {}) could not install the{}{} stores DCLF needs (another plugin patched them first; see the [SWF] log lines)",
			info.hostName ? info.hostName : "?", swf::client::VersionString(info.implVersion), (missing & SWF_CAP_SWITCH_INDEX) ? " switch-index" : "",
			(missing & SWF_CAP_FADE_RESET) ? " fade-reset" : "");
	}

	bool Subscribe(std::uint32_t a_event, SWF_EventFn a_fn, std::int32_t a_priority)
	{
		const auto* provider = Api();
		SWF_Subscription subscription = 0;
		const SWF_Result result = provider ? provider->Subscribe(a_event, SWF_PHASE_POST, a_priority, a_fn, nullptr, &subscription) : SWF_E_NOT_READY;
		if (result != SWF_OK)
			logger::warn("[DCLF] scene writer event {} not subscribed (SWF result {})", a_event, result);
		return result == SWF_OK;
	}
}
