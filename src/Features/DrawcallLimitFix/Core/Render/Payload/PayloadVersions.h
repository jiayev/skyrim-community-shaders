#pragma once

// The scene payload (D5): seven parts, each a chain of GPU-built versions (org::services::PublishedBufferVersion, SceneArtifact::PayloadPart
// on DCLF's state graph). The snapshot builder makes each publication's PayloadSource (immutable) and posts the next version of every part
// that changed for it: the part's journal's runs since its last version and its tails, uploaded once, the rest GPU-copied from that
// version on the streaming copy queue. The snapshot is posted once every version is GPU ready, naming them (ScenePayload); the frame that
// installs it catches nothing up, and the epochs read the versions by value (PayloadVersions). Included by Internal.h, after
// Builders/ShadowPayload.h.

#include "Features/DrawcallLimitFix/Core/Extensions/PayloadProducts.h"

#include <array>
#include <memory>
#include <optional>

namespace org::runtime
{
	class IUploadService;
}

namespace org::services
{
	struct Backing;
	struct PublishedBufferVersion;
}

namespace DCLF::Draws
{
	/** @brief The payload's buffers: one version chain each (PayloadPools, its own backing pool). */
	enum class PayloadPart : std::uint8_t
	{
		Objects,       // BindlessObject per object (the stream views' objects)
		Extras,        // the extras rows (16-byte rows)
		Geometries,    // GeometryDraw per geometry slot (the newest main payload's geometryDraws, else the shadow payload's geometries)
		MaterialRows,  // MaterialRow per main material row; no addresses (the draws' sequences carry their blocks')
		PipelineRows,  // PipelineRow per main pipeline row; likewise no addresses
		Inputs,        // DrawInput: the scene list (region, then the main frame inputs, then the shadow inputs)
		ShadowRows,    // ShadowMaterialRow per shadow material row; rows hold their own address
		Count
	};
	inline constexpr std::size_t kPayloadParts = static_cast<std::size_t>(PayloadPart::Count);

	/** @brief One part's buffer as the epochs read it: values only (no recording binds it). */
	struct PayloadBuffer
	{
		std::uint32_t srvIndex = 0;  // its SRV slot (0: the part is absent)
		std::uint64_t address = 0;   // its device address (the row parts; 0 for the others)
		std::uint64_t units = 0;     // the elements this payload holds (its rows: BindlessObject, extras row, GeometryDraw, DrawInput...)
	};

	/**
	 * @brief A publication's payload source (the snapshot builder's PublishPayload, immutable): where each part's rows are - the
	 * publication's stream views, its newest main payload and its shadow payload, kept alive with its draws. The parts' versions are built
	 * from it (PayloadPools::Post: each holds it until its upload completed); the snapshot names them (ScenePayload).
	 */
	struct PayloadSource
	{
		std::shared_ptr<const void> keep;  // the publication's draws (DrawPublication), which the pointers below are into
		const StreamViews* views = nullptr;
		const GeometryDrawsOut* geometries = nullptr;  // the main payload's geometryDraws, else the shadow payload's geometries
		const MainPayload* main = nullptr;             // NewestMainPayload (null: none)
		const ShadowPayload* shadow = nullptr;         // null: none
		const KeptView<DrawInput>* region = nullptr;   // the scene list's head: the main payload's resident, else the shadow payload's regionInputs
		const std::vector<DrawInput>* mainList = nullptr;
		const std::vector<DrawInput>* shadowList = nullptr;
		std::array<std::uint64_t, kPayloadParts> units{};  // each part's elements (PayloadBuffer::units)
		// The scene list's layout (PayloadPart::Inputs, in DrawInput units): the region at the head, the main payloads' frame inputs
		// at mainInputsAt, the shadow payload's at shadowInputsAt.
		std::uint64_t regionCount = 0, mainInputsAt = 0, shadowInputsAt = 0;
		// The main payload's products (MainBody::products, by registration): installed with the snapshot (Extensions::InstallPayloadProducts).
		Extensions::PayloadProducts products;
		std::uint64_t serial = 0;  // process-unique per source
	};

	/**
	 * @brief A snapshot's payload (PayloadPools::Finish, the snapshot builder's, immutable): every present part's version, GPU ready
	 * (a part unchanged for the source keeps its last version), and the source's layout. A kept publication keeps it, and its serial.
	 */
	struct ScenePayload
	{
		std::array<std::shared_ptr<const org::services::PublishedBufferVersion>, kPayloadParts> versions;  // null: the part is absent
		std::array<std::uint64_t, kPayloadParts> units{};  // the source's (PayloadBuffer::units)
		std::uint64_t regionCount = 0, mainInputsAt = 0, shadowInputsAt = 0;
		bool main = false;    // the main parts (material and pipeline rows, the main inputs) are present
		bool shadow = false;  // the shadow parts (shadow rows, the shadow inputs) are present
		Extensions::PayloadProducts products;  // the source's
		std::uint64_t serial = 0;              // the source's
		// TEMP (set parity): the main body the source was made from, for identity only (CommitMainPayload's pairing check). The snapshot
		// holds its draws.
		const void* mainBody = nullptr;
	};

