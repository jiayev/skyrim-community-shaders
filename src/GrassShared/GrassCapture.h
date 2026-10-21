#pragma once

#include <d3d11.h>

/**
 * Grass instance capture shared by Grass Optimizations and Drawcall Limit Fix.
 *
 * The cell-load hooks that see each grass shape's raw instance records live here, so either consumer, or
 * both, can be loaded without the other. Each consumer registers a Sink and calls InstallCapture(); the
 * hooks are installed once, and every capture, removal and rejection is handed to every sink in order.
 * Neither consumer includes the other, and DCLF includes nothing of TruePBR's: the PBR material fields are
 * read here, at capture.
 */
namespace GrassShared
{
	/** @brief Byte stride of the per-instance records. */
	inline constexpr uint32_t kGrassStride = 32;

	/** @brief One captured grass shape. Valid only during the sink call; sinks copy what they keep. */
	struct AddEvent
	{
		// Global and atomic, so a shape's add and remove keep their order across the loader and dtor threads.
		uint64_t seq = 0;
		RE::BSMultiStreamInstanceTriShape* shape = nullptr;
		const uint8_t* records = nullptr;  // count * kGrassStride bytes of half-packed engine records
		uint32_t count = 0;

		uint64_t vertexDesc = 0;
		RE::NiPoint3 origin;  // shape->world.translate at capture

		RE::BSGrassShaderProperty* property = nullptr;
		RE::BSShaderMaterial* material = nullptr;
		RE::NiSourceTexture* diffuse = nullptr;

		// The source mesh: its renderer data, which identifies the mesh, and the buffers it owns.
		const void* meshKey = nullptr;
		ID3D11Buffer* vertexBuffer = nullptr;
		ID3D11Buffer* indexBuffer = nullptr;
		uint32_t triangleCount = 0;

		RE::NiBound modelBound{};
		float wavePeriod = 1.0f;
		uint32_t clampMode = 3;     // the material's textureClampMode
		bool pbr = false;           // property flag kMenuScreen: TruePBR grass, colour technique TruePbr
		bool sphereNormal = false;  // property flag kEffectLighting: GrassSphereNormal descriptor
		bool twoSided = false;      // property flag kTwoSided
		bool uniformScale = false;  // property flag kUniformScale: the vertex stage's ScaleMask is (1, 1, 1), else (0, 0, 1)
		// The engine's alpha reference for the shape: (int)(NiAlphaProperty::alphaThreshold * property alpha) / 255 (0 without an
		// alpha property).
		float alphaRef = 0.0f;

		/** @brief TruePBR's grass material fields, read only when pbr is set. */
		struct Pbr
		{
			RE::NiSourceTexture* normal = nullptr;
			RE::NiSourceTexture* rmaos = nullptr;
			RE::NiSourceTexture* features0 = nullptr;  // null when absent or the default white
			uint32_t flags = 0;                        // PBRShaderFlags, as BSGrassShader_SetupMaterial uploads them
			float roughnessScale = 1.0f;
			float specularLevel = 0.0f;
			float subsurface[4]{};  // colour rgb, opacity
		} pbrData;

		enum class Source : uint8_t
		{
			Runtime,    // DoneAddingInstances
			GidBuffer,  // AddGroupGIDBuffer / AddQueuedGroupGIDBuffer
			GidFile     // AddGroupGIDFile / AddGroupQueuedGIDFile (.cgid)
		} source = Source::Runtime;
	};

	/**
	 * @brief A capture consumer. Every callback runs on the hook's thread (cell loaders, the dtor's caller),
	 * so a sink only stages; any member may be null.
	 */
	struct Sink
	{
		void (*add)(const AddEvent&) = nullptr;
		// Every BSMultiStreamInstanceTriShape destruction, grass or not, captured or not.
		void (*remove)(RE::BSMultiStreamInstanceTriShape*, uint64_t seq) = nullptr;
		// A grass shape whose records could not be captured (stride != kGrassStride, no texture or material).
		// The engine still draws it, so a consumer that hides grass wholesale must not.
		void (*rejected)(RE::BSMultiStreamInstanceTriShape*, uint64_t seq) = nullptr;
	};

	/** @brief Registers a sink, before InstallCapture. Holds at most two (Grass Optimizations, DCLF). */
	void AddSink(const Sink& sink);

	/** @brief Installs the capture hooks once; later calls do nothing. */
	void InstallCapture();

	/**
	 * @brief The engine's grass shader, from the first grass render pass seen by
	 * BSGrassShaderProperty::GetRenderPasses (vfunc 0x2A, chained with TruePBR's hook of the same slot).
	 * Null until a grass shape has been accumulated once.
	 */
	RE::BSShader* GrassShader();
}
