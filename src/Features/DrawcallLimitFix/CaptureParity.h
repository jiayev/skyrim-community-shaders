#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "ConstantEvaluator.h"

namespace DCLF
{
	/**
	 * @brief compares what the native main pass actually
	 * draws with what the CPU tables say Drawcall Limit Fix would draw.
	 *
	 * Called after the Lighting shader's SetupGeometry for every native lighting draw of the main
	 * (deferred) pass. For each draw of a geometry in the tables it checks the shader descriptors, the
	 * world transform, the PerMaterial and PerGeometry constants, and the material's textures and
	 * address modes. For each draw of a statically eligible geometry under a drawn category node that
	 * is not tracked, it counts a tracking miss.
	 */
	class CaptureParity
	{
	public:
		static bool Enabled();
		static CaptureParity& Get();

		void OnNativeLightingDraw(const RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags);

		/** @brief Map/Unmap detours: keep a copy of the native constants as the engine unmaps them. */
		void OnMap(ID3D11Resource* a_resource, void* a_data);
		void OnUnmap(ID3D11Resource* a_resource);

		/** @brief DrawIndexed detour: checks the draw that follows a checked SetupGeometry. */
		void OnDrawIndexed(ID3D11DeviceContext* a_context, UINT a_indexCount, UINT a_startIndex, INT a_baseVertex);

		/** @brief Logs and resets the counters every a_interval frames (called at the start of the main pass). */
		void Report(std::uint32_t a_frame, std::uint32_t a_interval);

	private:
		struct ConstantSnapshot
		{
			ID3D11Resource* buffer = nullptr;
			void* mapped = nullptr;
			void* lastMapped = nullptr;  // still valid after Unmap: DXVK keeps dynamic buffers persistently mapped
			std::uint32_t size = 0;
			std::array<std::uint8_t, 1024> bytes{};
			bool valid = false;
		};

		// Snapshot slots: [stage][level], stage 0 = VS, 1 = PS; levels PerTechnique (0), PerMaterial (1) and PerGeometry (2).
		ConstantSnapshot& Slot(std::uint32_t a_stage, std::uint32_t a_level) { return snapshots[a_stage][a_level]; }

		void NoteMismatch(std::string a_message);
		static void Snapshot(ConstantSnapshot& a_snapshot, ID3D11Resource* a_resource);
		bool CompareBlock(const RE::BSGeometry* a_geometry, const char* a_what, const ConstantBlock& a_expected, const StageLayout& a_layout,
			const std::uint8_t* a_nativeTable, std::size_t a_nativeTableSize, const ConstantSnapshot& a_native, ID3D11Resource* a_boundBuffer,
			std::uint32_t a_firstVariable, std::uint64_t a_variables, std::uint64_t a_tolerant);
		bool CompareMaterial(const RE::BSGeometry* a_geometry, std::uint32_t a_materialIndex);
		bool CompareGeometry(const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex, std::uint32_t a_renderFlags);
		bool CompareTechnique(const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex);
		void ComparePermutation(const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex);

		std::array<std::array<ConstantSnapshot, 3>, 2> snapshots;

		void InstallDrawHook();
		bool drawHookInstalled = false;
		std::int32_t pendingObject = -1;  // object whose SetupGeometry just ran; its draw is next
		// The geometry slots the pending object's draws should bind, in order: one, or for a skin of several
		// partitions one per partition DCLF draws (the engine issues a DrawIndexed for each).
		std::array<std::uint32_t, 8> pendingSlots{};
		std::uint32_t pendingSlotCount = 0;
		std::uint32_t pendingSlotCursor = 0;

		// Render state the native draw used, per property-derived key, to learn (and then encode) the mapping.
		struct RenderStateKey
		{
			bool twoSided, zTest, zWrite, alphaTest;
			std::uint8_t alphaThreshold;
			bool operator==(const RenderStateKey&) const = default;
		};
		struct RenderStateKeyHash
		{
			std::size_t operator()(const RenderStateKey& k) const noexcept
			{
				return (std::size_t(k.twoSided) << 0) | (std::size_t(k.zTest) << 1) | (std::size_t(k.zWrite) << 2) | (std::size_t(k.alphaTest) << 3) |
				       (std::size_t(k.alphaThreshold) << 4);
			}
		};
		struct RenderStateValue
		{
			std::uint32_t cull, depth, stencil, blend, depthBias;
			bool alphaTestEnabled;
			float alphaTestRef;
			bool operator==(const RenderStateValue&) const = default;
		};
		ankerl::unordered_dense::map<RenderStateKey, std::vector<RenderStateValue>, RenderStateKeyHash> renderStates;
		// How often each constant block was actually compared against a native buffer, and how often there
		// was no snapshot to compare with (which is not a passing check).
		ankerl::unordered_dense::map<std::string, std::pair<std::uint64_t, std::uint64_t>> blockCoverage;
		std::uint64_t drawMismatches = 0;  // draw arguments or bound buffers differ
		std::uint64_t drawsChecked = 0;
		// CS_DCLF_SKINNED: the bone palettes the native draw bound at VS b9/b10 against the rows the tables
		// copied from the skin instance. The buffers are the engine's dynamic ring, so their contents are
		// read through the pointer their last Map returned, which DXVK keeps valid until the next Map.
		std::uint32_t treeSamples = 0;  // tree amplitude diagnostics logged this session
		std::uint64_t boneChecks = 0;
		std::uint64_t boneMismatches = 0;
		std::map<ID3D11Resource*, void*> lastMappedAny;

