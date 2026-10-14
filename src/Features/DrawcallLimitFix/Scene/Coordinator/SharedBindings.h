#pragma once

// The coordinator's shared texture bindings (projected textures, technique shadow masks, shadow textures): their requests and answers.
// Scene lane only, except projectedPosted (the render thread posts it) and the replies queue (the import thread answers into it).

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <d3d11.h>
#include <winrt/base.h>

#include <ankerl/unordered_dense.h>

#include "Features/DrawcallLimitFix/Diagnostics/ParityCounter.h"
#include "Features/DrawcallLimitFix/Draws/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Runtime/SceneWake.h"
#include "Features/DrawcallLimitFix/Scene/Model/Lookups.h"

namespace DCLF
{
	/**
	 * @brief The texture bindings every record or pipeline shares, made by the scene work (Coordinator::UpdateSharedBindings), as
	 * MaterialBindings makes the material slots': the fixed bindings (the null descriptor and the engine's samplers, which the render
	 * thread makes and publishes with the import context: GpuTextures::Fixed), the projected textures, each used pipeline's technique
	 * shadow mask, and the alpha-tested casters' diffuse textures (the shadow textures).
	 *
	 * Each is asked for where its need arises - a projected capture the render thread posted (projectedPosted), a technique row's mask,
	 * a caster's texture the shadow dependency index added (Tables::shadowTextureChanges) - with GpuTextures::Request, answered as events
	 * into `replies`, which the next pass drains. A view the scene work has had answered before (here or for a material) resolves at once.
	 *
	 * The entries are the scene lane's lookups', written straight in where they are resolved, with the generations and
	 * versions the builds key on (the shared entries and their binding block, Pipeline's mask fields, shadowTextures). This holds what is
	 * not published: the requests, the answers kept, what each entry was resolved for.
	 */
	struct SharedBindings
	{
		enum Kind : std::uint32_t
		{
			kProjected,
			kMask,
			kShadow,
			kKinds
		};
		static constexpr std::array<const char*, kKinds> kKindNames{ "projected", "shadow masks", "shadow textures" };

		/** @brief The four projected textures as a native ProjectedUV draw bound them (Coordinator::NoteProjectedTextures), each referenced. */
		struct ProjectedCapture
		{
			std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 4> views;
		};
		struct Cached
		{
			std::weak_ptr<const void> owner;
			std::uint32_t index = Lookups::kNone;
		};
		/** @brief A pipeline slot's technique shadow mask (t14): what the lookups' entry (Pipeline::shadowMask*) was resolved for. */
		struct Mask
		{
			bool wanted = false;   // the technique binds a mask
			bool settled = false;  // answered (resolved or rejected: a rejection is permanent) or binding none: not looked up again
			bool listed = false;   // on `dirty`
		};
		/** @brief A shadow texture in the dependency index: asked for (pending), or answered (in the lookups' shadowTextures, permanent). */
		struct ShadowTexture
		{
			bool pending = true;
		};
		struct Stats
		{
			std::array<std::uint64_t, kKinds> requested{};  // views asked of the import thread, by the first kind that asked
			std::array<std::uint64_t, kKinds> answered{};   // their answers drained, by every kind that waited on the view
			std::array<std::uint64_t, kKinds> rejected{};
			std::array<std::uint64_t, kKinds> cached{};     // views served from an answer kept (this one's or the material bindings')
			std::uint64_t stale = 0;                        // answers to a request made before a reset
			std::uint64_t shadowAdded = 0, shadowRemoved = 0;
			std::uint64_t projectedCaptures = 0;            // new captures taken by the scene work
			std::uint64_t masksHeld = 0;                    // a resolved mask kept while its new view is asked for
			std::uint64_t masksUnheld = 0;                  // a row's mask view not the frame's (FrameGlobals::shadowMaskHeld): asked for unheld
			std::uint64_t fixedPublished = 0;               // new fixed bindings taken
			// What was written into the lookups: shared entry changes, mask changes, shadow texture changes.
			std::uint64_t writtenShared = 0, writtenMasks = 0, writtenShadow = 0;
		};

		// The render thread's newest projected capture (latest wins; Coordinator::NoteProjectedTextures), and the one the entries are for.
		std::atomic<std::shared_ptr<const ProjectedCapture>> projectedPosted;
		std::shared_ptr<const ProjectedCapture> projectedSeen;
		std::uint32_t projectedPending = 0;  // bit per texture: its view asked for
		// The fixed bindings the entries are for (GpuTextures::Fixed).
		std::shared_ptr<const GpuTextures::FixedBindings> fixed;
		bool sharedChanged = false;  // a shared entry's owner changed since the binding block was sealed
		// Parallel to Tables::pipelines.
		std::vector<Mask> masks;
		// The masks to look up this pass (UpdateSharedBindings), each once: a pipeline made or newly used, one not settled. Everything
		// else stands while the technique rows' bindings, the texture generation and the used set stand (the witnesses below).
		std::vector<std::uint32_t> dirty;
		std::vector<std::uint64_t> usedSeen;          // the used pipelines as the last pass saw them
		std::vector<std::uint32_t> techniqueSeen;     // each technique row's bindingVersion then
		std::uint32_t textureGenerationSeen = ~0u;    // GpuTextures::Generation then
		void MarkMask(std::uint32_t a_slot)
		{
			if (masks.size() <= a_slot)
				masks.resize(std::size_t(a_slot) + 1);
			if (!masks[a_slot].listed) {
				masks[a_slot].listed = true;
				dirty.push_back(a_slot);
			}
		}
		/** @brief The work the masks' change set saved, and its parity (every used mask checked; CS_DCLF_PERSISTENT_PARITY), for the material records' line. */
		struct Work
		{
			std::uint64_t masksChecked = 0, masksResolved = 0, maskSweeps = 0;
			Diagnostics::ParityCounter parity;
		} work;
		// The dependency index's shadow textures.
		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, ShadowTexture> shadowTextures;
		std::uint32_t shadowPending = 0;
		// The views asked for and not answered yet, with the kinds waiting on each (bit per Kind).
		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, std::uint32_t> inFlight;
		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, Cached> cache;
		std::size_t cacheSweep = 0;
		// The answers of the drain being applied (rejections included).
		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, GpuTextures::Binding> arrived;
		std::uint64_t cookie = 1;  // the requests' since the last reset: an answer with another is stale
		GpuTextures::Replies replies{ &WakeSceneTextureReply };  // an answer wakes the scene pump
		Stats stats;
	};
}
