// The vertex stream's entry, the scene resources' order and the stream's select and upload (see the header).

// Internal.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/VertexStreams.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"

namespace DCLF::Extensions
{
	namespace
	{
		// Written only by ResolveVertexStream (InstallFeatures, before DCLF's threads exist), read-only after.
		struct Resolved
		{
			VertexStreamEntry entry;
			std::vector<std::uint32_t> order;
		};

		Resolved& State()
		{
			static Resolved resolved;
			return resolved;
		}
	}

	void ResolveVertexStream(const Api::VertexStreamContribution& a_registered)
	{
		auto& resolved = State();
		const auto resources = RegisteredSceneResources();
		resolved.entry.registered = a_registered.entryClass != Api::EntryClass::Count;
		if (resolved.entry.registered) {
			resolved.entry.name = std::string(a_registered.name);
			resolved.entry.contribution = a_registered;
			resolved.entry.contribution.name = resolved.entry.name;
			resolved.order.push_back(a_registered.resources.index);
		}
		for (std::uint32_t i = 0; i < resources.size(); ++i)
			if (!resolved.entry.registered || i != a_registered.resources.index)
				resolved.order.push_back(i);
	}

	const VertexStreamEntry& VertexStream() { return State().entry; }

	bool VertexStreamTaken()
	{
		const auto& entry = State().entry;
		return entry.registered && EntryClassTaken(entry.contribution.entryClass);
	}

	std::span<const std::uint32_t> SceneResourcesOrder() { return State().order; }

	bool VertexStreamResources(std::uint32_t a_resources)
	{
		const auto& entry = State().entry;
		return entry.registered && entry.contribution.resources.index == a_resources;
	}

	std::shared_ptr<org::VersionedBuffer> SelectVertexStream(const std::vector<std::shared_ptr<void>>& a_states)
	{
		const auto& entry = State().entry;
		const std::uint32_t resources = entry.contribution.resources.index;
		if (!entry.registered || resources >= a_states.size() || !a_states[resources])
			return nullptr;
		Api::VertexStreamSelect select{ a_states[resources] };
		entry.contribution.select(select);
		return select.buffer ? *select.buffer : nullptr;
	}

	std::uint32_t UploadVertexStream(const org::services::NamedVersions& a_adopted, std::span<const Api::VertexStreamRow> a_rows, const Draws::SceneBuffers& a_scene,
		Draws::CommitUploads& a_uploads)
	{
		const auto& entry = State().entry;
		const auto& state = SceneResourcesStateOf(a_scene, entry.contribution.resources);
		if (!entry.registered || !state || !a_scene.vertexStream)
			return 0;
		return entry.contribution.upload({ a_adopted, a_rows, a_scene.vertexStream, state, a_uploads });
	}
}
