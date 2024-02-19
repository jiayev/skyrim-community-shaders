#pragma once

#include "Buffer.h"

class Bindings
{
public:
	static Bindings* GetSingleton()
	{
		static Bindings singleton;
		return &singleton;
	}

	bool overrideTerrain = false;

	enum class TerrainMaskMode : uint32_t
	{
		kNone,
		kWrite,
		kRead
	};

	TerrainMaskMode terrainMask = TerrainMaskMode::kNone;

	Texture2D* terrainBlendingMask;

	void DepthStencilStateSetDepthMode(RE::BSGraphics::DepthStencilDepthMode a_mode);

	void AlphaBlendStateSetMode(uint32_t a_mode);
	void AlphaBlendStateSetAlphaToCoverage(uint32_t a_value);
	void AlphaBlendStateSetWriteMode(uint32_t a_value);

	void SetOverwriteTerrainMode(bool a_enable);

	void SetOverwriteTerrainMaskingMode(TerrainMaskMode a_mode);

	void SetDirtyStates(bool a_isComputeShader);
	void SetupResources();
	void Reset();
};
