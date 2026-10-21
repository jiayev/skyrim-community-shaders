// Portals' programs (PortalPrograms.h's encoding, "cs.dclf.portal-programs"): each depth commit publishes the frame's encoding whole
// into an immutable pooled backing sized to it (no capacity substitution), retained by every execution that reads it until GPU
// retirement; the cull binding Programs names its descriptor (Api::CullBindingSlot::Programs, the depth latch's portalProgramsIndex).
// Also the member batch's scratch (scene resources required to the objects and the encoding's buckets) and PortalTest on the CPU.

// Helpers.h first, in its own include block: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"

#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalBatch.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalPrograms.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalViews.h"
#include "Features/DrawcallLimitFix/Features/Portals/Portals.h"

#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CullInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"
#include "Features/DrawcallLimitFix/Core/Gpu/PooledBackings.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Uploads.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBufferSet.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBuffers.h"
#include "Features/DrawcallLimitFix/Core/Runtime/FrameRetirement.h"

#include <ORGModuleServices/BackingPool.h>
#include <ORGModuleServices/GrowableBufferTypes.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace DCLF::Features::Portals
{
	namespace
	{
		constexpr const char* kProgramsName = "cs.dclf.portal-programs";
		constexpr std::uint32_t kBucketsInput = 0, kInputWords = 1;

		/** @brief An immutable published encoding: its words, the lease holding them on the GPU, and the descriptor the latch names. */
		struct Publication
		{
			std::vector<std::uint32_t> words;
			std::shared_ptr<const org::services::BackingLease> lease;
			std::uint32_t descriptor = 0;
		};

		/** @brief The member batch, the publications' pool and the depth commit's publication (render thread). */
		struct ProgramsScene
		{
			std::shared_ptr<Batch> members;
			std::shared_ptr<org::services::BackingPool> pool;
			std::shared_ptr<const Publication> current;
			std::uint32_t frame = ~0u;  // the scene frame current was published for
		};

		Api::SceneResourcesId sceneId;  // install

		ProgramsScene* SceneOf(const Draws::SceneBuffers& a_scene)
		{
			return Extensions::SceneResourcesOf<ProgramsScene>(a_scene, sceneId);
		}

		// The scene's tables are made (the first frame with a device): the member batch and the publications' pool.
		std::shared_ptr<void> Setup(const Api::SceneResourcesSetup& a_setup)
		{
			auto scene = std::make_shared<ProgramsScene>();
			scene->members = CreateBatch(a_setup, "cs.dclf.member-portal");
			if (!scene->members)
				return nullptr;
			scene->pool = org::services::BackingPool::Create(PooledBackings::Hooks(), FrameRetirement(),
				{ .name = kProgramsName, .maxPerClass = 0, .spareFree = PooledBackings::PerFrameSpares() });
			return scene;
		}

		void RegisterGraph(const Api::SceneResourcesGraph& a_graph)
		{
			RegisterBatch(a_graph.graph, *a_graph.State<ProgramsScene>().members);
		}

		// The member batch's scratch: by object, and its bucket scratch to the encoding's programs (posted with the revision's inputs).
		void Require(const Api::SceneResourcesRequirements& a_requirements)
		{
			const auto& scene = a_requirements.State<ProgramsScene>();
			const auto& batch = *scene.members;
			Draws::UnitGroup{}.Add(batch.queries).Add(batch.results).Add(batch.ranks).Add(batch.indices).Add(batch.tasks).Require(a_requirements.build, a_requirements.objectRows);
			const auto buckets = std::max(1u, a_requirements.inputs[kBucketsInput]);
			Draws::SceneBufferSet::Get().Require(a_requirements.build, batch.counts, 2ull * buckets);
			Draws::SceneBufferSet::Get().Require(a_requirements.build, batch.offsets, 2ull * buckets);
		}

		void Fit(Api::SceneResourcesFit& a_fit)
		{
			// A set sealed before a revision required the batch names none of it: no object fits it (as Fades' roots), never a throw.
			const auto& b = *a_fit.State<ProgramsScene>().members;
			std::uint64_t units = ~0ull;
			for (const auto* buffer : { &b.queries, &b.results, &b.ranks, &b.indices, &b.tasks })
				units = std::min(units, Draws::NamedUnits(a_fit.sealed, *buffer));
			a_fit.fit.objects = std::min(a_fit.fit.objects, static_cast<std::uint32_t>(std::min<std::uint64_t>(units, UINT32_MAX)));
		}

		template <Api::CullBindingSlot Slot>
		void SelectMembers(Api::CullBindingSelect& a_select)
		{
			if (const auto* scene = SceneOf(a_select.scene)) {
				const auto& b = *scene->members;
				if constexpr (Slot == Api::CullBindingSlot::MemberQueries)
					a_select.Versioned(b.queries);
				else if constexpr (Slot == Api::CullBindingSlot::MemberResults)
					a_select.Versioned(b.results);
				else
					a_select.Versioned(b.ranks);
			}
		}

		std::shared_ptr<const void> BatchSetup(const Api::PassSetup& a_setup)
		{
			return Extensions::SceneResourcesStateOf(a_setup.scene, sceneId);
		}
		template <bool Evaluate>
		std::shared_ptr<org::RenderPass> MemberBatch(const Api::PassContext& a_context)
		{
			const auto resources = a_context.Main();
			const auto* scene = SceneOf(*resources->scene);
			return scene ? MakeBatchPass(resources, scene->members, a_context.segment, Evaluate) : nullptr;
		}

		// The encoding's programs: what the member batch's bucket scratch must hold.
		void PostInputs(std::span<std::uint32_t> a_words)
		{
			const auto& encoded = PortalViews::Get().Encoded();
			a_words[kBucketsInput] = encoded.size() >= kPortalHeaderBytes / 4 ? encoded[kPortalSetBytes / 4] : 0u;
		}

		/**
		 * @brief The frame's encoding, whole, into a pooled backing sized to it (render thread, the frame's first depth commit). An
		 * unchanged encoding keeps the last publication. The backing is immutable once published: every execution retains it.
		 */
		std::shared_ptr<const Publication> Publish(ProgramsScene& a_scene, Draws::CommitUploads& a_uploads)
		{
			const auto& encoded = PortalViews::Get().Encoded();
			if (encoded.size() < kPortalHeaderBytes / 4)
				return nullptr;
			if (a_scene.current && a_scene.current->words == encoded)
				return a_scene.current;
			auto publication = std::make_shared<Publication>();
			publication->words = encoded;
			// Rows of a uint structured buffer, as the shaders' ByteAddressBuffer reads them (the view the previous buffer had).
			auto lease = std::make_shared<org::services::BackingLease>(a_scene.pool->Acquire(
				{ .capacityClass = std::bit_ceil(std::max<std::uint64_t>(64, encoded.size())), .stride = 4, .name = "cs.dclf.portal-programs.encoding" }));
			if (!*lease)
				throw std::runtime_error("allocation failed");
			publication->descriptor = (*lease)->srvIndex;
			if (!publication->descriptor)
				throw std::runtime_error("missing GPU descriptor");
			a_uploads((*lease)->buffer, encoded.data(), encoded.size() * sizeof(std::uint32_t), 0);
			publication->lease = std::move(lease);
			return publication;
		}

		// The depth commit only: FadeStateCS (the next frame's state) and the depth segment's query stages read the frame's programs.
		void Commit(const Api::CommitContext& a_commit)
		{
			if (a_commit.kind != Api::CommitKind::Depth)
				return;
			auto* scene = SceneOf(a_commit.scene);
			if (!scene)
				return;
			if (scene->frame != a_commit.frame) {
				CheckMainBatchCapacity(*scene->members, a_commit.frame, std::min(Draws::NamedUnits(a_commit.adopted, scene->members->counts), Draws::NamedUnits(a_commit.adopted, scene->members->offsets)) / 2);
				scene->frame = a_commit.frame;
				// Commit hooks run unguarded on the render thread. Without a publication the latch names none (descriptor 0) and every
				// reader takes the frustum alone, the "no programs" meaning: only an allocation failure leads here.
				try {
					scene->current = Publish(*scene, a_commit.uploads);
				} catch (const std::exception& e) {
					scene->current.reset();
					static std::uint64_t failed = 0;  // render thread
					if (std::has_single_bit(++failed))
						logger::error("[DCLF][structural-gap] portal programs not published at frame {}: {}. Occurrences: {}", a_commit.frame, e.what(), failed);
				}
			}
			// Each execution reading the latch's descriptor retains the backing until GPU retirement.
			if (scene->current)
				a_commit.bindingOwners.push_back(scene->current);
		}

		// The frame's publication's descriptor (the depth commit's latch; 0: none this frame).
		void SelectPrograms(Api::CullBindingSelect& a_select)
		{
			if (const auto* scene = SceneOf(a_select.scene); scene && scene->current && scene->frame == a_select.frame)
				a_select.Index(scene->current->descriptor);
		}
	}

	int PortalTest(std::span<const std::uint32_t> a_programs, std::uint32_t a_program, const float a_centre[3], float a_radius,
		std::uint32_t a_nodeFlags, std::string& a_why)
	{
		return PortalViews::Visible(a_programs, a_program, a_centre, a_radius, a_nodeFlags, a_why);
	}

	void RegisterPrograms(Api::InstallContext& a_context)
	{
		RegisterBatchCapture("members", [](const Draws::SceneBuffers& a_scene) -> const Batch* {
			const auto* scene = SceneOf(a_scene);
			return scene ? scene->members.get() : nullptr;
		});
		sceneId = a_context.RegisterSceneResources({ .name = "portal-programs", .setup = &Setup, .registerGraph = &RegisterGraph, .require = &Require, .fit = &Fit, .postInputs = &PostInputs, .inputWords = kInputWords });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::MemberQueries, .name = "member-portal-queries", .select = &SelectMembers<Api::CullBindingSlot::MemberQueries> });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::MemberResults, .name = "member-portal-results", .select = &SelectMembers<Api::CullBindingSlot::MemberResults> });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::MemberClaims, .name = "member-portal-claims", .select = &SelectMembers<Api::CullBindingSlot::MemberClaims> });
		a_context.RegisterPass({ .key = "portals.member-batch", .passName = "cs.dclf.z.member-portal-batch", .make = &MemberBatch<false>, .setup = &BatchSetup });
		a_context.RegisterPass({ .key = "portals.member-evaluate", .passName = "cs.dclf.z.member-portal-evaluate", .make = &MemberBatch<true>, .setup = &BatchSetup });

		a_context.RegisterCommitHook({ .name = "portal-programs", .commit = &Commit });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::Programs, .name = kProgramsName, .select = &SelectPrograms });
	}
}
