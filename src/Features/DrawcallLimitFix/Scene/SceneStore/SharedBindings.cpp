#include "Internal.h"

namespace DCLF
{
	namespace
	{
		// GpuTextures' diagnostic source tags: the projected textures from 32, the technique mask 48, a caster's diffuse 192.
		constexpr std::uint32_t kProjectedSourceTag = 32;
		constexpr std::uint32_t kMaskSourceTag = 48;
		constexpr std::uint32_t kShadowSourceTag = 192;
	}

	void SceneStore::UpdateSharedBindings()
	{
		ZoneScopedN("CS.DCLF.Scene.SharedBindings");
		auto& sb = sharedBindings;
		// The answers since the last pass, each to every kind that waited on its view. An answer nobody waits on any more lets its owner go
		// here: the last release of an import, or of its descriptor, runs on the cleanup queue (ResourceCleanupQueue::Make).
		std::uint32_t answeredKinds = 0;
		sb.replies.Drain([&](GpuTextures::Reply&& a_reply) {
			if (a_reply.cookie != sb.cookie) {
				++sb.stats.stale;
				return;
			}
			std::uint32_t kinds = 0;
			if (const auto it = sb.inFlight.find(a_reply.view); it != sb.inFlight.end()) {
				kinds = it->second;
				sb.inFlight.erase(it);
			}
			for (std::uint32_t kind = 0; kind < SharedBindings::kKinds; ++kind) {
				if ((kinds >> kind) & 1) {
					++sb.stats.answered[kind];
					if (!a_reply.binding.owner)
						++sb.stats.rejected[kind];
				}
			}
			answeredKinds |= kinds;
			if (a_reply.binding.owner) {
				auto& cached = sb.cache[a_reply.view];
				cached.owner = a_reply.binding.owner;
				cached.index = a_reply.binding.index;
				// Amortized: each answer cached looks at two other entries and drops one whose import nobody holds any more.
				for (std::uint32_t i = 0; i < 2 && !sb.cache.empty(); ++i) {
					sb.cacheSweep = (sb.cacheSweep + 1) % sb.cache.size();
					if (const auto swept = sb.cache.begin() + static_cast<std::ptrdiff_t>(sb.cacheSweep); swept->second.owner.expired())
						sb.cache.erase(swept);
				}
			}
			sb.arrived[a_reply.view] = std::move(a_reply.binding);
		});
		// The shadow textures answered (an answer for one the index has let go meanwhile is dropped with arrived).
		if (answeredKinds & (1u << SharedBindings::kShadow))
			for (const auto& answer : sb.arrived)
				ResolveShadowTextureBinding(answer.first);
		// The fixed bindings, as the render thread last published them with the import context: made there once, read here. Written into
		// the lookups where they differ (T6b2c step 5): the null texture and the samplers are the shadow builds' too, so either moves both
		// generations, and is a new shared version.
		if (auto fixed = GpuTextures::Get().Fixed(); fixed != sb.fixed) {
			sb.fixed = std::move(fixed);
			const std::uint32_t nullTexture = sb.fixed ? sb.fixed->null.index : Lookups::kNone;
			std::array<std::uint32_t, GpuTextures::kSamplerCount> samplers;
			for (std::uint32_t i = 0; i < GpuTextures::kSamplerCount; ++i)
				samplers[i] = sb.fixed ? sb.fixed->samplers[i].index : Lookups::kNone;
			const bool samplersResolved = sb.fixed != nullptr;
			if (lookups.nullTexture != nullTexture) {
				lookups.nullTexture = nullTexture;
				lookups.sharedVersion = lookups.NextVersion();
				++lookups.generation;
				++lookups.shadowGeneration;
				++sb.stats.writtenShared;
			}
			if (lookups.samplersResolved != samplersResolved || !std::equal(samplers.begin(), samplers.end(), lookups.samplers.begin())) {
				std::copy(samplers.begin(), samplers.end(), lookups.samplers.begin());
				lookups.samplersResolved = samplersResolved;
				lookups.sharedVersion = lookups.NextVersion();
				++lookups.generation;
				++lookups.shadowGeneration;
				++sb.stats.writtenShared;
			}
			sb.sharedChanged = true;
			++sb.stats.fixedPublished;
		}
		// The projected textures: a new capture's views asked for (the capture holds them meanwhile), else the pending ones answered.
		if (auto capture = sb.projectedPosted.load(std::memory_order_acquire); capture != sb.projectedSeen) {
			sb.projectedSeen = std::move(capture);
			++sb.stats.projectedCaptures;
			ResolveProjectedBindings(0xf);
		} else if (sb.projectedPending && (answeredKinds & (1u << SharedBindings::kProjected))) {
			ResolveProjectedBindings(sb.projectedPending);
		}
		// Each used pipeline's technique mask, from its row (the rows follow this pass's frame sample: RefreshTechniqueRows), as the render
		// thread's refresh took them every frame. The row's view is the frame's shadow mask target, which the frame's capture holds.
		if (sb.masks.size() < tables.pipelines.size())
			sb.masks.resize(tables.pipelines.size());
		if (lookups.pipelines.size() < tables.pipelines.size())
			lookups.pipelines.resize(tables.pipelines.size());
		auto* frameMask = FrameGlobals::Current().shadowMaskHeld.get();
		Tables::ForEachBit(tables.usedPipelineBits, [&](std::uint32_t a_slot) {
			if (a_slot < tables.pipelines.size() && tables.TechniqueConstantsValid(a_slot))
				ResolveMaskBinding(a_slot, frameMask);
		});
		// The shadow textures the dependency index added or removed since the last pass (RefreshShadowSets, the walk's).
		std::vector<std::pair<ID3D11ShaderResourceView*, bool>> changes;
		tables.TakeShadowTextureChanges(changes);
		for (const auto& [view, present] : changes) {
			if (present) {
				if (!sb.shadowTextures.try_emplace(view).second)
					continue;
				++sb.shadowPending;
				++sb.stats.shadowAdded;
				ResolveShadowTextureBinding(view);
			} else if (const auto it = sb.shadowTextures.find(view); it != sb.shadowTextures.end()) {
				// Out of the lookups' table at once (its casters have gone), its owner with it: the last release runs on the cleanup queue,
				// whichever holder of a publication naming it lets go last.
				if (it->second.pending)
					--sb.shadowPending;
				sb.shadowTextures.erase(it);
				if (lookups.shadowTextures.erase(view)) {
					++lookups.shadowGeneration;
					++sb.stats.writtenShadow;
				}
				lookups.shadowTextureOwners.erase(view);
				++sb.stats.shadowRemoved;
			}
		}
		sb.arrived.clear();
		// The shared entries' binding block: the owners of what they name, sealed again when one changed.
		if (sb.sharedChanged) {
			std::vector<std::shared_ptr<const void>> owners;
			owners.reserve(1 + GpuTextures::kSamplerCount + lookups.projectedOwners.size());
			if (sb.fixed) {
				owners.push_back(sb.fixed->null.owner);
				for (const auto& sampler : sb.fixed->samplers)
					owners.push_back(sampler.owner);
			}
			owners.insert(owners.end(), lookups.projectedOwners.begin(), lookups.projectedOwners.end());
			lookups.sharedBindingBlock = GpuTextures::Get().Seal(std::move(owners));
			++lookups.changes;
			sb.sharedChanged = false;
		}
		TracyPlot("CS.DCLF.ShadowTextureChanges", static_cast<std::int64_t>(changes.size()));
		TracyPlot("CS.DCLF.ShadowTexturePending", static_cast<std::int64_t>(sb.shadowPending));
		TracyPlot("CS.DCLF.ShadowTextureOwners", static_cast<std::int64_t>(sb.shadowTextures.size() - sb.shadowPending));
	}

