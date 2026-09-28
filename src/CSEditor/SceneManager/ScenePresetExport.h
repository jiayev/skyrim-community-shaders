#pragma once

#include "SceneSettingsManager.h"

/// Preset export dialog: metadata, artwork copied into Presets/<Name>/, and a load-ordered mod list.
/// Export is global; the page that opened it only decides which toolbar draws it.
namespace ScenePresetExport
{
	/// Whether there is anything to export: a user layer, or any installed overwrite.
	bool CanExport();

	/// Opens the dialog, owned by the page whose toolbar was clicked.
	void Open(const SceneSettingsManager::SceneContextId& context);

	/// Draws the dialog and its confirmations. Call once per frame from every toolbar; only the
	/// call whose context matches the one passed to Open() does anything.
	void Draw(const SceneSettingsManager::SceneContextId& context);
}
