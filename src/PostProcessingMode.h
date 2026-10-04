#pragma once

/** @brief Picks the one post-processing pipeline that drives the frame, keeping Post Processing and Effects 11 exclusive. */
namespace PostProcessingMode
{
	enum class Mode
	{
		Vanilla,
		PostProcessing,
		Effects11
	};

	/** @brief Effects 11 when UseEffect is on with a loaded preset, else Post Processing unless bypassed, else the game's own. */
	Mode Get();

	/** @brief Enables the chosen pipeline and disables the other; Effects 11 is ignored without a loaded preset. */
	void Set(Mode mode);

	/** @brief Reads the saved mode from the "General" section, defaulting to what is installed (Effects 11 with a preset, else Post Processing, else Vanilla); applied fully by ApplyPending(). */
	void Load(const json& a_general);

	/** @brief Writes the current mode into the user settings "General" section once it is known. */
	void Save(json& a_general);

	/** @brief Applies a mode read by Load() once Effects 11 is ready; called from Effects11::Reset. */
	void ApplyPending();

	/** @brief Draws the Vanilla / Post Processing / Effects 11 selector shared by both feature menus. */
	void DrawSelector();
}
