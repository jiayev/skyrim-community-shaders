#include "MaterialPort.h"

#include "Features/DrawcallLimitFix/Engine/EngineReadWindow.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace DCLF::MaterialPort
{
	namespace
	{
		/** @brief Whether a vtable is this DLL's: CS's own material classes (BSLightingShaderMaterialPBR, ...PBRLandscape). */
		bool OwnClass(const void* a_vtable)
		{
			static const auto [begin, end] = [] {
				const auto base = reinterpret_cast<std::uintptr_t>(&__ImageBase);
				const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + __ImageBase.e_lfanew);
				return std::pair{ base, base + nt->OptionalHeader.SizeOfImage };
			}();
			const auto address = reinterpret_cast<std::uintptr_t>(a_vtable);
			return address >= begin && address < end;
		}
	}

	bool Capture(const RE::BSShaderMaterial& a_material, MaterialSnapshot& a_out)
	{
		EngineReadWindow::Touch("MaterialPort::Capture");
		a_out.key = &a_material;
		a_out.vtable = *reinterpret_cast<const void* const*>(&a_material);
		a_out.feature = static_cast<std::uint32_t>(a_material.GetFeature());
		a_out.pbr = OwnClass(a_out.vtable);
		a_out.size = a_out.pbr ? PBRMaterialBytes(a_material) : VanillaClassSize(a_material);
		a_out.textureCount = 0;
		if (!a_out.size || a_out.size > kMaxMaterialBytes)
			return false;
		std::memcpy(a_out.bytes.data(), &a_material, a_out.size);
		std::array<std::uint16_t, kMaxTextureFields> fields{};
		std::uint32_t count = 0;
		if (a_out.pbr)
			PBRTextureFields(a_material, fields.data(), count);
		else
			VanillaTextureFields(a_material, fields.data(), count);
		for (std::uint32_t i = 0; i < count && i < kMaxTextureFields; ++i)
			a_out.textures[a_out.textureCount++] = { fields[i], ViewOf(a_out.At<const void*>(fields[i])) };
		return true;
	}

	MaterialFrame SampleFrame()
	{
		return { SampleVanillaFrame(), SampleFeatureFrame() };
	}

	bool Evaluate(const MaterialSnapshot& a_snapshot, std::uint32_t a_passDescriptor, const MaterialFrame& a_frame, MaterialRecord& a_out)
	{
		// The hooks' chain (installed in feature order): Advanced Skin and TerrainHelper around TruePBR around vanilla. TruePBR's
		// replacement takes the passes it takes (and hands back to vanilla what it does not draw: EvaluatePBR).
		const bool covered = PBRTakesPass(a_passDescriptor, a_frame.feature) ? EvaluatePBR(a_snapshot, a_passDescriptor, a_frame, a_out) :
		                                                                        EvaluateVanilla(a_snapshot, a_passDescriptor, a_frame.vanilla, a_out);
		if (!covered || !FeatureHooksCovered(a_snapshot, a_frame.feature))
			return false;
		EvaluateFeatureHooks(a_snapshot, a_passDescriptor, a_frame.feature, a_out);
		return true;
	}
}
