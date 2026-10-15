// Tree wind's object contributions: technique 12 (TreeAnim) DCLF's under the trees toggle, its objects' record flag, the TreeParams and
// WindTimers SetupGeometry derives for a TreeAnim property (the shading rows' wind, what a member draws with until TreeWindCS has an
// entry for its tree), and the engine globals that derivation reads, captured with the frame's.

// TreeWindScene.h first: volk must precede every Vulkan header of the translation unit (FrameGlobals, the records).
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWindScene.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"

#include <algorithm>
#include <bit>

namespace DCLF::Features::TreeWind
{
	namespace
	{
		using Engine::Global;

		// The frame globals' floats (FrameGlobals::features from slot.first): the wind source's magnitude (ShadowSceneNode +0x304), the
		// fade band, the timer scale.
		Api::FrameGlobalsSlot globalsSlot;  // install
		enum WindGlobal : std::uint32_t
		{
			kWindMagnitude,
			kWindFadeStart,
			kWindFadeEnd,
			kWindTimerScale,
			kWindGlobals,
		};

		// Render thread, the frame's start (FrameGlobals::Capture): what DeriveTreeAnim reads.
		void CaptureWindGlobals(float* a_out)
		{
			if (const auto source = Global<std::uintptr_t>(Engine::Address::kShadowSceneNode))
				a_out[kWindMagnitude] = *reinterpret_cast<const float*>(source + 0x304);
			a_out[kWindFadeStart] = Global<float>(Address::kTreeWindFadeStart);
			a_out[kWindFadeEnd] = Global<float>(Address::kTreeWindFadeEnd);
			a_out[kWindTimerScale] = Global<float>(Address::kTreeWindTimerScale);
		}

		// BSFadeNode vtable slot 0x1f8/8: the downcast to BSTreeNode, null for anything that is not one.
		constexpr std::size_t kAsTreeNodeSlot = 0x1f8 / 8;

		const void* AsTreeNode(const RE::BSFadeNode* a_fadeNode)
		{
			// The engine's "no node" sentinel (0x14332a2a0 holds a pointer to it) counts as null.
			if (!a_fadeNode || a_fadeNode == Engine::EmptyFadeNode())
				return nullptr;
			const auto* vtable = *reinterpret_cast<const std::uintptr_t* const*>(a_fadeNode);
			using Fn = const void* (*)(const RE::BSFadeNode*);
			return reinterpret_cast<Fn>(vtable[kAsTreeNodeSlot])(a_fadeNode);
		}

		// The engine's own square root, reproduced bit for bit (engine notes: Func6, case 0xc).
		float FastSqrt(float a_value)
		{
			// The engine shifts as a SIGNED integer (`sar`): this matters for a negative pattern (a fern's uninitialised distance
			// field), where the engine's result clamps to the maximum amplitude.
			const auto bits = std::bit_cast<std::int32_t>(a_value);
			const float estimate = std::bit_cast<float>(static_cast<std::uint32_t>(0x5f3759df - (bits >> 1)));
			return (1.5f - a_value * 0.5f * estimate * estimate) * estimate * a_value;
		}

		float TreeNodeFloat(const void* a_node, std::size_t a_offset)
		{
			return *reinterpret_cast<const float*>(static_cast<const std::byte*>(a_node) + a_offset);
		}

		/**
		 * @brief One object's tree-animation constants as SetupGeometry computes them (engine notes: Func6 at 1414dd040, case 0xc), for a
		 * TreeAnim property (the shading contribution). Skips its previousWindTimer = windTimer write: the native draw still does it, and
		 * twice would advance trees twice a frame.
		 */
		void DeriveTreeAnim(const Api::ShadingDerivation& a_derivation)
		{
			auto& out = a_derivation.out;
			const void* node = AsTreeNode(a_derivation.property.fadeNode);

			const auto& g = FrameGlobals::Current().features;
			const std::uint32_t at = globalsSlot.first;
			out.treeParams[0] = 0.0f;
			out.treeParams[1] = g[at + kWindMagnitude];

			// Amplitude falls off with distance. The engine takes the squared distance at +0x158 through the 0x5f3759df fast inverse
			// square root (one Newton step) times x, not sqrtf; std::sqrt differs for trees inside the fade band.
			const float distance = node ? FastSqrt(TreeNodeFloat(node, 0x158)) : 0.0f;
			const float maxAmplitude = node ? TreeNodeFloat(node, 0x15c) : 1.0f;
			const float fadeStart = g[at + kWindFadeStart];
			const float span = g[at + kWindFadeEnd] - fadeStart;
			float amplitude = (1.0f - (distance - fadeStart) / span) * maxAmplitude;
			amplitude = std::max(amplitude, 0.0f);
			amplitude = std::min(amplitude, maxAmplitude);
			out.treeParams[2] = amplitude;
			out.treeParams[3] = node ? TreeNodeFloat(node, 0x160) : 1.0f;

			const float scale = g[at + kWindTimerScale];
			out.windTimers[0] = node ? TreeNodeFloat(node, 0x164) * scale : 0.0f;
			out.windTimers[1] = node ? TreeNodeFloat(node, 0x168) * scale : 0.0f;
			// Diagnostics only (the shader reads xy): the amplitude's raw inputs, so a parity mismatch can tell node from distance.
			out.windTimers[2] = node ? TreeNodeFloat(node, 0x158) : -1.0f;
			out.windTimers[3] = node ? TreeNodeFloat(node, 0x15c) : -1.0f;
		}
	}

	void RegisterObjects(Api::InstallContext& a_context, Api::ToggleId a_trees)
	{
		// The TreeAnim technique, whose vertex animation reads TreeParams and WindTimers per object (the record's and the shading row's):
		// an object class under the trees toggle.
		a_context.RegisterTechnique({ .technique = kTechniqueTreeAnim, .toggle = a_trees, .objectFlags = kObjectTreeAnim });
		a_context.RegisterShading({ .name = "tree-anim", .propertyFlags = static_cast<std::uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kTreeAnim),
			.derive = &DeriveTreeAnim });
		globalsSlot = a_context.RegisterFrameGlobals({ .name = "tree-wind", .floats = kWindGlobals, .capture = &CaptureWindGlobals });
	}
}
