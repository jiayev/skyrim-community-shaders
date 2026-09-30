#pragma once

/// CS Editor Features-window filter: hides Debug CollapsingHeader / TreeNode dropdowns while a
/// Scope is armed. Main menu and feature code are untouched.
namespace FeatureDebugFilter
{
	/** @brief Installs ImGui detours used by the Features editor. Idempotent; safe if attach fails. */
	void Install();

	/// Arms Debug-dropdown hiding for one Features-editor DrawSettings call.
	class Scope
	{
	public:
		explicit Scope(bool hide);
		~Scope();

		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;

	private:
		bool previous;
	};
}
