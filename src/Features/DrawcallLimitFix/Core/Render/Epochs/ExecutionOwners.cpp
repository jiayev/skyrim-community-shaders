// An execution's owners (see the header).
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ExecutionOwners.h"

namespace DCLF::Draws
{
	ExecutionOwners ExecutionOwners::Make()
	{
		ExecutionOwners owners;
		auto* host = RenderGraphRuntime::Get().Host();
		if (const auto cleanup = host ? host->ResourceCleanup() : nullptr)
			owners.list = cleanup->Make<std::vector<std::shared_ptr<const void>>>();
		return owners;
	}

	void ExecutionOwners::AddPayload(const MainPayload& a_payload)
	{
		list->insert(list->end(), a_payload.body->bindingOwners.begin(), a_payload.body->bindingOwners.end());
	}

	void ExecutionOwners::Seal(SceneBuffers* a_scene)
	{
		if (a_scene)
			Add(a_scene->content.Snapshot());
	}
}
