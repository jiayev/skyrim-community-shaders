#pragma once

#include <d3d11.h>

/**
 * Grass instance capture shared by Grass Optimizations and Drawcall Limit Fix.
 *
 * The engine keeps each grass type of a 12x12 cell block in one BSMultiStreamInstanceTriShape, with a group of instances per
 * cell: cells add groups (AddGroup, the GID buffer and file loaders) and remove them on unload (RemoveGroup), and the shape
 * goes when its block does. The hooks that see these, and LoadGrassType's model paths, live here, so either consumer, or
 * both, can be loaded without the other. Each consumer registers a Sink and calls InstallCapture(); the hooks are installed
 * once, and every event is handed to every sink in registration order, under one lock, so the sinks see one order of a
 * shape's lifecycle. Neither consumer includes the other, and DCLF includes nothing of TruePBR's: the PBR material fields
 * are read here, at capture.
 */
namespace GrassShared
{
	/** @brief Byte stride of the per-instance records. */
	inline constexpr uint32_t kGrassStride = 32;

	/** @brief One captured instance group of a grass shape. Valid only during the sink call; sinks copy what they keep. */
	struct AddEvent
	{
		// Global, taken under the capture's lock, so a shape's events keep their order across the loader and dtor threads.
		uint64_t seq = 0;
		RE::BSMultiStreamInstanceTriShape* shape = nullptr;
		uint32_t group = 0;                // the engine's group index in the shape (a later group may reuse it)
		const uint8_t* records = nullptr;  // count * kGrassStride bytes of half-packed engine records
		uint32_t count = 0;

		uint64_t vertexDesc = 0;
		RE::NiPoint3 origin;  // shape->world.translate at capture
		// The wind's world direction in xy: the shape's model Y axis at capture, unitized (Grass Optimizations' GrassBucketStore).
		float windDirection[2]{ 0.0f, 1.0f };

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
			Runtime,    // AddGroup (DoneAddingInstances' chunks when not queued)
			GidBuffer,  // AddGroupGIDBuffer
			GidFile,    // AddGroupGIDFile (.cgid)
			Queued      // attached from the engine's queue (DoneAddingInstances' queued chunks, AddQueuedGroupGIDBuffer and -File)
		} source = Source::Runtime;
	};

	/**
	 * @brief A capture consumer. Every callback runs on the hook's thread (cell loaders, the dtor's caller) under the capture's
	 * lock, so a sink only stages; any member may be null. A sink takes groups either as indices it reads itself (group: under
	 * the engine's group lock, as Grass Optimizations' store does) or as events read here (add).
	 */
	struct Sink
	{
		// LoadGrassType: the shape's source .nif (its mesh record is MeshOf(shape) from here on).
		void (*model)(RE::BSMultiStreamInstanceTriShape*, const char* modelPath) = nullptr;
		// A group added to a grass shape, by index.
		void (*group)(RE::BSMultiStreamInstanceTriShape*, uint32_t group) = nullptr;
		// The engine attached queued groups (generated grass, queued cache groups) to their shapes, without AddGroup: a sink that
		// takes indices finds them itself (as Grass Optimizations' store scans its registered shapes); add sinks get them as adds.
		void (*attached)() = nullptr;
		// A group added to a grass shape, read: its records and the shape's state now.
		void (*add)(const AddEvent&) = nullptr;
		// A group the engine removed from a shape (a cell's unload), after the removal; any shape, grass or not.
		void (*removeGroup)(RE::BSMultiStreamInstanceTriShape*, uint32_t group, uint64_t seq) = nullptr;
		// Every BSMultiStreamInstanceTriShape destruction, grass or not, captured or not.
		void (*remove)(RE::BSMultiStreamInstanceTriShape*, uint64_t seq) = nullptr;
		// A grass shape's group whose records could not be read for add (no CPU records, stride != kGrassStride, no texture,
		// lighting material or mesh). The engine still draws it, so a consumer that hides grass wholesale must not.
		void (*rejected)(RE::BSMultiStreamInstanceTriShape*, uint32_t group, uint64_t seq) = nullptr;
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
