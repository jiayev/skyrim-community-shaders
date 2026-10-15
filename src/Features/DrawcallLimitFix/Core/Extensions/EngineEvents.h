#pragma once

// The engine event contracts' Core side (Api/EngineEvents.h): what a stand-in observer is handed, the calls the engine side makes, and
// where an engine channel's drain puts its keys.

#include "Features/DrawcallLimitFix/Api/EngineEvents.h"

#include <cstdint>
#include <vector>

namespace RE
{
	class NiAVObject;
}

namespace DCLF::Api
{
	/** @brief The stood-in roots' change (PrimaryCull::SyncFadeOwnership): the roots that left and joined, and the BSTreeNodes among them. */
	class StandInRootsChange
	{
	public:
		const std::vector<const RE::NiAVObject*>& left;
		const std::vector<const RE::NiAVObject*>& joined;
		const std::vector<const RE::NiAVObject*>& treesLeft;
		const std::vector<const RE::NiAVObject*>& treesJoined;
	};

	/**
	 * @brief Where an engine channel's drain (Api::EngineChannel::drain) puts its keys: the pass's batch, a load screen's named keys, or
	 * nowhere (dropped). Called per key by the queue's drain (EventQueue::Drain(a_sink)), inline in the feature's drain.
	 */
	class KeySink
	{
	public:
		explicit KeySink(std::vector<const void*>* a_out) :
			out(a_out) {}

		void operator()(const void* a_key) const
		{
			if (out)
				out->push_back(a_key);
		}

	private:
		std::vector<const void*>* out;
	};
}

namespace DCLF::Extensions
{
	/** @brief The engine channels a batch holds keys of (Coordinator::EventBatch::channelKeys): grow it before more are registered. */
	inline constexpr std::uint32_t kEngineChannelSlots = 4;

	/** @brief Main thread (PrimaryCull::Install): the primary cull installed, so the stand-in observers will be called. */
	void NoteStandInInstalled();
	/** @brief Main thread, at install: whether NoteStandInInstalled came (InstallContext::RegisterStandInObserver's answer). */
	bool StandInInstalled();
	// The primary cull's calls, each to every stand-in observer in order (Api::StandInObserver).
	void StandInRootsChanged(const Api::StandInRootsChange& a_change);
	void StandInRootsCleared();
	void StandInUpdateEntry(bool a_running);
}
