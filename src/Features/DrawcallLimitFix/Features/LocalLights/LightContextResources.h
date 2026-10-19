#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace DCLF::Api
{
	class InstallContext;
}
namespace DCLF::Draws
{
	struct SceneBuffers;
}

namespace DCLF::Features::LocalLights
{
	/**
	 * @brief Immutable selected-light atlas and its strict gates' verdicts (Portals/StrictGates.h), with their GPU leases. Consumers retain this
	 * owner alongside its descriptors and identity. No portal-strict context: no verdicts (gatesDescriptor 0). The objects' gates are the
	 * feature's versioned column (the cull binding ObjectGates), filled by the same commits.
	 */
	struct LightContextPublication
	{
		std::uint32_t frame = 0, descriptor = 0, gatesDescriptor = 0;
		std::uint64_t generation = 0;
		std::vector<std::uint32_t> words, gateWords;
		std::uint64_t unmodelledGates = 0;  // gates with a multibound shape the engine may test (open)
		std::uint64_t rootlessObjects = 0;  // objects without a fade root, gated by their own chain
		std::shared_ptr<const void> owner;
	};
	/**
	 * @brief Install frame-owned GPU publication at shadow/depth/colour commits, using the existing GPU retirement pool, the objects' gate
	 * column, and the cull bindings LightContexts, StrictGates and ObjectGates that name them.
	 */
	void RegisterLightContexts(Api::InstallContext& a_context);
	/** @brief Render thread: this frame's uploaded atlas, never a previous frame's selection. */
	std::shared_ptr<const LightContextPublication> CurrentLightContexts(const Draws::SceneBuffers& a_scene, std::uint32_t a_frame);
}
