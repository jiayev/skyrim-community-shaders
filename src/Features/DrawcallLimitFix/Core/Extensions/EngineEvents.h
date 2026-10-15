#pragma once

// The engine event contracts' Core side (Api/EngineEvents.h): what a stand-in observer is handed, the calls the engine side makes, and
// where an engine channel's drain puts its keys.

#include "Features/DrawcallLimitFix/Api/EngineEvents.h"

#include <array>
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
	/**
	 * @brief The adoption guards the frame inputs and a publication carry the generations of (Scene::FrameInputs::adoptionGuards,
	 * ScenePublication::adoptionGuards), by registration order: grow it before more are registered.
	 */
	inline constexpr std::uint32_t kAdoptionGuardSlots = 2;
	using AdoptionGenerations = std::array<std::uint64_t, kAdoptionGuardSlots>;
	/** @brief Any thread, after InstallFeatures: every registered guard's generation (Api::AdoptionGuard::generation), 0 past them. */
	AdoptionGenerations SampleAdoptionGuards();
	/** @brief Whether any of a_installed's guards is older than a_frame's (the frame withdraws its claims). */
	inline bool AdoptionGuarded(const AdoptionGenerations& a_installed, const AdoptionGenerations& a_frame)
	{
		for (std::uint32_t g = 0; g < kAdoptionGuardSlots; ++g)
			if (a_installed[g] < a_frame[g])
				return true;
		return false;
	}

	/**
	 * @brief Main thread, at PostPostLoad, before InstallFeatures (the hidden-node source's install, in the early hooks): its stores' sites;
	 * the sites InstallContext::ReserveHiddenSite hands out follow them.
	 */
	void NoteHiddenSourceSites(std::uint32_t a_sites);
	std::uint32_t HiddenSourceSites();

	/** @brief Main thread (PrimaryCull::Install): the primary cull installed, so the stand-in observers will be called. */
	void NoteStandInInstalled();
	/** @brief Main thread, at install: whether NoteStandInInstalled came (InstallContext::RegisterStandInObserver's answer). */
	bool StandInInstalled();
	// The primary cull's calls, each to every stand-in observer in order (Api::StandInObserver).
	void StandInRootsChanged(const Api::StandInRootsChange& a_change);
	void StandInRootsCleared();
	void StandInUpdateEntry(bool a_running);
}
