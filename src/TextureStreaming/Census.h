#pragma once

#include <filesystem>

struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

/**
 * @brief A survey of the game's file-loaded textures (CS_TEX_CENSUS=1): which ones the engine or CS bind through D3D11, which ones
 * the renderer imports, and what a base-mip window would save. It changes nothing the game does.
 */
namespace TextureStreaming::Census
{
	/** @brief Whether the census runs (read once). */
	bool Enabled();

	/** @brief Hooks the game's DDS loader. Main thread, at plugin load. */
	void InstallGameHooks();

	/** @brief Hooks the context's texture binds and the calls that reach a texture's own storage. */
	void InstallD3DHooks(ID3D11DeviceContext* a_context);

	/** @brief Any thread: the renderer imported this view. */
	void NoteRendererUse(ID3D11ShaderResourceView* a_view);

	/** @brief Writes every live texture's row and returns the file; logs the totals. */
	std::filesystem::path WriteJson(const std::filesystem::path& a_directory);
}
