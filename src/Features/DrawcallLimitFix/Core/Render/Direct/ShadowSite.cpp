// The CPU-driven pipeline's shadow site (ShadowSite.h, SiteFrame.h): the shadow views and the occlusion maps at shadow-maps.after, as the
// GPU path's shadows.view pass draws them (Features/Shadows/Engine's ShadowViewPass): each view's casters into its map's slice and
// viewport, the slice loaded and stored (the engine's casters are already in it; the engine cleared it), under the shadow set's layout
// with the view's push words, each lane's pipeline its key slot's in the view's map row (its mode and rasterizer state: the bias and
// cull state are the pipeline's).

// Main/Resources.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/ShadowSite.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/SiteFrame.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Shapes.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

#include <Resources/Buffers/Buffer.h>
#include <Resources/ExternalTextureResource.h>
#include <rhi.h>

#include <algorithm>
#include <array>
#include <deque>
#include <memory>
#include <vector>

namespace DCLF::Direct::detail
{
	namespace
	{
		/**
		 * @brief The shadow resources' maps as the site attaches them: a DSV per slice of each imported map, in a heap of DCLF's own, made once
		 * per import set (a map imported again, or grown, makes new ones; the ones before are kept by the slot of the frame that replaced them).
		 */
		struct ShadowTargets
		{
			// The images the views are of, held as long as the views (and keyed by: an import's address can be reused by the next one).
			std::array<std::shared_ptr<org::ExternalTextureResource>, Draws::kShadowDepthTargets> depth{};
			std::array<std::uint32_t, Draws::kShadowDepthTargets> layers{};
			std::array<std::uint32_t, Draws::kShadowDepthTargets> first{};  // its slice 0's DSV in the heap
			std::array<rhi::Format, Draws::kShadowDepthTargets> formats{};
			std::array<rhi::ResourceHandle, Draws::kShadowDepthTargets> resources{};
			std::array<bool, Draws::kShadowDepthTargets> valid{};
			rhi::DescriptorHeapPtr dsvHeap;
		};

		/** @brief One view's rendering (ShadowViewPass's BeginPass): its map's slice, its viewport in it. */
		struct ShadowPass
		{
			SiteView* view = nullptr;
			rhi::DescriptorSlot dsv{};
			rhi::ResourceHandle resource{};
			std::uint32_t x = 0, y = 0, width = 0, height = 0;
			float minDepth = 0.0f, maxDepth = 1.0f;
			bool occlusion = false;
		};

		/** @brief A shadow run's own state (SiteRun::shadowState): what its views' culls and recordings read, released with the run. */
		struct ShadowState
		{
			std::shared_ptr<const Draws::ShadowFrame> shape;  // its indirect state and views (the bindings point into it)
			std::shared_ptr<const Draws::ShadowResources> resources;
			std::shared_ptr<const ShadowTargets> targets;
			std::vector<Draws::SunEntryProcess> sunEntry;  // the sun views' full-frustum processes (CullView::sunEntry)
			std::shared_ptr<const StrictGates> strict;     // the frame's strict gates (portal-strict point lights' views; null: none)
			// Per view (never moved: the bindings and site views point into them): its map row, its bindings.
			std::deque<std::vector<std::uint32_t>> mapRows;
			std::deque<RecordBindings> bindings;
			std::vector<ShadowPass> passes;  // in the latch's order: the placements' views, then the occlusion maps'
		};

		/** @brief The site's own state across frames (render thread). */
		struct ShadowSiteState
		{
			std::shared_ptr<const ShadowTargets> targets;
			std::uint64_t ranFrame = 0;  // State::frames when the site last ran
			bool loggedTargets = false;
		};

		ShadowSiteState& Own()
		{
			static ShadowSiteState state;
			return state;
		}

