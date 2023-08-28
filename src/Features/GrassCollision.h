#pragma once

#include "Buffer.h"
#include "Feature.h"

struct GrassCollision : Feature
{
	static GrassCollision* GetSingleton()
	{
		static GrassCollision singleton;
		return &singleton;
	}

	virtual inline std::string GetName() { return "Grass Collision"; }
	virtual inline std::string GetShortName() { return "GrassCollision"; }

	struct Settings
	{
		std::uint32_t EnableGrassCollision = 1;
		float RadiusMultiplier = 2;
		float DisplacementMultiplier = 16;
	};

	struct alignas(16) PerFrame
	{
		Vector4 boundCentre[2];
		float boundRadius;
		Settings Settings;
	};

	struct CollisionSData
	{
		Vector3 centre[2];
		float radius;
	};

	std::unique_ptr<Buffer> collisions = nullptr;

	Settings settings;

	bool updatePerFrame = false;
	ConstantBuffer* perFrame = nullptr;
	int eyeCount = !REL::Module::IsVR() ? 1 : 2;

	virtual void SetupResources();
	virtual void Reset();

	virtual void DrawSettings();
	void UpdateCollisions();
	void ModifyGrass(const RE::BSShader* shader, const uint32_t descriptor);
	virtual void Draw(const RE::BSShader* shader, const uint32_t descriptor);

	virtual void Load(json& o_json);
	virtual void Save(json& o_json);
};
