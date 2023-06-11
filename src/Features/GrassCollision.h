#pragma once

#include "Buffer.h"

class GrassCollision
{
public:
	static GrassCollision* GetSingleton()
	{
		static GrassCollision singleton;
		return &singleton;
	}

	bool enabledFeature = false;
	std::string version;

	struct Settings
	{
		std::uint32_t EnableGrassCollision = 1;
		float RadiusMultiplier = 2;
		float DisplacementMultiplier = 16;
	};

	struct alignas(16) PerFrame
	{
		DirectX::XMFLOAT3 boundCentre;
		float boundRadius;
		Settings Settings;
		float pad0;
	};

	struct CollisionSData
	{
		DirectX::XMFLOAT3 centre;
		float radius;
	};

	bool enabled = false;

	std::unique_ptr<Buffer> collisions = nullptr;

	Settings settings;

	bool updatePerFrame = false;
	ConstantBuffer* perFrame = nullptr;

	void SetupResources();
	void Reset();

	void DrawSettings();
	void UpdateCollisions();
	void ModifyGrass(const RE::BSShader* shader, const uint32_t descriptor);
	void Draw(const RE::BSShader* shader, const uint32_t descriptor);

	void Load(json& o_json);
	void Save(json& o_json);

	bool ValidateCache(CSimpleIniA& a_ini);
	void WriteDiskCacheInfo(CSimpleIniA& a_ini);
};
