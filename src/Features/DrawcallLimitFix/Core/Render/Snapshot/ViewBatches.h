#pragma once

// The CPU pipeline's view batches (Direct.h; dclf-architecture.md, "The CPU-driven pipeline"): one immutable Direct::ViewBatchSet per
// snapshot, made by the snapshot builder from its draws while the graph is built for the CPU pipeline (RevisionInputs::cpuPipeline), with
// its resident sequences in a pooled version of their own, GPU ready before the snapshot is posted.

#include "Features/DrawcallLimitFix/Core/Render/Direct/Direct.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace org::runtime
{
	class IUploadService;
}

namespace DCLF
{
	struct SceneTables;
	struct Lookups;
	struct PipelineCatalog;
	struct DrawSequence;
}

namespace DCLF::Draws
{
	struct ScenePayload;
	struct MainPayload;
	struct ShadowPayload;

	/**
	 * @brief What a snapshot's view batches are made from: its draws (the scene list: the main payload's resident region and frame inputs,
	 * the shadow payload's frame inputs, read in place) and its payload (the row parts' versions, whose addresses the sequences embed), the
	 * publication's tables, lookups and catalog. keep holds them for as long as the set.
	 */
	struct ViewBatchInputs
	{
		std::shared_ptr<const void> keep;
		const SceneTables* tables = nullptr;
		const Lookups* lookups = nullptr;
		const PipelineCatalog* catalog = nullptr;
		std::uint32_t targetsGeneration = 0;  // the catalog's targets (GetIndirectState: the Z groups the main list is ordered by)
		const ScenePayload* payload = nullptr;
		const MainPayload* main = nullptr;      // NewestMainPayload (null: none)
		const ShadowPayload* shadow = nullptr;  // null: none
		std::shared_ptr<org::runtime::IUploadService> uploads;  // the frame's dedicated uploader (the sequences' version)
	};

	/**
	 * @brief The snapshot builder's view batches (one pass at a time; the builder's alone): the set of the snapshot being made, its
	 * sequences' version in flight, and the newest snapshot spheres FrameValues posted (Direct::SnapshotSpheres, asked for at every make).
	 */
	class ViewBatchBuilder
	{
	public:
		ViewBatchBuilder();
		~ViewBatchBuilder();
		ViewBatchBuilder(const ViewBatchBuilder&) = delete;
		ViewBatchBuilder& operator=(const ViewBatchBuilder&) = delete;
		struct Pending;  // a sequences version in flight (its outcome, the coordinator lane's)

		/**
		 * @brief The set for a_in's payload is complete (its sequences GPU ready: Set()): made at the first call for that payload, its
		 * sequences' version posted, and false until that version is ready (its readiness wakes the builder). True with no set when it
		 * cannot be made (no uploader that copies, a refused post, a failed version: logged): the snapshot is posted without one, and its
		 * CPU-mode epochs are not covered.
		 */
		bool Ready(const ViewBatchInputs& a_in);
		/** @brief The complete set (null: none, or not yet Ready). The builder stamps its serial and records its views (PostSnapshot). */
		const std::shared_ptr<Direct::ViewBatchSet>& Set() const { return set; }
		/** @brief The payload the set (or the one being made) is for (null: none). */
		const void* For() const { return payload; }
		/** @brief Forgets the set and what is in flight (new draws: the next Ready makes it again). */
		void Reset();

	private:
		const void* payload = nullptr;  // the payload the set or the pending one is for
		std::shared_ptr<Direct::ViewBatchSet> set, building;
		std::shared_ptr<Pending> pending;
		std::shared_ptr<const Direct::SnapshotSpheres> spheres;  // the newest taken
		std::uint64_t versions = 0;  // the sequences' versions numbered
		bool failed = false;         // this payload's set cannot be made (logged): Ready with none until another payload
	};

	/**
	 * @brief Builder thread, pure but for its scratch: a_in's set, every list and column filled, and its resident sequences into
	 * a_sequences (the GPU copy, uploaded by the caller; the set's sequenceArgs are their CPU arguments). a_spheres: the newest snapshot
	 * spheres (null: none yet, every lane a mover).
	 */
	std::shared_ptr<Direct::ViewBatchSet> MakeViewBatches(const ViewBatchInputs& a_in, const Direct::SnapshotSpheres* a_spheres,
		std::vector<DrawSequence>& a_sequences);

	/**
	 * @brief Render thread: a_set's sequences' backing is not reused before a_point (SubmittedRetirementPoint): wherever the installed
	 * payload's are bound (PayloadPools::BindInstalled), for the set the frame drew with.
	 */
	void BindViewBatches(const Direct::ViewBatchSet& a_set, std::uint64_t a_point);
}
