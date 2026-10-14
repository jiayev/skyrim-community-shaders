// The scene's growable buffers (SceneBufferSet): registrations, the set's ties to DCLF's state graph, uploader and device, and the
// producer's requirements and seals.
#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "Features/DrawcallLimitFix/Draws/IndirectDraws/Internal.h"

namespace DCLF::Draws
{
	namespace
	{
		std::uint64_t DeviceAddressOf(const org::Buffer& a_buffer)
		{
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host)
				return 0;
			auto device = host->GetDesc().device;
			return device.GetBufferDeviceAddress({ const_cast<org::Buffer&>(a_buffer).GetAPIResource().GetHandle(), 0 });
		}
	}

	SceneBufferSet& SceneBufferSet::Get()
	{
		static SceneBufferSet set;
		return set;
	}

	SceneBufferSet::SceneBufferSet() :
		set([this] {
			org::services::GrowableBufferSetHooks hooks;
			// Growth as graph work: made and filled on the preparation pool, ready once its copies completed; the batch's done
			// wakes the builder, which seals again.
			hooks.post = [](org::async::VersionGrowthWork a_work) { return SceneScheduler::Graph().PostGrowth(std::move(a_work)); };
			hooks.wake = [] { WakeSnapshotBuilder(); };
			hooks.uploads = [this] { return uploads.lock(); };
			hooks.addressOf = [](const org::Buffer& a_buffer) { return DeviceAddressOf(a_buffer); };
			hooks.grown = [](const org::services::GrowableRegistration& a_buffer, std::uint64_t a_from, std::uint64_t a_to, std::uint64_t a_bytes) {
				logger::info("[DCLF] {}: {} grown to {} ({} KB), as graph work", a_buffer.name, a_from, a_to, a_bytes / 1024);
			};
			return hooks;
		}())
	{}

	Versioned MakeVersioned(std::shared_ptr<org::Buffer> a_buffer, GrowableSpec a_spec)
	{
		return SceneBufferSet::Get().Make(std::move(a_buffer), std::move(a_spec));
	}

	Versioned SceneBufferSet::Make(std::shared_ptr<org::Buffer> a_first, GrowableSpec a_spec)
	{
		if (!a_first)
			return nullptr;
		org::services::GrowableRegistration registration;
		registration.name = a_first->GetName();
		auto versioned = org::VersionedBuffer::Create(std::move(a_first));
		registration.buffer = versioned;
		registration.extent = a_spec.extent;
		registration.policy = a_spec.policy;
		registration.contents = a_spec.contents;
		registration.capture = std::move(a_spec.capture);
		// Every entry carries its version's device address (the shaders read most of them by address).
		registration.address = true;
		(void)set.Register(std::move(registration));
		return versioned;
	}

	SealedSet SceneBufferSet::Seal(GrowthBuild&& a_build)
	{
		auto sealed = set.Seal(std::move(a_build));
		// The next seal that needs a failed growth posts it again.
		if (!sealed.error.empty() && failuresLogged++ < 8)
			logger::error("[DCLF] a growth of the scene's buffers failed: {}", sealed.error);
		return sealed;
	}

	void SceneBufferSet::Require(GrowthBuild& a_build, const Versioned& a_buffer, std::uint64_t a_units)
	{
		if (a_buffer && a_units)
			a_build.Require(set.HandleOf(*a_buffer), a_units);
	}

	void SceneBufferSet::RequireNewer(GrowthBuild& a_build, const Versioned& a_buffer, std::uint64_t a_serial)
	{
		if (a_buffer && a_serial)
			a_build.RequireNewer(set.HandleOf(*a_buffer), a_serial);
	}

	std::uint64_t SceneBufferSet::Units(const GrowthBuild& a_build, const Versioned& a_buffer)
	{
		return a_buffer ? a_build.Units(set.HandleOf(*a_buffer)) : 0;
	}

	std::uint64_t SceneBufferSet::Asked(const GrowthBuild& a_build, const Versioned& a_buffer)
	{
		if (!a_buffer)
			return 0;
		const auto handle = set.HandleOf(*a_buffer);
		return std::max(a_build.Asked(handle), a_build.Units(handle));
	}

	std::string SceneBufferSet::Report()
	{
		const auto stats = set.TakeStats();
		if (!stats.batches && !stats.grown && !stats.failed && !stats.latches && !stats.sealed && !stats.kept)
			return {};
		return fmt::format("{} growth batches posted, {} buffers grown, {} failed; {} latch blocks made; {} sets sealed, {} seals kept the last set", stats.batches,
			stats.grown, stats.failed, stats.latches, stats.sealed, stats.kept);
	}
}
#endif
