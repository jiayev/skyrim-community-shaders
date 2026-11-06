#pragma once

#include <cstdint>
#include <functional>
#include <string>

struct ID3D11ShaderResourceView;

/**
 * @brief Texture streaming's core: the game's file textures as shells with mip windows (docs/development/texture-streaming.md).
 *
 * The engine holds a shell: a texture and view with the file's description and no storage. A bind of the shell reads its window, an
 * ordinary texture holding mips [top, last]. Windows are built on background threads, through whatever scheduler the renderer installed,
 * which is how its graph gates its objects on them. One low-priority worker decides the windows from the demand the renderer gives it.
 * Nothing here knows about the renderer, and nothing here iterates objects or materials on a game thread.
 */
namespace TextureStreaming
{
	/** @brief Whether streaming is on: the menu feature, unless CS_DCLF_TEX_STREAM says otherwise, and not on VR. Read once, after the settings load: shells only exist from the first texture load on. */
	bool Enabled();

	/** @brief Whether new loads become shells: on, with a renderer to schedule their windows. */
	bool Active();

	/** @brief What the menu feature sets. A CS_DCLF_TEX_* switch that is set wins over its value here. */
	struct Settings
	{
		bool distance = true;          // the distance floor
		bool feedback = true;          // shaders report the resolution their surfaces want
		float distanceScale = 1.0f;    // the distance floor's sharpness: above 1 finer mips
		std::uint32_t baseSize = 128;  // the base window's largest side; read at the first texture load
		std::uint32_t rateMB = 256;    // upgrades started per second
		std::uint32_t budgetMB = 0;    // the memory to keep to, above or below the driver's budget; 0 for the driver's
		float visibleOffset = 1.0f;    // over the budget, what is on screen may step down this many mips; 0 loads what it asks for
	};

	/** @brief Any thread: applies from the worker's next tick. */
	void SetSettings(const Settings& a_settings);

	/** @brief Any thread: what applies now, switches included. */
	Settings GetSettings();

	/** @brief Whether every window is the full chain, made as the texture loads (CS_DCLF_TEX_STREAM_FULL=1): the baseline to compare with. */
	bool FullMode();

	/** @brief Hooks the game's DDS loader. Main thread, at plugin load. */
	void InstallGameHooks();

	using TextureId = std::uint64_t;

	/** @brief The streamed texture of a view, or 0 if a_view is not a shell. Any thread; only compares a_view's address. */
	TextureId IdOf(const ID3D11ShaderResourceView* a_view);

	/** @brief The shell's current window with a reference, or null. The caller releases it. Any thread; a_shell must be alive. */
	ID3D11ShaderResourceView* CurrentWindow(ID3D11ShaderResourceView* a_shell);

	/** @brief Whether a view needs nothing more to be drawn: not a shell, or it has its base window, or its load failed. Any thread. */
	bool BaseReady(const ID3D11ShaderResourceView* a_view);

	/** @brief Asks for a view's base window, once. Any thread, never blocks; nothing for a view that is not a shell. */
	void RequestBase(const ID3D11ShaderResourceView* a_view);

	/**
	 * @brief A holder keeps this view's first import rather than follow swaps (grass, tree LOD): its texture stays at full resolution.
	 * @return whether the full window is there; if not, it was asked for, and the holder asks again later.
	 */
	bool PinFull(const ID3D11ShaderResourceView* a_view);

	/** @brief For a request's gate. The first revision of a texture's window is its base. */
	inline constexpr std::uint64_t kBaseRevision = 1;

	/** @brief The window to build at the base's top. */
	inline constexpr std::uint32_t kBaseTop = ~0u;

	/** @brief Builds a window: a blocking read and upload, for a background thread. a_top is limited to what the texture allows. */
	void BuildWindow(TextureId a_id, std::uint32_t a_top);

	/** @brief Where window builds run. Without one, the worker runs them itself, and new loads stay the engine's (FullMode aside). Empty to stop. */
	struct Scheduler
	{
		std::function<void(TextureId a_id, std::uint64_t a_revision, std::uint32_t a_top)> post;  // any thread, never blocks; later runs BuildWindow
		std::function<void(TextureId a_id)> retire;                                               // the worker's: the texture is gone
	};
	void SetScheduler(Scheduler a_scheduler);

