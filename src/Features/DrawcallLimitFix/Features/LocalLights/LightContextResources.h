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
	/** @brief Immutable selected-light atlas and GPU lease. Consumers retain this owner alongside its descriptor and identity. */
	struct LightContextPublication
	{
		std::uint32_t frame = 0, descriptor = 0;
		std::uint64_t generation = 0;
		std::vector<std::uint32_t> words;
		std::shared_ptr<const void> owner;
	};
	/** @brief Install frame-owned GPU publication at shadow/depth/colour commits, using the existing GPU retirement pool. */
	void RegisterLightContexts(Api::InstallContext& a_context);
	/** @brief Render thread: this frame's uploaded atlas, never a previous frame's selection. */
	std::shared_ptr<const LightContextPublication> CurrentLightContexts(const Draws::SceneBuffers& a_scene, std::uint32_t a_frame);
}
