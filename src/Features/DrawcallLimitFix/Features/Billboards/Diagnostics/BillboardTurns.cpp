// CS_DCLF_SET_PARITY: the billboards' observer (Billboards.h, InstallTurns): the thunk after NiBillboardNode::OnVisible, on whatever
// thread runs the cull; the report takes its counts on the render thread.

#include "Features/DrawcallLimitFix/Features/Billboards/Billboards.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Core/Gpu/PlacementRows/BillboardRow.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"

#include <array>
#include <atomic>
#include <bit>
#include <cmath>

namespace DCLF::Features::Billboards
{
	namespace
	{
		using DCLF::PlacementRows::BillboardFaceMode;
		using DCLF::PlacementRows::SampleBillboardRow;

		// NiBillboardNode's vtable slot OnVisible and what it holds (AE 0x140d3f720, SE 0x140c78d90).
		constexpr std::size_t kOnVisibleSlot = 0x34;
		constexpr DCLF::Engine::Offset kOnVisible{ 0xd3f720, 0xc78d90 };
		// The observer's tolerance: a leaf whose world differs from the engine's by more than this (a rotation entry over its scale, a
		// translation in units) is counted as differing. The port multiplies in a different order (R0 * M * relative against the
		// engine's (R0 * M) then each child's local), so a few ulps are expected.
		constexpr float kRotationTolerance = 1e-4f;
		constexpr float kTranslationTolerance = 0.01f;

		// The observer's counts, since the last report.
		std::atomic<std::uint64_t> checked{ 0 }, differ{ 0 }, turns{ 0 };
		std::atomic<std::uint32_t> largestRotationBits{ 0 }, largestTranslationBits{ 0 };
		std::atomic<std::uint32_t> firstState{ 0 };  // 0 free, 1 being written, 2 written
		std::array<char, 1024> first{};
		bool installed = false;

		void Largest(std::atomic<std::uint32_t>& a_bits, float a_value)
		{
			// Non-negative floats order as their bits.
			const std::uint32_t bits = std::bit_cast<std::uint32_t>(a_value);
			std::uint32_t seen = a_bits.load(std::memory_order_relaxed);
			while (bits > seen && !a_bits.compare_exchange_weak(seen, bits, std::memory_order_relaxed)) {
			}
		}

		/** @brief The observer: a_leaf's world as the engine just turned it for a_camera, against DCLF's row and DCLF's turn of it. */
		void CheckLeaf(const RE::BSGeometry& a_leaf, const RE::NiBillboardNode& a_node, const Camera& a_camera)
		{
			BindlessPlacement row{};
			if (!SampleBillboardRow(a_leaf, row))
				return;
			// Only the leaves this node turns (SampleRow finds the nearest billboard: a nested one's leaves are its).
			const RE::NiNode* nearest = a_leaf.parent;
			while (nearest && !netimmerse_cast<RE::NiBillboardNode*>(const_cast<RE::NiNode*>(nearest)))
				nearest = nearest->parent;
			if (nearest != &a_node)
				return;
			const float base[3][3]{ { row.previousWorld[0], row.previousWorld[1], row.previousWorld[2] },
				{ row.previousWorld[4], row.previousWorld[5], row.previousWorld[6] }, { row.previousWorld[8], row.previousWorld[9], row.previousWorld[10] } };
			const float pivot[3]{ row.bound[0], row.bound[1], row.bound[2] };
			float turn[3][3];
			Orient(base, pivot, BillboardFaceMode(&a_node), a_camera, turn);
			// The vertex stage's (DCLFBillboardWorld): G = turn * R0^T applied to the unturned world about the pivot.
			float g[3][3];
			for (int i = 0; i < 3; ++i)
				for (int j = 0; j < 3; ++j)
					g[i][j] = turn[i][0] * base[j][0] + turn[i][1] * base[j][1] + turn[i][2] * base[j][2];
			const float offset[3]{ row.world[3] - pivot[0], row.world[7] - pivot[1], row.world[11] - pivot[2] };
			const auto& engine = a_leaf.world;
			const float scale = engine.scale != 0.0f ? engine.scale : 1.0f;
			float rotationError = 0.0f, translationError = 0.0f;
			for (int i = 0; i < 3; ++i) {
				for (int j = 0; j < 3; ++j) {
					const float port = g[i][0] * row.world[0 * 4 + j] + g[i][1] * row.world[1 * 4 + j] + g[i][2] * row.world[2 * 4 + j];
					rotationError = std::max(rotationError, std::abs(port - engine.rotate.entry[i][j] * scale) / std::abs(scale));
				}
				const float port = g[i][0] * offset[0] + g[i][1] * offset[1] + g[i][2] * offset[2] + pivot[i];
				const float translate = i == 0 ? engine.translate.x : i == 1 ? engine.translate.y : engine.translate.z;
				translationError = std::max(translationError, std::abs(port - translate));
			}
			checked.fetch_add(1, std::memory_order_relaxed);
			Largest(largestRotationBits, rotationError);
			Largest(largestTranslationBits, translationError);
			if (!(rotationError <= kRotationTolerance && translationError <= kTranslationTolerance)) {
				differ.fetch_add(1, std::memory_order_relaxed);
				std::uint32_t free = 0;
				if (firstState.compare_exchange_strong(free, 1, std::memory_order_acquire)) {
					// The node's own turn against the port's, to tell the turn from the leaf's relative transform.
					const auto& n = a_node.world.rotate.entry;
					float nodeError = 0.0f;
					for (int i = 0; i < 3; ++i)
						for (int j = 0; j < 3; ++j)
							nodeError = std::max(nodeError, std::abs(turn[i][j] - n[i][j]));
					const auto m3 = [](const float (&a_m)[3][3]) { return fmt::format("[{:.3f} {:.3f} {:.3f}; {:.3f} {:.3f} {:.3f}; {:.3f} {:.3f} {:.3f}]", a_m[0][0], a_m[0][1], a_m[0][2], a_m[1][0], a_m[1][1], a_m[1][2], a_m[2][0], a_m[2][1], a_m[2][2]); };
					float engineTurn[3][3], parentBase[3][3];
					const RE::NiMatrix3 again = a_node.parent ? a_node.parent->world.rotate * a_node.local.rotate : a_node.local.rotate;
					for (int i = 0; i < 3; ++i)
						for (int j = 0; j < 3; ++j) {
							engineTurn[i][j] = n[i][j];
							parentBase[i][j] = again.entry[i][j];
						}
					const auto written = fmt::format_to_n(first.data(), first.size() - 1,
						"'{}' under '{}' (mode {}): rotation {:.6f}, translation {:.4f}; the node's turn {:.6f}: port {}, engine {}, base {}, base now {}; camera forward "
						"{:.3f} {:.3f} {:.3f} up {:.3f} {:.3f} {:.3f} right {:.3f} {:.3f} {:.3f}",
						a_leaf.name.c_str() ? a_leaf.name.c_str() : "", a_node.name.c_str() ? a_node.name.c_str() : "", BillboardFaceMode(&a_node), rotationError, translationError,
						nodeError, m3(turn), m3(engineTurn), m3(base), m3(parentBase), a_camera.forward[0], a_camera.forward[1], a_camera.forward[2], a_camera.up[0],
						a_camera.up[1], a_camera.up[2], a_camera.right[0], a_camera.right[1], a_camera.right[2]);
					*written.out = '\0';
					firstState.store(2, std::memory_order_release);
				}
			}
		}