		std::uint64_t nativeDraws = 0;         // native lighting draws in the main pass
		std::uint64_t checkedDraws = 0;        // ... of geometry in the DCLF tables
		std::uint64_t mismatchedDraws = 0;     // ... whose state differs
		std::uint64_t materialMismatches = 0;  // ... in PerMaterial constants or textures
		// Each material a draw mismatched on, followed into the following frames' write drains: whether an
		// event for it arrived after the drain of the frame it mismatched in.
		struct MaterialMismatch
		{
			std::uint32_t firstFrame = 0;
			std::uint32_t lastFrame = 0;
			std::uint32_t frames = 0;
			bool keyDiffers = false;  // the record's material is not the drawn pass's
			bool writtenBefore = false;  // this frame's drain (before the draw) held it
		};
		ankerl::unordered_dense::map<const RE::BSShaderMaterial*, MaterialMismatch> materialMismatchFollow;
		std::map<std::string, std::uint32_t> materialMismatchResolved;
		std::uint64_t geometryMismatches = 0;  // ... in PerGeometry constants
		std::uint64_t techniqueMismatches = 0; // ... in PerTechnique constants or filter modes
		std::uint64_t inheritedFilters = 0;    // bound material textures whose filter mode neither SetupTechnique nor SetupMaterial sets
		std::uint64_t lightChecks = 0;
		std::uint64_t lightMismatches = 0;  // StrictLightData (LLF, PS b3) differs
		// Advanced Skin: its per-material textures (PS t71, t74) against the material record, its wetness (PS b7)
		// against the tables, and whether the object's actor ownership (SceneStore::Tracked::actorOwned) still holds.
		std::uint64_t skinTextureChecks = 0;
		std::uint64_t skinTextureMismatches = 0;
		std::uint64_t skinWetnessChecks = 0;
		std::uint64_t skinWetnessMismatches = 0;
		std::uint64_t skinWetDraws = 0;  // checked draws with sweat or water wetness (x or y)
		std::uint64_t skinOwnerMismatches = 0;
		std::uint64_t permutationChecks = 0;
		std::uint64_t permutationMismatches = 0;
		// Bindings Community Shaders owns (constant buffers b3 and up, shader resources outside the
		// engine's material and technique slots): they must be the same for every eligible draw of a
		// frame, so that DCLF can bind them once per pass. Baseline = the frame's first eligible draw.
		void CompareFeatureBindings(ID3D11DeviceContext* a_context);
		void CompareSkin(ID3D11DeviceContext* a_context, const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex);
		static constexpr std::uint32_t kFirstFeatureConstantBuffer = 3;
		static constexpr std::uint32_t kConstantBufferSlots = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
		static constexpr std::uint32_t kResourceSlots = 128;
		struct FeatureBindings
		{
			std::array<ID3D11Buffer*, kConstantBufferSlots> vsBuffers{}, psBuffers{};
			std::array<ID3D11ShaderResourceView*, kResourceSlots> vsResources{}, psResources{};
		};
		FeatureBindings baseline;
		bool baselineValid = false;
		// Baseline constant buffers mapped since the last eligible draw (bit = slot; VS in 0-15, PS in 16-31),
		// counted as rewritten only when another eligible draw of the frame follows.
		std::uint32_t pendingRewrites = 0;
		// Differences from the baseline and rewrites of a baseline constant buffer, keyed by "<what> <slot>".
		std::map<std::string, std::uint64_t> bindingChanges;

		// Diagnostics: statically eligible geometry drawn natively outside the tracked category nodes.
		std::uint64_t outsideCategories = 0;
		std::set<std::string> outsideChains;

		// Variables the native shader has in a group that DCLF leaves entirely unwritten, as "<group> <variable>".
		std::map<std::string, std::uint64_t> unevaluated;
		// Mismatches per block and variable. The sample list is capped and a standing difference fills
		// it, so a count per variable is what actually says where a new object class is going wrong.
		std::map<std::string, std::uint32_t> mismatchByVariable;
		// Differing permutation buffer bits: key = field index << 32 | differing bits.
		ankerl::unordered_dense::map<std::uint64_t, std::uint64_t> permutationDiffs;
		std::uint64_t untrackedEligible = 0;   // eligible geometry under a drawn category node, not tracked
		// Each untracked eligible geometry followed until it is tracked (identity only, never dereferenced
		// after its draw): how many frames it was drawn untracked, and what tracked it in the end.
		struct Untracked
		{
			std::uint32_t firstFrame = 0;
			std::uint32_t lastFrame = 0;
			std::uint32_t frames = 0;
			std::string chain;  // its parents at first sight, with the category node's discovery
		};
		ankerl::unordered_dense::map<const RE::BSGeometry*, Untracked> untracked;
		void ResolveUntracked(std::uint32_t a_frame);
		std::map<std::string, std::uint32_t> untrackedResolved;  // "<source> after <n> frames" -> geometries
		std::vector<std::string> untrackedStuck;                 // chains of those still untracked after kStuckFrames
		std::uint64_t notInTables = 0;         // tracked geometry drawn natively but excluded this frame
		std::uint64_t nativeOnlyPasses = 0;    // native passes of objects DCLF draws that DCLF does not model (hint 10)
		ankerl::unordered_dense::set<std::uint32_t> renderFlagsSeen;
		std::vector<std::string> samples;  // first few mismatch descriptions per report
	};
}
