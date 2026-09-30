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

	/** @brief Effects 11 when UseEffect is on (preset optional), else Post Processing unless bypassed, else the game's own. */
	Mode Get();

	/** @brief Enables the chosen pipeline and disables the other; Effects 11 may be selected before a preset loads. */
	void Set(Mode mode);

	/** @brief Draws the Vanilla / Post Processing / Effects 11 selector shared by both feature menus. */
	void DrawSelector();
}
