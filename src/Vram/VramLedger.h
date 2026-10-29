#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

/**
 * @brief VRAM attribution: every byte of the process's device memory, by owner.
 *
 * Merges DXVK's snapshot of its live buffers and images (Vram/DxvkMemory.h) with the render graph's tracked resources, attributes
 * each to an owner path ("CS/ScreenSpaceGI", "Game/RenderTarget/kMAIN", "DXVK/Staging buffer", "ORG/Alias pools"), and reconciles
 * the sum against what the driver reports per heap, so that memory nothing claims shows up as an explicit residual rather than
 * as a gap.
 */
namespace Vram
{
	enum RowFlags : std::uint32_t
	{
		kRowAliased = 1u << 0,       // placed in an aliasing pool, whose own row counts the bytes
		kRowNotCounted = 1u << 1,    // shown, but counted elsewhere: left out of totals
		kRowImported = 1u << 2,      // memory another allocator owns
		kRowUnattributed = 1u << 3,  // no owner
		kRowInternal = 1u << 4,      // the runtime's own (DXVK's internal resources, ORG's alias pools)
	};

	struct Row
	{
		std::string name;
		std::string owner;     // '/' nests: "CS/ScreenSpaceGI"
		std::string type;      // "Texture", "Buffer"
		std::string source;    // "D3D11", "DXVK", "ORG"
		std::string category;  // "<source>/<type>", the widget's category view
		std::string detail;    // extent and format, for the list
		std::string callsite;  // debug mode: where it was created
		std::uint64_t bytes = 0;
		std::uint64_t uid = 0;  // DXVK cookie or ORG resource ID
		std::uint32_t flags = 0;
		bool deviceLocal = true;
		std::uint32_t heapIndex = ~0u;  // Vulkan heap, ~0u where unknown (ORG)
	};

	struct Heap
	{
		std::uint32_t index = 0;
		bool deviceLocal = false;
		bool hasBudget = false;
		std::uint64_t size = 0;
		std::uint64_t usage = 0;  // process-wide (VK_EXT_memory_budget)
		std::uint64_t budget = 0;
		std::uint64_t dxvkAllocated = 0;
		std::uint64_t dxvkUsed = 0;
		std::uint64_t dxvkCache = 0;
		std::uint64_t dxvkOrgRetired = 0;
		std::uint64_t attributed = 0;  // rows' bytes in this heap (ORG's device rows go to the largest device-local heap)
	};

	enum class Kind : std::uint8_t
	{
		Total,
		Attributed,
		Residual,
		Opaque
	};

	struct Line
	{
		std::string label;
		std::uint64_t bytes = 0;
		Kind kind = Kind::Attributed;
		int depth = 0;
		std::string note;
	};

	struct Snapshot
	{
		std::vector<Row> rows;
		std::vector<Heap> heaps;
		std::vector<Line> reconciliation;  // device-local heaps, from driver usage down to resources
		std::uint64_t dxvkSerial = 0;      // 0: no DXVK snapshot (native D3D11, or none completed yet)
		bool hasDxvk = false;
		bool hasOrg = false;
		bool debug = false;  // CS_VRAM_DEBUG: creation call sites and ORG's allocation ledger
	};

	/**
	 * @brief The latest snapshot, refreshed when older than a_maxAgeSeconds (DXVK's part lags a request by its CS thread's
	 * turnaround). Any thread.
	 */
	Snapshot Current(double a_maxAgeSeconds = 0.5);

	/**
	 * @brief Labels the game's render targets, depth stencils and cubemaps "Game/RenderTarget/<name>" (replacing the generic
	 * owner their creation scope gave them). Right after the game creates them, before CS's setup replaces any (Deferred's
	 * replacements get their own owner from their scope).
	 */
	void TagGameRenderTargets();

	/** @brief One line (no newline): device-local totals, the largest a_owners owners, and the residuals. */
	std::string ReportLine(std::size_t a_owners);

	/** @brief Writes the snapshot as JSON (rows, heaps, reconciliation) under a_directory; returns the file, empty on failure. */
	std::filesystem::path WriteJson(const std::filesystem::path& a_directory);
}
