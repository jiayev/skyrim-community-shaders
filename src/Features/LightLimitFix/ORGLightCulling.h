#pragma once

#include <array>
#include <cstdint>
#include <d3d11.h>
#include <memory>

/**
 * @brief Light Limit Fix's clustered light culling as OpenRenderGraph passes.
 *
 * Runs LLF's own cluster-building and culling shaders (compiled to SPIR-V with
 * LLF_ORG_BINDLESS) on DXVK's Vulkan device through RenderGraphRuntime, and
 * produces the same lightIndexList / lightGrid layout the D3D11 path does. The
 * outputs are graph-owned buffers that D3D11 reads through wrappers, so pixel
 * shaders bind them at t36/t37 exactly as before.
 *
 * Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
class ORGLightCulling
{
public:
	/** @brief Per-frame inputs, copied at Execute; nothing is retained by reference. */
	struct FrameInputs
	{
		const void* lights = nullptr;  // LightLimitFix::LightData[lightCount]
		uint32_t lightCount = 0;
		uint32_t clusterSize[3]{};
		float lightsNear = 0.0f;
		float lightsFar = 0.0f;
		std::array<float, 16> cameraProjInverse{};  // row-major, as in FrameBuffer
		std::array<float, 16> cameraView{};         // row-major, as in FrameBuffer
	};

	static ORGLightCulling& Get();

	/**
	 * @brief (Re)creates the graph resources and registers the culling passes.
	 * @return Whether culling runs on the render graph; false keeps LLF on D3D11.
	 */
	bool Setup(uint32_t a_clusterCount, uint32_t a_maxLights, uint32_t a_lightStride, uint32_t a_maxLightsPerCluster);

	/** @brief Uploads the lights and runs one graph epoch. False means use the D3D11 path this frame. */
	bool Execute(const FrameInputs& a_inputs);

	bool IsActive() const;

	ID3D11ShaderResourceView* GetLightIndexListSRV() const;
	ID3D11ShaderResourceView* GetLightGridSRV() const;

	/**
	 * @brief Shader-visible descriptor heap indices of the graph's lights, light index list and light grid
	 * (Lighting's t35, t36, t37), for passes on the graph that read them directly. False when inactive.
	 */
	bool GetShaderResourceIndices(uint32_t& a_lights, uint32_t& a_lightIndexList, uint32_t& a_lightGrid) const;

	~ORGLightCulling();

private:
	ORGLightCulling() = default;

	struct Impl;
	std::unique_ptr<Impl> impl;
};
