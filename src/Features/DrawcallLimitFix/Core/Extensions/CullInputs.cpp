// The main cull input's entry, the main walk's install note and the cull bindings' entries (see the header).

#include "Features/DrawcallLimitFix/Core/Extensions/CullInputs.h"

namespace DCLF::Extensions
{
	namespace
	{
		// Written only by ResolveMainCullInput (InstallFeatures, before DCLF's threads exist), read-only after.
		struct Resolved
		{
			MainCullInputEntry entry;
			bool registered = false;
			// By slot (Written only by ResolveCullBindings, as the entry).
			std::array<CullBindingEntry, kCullBindingSlots> bindings;
			std::array<bool, kCullBindingSlots> bound{};
		};

		Resolved& State()
		{
			static Resolved resolved;
			return resolved;
		}

		// Written once on the main thread at PostPostLoad (a feature's early hooks, before PrimaryCull::Install reads it).
		bool mainWalkInstalled = false;

		// CullBindingBuffer's answer without a versioned one.
		const std::shared_ptr<org::VersionedBuffer> kNoCullBuffer;
	}

	void ResolveMainCullInput(const Api::MainCullInputContribution& a_registered)
	{
		auto& resolved = State();
		resolved.registered = !a_registered.name.empty();
		if (!resolved.registered)
			return;
		auto& entry = resolved.entry;
		entry.name = std::string(a_registered.name);
		entry.contribution = a_registered;
		entry.contribution.name = entry.name;
	}

	const MainCullInputEntry* MainCullInput()
	{
		const auto& resolved = State();
		return resolved.registered ? &resolved.entry : nullptr;
	}

	void NoteMainWalkInstalled() { mainWalkInstalled = true; }
	bool MainWalkInstalled() { return mainWalkInstalled; }

	void ResolveCullBindings(const std::array<Api::CullBinding, kCullBindingSlots>& a_registered)
	{
		auto& resolved = State();
		for (std::size_t i = 0; i < kCullBindingSlots; ++i) {
			resolved.bound[i] = a_registered[i].select != nullptr;
			if (!resolved.bound[i])
				continue;
			auto& entry = resolved.bindings[i];
			entry.name = std::string(a_registered[i].name);
			entry.binding = a_registered[i];
			entry.binding.name = entry.name;
		}
	}

	const CullBindingEntry* CullBindingOf(Api::CullBindingSlot a_slot)
	{
		const auto& resolved = State();
		const auto i = static_cast<std::size_t>(a_slot);
		return i < kCullBindingSlots && resolved.bound[i] ? &resolved.bindings[i] : nullptr;
	}

	Api::CullBindingSelect SelectCullBinding(Api::CullBindingSlot a_slot, const Draws::SceneBuffers& a_scene, std::uint32_t a_frame,
		const org::services::NamedVersions* a_adopted)
	{
		Api::CullBindingSelect select{ a_scene, a_frame, a_adopted };
		if (const auto* entry = CullBindingOf(a_slot))
			entry->binding.select(select);
		return select;
	}

	const std::shared_ptr<org::VersionedBuffer>& CullBindingBuffer(Api::CullBindingSlot a_slot, const Draws::SceneBuffers& a_scene, std::uint32_t a_frame)
	{
		const auto select = SelectCullBinding(a_slot, a_scene, a_frame);
		return select.versioned ? *select.versioned : kNoCullBuffer;
	}
}
