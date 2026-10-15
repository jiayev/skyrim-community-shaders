#pragma once

// The material port contract's Core side (Api/ObjectContributors.h, section 4): the ports by kind, resolved once at InstallFeatures
// (ResolveMaterialPorts) from Core's Lighting (kind 0: CoreLightingPort, its space and rules alone) and the features' registrations (kinds
// from 1 in their order: Features/Effects' Effect port is 1); what each callback is handed; and the calls Core makes per event
// (CapturePort, DerivePort, EvaluatePort, SamplePortShading, PortVariants). Where Core decides it reads the table as data (PortOfRtti,
// PortOfVtable, PortOn, PortRules, MaterialSpaceOf), never calling a port per object per frame.

#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

struct ID3D11ShaderResourceView;

namespace RE
{
	class BSShaderProperty;
}

namespace DCLF
{
	// Core's records and verdicts, which the callbacks are handed (defined with the scene's capture and ports).
	struct LightingDescriptors;
	struct MaterialRecord;
	struct BindlessShading;
	enum class Ineligible : std::uint8_t;
	namespace SceneCapture
	{
		struct PropertyRecord;
		struct LeafView;
	}
	namespace MaterialPort
	{
		struct MaterialSnapshot;
		struct MaterialFrame;
	}

	/**
	 * @brief Where each variable of a shader's stage lives in a ConstantBlock (in floats) and how many floats it has (a material space's,
	 * MaterialSpaceOf; Lighting's, LightingVSLayout). Matrices take 12, the variables nothing reads share a scratch region, the rest 4.
	 */
	struct StageLayout
	{
		std::uint32_t count;
		std::array<std::uint8_t, 64> offset;
		std::array<std::uint8_t, 64> size;
	};
	/** @brief A MaterialRecord's kind (MaterialRecord::kind, LightingDescriptors::port) for Core's Lighting; a port's is its kind. */
	inline constexpr std::uint8_t kMaterialKindLighting = 0;

	/** @brief Pure: the layout of a_count variables, 4 floats each but a_sized's (variable, floats), a_scratch's in the shared region. */
	StageLayout MakeStageLayout(std::uint32_t a_count, std::span<const std::pair<std::uint32_t, std::uint32_t>> a_sized, std::span<const std::uint32_t> a_scratch);
}

namespace DCLF::Api
{
	/** @brief A port's capture: the property captured, and the record's fields its derive and shading read (PropertyRecord's). */
	class MaterialPortCapture
	{
	public:
		const RE::BSShaderProperty& property;
		const void*& emissive;
		std::uint32_t& feature;
	};

	/** @brief A port's derive: the object's records; derive writes *out and reason (None: drawn). variants reads the records alone (out null). */
	class MaterialPortDerivation
	{
	public:
		const SceneCapture::PropertyRecord& property;
		const SceneCapture::LeafView& leaf;
		LightingDescriptors* out;
		Ineligible reason;
	};

	/**
	 * @brief A port's evaluate: the material's snapshot, the pass descriptor and the frame's sources; the record it writes. The port reads
	 * and writes them through the calls below (MaterialSnapshot's reads, the vanilla frame's sources, the record's fields), never their
	 * types.
	 */
	class MaterialPortEvaluation
	{
	public:
		const MaterialPort::MaterialSnapshot& snapshot;
		std::uint32_t passDescriptor;
		const MaterialPort::MaterialFrame& frame;
		MaterialRecord& out;

		// The snapshot: its port's kind (PortOfVtable), its bytes taken, the value at a_offset (zero past them), whether the texture field
		// at a_offset holds a texture and the view resolved for it (null: none).
		std::uint8_t Port() const;
		std::uint32_t Size() const;
		template <class T>
		T At(std::size_t a_offset) const
		{
			T value{};
			Read(a_offset, &value, sizeof(T));
			return value;
		}
		bool HasTexture(std::uint16_t a_offset) const { return At<const void*>(a_offset) != nullptr; }
		ID3D11ShaderResourceView* View(std::uint16_t a_offset) const;
		// The vanilla frame's sources: whether they were sampled, and the texture-transform buffer the frame reads.
		bool FrameSampled() const;
		std::uint32_t TextureTransformBuffer() const;
		// The record: cleared to kind a_kind (constants unwritten, no textures, address modes 0, filter modes unwritten); each stage's
		// floats (StageLayout order); a pixel slot's filter mode, and its texture with its address mode (written).
		void ResetRecord(std::uint8_t a_kind);
		float* VSFloats();
		float* PSFloats();
		void SetFilterMode(std::uint32_t a_slot, std::uint32_t a_mode);
		void BindTexture(std::uint32_t a_slot, ID3D11ShaderResourceView* a_view, std::uint32_t a_addressMode);

