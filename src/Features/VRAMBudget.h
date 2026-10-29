#pragma once

#include "Feature.h"
#include "I18n/I18n.h"

/**
 * @brief Keeps VRAM under a share of the driver's budget by unloading the oldest buffered cells.
 *
 * The engine keeps recently left cells loaded (`uInterior Cell Buffer`, `uExterior Cell Buffer`) and unloads them only
 * when a buffer overflows, on `pcb`, or when its own heap runs out, never for VRAM. Over the threshold this feature
 * unloads the oldest buffered cells a few at a time on the main thread, then runs the engine's purge (what `pcb` runs)
 * so their models and textures are released at once. Engine details: docs/development/skyrim-cell-buffer.md.
 */
struct VRAMBudget : Feature
{
	virtual inline std::string GetName() override { return "VRAM Budget"; }
	virtual std::string GetDisplayName() override { return T("feature.vram_budget.name", "VRAM Budget"); }
	virtual inline std::string GetShortName() override { return "VRAMBudget"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kUtility; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.vram_budget.description", "Unloads the oldest cells the game keeps cached when VRAM use goes over a share of the GPU's budget."),
			{ T("feature.vram_budget.key_feature_1", "Unloads cached cells a few at a time, oldest first, until VRAM use is back under the threshold"),
				T("feature.vram_budget.key_feature_2", "Never touches the cells around the player or the current interior"),
				T("feature.vram_budget.key_feature_3", "In interiors, also unloads the invisible distant land once the exterior cells are gone") } };
	}

	virtual bool IsCore() const override { return true; }

	/** @brief Draws the threshold setting and the current VRAM use. */
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	/** @brief Queues the controller's tick (four times a second) on the main thread. */
	virtual void EarlyPrepass() override;

	struct Settings
	{
		float ThresholdPercent = 80.0f;  // Share of the driver's VRAM budget above which cells are unloaded
	};

	Settings settings;

private:
	/** @brief One controller cycle, on the main thread (an SKSE task): samples VRAM use and takes the next step if it is over. */
	void Tick();

	/** @brief Main thread: unloads up to a_cells cells (or the step that has to come first), describing it in o_what (left empty
	 * when nothing is left). Returns the cells unloaded. */
	std::uint32_t Unload(RE::TES* a_tes, std::uint32_t a_cells, std::string& o_what);

	/** @brief Cells to unload, oldest first, at most a_max; only cells that are safe to unload now. */
	std::vector<RE::TESObjectCELL*> PickVictims(RE::TES* a_tes, size_t a_max) const;

	/** @brief The driver's VRAM use and budget into usage and budget; false without them. */
	bool Sample();

	/** @brief Main thread: pcb's purge (garbage, the model DB, then the texture DB), with IO paused. */
	void Purge();

	std::atomic<bool> tickQueued = false;
	std::atomic<std::int64_t> nextTick = 0;  // steady_clock ticks

	// The controller (main thread).
	enum class Phase
	{
		kIdle,
		kReleasing,  // a step was taken: waiting for the render graph and the engine to let go, then purging again
		kSettling    // purged: waiting to measure the step's response
	};
	Phase phase = Phase::kIdle;
	std::int64_t phaseEnd = 0;
	double stepUsageMiB = 0.0;
	std::uint32_t stepCells = 0;
	double mibPerCell = 64.0;                        // learnt from each step's response
	RE::TESObjectCELL* terrainUnloadedIn = nullptr;  // the interior whose stay already unloaded the distant land

	// Read by the menu.
	std::atomic<std::uint64_t> usage = 0;
	std::atomic<std::uint64_t> budget = 0;
	std::atomic<std::uint32_t> cellsUnloaded = 0;
	std::atomic<std::uint32_t> steps = 0;
	std::atomic<float> lastStepMs = 0.0f;
	std::atomic<float> mibPerCellShown = 64.0f;
	std::atomic<bool> exhausted = false;  // over budget with nothing left to unload
};
