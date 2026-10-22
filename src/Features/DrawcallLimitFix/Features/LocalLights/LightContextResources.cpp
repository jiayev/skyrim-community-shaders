// Helpers.h first: volk must precede every Vulkan header in this translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"

#include "Features/DrawcallLimitFix/Features/LocalLights/LightContextResources.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CullInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"
#include "Features/DrawcallLimitFix/Core/Gpu/PooledBackings.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Uploads.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBufferSet.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBuffers.h"
#include "Features/DrawcallLimitFix/Core/Runtime/FrameRetirement.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"
#include "Features/DrawcallLimitFix/Features/LocalLights/LightSelection.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalContextAtlas.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalViews.h"
#include "Features/DrawcallLimitFix/Features/Portals/StrictGates.h"

#include <ORGModuleServices/BackingPool.h>
#include <ORGModuleServices/GrowableBufferTypes.h>
#include <algorithm>
#include <bit>
#include <map>
#include <stdexcept>

namespace DCLF::Features::LocalLights
{
	namespace
	{
		Api::SceneResourcesId sceneId;
		constexpr const char* kObjectGatesName = "cs.dclf.object-gates";
		// Interned gates past this are dropped whole (every object judged again): chains of freed nodes are not kept for ever.
		constexpr std::size_t kMaxGates = 4096;

		/**
		 * @brief The strict gates (Portals/StrictGates.h): the chains interned (gate 0 the empty one, ids stable while the table stands), the
		 * fade roots' gates, and the object column's mirror (render thread).
		 */
		struct Gates
		{
			std::map<std::vector<std::uintptr_t>, std::uint32_t> interned;
			std::vector<Portals::GateChain> chains;
			std::uint64_t generation = 0;  // bumped when the table is dropped
			std::uint64_t rootsKey = 0;
			bool rootsBuilt = false;
			std::vector<std::uint32_t> rootGate;  // by root slot
			std::uint64_t columnKey = 0;
			bool columnBuilt = false;
			std::vector<std::uint32_t> column;  // by object slot: the GPU column's words
			std::uint64_t unmodelled = 0, rootless = 0;

			void Reset()
			{
				interned.clear();
				chains.assign(1, {});
				interned.emplace(std::vector<std::uintptr_t>{}, 0u);
				++generation;
				rootsBuilt = columnBuilt = false;
				unmodelled = 0;
			}

			std::uint32_t Intern(const Portals::GateChain& a_chain)
			{
				std::vector<std::uintptr_t> key;
				key.reserve(a_chain.size() * 4);
				for (const auto& link : a_chain)
					key.insert(key.end(), { std::uintptr_t(link.kind), reinterpret_cast<std::uintptr_t>(link.node), link.program, link.roomMode });
				const auto [it, added] = interned.try_emplace(std::move(key), static_cast<std::uint32_t>(chains.size()));
				if (added) {
					chains.push_back(a_chain);
					unmodelled += std::any_of(a_chain.begin(), a_chain.end(), [](const auto& a_link) { return a_link.kind == Portals::GateLink::kShape; });
				}
				return it->second;
			}
		};

		struct Scene
		{
			std::shared_ptr<org::services::BackingPool> pool;
			std::shared_ptr<const LightContextPublication> current;
			Draws::Versioned objectGates;  // by object slot: its gate (the cull binding ObjectGates)
			org::services::FilledContents<> objectGatesFilled;  // the version the mirror is in, and the column key it holds
			Gates gates;
		};

		std::shared_ptr<void> Setup(const Api::SceneResourcesSetup& a_setup)
		{
			auto scene = std::make_shared<Scene>();
			scene->pool = org::services::BackingPool::Create(PooledBackings::Hooks(), FrameRetirement(),
				{ .name = "cs.dclf.light-contexts", .maxPerClass = 0, .spareFree = PooledBackings::PerFrameSpares() });
			scene->objectGates = Draws::MakeVersioned(a_setup.StructuredBuffer(a_setup.smallStart ? 4u : 4096u, sizeof(std::uint32_t), kObjectGatesName), { .extent = { 0, 4, 4 } });
			scene->gates.Reset();
			return scene;
		}

		void RegisterGraph(const Api::SceneResourcesGraph& a_graph)
		{
			Draws::Register(a_graph.graph, org::ResourceIdentifier(kObjectGatesName), a_graph.State<Scene>().objectGates);
		}

		// The column, by object: to the objects the revision holds.
		void Require(const Api::SceneResourcesRequirements& a_requirements)
		{
			Draws::UnitGroup{}.Add(a_requirements.State<Scene>().objectGates).Require(a_requirements.build, a_requirements.objectRows);
		}

		void Fit(Api::SceneResourcesFit& a_fit)
		{
			// A set sealed before a revision required the column names none of it: no object fits it, never a throw.
			const auto units = Draws::NamedUnits(a_fit.sealed, a_fit.State<Scene>().objectGates);
			a_fit.fit.objects = std::min(a_fit.fit.objects, static_cast<std::uint32_t>(std::min<std::uint64_t>(units, UINT32_MAX)));
		}

