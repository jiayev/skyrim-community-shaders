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
	inline std::string_view GetShaderDefineName() override { return "GRASS_COLLISION"; }

	bool HasShaderDefine(RE::BSShader::Type shaderType) override;

	struct Settings
	{
		std::uint32_t EnableGrassCollision = 1;
		float RadiusMultiplier = 2;
		float DisplacementMultiplier = 16;
		float maxDistance = 1000.0;
		std::uint32_t frameInterval = 0;
	};

	struct alignas(16) PerFrame
	{
		Vector4 boundCentre[2];
		float boundRadius;
		Settings Settings;
		float pad01[2];
	};

	struct CollisionSData
	{
		Vector3 centre[2];
		float radius;
	};

	std::unique_ptr<Buffer> collisions = nullptr;
	std::uint32_t totalActorCount = 0;
	std::uint32_t activeActorCount = 0;
	std::uint32_t currentCollisionCount = 0;
	std::vector<RE::Actor*> actorList{};
	std::vector<CollisionSData> collisionsData{};
	std::uint32_t colllisionCount = 0;

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
