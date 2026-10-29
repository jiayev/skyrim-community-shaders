#include "VRAMBudget.h"

#include "Menu.h"
#include "Utils/UI.h"

#define I18N_KEY_PREFIX "feature.vram_budget."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	VRAMBudget::Settings,
	ThresholdPercent)

namespace
{
	using namespace std::chrono_literals;

	// Engine addresses, SE id first. Every one is described (with AE/SE addresses) in docs/development/skyrim-cell-buffer.md.

	/** @brief `uInterior Cell Buffer:General` / `uExterior Cell Buffer:General`: the buffers' slot counts (read each time, as the engine does). */
	std::uint32_t InteriorBufferSize()
	{
		static REL::Relocation<std::uint32_t*> value{ REL::RelocationID(500861, 358814) };
		return *value;
	}

	std::uint32_t ExteriorBufferSize()
	{
		static REL::Relocation<std::uint32_t*> value{ REL::RelocationID(500863, 358817) };
		return *value;
	}

	/** @brief The MemoryLevelManager `pcb` passes to the IO pause and the purge (a function-local static; the purge ignores it). */
	void* MemoryLevelManager()
	{
		static REL::Relocation<void*> object{ REL::RelocationID(514175, 400323) };
		return object.get();
	}

	/** @brief The active worldspace's `BGSTerrainManager*` (distant terrain, object and tree LOD); null without one. */
	void* ActiveTerrainManager()
	{
		static REL::Relocation<void**> manager{ REL::RelocationID(516111, 402262) };
		return *manager;
	}

	/** @brief `MemoryLevelManager::BeginMemoryFree(mlm, state, lockRenderer, pauseIO)`: pauses IO, waiting for the tasks in flight. */
	void BeginMemoryFree(char a_state[2])
	{
		using func_t = void(void*, char*, bool, bool);
		static REL::Relocation<func_t> func{ REL::RelocationID(35844, 36815) };
		func(MemoryLevelManager(), a_state, false, true);
	}

	/** @brief `MemoryLevelManager::EndMemoryFree(mlm, state)`: resumes what BeginMemoryFree paused. */
	void EndMemoryFree(char a_state[2])
	{
		using func_t = void(void*, char*);
		static REL::Relocation<func_t> func{ REL::RelocationID(35845, 36816) };
		func(MemoryLevelManager(), a_state);
	}

	/** @brief `pcb`'s full purge: garbage, deferred releases, then the model DB and the texture DB's whole release queues. */
	void FreeAll()
	{
		using func_t = void(void*);
		static REL::Relocation<func_t> func{ REL::RelocationID(35842, 36813) };
		func(MemoryLevelManager());
	}

	/** @brief `TES::RemoveExteriorFromBuffer(tes, cell, unload)`: clears the cell from the grid, unloads it, compacts the buffer. */
	void RemoveExteriorFromBuffer(RE::TES* a_tes, RE::TESObjectCELL* a_cell)
	{
		using func_t = void(RE::TES*, RE::TESObjectCELL*, bool);
		static REL::Relocation<func_t> func{ REL::RelocationID(13219, 13369) };
		func(a_tes, a_cell, true);
	}

	/** @brief `TES::RemoveInteriorFromBuffer(tes, cell)`: unloads the cell and compacts the buffer. Only for a cell in the buffer:
	 * given one that is not, it clears the last slot without unloading it. */
	void RemoveInteriorFromBuffer(RE::TES* a_tes, RE::TESObjectCELL* a_cell)
	{
		using func_t = void(RE::TES*, RE::TESObjectCELL*);
		static REL::Relocation<func_t> func{ REL::RelocationID(13220, 13370) };
		func(a_tes, a_cell);
	}

	/** @brief `BGSTerrainManager::UnloadAll(mgr, false, false)`, as `pcb` calls it while the player is in an interior. */
	void UnloadDistantLand(void* a_manager)
	{
		using func_t = void(void*, bool, bool);
		static REL::Relocation<func_t> func{ REL::RelocationID(31026, 31812) };
		func(a_manager, false, false);
	}