		/**
		 * @brief Each object's gate, when the objects, the roots or the rooms' structure moved: its fade root's (the root's chain, kept while the
		 * roots and rooms hold), its own chain without a root.
		 */
		void BuildColumn(Gates& a_gates, const SceneTables& a_tables)
		{
			if (a_gates.chains.size() > kMaxGates)
				a_gates.Reset();
			const auto roots = Extensions::RootListOf(a_tables);
			const std::uint64_t structure = PortalViews::Get().ProgramsVersion();
			const std::uint64_t rootsKey = roots.contentEpoch * 0x9E3779B97F4A7C15ull ^ structure;
			Portals::GateChain chain;
			if (!a_gates.rootsBuilt || a_gates.rootsKey != rootsKey || a_gates.rootGate.size() != roots.nodes.size()) {
				a_gates.rootsKey = rootsKey;
				a_gates.rootsBuilt = true;
				a_gates.rootGate.assign(roots.nodes.size(), 0u);
				for (std::size_t r = 0; r < roots.nodes.size(); ++r)
					if (const auto* root = static_cast<const RE::NiAVObject*>(roots.nodes[r])) {
						Portals::ChainOf(root, chain);
						a_gates.rootGate[r] = a_gates.Intern(chain);
					}
				a_gates.columnBuilt = false;
			}
			const std::uint64_t columnKey = (DCLF::Scene::FrameView::Get().GetTablesGeneration() * 0x9E3779B97F4A7C15ull ^ rootsKey) + a_gates.generation;
			if (a_gates.columnBuilt && a_gates.columnKey == columnKey && a_gates.column.size() == a_tables.objectGeometry.size())
				return;
			a_gates.columnKey = columnKey;
			a_gates.columnBuilt = true;
			a_gates.rootless = 0;
			a_gates.column.assign(a_tables.objectGeometry.size(), 0u);
			for (std::size_t o = 0; o < a_tables.objectGeometry.size(); ++o) {
				const auto root = roots.RootOf(static_cast<std::uint32_t>(o));
				if (root < a_gates.rootGate.size() && roots.NodeOf(root))
					a_gates.column[o] = a_gates.rootGate[root];
				else if (const auto* geometry = a_tables.objectGeometry[o]) {
					Portals::ChainOf(geometry, chain);
					a_gates.column[o] = a_gates.Intern(chain);
					++a_gates.rootless;
				}
			}
		}

		/**
		 * @brief The column into the adopted version: the words that differ from what that version holds, or every word for a version not
		 * filled (a growth). Every commit while a strict context is published: the passes reading it are this frame's.
		 */
		void UploadColumn(Scene& a_scene, const Api::CommitContext& a_commit)
		{
			BuildColumn(a_scene.gates, a_commit.tables);
			if (!a_commit.adopted.Names(*a_scene.objectGates))
				return;
			const auto& version = a_commit.adopted.Of(*a_scene.objectGates);
			const auto& column = a_scene.gates.column;
			if (column.size() > version.units)
				return;  // never: the revision required the column to its objects
			const auto filled = a_scene.objectGatesFilled.Since(version.serial);
			if (filled && *filled == a_scene.gates.columnKey)
				return;
			static std::vector<std::uint32_t> sent;  // render thread: the version's words as last sent
			std::vector<std::uint32_t> differing;
			if (!filled || sent.size() != column.size()) {
				if (!column.empty())
					a_commit.uploads(a_scene.objectGates, column.data(), column.size() * sizeof(std::uint32_t), 0);
			} else {
				for (std::uint32_t o = 0; o < column.size(); ++o)
					if (sent[o] != column[o])
						differing.push_back(o);
				Draws::SendWordRuns(differing, [&](std::uint32_t a_first, std::uint32_t a_count) {
					a_commit.uploads(a_scene.objectGates, column.data() + a_first, std::size_t(a_count) * sizeof(std::uint32_t), std::uint64_t(a_first) * sizeof(std::uint32_t));
				});
			}
			sent = column;
			a_scene.objectGatesFilled.Filled(version.serial, a_scene.gates.columnKey);
		}

		/**
		 * @brief The verdicts' words (Portals/StrictGates.h) for the atlas the selection encoded, each interned gate per context; empty without a
		 * strict context.
		 */
		std::vector<std::uint32_t> EncodeVerdicts(const Gates& a_gates, const LightSelection::Frame& a_selection)
		{
			const auto& atlas = a_selection.portalContexts;
			const std::uint32_t contexts = atlas.empty() ? 0u : atlas[0];
			std::vector<const Portals::Context*> byEntry(contexts, nullptr);
			bool strict = false;
			for (const auto& entry : a_selection.frameLights)
				if (entry.portalContext && entry.portalContextIndex < contexts) {
					byEntry[entry.portalContextIndex] = entry.portalContext.get();
					strict = strict || entry.portalContext->strict;
				}
			if (!strict)
				return {};
			const auto gateCount = static_cast<std::uint32_t>(a_gates.chains.size());
			std::vector<std::uint32_t> words;
			words.reserve(Portals::kGateHeaderWords + std::size_t(gateCount) * contexts);
			words.insert(words.end(), { gateCount, contexts, atlas[2], atlas[3], 0u, 0u, 0u, 0u });
			for (const auto& chain : a_gates.chains)
				for (std::uint32_t c = 0; c < contexts; ++c)
					words.push_back(byEntry[c] ? Portals::GateWord(chain, *byEntry[c]) : kPortalNoProgram);
			return words;
		}

