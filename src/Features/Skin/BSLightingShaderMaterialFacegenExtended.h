#pragma once

class BSLightingShaderMaterialFacegenExtended : public RE::BSLightingShaderMaterialFacegen
{
public:
	// override (BSLightingShaderMaterialFacegen)
	BSShaderMaterial* Create() override;                                                                                                    // 01
	void CopyMembers(BSShaderMaterial* a_other) override;                                                                                   // 02
	std::uint32_t ComputeCRC32(uint32_t srcHash) override;                                                                                  // 04
	Feature GetFeature() const override;                                                                                                    // 06 - { return Feature::kFaceGen; }
	void OnLoadTextureSet(std::uint64_t a_arg1, BSTextureSet* a_textureSet) override;                                                       // 08
	void ClearTextures() override;                                                                                                          // 09
	void ReceiveValuesFromRootMaterial(bool a_skinned, bool a_rimLighting, bool a_softLighting, bool a_backLighting, bool a_MSN) override;  // 0A
	uint32_t GetTextures(NiSourceTexture** textures) override;                                                                              // 0B

	RE::NiPointer<RE::NiSourceTexture> extendedTexture;  // Roughness in r, wet mask in g, fuzz mask in b, specular in a
}