		/** @brief Render thread: the shadow resources' maps' DSVs (made once per import set, ShadowTargets). */
		std::shared_ptr<const ShadowTargets> ShadowTargetsOf(FrameSlot& a_slot, const Draws::ShadowResources& a_resources)
		{
			auto& own = Own();
			if (own.targets && own.targets->depth == a_resources.depth && own.targets->layers == a_resources.depthLayers)
				return own.targets;
			if (own.targets)
				a_slot.holds.push_back(std::move(own.targets));
			auto& device = S().device;
			auto targets = std::make_shared<ShadowTargets>();
			targets->depth = a_resources.depth;
			targets->layers = a_resources.depthLayers;
			std::uint32_t dsvs = 0;
			for (std::uint32_t t = 0; t < Draws::kShadowDepthTargets; ++t)
				if (targets->depth[t]) {
					targets->first[t] = dsvs;
					dsvs += std::max(1u, targets->layers[t]);
				}
			bool made = dsvs == 0 ||
			            device.CreateDescriptorHeap({ rhi::DescriptorHeapType::DSV, dsvs, false, "cs.dclf.direct.shadow-dsv" }, targets->dsvHeap) == rhi::Result::Ok;
			for (std::uint32_t t = 0; made && t < Draws::kShadowDepthTargets; ++t) {
				const auto& depth = targets->depth[t];
				if (!depth)
					continue;
				targets->formats[t] = depth->GetFormat();
				targets->resources[t] = depth->GetAPIResource().GetHandle();
				// A slice each, as the graph's declared views (DepthReadWrite of org::DsvView{ ..., slice }): an array's by its layer.
				const std::uint32_t layers = std::max(1u, targets->layers[t]);
				bool valid = true;
				for (std::uint32_t slice = 0; valid && slice < layers; ++slice) {
					rhi::DsvDesc desc{};
					desc.dimension = targets->layers[t] > 1 ? rhi::DsvDim::Texture2DArray : rhi::DsvDim::Texture2D;
					desc.range = { 0, 1, targets->layers[t] > 1 ? slice : 0u, 1 };
					desc.formatOverride = targets->formats[t];
					valid = device.CreateDepthStencilView(rhi::DescriptorSlot(targets->dsvHeap->GetHandle(), targets->first[t] + slice), targets->resources[t], desc) ==
					        rhi::Result::Ok;
				}
				targets->valid[t] = valid;
				made = made && valid;
			}
			if (!made && !std::exchange(own.loggedTargets, true))
				logger::error("[DCLF] CPU pipeline: the shadow maps' views could not be made; the shadow site draws no view into a map without one");
			own.targets = targets;
			return own.targets;
		}

		/**
		 * @brief A view's map row (RecordBindings::shadowMapRow): by key slot, its pipeline under the view's mode and rasterizer state, from the
		 * shape's row buckets the commit latched the row of (UseShadowMapRow: a key slot's bucket, the bucket's pipeline); ~0u: none (Lookups::
		 * kNone, a pipeline past the published set: its draws skipped, as the GPU path's buckets leave them out).
		 */
		void MapRowOf(const Draws::RowBuckets& a_row, std::vector<std::uint32_t>& a_out)
		{
			a_out.assign(a_row.bucketOfSlot.size(), ~0u);
			for (std::size_t k = 0; k < a_row.bucketOfSlot.size(); ++k)
				if (const std::uint32_t bucket = a_row.bucketOfSlot[k]; bucket < a_row.pipelines.size())
					a_out[k] = a_row.pipelines[bucket];
		}

		bool IsOcclusionView(ViewType a_type) { return a_type == ViewType::SkyOccluder || a_type == ViewType::PrecipOccluder; }
	}

	void RecordShadowSite(SiteOut& a_out, SiteRun& a_run)
	{
		const auto* state = static_cast<const ShadowState*>(a_run.shadowState.get());
		if (!state)
			return;
		// Each view in the latch's order, its rendering its own (the GPU path's one pass a view): the slice loaded, added to, stored.
		for (const auto& pass : state->passes) {
			rhi::DepthAttachment depth{};
			depth.dsv = pass.dsv;
			depth.depthLoad = rhi::LoadOp::Load;
			depth.depthStore = rhi::StoreOp::Store;
			depth.stencilLoad = rhi::LoadOp::Load;
			depth.stencilStore = rhi::StoreOp::Store;
			depth.resource = pass.resource;
			rhi::PassBeginInfo begin{};
			begin.depth = &depth;
			begin.x = pass.x;
			begin.y = pass.y;
			begin.width = pass.width;
			begin.height = pass.height;
			begin.minDepth = pass.minDepth;
			begin.maxDepth = pass.maxDepth;
			begin.debugName = pass.occlusion ? "DCLF direct occlusion map" : "DCLF direct shadow view";
			RecordViewPass(a_out, a_run, *pass.view, begin, *pass.view->bindings);
		}
	}
}

namespace DCLF::Direct
{
	using namespace detail;

