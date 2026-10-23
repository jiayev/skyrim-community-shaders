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

		std::vector<IntegrityChange> integrityChanges;
		bool integrityChecked = false;

		// The families in a_caps, by SWF's names, joined.
		std::string CapNames(SWF_Caps a_caps)
		{
			static constexpr std::pair<SWF_Caps, const char*> kNames[] = { { SWF_CAP_HIDDEN, "hidden" }, { SWF_CAP_SWITCH_INDEX, "switch-index" },
				{ SWF_CAP_FADE_RESET, "fade-reset" }, { SWF_CAP_STRUCTURE, "structure" }, { SWF_CAP_SWITCH_STRUCTURAL, "switch-structural" },
				{ SWF_CAP_MOVES, "moves" }, { SWF_CAP_FADE_CALLS, "fade-calls" }, { SWF_CAP_NODE_CALLS, "node-calls" },
				{ SWF_CAP_PROPERTY_CALLS, "property-calls" }, { SWF_CAP_GEOMETRY_CALLS, "geometry-calls" }, { SWF_CAP_MATERIAL_CALLS, "material-calls" },
				{ SWF_CAP_ACTOR_CALLS, "actor-calls" }, { SWF_CAP_DECAL_CALLS, "decal-calls" }, { SWF_CAP_LOD_SEGMENT_CALLS, "lod-segment-calls" },
				{ SWF_CAP_TREE_LOD_CALLS, "tree-lod-calls" }, { SWF_CAP_TREE_ANIM_CALLS, "tree-anim-calls" }, { SWF_CAP_EFFECT_CALLS, "effect-calls" } };
			std::string names;
			for (const auto& [cap, name] : kNames)
				if (a_caps & cap)
					names += (names.empty() ? "" : ", ") + std::string(name);
			return names;
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
		const std::string names = CapNames(missing);
		SWF_ProviderInfo info{};
		info.size = sizeof(info);
		provider->GetProviderInfo(&info);
		return fmt::format("SceneWriterFramework ({} {}) could not install the hooks DCLF needs: {} (another plugin patched them first; see the [SWF] log lines)",
			info.hostName ? info.hostName : "?", swf::client::VersionString(info.implVersion), names);
	}

	bool Subscribe(std::uint32_t a_event, SWF_EventFn a_fn, std::uint32_t a_phases, std::int32_t a_priority)
	{
		const auto* provider = Api();
		SWF_Subscription subscription = 0;
		const SWF_Result result = provider ? provider->Subscribe(a_event, a_phases, a_priority, a_fn, nullptr, &subscription) : SWF_E_NOT_READY;
		if (result != SWF_OK)
			logger::warn("[DCLF] scene writer event {} not subscribed (SWF result {})", a_event, result);
		return result == SWF_OK;
	}

	void CheckIntegrity()
	{
		const auto* provider = Api();
		if (!provider || provider->abiMinor < 1)
			return;
		integrityChanges.clear();
		provider->CheckIntegrity(
			[](const SWF_IntegrityEntry* a_entry, void*) {
				integrityChanges.push_back({ a_entry->family ? a_entry->family : "?", a_entry->module ? a_entry->module : "", a_entry->kind, a_entry->offset });
			},
			nullptr);
		integrityChecked = true;
	}

	const std::vector<IntegrityChange>& IntegrityChanges() { return integrityChanges; }
	bool IntegrityChecked() { return integrityChecked; }

	std::string ProviderLine()
	{
		const auto* provider = Api();
		if (!provider)
			return "none (no copy of SceneWriterFramework elected; see the [SWF] log lines)";
		SWF_ProviderInfo info{};
		info.size = sizeof(info);
		provider->GetProviderInfo(&info);
		return fmt::format("{} {} (ABI {}.{}, {} candidate(s))", info.hostName ? info.hostName : "?", swf::client::VersionString(info.implVersion), info.abiMajor,
			info.abiMinor, info.candidateCount);
	}

	std::string CapsLine()
	{
		const std::string names = CapNames(Caps());
		return names.empty() ? "none" : names;
	}

	bool AddFilter(std::uint32_t a_filter, SWF_FilterFn a_fn, std::int32_t a_priority)
	{
		const auto* provider = Api();
		SWF_Subscription filter = 0;
		const SWF_Result result = provider ? provider->AddFilter(a_filter, a_priority, a_fn, nullptr, &filter) : SWF_E_NOT_READY;
		if (result != SWF_OK)
			logger::warn("[DCLF] scene writer filter {} not added (SWF result {})", a_filter, result);
		return result == SWF_OK;
	}
}
