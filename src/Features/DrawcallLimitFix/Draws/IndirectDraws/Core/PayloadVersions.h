#pragma once

// The scene payload's pooled versions (D5): one immutable PayloadVersions per snapshot, made by the snapshot builder's
// PayloadBuilder from the publication's payloads, installed with the draws and read by the epochs by value. Included by Internal.h,
// after Builders/ShadowPayload.h.

#include "Features/DrawcallLimitFix/Draws/Gpu/TextureTransforms.h"

namespace DCLF::Draws
{
	/** @brief The payload's buffers: one pool each (org::services::JournaledBufferSet's parts, in this order). */
	enum class PayloadPart : std::uint8_t
	{
		Objects,       // BindlessObject per object (the stream views' objects)
		Extras,        // the extras rows (16-byte rows)
		Geometries,    // GeometryDraw per geometry slot (the newest main payload's geometryDraws, else the shadow payload's geometries)
		MaterialRows,  // MaterialRow per main material row; rows hold their own address (PatchRowAddresses)
		PipelineRows,  // PipelineRow per main pipeline row; likewise
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
	 * @brief The payload of one snapshot (immutable once published): every part's backing, leased from its pool and caught up to
	 * the publication's payloads, with the uploads' readiness. The draws embed none of it: a frame installs it with the draws, binds
	 * its leases (Bind) and hands the epochs the parts by value (the BuildDraws latch's payload block, the shadow and occlusion
	 * latches, the shadow frame record's t-registers). A frame without a ready payload is not covered: no fallback.
	 */
	struct PayloadVersions
	{
		std::array<PayloadBuffer, kPayloadParts> parts{};
		// The scene list's layout (PayloadPart::Inputs, in DrawInput units): the region at the head, the main payloads' frame inputs
		// at mainInputsAt, the shadow payload's at shadowInputsAt.
		std::uint64_t regionCount = 0, mainInputsAt = 0, shadowInputsAt = 0;
		bool main = false;    // the main parts (material and pipeline rows, the main inputs) are present
		bool shadow = false;  // the shadow parts (shadow rows, the shadow inputs) are present
		// The parts' backings (by part; empty for an absent part): not reused while this lives and, after, until the clock reaches
		// what the frames that read it bound.
		std::array<org::services::BackingLease, kPayloadParts> leases;
		std::uint64_t serial = 0;  // process-unique per payload made (a kept publication keeps its payload, and its serial)
		// The main material rows that carry TexcoordOffset (MainPayload::textureTransforms): installed with the payload, the frame's
		// values patch them (FrameValues::SetTextureTransforms, TextureTransformsCS). Null without main parts.
		std::shared_ptr<const TextureTransformList> textureTransforms;
		// Every part's uploads (null: nothing was sent). The snapshot posts its Recordings stage only once this completed.
		std::shared_ptr<const org::async::GpuSubmissionSet> ready;
		// What the publication sent, per part (diagnostics: catch-up bytes and runs, whether a backing was made).
		std::array<std::uint64_t, kPayloadParts> bytes{}, runs{};
		std::array<bool, kPayloadParts> allocated{};

		[[nodiscard]] const PayloadBuffer& operator[](PayloadPart a_part) const { return parts[static_cast<std::size_t>(a_part)]; }
		/** @brief Any thread: the uploads completed (or there were none). */
		[[nodiscard]] bool Ready() const { return !ready || ready->Complete(); }
		/** @brief Any thread: every part's backing is kept from reuse until FrameRetirement() reaches a_safeAt (BackingLease::Bind). */
		void Bind(std::uint64_t a_safeAt) const noexcept
		{
			for (const auto& lease : leases)
				lease.Bind(a_safeAt);
		}
	};

	/**
	 * @brief The snapshot builder's payload maker (D5): one org::services::JournaledBufferSet with a pool per part, against
	 * FrameRetirement()'s clock, with no caps. Publish runs on the snapshot builder once its draws are built: each present part leases the
	 * free backing nearest its journal (preferEpoch = the journal's ContentEpoch()) and emits the runs since that backing's contentEpoch
	 * into it (rows patched with that backing's address, the inputs' tails written whole), then sets the contentEpoch; `ready` is
	 * built from the upload tickets. Nothing waits on the GPU.
	 *
	 * Single thread: the snapshot builder's (the pools' single acquirer). The payloads it returns are released from any thread.
	 */
	class PayloadBuilder
	{
	public:
		/** @brief Its pools on a_clock (FrameRetirement() in the game). */
		explicit PayloadBuilder(std::shared_ptr<org::services::RetirementClock> a_clock);
		~PayloadBuilder();
		PayloadBuilder(const PayloadBuilder&) = delete;
		PayloadBuilder& operator=(const PayloadBuilder&) = delete;

		/**
		 * @brief Snapshot builder: the payload of one publication: its stream views, its newest main payload (the colour build's when
		 * it ran, else the Z-prepass's; null: none) and its shadow payload (null: none). Throws when the uploader refuses a write or a
		 * backing cannot be made (the publication fails; no fallback).
		 */
		[[nodiscard]] std::shared_ptr<const PayloadVersions> Publish(const StreamViews& a_views, const MainPayload* a_main, const ShadowPayload* a_shadow,
			org::runtime::IUploadService& a_uploads);
		/**
		 * @brief Snapshot builder: the oldest content epoch a backing of a_part holds (JournaledBufferSet::OldestContentEpoch):
		 * the part's journals keep their changes back to here. nullopt: no backing holds anything (everything may be trimmed).
		 */
		[[nodiscard]] std::optional<std::uint64_t> OldestContentEpoch(PayloadPart a_part) const;
		/**
		 * @brief Snapshot builder: a_payload's uploads failed (its `ready` failed): its backings hold nothing a later publication may
		 * catch up from, so they are written whole when next leased.
		 */
		void Discard(const PayloadVersions& a_payload);
		/** @brief Any thread: a part's pool census (BackingPool::Count: atomics, approximate while leases move). */
		[[nodiscard]] org::services::BackingPool::Census Census(PayloadPart a_part) const noexcept;

		/** @brief Pool stats for the report (counters since the last call; the census is current). */
		struct Stats
		{
			std::uint64_t publications = 0;
			std::array<std::uint64_t, kPayloadParts> bytes{}, runs{}, allocated{};
			std::array<org::services::BackingPool::Census, kPayloadParts> pools{};
		};
		Stats TakeStats();

	private:
		std::unique_ptr<org::services::JournaledBufferSet> set;
		std::uint64_t serial = 0;
		Stats stats;
	};
}
