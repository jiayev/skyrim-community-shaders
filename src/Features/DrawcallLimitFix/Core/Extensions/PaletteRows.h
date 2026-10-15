#pragma once

// The palette rows contract's Core side (Api/FrameValues.h, PaletteRowsContribution): the classes' entries, resolved once at
// InstallFeatures; what their callbacks are handed (made by FrameValues and the walk for each call, valid for that call only); and the
// calls Core makes where the walk sizes a block and writes a record (Scene/Coordinator/Walk.cpp) and at the frame's start
// (FrameDriver::BeginFrame). FrameValues (Core/Gpu/FrameValues.cpp) runs the kick, the run and the chunks.

#include "Features/DrawcallLimitFix/Api/FrameValues.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <string>
#include <type_traits>

struct ID3D11Buffer;

namespace DCLF::Api
{
	/**
	 * @brief The geometry slot a class's draws read (geometrySource): units, the rule's for the record's geometry, in; the rest out. Core
	 * resolves the slot by key (one slot a key: an address of the feature's own, never an engine TriShape's), its buffers the engine's.
	 */
	struct PaletteGeometrySource
	{
		std::uint32_t units = 0;
		const void* key = nullptr;
		ID3D11Buffer* vertexBuffer = nullptr;
		ID3D11Buffer* indexBuffer = nullptr;
		std::uint64_t vertexDesc = 0;
		std::uint32_t vertexCount = 0, indexCount = 0;
	};

	/** @brief A class's frame input (kPaletteRowsInputBytes), written by its kick and read by its run and chunks as the class's type. */
	struct PaletteRowsInput
	{
		alignas(16) std::array<std::byte, kPaletteRowsInputBytes> bytes{};

		template <class T>
		T& As()
		{
			static_assert(sizeof(T) <= kPaletteRowsInputBytes && alignof(T) <= 16 && std::is_trivially_copyable_v<T>);
			return *std::launder(reinterpret_cast<T*>(bytes.data()));
		}
		template <class T>
		const T& As() const
		{
			static_assert(sizeof(T) <= kPaletteRowsInputBytes && alignof(T) <= 16 && std::is_trivially_copyable_v<T>);
			return *std::launder(reinterpret_cast<const T*>(bytes.data()));
		}
	};

	/** @brief A class's kick (render thread, FrameValues::Kick): the frame's producer, and its input to write. */
	class PaletteRowsKick
	{
	public:
		std::uint64_t seq = 0;
		PaletteRowsInput& input;
	};

	/** @brief An item of the class with a block, listed this run (in the run's order). */
	struct PaletteRowsItem
	{
		const void* geometry = nullptr;  // its BSGeometry, as a key (the run holds it)
		void* state = nullptr;           // the class's, set by beginRun, handed back in the item's write
		std::uint32_t listed = 0;        // Core's: its place in the run's list
	};

	/** @brief A class's run (the frame's producer: beginRun, endRun): the frame's input and the run's items of the class. */
	class PaletteRowsRun
	{
	public:
		const PaletteRowsInput& input;
		std::span<PaletteRowsItem> items;
	};

	/** @brief An item's write in a chunk: what Core sampled of it, in; its block's rows and its placement, out. */
	struct PaletteRowsWrite
	{
		void* state = nullptr;      // its PaletteRowsItem::state
		bool firstPerson = false;   // a CategoryRoot view's item (PlacementPlan::Item::firstPerson): its row is the render thread's
		std::uint32_t rows = 0;     // its block's rows
		float* palette = nullptr;   // rows float4s, every one the class's to write
		// Out: the placement row's world (row-major 3x4; its previous world the same) and bound; placed false keeps Core's sample.
		float world[12]{};
		float bound[4]{};
		bool placed = false;
	};

	/** @brief The first-person writes since the first-person report's last take (FrameValues::TakeFirstPersonPaletteStats). */
	struct PaletteRowsFirstPerson
	{
		std::atomic<std::uint64_t> writes{ 0 }, units{ 0 };
		std::atomic<std::uint32_t> farthest{ 0 };  // non-negative float bits (ordered as the float)
	};

	/** @brief A class's chunk (the producer's pool, once a chunk): the frame's input and the chunk's writes, in the run's order. */
	class PaletteRowsChunk
	{
	public:
		const PaletteRowsInput& input;
		std::span<PaletteRowsWrite> writes;
		PaletteRowsFirstPerson& firstPerson;

		/** @brief A first-person write placed: a_units drawn, a_distance from the view's eye (the first-person report's). */
		void NoteFirstPerson(std::uint32_t a_units, float a_distance) const
		{
			firstPerson.writes.fetch_add(1, std::memory_order_relaxed);
			firstPerson.units.fetch_add(a_units, std::memory_order_relaxed);
			const std::uint32_t bits = std::bit_cast<std::uint32_t>(a_distance);
			for (std::uint32_t seen = firstPerson.farthest.load(std::memory_order_relaxed);
				bits > seen && !firstPerson.farthest.compare_exchange_weak(seen, bits, std::memory_order_relaxed);) {}
		}
	};
}

namespace DCLF::Extensions
{
	/** @brief A class's entry: its registration (copied, its name owned). */
	struct PaletteRowsEntry
	{
		std::string name;
		Api::PaletteRowsContribution contribution;
	};

	/** @brief Main thread, the end of InstallFeatures: the entries from the registrations (by class index - 1; itemClass Count: none). */
	void ResolvePaletteRows(const std::array<Api::PaletteRowsContribution, Api::kPaletteRowsClasses>& a_registered);
	/** @brief Any thread, after InstallFeatures (none before): a_class's entry, or null (None, or no registration). */
	const PaletteRowsEntry* PaletteRows(Api::FrameValueItemClass a_class);
	/** @brief The scene lane (the walk's evaluation): a block's rows for a geometry of a_units units of a_class (its rule); 0 without one. */
	std::uint32_t PaletteRowsOf(Api::FrameValueItemClass a_class, std::uint32_t a_units);
	/**
	 * @brief The scene lane (a record's write): a_class's geometry slot for a geometry of a_units units (its rule's, into a_source.units),
	 * from its geometrySource; false without one, or when it has none yet.
	 */
	bool PaletteGeometryOf(Api::FrameValueItemClass a_class, std::uint32_t a_units, Api::PaletteGeometrySource& a_source);
	/** @brief Render thread, FrameDriver::BeginFrame: every class's frameStart, in class order. */
	void PaletteRowsFrameStart();
}
