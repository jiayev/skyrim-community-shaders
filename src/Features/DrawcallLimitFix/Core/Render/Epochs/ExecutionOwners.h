#pragma once

// What an epoch's execution holds until the GPU retires it (RenderGraphRuntime::ExecuteEpoch's resource owner), made the one way every
// submission makes it (dclf-architecture.md, "Descriptor ownership"): what the execution reads (its payloads' bindings, the adopted
// snapshot, its frame textures, its commit's own leases), then, sealed at the end of its record step, the scene's content owners - the
// descriptors every row it can read names, whichever commit wrote them.

#include <memory>
#include <vector>

namespace DCLF::Draws
{
	struct MainPayload;
	struct SceneBuffers;

	class ExecutionOwners
	{
	public:
		/** @brief Render thread: an empty list on the graph's cleanup queue; false without one (nothing can be submitted). */
		static ExecutionOwners Make();

		explicit operator bool() const { return list != nullptr; }
		void Add(std::shared_ptr<const void> a_owner)
		{
			if (a_owner)
				list->push_back(std::move(a_owner));
		}
		/** @brief a_payload's bindings (the rows it built name). */
		void AddPayload(const MainPayload& a_payload);
		/** @brief The list itself, for a commit that adds what only this execution reads (CommitContext::executionOwners). */
		std::vector<std::shared_ptr<const void>>& List() { return *list; }
		/**
		 * @brief At the end of the record step, after the execution's commit wrote its content: the content owners of a_scene (null: none)
		 * as they now stand.
		 */
		void Seal(SceneBuffers* a_scene);
		/** @brief The resource owner the execution is submitted with. */
		std::shared_ptr<const void> Handle() const { return list; }

	private:
		std::shared_ptr<std::vector<std::shared_ptr<const void>>> list;
	};
}