	/**
	 * @brief The frame's payload (PayloadPools::MakeFramePayload, render thread): every part's installed version, its SRV slot and
	 * device address, and the source's layout. The epochs read it by value (the BuildDraws latch's payload block, the shadow and occlusion
	 * latches, the frame records' t-registers).
	 */
	struct PayloadVersions
	{
		std::array<PayloadBuffer, kPayloadParts> parts{};
		std::uint64_t regionCount = 0, mainInputsAt = 0, shadowInputsAt = 0;
		bool main = false;    // the main parts (material and pipeline rows, the main inputs) are present
		bool shadow = false;  // the shadow parts (shadow rows, the shadow inputs) are present
		std::uint64_t serial = 0;  // the source's
		// TEMP (set parity): the main body the source was made from, for identity only (CommitMainPayload's pairing check).
		const void* mainBody = nullptr;
		// The parts' versions' backings (diagnostics: the set parity's copy of the scene list), held by the installed snapshot.
		std::array<std::shared_ptr<const org::services::Backing>, kPayloadParts> backings;

		[[nodiscard]] const PayloadBuffer& operator[](PayloadPart a_part) const { return parts[static_cast<std::size_t>(a_part)]; }
	};

	/**
	 * @brief Core's payload parts' versions: per part a backing pool (DCLF's retirement clock, a few backings a class, a build suspended
	 * while none frees) and a chain of versions, each built from the last (BuildVersionedBuffer's predecessor copy, on the graph's
	 * preparation pool: SceneGraph::PostPayloadPart): the part's journal's runs since the last version's journal position and its tails
	 * (the stream rows, the main and shadow frame inputs) uploaded, what the leased backing lacks of the last version GPU-copied from it.
	 * The journals keep their changes back to the oldest position a backing of the part's pool holds (OldestContentEpoch).
	 */
	namespace PayloadPools
	{
		/** @brief Snapshot builder: a publication's source from its draws (a_keep), stream views, newest main payload and shadow payload. */
		std::shared_ptr<const PayloadSource> MakeSource(std::shared_ptr<const void> a_keep, const StreamViews& a_views, const MainPayload* a_main,
			const ShadowPayload* a_shadow);
		/**
		 * @brief Snapshot builder: takes the finished builds into the parts' chains (a failure reported once a part, its last version
		 * kept); true when none is in flight. Each part's chain is serial: nothing is posted while this is false (a finished build wakes
		 * the builder).
		 */
		bool Settle();
		/** @brief Any thread, read racily (for a stall's message): the parts with a version in flight, by name; "none". */
		std::string PendingParts();
		/**
		 * @brief Snapshot builder, with Settle true: posts the next version of each part a_source has that changed for it (its journal
		 * moved, its units, or a tailed part's new source); the others keep theirs. a_uploads: the frame's dedicated uploader. False when
		 * nothing could be posted (no uploader that copies; a refused post): the source has no payload.
		 */
		bool Post(const std::shared_ptr<const PayloadSource>& a_source, const std::shared_ptr<org::runtime::IUploadService>& a_uploads);
		/** @brief Snapshot builder, once Settle is true after a_source's Post: its payload, the parts' versions; null when one failed. */
		std::shared_ptr<const ScenePayload> Finish(const PayloadSource& a_source);
		/** @brief Render thread, at the frame's adoption: the payload the frames read (null: none, the parts left empty). */
		void Install(std::shared_ptr<const ScenePayload> a_payload);
		/** @brief Render thread: the frame's payload, from the installed one's versions; false without one. */
		bool MakeFramePayload(PayloadVersions& a_out);
		/**
		 * @brief Render thread: the installed versions' backings are not reused before a_point (SubmittedRetirementPoint): at EndFrame,
		 * and for the one an adoption replaces (a frame whose EndFrame did not come read it).
		 */
		void BindInstalled(std::uint64_t a_point);
		/** @brief Render thread: whether the parts' next versions can be built (a graph host whose uploader has a dedicated streaming queue). */
		bool Available();
		/**
		 * @brief Any thread (the snapshot builder): the oldest journal position a backing of a_part's pool holds (its producer's, after
		 * each build), which the part's journals keep their changes back to; nullopt: none holds anything (everything may be trimmed).
		 */
		std::optional<std::uint64_t> OldestContentEpoch(PayloadPart a_part);

		/** @brief One part's report (TakeReport, the snapshot builder's): since the last take, then the pool's census and floor. */
		struct PartReport
		{
			std::uint64_t built = 0, uploadedBytes = 0, copiedBytes = 0, wholeCopies = 0, suspensions = 0, failures = 0;
			std::uint64_t liveBackings = 0, liveBytes = 0;
			std::optional<std::uint64_t> floor;
		};
		std::array<PartReport, kPayloadParts> TakeReport();
		/** @brief The parts' names, in PayloadPart order (diagnostics). */
		inline constexpr std::array<const char*, kPayloadParts> kPartNames{ "objects", "extras", "geometries", "material rows", "pipeline rows", "inputs",
			"shadow rows" };
	}

	/** @brief The participants' view of the payload parts' floors (AheadBuild::payloads): PayloadPools::OldestContentEpoch. */
	struct PayloadFloors
	{
		[[nodiscard]] std::optional<std::uint64_t> OldestContentEpoch(PayloadPart a_part) const { return PayloadPools::OldestContentEpoch(a_part); }
	};
}