	/** @brief The worldspace's own 3D (its portal graph, scene nodes and the 3D of its persistent objects), as `pcb` drops it in
	 * an interior before detaching the grid. */
	void UnloadWorldSpace3D(RE::TESWorldSpace* a_worldSpace)
	{
		using func_t = void(RE::TESWorldSpace*);
		static REL::Relocation<func_t> func{ REL::RelocationID(20072, 20515) };
		func(a_worldSpace);
	}

	constexpr std::uint8_t kLoadedDetached = 4;      // TESObjectCELL::cellState of a buffered cell nobody is attaching
	constexpr std::uint32_t kStillLoading = 1u << 21;  // TESForm::formFlags: an IO thread is still building the cell

	// The controller (Tick): a proportional step, cells = gain * excess / (MiB a cell frees), where the MiB a cell frees is
	// learnt from each step's measured response. It acts only once the last step's memory has come back (the plant's dead
	// time: the render graph letting go of the engine's references, the engine's release rings, the GPU's frames in flight).
	constexpr auto kTickInterval = 250ms;
	constexpr auto kRelease = 500ms;  // after a step, until the purge that frees what the render graph let go of meanwhile
	constexpr auto kSettle = 500ms;   // after that purge, until the response is measured
	constexpr double kGain = 0.8;
	constexpr double kMarginPercent = 2.0;  // a step aims this far under the threshold, so the next one is not one tick away
	constexpr std::uint32_t kMaxCellsPerStep = 16;
	constexpr double kMiB = 1024.0 * 1024.0;

	/** @brief The cell's spin-lock count: non-zero while some code holds the cell (the engine's purges skip those). */
	std::uint32_t LockCount(const RE::TESObjectCELL* a_cell)
	{
		return reinterpret_cast<const volatile std::uint32_t*>(&a_cell->GetRuntimeData().spinLock)[1];
	}

	std::int64_t Ticks(std::chrono::steady_clock::duration a_duration) { return a_duration.count(); }
	std::int64_t Now() { return Ticks(std::chrono::steady_clock::now().time_since_epoch()); }

	std::string Describe(const RE::TESObjectCELL* a_cell)
	{
		const char* editorID = a_cell->GetFormEditorID();
		return editorID && *editorID ? std::format("{} ({:08X})", editorID, a_cell->GetFormID()) : std::format("{:08X}", a_cell->GetFormID());
	}
}

void VRAMBudget::DrawSettings()
{
	ImGui::SliderFloat(T(TKEY("threshold"), "Threshold"), &settings.ThresholdPercent, 50.0f, 100.0f, "%.0f%%");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("threshold_tooltip"),
							  "Share of the GPU's VRAM budget above which the oldest cached cells are unloaded. "
							  "100% leaves the game's own cell caching alone."));
	}

	ImGui::Text(T(TKEY("usage"), "VRAM use: %.0f of %.0f MiB"), usage.load() / kMiB, budget.load() / kMiB);
	ImGui::Text(T(TKEY("unloaded"), "Cells unloaded: %u in %u steps (last step %.1f ms, %.0f MiB a cell)"), cellsUnloaded.load(), steps.load(), lastStepMs.load(), mibPerCellShown.load());
	if (exhausted)
		ImGui::Text("%s", T(TKEY("exhausted"), "Over the threshold with no cached cells left to unload."));
}

void VRAMBudget::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.ThresholdPercent = std::clamp(settings.ThresholdPercent, 50.0f, 100.0f);
}

void VRAMBudget::SaveSettings(json& o_json)
{
	o_json = settings;
}

void VRAMBudget::RestoreDefaultSettings()
{
	settings = {};
}

void VRAMBudget::EarlyPrepass()
{
	// Only the cadence lives here; the controller runs on the main thread, as an SKSE task, one at a time.
	const std::int64_t now = Now();
	std::int64_t due = nextTick.load(std::memory_order_relaxed);
	if (now < due || !nextTick.compare_exchange_strong(due, now + Ticks(kTickInterval)) || tickQueued.exchange(true))
		return;
	if (const auto tasks = SKSE::GetTaskInterface()) {
		tasks->AddTask([this]() {
			Tick();
			tickQueued = false;
		});
	} else {
		tickQueued = false;
	}
}