	void ShadowCommitted(const ShadowCommitInputs& a_inputs)
	{
		auto& s = S();
		auto* slot = s.current;
		if (!slot || !slot->set || !a_inputs.shape || !a_inputs.resources || !a_inputs.adopted)
			return;
		const auto& shape = *a_inputs.shape;
		const auto& resources = *a_inputs.resources;
		// The index pool's first indices as this commit left them: the frame's first drawing commit (UpdateIndexPool), whose ranges every
		// site of the frame draws with (TakePoolFirsts' rule; the shadow resources' pool is the scene's, the main sites').
		TakePoolFirsts(*slot, resources.pool.get());
		auto& run = NewBareRun(*slot, Site::Shadow);
		// The pre-work: the shape's latched copies (the views' blocks, the commit's constants) and the index pool's copies, which the
		// depth site's draws read too (its own commit gives out no range after this one's: depthWithoutIndexPool). Queued whatever the
		// views draw.
		Draws::RevisionShapes shapes;
		shapes.shadow = a_inputs.shape;
		const PreWorkInputs prework{ a_inputs.adopted, nullptr, a_inputs.resources, &shapes, a_inputs.latchSlot };
		run.prework = PrepareShadowPreWork(prework);
		auto state = std::make_shared<ShadowState>();
		state->shape = a_inputs.shape;
		state->resources = a_inputs.resources;
		state->targets = ShadowTargetsOf(*slot, resources);
		state->sunEntry.assign(a_inputs.sunEntry.begin(), a_inputs.sunEntry.end());
		if (resources.scene)
			state->strict = FrameStrictGates(*resources.scene, slot->sceneFrame);
		run.shadowState = state;
		Own().ranFrame = s.frames;
		// What every view's recordings bind: the shape's heaps and shadow set (its indirect state, the GPU path's ShadowViewPass binds), the
		// index pool's adopted version, the slot's first indices.
		RecordBindings common{};
		common.resourceHeap = shape.resourceHeap;
		common.samplerHeap = shape.samplerHeap;
		common.shadow = &shape.indirect;
		// The run's own: its callback binds them in DXVK's buffer (SiteOut::BindHeaps).
		run.bindings.resourceHeap = shape.resourceHeap;
		run.bindings.samplerHeap = shape.samplerHeap;
		if (resources.pool)
			if (auto version = a_inputs.adopted->Find(*resources.pool->indices); version && version->buffer) {
				common.indexPool = version->buffer->GetAPIResource().GetHandle();
				slot->holds.push_back(std::move(version));
			}
		common.poolFirsts = slot->poolFirsts;
		const auto& targets = *state->targets;
		const bool drawable = shape.indirect.valid && common.indexPool.valid() && shape.rows;
		for (const auto& latched : a_inputs.views) {
			if (!drawable || latched.index >= shape.views.size() || latched.index >= shape.rows->size())
				continue;
			const auto& view = shape.views[latched.index];
			// As ShadowViewPass's preparation: a map the resources import, a slice of it (a target grown since has slices it cannot name).
			if (view.target >= Draws::kShadowDepthTargets || !targets.depth[view.target] || !targets.valid[view.target] ||
				view.slice >= std::max(1u, targets.layers[view.target]))
				continue;
			// Nothing in its list: nothing to cull or draw.
			if (slot->set->List(latched.type).empty())
				continue;
			auto& mapRow = state->mapRows.emplace_back();
			MapRowOf((*shape.rows)[latched.index], mapRow);
			auto& bindings = state->bindings.emplace_back(common);
			// The view's own pass words (ShadowFrameView::push: its latched blocks' addresses), so its recording holds for as long as the shape.
			static_assert(std::tuple_size_v<decltype(view.push)> == std::tuple_size_v<decltype(bindings.shadowPush)>);
			std::copy(view.push.begin(), view.push.end(), bindings.shadowPush.begin());
			bindings.shadowMapRow = mapRow;
			const bool occlusion = IsOcclusionView(latched.type);
			CullView cull = occlusion ? OcclusionCullView(latched.type, latched.slot, latched.latch) :
			                            ShadowCullView(latched.type, latched.slot, latched.latch, state->sunEntry);
			cull.strict = state->strict.get();
			auto& site = NewView(*slot, run, latched.type, cull, false);
			site.bindings = &bindings;
			// Its map's rendering (its format, its viewport), which Parallel mode's batches' secondaries and a per-snapshot recording are
			// recorded inside.
			{
				site.secondary = run.record == RecordMode::Parallel;
				auto& inheritance = site.inheritance;
				inheritance.depthFormat = targets.formats[view.target];
				inheritance.stencilFormat = StencilOf(targets.formats[view.target]);
				inheritance.x = view.x;
				inheritance.y = view.y;
				inheritance.width = view.width;
				inheritance.height = view.height;
				inheritance.minDepth = view.minDepth;
				inheritance.maxDepth = view.maxDepth;
				inheritance.debugName = occlusion ? "DCLF direct occlusion map" : "DCLF direct shadow view";
			}
			PrepareSnapshotView(*slot, run, site);
			ShadowPass pass{};
			pass.view = &site;
			pass.dsv = rhi::DescriptorSlot(targets.dsvHeap->GetHandle(), targets.first[view.target] + (targets.layers[view.target] > 1 ? view.slice : 0u));
			pass.resource = targets.resources[view.target];
			pass.x = view.x;
			pass.y = view.y;
			pass.width = view.width;
			pass.height = view.height;
			pass.minDepth = view.minDepth;
			pass.maxDepth = view.maxDepth;
			pass.occlusion = occlusion;
			state->passes.push_back(pass);
		}
		OnCullFrame(*slot, [slot, run = &run](const std::shared_ptr<const CullFrame>& a_frame) {
			run->cullFrame = a_frame;
			for (auto* view : run->views)
				DispatchCulls(*slot, *run, *view, a_frame);
		});
		SetPending(run);
	}

	bool ShadowSiteRan()
	{
		const auto& s = S();
		return s.current && s.frames != 0 && Own().ranFrame == s.frames;
	}

	bool ShadowSiteLive()
	{
		// DCLF's shadows on: the site draws the shadow epoch's views and maps (its coverage, the occlusion maps' readiness).
		return ActiveToggles().shadows;
	}
}
