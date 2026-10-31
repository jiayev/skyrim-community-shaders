// MaterialPort's snapshot capture (any thread where the material is alive), the capture queue's producer side and Evaluate's
// dispatch to the vanilla port, the material contributors' chain (Extensions: Materials.h) and the material ports
// (Extensions::MaterialPorts: the Effect port).

#include "Features/DrawcallLimitFix/Core/Shading/MaterialPort.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Materials.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace DCLF::MaterialPort
{
	bool Capture(const RE::BSShaderMaterial& a_material, MaterialSnapshot& a_out)
	{
		EngineReadWindow::Touch("MaterialPort::Capture");
		a_out.key = &a_material;
		a_out.vtable = *reinterpret_cast<const void* const*>(&a_material);
		a_out.feature = static_cast<std::uint32_t>(a_material.GetFeature());
		// A material contributor's class (TruePBR's own classes) is Lighting's, its size and texture fields the contributor's.
		a_out.materialClass = Extensions::MaterialClassOf(a_out.vtable);
		// A material port's class (its vtable, resolved at install), its size and texture fields the port's.
		a_out.port = a_out.materialClass ? std::uint8_t(0) : Extensions::PortOfVtable(a_out.vtable);
		const auto& port = Extensions::MaterialPorts()[a_out.port];
		if (a_out.port)
			a_out.feature = 0;  // no Lighting material feature (an After contributor's covers: none of Advanced Skin's)
		a_out.size = a_out.materialClass ? Extensions::MaterialClassBytes(a_out.materialClass, a_material) : a_out.port ? port.materialBytes : VanillaClassSize(a_material);
		a_out.textureCount = 0;
		if (!a_out.size || a_out.size > kMaxMaterialBytes)
			return false;
		std::memcpy(a_out.bytes.data(), &a_material, a_out.size);
		std::array<std::uint16_t, kMaxTextureFields> fields{};
		std::uint32_t count = 0;
		if (a_out.materialClass)
			Extensions::MaterialClassTextureFields(a_out.materialClass, a_material, fields.data(), count);
		else if (a_out.port)
			for (std::size_t i = 0; i < port.textureFields.size() && count < kMaxTextureFields; ++i)
				fields[count++] = port.textureFields[i];
		else
			VanillaTextureFields(a_material, fields.data(), count);
		for (std::uint32_t i = 0; i < count && i < kMaxTextureFields; ++i)
			a_out.textures[a_out.textureCount++] = { fields[i], ViewOf(a_out.At<const void*>(fields[i])) };
		return true;
	}

	void PushCapture(const RE::BSShaderMaterial* a_material)
	{
		if (!a_material)
			return;
		// BSIntrusiveRefCounted's count (+0x8), taken only from an owned material: one being built (its count 0, a writer's
		// CopyMembers before the material manager holds it) is not captured here; its attach or a request captures it.
		auto* count = reinterpret_cast<volatile LONG*>(reinterpret_cast<std::byte*>(const_cast<RE::BSShaderMaterial*>(a_material)) + 0x8);
		for (LONG now = *count;;) {
			if (now <= 0)
				return;
			const LONG seen = InterlockedCompareExchange(count, now + 1, now);
			if (seen == now)
				break;
			now = seen;
		}
		auto held = std::make_unique<HeldSnapshot>();
		held->reference = const_cast<RE::BSShaderMaterial*>(a_material);
		held->sequence = SceneCapture::NextSequence();
		Capture(*a_material, held->snapshot);  // uncovered (no class known): the scene work keeps the material native, by its reason
		for (std::uint32_t i = 0; i < held->snapshot.textureCount; ++i)
			held->views[i].copy_from(held->snapshot.textures[i].view);
		captures.Push(std::move(held));
	}

	SnapshotChange SnapshotChangeOf(const MaterialSnapshot& a_held, const MaterialSnapshot& a_new)
	{
		// BSIntrusiveRefCounted's count (+0x8, 4 bytes), then the fields: the bytes a frame patch owns (Extensions' frame spans: BSShaderMaterial's
		// texture-transform buffers, +0xc..+0x2c, and the port's declared frame variables') apart from the rest.
		constexpr std::size_t kCount = 0x8, kFields = 0xc;
		if (a_held.key != a_new.key || a_held.vtable != a_new.vtable || a_held.feature != a_new.feature || a_held.materialClass != a_new.materialClass ||
			a_held.port != a_new.port || a_held.size != a_new.size || a_held.textureCount != a_new.textureCount)
			return SnapshotChange::Fields;
		for (std::uint32_t i = 0; i < a_new.textureCount && i < kMaxTextureFields; ++i)
			if (a_held.textures[i].offset != a_new.textures[i].offset || a_held.textures[i].view != a_new.textures[i].view)
				return SnapshotChange::Fields;
		const std::size_t size = std::min<std::size_t>(a_new.size, kMaxMaterialBytes);
		if (std::memcmp(a_held.bytes.data(), a_new.bytes.data(), std::min(kCount, size)) != 0)
			return SnapshotChange::Fields;
		if (size <= kFields)
			return SnapshotChange::None;
		const auto* held = a_held.bytes.data() + kFields;
		const auto* now = a_new.bytes.data() + kFields;
		if (Extensions::DifferOutsideFrameSpans(a_new.port, held, now, kFields, size))
			return SnapshotChange::Fields;
		return Extensions::DifferInFrameSpans(a_new.port, held, now, kFields, size) ? SnapshotChange::Transforms : SnapshotChange::None;
	}

	FrameComponentsWitness WitnessOf(const VanillaFrame& a_vanilla, const HookFrame& a_hook, const Extensions::MaterialContributorSamples& a_contributors)
	{
		// Each value as the port reads it: a byte tested for non-zero as a bit, a float by its bits, and a value the port writes only
		// under a flag (the character light's params, PS 6) zero while the flag is off. The character light's target is its class (none,
		// in the array, outside it: unbound, bound, a failed evaluation for vanilla), not its index, which alternates frame to frame.
		FrameComponentsWitness w;
		std::size_t at = 0;
		auto put = [&](std::uint32_t a_word) { w.words[at++] = a_word; };
		auto bits = [](float a_value) { return std::bit_cast<std::uint32_t>(a_value); };
		auto targetClass = [](std::int32_t a_target, std::uint32_t a_count) { return a_target < 0 ? 0u : static_cast<std::uint32_t>(a_target) < a_count ? 1u : 2u; };
		const auto& v = a_vanilla;
		put(std::uint32_t(v.sampled) | std::uint32_t(v.shaderKnown) << 1 | std::uint32_t(v.lodTexParamsZ != 0) << 2 | std::uint32_t(v.landSnowZ != 0) << 3 |
			std::uint32_t(v.ambientSpecularWritten) << 4 | std::uint32_t(v.snowRimW != 0) << 5 | std::uint32_t(v.fullBright != 0) << 6 | std::uint32_t(v.plainSnowSparkle) << 7 |
			targetClass(v.characterLightTarget, VanillaRenderTargets()) << 8 | std::uint32_t(v.techniqueByte12) << 16 | std::uint32_t(v.techniqueByte7) << 24);
		put(static_cast<std::uint32_t>(v.landSnowDivisor));
		for (const float c : v.ambientSpecular)
			put(v.ambientSpecularWritten ? bits(c) : 0u);
		for (const float c : v.snowRim)
			put(bits(c));
		for (const float c : v.characterLight)
			put(v.fullBright ? bits(c) : 0u);
		const auto& h = a_hook;
		put(std::uint32_t(h.characterLightEnabled) << 2 | targetClass(h.characterLightTarget, kHookRenderTargets) << 4 | std::uint32_t(h.techniqueByte12) << 16 |
			std::uint32_t(h.techniqueByte7) << 24);
		for (const float c : h.characterLightParams)
			put(h.characterLightEnabled ? bits(c) : 0u);
		// Which contributors run (TruePBR's and Advanced Skin's loaded flags), then their own words: Advanced Skin's keys (its covers) are
		// its table's identity, shared between frames while unchanged.
		put(a_contributors.ActiveBits());
		for (const std::uint32_t word : a_contributors.words)
			put(word);
		return w;
	}

	MaterialFrame SampleFrame()
	{
		MaterialFrame frame{ SampleVanillaFrame(), SampleHookFrame() };
		// The contributors after the hook frame (Advanced Skin's table resolves against its default black view).
		Extensions::SampleMaterialContributors(frame.hook, frame.contributors);
		frame.components = WitnessOf(frame.vanilla, frame.hook, frame.contributors);
		return frame;
	}

	bool Evaluate(const MaterialSnapshot& a_snapshot, std::uint32_t a_passDescriptor, const MaterialFrame& a_frame, MaterialRecord& a_out)
	{
		// A material port's material is its shader's (the Effect port's: BSEffectShader's, no Community Shaders hook on its SetupMaterial).
		if (a_snapshot.port)
			return Extensions::EvaluatePort(a_snapshot.port, a_snapshot, a_passDescriptor, a_frame, a_out);
		// The hooks' chain (installed in feature order): the After contributors (Advanced Skin) around the Replace (TruePBR) around
		// vanilla. A Replace takes the passes it takes (and hands back to vanilla what it does not draw); an After contributor whose
		// input is not set up yet makes the material uncovered for now (Retry).
		const Extensions::MaterialContributorInputs inputs{ a_snapshot, a_snapshot.materialClass, a_passDescriptor, a_frame.hook, a_frame.contributors };
		const auto outcome = Extensions::EvaluateReplace(inputs, a_out);
		const bool covered = outcome == Api::MaterialOutcome::Made ||
		                     (outcome == Api::MaterialOutcome::HandBack && EvaluateVanilla(a_snapshot, a_passDescriptor, a_frame.vanilla, a_out));
		if (!covered || Extensions::MaterialRetryable(inputs))
			return false;
		Extensions::EvaluateAfter(inputs, a_out);
		return true;
	}
}