	/** @brief A shell's window changed. a_shell is an identity: it may not be dereferenced. On the building thread, outside every lock here. */
	void SetWindowListener(std::function<void(const ID3D11ShaderResourceView* a_shell)> a_listener);

	/** @brief Where a demand comes from: what is on screen is the last to lose detail when VRAM is short. */
	enum class DemandKind : std::uint8_t
	{
		Distance,  // only by distance, whichever way the camera faces
		Visible    // on screen: shaders reported it, or it is in view and its shaders cannot report
	};

	/** @brief What a demand source reports to, on the worker's thread. A footprint is log2 of the UV one screen pixel covers (Policy.h). */
	class DemandSink
	{
	public:
		virtual ~DemandSink() = default;
		virtual void Note(const ID3D11ShaderResourceView* a_view, float a_footprintLog2, DemandKind a_kind) = 0;
	};
	/** @brief Called on each tick on the worker's thread: reports the current demand. It may keep nothing from a_sink. */
	void SetDemandSource(std::function<void(DemandSink& a_sink)> a_source);

	/** @brief For the reports: the materials feedback reported on since the last tick, and how many of those asked finer than the distance floor. */
	void NoteFeedback(std::uint64_t a_reported, std::uint64_t a_finer);

	/** @brief Counts each change of worldspace the worker has seen. A demand source drops what it remembers of the last place when this moves. */
	std::uint64_t WorldChanges();

	/** @brief For a report: a streamed view's file, and its resident and wanted tops. False if a_view is not a shell. Any thread. */
	bool Describe(const ID3D11ShaderResourceView* a_view, std::string& a_path, std::uint32_t& a_residentTop, std::uint32_t& a_wantedTop);

	/** @brief A test's: the worker writes every texture's state to a_path on its next tick, one line each. */
	void RequestDump(std::string a_path);

	/** @brief A test's override: every window at this top (clamped per texture), or negative for the policy's choice. */
	void ForceTop(int a_top);

	/** @brief Totals for the reports. */
	struct Stats
	{
		std::uint64_t textures = 0;       // live shells
		std::uint64_t withWindow = 0;     // of which have a window
		std::uint64_t residentBytes = 0;  // the windows' bytes
		std::uint64_t wantedBytes = 0;    // what the worker last wanted of them
		std::uint64_t fullBytes = 0;      // what the shells would take in full
		std::uint64_t windowsBuilt = 0;
		std::uint64_t upgrades = 0, downgrades = 0, pressureDowngrades = 0;  // what the worker asked for
		std::uint64_t bytesRead = 0;
		std::uint64_t loadFailures = 0;
		std::uint64_t safetyNetLoads = 0;  // base windows a bind had to wait for
		std::uint64_t storageFills = 0;    // shells something needed the storage of
		std::uint64_t passedThrough = 0;   // loads left to the engine
		std::uint64_t engineFailures = 0;  // of which the engine's load failed
		std::uint64_t noted = 0;           // demand notes the last tick took
		std::uint64_t pinned = 0;
		std::uint64_t nativeFull = 0;                                                           // textures full because native draws read them
		std::uint64_t feedbackReported = 0, feedbackFiner = 0;                                  // since the start
		std::uint64_t baseBuilds = 0, baseReadMicroseconds = 0, baseCreateMicroseconds = 0;     // the first window of each texture
		std::uint64_t otherBuilds = 0, otherReadMicroseconds = 0, otherCreateMicroseconds = 0;  // later ones

		// The budget's controller, from its last tick. All zero until it has measured.
		std::uint64_t cardBytes = 0;          // the card's own VRAM
		std::uint64_t driverBudgetBytes = 0;  // what the driver offers this process of it
		std::uint64_t budgetBytes = 0;        // what applies: the setting, or the driver's
		std::uint64_t usedBytes = 0;          // DXVK's memory wherever it lies, and the rest of the process's VRAM
		std::uint64_t overflowBytes = 0;      // of which in system memory
		std::uint64_t otherBytes = 0;         // of which not windows, smoothed
		std::uint64_t outsideBytes = 0;       // of the rest, what is not DXVK's: ORG, the swap chain, the driver
		std::uint64_t allowanceBytes = 0;     // what the windows are fitted to
		std::uint64_t visibleBytes = 0;       // of the wanted bytes, what is on screen asked for
		float distancePullback = 0.0f;        // in mips
		float visiblePullback = 0.0f;
	};
	Stats GetStats();
}