	private:
		// a_bytes at a_offset into a_out when the snapshot has them (MaterialSnapshot::At's test), a_out left as it is otherwise.
		void Read(std::size_t a_offset, void* a_out, std::size_t a_bytes) const;
	};

	/** @brief A port's shading: the property sampled, its pass descriptor and whether it is a member's; the shading row it writes. */
	class MaterialPortShading
	{
	public:
		const RE::BSShaderProperty& property;
		std::uint32_t pass;
		bool member;
		BindlessShading& out;
	};
}

namespace DCLF::Extensions
{
	/** @brief The material kinds: 0 Core's Lighting, then a port each (the records' index: grow it before more are registered). */
	inline constexpr std::uint8_t kMaterialKinds = 2;
	/** @brief The variables a stage's space may have (StageLayout's). */
	inline constexpr std::uint32_t kMaxStageVariables = 64;

	/** @brief A stage of a resolved space: its layout, groups and first variables, and names (MaterialStageSpace's). */
	struct MaterialStageEntry
	{
		StageLayout layout{};
		std::uint64_t groups[3]{};
		std::uint32_t first[3]{};
		std::span<const std::string_view> names;
	};
	/** @brief A kind's space: both stages, and the VS floats a record takes from the frame (rules.frameVSVariable's, at most 4). */
	struct MaterialSpace
	{
		MaterialStageEntry vs, ps;
		std::vector<std::uint32_t> frameVSFloats;
	};

	/** @brief A kind's entry: its contribution's, with the RTTI, vtable and space resolved (kind 0's: Lighting's space and rules alone). */
	struct MaterialPortEntry
	{
		std::string_view name;
		std::uint8_t shaderType = 0xFF;
		Api::ObjectClass objectClass = Api::ObjectClass::Count;
		const void* rtti = nullptr;
		const void* vtable = nullptr;
		std::uint32_t materialBytes = 0;
		std::span<const std::uint16_t> textureFields;
		MaterialSpace space;
		Api::MaterialPortRules rules;
		decltype(Api::MaterialPortContribution::capture) capture = nullptr;
		decltype(Api::MaterialPortContribution::derive) derive = nullptr;
		decltype(Api::MaterialPortContribution::evaluate) evaluate = nullptr;
		decltype(Api::MaterialPortContribution::shading) shading = nullptr;
		decltype(Api::MaterialPortContribution::variants) variants = nullptr;
		std::span<const std::string_view> variantNames;
		decltype(Api::MaterialPortContribution::emissiveOf) emissiveOf = nullptr;
	};
	using MaterialPortEntries = std::array<MaterialPortEntry, kMaterialKinds>;

	/** @brief Core's Lighting as kind 0's entry: its constant space and rules, no callbacks (Scene/Ports/ConstantEvaluator.cpp). */
	const Api::MaterialPortContribution& CoreLightingPort();
	/** @brief Main thread, the end of InstallFeatures: the entries from the registrations by kind (a_byKind[0] Lighting's), a_count of them. */
	void ResolveMaterialPorts(std::span<const Api::MaterialPortContribution> a_byKind, std::size_t a_count);
	/** @brief Any thread, after InstallFeatures (all empty before): the entries, by kind. */
	const MaterialPortEntries& MaterialPorts();

