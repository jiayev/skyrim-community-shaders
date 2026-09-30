#pragma once

/// Preset export dialog: metadata, artwork copied into Presets/<Name>/, and a load-ordered mod list.
/// Export is global (every context); open from any toolbar or the File menu.
namespace ScenePresetExport
{
	/// Whether there is anything to export: a user layer, or any installed overwrite.
	bool CanExport();

	/// Opens the export dialog.
	void Open();

	/// Draws the dialog and its confirmations. Call once per frame from the editor.
	void Draw();
}