bool VRAMBudget::Sample()
{
	const auto menu = Menu::GetSingleton();
	const auto adapter = menu ? menu->GetDXGIAdapter3() : nullptr;
	DXGI_QUERY_VIDEO_MEMORY_INFO info{};
	if (!adapter || FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)) || !info.Budget)
		return false;
	usage = info.CurrentUsage;
	budget = info.Budget;
	return true;
}

void VRAMBudget::Purge()
{
	char state[2]{};
	BeginMemoryFree(state);
	FreeAll();
	EndMemoryFree(state);
}

void VRAMBudget::Tick()
{
	if (!Sample())
		return;
	const std::int64_t now = Now();
	const double usageMiB = usage.load() / kMiB;
	const double budgetMiB = budget.load() / kMiB;

	switch (phase) {
	case Phase::kReleasing:
		if (now < phaseEnd)
			return;
		Purge();
		phase = Phase::kSettling;
		phaseEnd = now + Ticks(kSettle);
		return;
	case Phase::kSettling:
		{
			if (now < phaseEnd)
				return;
			const double drop = stepUsageMiB - usageMiB;
			if (stepCells && drop > 0.0)
				mibPerCell = std::clamp(0.5 * mibPerCell + 0.5 * drop / stepCells, 4.0, 1024.0);
			mibPerCellShown = static_cast<float>(mibPerCell);
			logger::info("[VRAM Budget] step response: {:.0f} -> {:.0f} MiB ({:+.0f}) after {} cells; {:.0f} MiB a cell", stepUsageMiB, usageMiB, -drop, stepCells, mibPerCell);
			phase = Phase::kIdle;
			break;
		}
	case Phase::kIdle:
		break;
	}

	if (usageMiB * 100.0 <= budgetMiB * settings.ThresholdPercent) {
		exhausted = false;
		return;
	}

	const auto tes = RE::TES::GetSingleton();
	const auto ui = RE::UI::GetSingleton();
	const auto player = RE::PlayerCharacter::GetSingleton();
	if (!tes || !tes->gridCells || !ui || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) || !player || !player->GetParentCell())
		return;

	const double targetMiB = budgetMiB * (settings.ThresholdPercent - kMarginPercent) / 100.0;
	const auto cells = static_cast<std::uint32_t>(std::clamp(std::ceil(kGain * (usageMiB - targetMiB) / mibPerCell), 1.0, static_cast<double>(kMaxCellsPerStep)));

	const auto start = std::chrono::steady_clock::now();
	std::string what;
	const std::uint32_t unloaded = Unload(tes, cells, what);
	if (what.empty()) {
		if (!exhausted.exchange(true))
			logger::info("[VRAM Budget] {:.0f} of {:.0f} MiB: over the threshold with no cached cells left to unload", usageMiB, budgetMiB);
		return;
	}
	const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();

	exhausted = false;
	cellsUnloaded += unloaded;
	++steps;
	lastStepMs = ms;
	stepUsageMiB = usageMiB;
	stepCells = unloaded;
	phase = Phase::kReleasing;
	phaseEnd = Now() + Ticks(kRelease);
	logger::info("[VRAM Budget] {:.0f} of {:.0f} MiB, target {:.0f}: asked for {} cells at {:.0f} MiB a cell; {} in {:.1f} ms", usageMiB, budgetMiB, targetMiB, cells, mibPerCell, what, ms);
}