		/** @brief A uint structured buffer of a_words, read as a ByteAddressBuffer (as the camera's programs publication). */
		std::shared_ptr<org::services::BackingLease> Upload(Scene& a_scene, const Api::CommitContext& a_commit, const std::vector<std::uint32_t>& a_words, const char* a_name)
		{
			const auto capacity = std::bit_ceil(std::max<std::uint64_t>(64, a_words.size()));
			auto lease = std::make_shared<org::services::BackingLease>(a_scene.pool->Acquire({ .capacityClass = capacity, .stride = 4, .name = a_name }));
			if (!*lease)
				throw std::runtime_error("allocation failed");
			if (!(*lease)->srvIndex)
				throw std::runtime_error("missing GPU descriptor");
			a_commit.uploads((*lease)->buffer, a_words.data(), a_words.size() * sizeof(std::uint32_t), 0);
			return lease;
		}

		void Commit(const Api::CommitContext& a_commit)
		{
			auto* scene = Extensions::SceneResourcesOf<Scene>(a_commit.scene, sceneId);
			if (!scene)
				return;
			const auto* selection = LightSelection::Get().Current(a_commit.frame);
			if (!selection || selection->portalContexts.empty()) {
				scene->current.reset();
				return;
			}
			const auto& words = selection->portalContexts;
			// Commit hooks run unguarded on the render thread: a rejected or unallocated atlas publishes nothing this frame.
			if (!scene->current || scene->current->frame != a_commit.frame ||
				scene->current->generation != (words.size() >= 4 ? std::uint64_t(words[2]) | (std::uint64_t(words[3]) << 32) : 0)) {
				scene->current.reset();
				try {
					auto publication = std::make_shared<LightContextPublication>();
					publication->frame = a_commit.frame;
					publication->generation = DCLF::Portals::ValidateContexts(words, a_commit.frame);
					publication->words = words;
					auto lease = Upload(*scene, a_commit, words, "cs.dclf.light-contexts.atlas");
					publication->descriptor = (*lease)->srvIndex;
					// The gates' column first (it interns the chains the verdicts cover), then each gate's verdict per context.
					std::shared_ptr<org::services::BackingLease> gates;
					if (std::any_of(selection->frameLights.begin(), selection->frameLights.end(), [](const auto& a_entry) { return a_entry.portalContext && a_entry.portalContext->strict; })) {
						UploadColumn(*scene, a_commit);
						publication->gateWords = EncodeVerdicts(scene->gates, *selection);
						publication->unmodelledGates = scene->gates.unmodelled;
						publication->rootlessObjects = scene->gates.rootless;
						if (!publication->gateWords.empty()) {
							gates = Upload(*scene, a_commit, publication->gateWords, "cs.dclf.light-contexts.gates");
							publication->gatesDescriptor = (*gates)->srvIndex;
						}
					}
					publication->owner = std::make_shared<std::pair<std::shared_ptr<org::services::BackingLease>, std::shared_ptr<org::services::BackingLease>>>(std::move(lease), std::move(gates));
					scene->current = std::move(publication);
				} catch (const std::exception& e) {
					static std::uint64_t rejected = 0;  // render thread
					if (std::has_single_bit(++rejected))
						logger::error("[DCLF][structural-gap] point-light context publication rejected at frame {}: {}. Occurrences: {}", a_commit.frame, e.what(), rejected);
					return;
				}
			} else if (scene->current->gatesDescriptor)
				UploadColumn(*scene, a_commit);  // a later commit's adopted version (the column is versioned): kept filled
			// Each execution retains the lease until GPU retirement, including executions sharing this frame's atlas.
			a_commit.executionOwners.push_back(scene->current);
		}

		template <bool Gates>
		void SelectContexts(Api::CullBindingSelect& a_select)
		{
			if (const auto publication = CurrentLightContexts(a_select.scene, a_select.frame))
				a_select.Index(Gates ? publication->gatesDescriptor : publication->descriptor);
		}

		void SelectObjectGates(Api::CullBindingSelect& a_select)
		{
			if (auto* scene = Extensions::SceneResourcesOf<Scene>(a_select.scene, sceneId))
				a_select.Versioned(scene->objectGates);
		}
	}

	void RegisterLightContexts(Api::InstallContext& a_context)
	{
		sceneId = a_context.RegisterSceneResources({ .name = "point-light contexts", .setup = &Setup, .registerGraph = &RegisterGraph, .require = &Require, .fit = &Fit });
		a_context.RegisterCommitHook({ .name = "point-light contexts", .commit = &Commit });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::LightContexts, .name = "cs.dclf.light-contexts", .select = &SelectContexts<false> });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::StrictGates, .name = "cs.dclf.strict-gates", .select = &SelectContexts<true> });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::ObjectGates, .name = kObjectGatesName, .select = &SelectObjectGates });
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