		void CheckSubtree(const RE::NiAVObject& a_object, const RE::NiBillboardNode& a_node, const Camera& a_camera, std::uint32_t a_depth)
		{
			if (const auto* geometry = const_cast<RE::NiAVObject&>(a_object).AsGeometry()) {
				CheckLeaf(*geometry, a_node, a_camera);
				return;
			}
			const auto* node = const_cast<RE::NiAVObject&>(a_object).AsNode();
			if (!node || a_depth > 64)
				return;
			for (const auto& child : node->GetChildren())
				if (child)
					CheckSubtree(*child, a_node, a_camera, a_depth + 1);
		}

		struct OnVisibleHook
		{
			static void thunk(RE::NiBillboardNode* a_node, RE::NiCullingProcess* a_process, std::int32_t a_alphaGroup)
			{
				// What 0x140d3f720 tests: the node is turned only for a process with camera-related updates.
				const bool turned = a_node && a_process && a_process->cameraRelatedUpdates && a_process->camera;
				func(a_node, a_process, a_alphaGroup);
				if (!turned)
					return;
				turns.fetch_add(1, std::memory_order_relaxed);
				// The culling camera as the engine's billboard code reads it (and as SetCameraData copies it into CameraView's rows).
				Camera camera;
				const auto& world = a_process->camera->world;
				for (int i = 0; i < 3; ++i) {
					camera.forward[i] = world.rotate.entry[i][0];
					camera.up[i] = world.rotate.entry[i][1];
					camera.right[i] = world.rotate.entry[i][2];
				}
				camera.eye[0] = world.translate.x;
				camera.eye[1] = world.translate.y;
				camera.eye[2] = world.translate.z;
				// The leaves below, now that the node's OnVisible has updated them (its children's UpdateDownwardPass, before it culled them).
				for (const auto& child : a_node->GetChildren())
					if (child)
						CheckSubtree(*child, *a_node, camera, 0);
			}
			static inline REL::Relocation<decltype(thunk)> func;
			static constexpr std::size_t size = kOnVisibleSlot;
		};
	}

	void InstallTurns()
	{
		if (installed || !Diagnostics::Observe().claims)
			return;
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_NiBillboardNode[0] };
		const auto* slots = reinterpret_cast<const std::uintptr_t*>(vtable.address());
		if (!kOnVisible.Known() || slots[kOnVisibleSlot] != REL::Offset(kOnVisible.Get()).address()) {
			logger::warn("[DCLF] billboards' observer: NiBillboardNode's OnVisible slot is not the expected function; not installed");
			return;
		}
		stl::write_vfunc<kOnVisibleSlot, OnVisibleHook>(RE::VTABLE_NiBillboardNode[0]);
		installed = true;
		logger::info("[DCLF] billboards' observer installed on NiBillboardNode::OnVisible (CS_DCLF_SET_PARITY)");
	}

	bool TurnsInstalled()
	{
		return installed;
	}

	TurnCounts TakeTurns()
	{
		TurnCounts c;
		c.checked = checked.exchange(0);
		c.differ = differ.exchange(0);
		c.turns = turns.exchange(0);
		c.largestRotation = std::bit_cast<float>(largestRotationBits.exchange(0));
		c.largestTranslation = std::bit_cast<float>(largestTranslationBits.exchange(0));
		if (firstState.load(std::memory_order_acquire) == 2) {
			c.first = first.data();
			firstState.store(0, std::memory_order_release);
		}
		return c;
	}
}
