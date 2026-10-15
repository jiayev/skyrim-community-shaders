// The material ports' table and calls (see the header).

#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"

#include <algorithm>

namespace DCLF
{
	StageLayout MakeStageLayout(std::uint32_t a_count, std::span<const std::pair<std::uint32_t, std::uint32_t>> a_sized, std::span<const std::uint32_t> a_scratch)
	{
		StageLayout layout{};
		layout.count = a_count;
		layout.size.fill(4);
		for (auto [variable, size] : a_sized)
			layout.size[variable] = static_cast<std::uint8_t>(size);
		// Variables nothing reads (the per-geometry point lights under Light Limit Fix) share one region at the end.
		constexpr std::uint32_t kScratchRegion = 228;
		std::uint32_t next = 0;
		for (std::uint32_t i = 0; i < a_count; ++i) {
			if (std::find(a_scratch.begin(), a_scratch.end(), i) != a_scratch.end()) {
				layout.offset[i] = static_cast<std::uint8_t>(kScratchRegion);
				continue;
			}
			layout.offset[i] = static_cast<std::uint8_t>(next);
			next += layout.size[i];
		}
		return layout;
	}
}

namespace DCLF::Extensions
{
	namespace
	{
		// Written only by ResolveMaterialPorts (InstallFeatures, before DCLF's threads exist), read-only after.
		MaterialPortEntries& Table()
		{
			static MaterialPortEntries table;
			return table;
		}

		MaterialStageEntry StageOf(const Api::MaterialStageSpace& a_space)
		{
			MaterialStageEntry stage;
			stage.layout = MakeStageLayout(a_space.variables, a_space.sized, a_space.scratch);
			std::copy(std::begin(a_space.groups), std::end(a_space.groups), std::begin(stage.groups));
			std::copy(std::begin(a_space.first), std::end(a_space.first), std::begin(stage.first));
			stage.names = a_space.names;
			return stage;
		}
	}

	void ResolveMaterialPorts(std::span<const Api::MaterialPortContribution> a_byKind, std::size_t a_count)
	{
		auto& table = Table();
		for (std::size_t kind = 0; kind < a_count && kind < table.size(); ++kind) {
			const auto& port = a_byKind[kind];
			auto& entry = table[kind];
			entry.shaderType = port.shaderType;
			entry.objectClass = port.objectClass;
			// The engine's addresses, read once here (REL), never per capture.
			entry.rtti = port.propertyRtti ? port.propertyRtti() : nullptr;
			entry.vtable = port.materialVtable ? port.materialVtable() : nullptr;
			entry.materialBytes = port.materialBytes;
			entry.textureFields = port.textureFields;
			entry.space.vs = StageOf(port.vs);
			entry.space.ps = StageOf(port.ps);
			// The frame's VS floats: the variable's components, at most 4 (MaterialSources' TexcoordOffset positions).
			const auto& vs = entry.space.vs.layout;
			if (const std::uint32_t variable = port.rules.frameVSVariable; variable < vs.count)
				for (std::uint32_t c = 0; c < vs.size[variable] && c < 4; ++c)
					entry.space.frameVSFloats.push_back(vs.offset[variable] + c);
			entry.rules = port.rules;
			entry.capture = port.capture;
			entry.derive = port.derive;
			entry.evaluate = port.evaluate;
			entry.shading = port.shading;
			entry.variants = port.variants;
			entry.variantNames = port.variantNames;
		}
	}

	const MaterialPortEntries& MaterialPorts() { return Table(); }

	void CapturePort(std::uint8_t a_kind, const RE::BSShaderProperty& a_property, const void*& a_emissive, std::uint32_t& a_feature)
	{
		Table()[a_kind].capture({ a_property, a_emissive, a_feature });
	}

	Ineligible DerivePort(std::uint8_t a_kind, const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf, LightingDescriptors& a_out)
	{
		Api::MaterialPortDerivation derivation{ a_property, a_leaf, &a_out, Ineligible{} };
		Table()[a_kind].derive(derivation);
		return derivation.reason;
	}

	std::uint32_t PortVariants(std::uint8_t a_kind, const SceneCapture::PropertyRecord& a_property, const SceneCapture::LeafView& a_leaf)
	{
		const auto variants = Table()[a_kind].variants;
		return variants ? variants({ a_property, a_leaf, nullptr, Ineligible{} }) : 0u;
	}

	bool EvaluatePort(std::uint8_t a_kind, const MaterialPort::MaterialSnapshot& a_snapshot, std::uint32_t a_passDescriptor, const MaterialPort::MaterialFrame& a_frame,
		MaterialRecord& a_out)
	{
		Api::MaterialPortEvaluation evaluation{ a_snapshot, a_passDescriptor, a_frame, a_out };
		return Table()[a_kind].evaluate(evaluation);
	}

	void SamplePortShading(std::uint8_t a_kind, const RE::BSShaderProperty& a_property, std::uint32_t a_pass, bool a_member, BindlessShading& a_out)
	{
		Api::MaterialPortShading shading{ a_property, a_pass, a_member, a_out };
		Table()[a_kind].shading(shading);
	}
}
