#pragma once

// The scene area's diagnostics state: what its observers keep between their hook points and the reports. Each struct names its owner
// thread (the coordinator's, or the render thread's); what crosses between them is handed over under a lock or by exchange.

#include "Census.h"
#include "SceneView.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace DCLF::Diagnostics
{
	/** @brief CS_DCLF_WALK_PARITY: the slot tables against a dense rebuild (SceneWalkParity.cpp), the coordinator's. */
	struct WalkParityState
	{
		std::uint32_t checks = 0;
		std::uint32_t objects = 0;
		std::uint32_t differ = 0;
		std::uint32_t missing = 0;
		std::uint32_t extra = 0;
		std::uint32_t staleVerdicts = 0;  // entries whose kept classification differs from the reference's
		std::uint32_t staleTraits = 0;    // entries the delta walk keeps that a fresh classification would evaluate every frame
		std::string first;
		std::string firstStale;
		std::string firstStaleTraits;
		std::map<std::string, std::uint32_t> byWhat;  // the differences by what differs
	};

	/** @brief CS_DCLF_CHANGE_LOG_PARITY: the change log against a diff of the tables (SceneChangeLogParity.cpp), the coordinator's. */
	struct ChangeLogParityState
	{
		std::vector<SceneTables::Columns> snapshot;
		LogCursor cursor;  // the log where the snapshot was taken; active while one is waiting to be checked
		std::uint64_t checks = 0, slots = 0, changed = 0, missing = 0, skipped = 0;
		std::string first;
	};


	/**
	 * @brief What the frame registered, for the scene pass after it (SceneRegistrations.cpp): the render thread at the capture's drain ->
	 * the next pass (latest wins).
	 */
	struct RegistrationDrain
	{
		// The main renderers' registrations of the frame.
		struct Registration
		{
			const RE::BSGeometry* geometry = nullptr;
			std::uint32_t hint = 0;
		};
		std::vector<Registration> registrations;
		bool observed = false;  // the frame's registrations were observed (CS_DCLF_PERSISTENT_PARITY), so registrations holds them
	};

	/** @brief CS_DCLF_PERSISTENT_PARITY: the registration parity's counts, reported with the scene membership line. */
	struct RegistrationParityState
	{
		std::uint64_t registeredUnbound = 0;      // main-pass registrations of eligible objects DCLF has not bound
		std::uint64_t lightingShaderDiffers = 0;  // a registered Lighting pass whose shader is not the engine instance DCLF uses (render thread)
		std::string registeredUnboundFirst;
		// The frames whose registrations were observed, and the main camera's registrations checked. 0 frames: not observed.
		std::uint64_t registrationFrames = 0, registrationsChecked = 0;
	};

	/** @brief CS_DCLF_PERSISTENT_PARITY, render thread at Prepass: the technique rows against the engine's values now. */
	struct TechniqueParityState
	{
		std::uint64_t checks = 0, techniquesChecked = 0, techniquesDiffer = 0;
		// A row against the live sample that differs because the sample moved since the row's write (the publication's lag: counted
		// apart, not as differ), and the first real difference.
		std::uint64_t techniquesLate = 0;
		std::string techniqueFirst;
	};

	/** @brief CS_DCLF_PERSISTENT_PARITY, render thread at Prepass: the extras rows completed against the engine's own routines. */
	struct ExtrasParityState
	{
		std::uint64_t objects = 0, differ = 0, staleStatic = 0;
		float maxDifference = 0.0f;
		std::string first;
	};

	/**
	 * @brief CS_DCLF_PERSISTENT_PARITY, render thread at Prepass: every drawn slot's shading as the engine has it now against the frame's
	 * rows (FrameValues): a difference is named for the next frame (an event the walk took), its event is queued (taken then, and named),
	 * or missed.
	 */
	struct ShadingParityState
	{
		std::uint64_t checks = 0, slots = 0, missing = 0, named = 0, late = 0;
		std::string first;
	};

	/** @brief CS_DCLF_RESIDENT_PARITY: the residents' passes built again and their records, the coordinator's (reported with the membership). */
	struct ResidentParityState
	{
		std::uint64_t parityChecks = 0, parityChecked = 0, parityPass = 0, parityRecord = 0;
		std::uint64_t parityPending = 0;  // residents whose fade root is fading: the feedback's next decode ends them
	};

	/** @brief CS_DCLF_PERSISTENT_PARITY: the candidate tables against their sets (reported with the scene's candidates line). */
	struct CandidateParityState
	{
		std::uint64_t checks = 0, mismatches = 0;
	};

	/** @brief CS_DCLF_PERSISTENT_PARITY: the replayed publications against the tables, every 60th (reported with the tables' replay). */
	struct ReplayParityState
	{
		std::uint64_t parityChecks = 0, parityMaterials = 0, parityDiffer = 0;
		std::uint64_t parityObjects = 0, parityObjectsDiffer = 0, parityLogsDiffer = 0;
		std::uint64_t parityGeometries = 0, parityGeometriesDiffer = 0, parityFamiliesDiffer = 0;
	};

	/** @brief CS_DCLF_MIRROR_PARITY, the records: the probe's slice, the coordinator -> the render thread (each geometry held: the render thread drops them). */
	struct MirrorProbeRequest
	{
		std::vector<RE::NiPointer<RE::BSGeometry>> geometries;
	};

	/** @brief CS_DCLF_MIRROR_PARITY, the records: a slice captured live, and each record's owning reference (form ID; 0 above any reference's root). */
	struct MirrorProbe
	{
		SceneCapture::Records records;
		ankerl::unordered_dense::map<const void*, std::uint32_t> owners;
		// The frame's cull hidden nodes (Api::CullHiddenSource): the engine hides and shows them around the views within the frame, each
		// store with its event, so a slice can catch one between the two. Their hidden bit is not compared. Sorted.
		std::vector<const void*> frameLocalHidden;
	};

	/** @brief CS_DCLF_MIRROR_PARITY and the compatibility monitor (PlayerParity.h), the records (SceneMirrorParity.cpp). */
	struct MirrorParityState
	{
		// The coordinator's: the next slice's position, the frame the last was asked in; the applied batch's probe, and the objects and
		// fields the batch's events named.
		std::size_t probeCursor = 0;
		std::uint32_t probeFrame = ~0u;
		std::unique_ptr<MirrorProbe> batchProbe, probe;
		SceneMirror::KeySet eventKeys;
		SceneMirror::FieldMap eventFields;
		// The coordinator -> the render thread: the slices asked (one at a time: probeAsked), and the records back.
		EventQueue<std::shared_ptr<MirrorProbeRequest>, 16> probeRequests;
		LatestSlot<MirrorProbe> probePosted;
		std::atomic<bool> probeAsked{ false };
		// What a check found stale, for the render thread's watch (CS_DCLF_MIRROR_WATCH=parity): a property, an alpha, a fade node.
		std::atomic<const void*> watchProperty{ nullptr }, watchAlpha{ nullptr }, watchNode{ nullptr };
		const void* missedProperty = nullptr;
		const void* missedAlpha = nullptr;
		const void* missedNode = nullptr;  // a fade node stale in its currentFade (missed or evented), for CS_DCLF_MIRROR_WATCH=parity:current
		// CS_DCLF_MIRROR_WATCH=missed:<field>: the first node or geometry found missed in a watched field, its type and fields.
		const void* missedWatched = nullptr;
		std::uint8_t missedWatchedType = 0;
		std::uint32_t missedWatchedFields = 0;
		std::atomic<const void*> watchMissed{ nullptr };
		std::atomic<std::uint32_t> watchMissedTypeFields{ 0 };  // the type in the top byte, the fields below
		// The check, by record type (node, geometry, property, alpha) and field.
		struct Pending
		{
			std::uint8_t type = 0;
			std::uint32_t fields = 0;
			std::string what;
			std::uint32_t flagBits = 0;  // a node's: the flag bits that differ
			std::uint32_t owner = 0;     // the probe's owning reference (MirrorProbe::owners)
		};
		// By object and record type (a geometry's node and geometry records share its key).
		ankerl::unordered_dense::map<std::uintptr_t, Pending> pending;
		// The objects an event named since their last capture (a capture takes them off): a field that differs on one has a hook.
		SceneMirror::KeySet named;
		struct Tally
		{
			std::uint64_t probes = 0, checked = 0, absent = 0;
			std::array<std::array<std::uint64_t, SceneMirror::kMaxFields>, SceneMirror::kTypes> evented{}, late{}, missed{};
			std::string firstAbsent, firstMissed, firstEvented;
			// Per type and field, the first missed object; per node flag bit, how often it differed (missed, evented).
			std::array<std::array<std::string, SceneMirror::kMaxFields>, SceneMirror::kTypes> firstMissedBy;
			std::array<std::uint64_t, 32> flagBitsMissed{}, flagBitsEvented{};
		} tally;
	};

	/**
	 * @brief CS_DCLF_SET_PARITY: the set's commit decided again (reported with the set's line). The root view's slots its decisions leave
	 * out are Features/FirstPerson's (the check's decisions announced: SetSlotRedecided).
	 */
	struct CommitParityState
	{
		std::uint64_t commitParityChecks = 0, commitParityDiffer = 0;  // full evaluations, slots that differed
	};

	/** @brief The scene area's diagnostics state: what its observers keep between their hook points and the reports. */
	struct SceneState
	{
		CommitParityState commit;
		MirrorParityState mirror;
		ReplayParityState replay;
		CandidateParityState candidates;
		ResidentParityState residents;
		// The render thread's (Prepass).
		TechniqueParityState techniques;
		ExtrasParityState extras;
		ShadingParityState shading;
		WalkParityState walk;
		ChangeLogParityState changeLog;
		// The capture's drains: the render thread's post, and the pass's newest taken (null until the first; none posted since the last
		// pass leaves none, so no drain is checked twice).
		std::atomic<std::shared_ptr<const RegistrationDrain>> drainPosted;
		std::shared_ptr<const RegistrationDrain> drain;
		// Render thread: the batch renderers the main accumulator draws from. Registration is captured from every batch renderer in the
		// game, the shadow cameras' included, so the captured set is filtered to these.
		ankerl::unordered_dense::set<const RE::BSBatchRenderer*> mainBatchRenderers;
		RegistrationParityState registrations;
		// The census's request and reply; null while none waits.
		std::atomic<std::shared_ptr<CensusRequest>> censusRequest;
		std::atomic<std::shared_ptr<CensusReply>> censusReply;
	};

	/** @brief The scene area's diagnostics state. */
	SceneState& SceneDiagnostics();
}
