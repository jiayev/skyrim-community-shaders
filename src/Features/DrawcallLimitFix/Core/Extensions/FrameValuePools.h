#pragma once

// The frame-value pool contract's Core side (Api/FrameValues.h): what a registered pool's frame start, kick and producer are handed. Made by
// FrameValues (Core/Gpu/FrameValues.cpp) for each call; valid for that call only.

#include "Features/DrawcallLimitFix/Api/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace DCLF
{
	struct FrameGlobals;
}

namespace DCLF::Api
{
	/** @brief A pool's frame start (render thread, every FrameValues::Kick): the frame's engine globals (null: none captured). */
	class FrameValuePoolFrameStart
	{
	public:
		const std::shared_ptr<const FrameGlobals>& globals;
	};

	/** @brief A pool's kick (render thread, FrameValues::Kick): the frame it is for. */
	class FrameValuePoolKick
	{
	public:
		std::uint64_t seq = 0;                     // the frame's producer
		const FrameGlobals* globals = nullptr;     // the frame's engine globals (null: none captured)
	};

	/**
	 * @brief A pool's producer (DCLF's pool, the frame's producer: sample, then send): the frame's job as the pool sees it, and its
	 * rows' way into the frame's backing.
	 */
	class FrameValuePoolProducer
	{
	public:
		/** @brief A row journal's runs since a_contentEpoch, into a_sink(data, bytes, offset) (KeptView::Emit, typed by the view's Row). */
		struct RowSink
		{
			void* owner = nullptr;
			void (*put)(void* a_owner, const void* a_data, std::size_t a_bytes, std::size_t a_offset) = nullptr;
			void operator()(const void* a_data, std::size_t a_bytes, std::size_t a_offset) const { put(owner, a_data, a_bytes, a_offset); }
		};
		using EmitRowsFn = void (*)(const void* a_view, std::uint64_t a_contentEpoch, const RowSink& a_sink);
		using SendRowsFn = void (*)(void* a_owner, std::size_t a_pool, std::size_t a_count, std::uint64_t a_contentEpoch, const void* a_view, EmitRowsFn a_emit);

		std::uint64_t seq = 0;                       // the frame's producer
		bool parity = false;                         // a parity frame (Policy::SampleEveryMover): the pool checks what it sends
		const FrameGlobals* globals = nullptr;       // the frame's engine globals (null: none captured)
		const std::shared_ptr<const void>* input = nullptr;  // what the pool's kick handed this frame (FrameValueInput::input)

		/**
		 * @brief The texture-transform buffer the frame's shaders read (the frame's capture of it, after Main::Update's flip: the engine's
		 * index as captured; 0 without globals). FrameValuePools.cpp.
		 */
		std::uint32_t TextureTransformBuffer() const;
		/** @brief The kick's input as the pool's type (null: none). */
		template <class T>
		const T* Input() const
		{
			return static_cast<const T*>(input ? input->get() : nullptr);
		}
		/**
		 * @brief Whether this producer's Kick is still the newest (the render thread's store): an engine pointer its frame's publication
		 * holds may be read, under an EngineReadWindow::Lease, only while it is (the publication is let go no earlier than the next
		 * frame's start).
		 */
		bool Newest() const { return kickedSeq && kickedSeq->load(std::memory_order_acquire) == seq; }
		/** @brief Sample: opens a_rows' build at the job's floor (every change forgotten when no backing holds any content). */
		template <class Row>
		void BeginBuild(KeptWholeArray<Row>& a_rows) const
		{
			a_rows.BeginBuild(oldestContentEpoch ? *oldestContentEpoch : a_rows.ContentEpoch());
		}
		/**
		 * @brief Send: a_rows' runs the frame's backing lacks, into it (throws when it cannot: the frame's values fail). Emitted from the
		 * rows themselves (KeptArray::Emit: no view's copies), which nothing writes while the producer sends.
		 */
		template <class Row>
		void Send(const KeptWholeArray<Row>& a_rows) const
		{
			send(owner, pool, a_rows.Size(), a_rows.ContentEpoch(), &a_rows, [](const void* a_view, std::uint64_t a_contentEpoch, const RowSink& a_sink) {
				static_cast<const KeptWholeArray<Row>*>(a_view)->Emit(a_contentEpoch, a_sink);
			});
		}
		/** @brief Sample: a_rows rows seeded from what the frame's start read of the engine, counted with the frame values' own seeds. */
		void NoteSeeded(std::uint64_t a_rows) const
		{
			if (seeded && a_rows)
				seeded->fetch_add(a_rows);
		}

		// FrameValues': the job's floor for the pool, the newest Kick, the send into the pool's backing, and its seeded count.
		std::optional<std::uint64_t> oldestContentEpoch;
		const std::atomic<std::uint64_t>* kickedSeq = nullptr;
		SendRowsFn send = nullptr;
		void* owner = nullptr;
		std::size_t pool = 0;
		std::atomic<std::uint64_t>* seeded = nullptr;
	};
}

namespace DCLF::Extensions
{
	/**
	 * @brief Render thread, from FrameValues::Kick until the next: the descriptor of a_pool's frame backing the frame's GPU work reads
	 * (0 without a producer: FrameValues::PoolIndex). FrameValuePools.cpp.
	 */
	std::uint32_t FrameValuePoolIndex(Api::FrameValuePoolId a_pool);
}
