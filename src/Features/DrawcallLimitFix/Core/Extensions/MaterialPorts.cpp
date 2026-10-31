// The material ports' table and calls (see the header).

#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialPort.h"

#include <algorithm>
#include <cstring>

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
		// BSShaderMaterial's texCoordOffset/texCoordScale (two buffers of 8 bytes each): the texture transforms' fields, every kind's.
		constexpr std::uint16_t kTransformsFirst = 0xc, kTransformsEnd = 0x2c;

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
			entry.name = port.name;
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
			// The declared frame variables' floats, and the bytes the frame owns: the texture transforms' and the declared spans, merged.
			entry.frameVariables = port.frameVariables;
			entry.frameSpans = { { kTransformsFirst, kTransformsEnd } };
			for (const auto& variable : port.frameVariables) {
				const auto& layout = variable.pixel ? entry.space.ps.layout : vs;
				auto& floats = variable.pixel ? entry.space.declaredPSFloats : entry.space.declaredVSFloats;
				for (std::uint32_t c = 0; variable.variable < layout.count && c < layout.size[variable.variable] && c < 4; ++c)
					floats.push_back(layout.offset[variable.variable] + c);
				entry.frameSpans.push_back({ variable.first, variable.end });
			}
			std::sort(entry.frameSpans.begin(), entry.frameSpans.end());
			std::vector<MaterialSpan> merged;
			for (const auto& span : entry.frameSpans)
				if (!merged.empty() && span.first <= merged.back().second)
					merged.back().second = std::max(merged.back().second, span.second);
				else
					merged.push_back(span);
			entry.frameSpans = std::move(merged);
			entry.rules = port.rules;
			entry.capture = port.capture;
			entry.derive = port.derive;
			entry.evaluate = port.evaluate;
			entry.shading = port.shading;
			entry.variants = port.variants;
			entry.variantNames = port.variantNames;
			entry.emissiveOf = port.emissiveOf;
		}
	}

	const MaterialPortEntries& MaterialPorts() { return Table(); }

	bool DifferOutsideFrameSpans(std::uint8_t a_kind, const std::byte* a_left, const std::byte* a_right, std::size_t a_first, std::size_t a_end)
	{
		const auto differ = [&](std::size_t a_begin, std::size_t a_stop) {
			return a_begin < a_stop && std::memcmp(a_left + (a_begin - a_first), a_right + (a_begin - a_first), a_stop - a_begin) != 0;
		};
		std::size_t at = a_first;
		if (a_kind < kMaterialKinds)
			for (const auto& [first, end] : Table()[a_kind].frameSpans) {
				if (first >= a_end)
					break;
				if (differ(at, first))
					return true;
				at = std::max<std::size_t>(at, end);
			}
		return differ(at, a_end);
	}

	bool DifferInFrameSpans(std::uint8_t a_kind, const std::byte* a_left, const std::byte* a_right, std::size_t a_first, std::size_t a_end)
	{
		if (a_kind >= kMaterialKinds)
			return false;
		for (const auto& [first, end] : Table()[a_kind].frameSpans) {
			const std::size_t begin = std::max<std::size_t>(first, a_first), stop = std::min<std::size_t>(end, a_end);
			if (begin < stop && std::memcmp(a_left + (begin - a_first), a_right + (begin - a_first), stop - begin) != 0)
				return true;
		}
		return false;
	}

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

namespace DCLF::Api
{
	std::uint8_t MaterialPortEvaluation::Port() const { return snapshot.port; }
	std::uint32_t MaterialPortEvaluation::Size() const { return snapshot.size; }
	ID3D11ShaderResourceView* MaterialPortEvaluation::View(std::uint16_t a_offset) const { return snapshot.View(a_offset); }
	bool MaterialPortEvaluation::FrameSampled() const { return frame.vanilla.sampled; }
	std::uint32_t MaterialPortEvaluation::TextureTransformBuffer() const { return frame.vanilla.textureTransformBuffer; }

	void MaterialPortEvaluation::Read(std::size_t a_offset, void* a_out, std::size_t a_bytes) const
	{
		if (a_offset + a_bytes <= snapshot.size)
			std::memcpy(a_out, snapshot.bytes.data() + a_offset, a_bytes);
	}

	void MaterialPortEvaluation::ResetRecord(std::uint8_t a_kind)
	{
		out.vs.Reset();
		out.ps.Reset();
		out.textures.fill(nullptr);
		out.addressModes.fill(0);
		out.filterModes.fill(kUnwrittenFilterMode);
		out.textureWritten = 0;
		out.featureTextures.fill(nullptr);
		out.kind = a_kind;
	}

	float* MaterialPortEvaluation::VSFloats() { return out.vs.floats.data(); }
	float* MaterialPortEvaluation::PSFloats() { return out.ps.floats.data(); }
	void MaterialPortEvaluation::SetFilterMode(std::uint32_t a_slot, std::uint32_t a_mode) { out.filterModes[a_slot] = a_mode; }

	void MaterialPortEvaluation::BindTexture(std::uint32_t a_slot, ID3D11ShaderResourceView* a_view, std::uint32_t a_addressMode)
	{
		out.textures[a_slot] = a_view;
		out.addressModes[a_slot] = a_addressMode;
		out.textureWritten |= 1u << a_slot;
	}
}
