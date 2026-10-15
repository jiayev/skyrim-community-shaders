#pragma once

// The category nodes (CoordinatorCategories): the set the tracked content hangs under, the portal graphs' roots, the
// first-person root, the published copy and the diagnostics' finds. Refreshed in Classification.cpp and ApplyEvents.cpp.
//
// A private base of Scene::Coordinator, included by Coordinator.h: names, types and initialisers are the coordinator's own,
// and the functions over them are declared there.

namespace DCLF::Scene
{
	struct CoordinatorCategories
	{
		std::shared_ptr<const CategoryCapture> categoryTaken;
		// The signature the category set was last rebuilt for.
		std::uint64_t categorySignature = 0;
		ankerl::unordered_dense::set<RE::NiNode*> categoryNodes;
		// The portal graphs' parentless roots (alwaysRenderChildren), each filed under its graph's shared portal node, a category node
		// (FindCategoryNode, RefreshCategoryNodes); held while listed.
		struct AlwaysRenderRoot
		{
			RE::NiPointer<RE::NiAVObject> root;
			RE::NiNode* category = nullptr;
		};
		ankerl::unordered_dense::map<const RE::NiAVObject*, AlwaysRenderRoot> alwaysRenderRoots;
		// The first-person members' category node (CategoryCapture::firstPersonRoot). Its kHidden is view-scoped (Main::Draw shows it only
		// for first person): ClassifyFrame stops at it before the hidden test, and its hidden events announce nothing.
		const RE::NiNode* firstPersonCategory = nullptr;
		// Diagnostics: the frame each category node was found and why (GetCategoryInfo), and the source AddGeometry stamps on new entries.
		ankerl::unordered_dense::map<const RE::NiNode*, std::pair<std::uint32_t, std::uint8_t>> categoryFound;
		CoordinatorTracked::TrackSource addSource = CoordinatorTracked::TrackSource::AttachEvent;
		std::size_t validationCursor = 0;
		// Set by a load screen's marker: the next batch rebuilds the tracked set from scratch.
		bool rescanPending = false;
		// The last copy of the category nodes, and whether the set changed since (copied again at the next publication).
		std::shared_ptr<const ankerl::unordered_dense::set<RE::NiNode*>> publishedCategoryNodes;
		bool nodeSetsDirty = true;  // the category nodes changed
	};
}