	GpuTextures::Binding SceneStore::SharedViewBinding(ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag, SharedBindings::Kind a_kind)
	{
		auto& sb = sharedBindings;
		if (const auto it = sb.arrived.find(a_view); it != sb.arrived.end())
			return it->second;
		// An answer kept, here or by the material bindings (a caster's diffuse is its material's t0): an owner alive holds the view, so its
		// address names the same view still.
		auto known = [&](auto& a_cache, GpuTextures::Binding& a_out) {
			const auto it = a_cache.find(a_view);
			if (it == a_cache.end())
				return false;
			if (auto owner = it->second.owner.lock()) {
				a_out = { it->second.index, std::move(owner) };
				return true;
			}
			a_cache.erase(it);
			return false;
		};
		if (GpuTextures::Binding binding; known(sb.cache, binding) || known(materialBindings.cache, binding)) {
			++sb.stats.cached[a_kind];
			return binding;
		}
		// Asked once per view in flight; the request takes its own reference on the view at once (the caller's holds it until then).
		auto [waiting, asked] = sb.inFlight.try_emplace(a_view, 0u);
		if (asked) {
			GpuTextures::Get().Request(a_view, a_sourceTag, sb.replies, sb.cookie);
			++sb.stats.requested[a_kind];
		}
		waiting->second |= 1u << a_kind;
		return { Lookups::kNone, {}, true };
	}

