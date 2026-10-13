#pragma once

#include "Census.h"
#include "Residue.h"
#include "SceneView.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace DCLF::Diagnostics
{
	/**
	 * @brief A tracked entry's notes (Tracked::notes, made by the first observer that writes one): the timeline's stamps. The scene
	 * pass's thread alone.
	 */
	struct TrackedNotes
	{
		// The frames its record was last taken anew (a slot), it was last bound (made resident) and it last joined the set; 0: never.
		std::uint32_t writtenFrame = 0, boundFrame = 0, memberFrame = 0;
		// Its last membership pass that gave none (PrimaryCull::LastSyntheticFail), with the frame; a record so left is bound only when
		// written again.
		std::uint32_t bindFailFrame = 0;
		std::uint8_t bindFail = 0;
	};

	/** @brief An entry's notes, made when absent. */
	TrackedNotes& NotesOf(const SceneView::Tracked& a_entry);
	/** @brief An entry's notes, all zero when it has none. */
	const TrackedNotes& NotesOrNone(const SceneView::Tracked& a_entry);

	/** @brief CS_DCLF_WALK_PARITY: the slot tables against a dense rebuild (WalkParity.cpp), the coordinator's. */
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

	/** @brief CS_DCLF_CHANGE_LOG_PARITY: the change log against a diff of the tables (ChangeLogParity.cpp), the coordinator's. */
	struct ChangeLogParityState
	{
		std::vector<SceneTables::Columns> snapshot;
		LogCursor cursor;  // the log where the snapshot was taken; active while one is waiting to be checked
		std::uint64_t checks = 0, slots = 0, changed = 0, missing = 0, skipped = 0;
		std::string first;
	};

	/** @brief CS_DCLF_TIMELINE: the frames from an event to the set, by geometry joining (Scene::kAgeBuckets). */
	struct TimelineStats
	{
		std::array<std::uint64_t, Scene::kAgeBuckets> attachToMember{};   // a geometry's first join since it was tracked
		std::array<std::uint64_t, Scene::kAgeBuckets> writtenToBound{};   // a record taken anew to its binding
		std::array<std::uint64_t, Scene::kAgeBuckets> writtenToMember{};  // a record taken anew to its join
		std::array<std::uint64_t, Scene::kAgeBuckets> showToMember{};     // a stale hidden verdict's show (its chain's last) to its join
		std::uint64_t joins = 0, firstJoins = 0, bound = 0;
		std::array<std::uint64_t, 6> bindFailures{};  // membership passes that gave none, by PrimaryCull::LastSyntheticFail
		std::string bindFailFirst;
	};

	/**
	 * @brief What the frame registered, for the scene pass after it (SceneRegistrations.cpp): the render thread at the capture's drain ->
	 * the next pass (latest wins, but for the residue: a drain no pass took carries its residue into the next, so every residue geometry is
	 * classified once).
	 */
	struct RegistrationDrain
	{
		// A reflection residue geometry with its names, read by the render thread at the drain: the scene pass classifies it a frame later,
		// past a Present that may have released it, so it dereferences none untracked.
		struct Residue
		{
			const RE::BSGeometry* geometry = nullptr;
			std::string names;  // "'name' under 'parent' (RTTI)"
		};
		// The main renderers' registrations of the frame.
		struct Registration
		{
			const RE::BSGeometry* geometry = nullptr;
			std::uint32_t hint = 0;
		};
		std::vector<Residue> residue;
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
		std::uint64_t checks = 0, slots = 0, missing = 0, named = 0, late = 0, wetness = 0, wetnessDiffer = 0;
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

	/** @brief CS_DCLF_PERSISTENT_PARITY: the kept decal order against one made whole, the coordinator's. */
	struct DecalOrderParityState
	{
		std::uint64_t checks = 0, decals = 0, differ = 0, staleKeys = 0, unordered = 0;
		std::string first, staleFirst;
	};

	/** @brief CS_DCLF_PERSISTENT_PARITY: the object LOD shapes' kept ranges against their live state, the coordinator's. */
	struct LodSegmentParityState
	{
		std::uint64_t checks = 0, shapes = 0, differ = 0;
		std::string first;
		std::uint32_t pass = 0;  // the pass (PassStamp) that ran the check: once a pass
	};

	/** @brief CS_DCLF_PERSISTENT_PARITY: the replayed publications against the tables, every 60th (reported with the tables' replay). */
	struct ReplayParityState
	{
		std::uint64_t parityChecks = 0, parityMaterials = 0, parityDiffer = 0;
		std::uint64_t parityObjects = 0, parityObjectsDiffer = 0, parityLogsDiffer = 0;
		std::uint64_t parityGeometries = 0, parityGeometriesDiffer = 0, parityFamiliesDiffer = 0;
	};

	/** @brief The LOD gates' observers: the flipped incoming geometries the flip's own publication does not claim (the coordinator's). */
	struct GateObserverState
	{
		std::uint64_t publishHoles = 0;
		std::string firstPublishHole;
	};

	/** @brief The LOD gates' observers at the release (render thread): the flips checked against the installed claims. */
	struct GateReleaseObserverState
	{
		std::uint64_t checked = 0, holes = 0, coverage = 0;
		std::string firstHole, firstCoverage;
	};

	/** @brief CS_DCLF_FANOUT_PARITY: the fanned-out rounds against their serial evaluation (reported with the fan-out line), the coordinator's. */
	struct FanoutParityState
	{
		std::uint64_t parityRounds = 0, parityEntries = 0, parityDiffers = 0;
		std::string parityFirst;
	};

	/** @brief CS_DCLF_MIRROR_PARITY, the records: the probe's slice, the coordinator -> the render thread (each geometry held: the render thread drops them). */
	struct MirrorProbeRequest
	{
		std::vector<RE::NiPointer<RE::BSGeometry>> geometries;
	};

	/** @brief CS_DCLF_MIRROR_PARITY, the records (SceneMirrorParity.cpp). */
	struct MirrorParityState
	{
		// The coordinator's: the next slice's position, the frame the last was asked in; the applied batch's probe, and the objects and
		// fields the batch's events named.
		std::size_t probeCursor = 0;
		std::uint32_t probeFrame = ~0u;
		std::unique_ptr<SceneCapture::Records> batchProbe, probe;
		SceneMirror::KeySet eventKeys;
		SceneMirror::FieldMap eventFields;
		// The coordinator -> the render thread: the slices asked (one at a time: probeAsked), and the records back.
		EventQueue<std::shared_ptr<MirrorProbeRequest>, 16> probeRequests;
		LatestSlot<SceneCapture::Records> probePosted;
		std::atomic<bool> probeAsked{ false };
		// What a check found stale, for the render thread's watch (CS_DCLF_MIRROR_WATCH=parity): a property, an alpha, a fade node.
		std::atomic<const void*> watchProperty{ nullptr }, watchAlpha{ nullptr }, watchNode{ nullptr };
		const void* missedProperty = nullptr;
		const void* missedAlpha = nullptr;
		const void* missedNode = nullptr;  // a fade node stale in its currentFade (missed or evented), for CS_DCLF_MIRROR_WATCH=parity:current
		// The check, by record type (node, geometry, property, alpha) and field.
		struct Pending
		{
			std::uint8_t type = 0;
			std::uint32_t fields = 0;
			std::string what;
			std::uint32_t flagBits = 0;  // a node's: the flag bits that differ
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

	/** @brief CS_DCLF_PERSISTENT_PARITY: tree LOD's mirror against the LOD trees root (render thread; reported with the mirror's line). */
	struct TreeLodParityState
	{
		std::uint64_t checks = 0, shapes = 0, stale = 0, missing = 0, differ = 0, late = 0, unsettled = 0;
		std::string first;
		ankerl::unordered_dense::map<const void*, std::string> suspects;  // differed at the last check, with what
		std::uint32_t frame = ~0u;                                       // the frame the last check ran in
	};

	/**
	 * @brief CS_DCLF_SET_PARITY: entries whose mirror chain reaches the first-person root while they are filed under another category node
	 * (the first-person view never takes them). Counted from the evaluations' threads.
	 */
	struct FirstPersonObserverState
	{
		std::atomic<std::uint64_t> misfiled{ 0 };
		std::atomic<const void*> misfiledGeometry{ nullptr }, misfiledCategory{ nullptr };
	};

	/** @brief CS_DCLF_SET_PARITY: the set's commit decided again, and the first-person slots its decisions leave out (reported with the set's line). */
	struct CommitParityState
	{
		std::uint64_t commitParityChecks = 0, commitParityDiffer = 0;  // full evaluations, slots that differed
		// First-person entries' slots the first-person view takes no part in (SetParticipation), the first.
		std::uint64_t firstPersonExcluded = 0;
		std::string firstFirstPersonExcluded;
		// First-person slots that take part but are held out (not resident, not main-ready, pipelines), the first.
		std::uint64_t firstPersonHeld = 0;
		std::string firstFirstPersonHeld;
	};

	/** @brief The scene area's diagnostics state: what its observers keep between their hook points and the reports. */
	struct SceneState
	{
		CommitParityState commit;
		FirstPersonObserverState firstPerson;
		TreeLodParityState treeLod;
		MirrorParityState mirror;
		FanoutParityState fanout;
		GateObserverState gates;
		GateReleaseObserverState gateReleases;
		ReplayParityState replay;
		LodSegmentParityState lodSegments;
		DecalOrderParityState decalOrder;
		CandidateParityState candidates;
		ResidentParityState residents;
		// The render thread's (Prepass).
		TechniqueParityState techniques;
		ExtrasParityState extras;
		ShadingParityState shading;
		WalkParityState walk;
		ChangeLogParityState changeLog;
		// The timeline's counts and the residue's classes: written by the coordinator, the residue's taken by the render thread's
		// reflection report, under the lock.
		std::mutex residueLock;
		TimelineStats timeline;   // under residueLock
		ResidueClasses residue;   // under residueLock
		// The timeline's: a node's last show (the hidden events) -> {frame, the store's index}; pruned by age. The coordinator's.
		ankerl::unordered_dense::map<const void*, std::pair<std::uint32_t, std::uint32_t>> unhideKeys;
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

	// The observers' report parts (the reports in Diagnostics/Reports/Scene*.cpp call them).
	/** @brief The coordinator: the timeline's counts since the last call. */
	TimelineStats TakeTimeline();
}
