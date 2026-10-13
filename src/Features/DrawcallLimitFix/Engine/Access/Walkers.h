#pragma once

// The scene graph walkers: a live subtree depth first (ForEachLive) and a live parent chain (ForEachParent). The mirror's walks over its
// records are SceneMirror's (ForEachMirror, ForEachMirrorParent). Any thread where the walked nodes are alive.

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace DCLF::Engine
{
	/** @brief The deepest parent chain a walk follows (a chain longer than this is not the scene graph's). */
	inline constexpr std::uint32_t kMaxParentDepth = 64;

	/** @brief What a walk's visit makes of the object it visited: its children walked, left out, or the walk ended. */
	enum class Walk : std::uint8_t
	{
		Descend,
		Skip,
		Stop
	};

	namespace Detail
	{
		/** @brief A visit's answer as a Walk: a bool is Descend (true) or Stop (false); void is Descend. */
		template <class Visit, class... Args>
		Walk Answer(Visit& a_visit, Args&&... a_args)
		{
			using R = std::invoke_result_t<Visit&, Args...>;
			if constexpr (std::is_void_v<R>) {
				a_visit(std::forward<Args>(a_args)...);
				return Walk::Descend;
			} else if constexpr (std::is_same_v<R, bool>) {
				return a_visit(std::forward<Args>(a_args)...) ? Walk::Descend : Walk::Stop;
			} else {
				return a_visit(std::forward<Args>(a_args)...);
			}
		}
	}

	/**
	 * @brief The subtree under a_root depth first, a_root included, each node's children pushed in order and taken last first:
	 * a_visit(object, depth) (depth 0 at a_root) for each, at most a_limit objects. False when a visit stopped the walk.
	 */
	template <class Visit>
	bool ForEachLive(const RE::NiAVObject* a_root, Visit&& a_visit, std::size_t a_limit = ~std::size_t{ 0 })
	{
		if (!a_root)
			return true;
		std::vector<std::pair<RE::NiAVObject*, std::uint32_t>> stack{ { const_cast<RE::NiAVObject*>(a_root), 0u } };
		for (std::size_t visited = 0; !stack.empty() && visited < a_limit; ++visited) {
			const auto [object, depth] = stack.back();
			stack.pop_back();
			const Walk walk = Detail::Answer(a_visit, *object, depth);
			if (walk == Walk::Stop)
				return false;
			if (walk == Walk::Skip)
				continue;
			if (auto* node = object->AsNode())
				for (auto& child : node->GetChildren())
					if (child)
						stack.emplace_back(child.get(), depth + 1);
		}
		return true;
	}

	/**
	 * @brief The live parent chain from a_from (included) up to its top: a_visit(object) for each until it returns false. The object it
	 * stopped at, or null when it reached the top.
	 */
	template <class Visit>
	const RE::NiAVObject* ForEachParent(const RE::NiAVObject* a_from, Visit&& a_visit)
	{
		for (const RE::NiAVObject* object = a_from; object; object = object->parent)
			if (!a_visit(*object))
				return object;
		return nullptr;
	}
}
