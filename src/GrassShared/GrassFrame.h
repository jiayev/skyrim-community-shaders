#pragma once

#include <d3d11.h>

/**
 * Per-frame state shared between DCLF's grass draws and the features that normally run inside the engine's
 * grass passes (Grass Optimizations, Grass Collision).
 *
 * While DCLF owns grass it withholds the engine's grass passes, so BSGrassShader::SetupGeometry never runs and
 * nothing hooked there updates. DCLF sets DclfOwned() each frame it owns grass and runs the stood-down
 * callbacks from its own frame point instead; those features stand down while the flag is set and publish
 * what DCLF needs (cull settings, the collision texture) here. Nothing in this header includes DCLF.
 */
namespace GrassShared
{
	/** @brief True on frames DCLF draws grass. Grass Optimizations and Grass Collision stand down while set. */
	bool DclfOwned();
	void SetDclfOwned(bool owned);

	/** @brief Registers a callback for frames DCLF owns grass. Holds at most four; register during PostPostLoad. */
	void OnStoodDownFrame(void (*callback)());

	/** @brief Runs every stood-down callback in registration order. DCLF calls it on the frames it owns grass. */
	void RunStoodDownFrame();

	/** @brief Grass Optimizations' cull settings. The defaults are the vanilla-equivalent ones used without it. */
	struct CullSettings
	{
		float MinPixelSize = 0.0f;
		float FullDetailPixelSize = 16.0f;
		float MinDensity = 1.0f;
		float MeshCostBias = 0.0f;
		float CostBiasStartDistance = 6000.0f;
		float InvisibleFadeCull = 0.0f;
		float RenderDistanceOverride = 0.0f;
		float EdgeFadeStart = 1.0f;
		bool EnableOcclusionCulling = true;
		float SimpleShadingPixelSize = 0.0f;
		float OcclusionBias = 0.001f;
		float CollisionDistance = 2048.0f;
		bool EnableMeshLOD = false;
		bool EnableMidLOD = true;
		float MidLODPixelSize = 8.0f;
		bool EnableFarLOD = true;
		float FarLODPixelSize = 4.0f;
		float MeshLODBandPixels = 3.0f;
		bool valid = false;  // true once Grass Optimizations has published
	};

	/** @brief Publishes Grass Optimizations' settings (load, defaults, every settings-panel draw). */
	void PublishSettings(const CullSettings& settings);

	/** @brief The latest published settings, or the vanilla-equivalent defaults (valid false). */
	CullSettings Settings();

	/** @brief Grass Collision's displacement texture and its per-frame constants, as RunGrass binds them at t100 and b5. */
	struct Collision
	{
		ID3D11ShaderResourceView* texture = nullptr;  // owned by Grass Collision for the session
		std::array<std::byte, 64> perFrame{};         // GrassCollision::PerFrame, zero-padded
		uint32_t frame = 0;                           // graphicsState->frameCount at the update
	};

	/** @brief Publishes the collision state of a stood-down frame. */
	void PublishCollision(const Collision& collision);

	/** @brief The latest published collision state; false when none was ever published. */
	bool LatestCollision(Collision& out);
}