std::vector<RE::TESObjectCELL*> VRAMBudget::PickVictims(RE::TES* a_tes, size_t a_max) const
{
	const auto player = RE::PlayerCharacter::GetSingleton();
	const RE::TESObjectCELL* playerCell = player ? player->GetParentCell() : nullptr;
	const auto grid = a_tes->gridCells;
	const std::uint32_t gridSlots = grid && grid->cells ? grid->length * grid->length : 0;

	const auto unloadable = [&](const RE::TESObjectCELL* a_cell) {
		if (!a_cell || a_cell == a_tes->interiorCell || a_cell == playerCell)
			return false;
		if (a_cell->cellState.underlying() != kLoadedDetached || (a_cell->formFlags & kStillLoading) || LockCount(a_cell))
			return false;
		return std::find(grid->cells, grid->cells + gridSlots, a_cell) == grid->cells + gridSlots;
	};

	// Both buffers are most-recently-used lists by slot: the highest occupied slot is the oldest. The exterior buffer's
	// first slots hold the attached grid, which the checks above skip; once the grid is detached they are unloadable too.
	std::vector<RE::TESObjectCELL*> exteriors;
	std::vector<RE::TESObjectCELL*> interiors;
	if (a_tes->exteriorBuffer) {
		const std::uint32_t slots = ExteriorBufferSize();
		const auto occupied = static_cast<size_t>(std::count_if(a_tes->exteriorBuffer, a_tes->exteriorBuffer + slots, [](auto* a_cell) { return a_cell != nullptr; }));
		// Never the last one: only the engine's own eviction releases the worldspace once the exterior buffer is empty.
		for (std::uint32_t i = slots; i-- > 0 && exteriors.size() + 1 < occupied;)
			if (unloadable(a_tes->exteriorBuffer[i]))
				exteriors.push_back(a_tes->exteriorBuffer[i]);
	}
	if (a_tes->interiorBuffer) {
		for (std::uint32_t i = InteriorBufferSize(); i-- > 0;)
			if (unloadable(a_tes->interiorBuffer[i]))
				interiors.push_back(a_tes->interiorBuffer[i]);
	}

	// The other space's cells first (exteriors while inside, interiors while outside), as the engine's own eviction does.
	auto victims = a_tes->interiorCell ? std::move(exteriors) : std::move(interiors);
	const auto& rest = a_tes->interiorCell ? interiors : exteriors;
	victims.insert(victims.end(), rest.begin(), rest.end());
	if (victims.size() > a_max)
		victims.resize(a_max);
	return victims;
}

std::uint32_t VRAMBudget::Unload(RE::TES* a_tes, std::uint32_t a_cells, std::string& o_what)
{
	if (a_tes->interiorCell != terrainUnloadedIn)
		terrainUnloadedIn = nullptr;
	const auto grid = a_tes->gridCells;
	const bool gridAttached = grid->cells && std::any_of(grid->cells, grid->cells + grid->length * grid->length, [](auto* a_cell) { return a_cell != nullptr; });
	void* terrainManager = ActiveTerrainManager();

	// Inside, in this order: the buffered exteriors; the exterior grid, which the engine keeps attached while the player is in
	// an interior (detached as pcb does, then unloaded like the others); the invisible distant land; the other interiors.
	enum class Step
	{
		kCells,
		kDetachGrid,
		kDistantLand
	};
	const auto victims = PickVictims(a_tes, a_cells);
	const bool exteriorsLeft = !victims.empty() && !victims.front()->IsInteriorCell();
	Step step = Step::kCells;
	if (a_tes->interiorCell && !exteriorsLeft) {
		if (gridAttached)
			step = Step::kDetachGrid;
		else if (!terrainUnloadedIn && terrainManager)
			step = Step::kDistantLand;
	}
	if (step == Step::kCells && victims.empty())
		return 0;

	char state[2]{};
	BeginMemoryFree(state);

	if (step == Step::kDetachGrid) {
		if (auto* worldSpace = a_tes->GetRuntimeData2().worldSpace)
			UnloadWorldSpace3D(worldSpace);
		grid->DetachAll();
		o_what = "detached the exterior grid; ";
	}
	std::uint32_t count = 0;
	if (step == Step::kDistantLand) {
		UnloadDistantLand(terrainManager);
		terrainUnloadedIn = a_tes->interiorCell;
		o_what = "unloaded the distant land";
	} else {
		// Picked again with IO paused: the cell loader tasks write the buffers from IO threads.
		std::string names;
		for (auto* cell : PickVictims(a_tes, a_cells)) {
			names += (count++ ? ", " : "") + Describe(cell);
			if (cell->IsInteriorCell())
				RemoveInteriorFromBuffer(a_tes, cell);
			else
				RemoveExteriorFromBuffer(a_tes, cell);
		}
		o_what += std::format("unloaded {} cells: {}", count, names.empty() ? "none (taken meanwhile)" : names);
	}
	FreeAll();
	EndMemoryFree(state);
	return count;
}

#undef I18N_KEY_PREFIX
