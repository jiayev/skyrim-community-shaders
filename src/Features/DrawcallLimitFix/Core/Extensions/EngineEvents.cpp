// The engine side's calls to the registered stand-in observers (see the header).

#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Engine/Capture/SceneCapture.h"

namespace DCLF::Extensions
{
	namespace
	{
		// Written once on the main thread at PostPostLoad (PrimaryCull::Install, before InstallFeatures reads it).
		bool standInInstalled = false;
		// Written once on the main thread at PostPostLoad (the hidden-node source's install, before InstallFeatures reads it).
		std::uint32_t hiddenSourceSites = 0;
		// By need slot: its channel's index (~0u: unbound). Written only by BindEngineChannelNeeds (the end of InstallFeatures, before
		// DCLF's threads exist), read-only after.
		std::array<std::uint32_t, kEngineChannelNeedSlots> boundChannelNeeds = [] {
			std::array<std::uint32_t, kEngineChannelNeedSlots> bound;
			bound.fill(~0u);
			return bound;
		}();
	}

	static_assert(Api::kNodeFieldFadeAmount == SceneCapture::NodeRecord::kFadeAmount, "the Api's node fields are the capture's");

	void BindEngineChannelNeeds(std::span<const std::string> a_needs)
	{
		const auto channels = RegisteredEngineChannels();
		for (std::size_t n = 0; n < a_needs.size() && n < kEngineChannelNeedSlots; ++n)
			for (std::uint32_t c = 0; c < channels.size(); ++c)
				if (channels[c].name == a_needs[n])
					boundChannelNeeds[n] = c;
	}

	void PushChannelKey(Api::EngineChannelId a_channel, const void* a_key)
	{
		std::uint32_t index = a_channel.index;
		if (index != ~0u && (index & kEngineChannelNeedBit)) {
			const std::uint32_t need = index & ~kEngineChannelNeedBit;
			index = need < kEngineChannelNeedSlots ? boundChannelNeeds[need] : ~0u;
		}
		const auto channels = RegisteredEngineChannels();
		if (index < channels.size() && channels[index].push)
			channels[index].push(a_key);
	}

	void NoteStandInInstalled() { standInInstalled = true; }
	bool StandInInstalled() { return standInInstalled; }
	void NoteHiddenSourceSites(std::uint32_t a_sites) { hiddenSourceSites = a_sites; }
	std::uint32_t HiddenSourceSites() { return hiddenSourceSites; }

	AdoptionGenerations SampleAdoptionGuards()
	{
		AdoptionGenerations generations{};
		const auto guards = RegisteredAdoptionGuards();
		for (std::size_t g = 0; g < guards.size(); ++g)
			generations[g] = guards[g].generation();
		return generations;
	}

	void StandInRootsChanged(const Api::StandInRootsChange& a_change)
	{
		for (const auto& observer : RegisteredStandInObservers())
			if (observer.changed)
				observer.changed(a_change);
	}

	void StandInRootsCleared()
	{
		for (const auto& observer : RegisteredStandInObservers())
			if (observer.cleared)
				observer.cleared();
	}

	void StandInUpdateEntry(bool a_running)
	{
		for (const auto& observer : RegisteredStandInObservers())
			if (observer.updateEntry)
				observer.updateEntry(a_running);
	}

	void StandInAfterWrites(bool a_running)
	{
		for (const auto& observer : RegisteredStandInObservers())
			if (observer.afterWrites)
				observer.afterWrites(a_running);
	}

	void StandInStructure(const void* a_parent)
	{
		for (const auto& observer : RegisteredStandInObservers())
			if (observer.structure)
				observer.structure(a_parent);
	}

	void StandInListJobsDone()
	{
		for (const auto& observer : RegisteredStandInObservers())
			if (observer.listJobsDone)
				observer.listJobsDone();
	}

	bool StandInVerdict(const void* a_root) { return IndirectDraws::Get().AccumulatedVerdict(a_root); }
	bool ListedVerdictOf(const void* a_root, bool& a_visible) { return IndirectDraws::Get().AccumulatedVerdictOf(a_root, a_visible); }
}