	void SceneStore::ResolveProjectedBindings(std::uint32_t a_textures)
	{
		auto& sb = sharedBindings;
		for (std::uint32_t i = 0; i < lookups.projectedTextures.size(); ++i) {
			if (!((a_textures >> i) & 1))
				continue;
			// None captured yet: unresolvable, which defers the ProjectedUV pipelines' draws. A view asked for is kNone while it is.
			auto* view = sb.projectedSeen ? sb.projectedSeen->views[i].get() : nullptr;
			auto binding = view ? SharedViewBinding(view, kProjectedSourceTag + i, SharedBindings::kProjected) : GpuTextures::Binding{ Lookups::kNone, {} };
			if (binding.pending)
				sb.projectedPending |= 1u << i;
			else
				sb.projectedPending &= ~(1u << i);
			// Into the lookups where it differs: a new shared version, and a new generation for each of the owner and the index that moved.
			const bool ownerChanged = lookups.projectedOwners[i].get() != binding.owner.get();
			const bool indexChanged = lookups.projectedTextures[i] != binding.index;
			if (!ownerChanged && !indexChanged)
				continue;
			if (ownerChanged)
				++lookups.generation;
			if (indexChanged)
				++lookups.generation;
			lookups.sharedVersion = lookups.NextVersion();
			lookups.projectedTextures[i] = binding.index;
			lookups.projectedOwners[i] = std::move(binding.owner);
			sb.sharedChanged = true;
			++sb.stats.writtenShared;
		}
	}

	void SceneStore::ResolveMaskBinding(std::uint32_t a_slot, ID3D11ShaderResourceView* a_frameMask)
	{
		auto& sb = sharedBindings;
		auto& mask = sb.masks[a_slot];
		const auto& entry = lookups.pipelines[a_slot];
		const auto& technique = tables.TechniqueOf(a_slot).value;
		const bool wanted = technique.shadowMask;
		auto* view = wanted ? technique.shadowMaskTexture : nullptr;
		const std::uint32_t textureGeneration = GpuTextures::Get().Generation();
		// Unchanged and settled (resolved, rejected, or binding none): kept. One still asked for is looked up again until it is answered.
		if (mask.wanted == wanted && entry.shadowMaskView == view && entry.shadowMaskTextureGeneration == textureGeneration && mask.settled)
			return;
		GpuTextures::Binding binding{ Lookups::kNone, {} };
		if (wanted && view) {
			// The rows' views are the frame sample's (RefreshTechniqueRows), held by its capture; one that is not is counted (an observer).
			if (view != a_frameMask)
				++sb.stats.masksUnheld;
			binding = SharedViewBinding(view, kMaskSourceTag, SharedBindings::kMask);
		} else if (wanted) {
			// A technique that binds the mask while the target has no view reads the null descriptor (waited for until it is published).
			if (sb.fixed)
				binding = sb.fixed->null;
			else
				binding.pending = true;
		}
		// Held while the new view is asked for, as a material's textures are (the set's members draw with it).
		if (binding.pending && entry.shadowMaskOwner && entry.shadowMaskIndex != Lookups::kNone && entry.shadowMaskTextureGeneration == textureGeneration) {
			++sb.stats.masksHeld;
			return;
		}
		mask.settled = !binding.pending;
		mask.wanted = wanted;
		// Into the lookups' entry (T6b2c step 5): what it was resolved for, and where the owner or the index moved, a new generation for
		// each and a new version of the entry.
		auto& written = lookups.pipelines.Mutable(a_slot);
		written.shadowMaskView = view;
		written.shadowMaskTextureGeneration = textureGeneration;
		const bool ownerChanged = written.shadowMaskOwner.get() != binding.owner.get();
		const bool indexChanged = written.shadowMaskIndex != binding.index;
		if (!ownerChanged && !indexChanged)
			return;
		if (ownerChanged) {
			++lookups.generation;
			written.shadowMaskOwner = std::move(binding.owner);
		}
		if (indexChanged)
			++lookups.generation;
		written.shadowMaskIndex = binding.index;
		written.version = lookups.NextVersion();
		++sb.stats.writtenMasks;
	}

