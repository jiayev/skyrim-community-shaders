#pragma once

#include <shared_mutex>

#include <DirectXMath.h>
#include <d3d11.h>

#include "Buffer.h"

// Cat: since this is WIP I'm leaving it as is

class Clustered
{
public:
	static Clustered* GetSingleton()
	{
		static Clustered render;
		return &render;
	}

	struct LightSData
	{
		DirectX::XMVECTOR color;
		DirectX::XMVECTOR positionWS;
		DirectX::XMVECTOR positionVS;
		float radius;
		bool shadow;
		float mask;
		bool active;
	};

	bool rendered = false;

	std::unique_ptr<Buffer> lights = nullptr;

	void Reset();
	void UpdateLights();
	void Bind(bool a_update);
};
