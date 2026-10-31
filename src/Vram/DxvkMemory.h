#pragma once

#include <cstdint>
#include <vector>

struct DxvkMemoryResourceRow;
struct DxvkMemoryHeapReport;
struct IUnknown;

/**
 * @brief VRAM attribution's view of DXVK: the owner scopes CS opens around resource creation, and DXVK's snapshot of every live
 * buffer and image (dxvk_d3d11.dll's VRAM attribution exports, @153-@158).
 *
 * Every call is a no-op (or returns nothing) without those exports: native D3D11, or a DXVK build without them.
 */
namespace Vram
{
	/** @brief Whether DXVK's VRAM attribution exports resolved. */
	bool DxvkAvailable();

	/**
	 * @brief Labels every buffer and image DXVK creates on this thread while it lives with a_owner ("CS/ScreenSpaceGI",
	 * "Game/RenderTarget"). Scopes nest; the innermost wins. a_owner must outlive the scope.
	 */
	class OwnerScope
	{
	public:
		explicit OwnerScope(const char* a_owner);
		~OwnerScope();
		OwnerScope(const OwnerScope&) = delete;
		OwnerScope& operator=(const OwnerScope&) = delete;

	private:
		bool m_pushed = false;
	};

	/**
	 * @brief Sets the owner of a resource that already exists (a buffer, a texture, or the texture behind an SRV). Without
	 * a_overwrite, an owner from a creation scope is kept. a_owner is copied.
	 */
	void SetOwner(IUnknown* a_resource, const char* a_owner, bool a_overwrite);

	/** @brief Asks DXVK for a new snapshot of its live resources (taken on its CS thread; does not wait). */
	void RequestDxvkSnapshot();

	/** @brief The last completed snapshot into a_rows; returns its serial, 0 before the first one or without DXVK. */
	std::uint64_t ReadDxvkSnapshot(std::vector<DxvkMemoryResourceRow>& a_rows);

	/** @brief One report per Vulkan memory heap; empty without DXVK. */
	std::vector<DxvkMemoryHeapReport> DxvkHeapReports();
}