	void SceneStore::ResolveShadowTextureBinding(ID3D11ShaderResourceView* a_view)
	{
		auto& sb = sharedBindings;
		const auto it = sb.shadowTextures.find(a_view);
		if (it == sb.shadowTextures.end() || !it->second.pending)
			return;
		// The view is a member's (its caster's record names it): alive, and the request takes its own reference at once.
		auto binding = SharedViewBinding(a_view, kShadowSourceTag, SharedBindings::kShadow);
		if (binding.pending)
			return;  // not in the lookups' table until answered, so its casters wait (deferredTextures)
		// Imported or rejected (kNone, which its casters skip): a rejection is permanent, so neither is asked again. Into the lookups'
		// table, a new shadow generation where it changed.
		it->second.pending = false;
		--sb.shadowPending;
		auto [slot, inserted] = lookups.shadowTextures.try_emplace(a_view, binding.index);
		const auto owner = lookups.shadowTextureOwners.find(a_view);
		const void* heldOwner = owner == lookups.shadowTextureOwners.end() ? nullptr : owner->second.get();
		if (inserted || slot->second != binding.index || heldOwner != binding.owner.get()) {
			++lookups.shadowGeneration;
			++sb.stats.writtenShadow;
		}
		slot->second = binding.index;
		if (binding.owner)
			lookups.shadowTextureOwners[a_view] = std::move(binding.owner);
		else if (owner != lookups.shadowTextureOwners.end())
			lookups.shadowTextureOwners.erase(owner);
	}

	void SceneStore::RetireMaskBinding(std::uint32_t a_slot)
	{
		// Its owner let go (the last release runs on the cleanup queue, whichever holder of a publication naming it lets go last); a
		// successor in the slot resolves its own. The lookups' entry emptied, a new generation for each of the owner and the index that moved.
		if (a_slot < sharedBindings.masks.size())
			sharedBindings.masks[a_slot] = {};
		if (a_slot >= lookups.pipelines.size())
			return;
		if (const auto& entry = lookups.pipelines[a_slot]; !entry.shadowMaskOwner && entry.shadowMaskIndex == Lookups::kNone && !entry.shadowMaskView)
			return;
		auto& written = lookups.pipelines.Mutable(a_slot);
		const bool ownerChanged = written.shadowMaskOwner != nullptr;
		const bool indexChanged = written.shadowMaskIndex != Lookups::kNone;
		written.shadowMaskView = nullptr;
		written.shadowMaskTextureGeneration = 0;
		written.shadowMaskOwner.reset();
		written.shadowMaskIndex = Lookups::kNone;
		if (!ownerChanged && !indexChanged)
			return;
		if (ownerChanged)
			++lookups.generation;
		if (indexChanged)
			++lookups.generation;
		written.version = lookups.NextVersion();
		++sharedBindings.stats.writtenMasks;
	}

	void SceneStore::ResetSharedBindings()
	{
		// The lookups are made new with the tables (ResetSlotTables), and these entries with them. The posted projected capture is the
		// render thread's and stays: the next pass takes it again.
		auto& sb = sharedBindings;
		sb.projectedSeen.reset();
		sb.projectedPending = 0;
		sb.fixed.reset();
		sb.sharedChanged = false;
		sb.masks.clear();
		sb.shadowTextures.clear();
		sb.shadowPending = 0;
		sb.inFlight.clear();
		sb.cache.clear();
		sb.arrived.clear();
		++sb.cookie;
	}
}
