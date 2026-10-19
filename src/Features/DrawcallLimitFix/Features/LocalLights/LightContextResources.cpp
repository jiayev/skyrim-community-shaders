// Helpers.h first: volk must precede every Vulkan header in this translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"

#include "Features/DrawcallLimitFix/Features/LocalLights/LightContextResources.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"
#include "Features/DrawcallLimitFix/Core/Gpu/PooledBackings.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Uploads.h"
#include "Features/DrawcallLimitFix/Core/Runtime/FrameRetirement.h"
#include "Features/DrawcallLimitFix/Features/LocalLights/LightSelection.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalContextAtlas.h"

#include <ORGModuleServices/BackingPool.h>
#include <bit>
#include <stdexcept>

namespace DCLF::Features::LocalLights
{
	namespace
	{
		Api::SceneResourcesId sceneId;
		struct Scene
		{
			std::shared_ptr<org::services::BackingPool> pool;
			std::shared_ptr<const LightContextPublication> current;
		};
		std::shared_ptr<void> Setup(const Api::SceneResourcesSetup&)
		{
			auto scene = std::make_shared<Scene>();
			scene->pool = org::services::BackingPool::Create(PooledBackings::Hooks(), FrameRetirement(),
				{ .name = "cs.dclf.light-contexts", .maxPerClass = 0, .spareFree = PooledBackings::PerFrameSpares() });
			return scene;
		}
		void Commit(const Api::CommitContext& a_commit)
		{
			auto* scene = Extensions::SceneResourcesOf<Scene>(a_commit.scene, sceneId);
			if (!scene)
				return;
			const auto* selection = LightSelection::Get().Current(a_commit.frame);
			if (!selection) {
				scene->current.reset();
				return;
			}
			const auto& words = selection->portalContexts;
			const auto generation = DCLF::Portals::ValidateContexts(words, a_commit.frame);
			if (!scene->current || scene->current->frame != a_commit.frame || scene->current->generation != generation) {
				auto publication = std::make_shared<LightContextPublication>();
				publication->frame = a_commit.frame;
				publication->generation = generation;
				publication->words = words;
				const auto capacity = std::bit_ceil(std::max<std::uint64_t>(256, words.size() * sizeof(std::uint32_t)));
				auto lease = std::make_shared<org::services::BackingLease>(scene->pool->Acquire(
					{ .capacityClass = capacity, .name = "cs.dclf.light-contexts.atlas" }));
				if (!*lease)
					throw std::runtime_error("point-light context publication: allocation failed");
				publication->descriptor = (*lease)->srvIndex;
				if (!publication->descriptor)
					throw std::runtime_error("point-light context publication: missing GPU descriptor");
				a_commit.uploads((*lease)->buffer, words.data(), words.size() * sizeof(std::uint32_t), 0);
				publication->owner = std::move(lease);
				scene->current = std::move(publication);
			}
			// Each execution retains the lease until GPU retirement, including executions sharing this frame's atlas.
			a_commit.bindingOwners.push_back(scene->current);
		}
	}

	void RegisterLightContexts(Api::InstallContext& a_context)
	{
		sceneId = a_context.RegisterSceneResources({ .name = "point-light contexts", .setup = &Setup });
		a_context.RegisterCommitHook({ .name = "point-light contexts", .commit = &Commit });
	}

	std::shared_ptr<const LightContextPublication> CurrentLightContexts(const Draws::SceneBuffers& a_scene, std::uint32_t a_frame)
	{
		const auto* scene = Extensions::SceneResourcesOf<Scene>(a_scene, sceneId);
		const auto* selection = LightSelection::Get().Current(a_frame);
		if (!scene || !scene->current || scene->current->frame != a_frame || !selection || selection->portalContexts.size() < 4)
			return nullptr;
		const auto& words = selection->portalContexts;
		const auto generation = std::uint64_t(words[2]) | (std::uint64_t(words[3]) << 32);
		return scene->current->generation == generation ? scene->current : nullptr;
	}
}