	/** @brief The port whose property has a_rtti (its exact RTTI), or 0 for none. */
	inline std::uint8_t PortOfRtti(const void* a_rtti)
	{
		const auto& ports = MaterialPorts();
		for (std::uint8_t kind = 1; kind < kMaterialKinds; ++kind)
			if (a_rtti && ports[kind].rtti == a_rtti)
				return kind;
		return 0;
	}
	/** @brief The port whose material has a_vtable, or 0 for none. */
	inline std::uint8_t PortOfVtable(const void* a_vtable)
	{
		const auto& ports = MaterialPorts();
		for (std::uint8_t kind = 1; kind < kMaterialKinds; ++kind)
			if (a_vtable && ports[kind].vtable == a_vtable)
				return kind;
		return 0;
	}
	/** @brief Whether port a_kind's objects are DCLF's under a_toggles (its class on; false for 0 and past the kinds). */
	inline bool PortOn(const ToggleSet& a_toggles, std::uint8_t a_kind)
	{
		return a_kind != 0 && a_kind < kMaterialKinds && ObjectClassOn(a_toggles, MaterialPorts()[a_kind].objectClass);
	}
	/** @brief Whether port a_kind classifies a particle geometry's leaf under a_toggles (on, and rules.particleGeometry). */
	inline bool PortTakesParticles(const ToggleSet& a_toggles, std::uint8_t a_kind)
	{
		return PortOn(a_toggles, a_kind) && MaterialPorts()[a_kind].rules.particleGeometry;
	}
	/**
	 * @brief Whether a leaf of port a_kind (0: none) of geometry type a_type, skinned or not, is a shape the billboards turn under
	 * a_toggles: the parent contribution admits the turned shapes (Api::ParentLeaves::TurnedEffectShapes), its port's rules say the
	 * billboards turn its properties, and it is a skinless BSTriShape. The classification's rule (Coordinator::BillboardModeOf) and the
	 * census's; under exactly one billboard node such a leaf is DCLF's.
	 */
	inline bool BillboardTurnedLeaf(const ToggleSet& a_toggles, std::uint8_t a_kind, std::uint8_t a_type, bool a_skinned)
	{
		return ParentAdmits(a_toggles, Api::EngineParent::Billboard, Api::ParentLeaves::TurnedEffectShapes) && a_kind != 0 &&
		       MaterialPorts()[a_kind].rules.turnedByBillboards && a_type == static_cast<std::uint8_t>(RE::BSGeometry::Type::kTriShape) && !a_skinned;
	}
	/** @brief Kind a_kind's rules (0: Lighting's). */
	inline const Api::MaterialPortRules& PortRules(std::uint8_t a_kind) { return MaterialPorts()[a_kind].rules; }
	/** @brief Kind a_kind's constant space (0: Lighting's): a record's of that kind (MaterialRecord::kind), a key's of that shader. */
	inline const MaterialSpace& MaterialSpaceOf(std::uint8_t a_kind) { return MaterialPorts()[a_kind].space; }

	/** @brief Port a_kind's capture of a_property into the record's emissive and feature (CaptureProperty). */
	void CapturePort(std::uint8_t a_kind, const RE::BSShaderProperty& a_property, const void*& a_emissive, std::uint32_t& a_feature);
	/** @brief Port a_kind's derive (a_property is its, a_leaf's): a_out, and why the object stays native (None: drawn). */
	Ineligible DerivePort(std::uint8_t a_kind, const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf, LightingDescriptors& a_out);
	/** @brief Port a_kind's variant bits of a_property on a_leaf (the census; 0 without variants). */
	std::uint32_t PortVariants(std::uint8_t a_kind, const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf);
	/** @brief Port a_kind's evaluation of a_snapshot (its material's) under a_passDescriptor into a_out; false: not covered. */
	bool EvaluatePort(std::uint8_t a_kind, const MaterialPort::MaterialSnapshot& a_snapshot, std::uint32_t a_passDescriptor, const MaterialPort::MaterialFrame& a_frame,
		MaterialRecord& a_out);
	/** @brief Port a_kind's shading row of a_property (its). */
	void SamplePortShading(std::uint8_t a_kind, const RE::BSShaderProperty& a_property, std::uint32_t a_pass, bool a_member, BindlessShading& a_out);
}
