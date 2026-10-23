#pragma once

// The engine event contracts' Core side (Api/EngineEvents.h): what a stand-in observer is handed, the calls the engine side makes, and
// where an engine channel's drain puts its keys.

#include "Features/DrawcallLimitFix/Api/EngineEvents.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace RE
{
	class BSGeometry;
	class NiAVObject;
}

namespace DCLF::Api
{
	/** @brief The stood-in roots' change (PrimaryCull::SyncRootOwnership): the roots that left and joined. */
	class StandInRootsChange
	{
	public:
		const std::vector<const RE::NiAVObject*>& left;
		const std::vector<const RE::NiAVObject*>& joined;
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
	inline constexpr std::uint32_t kEngineChannelSlots = 6;
	/**
	 * @brief The engine channels needed by name (InstallContext::NeedEngineChannel): a need's id is this bit and its slot, which
	 * PushChannelKey resolves through the needs bound at the end of InstallFeatures (BindEngineChannelNeeds). Grow the slots before more
	 * names are needed.
	 */
	inline constexpr std::uint32_t kEngineChannelNeedBit = 1u << 31;
	inline constexpr std::uint32_t kEngineChannelNeedSlots = 2;
	/** @brief Main thread, the end of InstallFeatures: each needed name bound to its channel (none: its pushes are dropped). */
	void BindEngineChannelNeeds(std::span<const std::string> a_needs);
	/**
	 * @brief Any thread, lock-free, after InstallFeatures, in a feature's hook after the store it reports: a_key pushed into a_channel
	 * (a registered id, or a need's: InstallContext::NeedEngineChannel) through the channel's push (Api::EngineChannel::push). Dropped
	 * before the needs are bound, for an unbound need, or a channel without a push.
	 */
	void PushChannelKey(Api::EngineChannelId a_channel, const void* a_key);

	/**
	 * @brief Main thread, at PostPostLoad, before InstallFeatures (the hidden-node source's install, in the early hooks): its stores' sites;
	 * the sites InstallContext::ReserveHiddenSite hands out follow them.
	 */
	void NoteHiddenSourceSites(std::uint32_t a_sites);
	std::uint32_t HiddenSourceSites();

	// The features' engine hooks' pushes (Core/Engine/Capture/SceneEvents.cpp), any thread, after InstallFeatures.
	/**
	 * @brief A SceneStack channel's event (Api::Stacked) onto the scene's event stack, after what this thread pushed before it: a_pinned
	 * pinned by the event until its batch is released (render thread), a_named keys alone (nulls dropped from both); stamped and the pass
	 * woken when the channel's wake is Pass. Nothing for token 0 or once the tracker stopped.
	 */
	void PushStacked(Api::EngineChannelId a_channel, std::uint64_t a_token, std::span<RE::NiAVObject* const> a_pinned, std::span<RE::NiAVObject* const> a_named);
	/** @brief a_channel's wake (Api::EngineChannel::wake): a scene pass for Pass (SceneWake::Channel), nothing for None. */
	void WakeChannel(Api::EngineChannelId a_channel);
	/**
	 * @brief A store of the feature's own to a_object's kHidden bit, as a hidden event on a_site (InstallContext::ReserveHiddenSite), on
	 * the storing thread after the store. Nothing while the hidden-node source is not installed.
	 */
	void PushReservedHidden(const RE::NiAVObject* a_object, std::uint32_t a_site);
	/**
	 * @brief Any thread, after InstallFeatures: whether the hidden-node source is installed, so the hidden events flow (a feature that
	 * follows them, the LOD gates, engages only then).
	 */
	bool HiddenEventsLive();
	/**
	 * @brief A store of the feature's own to what a_geometry's ranged type captures (Api::GeometryTypeContribution::ranges: a
	 * BSSubIndexTriShape's segments): the mirror's update of its drawn ranges, on the storing thread after the store, in order with what
	 * that thread pushed before. Nothing for null.
	 */
	void PushRangesUpdate(const RE::BSGeometry* a_geometry);
	/** @brief The move writers the report counts (Core's and the reserved ones; Scene::moveWriterCalls): grow it before more are reserved. */
	inline constexpr std::uint32_t kMoveWriterSlots = 9;
	/**
	 * @brief Main thread at PostPostLoad, before DCLF's threads start (a feature's early hooks or install), before the writer's hooks go in:
	 * a move writer of the feature's own named a_name (copied; the report's move writers' calls name it). Core's writers keep their slots
	 * (animation 0, reference 3D position 1, graph animation 5, update pass 6) and the reserved ones take the free slots in reservation
	 * order, so the report's order is the writers' as it was (Features/Actors' three at its early hooks: 2-4; Features/Particles' at its
	 * install, through InstallContext::ReserveMoveWriter: 7; Features/TreeWind's tree animation walk at its install, SE 1.5.97 only: 8).
	 * Scene::MoveEventsLive is false until the writer is installed (MoveWriterInstalled). Fails for no name or past kMoveWriterSlots.
	 */
	Api::MoveWriterId ReserveMoveWriter(std::string_view a_name);
	/** @brief Main thread at PostPostLoad: a_writer's hooks are in. */
	void MoveWriterInstalled(Api::MoveWriterId a_writer);
	/**
	 * @brief Main thread at PostPostLoad, after Core's scene events: whether Core's own move writers are in (the cells' update passes where
	 * expected). A feature whose writers complete Core's movers installs them only then (all or none, as when Core installed them).
	 */
	bool CoreMoveWritersLive();
	/**
	 * @brief Any thread, in a reserved writer's hook after the write: a_key moved (MoveEvents; FrameValueMoves too for WithPlacements), and
	 * a call counted for a_writer. A null key is counted, not pushed.
	 */
	void PushReservedMove(Api::MoveWriterId a_writer, const void* a_key, Api::MoveReach a_reach);

	/** @brief Main thread (PrimaryCull::Install): the primary cull installed, so the stand-in observers will be called. */
	void NoteStandInInstalled();
	/** @brief Main thread, at install: whether NoteStandInInstalled came (InstallContext::RegisterStandInObserver's answer). */
	bool StandInInstalled();
	// The primary cull's calls, each to every stand-in observer in order (Api::StandInObserver).
	void StandInRootsChanged(const Api::StandInRootsChange& a_change);
	void StandInRootsCleared();
	void StandInUpdateEntry(bool a_running);
	void StandInAfterWrites(bool a_running);
	/** @brief Any thread (PrimaryCull::NoteListStructure, first, before the main cull input's): each observer's structure. No lock. */
	void StandInStructure(const void* a_parent);
	/**
	 * @brief Render thread, at the list jobs' Finish (PrimaryCull's ListJobsFinish, where the animated batch is drained today): each
	 * observer's listJobsDone.
	 */
	void StandInListJobsDone();
	// What a stand-in observer reads of DCLF's main-camera verdicts (the roots' kAccumulated as written at Main::Update's entry, before
	// updateEntry): direct calls, a feature's per-root loop may make them. Main thread, after Main::Update's entry; no engine memory read.
	/** @brief A stood-in root's verdict as last written; visible when not written yet (IndirectDraws::AccumulatedVerdict). */
	bool StandInVerdict(const void* a_root);
	/**
	 * @brief The verdict of any root DCLF lists (a fade root or plain entry root, from the wide writes) into a_visible; false when it has
	 * none (IndirectDraws::AccumulatedVerdictOf).
	 */
	bool ListedVerdictOf(const void* a_root, bool& a_visible);
}
