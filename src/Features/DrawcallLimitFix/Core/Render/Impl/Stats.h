#pragma once

// IndirectDraws::Impl's report counters (ImplStats, a base of Impl).
// Part of IndirectDraws' implementation: included by Impl.h, after Impl/SnapshotBuilder.h (snapshotTimings names its SnapshotTiming).

namespace DCLF
{
	// The report counters: each says whose it is and what it counts since the last report.
	struct ImplStats
	{
		struct CapabilityStats
		{
			std::uint32_t changes = 0;  // every change of the modes or the catalog, setup's included
			std::uint32_t defects = 0;  // of them, a view the capability did not foresee
			std::string lastCause;
		} capabilityStats;
		std::uint64_t shadowUnbuilt = 0;  // covered frames whose installed publication had no shadow payload (<- UNBUILT), since the last report
		// Frames whose views were left to the engine (no shape or payload yet), since the last report.
		std::uint64_t shadowUnrecorded = 0;
		std::uint64_t occlusionUnrecorded = 0;  // maps left to the engine for want of the revision's shape, since the last report
		// The builder's counters (atomic: the report's exchange), and its builds' times.
		std::atomic<std::uint64_t> snapshotsBuilt{ 0 }, snapshotsCoalesced{ 0 }, snapshotsWithoutDraws{ 0 };
		std::atomic<std::uint64_t> payloadsInstalled{ 0 };  // the frame's (AdoptSnapshot: a new payload installed), the builder's report's exchange
		EventQueue<ImplSnapshotBuilder::SnapshotTiming, 256> snapshotTimings;
		// The frame's, since the last report: adopted, frames adopting nothing new, stale by kind, frames from commit to adoption.
		enum SnapshotStale : std::size_t
		{
			kStaleBuild,
			kStaleTargets,
			kStaleShadowFormat,
			kStaleToggles,
			kStaleMain,
			kStaleKinds
		};
		std::uint64_t snapshotsAdopted = 0, snapshotFramesKept = 0;
		std::array<std::uint64_t, kStaleKinds> snapshotsStale{};
		std::vector<std::uint32_t> adoptionLatency;
		// Frames from a growth's adoption (~0u: none) to draws embedding its versions (DecideCoverage).
		std::uint32_t growthAdoptedFrame = ~0u;
		std::vector<std::uint32_t> growthToDraws;
		// From the oldest event its publication first carried (ScenePublication::eventNs, eventFrame) to its adoption, ms and frames.
		std::vector<double> eventAdoptionMs;
		std::vector<std::uint32_t> eventAdoptionFrames;
		// The forward epoch's report counters.
		struct ForwardEpochStats
		{
			std::uint64_t reached = 0, epochs = 0, noColour = 0, noRecording = 0, failed = 0;
			std::uint32_t submittedFrame = ~0u;
			// Refraction: epochs that drew the normals (marked begun for the engine), and those whose members could not.
			std::uint64_t refractionDrawn = 0, refractionNotReady = 0;
			std::uint32_t refractionFrame = ~0u;  // the frame whose forward epoch drew the refraction normals
		} forwardEpoch;
		// Per main epoch since the last report: covered frames with no payload for the main resources (not submitted, <- UNBUILT).
		std::array<std::uint64_t, 2> unbuilt{};
		std::uint64_t frameSlotsCarried = 0;  // since the last report
	};
}
